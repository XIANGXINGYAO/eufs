// 验证完整对象替换的版本条件、缩短/清空、direct/indirect 转换和失败零副作用。
#include "checker/consistency_checker.h"
#include "journal/journal_control_store.h"
#include "metadata/file_write_plan.h"
#include "metadata/new_object_plan.h"
#include "metadata/object_replace_plan.h"
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
#include <limits>
#include <memory>
#include <string>
#include <sys/stat.h>
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
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-object-replace-XXXXXX");
  const int fd = mkstemp(path_template.data());
  Require(fd >= 0, "mkstemp failed");
  close(fd);
  unlink(path_template.data());
  return path_template.data();
}

std::string CreateImage(std::uint64_t bytes = 8ULL * 1024ULL * 1024ULL) {
  eufs::storage::MkfsOptions options;
  options.image_path = TemporaryPath();
  options.image_size_bytes = bytes;
  options.total_inodes = 256;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());
  return options.image_path;
}

std::unique_ptr<eufs::storage::ImageReader> OpenReader(
    const std::string& path) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  return reader;
}

template <typename Plan>
void ApplyPlan(const std::string& path, const Plan& plan) {
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteOrderedDataAndUnexposedBody(
              plan.total_blocks, plan.filesystem_uuid, plan.before_images,
              plan.ordered_data_after_images, plan.metadata_after_images,
              &body, &detail) == 0,
          detail.c_str());
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  Require(store->WriteCommit(&detail) == 0, detail.c_str());
  Require(store->CompleteCommittedTransaction(&detail) == 0,
          detail.c_str());
}

std::string Pattern(std::size_t size, char base) {
  std::string output(size, '\0');
  for (std::size_t index = 0; index < output.size(); ++index) {
    output[index] = static_cast<char>(base + index % 17U);
  }
  return output;
}

std::uint32_t SeedObject(const std::string& path, std::string_view data,
                         std::uint64_t generation = 1) {
  auto reader = OpenReader(path);
  eufs::metadata::NewObjectPlan plan;
  std::string detail;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "a", data, 0640, 1000, 1001, 10, &plan, &detail) == 0,
          detail.c_str());
  if (generation != 1) {
    const auto& superblock = reader->superblock();
    const std::uint64_t byte_index =
        static_cast<std::uint64_t>(plan.inode_number - 1U) *
        eufs::ondisk::kInodeRecordSize;
    const std::uint32_t table_block =
        superblock.inode_table.start_block +
        static_cast<std::uint32_t>(byte_index / eufs::ondisk::kBlockSize);
    const std::size_t offset =
        static_cast<std::size_t>(byte_index % eufs::ondisk::kBlockSize);
    eufs::ondisk::InodeBytes bytes{};
    std::copy_n(plan.metadata_after_images.at(table_block).begin() + offset,
                bytes.size(), bytes.begin());
    eufs::ondisk::InodeRecord inode;
    Require(eufs::ondisk::DecodeInode(bytes, plan.inode_number, &inode,
                                      &detail),
            detail.c_str());
    inode.generation = generation;
    Require(eufs::ondisk::EncodeInode(inode, &bytes, &detail), detail.c_str());
    std::copy(bytes.begin(), bytes.end(),
              plan.metadata_after_images.at(table_block).begin() + offset);
  }
  reader.reset();
  ApplyPlan(path, plan);
  return plan.inode_number;
}

std::string ReadObject(const eufs::storage::ImageReader& reader,
                       std::uint32_t inode_number) {
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(reader.ReadInode(inode_number, &inode, &detail) == 0,
          detail.c_str());
  std::string output(static_cast<std::size_t>(inode.size), '\0');
  std::size_t bytes_read = 0;
  Require(reader.ReadFile(
              inode_number, 0,
              reinterpret_cast<std::uint8_t*>(output.data()), output.size(),
              &bytes_read, &detail) == 0 &&
              bytes_read == output.size(),
          detail.c_str());
  return output;
}

bool PreadAll(int fd, std::uint8_t* output, std::size_t size) {
  std::size_t completed = 0;
  while (completed < size) {
    const ssize_t result =
        pread(fd, output + completed, size - completed,
              static_cast<off_t>(completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

std::vector<std::uint8_t> ReadWholeImage(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image snapshot");
  struct stat attributes {};
  Require(fstat(fd, &attributes) == 0 && attributes.st_size > 0,
          "could not stat image snapshot");
  std::vector<std::uint8_t> output(
      static_cast<std::size_t>(attributes.st_size));
  Require(PreadAll(fd, output.data(), output.size()),
          "could not read image snapshot");
  close(fd);
  return output;
}

void RequireHealthy(const std::string& path) {
  eufs::checker::ConsistencyReport report;
  std::string detail;
  Require(eufs::checker::CheckImage(path, &report, &detail) == 0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete &&
              report.root_reachability_complete &&
              report.block_reference_scan_complete &&
              report.inode_reference_scan_complete && report.issues.empty(),
          "replacement left an inconsistent image");
}

void TestShrinkAndEmptyReplacement() {
  const std::string path = CreateImage();
  const std::uint32_t inode_number = SeedObject(path, Pattern(9000, 'A'));
  auto reader = OpenReader(path);
  eufs::metadata::ObjectReplacePlan shrink;
  std::string detail;
  const std::string shorter = Pattern(5000, 's');
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 1, shorter, 20, &shrink, &detail) == 0 &&
              shrink.old_data_blocks.size() == 3 &&
              shrink.new_data_blocks.size() == 2 &&
              shrink.new_generation == 2,
          detail.c_str());
  const auto old_blocks = shrink.old_data_blocks;
  reader.reset();
  ApplyPlan(path, shrink);

  reader = OpenReader(path);
  eufs::ondisk::InodeRecord inode;
  Require(ReadObject(*reader, inode_number) == shorter &&
              reader->ReadInode(inode_number, &inode, &detail) == 0 &&
              inode.size == shorter.size() && inode.generation == 2,
          "short replacement retained old tail or version");
  for (const std::uint32_t block : old_blocks) {
    Require(!reader->IsBlockAllocated(block),
            "short replacement did not release an old data block");
  }

  eufs::metadata::ObjectReplacePlan empty;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 2, "", 30, &empty, &detail) == 0 &&
              empty.new_data_blocks.empty(),
          detail.c_str());
  const auto second_blocks = empty.old_data_blocks;
  reader.reset();
  ApplyPlan(path, empty);
  reader = OpenReader(path);
  Require(ReadObject(*reader, inode_number).empty() &&
              reader->ReadInode(inode_number, &inode, &detail) == 0 &&
              inode.size == 0 && inode.generation == 3 &&
              inode.indirect_block == 0,
          "empty replacement did not publish an empty generation");
  for (const std::uint32_t block : second_blocks) {
    Require(!reader->IsBlockAllocated(block),
            "empty replacement did not release all old data blocks");
  }
  reader.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

void TestDirectIndirectTransitions() {
  const std::string path = CreateImage();
  const std::uint32_t inode_number = SeedObject(path, "small");
  auto reader = OpenReader(path);
  const std::string large = Pattern(13U * eufs::ondisk::kBlockSize, 'L');
  eufs::metadata::ObjectReplacePlan grow;
  std::string detail;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 1, large, 20, &grow, &detail) == 0 &&
              grow.old_indirect_block == 0 && grow.new_indirect_block != 0,
          detail.c_str());
  reader.reset();
  ApplyPlan(path, grow);

  reader = OpenReader(path);
  Require(ReadObject(*reader, inode_number) == large,
          "direct-to-indirect replacement lost payload");
  eufs::metadata::ObjectReplacePlan shrink;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 2, "tiny", 30, &shrink, &detail) == 0 &&
              shrink.old_indirect_block == grow.new_indirect_block &&
              shrink.new_indirect_block == 0,
          detail.c_str());
  const std::uint32_t old_indirect = shrink.old_indirect_block;
  reader.reset();
  ApplyPlan(path, shrink);
  reader = OpenReader(path);
  Require(ReadObject(*reader, inode_number) == "tiny" &&
              !reader->IsBlockAllocated(old_indirect),
          "indirect-to-direct replacement retained old index ownership");
  reader.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

void TestVersionAndCapacityFailuresArePure() {
  const std::string path = CreateImage(1ULL * 1024ULL * 1024ULL);
  const std::uint32_t inode_number = SeedObject(path, "old");
  auto reader = OpenReader(path);
  const auto before = ReadWholeImage(path);
  eufs::metadata::ObjectReplacePlan output;
  output.inode_number = 77;
  std::string detail;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 99, "new", 20, &output, &detail) ==
                  -ESTALE &&
              output.inode_number == 77 && ReadWholeImage(path) == before,
          "stale version changed output or image");
  const std::string maximum(eufs::ondisk::kMaxFileSize, 'x');
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 1, maximum, 20, &output, &detail) ==
                  -ENOSPC &&
              output.inode_number == 77 && ReadWholeImage(path) == before,
          "ENOSPC returned a partial plan or changed image");
  reader.reset();
  unlink(path.c_str());

  const std::string max_path = CreateImage();
  const std::uint32_t max_inode = SeedObject(
      max_path, "old", std::numeric_limits<std::uint64_t>::max());
  reader = OpenReader(max_path);
  eufs::metadata::ObjectReplacePlan overflow;
  overflow.inode_number = 88;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, max_inode, std::numeric_limits<std::uint64_t>::max(),
              "new", 20, &overflow, &detail) == -EOVERFLOW &&
              overflow.inode_number == 88,
          "generation wrap was accepted or changed output");
  reader.reset();
  unlink(max_path.c_str());
}

void TestFileWriteInvalidatesObjectVersion() {
  const std::string path = CreateImage();
  const std::uint32_t inode_number = SeedObject(path, "abcdef");
  auto reader = OpenReader(path);
  std::string detail;

  // 普通文件写也必须推进同一 inode generation，不能绕过对象版本条件。
  eufs::metadata::FileWritePlan write;
  Require(eufs::metadata::PrepareFileWrite(*reader, inode_number, 2, "XY", 20,
                                           &write, &detail) == 0 &&
              write.old_generation == 1 && write.new_generation == 2,
          detail.c_str());
  reader.reset();
  ApplyPlan(path, write);

  reader = OpenReader(path);
  eufs::ondisk::InodeRecord inode;
  Require(ReadObject(*reader, inode_number) == "abXYef" &&
              reader->ReadInode(inode_number, &inode, &detail) == 0 &&
              inode.generation == 2,
          "ordinary file write did not publish the shared content version");

  eufs::metadata::ObjectReplacePlan stale;
  stale.inode_number = 77;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 1, "stale", 30, &stale, &detail) ==
              -ESTALE &&
              stale.inode_number == 77,
          "object replacement accepted a token invalidated by file write");

  eufs::metadata::ObjectReplacePlan current;
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, inode_number, 2, "current", 40, &current, &detail) == 0 &&
              current.old_generation == 2 && current.new_generation == 3,
          detail.c_str());
  reader.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestShrinkAndEmptyReplacement();
  TestDirectIndirectTransitions();
  TestVersionAndCapacityFailuresArePure();
  TestFileWriteInvalidatesObjectVersion();
  std::cout << "PASS: conditional full-object replacement planning\n";
  return 0;
}
