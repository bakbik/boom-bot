# Host build of the protocol unit tests. No hardware or ESP32 toolchain.
#   make        build and run
#   make clean  remove artifacts
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -Werror -O2
BIN       = test_protocol

.PHONY: all clean
all: $(BIN)
	./$(BIN)

$(BIN): test_protocol.cpp ../common/protocol.h
	$(CXX) $(CXXFLAGS) -o $@ test_protocol.cpp

clean:
	rm -f $(BIN)
