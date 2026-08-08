// 构造 bitmap 中已分配但没有目录路径可达的 inode，验证孤儿对象检测。
// 该测试确保“磁盘上存在”和“命名空间可达”不会被错误地合并。
#include "checker/consistency_checker.h"
#include "metadata/empty_file_create_plan.h"
#include "tests/support/first_block_write_plan.h"
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "tests/support/writable_image.h"

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

bool ContainsIssueForInode(const eufs::checker::ConsistencyReport& report,
                           eufs::checker::IssueCode code,
                           std::uint32_t inode_number) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code, inode_number](const eufs::checker::CheckIssue& issue) {
        return issue.code == code && issue.inode_number == inode_number;
      });
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

bool DirectoryScanIs(const eufs::checker::ConsistencyReport& report,
                     std::uint32_t inode_number, bool complete) {
  return std::any_of(
      report.directory_scan_states.begin(),
      report.directory_scan_states.end(),
      [inode_number, complete](
          const eufs::checker::DirectoryScanState& state) {
        return state.inode_number == inode_number &&
               state.complete == complete;
      });
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-orphan-XXXXXX");
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

  eufs::ondisk::Block free_directory_block{};
  free_directory_block[4] =
      static_cast<std::uint8_t>(eufs::ondisk::kBlockSize & 0xFFU);
  free_directory_block[5] = static_cast<std::uint8_t>(
      (eufs::ondisk::kBlockSize >> 8U) & 0xFFU);

  const int fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "fixture could not open image");
  const off_t directory_offset = static_cast<off_t>(
      static_cast<std::uint64_t>(plan.directory_block) *
      eufs::ondisk::kBlockSize);
  Require(pwrite(fd, free_directory_block.data(), free_directory_block.size(),
                 directory_offset) ==
              static_cast<ssize_t>(free_directory_block.size()),
          "fixture could not remove the root directory entry");
  Require(fdatasync(fd) == 0, "fixture could not sync the orphan state");
  close(fd);

  eufs::checker::ConsistencyReport report;
  Require(eufs::checker::CheckImage(options.image_path, &report, &detail) ==
              0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete,
          "complete orphan fixture was not fully scanned");
  Require(report.root_reachability_complete,
          "complete root directory did not close the reachability scan");
  Require(report.root_entries.empty(),
          "free root directory record was treated as an active entry");
  Require(report.physical_scan_inode_numbers ==
              std::vector<std::uint32_t>({1U, plan.inode_number}),
          "physical scan set did not include the bitmap-allocated inode");
  Require(report.root_reachable_inode_numbers ==
              std::vector<std::uint32_t>({1U}),
          "root reachability incorrectly adopted the orphan inode");
  Require(report.discovered_inodes.size() == 1 &&
              report.discovered_inodes[0].inode_number == plan.inode_number &&
              report.discovered_inodes[0].bitmap_allocated &&
              S_ISREG(report.discovered_inodes[0].inode.mode),
          "checker did not decode the bitmap-discovered orphan inode");
  Require(ContainsIssueForInode(
              report, eufs::checker::IssueCode::kInodeUnreachable,
              plan.inode_number),
          "checker did not report the allocated unreachable inode");

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::FirstBlockWritePlan write_plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, plan.inode_number, "directory block", 123456790ULL,
              &write_plan, &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyFirstBlockWritePlan(
              options.image_path, write_plan, &detail) == 0,
          detail.c_str());

  const int graph_fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(graph_fd >= 0, "fixture could not reopen graph image");
  const std::uint64_t inode_table_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
      eufs::ondisk::kBlockSize;
  const std::uint64_t inode_two_offset =
      inode_table_offset +
      static_cast<std::uint64_t>(plan.inode_number - 1U) *
          eufs::ondisk::kInodeRecordSize;
  eufs::ondisk::InodeBytes inode_two_bytes{};
  Require(pread(graph_fd, inode_two_bytes.data(), inode_two_bytes.size(),
                static_cast<off_t>(inode_two_offset)) ==
              static_cast<ssize_t>(inode_two_bytes.size()),
          "fixture could not read inode 2");
  eufs::ondisk::InodeRecord inode_two;
  Require(eufs::ondisk::DecodeInode(inode_two_bytes, plan.inode_number,
                                    &inode_two, &detail),
          detail.c_str());
  inode_two.mode = S_IFDIR | 0755;
  inode_two.size = eufs::ondisk::kBlockSize;
  Require(eufs::ondisk::EncodeInode(inode_two, &inode_two_bytes, &detail),
          detail.c_str());
  Require(pwrite(graph_fd, inode_two_bytes.data(), inode_two_bytes.size(),
                 static_cast<off_t>(inode_two_offset)) ==
              static_cast<ssize_t>(inode_two_bytes.size()),
          "fixture could not write directory inode 2");

  constexpr std::uint32_t kFalseFreeChildInode = 3;
  eufs::ondisk::InodeRecord inode_three;
  inode_three.inode_number = kFalseFreeChildInode;
  inode_three.mode = S_IFREG | 0644;
  inode_three.uid = 1000;
  inode_three.gid = 1000;
  inode_three.link_count = 1;
  inode_three.generation = 1;
  eufs::ondisk::InodeBytes inode_three_bytes{};
  Require(eufs::ondisk::EncodeInode(inode_three, &inode_three_bytes, &detail),
          detail.c_str());
  const std::uint64_t inode_three_offset =
      inode_table_offset +
      static_cast<std::uint64_t>(kFalseFreeChildInode - 1U) *
          eufs::ondisk::kInodeRecordSize;
  Require(pwrite(graph_fd, inode_three_bytes.data(), inode_three_bytes.size(),
                 static_cast<off_t>(inode_three_offset)) ==
              static_cast<ssize_t>(inode_three_bytes.size()),
          "fixture could not write false-free inode 3");

  eufs::ondisk::Block child_directory_block{};
  const eufs::ondisk::DirectoryEntry child_entry{
      kFalseFreeChildInode,
      eufs::ondisk::DirectoryFileType::kRegular,
      "b.txt",
      0};
  Require(eufs::ondisk::EncodeDirectoryEntry(
              child_entry, eufs::ondisk::kBlockSize,
              child_directory_block.data(), child_directory_block.size(),
              &detail),
          detail.c_str());
  const off_t child_directory_offset = static_cast<off_t>(
      static_cast<std::uint64_t>(write_plan.data_block) *
      eufs::ondisk::kBlockSize);
  Require(pwrite(graph_fd, child_directory_block.data(),
                 child_directory_block.size(), child_directory_offset) ==
              static_cast<ssize_t>(child_directory_block.size()),
          "fixture could not write orphan directory edge");
  Require(fdatasync(graph_fd) == 0,
          "fixture could not sync orphan directory graph");
  close(graph_fd);

  eufs::checker::ConsistencyReport graph_report;
  Require(eufs::checker::CheckImage(options.image_path, &graph_report,
                                       &detail) == 0,
          detail.c_str());
  Require(graph_report.status == eufs::checker::ScanStatus::kComplete &&
              graph_report.root_reachability_complete,
          "complete orphan directory graph was not fully scanned");
  Require(graph_report.physical_scan_inode_numbers ==
              std::vector<std::uint32_t>(
                  {1U, plan.inode_number, kFalseFreeChildInode}),
          "physical scan did not follow the orphan directory edge");
  Require(graph_report.root_reachable_inode_numbers ==
              std::vector<std::uint32_t>({1U}),
          "orphan directory edge contaminated root reachability");
  Require(graph_report.directory_edges.size() == 1 &&
              graph_report.directory_edges[0].parent_inode_number ==
                  plan.inode_number &&
              graph_report.directory_edges[0].child_inode_number ==
                  kFalseFreeChildInode,
          "checker did not preserve the physical directory edge");
  Require(DirectoryScanIs(graph_report, 1U, true) &&
              DirectoryScanIs(graph_report, plan.inode_number, true),
          "checker did not record per-directory scan completeness");
  Require(ContainsIssueForInode(
              graph_report, eufs::checker::IssueCode::kInodeUnreachable,
              plan.inode_number),
          "checker did not report the allocated orphan directory");
  Require(ContainsRelatedIssue(
              graph_report,
              eufs::checker::IssueCode::kDentryTargetInodeMarkedFree,
              plan.inode_number, kFalseFreeChildInode),
          "checker missed the false-free child of an orphan directory");
  Require(!ContainsIssueForInode(
              graph_report, eufs::checker::IssueCode::kInodeUnreachable,
              kFalseFreeChildInode),
          "checker confused a false-free target with an allocated orphan");

  eufs::ondisk::Block malformed_orphan_directory{};
  const int malformed_orphan_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(malformed_orphan_fd >= 0,
          "fixture could not reopen orphan directory block");
  Require(pwrite(malformed_orphan_fd, malformed_orphan_directory.data(),
                 malformed_orphan_directory.size(),
                 child_directory_offset) ==
              static_cast<ssize_t>(malformed_orphan_directory.size()),
          "fixture could not corrupt orphan directory block");
  Require(fdatasync(malformed_orphan_fd) == 0,
          "fixture could not sync malformed orphan directory");
  close(malformed_orphan_fd);

  eufs::checker::ConsistencyReport orphan_partial;
  Require(eufs::checker::CheckImage(options.image_path, &orphan_partial,
                                       &detail) == 0,
          detail.c_str());
  Require(orphan_partial.status == eufs::checker::ScanStatus::kPartial,
          "malformed orphan directory did not make physical scan partial");
  Require(orphan_partial.root_reachability_complete,
          "unreachable directory damage invalidated root reachability");
  Require(DirectoryScanIs(orphan_partial, plan.inode_number, false),
          "malformed orphan directory was marked completely scanned");
  Require(ContainsIssueForInode(
              orphan_partial, eufs::checker::IssueCode::kInodeUnreachable,
              plan.inode_number),
          "complete root proof did not retain the orphan conclusion");

  eufs::ondisk::Block malformed_directory_block{};
  const int malformed_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(malformed_fd >= 0, "fixture could not reopen image");
  Require(pwrite(malformed_fd, malformed_directory_block.data(),
                 malformed_directory_block.size(), directory_offset) ==
              static_cast<ssize_t>(malformed_directory_block.size()),
          "fixture could not write the malformed directory record");
  Require(fdatasync(malformed_fd) == 0,
          "fixture could not sync the malformed directory record");
  close(malformed_fd);

  eufs::checker::ConsistencyReport incomplete;
  Require(eufs::checker::CheckImage(options.image_path, &incomplete,
                                       &detail) == 0,
          detail.c_str());
  Require(incomplete.status == eufs::checker::ScanStatus::kPartial &&
              !incomplete.root_reachability_complete,
          "malformed root directory did not invalidate reachability proof");
  Require(!ContainsIssueForInode(
              incomplete, eufs::checker::IssueCode::kInodeUnreachable,
              plan.inode_number),
          "checker inferred unreachability from an incomplete root scan");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker separates physical scan from root reachability\n";
  return 0;
}
