#include "journal/ring_reservation.h"

#include <cerrno>
#include <limits>
#include <string_view>
#include <utility>

namespace eufs::journal {
namespace {

int Fail(std::string* detail, int error_number, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
  return -error_number;
}

std::uint32_t Advance(std::uint32_t start, std::size_t distance,
                      std::uint32_t ring_blocks) {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(start) + distance) % ring_blocks);
}

}  // namespace

int PlanRingReservation(const JournalControl& current,
                        std::size_t metadata_payload_count,
                        RingReservationPlan* output, std::string* detail) {
  if (output == nullptr) {
    return Fail(detail, EINVAL, "ring reservation output is required");
  }

  ondisk::Block encoded_control{};
  std::string validation_error;
  if (!EncodeControl(current, &encoded_control, nullptr, &validation_error)) {
    if (detail != nullptr) {
      detail->assign("current journal control is invalid: ");
      detail->append(validation_error);
    }
    return -EINVAL;
  }
  if (current.used_blocks != 0) {
    return Fail(detail, EBUSY,
                "v1 requires recovery or checkpoint before another "
                "reservation");
  }
  if (metadata_payload_count == 0) {
    return Fail(detail, EINVAL,
                "a journal transaction requires at least one metadata "
                "payload");
  }
  if (metadata_payload_count > kMaxDescriptorEntries) {
    return Fail(detail, E2BIG,
                "metadata payload count exceeds the descriptor capacity");
  }

  const std::size_t transaction_blocks = metadata_payload_count + 2U;
  if (transaction_blocks > current.ring_blocks) {
    return Fail(detail, ENOSPC,
                "the complete transaction does not fit in the journal ring");
  }
  if (current.next_transaction_id ==
      std::numeric_limits<std::uint64_t>::max()) {
    return Fail(detail, EOVERFLOW,
                "v1 transaction identifiers do not wrap through zero");
  }

  RingReservationPlan candidate;
  candidate.transaction_id = current.next_transaction_id;
  candidate.descriptor_ring_index = current.head;
  candidate.payload_ring_indices.reserve(metadata_payload_count);
  for (std::size_t index = 0; index < metadata_payload_count; ++index) {
    candidate.payload_ring_indices.push_back(
        Advance(current.head, index + 1U, current.ring_blocks));
  }
  candidate.commit_ring_index =
      Advance(current.head, metadata_payload_count + 1U,
              current.ring_blocks);

  candidate.exposed_control = current;
  candidate.exposed_control.generation = current.generation + 1U;
  candidate.exposed_control.head =
      Advance(current.head, transaction_blocks, current.ring_blocks);
  candidate.exposed_control.used_blocks =
      static_cast<std::uint32_t>(transaction_blocks);
  candidate.exposed_control.next_transaction_id =
      current.next_transaction_id + 1U;
  candidate.exposed_control.checksum = 0;

  if (!EncodeControl(candidate.exposed_control, &encoded_control, nullptr,
                     &validation_error)) {
    if (detail != nullptr) {
      detail->assign("planned journal control is invalid: ");
      detail->append(validation_error);
    }
    return -EINVAL;
  }

  *output = std::move(candidate);
  if (detail != nullptr) {
    detail->clear();
  }
  return 0;
}

}  // namespace eufs::journal
