// NewSch1122 复用 classic_scheduler_test 的 8 个确定性场景：
// 通过 PB_SCHED_* 宏把被测类型换成新调度器（见 Makefile 的 test-new-scheduler）。
// 当前 NewSch1122 是 ClassicScheduler 的逐行副本，所以两边必须同时全绿。
#define PB_SCHED_HEADER "scheduler/new_sch1122.hpp"
#define PB_SCHED_CLASS NewSch1122
#define PB_SCHED_NAME "new_sch1122_test"
#include "classic_scheduler_test.cpp"
