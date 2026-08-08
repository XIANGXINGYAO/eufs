// 对比目录项实际引用次数与 inode.link_count，验证硬链接计数不变量。
// 物理 inode 扫描和命名空间引用统计在这里被刻意分成两个阶段验证。
#include "checker/consistency_checker.h"
#include "metadata/empty_file_create_plan.h"
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

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool ContainsIssue(const eufs::checker::ConsistencyReport& report,
                   eufs::checker::IssueCode code,
                   std::uint32_t inode_number) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code, inode_number](const eufs::checker::CheckIssue& issue) {
        return issue.code == code && issue.inode_number == inode_number;
      });
}

const eufs::checker::InodeLinkObservation* FindObservation(
    const eufs::checker::ConsistencyReport& report, std::uint32_t inode_number) {
  const auto found = std::find_if(
      report.inode_link_observations.begin(),
      report.inode_link_observations.end(),
      [inode_number](const eufs::checker::InodeLinkObservation& observation) {
        return observation.inode_number == inode_number;
      });
  return found == report.inode_link_observations.end() ? nullptr : &*found;
}

std::uint64_t InodeOffset(const eufs::ondisk::Superblock& superblock,
                          std::uint32_t inode_number) {
  return static_cast<std::uint64_t>(superblock.inode_table.start_block) *
             eufs::ondisk::kBlockSize +
         static_cast<std::uint64_t>(inode_number - 1U) *
             eufs::ondisk::kInodeRecordSize;
}

eufs::ondisk::InodeRecord ReadInode(int fd,
                                    const eufs::ondisk::Superblock& superblock,
                                    std::uint32_t inode_number,
                                    std::string* detail) {
  eufs::ondisk::InodeBytes bytes{};
  Require(pread(fd, bytes.data(), bytes.size(),
                static_cast<off_t>(InodeOffset(superblock, inode_number))) ==
              static_cast<ssize_t>(bytes.size()),
          "fixture could not read inode");
  eufs::ondisk::InodeRecord inode;
  Require(eufs::ondisk::DecodeInode(bytes, inode_number, &inode, detail),
          detail->c_str());
  return inode;
}

void WriteInode(int fd, const eufs::ondisk::Superblock& superblock,
                const eufs::ondisk::InodeRecord& inode,
                std::string* detail) {
  eufs::ondisk::InodeBytes bytes{};
  Require(eufs::ondisk::EncodeInode(inode, &bytes, detail), detail->c_str());
  Require(pwrite(fd, bytes.data(), bytes.size(),
                 static_cast<off_t>(
                     InodeOffset(superblock, inode.inode_number))) ==
              static_cast<ssize_t>(bytes.size()),
          "fixture could not write inode");
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-links-XXXXXX");
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
  eufs::metadata::EmptyFileCreatePlan create_plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 100ULL, &create_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(options.image_path, create_plan,
                                         &detail) == 0,
          detail.c_str());

  const int fixture_fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fixture_fd >= 0, "fixture could not open link-count image");
  const off_t root_directory_offset = static_cast<off_t>(
      static_cast<std::uint64_t>(create_plan.directory_block) *
      eufs::ondisk::kBlockSize);
  eufs::ondisk::Block original_root_directory{};
  Require(pread(fixture_fd, original_root_directory.data(),
                original_root_directory.size(), root_directory_offset) ==
              static_cast<ssize_t>(original_root_directory.size()),
          "fixture could not preserve root directory block");
  close(fixture_fd);

  eufs::checker::ConsistencyReport healthy;
  Require(eufs::checker::CheckImage(options.image_path, &healthy, &detail) ==
              0,
          detail.c_str());
  const auto* healthy_root = FindObservation(healthy, superblock.root_inode);
  const auto* healthy_file =
      FindObservation(healthy, create_plan.inode_number);
  Require(healthy.inode_reference_scan_complete && healthy_root != nullptr &&
              healthy_root->declared_link_count == 2 &&
              healthy_root->observed_dentry_references == 0 &&
              healthy_root->observed_child_directories == 0 &&
              healthy_root->expected_link_count == 2 &&
              healthy_root->expectation_complete,
          "healthy root link-count evidence is incorrect");
  Require(healthy_file != nullptr &&
              healthy_file->declared_link_count == 1 &&
              healthy_file->observed_dentry_references == 1 &&
              healthy_file->expected_link_count == 1 &&
              healthy_file->expectation_complete,
          "healthy regular-file link-count evidence is incorrect");
  Require(!ContainsIssue(healthy,
                         eufs::checker::IssueCode::kInodeLinkCountMismatch,
                         create_plan.inode_number),
          "healthy regular file reported a link-count mismatch");

  const int mismatch_fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(mismatch_fd >= 0, "fixture could not reopen mismatch image");
  eufs::ondisk::InodeRecord file =
      ReadInode(mismatch_fd, superblock, create_plan.inode_number, &detail);
  file.link_count = 7;
  WriteInode(mismatch_fd, superblock, file, &detail);
  Require(fdatasync(mismatch_fd) == 0,
          "fixture could not sync link-count mismatch");
  close(mismatch_fd);

  eufs::checker::ConsistencyReport mismatch;
  Require(eufs::checker::CheckImage(options.image_path, &mismatch,
                                       &detail) == 0,
          detail.c_str());
  const auto* mismatch_file =
      FindObservation(mismatch, create_plan.inode_number);
  Require(mismatch.inode_reference_scan_complete && mismatch_file != nullptr &&
              mismatch_file->declared_link_count == 7 &&
              mismatch_file->observed_dentry_references == 1 &&
              mismatch_file->expected_link_count == 1,
          "checker did not preserve link-count mismatch evidence");
  Require(ContainsIssue(mismatch,
                        eufs::checker::IssueCode::kInodeLinkCountMismatch,
                        create_plan.inode_number),
          "checker did not report the link-count mismatch");

  eufs::ondisk::Block malformed_root_directory{};
  const int partial_fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(partial_fd >= 0, "fixture could not reopen partial-scan image");
  Require(pwrite(partial_fd, malformed_root_directory.data(),
                 malformed_root_directory.size(), root_directory_offset) ==
              static_cast<ssize_t>(malformed_root_directory.size()),
          "fixture could not corrupt root directory");
  Require(fdatasync(partial_fd) == 0,
          "fixture could not sync partial directory scan");
  close(partial_fd);

  eufs::checker::ConsistencyReport partial;
  Require(eufs::checker::CheckImage(options.image_path, &partial, &detail) ==
              0,
          detail.c_str());
  Require(!partial.inode_reference_scan_complete,
          "malformed directory did not invalidate reference-count proof");
  Require(!ContainsIssue(partial,
                         eufs::checker::IssueCode::kInodeLinkCountMismatch,
                         create_plan.inode_number),
          "checker inferred a link-count mismatch from incomplete directories");

  eufs::ondisk::Block two_file_links{};
  const eufs::ondisk::DirectoryEntry first_link{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kRegular,
      "a",
      0};
  const eufs::ondisk::DirectoryEntry second_link{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kRegular,
      "b",
      0};
  const std::uint16_t first_length = static_cast<std::uint16_t>(
      eufs::ondisk::MinimumDirectoryRecordLength(first_link.name.size()));
  Require(eufs::ondisk::EncodeDirectoryEntry(
              first_link, first_length, two_file_links.data(),
              two_file_links.size(), &detail),
          detail.c_str());
  Require(eufs::ondisk::EncodeDirectoryEntry(
              second_link,
              static_cast<std::uint16_t>(eufs::ondisk::kBlockSize -
                                         first_length),
              two_file_links.data() + first_length,
              two_file_links.size() - first_length, &detail),
          detail.c_str());

  const int hard_link_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(hard_link_fd >= 0, "fixture could not reopen hard-link image");
  file.link_count = 2;
  WriteInode(hard_link_fd, superblock, file, &detail);
  Require(pwrite(hard_link_fd, two_file_links.data(), two_file_links.size(),
                 root_directory_offset) ==
              static_cast<ssize_t>(two_file_links.size()),
          "fixture could not write two regular-file links");
  Require(fdatasync(hard_link_fd) == 0,
          "fixture could not sync regular-file hard links");
  close(hard_link_fd);

  eufs::checker::ConsistencyReport hard_link;
  Require(eufs::checker::CheckImage(options.image_path, &hard_link,
                                       &detail) == 0,
          detail.c_str());
  const auto* hard_link_file =
      FindObservation(hard_link, create_plan.inode_number);
  Require(hard_link.inode_reference_scan_complete &&
              hard_link_file != nullptr &&
              hard_link_file->declared_link_count == 2 &&
              hard_link_file->observed_dentry_references == 2 &&
              hard_link_file->expected_link_count == 2,
          "checker did not count both regular-file namespace links");
  Require(!ContainsIssue(hard_link,
                         eufs::checker::IssueCode::kInodeLinkCountMismatch,
                         create_plan.inode_number),
          "numerically consistent hard links reported a count mismatch");
  Require(ContainsIssue(hard_link,
                        eufs::checker::IssueCode::kRegularFileHardLink,
                        create_plan.inode_number),
          "checker did not enforce the eufs v1 hard-link policy");

  eufs::ondisk::Block root_with_child_directory{};
  const eufs::ondisk::DirectoryEntry child_directory_entry{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kDirectory,
      "d",
      0};
  Require(eufs::ondisk::EncodeDirectoryEntry(
              child_directory_entry, eufs::ondisk::kBlockSize,
              root_with_child_directory.data(), root_with_child_directory.size(),
              &detail),
          detail.c_str());
  const int directory_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(directory_fd >= 0,
          "fixture could not reopen child-directory image");
  eufs::ondisk::InodeRecord root =
      ReadInode(directory_fd, superblock, superblock.root_inode, &detail);
  root.link_count = 3;
  WriteInode(directory_fd, superblock, root, &detail);
  file.mode = S_IFDIR | 0755;
  file.link_count = 2;
  file.size = 0;
  file.direct_blocks.fill(0);
  file.indirect_block = 0;
  WriteInode(directory_fd, superblock, file, &detail);
  Require(pwrite(directory_fd, root_with_child_directory.data(),
                 root_with_child_directory.size(), root_directory_offset) ==
              static_cast<ssize_t>(root_with_child_directory.size()),
          "fixture could not write child-directory dentry");
  Require(fdatasync(directory_fd) == 0,
          "fixture could not sync child-directory link counts");
  close(directory_fd);

  eufs::checker::ConsistencyReport directory;
  Require(eufs::checker::CheckImage(options.image_path, &directory,
                                       &detail) == 0,
          detail.c_str());
  const auto* directory_root =
      FindObservation(directory, superblock.root_inode);
  const auto* child_directory =
      FindObservation(directory, create_plan.inode_number);
  Require(directory.inode_reference_scan_complete &&
              directory_root != nullptr &&
              directory_root->declared_link_count == 3 &&
              directory_root->observed_dentry_references == 0 &&
              directory_root->observed_child_directories == 1 &&
              directory_root->expected_link_count == 3,
          "checker did not apply root 2-plus-child-directory link semantics");
  Require(child_directory != nullptr &&
              child_directory->declared_link_count == 2 &&
              child_directory->observed_dentry_references == 1 &&
              child_directory->observed_child_directories == 0 &&
              child_directory->expected_link_count == 2,
          "checker did not apply non-root directory link semantics");
  Require(!ContainsIssue(directory,
                         eufs::checker::IssueCode::kInodeLinkCountMismatch,
                         superblock.root_inode) &&
              !ContainsIssue(directory,
                             eufs::checker::IssueCode::kInodeLinkCountMismatch,
                             create_plan.inode_number),
          "healthy directory link counts were reported as mismatched");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker audits inode link counts and link policy\n";
  return 0;
}
