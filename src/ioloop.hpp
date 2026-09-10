#ifndef IOLOOP_HPP
#define IOLOOP_HPP

#include <utility>
#include <vector>

#ifndef __FreeBSD__
#include <unordered_map>
#endif

const int EV_READ = 1;
const int EV_WRITE = 2;
const int EV_ERR = 4;
const int EV_HUP = 8;

class IoLoop {
public:
    IoLoop();
    ~IoLoop();
    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;

    bool valid() const;
    bool add_read(int fd);
    bool add_write(int fd);
    bool remove_write(int fd);
    bool remove(int fd);
    int wait(std::vector<std::pair<int, int>>& events);

private:
#ifdef __FreeBSD__
    int kq_ = -1;
#else
    std::unordered_map<int, short> want_;
#endif
};

#endif
