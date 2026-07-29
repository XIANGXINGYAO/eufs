#include "metadata/ondisk_format.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace {

using eufs::ondisk::Block;
using eufs::ondisk::DirectoryEntry;
using eufs::ondisk::DirectoryFileType;
using eufs::ondisk::InodeBytes;
using eufs::ondisk::InodeRecord;
using eufs::ondisk::Region;
using eufs::ondisk::Superblock;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

Superblock MakeSuperblock() {
  Superblock value;
  std::string error;
  Require(eufs::ondisk::BuildSuperblockLayout(16384, 1024, 256, &value,
                                              &error),
          error.c_str());
  value.feature_compat = 0x0102030405060708ULL;
  value.created_time_ns = 123456789ULL;
  for (std::size_t index = 0; index < value.filesystem_uuid.size(); ++index) {
    value.filesystem_uuid[index] = static_cast<std::uint8_t>(index);
  }
  return value;
}

void TestCrc32cKnownVector() {
  constexpr std::array<std::uint8_t, 9> input{
      '1', '2', '3', '4', '5', '6', '7', '8', '9'};
  Require(eufs::ondisk::Crc32c(input.data(), input.size()) == 0xE3069283U,
          "CRC32C standard check value changed");
}

void TestSuperblockRoundTripAndCorruption() {
  const Superblock original = MakeSuperblock();
  Block bytes{};
  std::string error;
  Require(eufs::ondisk::EncodeSuperblock(original, &bytes, &error),
          error.c_str());
  Require(bytes[0] == 'E' && bytes[7] == '1',
          "superblock magic is not encoded at byte zero");
  Require(bytes[16] == 0x00 && bytes[17] == 0x10 && bytes[18] == 0x00 &&
              bytes[19] == 0x00,
          "block size is not encoded as little-endian 4096");

  Superblock decoded;
  Require(eufs::ondisk::DecodeSuperblock(bytes, &decoded, &error),
          error.c_str());
  Require(decoded.total_blocks == original.total_blocks &&
              decoded.total_inodes == original.total_inodes &&
              decoded.data.start_block == original.data.start_block &&
              decoded.filesystem_uuid == original.filesystem_uuid,
          "superblock round trip changed fields");

  bytes[72] ^= 0x01U;
  Require(!eufs::ondisk::DecodeSuperblock(bytes, &decoded, &error),
          "corrupted superblock unexpectedly passed CRC validation");
}

void TestSuperblockRejectsInvalidLayout() {
  Superblock value = MakeSuperblock();
  value.inode_table.start_block += 1;
  Block bytes{};
  std::string error;
  Require(!eufs::ondisk::EncodeSuperblock(value, &bytes, &error),
          "overlapping or gapped metadata layout unexpectedly encoded");

  Superblock too_small_journal;
  Require(!eufs::ondisk::BuildSuperblockLayout(
              16384, 1024, eufs::ondisk::kMinimumJournalBlocks - 1,
              &too_small_journal, &error),
          "journal without two controls and a minimum transaction was accepted");
  Require(eufs::ondisk::BuildSuperblockLayout(
              16384, 1024, eufs::ondisk::kMinimumJournalBlocks,
              &too_small_journal, &error),
          error.c_str());
}

void TestInodeRoundTripAndCorruption() {
  InodeRecord original;
  original.inode_number = 5;
  original.mode = S_IFREG | 0644;
  original.uid = 1000;
  original.gid = 1000;
  original.link_count = 1;
  original.size = 8193;
  original.mtime_ns = 987654321ULL;
  original.generation = 7;
  original.direct_blocks[0] = 300;
  original.direct_blocks[1] = 301;
  original.indirect_block = 302;

  InodeBytes bytes{};
  std::string error;
  Require(eufs::ondisk::EncodeInode(original, &bytes, &error), error.c_str());
  Require(bytes[16] == 0x01 && bytes[17] == 0x20,
          "inode size is not encoded at the specified little-endian offset");

  InodeRecord decoded;
  Require(eufs::ondisk::DecodeInode(bytes, 5, &decoded, &error),
          error.c_str());
  Require(decoded.mode == original.mode && decoded.size == original.size &&
              decoded.inode_number == original.inode_number &&
              decoded.direct_blocks == original.direct_blocks &&
              decoded.indirect_block == original.indirect_block,
          "inode round trip changed fields");

  bytes[64] ^= 0x01U;
  Require(!eufs::ondisk::DecodeInode(bytes, 5, &decoded, &error),
          "corrupted inode unexpectedly passed CRC validation");

  Require(eufs::ondisk::EncodeInode(original, &bytes, &error), error.c_str());
  Require(!eufs::ondisk::DecodeInode(bytes, 6, &decoded, &error),
          "inode moved to a different identity unexpectedly passed CRC validation");
}

void TestDirectoryEntryRoundTripAndBounds() {
  DirectoryEntry original;
  original.inode = 5;
  original.file_type = DirectoryFileType::kRegular;
  original.name = "a.txt";

  std::array<std::uint8_t, 32> bytes{};
  std::string error;
  const auto minimum = eufs::ondisk::MinimumDirectoryRecordLength(
      original.name.size());
  Require(minimum == 16, "a.txt directory record minimum should be 16 bytes");
  Require(eufs::ondisk::EncodeDirectoryEntry(
              original, static_cast<std::uint16_t>(minimum), bytes.data(),
              bytes.size(), &error),
          error.c_str());

  DirectoryEntry decoded;
  Require(eufs::ondisk::DecodeDirectoryEntry(bytes.data(), minimum, &decoded,
                                             &error),
          error.c_str());
  Require(decoded.inode == original.inode && decoded.name == original.name &&
              decoded.file_type == original.file_type,
          "directory entry round trip changed fields");

  bytes[4] = 10;
  bytes[5] = 0;
  Require(!eufs::ondisk::DecodeDirectoryEntry(bytes.data(), minimum, &decoded,
                                              &error),
          "misaligned directory record unexpectedly decoded");

  Require(eufs::ondisk::EncodeDirectoryEntry(
              original, static_cast<std::uint16_t>(minimum), bytes.data(),
              bytes.size(), &error),
          error.c_str());
  bytes[15] = 1;
  Require(!eufs::ondisk::DecodeDirectoryEntry(bytes.data(), minimum, &decoded,
                                              &error),
          "non-zero directory padding unexpectedly decoded");
}

}  // namespace

int main() {
  TestCrc32cKnownVector();
  TestSuperblockRoundTripAndCorruption();
  TestSuperblockRejectsInvalidLayout();
  TestInodeRoundTripAndCorruption();
  TestDirectoryEntryRoundTripAndBounds();
  std::cout << "PASS: eufs v1 on-disk format tests\n";
  return 0;
}
