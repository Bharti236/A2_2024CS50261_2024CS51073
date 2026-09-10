#include "common.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sstream>
#include <thread>

bool parse_instrument(const std::string& s, Instrument& out) {
    if (s == "JNST") {
        out = Instrument::JNST;
        return true;
    }
    if (s == "IMCT") {
        out = Instrument::IMCT;
        return true;
    }
    return false;
}

const char* instrument_name(Instrument inst) {
    return inst == Instrument::JNST ? "JNST" : "IMCT";
}

bool parse_i32(const std::string& s, int& out, int min_v, int max_v) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || errno == ERANGE) {
        return false;
    }
    if (v < min_v || v > max_v) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tok;
    std::istringstream ss(line);
    std::string t;
    while (ss >> t) {
        tok.push_back(t);
    }
    return tok;
}

void ignore_sigpipe() {
    signal(SIGPIPE, SIG_IGN);
}

void set_nosigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        flags = 0;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_small_sockbufs(int fd) {
    int sz = kSmallSockBuf;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
}

void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

void configure_accepted_socket(int fd) {
    set_nosigpipe(fd);
    set_nonblock(fd);
    set_small_sockbufs(fd);
    set_nodelay(fd);
}

bool raise_open_file_limit() {
    struct rlimit r {};
    if (getrlimit(RLIMIT_NOFILE, &r) != 0) {
        return false;
    }
    rlim_t want = 131072;
    if (r.rlim_max != static_cast<rlim_t>(RLIM_INFINITY) && r.rlim_max < want) {
        want = r.rlim_max;
    }
    r.rlim_cur = want;
    return setrlimit(RLIMIT_NOFILE, &r) == 0;
}

static int set_reuse(int fd) {
    int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
}

int tcp_listen(const std::string& host, const std::string& port) {
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        set_reuse(fd);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, kListenBacklog) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        set_nosigpipe(fd);
    }
    return fd;
}

int tcp_connect(const std::string& host, const std::string& port) {
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        set_nosigpipe(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool send_line(int fd, const std::string& msg) {
    return send_all(fd, msg + "\n");
}

LineReader::LineReader() : fd_(-1) {}

LineReader::LineReader(int fd) : fd_(fd) {}

void LineReader::append(const char* p, size_t n) {
    buf_.append(p, n);
}

bool LineReader::overflow() const {
    return buf_.size() > static_cast<size_t>(kMaxLine);
}

bool LineReader::pop_line(std::string& line) {
    auto pos = buf_.find('\n');
    if (pos == std::string::npos) {
        return false;
    }
    line = buf_.substr(0, pos);
    buf_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

int LineReader::read_line(std::string& line) {
    while (true) {
        if (pop_line(line)) {
            return 1;
        }
        if (overflow()) {
            return -1;
        }
        char tmp[1024];
        ssize_t n = recv(fd_, tmp, sizeof(tmp), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 0;
        }
        buf_.append(tmp, static_cast<size_t>(n));
    }
}

void run_interactive_client(int fd) {
    std::atomic<bool> closed{false};
    std::thread rx([&]() {
        LineReader reader(fd);
        std::string line;
        while (reader.read_line(line) == 1) {
            std::cout << line << std::endl;
        }
        closed = true;
        std::cerr << "disconnected from server\n";
        shutdown(fd, SHUT_RDWR);
        close(STDIN_FILENO);
    });
    std::string line;
    while (!closed && std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!send_line(fd, line)) {
            break;
        }
        auto tok = tokenize(line);
        if (!tok.empty() && tok[0] == "QUIT") {
            break;
        }
    }
    shutdown(fd, SHUT_RDWR);
    rx.join();
    close(fd);
}
