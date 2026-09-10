#include "common.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

static std::vector<in_addr> parse_locals(const char* spec) {
    std::vector<in_addr> out;
    std::string s(spec);
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            if (!cur.empty()) {
                in_addr a {};
                if (inet_pton(AF_INET, cur.c_str(), &a) == 1) {
                    out.push_back(a);
                } else {
                    std::cerr << "bad local address: " << cur << "\n";
                }
                cur.clear();
            }
        } else if (s[i] != ' ') {
            cur.push_back(s[i]);
        }
    }
    return out;
}

static int start_one(const sockaddr_in& dst, const std::vector<in_addr>& locals, size_t& local_i) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    set_nosigpipe(fd);
    set_nonblock(fd);
    set_small_sockbufs(fd);
    set_nodelay(fd);
    if (!locals.empty()) {
        sockaddr_in src {};
        src.sin_family = AF_INET;
        src.sin_port = 0;
        src.sin_addr = locals[local_i % locals.size()];
        local_i++;
        if (bind(fd, reinterpret_cast<sockaddr*>(&src), sizeof(src)) != 0) {
            static bool warned_bind = false;
            if (!warned_bind) {
                warned_bind = true;
                std::cerr << "bind(" << inet_ntoa(src.sin_addr)
                          << ") failed: " << std::strerror(errno)
                          << " (continuing without that bind)\n";
            }
        }
    }
    int rc = connect(fd, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
    if (rc == 0) {
        return fd;
    }
    if (errno == EINPROGRESS) {
        return fd;
    }
    close(fd);
    return -1;
}

static int finish_connect(int fd) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        return -1;
    }
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: conn-gen <address> <port> <count> [local_ip,local_ip,...]\n";
        return 1;
    }
    ignore_sigpipe();
    raise_open_file_limit();

    int want = 0;
    if (!parse_i32(argv[3], want, 1, kMaxInt)) {
        std::cerr << "invalid count\n";
        return 1;
    }

    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(argv[1], argv[2], &hints, &res) != 0 || res == nullptr) {
        std::cerr << "failed to resolve " << argv[1] << " " << argv[2] << "\n";
        return 1;
    }
    sockaddr_in dst {};
    std::memcpy(&dst, res->ai_addr, sizeof(dst));
    freeaddrinfo(res);

    std::vector<in_addr> locals;
    if (argc == 5) {
        locals = parse_locals(argv[4]);
        if (locals.empty()) {
            std::cerr << "no valid local addresses\n";
            return 1;
        }
    } else {
        int n_ips = (want + 19999) / 20000;
        if (n_ips < 1) {
            n_ips = 1;
        }
        if (n_ips > 8) {
            n_ips = 8;
        }
        for (int i = 0; i < n_ips; ++i) {
            in_addr a {};
            char buf[32];
            std::snprintf(buf, sizeof(buf), "127.0.0.%d", i + 1);
            if (inet_pton(AF_INET, buf, &a) == 1) {
                locals.push_back(a);
            }
        }
        if (n_ips > 1) {
            std::cerr << "using " << n_ips
                      << " loopback addresses (127.0.0.1 .. 127.0.0." << n_ips
                      << "); on FreeBSD add aliases if bind fails:\n"
                      << "  ifconfig lo0 alias 127.0.0.2\n";
        }
    }

    const size_t target = static_cast<size_t>(want);
    const size_t inflight_cap = 512;
    std::vector<int> live;
    std::vector<int> pending;
    live.reserve(target);
    size_t local_i = 0;
    int consecutive_fail = 0;
    size_t last_report = 0;

    while (live.size() < target) {
        while (live.size() + pending.size() < target && pending.size() < inflight_cap) {
            int fd = start_one(dst, locals, local_i);
            if (fd < 0) {
                consecutive_fail++;
                if (consecutive_fail > 10000) {
                    std::cerr << "too many connect failures (errno " << errno << ")\n";
                    goto done;
                }
                break;
            }
            consecutive_fail = 0;
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr == 0) {
                sockaddr_in peer {};
                socklen_t pl = sizeof(peer);
                if (getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &pl) == 0) {
                    live.push_back(fd);
                    continue;
                }
            }
            pending.push_back(fd);
        }

        if (pending.empty()) {
            if (live.size() < target) {
                std::cerr << "cannot start more connections (have " << live.size() << ")\n";
            }
            break;
        }

        std::vector<pollfd> pfds(pending.size());
        for (size_t i = 0; i < pending.size(); ++i) {
            pfds[i].fd = pending[i];
            pfds[i].events = POLLOUT;
        }
        int n = poll(pfds.data(), pfds.size(), 5000);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            continue;
        }
        std::vector<int> still;
        still.reserve(pending.size());
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pfds[i].revents == 0) {
                still.push_back(pending[i]);
                continue;
            }
            if (finish_connect(pending[i]) == 0) {
                live.push_back(pending[i]);
            } else {
                close(pending[i]);
            }
        }
        pending.swap(still);

        if (live.size() >= last_report + 1000) {
            last_report = live.size();
            std::cerr << "established " << live.size() << " / " << target << "\n";
        }
    }

done:
    for (int fd : pending) {
        close(fd);
    }
    std::cout << "established " << live.size() << std::endl;
    if (live.size() < target) {
        std::cerr << "stopped short of " << target << " connections\n";
    } else {
        std::cerr << "idle; terminate to close\n";
    }
    while (true) {
        pause();
    }
}
