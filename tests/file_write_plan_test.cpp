#include "metadata/empty_file_create_plan.h"
#include "metadata/file_write_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "storage/writable_image.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string TemporaryPath() {
  std::array<char, 64> path{};
  std::strcpy(path.data(), "/tmp/eufs-file-write-plan-XXXXXX");
  const int fd = mkstemp(path.data());
  Require(fd >= 0, "mkstemp failed");
  close(fd);
  unlink(path.data());
  return path.data();
}

std::string Pattern(std::size_t size, char base) {
  std::string output(size, '\0');
  for (std::size_t index = 0; index < size; ++index) {
    output[index] = static_cast<char>(base + index % 17U);
  }
  return output;
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  return static_cast<std::uint32_t>(input[0]) |
         (static_cast<std::uint32_t>(input[1]) << 8U) |
         (static_cast<std::uint32_t>(input[2]) << 16U) |
         (static_cast<std::uint32_t>(input[3]) << 24U);
}

void CreateEmptyFile(const std::string& path, std::uint64_t image_size) {
  eufs::storage::MkfsOptions options;
  options.image_path = path;
  options.image_size_bytes = image_size;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  eufs::metadata::EmptyFileCreatePlan create;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 10, &create, &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(path, create, &detail) == 0,
          detail.c_str());
}

std::unique_ptr<eufs::storage::ImageReader> OpenReader(
    const std::string& path) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  return reader;
}

eufs::ondisk::InodeRecord ReadFileInode(
    const eufs::storage::ImageReader& reader) {
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(reader.ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0 &&
              inode_number == 2,
          detail.c_str());
  return inode;
}

eufs::ondisk::InodeRecord PlannedInode(
    const eufs::storage::ImageReader& reader,
    const eufs::metadata::FileWritePlan& plan) {
  const auto& superblock = reader.superblock();
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(plan.inode_number - 1U) *
      eufs::ondisk::kInodeRecordSize;
  const std::uint32_t home =
      superblock.inode_table.start_block +
      static_cast<std::uint32_t>(byte_index / eufs::ondisk::kBlockSize);
  const std::size_t offset =
      static_cast<std::size_t>(byte_index % eufs::ondisk::kBlockSize);
  const auto after = plan.metadata_after_images.find(home);
  Require(after != plan.metadata_after_images.end(),
          "plan does not contain the inode-table after-image");
  eufs::ondisk::InodeBytes bytes{};
  std::copy_n(after->second.begin() + offset, bytes.size(), bytes.begin());
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(eufs::ondisk::DecodeInode(bytes, plan.inode_number, &inode, &detail),
          detail.c_str());
  return inode;
}

bool PlannedBitmapBit(const eufs::storage::ImageReader& reader,
                      const eufs::metadata::FileWritePlan& plan,
                      std::uint32_t bit) {
  const auto& bitmap = reader.superblock().block_bitmap;
  const std::uint32_t local_block =
      bit / (eufs::ondisk::kBlockSize * 8U);
  const std::uint32_t home = bitmap.start_block + local_block;
  const auto after = plan.metadata_after_images.find(home);
  Require(after != plan.metadata_after_images.end(),
          "plan does not contain the changed bitmap block");
  const std::uint32_t local_bit =
      bit % (eufs::ondisk::kBlockSize * 8U);
  return (after->second[local_bit / 8U] &
          static_cast<std::uint8_t>(1U << (local_bit % 8U))) != 0;
}

std::string ReadAll(const eufs::storage::ImageReader& reader) {
  const auto inode = ReadFileInode(reader);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(inode.size));
  std::size_t read = 0;
  std::string detail;
  Require(reader.ReadFile(2, 0, bytes.data(), bytes.size(), &read, &detail) ==
                  0 &&
              read == bytes.size(),
          detail.c_str());
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

eufs::ondisk::Block ExpectedBlock(std::string_view contents,
                                  std::uint32_t logical) {
  eufs::ondisk::Block block{};
  const std::size_t begin =
      static_cast<std::size_t>(logical) * eufs::ondisk::kBlockSize;
  if (begin >= contents.size()) {
    return block;
  }
  const std::size_t count =
      std::min<std::size_t>(eufs::ondisk::kBlockSize, contents.size() - begin);
  std::copy_n(contents.begin() + begin, count, block.begin());
  return block;
}

void TestOverwriteAndIndirectCow() {
  const std::string path = TemporaryPath();
  CreateEmptyFile(path, 8ULL * 1024ULL * 1024ULL);
  std::string detail;

  auto reader = OpenReader(path);
  const std::string initial = Pattern(6000, 'A');
  eufs::metadata::FileWritePlan initial_plan;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 0, initial, 20, &initial_plan, &detail) == 0 &&
              initial_plan.ordered_data_after_images.size() == 2,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyFileWritePlan(path, initial_plan, &detail) == 0,
          detail.c_str());

  reader = OpenReader(path);
  const auto old_inode = ReadFileInode(*reader);
  Require(ReadAll(*reader) == initial, "initial multi-block write was incorrect");
  const std::set<std::uint32_t> old_blocks = {old_inode.direct_blocks[0],
                                               old_inode.direct_blocks[1]};

  const std::string payload = Pattern(5000, 'k');
  std::string expected = initial;
  expected.resize(8500, '\0');
  std::copy(payload.begin(), payload.end(), expected.begin() + 3500);
  eufs::metadata::FileWritePlan overwrite;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 3500, payload, 30, &overwrite, &detail) == 0,
          detail.c_str());
  Require(overwrite.old_size == 6000 && overwrite.new_size == 8500 &&
              overwrite.ordered_data_after_images.size() == 3,
          "overwrite plan has the wrong size or COW block count");
  const auto overwrite_inode = PlannedInode(*reader, overwrite);
  std::set<std::uint32_t> new_blocks;
  for (std::uint32_t logical = 0; logical < 3; ++logical) {
    const std::uint32_t physical = overwrite_inode.direct_blocks[logical];
    new_blocks.insert(physical);
    Require(old_blocks.count(physical) == 0,
            "COW plan reused an old data block before COMMIT");
    Require(overwrite.ordered_data_after_images.at(physical) ==
                ExpectedBlock(expected, logical),
            "COW data after-image is incorrect");
    Require(PlannedBitmapBit(*reader, overwrite, physical),
            "new COW block is free in the bitmap after-image");
  }
  Require(new_blocks.size() == 3,
          "COW plan assigned one physical block more than once");
  for (const std::uint32_t old_block : old_blocks) {
    Require(!PlannedBitmapBit(*reader, overwrite, old_block),
            "old COW block remains allocated in the bitmap after-image");
  }
  Require(ReadAll(*reader) == initial,
          "planning an overwrite modified the source image");
  reader.reset();
  Require(eufs::storage::ApplyFileWritePlan(path, overwrite, &detail) == 0,
          detail.c_str());
  reader = OpenReader(path);
  Require(ReadAll(*reader) == expected,
          "applied partial/full/tail COW overwrite is incorrect");

  const std::string thirteen_blocks(
      13U * eufs::ondisk::kBlockSize, 'I');
  eufs::metadata::FileWritePlan crossing;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 0, thirteen_blocks, 40, &crossing, &detail) == 0,
          detail.c_str());
  const auto crossing_inode = PlannedInode(*reader, crossing);
  Require(crossing_inode.indirect_block != 0 &&
              crossing.metadata_after_images.count(
                  crossing_inode.indirect_block) == 1 &&
              crossing.ordered_data_after_images.count(
                  crossing_inode.indirect_block) == 0,
          "direct/indirect crossing did not COW a metadata index block");
  const auto& indirect =
      crossing.metadata_after_images.at(crossing_inode.indirect_block);
  const std::uint32_t logical_twelve = GetLe32(indirect.data());
  Require(crossing.ordered_data_after_images.count(logical_twelve) == 1,
          "new indirect entry does not reference the thirteenth data block");
  reader.reset();
  Require(eufs::storage::ApplyFileWritePlan(path, crossing, &detail) == 0,
          detail.c_str());
  reader = OpenReader(path);
  Require(ReadAll(*reader) == thirteen_blocks,
          "applied direct/indirect crossing write is incorrect");

  const auto indirect_inode = ReadFileInode(*reader);
  eufs::metadata::FileWritePlan direct_only;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 0, "D", 50, &direct_only, &detail) == 0,
          detail.c_str());
  const auto direct_only_inode = PlannedInode(*reader, direct_only);
  Require(direct_only_inode.indirect_block == indirect_inode.indirect_block &&
              direct_only.metadata_after_images.count(
                  indirect_inode.indirect_block) == 0,
          "direct-only overwrite unnecessarily replaced indirect metadata");

  eufs::metadata::FileWritePlan indirect_update;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 12ULL * eufs::ondisk::kBlockSize, "Q", 60,
              &indirect_update, &detail) == 0,
          detail.c_str());
  const auto indirect_update_inode = PlannedInode(*reader, indirect_update);
  Require(indirect_update_inode.indirect_block !=
                  indirect_inode.indirect_block &&
              indirect_update.metadata_after_images.count(
                  indirect_update_inode.indirect_block) == 1 &&
              !PlannedBitmapBit(*reader, indirect_update,
                                indirect_inode.indirect_block),
          "indirect overwrite did not COW and release the old index block");

  reader.reset();
  unlink(path.c_str());
}

void TestGapZeroFill() {
  const std::string path = TemporaryPath();
  CreateEmptyFile(path, 8ULL * 1024ULL * 1024ULL);
  auto reader = OpenReader(path);
  std::string detail;
  eufs::metadata::FileWritePlan gap;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 9000, "0123456789", 20, &gap, &detail) == 0 &&
              gap.ordered_data_after_images.size() == 3,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyFileWritePlan(path, gap, &detail) == 0,
          detail.c_str());
  reader = OpenReader(path);
  const std::string contents = ReadAll(*reader);
  Require(contents.size() == 9010 &&
              std::all_of(contents.begin(), contents.begin() + 9000,
                          [](char value) { return value == '\0'; }) &&
              contents.substr(9000) == "0123456789",
          "write beyond EOF did not expose a zero-filled gap");
  reader.reset();
  unlink(path.c_str());
}

void TestEnospcLeavesNoPlan() {
  const std::string path = TemporaryPath();
  CreateEmptyFile(path, 1ULL * 1024ULL * 1024ULL);
  auto reader = OpenReader(path);
  const std::string oversized(eufs::ondisk::kMaxFileSize, 'x');
  eufs::metadata::FileWritePlan output;
  output.inode_number = 99;
  std::string detail;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, 2, 0, oversized, 20, &output, &detail) == -ENOSPC &&
              output.inode_number == 99,
          "ENOSPC returned a partial file-write plan");
  Require(ReadFileInode(*reader).size == 0,
          "ENOSPC planning changed the source inode");
  reader.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestOverwriteAndIndirectCow();
  TestGapZeroFill();
  TestEnospcLeavesNoPlan();
  std::cout << "PASS: general COW file-write planner tests\n";
  return 0;
}
