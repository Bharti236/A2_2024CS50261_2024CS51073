CXX := c++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS := -pthread

BUILD := build

.PHONY: all clean

all: $(BUILD)/exchange-server $(BUILD)/trader $(BUILD)/market-data $(BUILD)/conn-gen
	chmod +x server/run-server client/run-trader client/run-market-data bonus/run-conn-gen

$(BUILD)/exchange-server: src/server.cpp src/ioloop.cpp src/ioloop.hpp src/common.cpp src/common.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/server.cpp src/ioloop.cpp src/common.cpp $(LDFLAGS)

$(BUILD)/trader: src/trader.cpp src/common.cpp src/common.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/trader.cpp src/common.cpp $(LDFLAGS)

$(BUILD)/market-data: src/market_data.cpp src/common.cpp src/common.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/market_data.cpp src/common.cpp $(LDFLAGS)

$(BUILD)/conn-gen: src/conn_gen.cpp src/common.cpp src/common.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/conn_gen.cpp src/common.cpp $(LDFLAGS)

clean:
	rm -rf $(BUILD)
