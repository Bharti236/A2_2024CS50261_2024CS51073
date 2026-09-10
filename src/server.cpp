#include "common.hpp"
#include "ioloop.hpp"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

enum class Role { Unknown, Trader, MarketData };
enum class Side { Buy, Sell };

static std::vector<int> g_need_write;
static std::vector<int> g_need_unwrite;

struct Session {
    int fd = -1;
    Role role = Role::Unknown;
    bool logged_in = false;
    std::string username;
    bool sub_jnst = false;
    bool sub_imct = false;
    bool alive = true;
    std::string send_buf;
    LineReader reader;

    bool flush_send() {
        while (!send_buf.empty() && fd >= 0) {
            ssize_t n = ::send(fd, send_buf.data(), send_buf.size(), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return true;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            send_buf.erase(0, static_cast<size_t>(n));
        }
        return true;
    }

    void note_write() {
        if (fd < 0) {
            return;
        }
        if (send_buf.empty()) {
            g_need_unwrite.push_back(fd);
        } else {
            g_need_write.push_back(fd);
        }
    }

    bool send(const std::string& msg) {
        if (!alive || fd < 0) {
            return false;
        }
        send_buf.append(msg);
        send_buf.push_back('\n');
        bool ok = flush_send();
        note_write();
        return ok;
    }

    bool subscribed(Instrument inst) const {
        return inst == Instrument::JNST ? sub_jnst : sub_imct;
    }
};

struct Order {
    int id = 0;
    Side side = Side::Buy;
    Instrument inst = Instrument::JNST;
    int price = 0;
    int remaining = 0;
    std::weak_ptr<Session> owner;
};

struct Book {
    std::unordered_map<int, std::deque<int>> buys;
    std::unordered_map<int, std::deque<int>> sells;
};

struct PendingSend {
    std::shared_ptr<Session> session;
    std::string msg;
};

class Exchange {
public:
    void handle_line(const std::shared_ptr<Session>& s, const std::string& line) {
        auto tok = tokenize(line);
        if (tok.empty()) {
            reply(s, "ERROR empty message");
            return;
        }
        const std::string& cmd = tok[0];
        if (cmd == "QUIT") {
            if (tok.size() != 1) {
                reply(s, "ERROR invalid arguments");
                return;
            }
            s->alive = false;
            return;
        }
        if (cmd == "LOGIN") {
            login(s, tok);
            return;
        }
        if (cmd == "BUY" || cmd == "SELL") {
            order_cmd(s, tok);
            return;
        }
        if (cmd == "CANCEL") {
            cancel_cmd(s, tok);
            return;
        }
        if (cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE") {
            sub_cmd(s, tok);
            return;
        }
        reply(s, "ERROR unknown command");
    }

    void disconnect(const std::shared_ptr<Session>& s) {
        std::lock_guard<std::mutex> g(mu_);
        if (s->role == Role::Trader && s->logged_in) {
            traders_.erase(s->username);
        }
        s->logged_in = false;
        s->alive = false;
        md_clients_.erase(s.get());
    }

private:
    std::mutex mu_;
    int next_order_id_ = 0;
    std::unordered_map<int, Order> orders_;
    Book books_[2];
    std::unordered_map<std::string, std::shared_ptr<Session>> traders_;
    std::unordered_map<Session*, std::shared_ptr<Session>> md_clients_;

    static int inst_index(Instrument inst) {
        return inst == Instrument::JNST ? 0 : 1;
    }

    static void reply(const std::shared_ptr<Session>& s, const std::string& msg) {
        s->send(msg);
    }

    static void remove_id(std::deque<int>& q, int id) {
        q.erase(std::remove(q.begin(), q.end(), id), q.end());
    }

    void login(const std::shared_ptr<Session>& s, const std::vector<std::string>& tok) {
        if (tok.size() != 2) {
            reply(s, "ERROR invalid arguments");
            return;
        }
        std::string resp;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (s->role == Role::MarketData) {
                resp = "ERROR not permitted";
            } else {
                s->role = Role::Trader;
                if (s->logged_in) {
                    resp = "ERROR already logged in";
                } else if (tok[1].empty()) {
                    resp = "ERROR invalid arguments";
                } else if (traders_.count(tok[1])) {
                    resp = "ERROR username taken";
                } else {
                    s->username = tok[1];
                    s->logged_in = true;
                    traders_[tok[1]] = s;
                    resp = "OK";
                }
            }
        }
        reply(s, resp);
    }

    static const char* trader_err(const std::shared_ptr<Session>& s) {
        if (s->role == Role::MarketData) {
            return "ERROR not permitted";
        }
        if (s->role != Role::Trader || !s->logged_in) {
            return "ERROR not logged in";
        }
        return nullptr;
    }

    const char* bind_md(const std::shared_ptr<Session>& s) {
        if (s->role == Role::Trader) {
            return "ERROR not permitted";
        }
        s->role = Role::MarketData;
        md_clients_[s.get()] = s;
        return nullptr;
    }

    void order_cmd(const std::shared_ptr<Session>& s, const std::vector<std::string>& tok) {
        if (tok.size() != 4) {
            reply(s, "ERROR invalid arguments");
            return;
        }
        Instrument inst;
        int qty = 0;
        int price = 0;
        if (!parse_instrument(tok[1], inst)) {
            reply(s, "ERROR unsupported instrument");
            return;
        }
        if (!parse_i32(tok[2], qty, kMinPositive, kMaxInt)) {
            reply(s, "ERROR invalid quantity");
            return;
        }
        if (!parse_i32(tok[3], price, kMinPositive, kMaxInt)) {
            reply(s, "ERROR invalid price");
            return;
        }

        std::vector<PendingSend> out;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (const char* err = trader_err(s)) {
                out.push_back({s, err});
            } else if (next_order_id_ > kMaxInt) {
                out.push_back({s, "ERROR invalid order id"});
            } else {
                Order o;
                o.id = next_order_id_++;
                o.side = (tok[0] == "BUY") ? Side::Buy : Side::Sell;
                o.inst = inst;
                o.price = price;
                o.remaining = qty;
                o.owner = s;
                orders_[o.id] = o;
                out.push_back({s, "ORDER_ACCEPTED " + std::to_string(o.id)});
                match(o.id, out);
            }
        }
        flush(out);
    }

    void match(int incoming_id, std::vector<PendingSend>& out) {
        auto it = orders_.find(incoming_id);
        if (it == orders_.end()) {
            return;
        }
        Order& incoming = it->second;
        Book& book = books_[inst_index(incoming.inst)];
        auto& opposite = (incoming.side == Side::Buy) ? book.sells[incoming.price]
                                                      : book.buys[incoming.price];

        while (incoming.remaining > 0 && !opposite.empty()) {
            int rest_id = opposite.front();
            auto rit = orders_.find(rest_id);
            if (rit == orders_.end() || rit->second.remaining <= 0) {
                opposite.pop_front();
                continue;
            }
            Order& rest = rit->second;
            int tq = incoming.remaining < rest.remaining ? incoming.remaining : rest.remaining;
            int tp = incoming.price;
            incoming.remaining -= tq;
            rest.remaining -= tq;

            std::shared_ptr<Session> buyer =
                (incoming.side == Side::Buy) ? incoming.owner.lock() : rest.owner.lock();
            std::shared_ptr<Session> seller =
                (incoming.side == Side::Sell) ? incoming.owner.lock() : rest.owner.lock();
            std::string insts = instrument_name(incoming.inst);
            std::string payload = insts + " " + std::to_string(tq) + " " + std::to_string(tp);
            if (buyer && buyer->alive) {
                out.push_back({buyer, "BOUGHT " + payload});
            }
            if (seller && seller->alive) {
                out.push_back({seller, "SOLD " + payload});
            }
            for (const auto& kv : md_clients_) {
                const std::shared_ptr<Session>& md = kv.second;
                if (md && md->alive && md->subscribed(incoming.inst)) {
                    out.push_back({md, "TRADE " + payload});
                }
            }

            if (rest.remaining == 0) {
                opposite.pop_front();
                orders_.erase(rest_id);
            }
        }

        if (incoming.remaining == 0) {
            orders_.erase(incoming_id);
            return;
        }
        auto& same = (incoming.side == Side::Buy) ? book.buys[incoming.price]
                                                  : book.sells[incoming.price];
        same.push_back(incoming_id);
    }

    void cancel_cmd(const std::shared_ptr<Session>& s, const std::vector<std::string>& tok) {
        if (tok.size() != 2) {
            reply(s, "ERROR invalid arguments");
            return;
        }
        int oid = 0;
        if (!parse_i32(tok[1], oid, kMinOrderId, kMaxInt)) {
            reply(s, "ERROR invalid order id");
            return;
        }
        std::string resp;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (const char* err = trader_err(s)) {
                resp = err;
            } else {
                auto it = orders_.find(oid);
                if (it == orders_.end() || it->second.remaining <= 0) {
                    resp = "ERROR order not found";
                } else {
                    auto owner = it->second.owner.lock();
                    if (owner.get() != s.get()) {
                        resp = "ERROR order not found";
                    } else {
                        Order& o = it->second;
                        Book& book = books_[inst_index(o.inst)];
                        auto& q = (o.side == Side::Buy) ? book.buys[o.price] : book.sells[o.price];
                        remove_id(q, oid);
                        orders_.erase(it);
                        resp = "ORDER_CANCELLED " + std::to_string(oid);
                    }
                }
            }
        }
        reply(s, resp);
    }

    void sub_cmd(const std::shared_ptr<Session>& s, const std::vector<std::string>& tok) {
        if (tok.size() != 2) {
            reply(s, "ERROR invalid arguments");
            return;
        }
        Instrument inst;
        if (!parse_instrument(tok[1], inst)) {
            reply(s, "ERROR unsupported instrument");
            return;
        }
        std::string resp;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (const char* err = bind_md(s)) {
                resp = err;
            } else {
                bool on = (tok[0] == "SUBSCRIBE");
                if (inst == Instrument::JNST) {
                    s->sub_jnst = on;
                } else {
                    s->sub_imct = on;
                }
                resp = "OK";
            }
        }
        reply(s, resp);
    }

    static void flush(const std::vector<PendingSend>& out) {
        for (const auto& p : out) {
            if (p.session) {
                p.session->send(p.msg);
            }
        }
    }
};

static void apply_write_interest(IoLoop& loop, std::unordered_map<int, std::shared_ptr<Session>>& conns) {
    for (int fd : g_need_unwrite) {
        auto it = conns.find(fd);
        if (it != conns.end() && it->second->send_buf.empty()) {
            loop.remove_write(fd);
        }
    }
    for (int fd : g_need_write) {
        auto it = conns.find(fd);
        if (it != conns.end() && !it->second->send_buf.empty()) {
            loop.add_write(fd);
        }
    }
    g_need_unwrite.clear();
    g_need_write.clear();
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: exchange-server <address> <port>\n";
        return 1;
    }
    ignore_sigpipe();
    if (!raise_open_file_limit()) {
        std::cerr << "warning: could not raise RLIMIT_NOFILE; check ulimit -n\n";
    } else {
        struct rlimit r {};
        if (getrlimit(RLIMIT_NOFILE, &r) == 0) {
            std::cerr << "RLIMIT_NOFILE " << r.rlim_cur << "\n";
        }
    }

    int lfd = tcp_listen(argv[1], argv[2]);
    if (lfd < 0) {
        std::cerr << "failed to listen on " << argv[1] << " " << argv[2] << "\n";
        return 1;
    }
    set_nonblock(lfd);

    IoLoop loop;
    if (!loop.valid() || !loop.add_read(lfd)) {
        std::cerr << "failed to start I/O loop\n";
        return 1;
    }

    Exchange ex;
    std::unordered_map<int, std::shared_ptr<Session>> conns;
#ifdef __FreeBSD__
    std::cerr << "listening on " << argv[1] << " " << argv[2] << " (kqueue)\n";
#else
    std::cerr << "listening on " << argv[1] << " " << argv[2] << " (poll)\n";
#endif

    auto drop = [&](int fd) {
        auto it = conns.find(fd);
        if (it == conns.end()) {
            return;
        }
        auto sess = it->second;
        loop.remove(fd);
        conns.erase(it);
        ex.disconnect(sess);
        if (sess->fd >= 0) {
            close(sess->fd);
            sess->fd = -1;
        }
        sess->alive = false;
    };

    auto accept_all = [&]() {
        while (true) {
            int cfd = accept(lfd, nullptr, nullptr);
            if (cfd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                break;
            }
            configure_accepted_socket(cfd);
            auto sess = std::make_shared<Session>();
            sess->fd = cfd;
            conns[cfd] = sess;
            if (!loop.add_read(cfd)) {
                conns.erase(cfd);
                close(cfd);
            }
        }
    };

    auto readable = [&](const std::shared_ptr<Session>& sess) -> bool {
        char tmp[4096];
        bool peer_closed = false;
        while (true) {
            ssize_t n = recv(sess->fd, tmp, sizeof(tmp), 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            }
            if (n == 0) {
                // A final recv can contain complete application lines before
                // the EOF.  Dispatch those lines before dropping the fd.
                peer_closed = true;
                break;
            }
            sess->reader.append(tmp, static_cast<size_t>(n));
            if (sess->reader.overflow()) {
                return false;
            }
        }
        std::string line;
        while (sess->reader.pop_line(line)) {
            ex.handle_line(sess, line);
            if (!sess->alive) {
                return false;
            }
        }
        return !peer_closed;
    };

    while (true) {
        std::vector<std::pair<int, int>> events;
        if (loop.wait(events) < 0) {
            continue;
        }
        std::unordered_set<int> closing;
        for (const auto& ev : events) {
            int fd = ev.first;
            int mask = ev.second;
            if (fd == lfd) {
                accept_all();
                continue;
            }
            auto it = conns.find(fd);
            if (it == conns.end()) {
                continue;
            }
            auto sess = it->second;
            if (mask & EV_ERR) {
                closing.insert(fd);
                continue;
            }
            if (mask & EV_READ) {
                if (!readable(sess)) {
                    closing.insert(fd);
                    continue;
                }
            }
            if ((mask & EV_WRITE) && !sess->send_buf.empty()) {
                if (!sess->flush_send()) {
                    closing.insert(fd);
                    continue;
                }
                sess->note_write();
            }
            if (mask & EV_HUP) {
                closing.insert(fd);
            }
        }
        apply_write_interest(loop, conns);
        for (int fd : closing) {
            drop(fd);
        }
    }
}
