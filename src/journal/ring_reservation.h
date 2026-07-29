#pragma once

#include "journal/ondisk_journal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eufs::journal {

struct RingReservationPlan {
  std::uint64_t transaction_id{0};
  std::uint32_t descriptor_ring_index{0};
  std::vector<std::uint32_t> payload_ring_indices;
  std::uint32_t commit_ring_index{0};
  JournalControl exposed_control;
};

int PlanRingReservation(const JournalControl& current,
                        std::size_t metadata_payload_count,
                        RingReservationPlan* output, std::string* detail);

}  // namespace eufs::journal
