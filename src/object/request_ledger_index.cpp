#include "object/request_ledger_index.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <utility>

namespace eufs::object_store {
namespace {

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void SetSlotDetail(std::string* detail, std::uint64_t sequence,
                   std::string_view reason) {
  if (detail == nullptr) {
    return;
  }
  detail->assign("request ledger slot ");
  detail->append(std::to_string(sequence));
  detail->append(": ");
  detail->append(reason);
}

}  // namespace

const RequestLedgerRecord* RequestLedgerIndex::Find(
    const RequestId& request_id) const {
  const auto found = records_.find(request_id);
  return found == records_.end() ? nullptr : &found->second;
}

int RequestLedgerIndex::AppendCommitted(const RequestLedgerRecord& record,
                                        std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (full()) {
    SetDetail(detail, "request ledger index has no empty slots");
    return -ENOSPC;
  }
  if (record.sequence != next_sequence_) {
    SetDetail(detail, "committed ledger sequence does not match the index");
    return -EUCLEAN;
  }

  // 复用磁盘编码器验证 operation/result/version 的完整组合约束。
  RequestLedgerBytes encoded{};
  std::string encode_detail;
  if (!EncodeRequestLedgerRecord(record, &encoded, &encode_detail)) {
    SetDetail(detail, encode_detail);
    return -EUCLEAN;
  }
  try {
    if (!records_.emplace(record.request_id, record).second) {
      SetDetail(detail, "committed request id already exists in the index");
      return -EEXIST;
    }
  } catch (const std::bad_alloc&) {
    SetDetail(detail, "could not allocate the committed ledger index entry");
    return -ENOMEM;
  }
  ++next_sequence_;
  return 0;
}

int ScanRequestLedger(const storage::ImageReader& image,
                      RequestLedgerIndex* output, std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "request ledger index output is required");
    return -EINVAL;
  }

  const auto& superblock = image.superblock();
  if ((superblock.feature_incompat &
       ondisk::kFeatureIncompatRequestLedger) == 0) {
    SetDetail(detail, "image does not declare the request ledger feature");
    return -EOPNOTSUPP;
  }
  if ((superblock.feature_incompat &
       ~ondisk::kSupportedFeatureIncompat) != 0) {
    SetDetail(detail, "image declares unsupported incompatible features");
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
      inode.size % kRequestLedgerRecordSize != 0) {
    SetDetail(detail, "request ledger identity or inode geometry is invalid");
    return -EUCLEAN;
  }

  const std::uint64_t capacity = inode.size / kRequestLedgerRecordSize;
  if (capacity > std::numeric_limits<std::size_t>::max()) {
    SetDetail(detail, "request ledger capacity cannot fit the host index");
    return -EOVERFLOW;
  }
  constexpr std::size_t kEntriesPerBlock =
      ondisk::kBlockSize / kRequestLedgerRecordSize;
  const std::uint32_t block_count =
      static_cast<std::uint32_t>(inode.size / ondisk::kBlockSize);

  RequestLedgerIndex candidate;
  candidate.capacity_ = capacity;
  bool saw_empty = false;
  std::uint64_t sequence = 1;
  for (std::uint32_t logical = 0; logical < block_count; ++logical) {
    std::uint32_t physical = 0;
    result = image.MapLogicalBlock(inode, logical, &physical, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block block{};
    result = image.ReadBlock(physical, &block, detail);
    if (result != 0) {
      return result;
    }

    for (std::size_t local = 0; local < kEntriesPerBlock;
         ++local, ++sequence) {
      RequestLedgerBytes bytes{};
      std::copy_n(block.begin() + local * kRequestLedgerRecordSize,
                  bytes.size(), bytes.begin());
      RequestLedgerRecord record;
      std::string decode_detail;
      const auto status = DecodeRequestLedgerRecord(
          bytes, sequence, &record, &decode_detail);
      if (status == LedgerDecodeStatus::kCorrupt) {
        SetSlotDetail(detail, sequence, decode_detail);
        return -EUCLEAN;
      }
      if (status == LedgerDecodeStatus::kEmpty) {
        saw_empty = true;
        continue;
      }
      if (saw_empty) {
        SetSlotDetail(detail, sequence,
                      "record appears after the first empty slot");
        return -EUCLEAN;
      }
      if (!candidate.records_.emplace(record.request_id, record).second) {
        SetSlotDetail(detail, sequence, "duplicate request id");
        return -EUCLEAN;
      }
    }
  }

  candidate.next_sequence_ = candidate.records_.size() + 1U;
  *output = std::move(candidate);
  return 0;
}

}  // namespace eufs::object_store
