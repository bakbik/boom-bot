# Host builds — no hardware or ESP32 toolchain required.
#   make            build + run the unit tests (protocol, control)
#   make gestures   render the gesture bank to gestures.svg
#   make clean      remove artifacts
CXX      ?= g++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -Werror -O2

.PHONY: all clean gestures
all: test_protocol test_control
	./test_protocol
	./test_control

test_protocol: test_protocol.cpp ../common/protocol.h
	$(CXX) $(CXXFLAGS) -o $@ test_protocol.cpp

test_control: test_control.cpp ../common/control.h
	$(CXX) $(CXXFLAGS) -o $@ test_control.cpp

gestures: render_gestures
	./render_gestures

render_gestures: render_gestures.cpp ../common/gestures.h ../common/protocol.h
	$(CXX) $(CXXFLAGS) -o $@ render_gestures.cpp

clean:
	rm -f test_protocol test_control render_gestures gestures.svg
