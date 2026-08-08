// 验证 mkdir/create 的统一新对象计划对目录项、inode、bitmap 和父目录元数据的修改。
// 同时检查重名、父目录非法、空间不足和目录扩容等失败路径不产生半份计划。
#include "checker/consistency_checker.h"
#include "journal/journal_control_store.h"
#include "metadata/new_object_plan.h"
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
#include <memory>
#include <string>
#include <string_view>
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
  std::strcpy(path_template.data(), "/tmp/eufs-new-object-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());
  return path_template.data();
}

std::string CreateImage(std::uint64_t size_bytes,
                        std::uint32_t total_inodes) {
  eufs::storage::MkfsOptions options;
  options.image_path = TemporaryPath();
  options.image_size_bytes = size_bytes;
  options.total_inodes = total_inodes;
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

void ApplyPlan(const std::string& path,
               const eufs::metadata::NewObjectPlan& plan) {
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

std::string ReadObject(const eufs::storage::ImageReader& reader,
                       std::string_view name) {
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  const std::string path = "/" + std::string(name);
  Require(reader.ResolvePath(path, &inode_number, &inode, &detail) == 0,
          detail.c_str());
  std::vector<std::uint8_t> output(static_cast<std::size_t>(inode.size));
  std::size_t bytes_read = 0;
  Require(reader.ReadFile(inode_number, 0, output.data(), output.size(),
                          &bytes_read, &detail) == 0 &&
              bytes_read == output.size(),
          detail.c_str());
  return std::string(reinterpret_cast<const char*>(output.data()),
                     output.size());
}

bool PreadAll(int fd, std::uint8_t* output, std::size_t size,
              std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const ssize_t result =
        pread(fd, output + completed, size - completed,
              static_cast<off_t>(offset + completed));
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
  Require(PreadAll(fd, output.data(), output.size(), 0),
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
              report.bitmap_geometry_scan_complete &&
              report.root_inode_decoded &&
              report.root_reachability_complete &&
              report.block_reference_scan_complete &&
              report.inode_reference_scan_complete && report.issues.empty(),
          "new-object image failed global consistency checks");
}

std::string Pattern(std::size_t size, char base) {
  std::string output(size, '\0');
  for (std::size_t index = 0; index < output.size(); ++index) {
    output[index] = static_cast<char>(base + index % 19U);
  }
  return output;
}

std::string LongUniqueName(std::uint32_t index) {
  std::string name(eufs::ondisk::kMaxNameLength, 'n');
  const std::string prefix = std::to_string(index);
  std::copy(prefix.begin(), prefix.end(), name.begin());
  return name;
}

void TestPackedDirectoryAndIndirectObject() {
  const std::string path = CreateImage(8ULL * 1024ULL * 1024ULL, 512);
  std::string detail;

  auto reader = OpenReader(path);
  eufs::metadata::NewObjectPlan first;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "a.txt", "alpha", 0644, 1000, 1000, 10, &first,
              &detail) == 0 &&
              first.directory_grew && first.data_blocks.size() == 1,
          detail.c_str());
  reader.reset();
  ApplyPlan(path, first);

  const std::string payload =
      Pattern(13U * eufs::ondisk::kBlockSize, 'A');
  reader = OpenReader(path);
  eufs::metadata::NewObjectPlan second;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "b.bin", payload, 0600, 1001, 1002, 20, &second,
              &detail) == 0 &&
              !second.directory_grew &&
              second.directory_block == first.directory_block &&
              second.data_blocks.size() == 13 &&
              second.file_indirect_block != 0 &&
              second.metadata_after_images.count(
                  second.file_indirect_block) == 1,
          detail.c_str());
  reader.reset();
  ApplyPlan(path, second);

  reader = OpenReader(path);
  Require(ReadObject(*reader, "a.txt") == "alpha" &&
              ReadObject(*reader, "b.bin") == payload,
          "packed directory objects do not contain committed data");
  std::vector<eufs::ondisk::DirectoryEntry> entries;
  Require(reader->ListDirectory(reader->superblock().root_inode, &entries,
                                &detail) == 0 &&
              entries.size() == 2,
          "second object did not share the existing directory block");
  eufs::ondisk::InodeRecord root;
  Require(reader->ReadInode(reader->superblock().root_inode, &root, &detail) ==
                  0 &&
              root.size == eufs::ondisk::kBlockSize,
          "packed insertion unexpectedly grew the root directory");

  const auto before_duplicate = ReadWholeImage(path);
  eufs::metadata::NewObjectPlan duplicate;
  duplicate.inode_number = 99;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "a.txt", "replacement", 0644, 0, 0, 30, &duplicate,
              &detail) == -EEXIST &&
              duplicate.inode_number == 99 &&
              ReadWholeImage(path) == before_duplicate,
          "duplicate object planning changed output or image state");
  reader.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

void TestMaximumObjectFitsMetadataJournal() {
  const std::string path = CreateImage(8ULL * 1024ULL * 1024ULL, 512);
  const std::string payload = Pattern(eufs::ondisk::kMaxFileSize, 'M');
  auto reader = OpenReader(path);
  eufs::metadata::NewObjectPlan plan;
  std::string detail;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "maximum.bin", payload, 0644, 1000, 1000, 30, &plan,
              &detail) == 0 &&
              plan.data_blocks.size() ==
                  eufs::ondisk::kMaxFileSize / eufs::ondisk::kBlockSize &&
              plan.file_indirect_block != 0 &&
              plan.metadata_after_images.size() < 16,
          detail.c_str());
  reader.reset();
  ApplyPlan(path, plan);

  reader = OpenReader(path);
  Require(ReadObject(*reader, "maximum.bin") == payload,
          "maximum-size object did not survive metadata-only journaling");
  reader.reset();
  RequireHealthy(path);
  std::cout << "maximum_object_bytes=" << payload.size()
            << " data_blocks=" << plan.data_blocks.size()
            << " journaled_metadata_blocks="
            << plan.metadata_after_images.size() << '\n';
  unlink(path.c_str());
}

void TestDirectoryGrowsAcrossSingleIndirectBoundary() {
  const std::string path = CreateImage(16ULL * 1024ULL * 1024ULL, 512);
  std::string detail;
  std::uint32_t object_count = 0;
  eufs::metadata::NewObjectPlan crossing;

  for (;;) {
    auto reader = OpenReader(path);
    eufs::ondisk::InodeRecord root;
    Require(reader->ReadInode(reader->superblock().root_inode, &root,
                              &detail) == 0,
            detail.c_str());
    if (root.size == 13ULL * eufs::ondisk::kBlockSize) {
      Require(root.indirect_block != 0 && object_count > 12,
              "thirteenth directory block lacks single-indirect mapping");
      break;
    }

    eufs::metadata::NewObjectPlan plan;
    const std::string name = LongUniqueName(object_count);
    Require(eufs::metadata::PrepareNewRootObject(
                *reader, name, "", 0644, 1000, 1000,
                1000ULL + object_count, &plan, &detail) == 0,
            detail.c_str());
    reader.reset();
    ApplyPlan(path, plan);
    ++object_count;
    if (plan.root_indirect_block != 0) {
      crossing = plan;
    }
    Require(object_count < 250,
            "directory did not reach single-indirect mapping in time");
  }

  Require(crossing.directory_grew && crossing.root_indirect_block != 0 &&
              crossing.metadata_after_images.count(
                  crossing.root_indirect_block) == 1,
          "root directory crossing did not journal its indirect block");
  auto reader = OpenReader(path);
  std::vector<eufs::ondisk::DirectoryEntry> entries;
  Require(reader->ListDirectory(reader->superblock().root_inode, &entries,
                                &detail) == 0 &&
              entries.size() == object_count,
          "multi-block directory lost object names");
  Require(ReadObject(*reader, LongUniqueName(0)).empty() &&
              ReadObject(*reader, LongUniqueName(object_count - 1U)).empty(),
          "empty objects at directory boundaries are unreadable");
  reader.reset();
  RequireHealthy(path);
  std::cout << "directory_objects=" << object_count
            << " directory_blocks=13\n";
  unlink(path.c_str());
}

void TestEnospcLeavesNoPlanOrDiskChanges() {
  const std::string path = CreateImage(1ULL * 1024ULL * 1024ULL, 128);
  auto reader = OpenReader(path);
  const auto before = ReadWholeImage(path);
  const std::string oversized(eufs::ondisk::kMaxFileSize, 'x');
  eufs::metadata::NewObjectPlan output;
  output.inode_number = 77;
  std::string detail;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "too-large-for-image", oversized, 0644, 0, 0, 10,
              &output, &detail) == -ENOSPC &&
              output.inode_number == 77 && ReadWholeImage(path) == before,
          "new-object ENOSPC returned a plan or changed the image");
  reader.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestPackedDirectoryAndIndirectObject();
  TestMaximumObjectFitsMetadataJournal();
  TestDirectoryGrowsAcrossSingleIndirectBoundary();
  TestEnospcLeavesNoPlanOrDiskChanges();
  std::cout << "PASS: atomic new-object planning and directory growth\n";
  return 0;
}
