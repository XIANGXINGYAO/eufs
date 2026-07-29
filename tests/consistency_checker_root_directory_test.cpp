#include "checker/consistency_checker.h"
#include "metadata/empty_file_create_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "storage/writable_image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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

bool ContainsIssue(const eufs::checker::ConsistencyReport& report,
                   eufs::checker::IssueCode code) {
  for (const auto& issue : report.issues) {
    if (issue.code == code) {
      return true;
    }
  }
  return false;
}

bool ContainsRelatedIssue(const eufs::checker::ConsistencyReport& report,
                          eufs::checker::IssueCode code,
                          std::uint32_t parent_inode,
                          std::uint32_t child_inode) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code, parent_inode, child_inode](
          const eufs::checker::CheckIssue& issue) {
        return issue.code == code && issue.inode_number == parent_inode &&
               issue.related_inode_number == child_inode;
      });
}

bool DirectoryScanIsComplete(const eufs::checker::ConsistencyReport& report,
                             std::uint32_t inode_number) {
  return std::any_of(
      report.directory_scan_states.begin(),
      report.directory_scan_states.end(),
      [inode_number](const eufs::checker::DirectoryScanState& state) {
        return state.inode_number == inode_number && state.complete;
      });
}

void ClearBitmapBit(int fd, std::uint64_t bitmap_offset, std::uint32_t bit) {
  const std::uint64_t byte_offset = bitmap_offset + bit / 8U;
  std::uint8_t byte = 0;
  Require(pread(fd, &byte, 1, static_cast<off_t>(byte_offset)) == 1,
          "fixture could not read bitmap byte");
  byte &= static_cast<std::uint8_t>(~(1U << (bit % 8U)));
  Require(pwrite(fd, &byte, 1, static_cast<off_t>(byte_offset)) == 1,
          "fixture could not corrupt bitmap byte");
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-directory-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 64ULL * 1024ULL * 1024ULL;
  options.total_inodes = 1024;
  options.journal_blocks = 256;
  eufs::ondisk::Superblock superblock;
  std::string detail;
  Require(eufs::storage::FormatImage(options, &superblock, &detail),
          detail.c_str());

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::EmptyFileCreatePlan plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 123456789ULL, &plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(options.image_path, plan, &detail) ==
              0,
          detail.c_str());

  const int fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "fixture could not open image");
  ClearBitmapBit(fd,
                 static_cast<std::uint64_t>(
                     superblock.block_bitmap.start_block) *
                     eufs::ondisk::kBlockSize,
                 plan.directory_block);
  ClearBitmapBit(fd,
                 static_cast<std::uint64_t>(
                     superblock.inode_bitmap.start_block) *
                     eufs::ondisk::kBlockSize,
                 plan.inode_number - 1U);
  Require(fdatasync(fd) == 0, "fixture could not sync bitmap corruption");
  close(fd);

  eufs::checker::ConsistencyReport report;
  Require(eufs::checker::CheckImage(options.image_path, &report, &detail) ==
              0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete,
          "bitmap contradictions incorrectly made discovery incomplete");
  Require(report.root_entries.size() == 1 &&
              report.root_entries[0].name == "a.txt" &&
              report.root_entries[0].inode == plan.inode_number,
          "checker did not decode the root directory entry");
  Require(report.discovered_inodes.size() == 1 &&
              report.discovered_inodes[0].inode_number == plan.inode_number &&
              !report.discovered_inodes[0].bitmap_allocated &&
              S_ISREG(report.discovered_inodes[0].inode.mode),
          "checker did not decode the false-free dentry target inode");
  Require(ContainsIssue(
              report, eufs::checker::IssueCode::kDirectoryBlockMarkedFree),
          "checker missed the false-free directory block");
  Require(ContainsIssue(
              report,
              eufs::checker::IssueCode::kDentryTargetInodeMarkedFree),
          "checker missed the false-free dentry target inode");

  const int mismatch_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(mismatch_fd >= 0, "fixture could not reopen inode table");
  const std::uint64_t inode_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
          eufs::ondisk::kBlockSize +
      static_cast<std::uint64_t>(plan.inode_number - 1U) *
          eufs::ondisk::kInodeRecordSize;
  eufs::ondisk::InodeBytes inode_bytes{};
  Require(pread(mismatch_fd, inode_bytes.data(), inode_bytes.size(),
                static_cast<off_t>(inode_offset)) ==
              static_cast<ssize_t>(inode_bytes.size()),
          "fixture could not read dentry target inode");
  eufs::ondisk::InodeRecord target_inode;
  Require(eufs::ondisk::DecodeInode(inode_bytes, plan.inode_number,
                                    &target_inode, &detail),
          detail.c_str());
  target_inode.mode = S_IFDIR | 0755;
  Require(eufs::ondisk::EncodeInode(target_inode, &inode_bytes, &detail),
          detail.c_str());
  Require(pwrite(mismatch_fd, inode_bytes.data(), inode_bytes.size(),
                 static_cast<off_t>(inode_offset)) ==
              static_cast<ssize_t>(inode_bytes.size()),
          "fixture could not write mismatched directory inode");
  Require(fdatasync(mismatch_fd) == 0,
          "fixture could not sync dentry type mismatch");
  close(mismatch_fd);

  eufs::checker::ConsistencyReport mismatch_report;
  Require(eufs::checker::CheckImage(options.image_path, &mismatch_report,
                                       &detail) == 0,
          detail.c_str());
  Require(mismatch_report.status == eufs::checker::ScanStatus::kComplete &&
              mismatch_report.root_reachability_complete,
          "dentry type mismatch incorrectly stopped evidence collection");
  Require(mismatch_report.root_reachable_inode_numbers ==
              std::vector<std::uint32_t>(
                  {superblock.root_inode, plan.inode_number}),
          "type mismatch changed namespace reachability");
  Require(DirectoryScanIsComplete(mismatch_report, plan.inode_number),
          "checker trusted dentry type instead of scanning the inode mode");
  Require(ContainsRelatedIssue(
              mismatch_report,
              eufs::checker::IssueCode::kDentryTypeMismatch,
              superblock.root_inode, plan.inode_number),
          "checker did not report dentry type versus inode mode mismatch");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker follows false-free directory references\n";
  return 0;
}
