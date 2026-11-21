#include "paintboard/board.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
  paintboard::Board b;
  paintboard::Rules r;
  r.width = 4;
  r.height = 3;
  r.radius = 1;
  b.apply_rules(r);
  assert(b.view().rules.width == 4);
  assert(b.view().rules.height == 3);
  assert(b.since() == 0);

  r.width = 2;
  r.height = 2;
  b.apply_rules(r);
  assert(b.view().rules.width == 2);
  assert(b.view().rules.height == 2);

  std::vector<std::uint8_t> rgb;
  assert(b.copy_canvas_rgb(rgb));
  assert(rgb.size() == 2u * 2u * 3u);

  paintboard::SonarBatch s;
  s.radius = 1;
  s.tick = 5;
  s.center = {0};
  s.color.assign(5, 0xFF0000FFu);
  b.apply_sonar(s);
  assert(b.sonar_snapshot().tick[0] == 5);
  return 0;
}
