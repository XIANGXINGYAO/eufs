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

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool ContainsIssueCode(const eufs::checker::ConsistencyReport& report,
                       eufs::checker::IssueCode code) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code](const eufs::checker::CheckIssue& issue) {
        return issue.code == code;
      });
}

bool ContainsIssue(const eufs::checker::ConsistencyReport& report,
                   eufs::checker::IssueCode code,
                   std::uint32_t inode_number,
                   std::uint32_t block_number,
                   std::uint32_t related_inode_number) {
  return std::any_of(
      report.issues.begin(), report.issues.end(),
      [code, inode_number, block_number,
       related_inode_number](const eufs::checker::CheckIssue& issue) {
        return issue.code == code && issue.inode_number == inode_number &&
               issue.block_number == block_number &&
               issue.related_inode_number == related_inode_number;
      });
}

bool ContainsReference(const eufs::checker::ConsistencyReport& report,
                       std::uint32_t block_number,
                       std::uint32_t inode_number,
                       std::uint32_t logical_block,
                       eufs::checker::BlockReferenceKind kind) {
  return std::any_of(
      report.block_references.begin(), report.block_references.end(),
      [block_number, inode_number, logical_block,
       kind](const eufs::checker::BlockReference& reference) {
        return reference.block_number == block_number &&
               reference.inode_number == inode_number &&
               reference.logical_block == logical_block &&
               reference.kind == kind;
      });
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
          "fixture could not read inode record");
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
          "fixture could not write inode record");
}

void SetBlockBitmapBit(int fd, const eufs::ondisk::Superblock& superblock,
                       std::uint32_t block_number, bool allocated) {
  const std::uint64_t byte_offset =
      static_cast<std::uint64_t>(superblock.block_bitmap.start_block) *
          eufs::ondisk::kBlockSize +
      block_number / 8U;
  std::uint8_t byte = 0;
  Require(pread(fd, &byte, 1, static_cast<off_t>(byte_offset)) == 1,
          "fixture could not read block bitmap byte");
  const std::uint8_t mask =
      static_cast<std::uint8_t>(1U << (block_number % 8U));
  if (allocated) {
    byte |= mask;
  } else {
    byte &= static_cast<std::uint8_t>(~mask);
  }
  Require(pwrite(fd, &byte, 1, static_cast<off_t>(byte_offset)) == 1,
          "fixture could not write block bitmap byte");
}

void PutLe32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] =
        static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
  }
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-blocks-XXXXXX");
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

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::FirstBlockWritePlan write_plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, create_plan.inode_number, "hello", 200ULL, &write_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyFirstBlockWritePlan(
              options.image_path, write_plan, &detail) == 0,
          detail.c_str());

  eufs::checker::ConsistencyReport healthy;
  Require(eufs::checker::CheckImage(options.image_path, &healthy, &detail) ==
              0,
          detail.c_str());
  Require(healthy.block_reference_scan_complete,
          "healthy image did not complete block-reference scan");
  Require(healthy.allocated_unreferenced_data_blocks.empty(),
          "healthy image reported an allocated unreferenced block");
  Require(ContainsReference(healthy, create_plan.directory_block,
                            superblock.root_inode, 0,
                            eufs::checker::BlockReferenceKind::kData) &&
              ContainsReference(healthy, write_plan.data_block,
                                create_plan.inode_number, 0,
                                eufs::checker::BlockReferenceKind::kData),
          "healthy direct block references were not recorded");

  const int duplicate_fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(duplicate_fd >= 0, "fixture could not open duplicate-block image");
  eufs::ondisk::InodeRecord inode_two =
      ReadInode(duplicate_fd, superblock, create_plan.inode_number, &detail);
  inode_two.direct_blocks[0] = create_plan.directory_block;
  WriteInode(duplicate_fd, superblock, inode_two, &detail);
  Require(fdatasync(duplicate_fd) == 0,
          "fixture could not sync duplicate-block state");
  close(duplicate_fd);

  eufs::checker::ConsistencyReport duplicate;
  Require(eufs::checker::CheckImage(options.image_path, &duplicate,
                                       &detail) == 0,
          detail.c_str());
  Require(duplicate.block_reference_scan_complete,
          "duplicate block prevented complete reference enumeration");
  Require(ContainsIssue(duplicate,
                        eufs::checker::IssueCode::kDuplicateBlockReference,
                        create_plan.inode_number, create_plan.directory_block,
                        superblock.root_inode),
          "checker did not report two inodes sharing one physical block");
  Require(ContainsIssue(
              duplicate,
              eufs::checker::IssueCode::kAllocatedDataBlockUnreferenced, 0,
              write_plan.data_block, 0),
          "checker did not report the displaced allocated data block");

  const int false_free_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(false_free_fd >= 0, "fixture could not open false-free image");
  inode_two.direct_blocks[0] = write_plan.data_block;
  WriteInode(false_free_fd, superblock, inode_two, &detail);
  SetBlockBitmapBit(false_free_fd, superblock, write_plan.data_block, false);
  Require(fdatasync(false_free_fd) == 0,
          "fixture could not sync false-free data block");
  close(false_free_fd);

  eufs::checker::ConsistencyReport false_free;
  Require(eufs::checker::CheckImage(options.image_path, &false_free,
                                       &detail) == 0,
          detail.c_str());
  Require(false_free.block_reference_scan_complete,
          "false-free data block prevented reference enumeration");
  Require(ContainsIssue(false_free,
                        eufs::checker::IssueCode::kReferencedBlockMarkedFree,
                        create_plan.inode_number, write_plan.data_block, 0),
          "checker did not report a referenced false-free data block");
  Require(!ContainsIssueCode(
              false_free,
              eufs::checker::IssueCode::kAllocatedDataBlockUnreferenced),
          "false-free referenced block was also reported as a leak");

  const int indirect_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(indirect_fd >= 0, "fixture could not open indirect-block image");
  constexpr std::uint32_t kFixtureBlockGap = 16;
  const std::uint32_t first_direct_block =
      write_plan.data_block + kFixtureBlockGap;
  const std::uint32_t indirect_block =
      first_direct_block + eufs::ondisk::kDirectBlockCount;
  const std::uint32_t indirect_data_block = indirect_block + 1U;
  Require(indirect_data_block < superblock.total_blocks,
          "indirect fixture exceeds image geometry");

  inode_two.size =
      (eufs::ondisk::kDirectBlockCount + 1U) *
      static_cast<std::uint64_t>(eufs::ondisk::kBlockSize);
  for (std::uint32_t index = 0; index < eufs::ondisk::kDirectBlockCount;
       ++index) {
    inode_two.direct_blocks[index] = first_direct_block + index;
  }
  inode_two.indirect_block = indirect_block;
  WriteInode(indirect_fd, superblock, inode_two, &detail);
  SetBlockBitmapBit(indirect_fd, superblock, write_plan.data_block, false);
  for (std::uint32_t block = first_direct_block;
       block <= indirect_data_block; ++block) {
    SetBlockBitmapBit(indirect_fd, superblock, block, true);
  }
  eufs::ondisk::Block indirect_bytes{};
  PutLe32(indirect_bytes.data(), indirect_data_block);
  Require(pwrite(indirect_fd, indirect_bytes.data(), indirect_bytes.size(),
                 static_cast<off_t>(
                     static_cast<std::uint64_t>(indirect_block) *
                     eufs::ondisk::kBlockSize)) ==
              static_cast<ssize_t>(indirect_bytes.size()),
          "fixture could not write single-indirect pointer block");
  Require(fdatasync(indirect_fd) == 0,
          "fixture could not sync single-indirect state");
  close(indirect_fd);

  eufs::checker::ConsistencyReport indirect;
  Require(eufs::checker::CheckImage(options.image_path, &indirect,
                                       &detail) == 0,
          detail.c_str());
  Require(indirect.block_reference_scan_complete,
          "single-indirect image did not complete reference enumeration");
  Require(indirect.allocated_unreferenced_data_blocks.empty(),
          "single-indirect image reported a false leak");
  Require(ContainsReference(indirect, indirect_block,
                            create_plan.inode_number, 0,
                            eufs::checker::BlockReferenceKind::kIndirect) &&
              ContainsReference(
                  indirect, indirect_data_block, create_plan.inode_number,
                  eufs::ondisk::kDirectBlockCount,
                  eufs::checker::BlockReferenceKind::kData),
          "checker did not traverse the single-indirect pointer block");
  Require(!ContainsIssueCode(
              indirect, eufs::checker::IssueCode::kMissingRequiredBlock) &&
              !ContainsIssueCode(
                  indirect,
                  eufs::checker::IssueCode::kDuplicateBlockReference) &&
              !ContainsIssueCode(
                  indirect,
                  eufs::checker::IssueCode::kReferencedBlockMarkedFree),
          "valid single-indirect mapping produced a block-reference issue");

  const int undecodable_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(undecodable_fd >= 0,
          "fixture could not open undecodable-inode image");
  std::uint8_t first_inode_byte = 0;
  const off_t inode_two_offset = static_cast<off_t>(
      InodeOffset(superblock, create_plan.inode_number));
  Require(pread(undecodable_fd, &first_inode_byte, 1, inode_two_offset) == 1,
          "fixture could not read inode byte for CRC corruption");
  first_inode_byte ^= 0x01U;
  Require(pwrite(undecodable_fd, &first_inode_byte, 1, inode_two_offset) == 1,
          "fixture could not corrupt inode CRC");
  Require(fdatasync(undecodable_fd) == 0,
          "fixture could not sync undecodable inode");
  close(undecodable_fd);

  eufs::checker::ConsistencyReport undecodable;
  Require(eufs::checker::CheckImage(options.image_path, &undecodable,
                                       &detail) == 0,
          detail.c_str());
  Require(undecodable.status == eufs::checker::ScanStatus::kPartial &&
              !undecodable.block_reference_scan_complete,
          "undecodable allocated inode did not invalidate block-reference proof");
  Require(undecodable.allocated_unreferenced_data_blocks.empty() &&
              !ContainsIssueCode(
                  undecodable,
                  eufs::checker::IssueCode::kAllocatedDataBlockUnreferenced),
          "checker inferred leaked blocks while inode ownership was unknown");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker audits direct and single-indirect block ownership\n";
  return 0;
}
