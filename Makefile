CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I. -Iinclude -Isrc -I.deps/zstd-dev/usr/include
LDLIBS = -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1

PROTO_SRCS = src/protocol/wire.cpp src/base/log.cpp src/base/clock.cpp

.PHONY: all test test-protocol clean

all: main

test: test-protocol

test-protocol: testfiles/protocol_test testfiles/tick_selftest

testfiles/protocol_test: testfiles/protocol_test.cpp $(PROTO_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/tick_selftest: testfiles/tick_selftest.cpp $(PROTO_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

clean:
	rm -f testfiles/protocol_test testfiles/tick_selftest
