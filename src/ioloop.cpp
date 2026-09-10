#include "ioloop.hpp"

#include <cerrno>

#ifdef __FreeBSD__
#include <sys/event.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <poll.h>
#endif

#ifdef __FreeBSD__

IoLoop::IoLoop() : kq_(kqueue()) {}

IoLoop::~IoLoop() {
    if (kq_ >= 0) {
        close(kq_);
    }
}

bool IoLoop::valid() const {
    return kq_ >= 0;
}

bool IoLoop::add_read(int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    return kevent(kq_, &ev, 1, nullptr, 0, nullptr) != -1;
}

bool IoLoop::add_write(int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    return kevent(kq_, &ev, 1, nullptr, 0, nullptr) != -1;
}

bool IoLoop::remove_write(int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    return true;
}

bool IoLoop::remove(int fd) {
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, ev, 2, nullptr, 0, nullptr);
    return true;
}

int IoLoop::wait(std::vector<std::pair<int, int>>& events) {
    struct kevent evs[128];
    int n = kevent(kq_, nullptr, 0, evs, 128, nullptr);
    if (n < 0) {
        if (errno == EINTR) {
            events.clear();
            return 0;
        }
        return -1;
    }
    events.clear();
    events.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        int fd = static_cast<int>(evs[i].ident);
        int mask = 0;
        if (evs[i].flags & EV_ERROR) {
            mask |= EV_ERR;
        }
        if (evs[i].filter == EVFILT_READ) {
            mask |= EV_READ;
            if (evs[i].flags & EV_EOF) {
                mask |= EV_HUP;
            }
        } else if (evs[i].filter == EVFILT_WRITE) {
            mask |= EV_WRITE;
            if (evs[i].flags & EV_EOF) {
                mask |= EV_HUP;
            }
        }
        events.push_back({fd, mask});
    }
    return n;
}

#else

IoLoop::IoLoop() = default;

IoLoop::~IoLoop() = default;

bool IoLoop::valid() const {
    return true;
}

bool IoLoop::add_read(int fd) {
    want_[fd] = static_cast<short>(want_[fd] | POLLIN);
    return true;
}

bool IoLoop::add_write(int fd) {
    want_[fd] = static_cast<short>(want_[fd] | POLLOUT);
    return true;
}

bool IoLoop::remove_write(int fd) {
    auto it = want_.find(fd);
    if (it == want_.end()) {
        return true;
    }
    it->second = static_cast<short>(it->second & ~POLLOUT);
    if (it->second == 0) {
        want_.erase(it);
    }
    return true;
}

bool IoLoop::remove(int fd) {
    want_.erase(fd);
    return true;
}

int IoLoop::wait(std::vector<std::pair<int, int>>& events) {
    std::vector<pollfd> pfds;
    pfds.reserve(want_.size());
    for (const auto& kv : want_) {
        pollfd p {};
        p.fd = kv.first;
        p.events = kv.second;
        pfds.push_back(p);
    }
    int n = poll(pfds.data(), pfds.size(), -1);
    if (n < 0) {
        if (errno == EINTR) {
            events.clear();
            return 0;
        }
        return -1;
    }
    events.clear();
    for (const auto& p : pfds) {
        if (p.revents == 0) {
            continue;
        }
        int mask = 0;
        if (p.revents & POLLIN) {
            mask |= EV_READ;
        }
        if (p.revents & POLLOUT) {
            mask |= EV_WRITE;
        }
        if (p.revents & POLLERR) {
            mask |= EV_ERR;
        }
        if (p.revents & (POLLHUP | POLLNVAL)) {
            mask |= EV_HUP;
        }
        events.push_back({p.fd, mask});
    }
    return n;
}

#endif
