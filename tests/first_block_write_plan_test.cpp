// 验证早期单块写测试计划只允许空 inode 的第一次分配，并生成完整块镜像集合。
// 该代码已隔离在 tests/support，保留它是为了复现历史恢复边界。
#include "metadata/empty_file_create_plan.h"
#include "tests/support/first_block_write_plan.h"
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "tests/support/writable_image.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

eufs::ondisk::InodeBytes InodeBytesFromTableBlock(
    const eufs::ondisk::Block& block, std::uint32_t inode_number) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(inode_number - 1U) *
      eufs::ondisk::kInodeRecordSize;
  const std::size_t offset =
      static_cast<std::size_t>(byte_index % eufs::ondisk::kBlockSize);
  eufs::ondisk::InodeBytes bytes{};
  std::copy_n(block.begin() + offset, bytes.size(), bytes.begin());
  return bytes;
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-first-write-plan-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 64ULL * 1024ULL * 1024ULL;
  options.total_inodes = 1024;
  options.journal_blocks = 256;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::EmptyFileCreatePlan create_plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 100ULL, &create_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(options.image_path, create_plan,
                                         &detail) == 0,
          detail.c_str());

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  std::map<std::uint32_t, eufs::ondisk::Block> original_blocks;
  for (const std::uint32_t block : {1U, 2U, 3U, 291U, 292U}) {
    Require(reader->ReadBlock(block, &original_blocks[block], &detail) == 0,
            detail.c_str());
  }

  eufs::metadata::FirstBlockWritePlan plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, 2, "hello", 200ULL, &plan, &detail) == 0,
          detail.c_str());
  Require(plan.inode_number == 2 && plan.data_block == 292,
          "first write selected the wrong inode or data block");
  Require(plan.ordered_data_after_images.size() == 1 &&
              plan.ordered_data_after_images.count(292) == 1 &&
              plan.metadata_after_images.size() == 2 &&
              plan.metadata_after_images.count(2) == 1 &&
              plan.metadata_after_images.count(3) == 1,
          "first write did not classify block 292 as data and 2/3 as metadata");
  Require(plan.metadata_after_images.at(2)[36] == 0x1F,
          "block bitmap after-image did not allocate block 292");

  const auto& data_after = plan.ordered_data_after_images.at(292);
  Require(std::equal(data_after.begin(), data_after.begin() + 5,
                     reinterpret_cast<const std::uint8_t*>("hello")),
          "data block does not start with hello");
  Require(std::all_of(data_after.begin() + 5, data_after.end(),
                      [](std::uint8_t value) { return value == 0; }),
          "new data block tail was not zero-filled");

  const auto inode_bytes =
      InodeBytesFromTableBlock(plan.metadata_after_images.at(3), 2);
  eufs::ondisk::InodeRecord inode;
  Require(eufs::ondisk::DecodeInode(inode_bytes, 2, &inode, &detail),
          detail.c_str());
  Require(inode.size == 5 && inode.direct_blocks[0] == 292 &&
              inode.mtime_ns == 200 && inode.ctime_ns == 200,
          "inode 2 after-image does not describe the hello data");
  Require(std::equal(plan.metadata_after_images.at(3).begin(),
                     plan.metadata_after_images.at(3).begin() +
                         eufs::ondisk::kInodeRecordSize,
                     original_blocks.at(3).begin()),
          "updating inode 2 unexpectedly changed root inode 1");

  for (const auto& [block_number, original] : original_blocks) {
    eufs::ondisk::Block current{};
    Require(reader->ReadBlock(block_number, &current, &detail) == 0,
            detail.c_str());
    Require(current == original,
            "preparing first-write after-images modified the source image");
  }
  Require(plan.metadata_after_images.count(1) == 0 &&
              plan.metadata_after_images.count(291) == 0 &&
              plan.ordered_data_after_images.count(1) == 0 &&
              plan.ordered_data_after_images.count(291) == 0,
          "first write unexpectedly modified inode bitmap or directory data");

  eufs::metadata::FirstBlockWritePlan invalid_plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, 2, "", 200ULL, &invalid_plan, &detail) == -EINVAL,
          "empty input unexpectedly produced a first-write plan");
  const std::string oversized(eufs::ondisk::kBlockSize + 1U, 'x');
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, 2, oversized, 200ULL, &invalid_plan, &detail) == -EFBIG,
          "oversized input unexpectedly produced a first-write plan");

  reader.reset();
  unlink(options.image_path.c_str());
  std::cout << "PASS: first data-block write after-image plan test\n";
  return 0;
}
