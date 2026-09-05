#include "common.hpp"

#include <atomic>
#include <algorithm>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

enum class Role { Unknown, Trader, MarketData };
enum class Side { Buy, Sell };

struct Session {
    int fd = -1;
    Role role = Role::Unknown;
    bool logged_in = false;
    std::string username;
    bool sub_jnst = false;
    bool sub_imct = false;
    std::atomic<bool> alive{true};
    std::mutex send_mu;

    bool send(const std::string& msg) {
        std::lock_guard<std::mutex> g(send_mu);
        if (!alive.load() || fd < 0) {
            return false;
        }
        return send_line(fd, msg);
    }

    void shutdown_fd() {
        std::lock_guard<std::mutex> g(send_mu);
        alive.store(false);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
            fd = -1;
        }
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
            s->shutdown_fd();
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
        s->alive.store(false);
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
            if (buyer && buyer->alive.load()) {
                out.push_back({buyer, "BOUGHT " + payload});
            }
            if (seller && seller->alive.load()) {
                out.push_back({seller, "SOLD " + payload});
            }
            for (const auto& kv : md_clients_) {
                const std::shared_ptr<Session>& md = kv.second;
                if (md && md->alive.load() && md->subscribed(incoming.inst)) {
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

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: exchange-server <address> <port>\n";
        return 1;
    }
    ignore_sigpipe();
    int lfd = tcp_listen(argv[1], argv[2]);
    if (lfd < 0) {
        std::cerr << "failed to listen on " << argv[1] << " " << argv[2] << "\n";
        return 1;
    }

    Exchange ex;
    std::cerr << "listening on " << argv[1] << " " << argv[2] << "\n";

    while (true) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            continue;
        }
        set_nosigpipe(cfd);
        auto sess = std::make_shared<Session>();
        sess->fd = cfd;
        std::thread([sess, &ex]() {
            LineReader reader(sess->fd);
            std::string line;
            while (sess->fd >= 0) {
                int rc = reader.read_line(line);
                if (rc <= 0) {
                    break;
                }
                ex.handle_line(sess, line);
                if (!sess->alive.load()) {
                    break;
                }
            }
            ex.disconnect(sess);
            sess->shutdown_fd();
        }).detach();
    }
}
