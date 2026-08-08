// 对 descriptor、payload、commit 和 A/B control 做编码/解码与校验和边界测试。
// 它只验证日志磁盘格式，不验证实际 fsync 顺序或 home replay。
#include "journal/ondisk_journal.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

void PutLe32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void RefreshRecordChecksum(eufs::ondisk::Block* block) {
  PutLe32(block->data() + 60, 0);
  PutLe32(block->data() + 60,
          eufs::ondisk::Crc32c(block->data(), block->size()));
}

void RefreshControlChecksum(eufs::ondisk::Block* block) {
  PutLe32(block->data() + 124, 0);
  PutLe32(block->data() + 124,
          eufs::ondisk::Crc32c(block->data(), block->size()));
}

}  // namespace

int main() {
  eufs::journal::JournalControl control_a;
  control_a.ring_blocks = 254;
  for (std::size_t index = 0; index < control_a.filesystem_uuid.size();
       ++index) {
    control_a.filesystem_uuid[index] =
        static_cast<std::uint8_t>(index + 1U);
  }
  control_a.generation = 10;
  control_a.next_transaction_id = 1;
  eufs::ondisk::Block control_a_block{};
  std::string error;
  Require(eufs::journal::EncodeControl(control_a, &control_a_block, nullptr,
                                       &error),
          error.c_str());

  auto control_b = control_a;
  control_b.generation = 11;
  control_b.head = 4;
  control_b.used_blocks = 4;
  control_b.next_transaction_id = 2;
  eufs::ondisk::Block control_b_block{};
  Require(eufs::journal::EncodeControl(control_b, &control_b_block, nullptr,
                                       &error),
          error.c_str());

  eufs::journal::JournalControl selected;
  eufs::journal::ControlCopy selected_copy{};
  Require(eufs::journal::SelectControl(
              control_a_block, control_b_block, control_a.filesystem_uuid, 254,
              &selected, &selected_copy, &error),
          error.c_str());
  Require(selected_copy == eufs::journal::ControlCopy::kB &&
              selected.generation == 11 && selected.used_blocks == 4,
          "control selection did not choose the newer valid copy");

  auto torn_b = control_b_block;
  torn_b[48] ^= 0x01;
  Require(eufs::journal::SelectControl(
              control_a_block, torn_b, control_a.filesystem_uuid, 254,
              &selected, &selected_copy, &error),
          error.c_str());
  Require(selected_copy == eufs::journal::ControlCopy::kA &&
              selected.generation == 10,
          "control selection trusted a newer copy with an invalid checksum");

  auto torn_a = control_a_block;
  torn_a[40] ^= 0x01;
  Require(!eufs::journal::SelectControl(
              torn_a, torn_b, control_a.filesystem_uuid, 254, &selected,
              &selected_copy, &error),
          "control selection accepted two invalid copies");

  auto equal_b = control_a_block;
  Require(eufs::journal::SelectControl(
              control_a_block, equal_b, control_a.filesystem_uuid, 254,
              &selected, &selected_copy, &error) &&
              selected_copy == eufs::journal::ControlCopy::kA,
          "identical equal-generation controls were not accepted");

  auto split_brain = control_a;
  split_brain.head = 3;
  split_brain.used_blocks = 3;
  split_brain.next_transaction_id = 2;
  eufs::ondisk::Block split_brain_block{};
  Require(eufs::journal::EncodeControl(split_brain, &split_brain_block,
                                       nullptr, &error),
          error.c_str());
  Require(!eufs::journal::SelectControl(
              control_a_block, split_brain_block, control_a.filesystem_uuid,
              254, &selected, &selected_copy, &error),
          "equal generations with different states were accepted");

  auto wrap_old = control_a;
  wrap_old.generation = std::numeric_limits<std::uint64_t>::max();
  eufs::ondisk::Block wrap_old_block{};
  Require(eufs::journal::EncodeControl(wrap_old, &wrap_old_block, nullptr,
                                       &error),
          error.c_str());
  auto wrap_new = control_a;
  wrap_new.generation = 0;
  eufs::ondisk::Block wrap_new_block{};
  Require(eufs::journal::EncodeControl(wrap_new, &wrap_new_block, nullptr,
                                       &error),
          error.c_str());
  Require(eufs::journal::SelectControl(
              wrap_old_block, wrap_new_block, control_a.filesystem_uuid, 254,
              &selected, &selected_copy, &error) &&
              selected_copy == eufs::journal::ControlCopy::kB,
          "generation wrap did not select zero after UINT64_MAX");

  auto ambiguous = control_a;
  ambiguous.generation = std::uint64_t{1} << 63U;
  eufs::ondisk::Block ambiguous_block{};
  Require(eufs::journal::EncodeControl(ambiguous, &ambiguous_block, nullptr,
                                       &error),
          error.c_str());
  auto generation_zero = control_a;
  generation_zero.generation = 0;
  eufs::ondisk::Block generation_zero_block{};
  Require(eufs::journal::EncodeControl(generation_zero,
                                       &generation_zero_block, nullptr, &error),
          error.c_str());
  Require(!eufs::journal::SelectControl(
              ambiguous_block, generation_zero_block,
              control_a.filesystem_uuid, 254, &selected, &selected_copy,
              &error),
          "half-range ambiguous generations were ordered");

  auto full_control = control_a;
  full_control.used_blocks = full_control.ring_blocks;
  eufs::ondisk::Block full_control_block{};
  Require(eufs::journal::EncodeControl(full_control, &full_control_block,
                                       nullptr, &error),
          "full ring state with head equal to tail was rejected");

  auto invalid_count = control_b_block;
  PutLe32(invalid_count.data() + 56, 5);
  RefreshControlChecksum(&invalid_count);
  eufs::journal::JournalControl decoded_control;
  Require(!eufs::journal::DecodeControl(invalid_count, &decoded_control,
                                       &error),
          "control accepted a head/tail/used mismatch with valid CRC");

  auto control_with_reserved_data = control_a_block;
  control_with_reserved_data[96] = 1;
  RefreshControlChecksum(&control_with_reserved_data);
  Require(!eufs::journal::DecodeControl(
              control_with_reserved_data, &decoded_control, &error),
          "control accepted nonzero reserved bytes with valid CRC");

  eufs::ondisk::Block bitmap_after{};
  bitmap_after[36] = 0x1F;
  eufs::ondisk::Block inode_after{};
  inode_after[128 + 16] = 5;

  eufs::journal::DescriptorRecord descriptor;
  descriptor.transaction_id = 1;
  for (std::size_t index = 0; index < descriptor.filesystem_uuid.size();
       ++index) {
    descriptor.filesystem_uuid[index] = static_cast<std::uint8_t>(index + 1U);
  }
  descriptor.transaction_block_count = 4;
  descriptor.entries = {
      {2, 1, eufs::ondisk::Crc32c(bitmap_after.data(), bitmap_after.size()), 0},
      {3, 2, eufs::ondisk::Crc32c(inode_after.data(), inode_after.size()), 0},
  };

  eufs::ondisk::Block descriptor_block{};
  std::uint32_t descriptor_checksum = 0;
  Require(eufs::journal::EncodeDescriptor(
              descriptor, &descriptor_block, &descriptor_checksum, &error),
          error.c_str());
  Require(GetLe32(descriptor_block.data() + 8) == 1 &&
              GetLe32(descriptor_block.data() + 20) == 2 &&
              GetLe32(descriptor_block.data() + 64) == 2 &&
              GetLe32(descriptor_block.data() + 68) == 1,
          "descriptor fixed offsets are not little-endian v1 values");

  eufs::journal::DescriptorRecord decoded_descriptor;
  Require(eufs::journal::DecodeDescriptor(descriptor_block,
                                          &decoded_descriptor, &error),
          error.c_str());
  Require(decoded_descriptor.transaction_id == 1 &&
              decoded_descriptor.entries.size() == 2 &&
              decoded_descriptor.entries[0].home_block == 2 &&
              decoded_descriptor.entries[1].home_block == 3 &&
              decoded_descriptor.checksum == descriptor_checksum,
          "descriptor round trip changed transaction fields");

  eufs::journal::CommitRecord commit;
  commit.transaction_id = 1;
  commit.filesystem_uuid = descriptor.filesystem_uuid;
  commit.entry_count = 2;
  commit.transaction_block_count = 4;
  commit.descriptor_ring_index = 0;
  commit.descriptor_crc32c = descriptor_checksum;
  eufs::ondisk::Block commit_block{};
  Require(eufs::journal::EncodeCommit(commit, &commit_block, &error),
          error.c_str());

  eufs::journal::CommitRecord decoded_commit;
  Require(eufs::journal::DecodeCommit(commit_block, &decoded_commit, &error),
          error.c_str());
  Require(eufs::journal::CommitMatchesDescriptor(
              decoded_descriptor, 0, decoded_commit, &error),
          error.c_str());

  auto corrupted_descriptor = descriptor_block;
  corrupted_descriptor[72] ^= 0x01;
  Require(!eufs::journal::DecodeDescriptor(
              corrupted_descriptor, &decoded_descriptor, &error),
          "descriptor bit flip was not rejected");

  auto descriptor_with_reserved_data = descriptor_block;
  descriptor_with_reserved_data[96] = 1;
  RefreshRecordChecksum(&descriptor_with_reserved_data);
  Require(!eufs::journal::DecodeDescriptor(
              descriptor_with_reserved_data, &decoded_descriptor, &error),
          "descriptor accepted nonzero reserved bytes with a valid checksum");

  auto corrupted_commit = commit_block;
  corrupted_commit[24] ^= 0x01;
  Require(!eufs::journal::DecodeCommit(corrupted_commit, &decoded_commit,
                                      &error),
          "commit bit flip was not rejected");

  auto commit_with_reserved_data = commit_block;
  commit_with_reserved_data[64] = 1;
  RefreshRecordChecksum(&commit_with_reserved_data);
  Require(!eufs::journal::DecodeCommit(commit_with_reserved_data,
                                      &decoded_commit, &error),
          "commit accepted nonzero reserved bytes with a valid checksum");

  Require(eufs::journal::DecodeCommit(commit_block, &decoded_commit, &error),
          error.c_str());
  decoded_commit.descriptor_crc32c ^= 0x01;
  Require(!eufs::journal::CommitMatchesDescriptor(
              decoded_descriptor, 0, decoded_commit, &error),
          "commit accepted the wrong descriptor checksum");

  auto duplicate_home = descriptor;
  duplicate_home.entries[1].home_block = 2;
  Require(!eufs::journal::EncodeDescriptor(
              duplicate_home, &descriptor_block, nullptr, &error),
          "descriptor accepted duplicate home blocks");

  std::cout << "control_newer=B torn_fallback=A wrap=max_to_zero "
               "full_used=254\n";
  std::cout << "descriptor_txid=1 entries=2 blocks=4 crc="
            << descriptor_checksum << '\n';
  std::cout << "PASS: journal descriptor and commit format test\n";
  return 0;
}
