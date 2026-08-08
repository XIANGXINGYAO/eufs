#pragma once

#include "journal/ondisk_journal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eufs::journal {

// 在当前 control 基础上推导一次事务占用的全部 ring 位置及暴露后的 control。
struct RingReservationPlan {
  std::uint64_t transaction_id{0};
  std::uint32_t descriptor_ring_index{0};
  // descriptor 后依次排列所有 payload，最后一个位置固定为 COMMIT。
  std::vector<std::uint32_t> payload_ring_indices;
  std::uint32_t commit_ring_index{0};
  // 仅生成纯内存计划，不写 ring 或 control。
  JournalControl exposed_control;
};

// 空 journal 上为 metadata_payload_count 个块规划 descriptor/payload/COMMIT；
// v1 不允许多个未 checkpoint 事务并存，容量不足返回 ENOSPC。
int PlanRingReservation(const JournalControl& current,
                        std::size_t metadata_payload_count,
                        RingReservationPlan* output, std::string* detail);

}  // namespace eufs::journal
