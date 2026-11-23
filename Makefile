.DEFAULT_GOAL := all

CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -I. -Iinclude -Isrc -I.deps/zstd-dev/usr/include
LDLIBS = -lssl -lcrypto -pthread /lib/x86_64-linux-gnu/libzstd.so.1

PROTO_SRCS = src/protocol/wire.cpp src/base/log.cpp src/base/clock.cpp
BOARD_SRCS = src/board/board.cpp $(PROTO_SRCS)
QUEUE_SRCS = src/paint/queue.cpp
PAINTER_SRCS = src/paint/token_painter.cpp
WORK_SRCS = src/net/work_connection.cpp $(PAINTER_SRCS) $(PROTO_SRCS)
CLASSIC_SRCS = src/scheduler/classic_scheduler.cpp src/scheduler/target_builder.cpp \
	src/scheduler/result_handler.cpp $(QUEUE_SRCS) $(BOARD_SRCS)
APP_SRCS = src/app/cli.cpp src/app/engine.cpp src/app/main.cpp $(CLASSIC_SRCS) \
	src/scheduler/sonar_tiling.cpp src/scheduler/new_sch1122.cpp \
	src/paint/token_painter.cpp src/net/transport.cpp src/net/public_client.cpp \
	src/net/work_connection.cpp
# 不含 main() 的应用层，供需要自己写 main 的集成测试复用。
ENGINE_SRCS = $(filter-out src/app/main.cpp,$(APP_SRCS))

.PHONY: all fix-mtime test test-fake-server test-protocol test-board test-replay test-net test-queue test-painter test-classic-target test-classic-scheduler test-new-scheduler test-new-paint test-new-sonar test-new-repair test-sonar-tiling test-cli test-alternate-scheduler clean

# 跨设备同步可能把源文件的 mtime 带成“未来时间”，make 会误判为无需重编
# （并打印“时钟错误”警告）。发现 make 不重新编译时先跑：make fix-mtime
fix-mtime:
	find . -path ./.git -prune -o -path ./repo -prune -o -type f -newermt '+1 minute' -exec touch {} +

all: main

test: test-protocol test-board test-replay test-net test-queue test-painter test-classic-target test-classic-scheduler test-new-scheduler test-new-paint test-new-sonar test-new-repair test-sonar-tiling test-cli test-alternate-scheduler

test-net:
	$(CXX) $(CXXFLAGS) -fsyntax-only src/net/transport.cpp src/net/public_client.cpp src/net/work_connection.cpp

test-queue: testfiles/queue_test

test-painter: testfiles/fake_transport_test

test-classic-target: testfiles/classic_target_test

test-classic-scheduler: testfiles/classic_scheduler_test

test-new-scheduler: testfiles/new_sch1122_test

test-new-sonar: testfiles/new_sch1122_sonar_test

test-new-paint: testfiles/new_sch1122_paint_test

test-new-repair: testfiles/new_sch1122_repair_test

test-sonar-tiling: testfiles/sonar_tiling_test

test-cli: testfiles/cli_test

test-alternate-scheduler: testfiles/alternate_scheduler_test

test-protocol: testfiles/protocol_test testfiles/tick_selftest

test-board: testfiles/board_test

test-replay: testfiles/replay_main

testfiles/protocol_test: testfiles/protocol_test.cpp $(PROTO_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/tick_selftest: testfiles/tick_selftest.cpp $(PROTO_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/board_test: testfiles/board_test.cpp $(BOARD_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/replay_main: testfiles/replay_main.cpp $(BOARD_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/queue_test: testfiles/queue_test.cpp $(QUEUE_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/fake_transport_test: testfiles/fake_transport_test.cpp $(WORK_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/classic_target_test: testfiles/classic_target_test.cpp $(CLASSIC_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/classic_scheduler_test: testfiles/classic_scheduler_test.cpp $(CLASSIC_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/sonar_tiling_test: testfiles/sonar_tiling_test.cpp \
		src/scheduler/sonar_tiling.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

# 复用 classic_scheduler_test.cpp 的 8 个场景，只用宏换被测调度器类型。
testfiles/new_sch1122_test: testfiles/new_sch1122_test.cpp \
		testfiles/classic_scheduler_test.cpp src/scheduler/new_sch1122.cpp \
		src/scheduler/sonar_tiling.cpp $(CLASSIC_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) testfiles/new_sch1122_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS) -o $@ $(LDLIBS)

# 调度层集成测试：密铺分块 -> affinity -> step 派发。
testfiles/new_sch1122_sonar_test: testfiles/new_sch1122_sonar_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) testfiles/new_sch1122_sonar_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS) -o $@ $(LDLIBS)

# 调度层集成测试：paint 矩形分块 + 块内蛇形 + 块内重刷。
testfiles/new_sch1122_paint_test: testfiles/new_sch1122_paint_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS)
	$(CXX) $(CXXFLAGS) testfiles/new_sch1122_paint_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS) -o $@ $(LDLIBS)

# 调度层集成测试：repair 匀配（16 自由槽 + 绑定槽）与 claim_repair。
testfiles/new_sch1122_repair_test: testfiles/new_sch1122_repair_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS)
	$(CXX) $(CXXFLAGS) testfiles/new_sch1122_repair_test.cpp \
		src/scheduler/new_sch1122.cpp src/scheduler/sonar_tiling.cpp \
		$(CLASSIC_SRCS) -o $@ $(LDLIBS)

testfiles/cli_test: testfiles/cli_test.cpp src/app/cli.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

testfiles/alternate_scheduler_test: testfiles/alternate_scheduler_test.cpp \
		src/scheduler/round_robin.cpp $(QUEUE_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

HDRS := $(shell find include src -name '*.h' -o -name '*.hpp') websock.hpp common_head.hpp zstdwrap.hpp

# v1 假服务端：真实 TLS/WS/zstd，只响应客户端；测试显式使用 NewSch1122。
testfiles/fake_server_cert.pem testfiles/fake_server_key.pem:
	openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
		-keyout testfiles/fake_server_key.pem \
		-out testfiles/fake_server_cert.pem \
		-subj "/CN=127.0.0.1" \
		-addext "subjectAltName=IP:127.0.0.1"

testfiles/fake_server_test: testfiles/fake_server_test.cpp $(ENGINE_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) testfiles/fake_server_test.cpp $(ENGINE_SRCS) -o $@ $(LDLIBS)

test-fake-server: testfiles/fake_server_test testfiles/fake_server_cert.pem testfiles/fake_server_key.pem
	./testfiles/fake_server_test "$(CURDIR)/testfiles/fake_server_cert.pem" "$(CURDIR)/testfiles/fake_server_key.pem"

main: $(APP_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(APP_SRCS) -o $@.tmp $(LDLIBS)
	mv -f $@.tmp $@

clean:
	rm -f main main.tmp paintboard_new testfiles/protocol_test testfiles/tick_selftest testfiles/board_test testfiles/replay_main testfiles/queue_test testfiles/fake_transport_test testfiles/classic_target_test testfiles/classic_scheduler_test testfiles/new_sch1122_test testfiles/new_sch1122_sonar_test testfiles/new_sch1122_repair_test testfiles/new_sch1122_paint_test testfiles/sonar_tiling_test testfiles/cli_test testfiles/alternate_scheduler_test testfiles/fake_server_test testfiles/fake_server_cert.pem testfiles/fake_server_key.pem
