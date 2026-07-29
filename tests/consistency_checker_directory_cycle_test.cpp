#include "checker/consistency_checker.h"
#include "metadata/empty_file_create_plan.h"
#include "metadata/first_block_write_plan.h"
#include "metadata/ondisk_format.h"
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
                   eufs::checker::IssueCode code,
                   std::uint32_t inode_number,
                   std::uint32_t related_inode_number) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code, inode_number,
       related_inode_number](const eufs::checker::CheckIssue& issue) {
        return issue.code == code && issue.inode_number == inode_number &&
               issue.related_inode_number == related_inode_number;
      });
}

bool ContainsIssueCode(const eufs::checker::ConsistencyReport& report,
                       eufs::checker::IssueCode code) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code](const eufs::checker::CheckIssue& issue) {
        return issue.code == code;
      });
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-cycle-XXXXXX");
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
              *reader, "d", 0755, 1000, 1000, 100ULL, &create_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(options.image_path, create_plan,
                                         &detail) == 0,
          detail.c_str());

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::FirstBlockWritePlan write_plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, create_plan.inode_number, "directory block", 200ULL,
              &write_plan, &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyFirstBlockWritePlan(
              options.image_path, write_plan, &detail) == 0,
          detail.c_str());

  const int fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "fixture could not open cycle image");
  const std::uint64_t inode_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
          eufs::ondisk::kBlockSize +
      static_cast<std::uint64_t>(create_plan.inode_number - 1U) *
          eufs::ondisk::kInodeRecordSize;
  eufs::ondisk::InodeBytes inode_bytes{};
  Require(pread(fd, inode_bytes.data(), inode_bytes.size(),
                static_cast<off_t>(inode_offset)) ==
              static_cast<ssize_t>(inode_bytes.size()),
          "fixture could not read inode 2");
  eufs::ondisk::InodeRecord child_directory;
  Require(eufs::ondisk::DecodeInode(inode_bytes, create_plan.inode_number,
                                    &child_directory, &detail),
          detail.c_str());
  child_directory.mode = S_IFDIR | 0755;
  child_directory.size = eufs::ondisk::kBlockSize;
  Require(eufs::ondisk::EncodeInode(child_directory, &inode_bytes, &detail),
          detail.c_str());
  Require(pwrite(fd, inode_bytes.data(), inode_bytes.size(),
                 static_cast<off_t>(inode_offset)) ==
              static_cast<ssize_t>(inode_bytes.size()),
          "fixture could not write directory inode 2");

  eufs::ondisk::Block root_directory_block{};
  const eufs::ondisk::DirectoryEntry root_to_child{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kDirectory,
      "d",
      0};
  Require(eufs::ondisk::EncodeDirectoryEntry(
              root_to_child, eufs::ondisk::kBlockSize,
              root_directory_block.data(), root_directory_block.size(),
              &detail),
          detail.c_str());
  Require(pwrite(fd, root_directory_block.data(), root_directory_block.size(),
                 static_cast<off_t>(
                     static_cast<std::uint64_t>(create_plan.directory_block) *
                     eufs::ondisk::kBlockSize)) ==
              static_cast<ssize_t>(root_directory_block.size()),
          "fixture could not write root-to-child directory edge");

  eufs::ondisk::Block child_directory_block{};
  const eufs::ondisk::DirectoryEntry child_to_root{
      superblock.root_inode,
      eufs::ondisk::DirectoryFileType::kDirectory,
      "back",
      0};
  Require(eufs::ondisk::EncodeDirectoryEntry(
              child_to_root, eufs::ondisk::kBlockSize,
              child_directory_block.data(), child_directory_block.size(),
              &detail),
          detail.c_str());
  Require(pwrite(fd, child_directory_block.data(),
                 child_directory_block.size(),
                 static_cast<off_t>(
                     static_cast<std::uint64_t>(write_plan.data_block) *
                     eufs::ondisk::kBlockSize)) ==
              static_cast<ssize_t>(child_directory_block.size()),
          "fixture could not write child-to-root directory edge");
  Require(fdatasync(fd) == 0, "fixture could not sync directory cycle");
  close(fd);

  eufs::checker::ConsistencyReport report;
  Require(eufs::checker::CheckImage(options.image_path, &report, &detail) ==
              0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete,
          "fully decodable cycle was treated as an incomplete scan");
  Require(report.root_reachability_complete,
          "directory cycle incorrectly invalidated reachability closure");
  Require(report.root_reachable_inode_numbers ==
              std::vector<std::uint32_t>(
                  {superblock.root_inode, create_plan.inode_number}),
          "BFS did not terminate with the expected reachable closure");
  Require(report.directory_edges.size() == 2,
          "checker did not preserve both edges in the cycle");
  Require(ContainsIssue(report, eufs::checker::IssueCode::kDirectoryCycle,
                        create_plan.inode_number, superblock.root_inode),
          "checker did not report the child-to-root cycle edge");
  Require(ContainsIssue(
              report,
              eufs::checker::IssueCode::kRootDirectoryReferenced,
              superblock.root_inode, create_plan.inode_number),
          "checker did not report an on-disk directory entry to root");

  eufs::ondisk::Block aliased_root_directory{};
  const eufs::ondisk::DirectoryEntry first_alias{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kDirectory,
      "a",
      0};
  const eufs::ondisk::DirectoryEntry second_alias{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kDirectory,
      "b",
      0};
  const std::uint16_t first_record_length = static_cast<std::uint16_t>(
      eufs::ondisk::MinimumDirectoryRecordLength(first_alias.name.size()));
  Require(eufs::ondisk::EncodeDirectoryEntry(
              first_alias, first_record_length, aliased_root_directory.data(),
              aliased_root_directory.size(), &detail),
          detail.c_str());
  Require(eufs::ondisk::EncodeDirectoryEntry(
              second_alias,
              static_cast<std::uint16_t>(eufs::ondisk::kBlockSize -
                                         first_record_length),
              aliased_root_directory.data() + first_record_length,
              aliased_root_directory.size() - first_record_length, &detail),
          detail.c_str());

  eufs::ondisk::Block empty_child_directory{};
  empty_child_directory[4] =
      static_cast<std::uint8_t>(eufs::ondisk::kBlockSize & 0xFFU);
  empty_child_directory[5] = static_cast<std::uint8_t>(
      (eufs::ondisk::kBlockSize >> 8U) & 0xFFU);
  const int alias_fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(alias_fd >= 0, "fixture could not reopen alias image");
  Require(pwrite(alias_fd, aliased_root_directory.data(),
                 aliased_root_directory.size(),
                 static_cast<off_t>(
                     static_cast<std::uint64_t>(create_plan.directory_block) *
                     eufs::ondisk::kBlockSize)) ==
              static_cast<ssize_t>(aliased_root_directory.size()),
          "fixture could not write two directory aliases");
  Require(pwrite(alias_fd, empty_child_directory.data(),
                 empty_child_directory.size(),
                 static_cast<off_t>(
                     static_cast<std::uint64_t>(write_plan.data_block) *
                     eufs::ondisk::kBlockSize)) ==
              static_cast<ssize_t>(empty_child_directory.size()),
          "fixture could not remove the former cycle edge");
  Require(fdatasync(alias_fd) == 0,
          "fixture could not sync directory hard-link state");
  close(alias_fd);

  eufs::checker::ConsistencyReport alias_report;
  Require(eufs::checker::CheckImage(options.image_path, &alias_report,
                                       &detail) == 0,
          detail.c_str());
  Require(alias_report.status == eufs::checker::ScanStatus::kComplete &&
              alias_report.root_reachability_complete,
          "directory aliases incorrectly invalidated scan completeness");
  Require(alias_report.root_reachable_inode_numbers ==
              std::vector<std::uint32_t>(
                  {superblock.root_inode, create_plan.inode_number}),
          "directory aliases changed the reachable inode set");
  Require(!ContainsIssueCode(alias_report,
                             eufs::checker::IssueCode::kDirectoryCycle),
          "edge to a completed directory was incorrectly reported as a cycle");
  Require(ContainsIssue(alias_report,
                        eufs::checker::IssueCode::kDirectoryHardLink,
                        superblock.root_inode, create_plan.inode_number),
          "checker did not report the second namespace link to a directory");

  constexpr std::uint32_t kSecondTargetInode = 3;
  eufs::ondisk::InodeRecord second_target;
  second_target.inode_number = kSecondTargetInode;
  second_target.mode = S_IFREG | 0644;
  second_target.uid = 1000;
  second_target.gid = 1000;
  second_target.link_count = 1;
  second_target.generation = 1;
  eufs::ondisk::InodeBytes second_target_bytes{};
  Require(eufs::ondisk::EncodeInode(second_target, &second_target_bytes,
                                    &detail),
          detail.c_str());

  eufs::ondisk::Block duplicate_name_directory{};
  const eufs::ondisk::DirectoryEntry first_duplicate{
      create_plan.inode_number,
      eufs::ondisk::DirectoryFileType::kDirectory,
      "x",
      0};
  const eufs::ondisk::DirectoryEntry second_duplicate{
      kSecondTargetInode,
      eufs::ondisk::DirectoryFileType::kRegular,
      "x",
      0};
  const std::uint16_t first_duplicate_length =
      static_cast<std::uint16_t>(eufs::ondisk::MinimumDirectoryRecordLength(
          first_duplicate.name.size()));
  Require(eufs::ondisk::EncodeDirectoryEntry(
              first_duplicate, first_duplicate_length,
              duplicate_name_directory.data(), duplicate_name_directory.size(),
              &detail),
          detail.c_str());
  Require(eufs::ondisk::EncodeDirectoryEntry(
              second_duplicate,
              static_cast<std::uint16_t>(eufs::ondisk::kBlockSize -
                                         first_duplicate_length),
              duplicate_name_directory.data() + first_duplicate_length,
              duplicate_name_directory.size() - first_duplicate_length,
              &detail),
          detail.c_str());

  const int duplicate_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(duplicate_fd >= 0, "fixture could not reopen duplicate-name image");
  const std::uint64_t second_target_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
          eufs::ondisk::kBlockSize +
      static_cast<std::uint64_t>(kSecondTargetInode - 1U) *
          eufs::ondisk::kInodeRecordSize;
  Require(pwrite(duplicate_fd, second_target_bytes.data(),
                 second_target_bytes.size(),
                 static_cast<off_t>(second_target_offset)) ==
              static_cast<ssize_t>(second_target_bytes.size()),
          "fixture could not write second duplicate-name target");
  const std::uint64_t inode_bitmap_byte_offset =
      static_cast<std::uint64_t>(superblock.inode_bitmap.start_block) *
          eufs::ondisk::kBlockSize +
      (kSecondTargetInode - 1U) / 8U;
  std::uint8_t inode_bitmap_byte = 0;
  Require(pread(duplicate_fd, &inode_bitmap_byte, 1,
                static_cast<off_t>(inode_bitmap_byte_offset)) == 1,
          "fixture could not read inode bitmap byte");
  inode_bitmap_byte |= static_cast<std::uint8_t>(
      1U << ((kSecondTargetInode - 1U) % 8U));
  Require(pwrite(duplicate_fd, &inode_bitmap_byte, 1,
                 static_cast<off_t>(inode_bitmap_byte_offset)) == 1,
          "fixture could not allocate second duplicate-name target");
  Require(pwrite(duplicate_fd, duplicate_name_directory.data(),
                 duplicate_name_directory.size(),
                 static_cast<off_t>(
                     static_cast<std::uint64_t>(create_plan.directory_block) *
                     eufs::ondisk::kBlockSize)) ==
              static_cast<ssize_t>(duplicate_name_directory.size()),
          "fixture could not write duplicate directory names");
  Require(fdatasync(duplicate_fd) == 0,
          "fixture could not sync duplicate-name state");
  close(duplicate_fd);

  eufs::checker::ConsistencyReport duplicate_report;
  Require(eufs::checker::CheckImage(options.image_path, &duplicate_report,
                                       &detail) == 0,
          detail.c_str());
  Require(duplicate_report.status == eufs::checker::ScanStatus::kComplete &&
              duplicate_report.root_reachability_complete,
          "duplicate names incorrectly stopped complete evidence collection");
  Require(duplicate_report.root_reachable_inode_numbers ==
              std::vector<std::uint32_t>({superblock.root_inode,
                                          create_plan.inode_number,
                                          kSecondTargetInode}),
          "checker did not retain both duplicate-name targets as evidence");
  Require(ContainsIssue(duplicate_report,
                        eufs::checker::IssueCode::kDuplicateDentryName,
                        superblock.root_inode, kSecondTargetInode),
          "checker did not report the second duplicate active name");
  Require(!ContainsIssueCode(duplicate_report,
                             eufs::checker::IssueCode::kDirectoryCycle) &&
              !ContainsIssueCode(
                  duplicate_report,
                  eufs::checker::IssueCode::kDirectoryHardLink),
          "duplicate names were conflated with cycle or directory hard link");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker separates cycles, hard links, and duplicate names\n";
  return 0;
}
