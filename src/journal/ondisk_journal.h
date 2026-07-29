#pragma once

#include "metadata/ondisk_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eufs::journal {

constexpr std::uint32_t kJournalFormatVersion = 1;
constexpr std::size_t kJournalControlHeaderSize = 128;
constexpr std::size_t kJournalRecordHeaderSize = 64;
constexpr std::size_t kDescriptorEntrySize = 16;
constexpr std::size_t kMaxDescriptorEntries =
    (ondisk::kBlockSize - kJournalRecordHeaderSize) / kDescriptorEntrySize;

enum class RecordType : std::uint32_t {
  kDescriptor = 1,
  kCommit = 2,
};

enum class ControlCopy {
  kA,
  kB,
};

enum class GenerationComparison {
  kEqual,
  kFirstNewer,
  kSecondNewer,
  kAmbiguous,
};

struct JournalControl {
  std::uint32_t ring_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::uint64_t generation{0};
  std::uint32_t head{0};
  std::uint32_t tail{0};
  std::uint32_t used_blocks{0};
  std::uint32_t state_flags{0};
  std::uint64_t next_transaction_id{0};
  std::uint64_t feature_compat{0};
  std::uint64_t feature_ro_compat{0};
  std::uint64_t feature_incompat{0};
  std::uint32_t checksum{0};
};

struct DescriptorEntry {
  std::uint32_t home_block{0};
  std::uint32_t payload_ring_index{0};
  std::uint32_t payload_crc32c{0};
  std::uint32_t flags{0};
};

struct DescriptorRecord {
  std::uint64_t transaction_id{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::uint32_t transaction_block_count{0};
  std::uint32_t flags{0};
  std::uint32_t checksum{0};
  std::vector<DescriptorEntry> entries;
};

struct CommitRecord {
  std::uint64_t transaction_id{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::uint32_t entry_count{0};
  std::uint32_t transaction_block_count{0};
  std::uint32_t descriptor_ring_index{0};
  std::uint32_t descriptor_crc32c{0};
  std::uint32_t checksum{0};
};

bool EncodeControl(const JournalControl& value, ondisk::Block* output,
                   std::uint32_t* checksum, std::string* error);
bool DecodeControl(const ondisk::Block& input, JournalControl* output,
                   std::string* error);

GenerationComparison CompareGenerations(std::uint64_t first,
                                        std::uint64_t second);
bool SelectControl(const ondisk::Block& control_a,
                   const ondisk::Block& control_b,
                   const std::array<std::uint8_t, 16>& expected_uuid,
                   std::uint32_t expected_ring_blocks,
                   JournalControl* output, ControlCopy* selected_copy,
                   std::string* error);

bool EncodeDescriptor(const DescriptorRecord& value, ondisk::Block* output,
                      std::uint32_t* checksum, std::string* error);
bool DecodeDescriptor(const ondisk::Block& input, DescriptorRecord* output,
                      std::string* error);

bool EncodeCommit(const CommitRecord& value, ondisk::Block* output,
                  std::string* error);
bool DecodeCommit(const ondisk::Block& input, CommitRecord* output,
                  std::string* error);

bool CommitMatchesDescriptor(const DescriptorRecord& descriptor,
                             std::uint32_t descriptor_ring_index,
                             const CommitRecord& commit,
                             std::string* error);

}  // namespace eufs::journal
