#include "object/request_ledger_plan.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace eufs::object_store {
namespace {

constexpr std::size_t kEntriesPerBlock =
    ondisk::kBlockSize / kRequestLedgerRecordSize;

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

bool IsZeroSlot(const ondisk::Block& block, std::size_t offset) {
  return std::all_of(block.begin() + offset,
                     block.begin() + offset + kRequestLedgerRecordSize,
                     [](std::uint8_t value) { return value == 0; });
}

// Merge 也校验传入计划，避免手工构造或未来 planner 回归造成静默覆盖。
int ValidateAppendPlan(const RequestLedgerAppendPlan& plan,
                       std::string* detail) {
  if (plan.sequence == 0 || plan.record.sequence != plan.sequence ||
      plan.total_blocks == 0 || plan.physical_block >= plan.total_blocks) {
    SetDetail(detail, "request ledger append plan identity is invalid");
    return -EINVAL;
  }

  const std::uint64_t slot_index = plan.sequence - 1U;
  const std::uint64_t expected_logical = slot_index / kEntriesPerBlock;
  if (expected_logical > std::numeric_limits<std::uint32_t>::max() ||
      plan.logical_block != static_cast<std::uint32_t>(expected_logical)) {
    SetDetail(detail, "request ledger append plan targets the wrong block");
    return -EINVAL;
  }
  const std::size_t inside = static_cast<std::size_t>(
      (slot_index % kEntriesPerBlock) * kRequestLedgerRecordSize);
  if (!IsZeroSlot(plan.before_image, inside)) {
    SetDetail(detail, "request ledger append before-image slot is not empty");
    return -EINVAL;
  }

  RequestLedgerBytes encoded{};
  std::string encode_detail;
  if (!EncodeRequestLedgerRecord(plan.record, &encoded, &encode_detail)) {
    SetDetail(detail, encode_detail);
    return -EINVAL;
  }
  ondisk::Block expected_after = plan.before_image;
  std::copy(encoded.begin(), encoded.end(), expected_after.begin() + inside);
  if (expected_after != plan.after_image) {
    SetDetail(detail,
              "request ledger append after-image changes bytes outside its slot");
    return -EINVAL;
  }
  return 0;
}

}  // namespace

int PrepareRequestLedgerAppend(const storage::ImageReader& image,
                               const RequestLedgerIndex& index,
                               RequestLedgerRecord record_without_sequence,
                               RequestLedgerAppendPlan* output,
                               std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "request ledger append output is required");
    return -EINVAL;
  }
  if (index.full()) {
    SetDetail(detail, "request ledger has no empty slots");
    return -ENOSPC;
  }

  const auto& superblock = image.superblock();
  if ((superblock.feature_incompat &
       ondisk::kFeatureIncompatRequestLedger) == 0) {
    SetDetail(detail, "image does not declare the request ledger feature");
    return -EOPNOTSUPP;
  }

  const std::string ledger_path =
      std::string("/") + std::string(kRequestLedgerName);
  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  int result =
      image.ResolvePath(ledger_path, &inode_number, &inode, detail);
  if (result != 0) {
    if (result == -ENOENT) {
      SetDetail(detail, "required request ledger path is missing");
      return -EUCLEAN;
    }
    return result;
  }
  if (inode_number != kRequestLedgerInodeNumber || !S_ISREG(inode.mode) ||
      inode.link_count != 1 || inode.size == 0 ||
      inode.size % ondisk::kBlockSize != 0 ||
      inode.size % kRequestLedgerRecordSize != 0 ||
      inode.size / kRequestLedgerRecordSize != index.capacity()) {
    SetDetail(detail,
              "request ledger inode geometry disagrees with its startup index");
    return -EUCLEAN;
  }

  const std::uint64_t sequence = index.next_sequence();
  if (sequence == 0 || sequence > index.capacity()) {
    SetDetail(detail, "request ledger next sequence is outside its capacity");
    return -EUCLEAN;
  }
  const std::uint64_t slot_index = sequence - 1U;
  const std::uint64_t logical = slot_index / kEntriesPerBlock;
  if (logical > std::numeric_limits<std::uint32_t>::max()) {
    SetDetail(detail, "request ledger logical block cannot be represented");
    return -EOVERFLOW;
  }

  RequestLedgerAppendPlan candidate;
  candidate.sequence = sequence;
  candidate.logical_block = static_cast<std::uint32_t>(logical);
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;
  result = image.MapLogicalBlock(inode, candidate.logical_block,
                                 &candidate.physical_block, detail);
  if (result != 0) {
    return result;
  }
  result = image.ReadBlock(candidate.physical_block, &candidate.before_image,
                           detail);
  if (result != 0) {
    return result;
  }

  const std::size_t inside = static_cast<std::size_t>(
      (slot_index % kEntriesPerBlock) * kRequestLedgerRecordSize);
  if (!IsZeroSlot(candidate.before_image, inside)) {
    SetDetail(detail,
              "request ledger next slot is occupied despite the startup index");
    return -EUCLEAN;
  }

  record_without_sequence.sequence = sequence;
  RequestLedgerBytes encoded{};
  std::string encode_detail;
  if (!EncodeRequestLedgerRecord(record_without_sequence, &encoded,
                                 &encode_detail)) {
    SetDetail(detail, encode_detail);
    return -EINVAL;
  }
  candidate.record = record_without_sequence;
  candidate.after_image = candidate.before_image;
  std::copy(encoded.begin(), encoded.end(),
            candidate.after_image.begin() + inside);
  *output = std::move(candidate);
  return 0;
}

int MergeRequestLedgerAppend(
    const RequestLedgerAppendPlan& ledger, std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    std::map<std::uint32_t, ondisk::Block>* before_images,
    std::map<std::uint32_t, ondisk::Block>* ordered_data_after_images,
    std::map<std::uint32_t, ondisk::Block>* metadata_after_images,
    std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (before_images == nullptr || ordered_data_after_images == nullptr ||
      metadata_after_images == nullptr ||
      before_images == ordered_data_after_images ||
      before_images == metadata_after_images ||
      ordered_data_after_images == metadata_after_images) {
    SetDetail(detail, "three distinct transaction image maps are required");
    return -EINVAL;
  }
  int result = ValidateAppendPlan(ledger, detail);
  if (result != 0) {
    return result;
  }
  if (ledger.total_blocks != total_blocks ||
      ledger.filesystem_uuid != filesystem_uuid) {
    SetDetail(detail,
              "object and request ledger plans come from different images");
    return -EINVAL;
  }

  const std::uint32_t block = ledger.physical_block;
  if (before_images->count(block) != 0 ||
      ordered_data_after_images->count(block) != 0 ||
      metadata_after_images->count(block) != 0) {
    SetDetail(detail,
              "request ledger block conflicts with an existing object plan");
    return -EINVAL;
  }

  auto candidate_before = *before_images;
  auto candidate_ordered = *ordered_data_after_images;
  auto candidate_metadata = *metadata_after_images;
  const bool inserted_before =
      candidate_before.emplace(block, ledger.before_image).second;
  const bool inserted_metadata =
      candidate_metadata.emplace(block, ledger.after_image).second;
  if (!inserted_before || !inserted_metadata) {
    SetDetail(detail, "request ledger block could not be merged uniquely");
    return -EINVAL;
  }

  *before_images = std::move(candidate_before);
  *ordered_data_after_images = std::move(candidate_ordered);
  *metadata_after_images = std::move(candidate_metadata);
  return 0;
}

}  // namespace eufs::object_store
