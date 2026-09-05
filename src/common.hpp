#ifndef COMMON_HPP
#define COMMON_HPP

#include <string>
#include <vector>

const int kMinPositive = 1;
const int kMaxInt = 2147483647;
const int kMinOrderId = 0;
const int kMaxLine = 8192;

enum class Instrument { JNST, IMCT };

bool parse_instrument(const std::string& s, Instrument& out);
const char* instrument_name(Instrument inst);

bool parse_i32(const std::string& s, int& out, int min_v, int max_v);
std::vector<std::string> tokenize(const std::string& line);

void ignore_sigpipe();
void set_nosigpipe(int fd);

int tcp_listen(const std::string& host, const std::string& port);
int tcp_connect(const std::string& host, const std::string& port);

bool send_all(int fd, const std::string& data);
bool send_line(int fd, const std::string& msg);

class LineReader {
public:
    explicit LineReader(int fd);
    // 1 = line, 0 = EOF, -1 = error / overflow
    int read_line(std::string& line);

private:
    int fd_;
    std::string buf_;
};

void run_interactive_client(int fd);

#endif
