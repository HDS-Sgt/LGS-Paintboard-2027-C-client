#include "paintboard/paint_queue.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
  paintboard::PaintQueue q;
  paintboard::PaintOp a;
  a.id = 1;
  a.idx = 10;
  a.color = 0xFF0000FFu;
  a.group = paintboard::Group::Paint;
  a.due = 0;
  paintboard::PaintOp b;
  b.id = 2;
  b.idx = 11;
  b.color = 0xFF0000FFu;
  b.group = paintboard::Group::Repair;
  b.due = 0;
  paintboard::PaintOp c;
  c.id = 3;
  c.idx = 12;
  c.color = 0xFF0000FFu;
  c.group = paintboard::Group::Sonar;
  c.due = 100;
  q.push(a);
  q.push(b);
  q.push(c);
  assert(q.size() == 3);

  std::vector<paintboard::PaintOp> due;
  q.take_due(0, due);
  assert(due.size() == 2);
  assert(q.size() == 3); // 取出不减
  q.consume(due[0]);
  assert(q.size() == 2); // 发送/丢弃才减
  q.requeue(due[1]);
  assert(q.size() == 2); // 重排队不变

  auto removed = q.remove_if([](const auto& op) {
    return op.group != paintboard::Group::Paint;
  });
  assert(removed.size() == 2);
  assert(q.size() == 0);
  return 0;
}
