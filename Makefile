CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I. -Iinclude -Isrc -I.deps/zstd-dev/usr/include
LDLIBS = -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1

PROTO_SRCS = src/protocol/wire.cpp src/base/log.cpp src/base/clock.cpp
BOARD_SRCS = src/board/board.cpp $(PROTO_SRCS)
QUEUE_SRCS = src/paint/queue.cpp
PAINTER_SRCS = src/paint/token_painter.cpp
WORK_SRCS = src/net/work_connection.cpp $(PAINTER_SRCS) $(PROTO_SRCS)
CLASSIC_SRCS = src/scheduler/classic_scheduler.cpp $(QUEUE_SRCS) $(BOARD_SRCS)

.PHONY: all test test-protocol test-board test-replay test-net test-queue test-painter test-classic-target clean

all: main

test: test-protocol test-board test-replay test-net test-queue test-painter test-classic-target

test-net:
	$(CXX) $(CXXFLAGS) -fsyntax-only src/net/transport.cpp src/net/public_client.cpp src/net/work_connection.cpp

test-queue: testfiles/queue_test

test-painter: testfiles/fake_transport_test

test-classic-target: testfiles/classic_target_test

test-protocol: testfiles/protocol_test testfiles/tick_selftest

test-board: testfiles/board_test

test-replay: testfiles/replay_main

testfiles/protocol_test: testfiles/protocol_test.cpp $(PROTO_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/tick_selftest: testfiles/tick_selftest.cpp $(PROTO_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/board_test: testfiles/board_test.cpp $(BOARD_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/replay_main: testfiles/replay_main.cpp $(BOARD_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/queue_test: testfiles/queue_test.cpp $(QUEUE_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/fake_transport_test: testfiles/fake_transport_test.cpp $(WORK_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/classic_target_test: testfiles/classic_target_test.cpp $(CLASSIC_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

clean:
	rm -f testfiles/protocol_test testfiles/tick_selftest testfiles/board_test testfiles/replay_main testfiles/queue_test testfiles/fake_transport_test testfiles/classic_target_test
