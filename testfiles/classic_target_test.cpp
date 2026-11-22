#include "paintboard/scheduler.hpp"
#include "scheduler/target_builder.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace paintboard;

namespace {

Rules make_rules() {
  Rules rules;
  rules.width = 4;
  rules.height = 3;
  rules.radius = 1;
  rules.tps = 100;
  rules.cooldown_base = 20;
  rules.cooldown_per_px = 2;
  rules.max_slots = 16;
  return rules;
}

void test_plan() {
  Plan plan;
  plan.alloc(12, 1);

  assert(!plan.done(6));
  assert(plan.claim(6));
  assert(!plan.claim(6));
  plan.release(6);
  assert(plan.claim(6));
  plan.mark_done(6);
  assert(plan.done(6));
  assert(!plan.claim(6));
  assert(plan.done_total() == 1);

  assert(plan.claim(2));
  assert(plan.claim_sonar(2));
  assert(plan.claimed_sonar(2));
  plan.release_sonar(2);
  assert(!plan.claimed_sonar(2));
  assert(plan.claimed(2));

  plan.reset();
  assert(!plan.done(6));
  assert(plan.done_total() == 0);
}

void test_empty_target() {
  const Rules rules = make_rules();
  const Config config;
  Board board;
  const Target target = build_target(config, rules, board);

  assert(target.built);
  assert(target.known_idx.empty());
  assert(!target.pix.empty());
  assert(!target_complete(target, Plan{}));
}

void test_draw_target() {
  const Rules rules = make_rules();
  Config config;
  config.draws.push_back(Draw{2, 1, 0xFF112233u});
  Board board;

  const Target target = build_target(config, rules, board);
  assert(target.known_idx.size() == 1);
  assert(target.known_idx[0] == 6);
  assert(target.known[6] == 1);
  assert(target.pix[6] == 0xFF112233u);
  assert(target.bb_x0 == 2 && target.bb_y0 == 1);
  assert(target.bb_x1 == 2 && target.bb_y1 == 1);

  Plan plan;
  plan.alloc(12, target.known_idx.size());
  assert(!target_complete(target, plan));
  assert(plan.claim(6));
  plan.mark_done(6);
  assert(target_complete(target, plan));
}

void test_target_builder_direct() {
  const Rules rules = make_rules();
  Config config;
  config.draws.push_back(Draw{2, 1, 0xFF112233u});

  const Target target = TargetBuilder{}.build(config, rules);
  assert(target.built);
  assert(target.known_idx.size() == 1 && target.known_idx[0] == 6);
  assert(target.pix[6] == 0xFF112233u);
  assert(target.bb_x0 == 2 && target.bb_y0 == 1);
}

} // namespace

int main() {
  test_plan();
  test_empty_target();
  test_draw_target();
  test_target_builder_direct();
  std::puts("classic_target_test OK");
  return 0;
}
