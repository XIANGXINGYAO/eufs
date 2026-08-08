// 验证环形日志为 descriptor、payload 和 COMMIT 预留位置时的回绕数学与容量拒绝。
// 预留结果必须唯一，恢复端才能从 descriptor 推导 COMMIT 的合法位置。
#include "journal/ring_reservation.h"

#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

eufs::journal::JournalControl MakeCleanControl(std::uint32_t ring_blocks,
                                                std::uint32_t position) {
  eufs::journal::JournalControl control;
  control.ring_blocks = ring_blocks;
  control.filesystem_uuid[0] = 0x42;
  control.generation = 7;
  control.head = position;
  control.tail = position;
  control.next_transaction_id = 41;
  return control;
}

bool SameControl(const eufs::journal::JournalControl& first,
                 const eufs::journal::JournalControl& second) {
  return first.ring_blocks == second.ring_blocks &&
         first.filesystem_uuid == second.filesystem_uuid &&
         first.generation == second.generation && first.head == second.head &&
         first.tail == second.tail &&
         first.used_blocks == second.used_blocks &&
         first.state_flags == second.state_flags &&
         first.next_transaction_id == second.next_transaction_id &&
         first.feature_compat == second.feature_compat &&
         first.feature_ro_compat == second.feature_ro_compat &&
         first.feature_incompat == second.feature_incompat &&
         first.checksum == second.checksum;
}

bool SamePlan(const eufs::journal::RingReservationPlan& first,
              const eufs::journal::RingReservationPlan& second) {
  return first.transaction_id == second.transaction_id &&
         first.descriptor_ring_index == second.descriptor_ring_index &&
         first.payload_ring_indices == second.payload_ring_indices &&
         first.commit_ring_index == second.commit_ring_index &&
         SameControl(first.exposed_control, second.exposed_control);
}

void TestContiguousPlan() {
  const auto current = MakeCleanControl(8, 1);
  const auto before = current;
  eufs::journal::RingReservationPlan plan;
  std::string detail;

  Require(eufs::journal::PlanRingReservation(current, 2, &plan, &detail) == 0,
          detail.c_str());
  Require(plan.transaction_id == 41 && plan.descriptor_ring_index == 1 &&
              plan.payload_ring_indices ==
                  std::vector<std::uint32_t>({2, 3}) &&
              plan.commit_ring_index == 4,
          "contiguous transaction positions are wrong");
  Require(plan.exposed_control.generation == 8 &&
              plan.exposed_control.head == 5 &&
              plan.exposed_control.tail == 1 &&
              plan.exposed_control.used_blocks == 4 &&
              plan.exposed_control.next_transaction_id == 42,
          "contiguous exposure control is wrong");
  Require(SameControl(current, before), "planner mutated its input control");
}

void TestWrapPlan() {
  const auto current = MakeCleanControl(8, 6);
  eufs::journal::RingReservationPlan plan;
  std::string detail;

  Require(eufs::journal::PlanRingReservation(current, 3, &plan, &detail) == 0,
          detail.c_str());
  Require(plan.descriptor_ring_index == 6 &&
              plan.payload_ring_indices ==
                  std::vector<std::uint32_t>({7, 0, 1}) &&
              plan.commit_ring_index == 2 &&
              plan.exposed_control.head == 3 &&
              plan.exposed_control.tail == 6 &&
              plan.exposed_control.used_blocks == 5,
          "wrapped transaction positions are wrong");
}

void TestExactFillAndGenerationWrap() {
  auto current = MakeCleanControl(5, 3);
  current.generation = std::numeric_limits<std::uint64_t>::max();
  eufs::journal::RingReservationPlan plan;
  std::string detail;

  Require(eufs::journal::PlanRingReservation(current, 3, &plan, &detail) == 0,
          detail.c_str());
  Require(plan.descriptor_ring_index == 3 &&
              plan.payload_ring_indices ==
                  std::vector<std::uint32_t>({4, 0, 1}) &&
              plan.commit_ring_index == 2 &&
              plan.exposed_control.head == 3 &&
              plan.exposed_control.tail == 3 &&
              plan.exposed_control.used_blocks == 5 &&
              plan.exposed_control.generation == 0,
          "exact-fill or generation-wrap plan is wrong");
}

void TestPolicyAndCapacityFailuresPreserveOutput() {
  eufs::journal::RingReservationPlan sentinel;
  sentinel.transaction_id = 999;
  sentinel.descriptor_ring_index = 7;
  sentinel.payload_ring_indices = {6, 5};
  sentinel.commit_ring_index = 4;
  sentinel.exposed_control = MakeCleanControl(8, 3);
  const auto expected = sentinel;
  std::string detail;

  auto current = MakeCleanControl(4, 1);
  Require(eufs::journal::PlanRingReservation(current, 3, &sentinel, &detail) ==
              -ENOSPC &&
              SamePlan(sentinel, expected),
          "ENOSPC changed the output plan");

  current = MakeCleanControl(8, 1);
  current.head = 5;
  current.used_blocks = 4;
  Require(eufs::journal::PlanRingReservation(current, 1, &sentinel, &detail) ==
              -EBUSY &&
              SamePlan(sentinel, expected),
          "EBUSY policy check changed the output plan");

  current = MakeCleanControl(8, 1);
  Require(eufs::journal::PlanRingReservation(current, 0, &sentinel, &detail) ==
              -EINVAL &&
              SamePlan(sentinel, expected),
          "empty transaction rejection changed the output plan");

  current.next_transaction_id = std::numeric_limits<std::uint64_t>::max();
  Require(eufs::journal::PlanRingReservation(current, 1, &sentinel, &detail) ==
              -EOVERFLOW &&
              SamePlan(sentinel, expected),
          "txid overflow rejection changed the output plan");
}

void TestDescriptorAndControlValidation() {
  eufs::journal::RingReservationPlan plan;
  std::string detail;
  auto current = MakeCleanControl(300, 0);

  Require(eufs::journal::PlanRingReservation(
              current, eufs::journal::kMaxDescriptorEntries + 1U, &plan,
              &detail) == -E2BIG,
          "descriptor overflow did not return E2BIG");

  current = MakeCleanControl(8, 1);
  current.tail = 2;
  Require(eufs::journal::PlanRingReservation(current, 1, &plan, &detail) ==
              -EINVAL,
          "invalid current control was accepted");
  current = MakeCleanControl(8, 1);
  Require(eufs::journal::PlanRingReservation(current, 1, nullptr, &detail) ==
              -EINVAL,
          "null output was accepted");
}

}  // namespace

int main() {
  TestContiguousPlan();
  TestWrapPlan();
  TestExactFillAndGenerationWrap();
  TestPolicyAndCapacityFailuresPreserveOutput();
  TestDescriptorAndControlValidation();
  std::cout << "contiguous=ok wrap=ok exact_fill=ok busy=ok overflow=ok\n";
  std::cout << "PASS: pure v1 journal ring reservation planner test\n";
  return 0;
}
