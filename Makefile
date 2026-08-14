CXX = g++
CXXFLAGS = -std=c++20 -O3 -pthread -Wall -Wextra -Iinclude -march=native -flto
TARGET = bin/trading_engine

SRCS = src/main.cpp
HEADERS = $(wildcard include/*.hpp)

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -rf bin

test: $(TARGET)
	./$(TARGET) --run-tests

benchmark: $(TARGET)
	./$(TARGET) --run-benchmark

live: $(TARGET)
	./$(TARGET) --run-live

backtest: $(TARGET)
	./$(TARGET) --run-backtest

.PHONY: all clean test benchmark live backtest
