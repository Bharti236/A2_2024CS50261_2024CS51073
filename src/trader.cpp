#include "common.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: trader <address> <port> <username>\n";
        return 1;
    }
    ignore_sigpipe();
    int fd = tcp_connect(argv[1], argv[2]);
    if (fd < 0) {
        std::cerr << "failed to connect to " << argv[1] << " " << argv[2] << "\n";
        return 1;
    }
    if (!send_line(fd, std::string("LOGIN ") + argv[3])) {
        std::cerr << "failed to send LOGIN\n";
        return 1;
    }
    run_interactive_client(fd);
    return 0;
}
