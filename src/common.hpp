#ifndef COMMON_HPP
#define COMMON_HPP

#include <string>
#include <vector>

const int kMinPositive = 1;
const int kMaxInt = 2147483647;
const int kMinOrderId = 0;
const int kMaxLine = 8192;
const int kSmallSockBuf = 4096;
const int kListenBacklog = 8192;

enum class Instrument { JNST, IMCT };

bool parse_instrument(const std::string& s, Instrument& out);
const char* instrument_name(Instrument inst);

bool parse_i32(const std::string& s, int& out, int min_v, int max_v);
std::vector<std::string> tokenize(const std::string& line);

void ignore_sigpipe();
void set_nosigpipe(int fd);
void set_nonblock(int fd);
void set_small_sockbufs(int fd);
void set_nodelay(int fd);
void configure_accepted_socket(int fd);
bool raise_open_file_limit();

int tcp_listen(const std::string& host, const std::string& port);
int tcp_connect(const std::string& host, const std::string& port);

bool send_all(int fd, const std::string& data);
bool send_line(int fd, const std::string& msg);

class LineReader {
public:
    LineReader();
    explicit LineReader(int fd);
    void append(const char* p, size_t n);
    bool overflow() const;
    bool pop_line(std::string& line);
    // 1 = line, 0 = EOF, -1 = error / overflow
    int read_line(std::string& line);

private:
    int fd_;
    std::string buf_;
};

void run_interactive_client(int fd);

#endif
