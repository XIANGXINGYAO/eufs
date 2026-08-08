// 构造 bitmap 几何和镜像边界损坏，验证 eufsck 报告结构性错误而不越界读取。
// 该测试关注“还能安全扫描多少”，而不是目录可达性。
#include "checker/consistency_checker.h"
#include "checker/consistency_report.h"
#include "checker/eufsck_command.h"
#include "storage/mkfs.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

const eufs::checker::CheckIssue* FindIssue(
    const eufs::checker::ConsistencyReport& report,
    eufs::checker::IssueCode code) {
  for (const auto& issue : report.issues) {
    if (issue.code == code) {
      return &issue;
    }
  }
  return nullptr;
}

void ClearBitmapBit(int fd, const eufs::ondisk::Region& bitmap_region,
                    std::uint64_t bit) {
  const std::uint64_t offset =
      static_cast<std::uint64_t>(bitmap_region.start_block) *
          eufs::ondisk::kBlockSize +
      bit / 8U;
  std::uint8_t byte = 0;
  Require(pread(fd, &byte, 1, static_cast<off_t>(offset)) == 1,
          "could not read bitmap byte");
  byte &= static_cast<std::uint8_t>(
      ~static_cast<std::uint8_t>(1U << (bit % 8U)));
  Require(pwrite(fd, &byte, 1, static_cast<off_t>(offset)) == 1,
          "could not clear bitmap bit");
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-bitmap-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  eufs::ondisk::Superblock superblock;
  std::string detail;
  Require(eufs::storage::FormatImage(options, &superblock, &detail),
          detail.c_str());

  eufs::checker::ConsistencyReport healthy;
  Require(eufs::checker::CheckImage(options.image_path, &healthy, &detail) ==
              0,
          detail.c_str());
  Require(healthy.bitmap_geometry_scan_complete && healthy.issues.empty(),
          "healthy bitmap geometry evidence is incorrect");

  const int fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "could not open bitmap corruption fixture");
  ClearBitmapBit(fd, superblock.block_bitmap, 0);
  ClearBitmapBit(fd, superblock.block_bitmap,
                 superblock.data.start_block - 1U);
  ClearBitmapBit(fd, superblock.inode_bitmap, superblock.total_inodes);
  ClearBitmapBit(fd, superblock.inode_bitmap,
                 static_cast<std::uint64_t>(superblock.total_inodes) + 2U);
  ClearBitmapBit(fd, superblock.block_bitmap, superblock.total_blocks);
  ClearBitmapBit(fd, superblock.block_bitmap,
                 static_cast<std::uint64_t>(superblock.total_blocks) + 2U);
  Require(fdatasync(fd) == 0, "could not sync bitmap corruption fixture");
  close(fd);

  eufs::checker::ConsistencyReport corrupted;
  Require(eufs::checker::CheckImage(options.image_path, &corrupted, &detail) ==
              0,
          detail.c_str());
  Require(corrupted.status == eufs::checker::ScanStatus::kComplete &&
              corrupted.bitmap_geometry_scan_complete &&
              corrupted.root_inode_decoded &&
              corrupted.root_reachability_complete &&
              corrupted.block_reference_scan_complete &&
              corrupted.inode_reference_scan_complete,
          "bitmap contradictions incorrectly stopped later checker passes");
  Require(corrupted.issues.size() == 3,
          "bitmap contradictions were not aggregated by invariant class");

  const auto* metadata = FindIssue(
      corrupted, eufs::checker::IssueCode::kMetadataBlockMarkedFree);
  const auto* inode_tail = FindIssue(
      corrupted, eufs::checker::IssueCode::kInodeBitmapTailNotReserved);
  const auto* block_tail = FindIssue(
      corrupted, eufs::checker::IssueCode::kBlockBitmapTailNotReserved);
  Require(metadata != nullptr && metadata->block_number == 0 &&
              metadata->first_bitmap_bit == 0 &&
              metadata->occurrence_count == 2,
          "metadata-prefix contradiction evidence is incorrect");
  Require(inode_tail != nullptr &&
              inode_tail->first_bitmap_bit == superblock.total_inodes &&
              inode_tail->occurrence_count == 2,
          "inode-tail contradiction evidence is incorrect");
  Require(block_tail != nullptr &&
              block_tail->first_bitmap_bit == superblock.total_blocks &&
              block_tail->occurrence_count == 2,
          "block-tail contradiction evidence is incorrect");
  Require(eufs::checker::ClassifyReport(corrupted) ==
              eufs::checker::CheckVerdict::kInconsistent,
          "complete bitmap contradictions did not classify as inconsistent");

  std::ostringstream output;
  std::ostringstream error;
  Require(eufs::checker::RunEufsck({"--json", options.image_path}, output,
                                   error) ==
              eufs::checker::kEufsckExitInconsistent,
          "bitmap contradictions did not produce eufsck exit 1");
  Require(error.str().empty() &&
              output.str().find("\"bitmap_geometry_scan_complete\":true") !=
                  std::string::npos &&
              output.str().find("\"occurrences\":2") !=
                  std::string::npos,
          "JSON report lost aggregated bitmap geometry evidence");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker bitmap geometry audit\n";
  return 0;
}
