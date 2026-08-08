// 验证创建空文件计划同时更新父目录块、父 inode、新 inode 和 inode bitmap。
// planner 只生成 before/after 块镜像；本测试不把计划直接视为已持久化结果。
#include "metadata/empty_file_create_plan.h"
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"

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

eufs::ondisk::InodeBytes InodeBytesFromBlock(const eufs::ondisk::Block& block,
                                             std::uint32_t inode_number) {
  const std::size_t offset =
      static_cast<std::size_t>(inode_number - 1U) *
      eufs::ondisk::kInodeRecordSize;
  eufs::ondisk::InodeBytes bytes{};
  std::copy_n(block.begin() + offset, bytes.size(), bytes.begin());
  return bytes;
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-create-plan-XXXXXX");
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
  std::map<std::uint32_t, eufs::ondisk::Block> original_blocks;
  for (const std::uint32_t block : {1U, 2U, 3U, 291U}) {
    Require(reader->ReadBlock(block, &original_blocks[block], &detail) == 0,
            detail.c_str());
  }

  eufs::metadata::EmptyFileCreatePlan plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 123456789ULL, &plan,
              &detail) == 0,
          detail.c_str());
  Require(plan.inode_number == 2 && plan.directory_block == 291,
          "create plan selected the wrong inode or directory block");
  Require(plan.after_images.size() == 4 &&
              plan.after_images.count(1) == 1 &&
              plan.after_images.count(2) == 1 &&
              plan.after_images.count(3) == 1 &&
              plan.after_images.count(291) == 1,
          "create plan did not produce exactly blocks 1, 2, 3, and 291");
  Require(plan.before_images.size() == 4,
          "create plan did not preserve every changed home before-image");
  for (const auto& [block_number, original] : original_blocks) {
    Require(plan.before_images.count(block_number) == 1 &&
                plan.before_images.at(block_number) == original,
            "create plan before-images do not exactly cover changed homes");
  }
  Require(plan.after_images.at(1)[0] == 0x03,
          "inode bitmap after-image did not allocate inode 2");
  Require(plan.after_images.at(2)[36] == 0x0F,
          "block bitmap after-image did not allocate block 291");

  const auto root_bytes = InodeBytesFromBlock(plan.after_images.at(3), 1);
  const auto file_bytes = InodeBytesFromBlock(plan.after_images.at(3), 2);
  eufs::ondisk::InodeRecord root;
  eufs::ondisk::InodeRecord file;
  Require(eufs::ondisk::DecodeInode(root_bytes, 1, &root, &detail),
          detail.c_str());
  Require(eufs::ondisk::DecodeInode(file_bytes, 2, &file, &detail),
          detail.c_str());
  Require(root.size == eufs::ondisk::kBlockSize &&
              root.direct_blocks[0] == 291,
          "root inode was not updated in the inode-table after-image");
  Require(S_ISREG(file.mode) && (file.mode & 0777U) == 0644 &&
              file.size == 0 && file.link_count == 1,
          "new empty-file inode is incorrect");

  eufs::ondisk::DirectoryEntry entry;
  Require(eufs::ondisk::DecodeDirectoryEntry(
              plan.after_images.at(291).data(), eufs::ondisk::kBlockSize,
              &entry, &detail),
          detail.c_str());
  Require(entry.inode == 2 && entry.name == "a.txt" &&
              entry.record_length == eufs::ondisk::kBlockSize,
          "directory block after-image is incorrect");

  for (const auto& [block_number, original] : original_blocks) {
    eufs::ondisk::Block current{};
    Require(reader->ReadBlock(block_number, &current, &detail) == 0,
            detail.c_str());
    Require(current == original,
            "preparing after-images unexpectedly modified the source image");
  }

  eufs::metadata::EmptyFileCreatePlan invalid_plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "bad/name", 0644, 1000, 1000, 123456789ULL,
              &invalid_plan, &detail) == -EINVAL,
          "invalid filename unexpectedly produced a create plan");

  reader.reset();
  unlink(options.image_path.c_str());
  std::cout << "PASS: empty-file create after-image plan test\n";
  return 0;
}
