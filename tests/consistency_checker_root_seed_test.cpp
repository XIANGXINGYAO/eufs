#include "checker/consistency_checker.h"
#include "storage/mkfs.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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
                   eufs::checker::IssueCode code) {
  for (const auto& issue : report.issues) {
    if (issue.code == code) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-check-root-XXXXXX");
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
  Require(healthy.status == eufs::checker::ScanStatus::kComplete &&
              healthy.root_inode_decoded && healthy.issues.empty() &&
              S_ISDIR(healthy.root_inode.mode),
          "healthy root seed audit is incorrect");

  const int fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "checker fixture could not open image");
  const std::uint64_t bitmap_offset =
      static_cast<std::uint64_t>(superblock.inode_bitmap.start_block) *
      eufs::ondisk::kBlockSize;
  std::uint8_t first_byte = 0;
  Require(pread(fd, &first_byte, 1, static_cast<off_t>(bitmap_offset)) == 1,
          "checker fixture could not read inode bitmap");
  first_byte &= static_cast<std::uint8_t>(~1U);
  Require(pwrite(fd, &first_byte, 1, static_cast<off_t>(bitmap_offset)) == 1,
          "checker fixture could not corrupt inode bitmap");
  Require(fdatasync(fd) == 0, "checker fixture could not sync corruption");
  close(fd);

  eufs::checker::ConsistencyReport corrupted;
  Require(eufs::checker::CheckImage(options.image_path, &corrupted,
                                       &detail) == 0,
          detail.c_str());
  Require(corrupted.status == eufs::checker::ScanStatus::kComplete,
          "bitmap contradiction incorrectly made the root scan incomplete");
  Require(corrupted.root_inode_decoded && S_ISDIR(corrupted.root_inode.mode),
          "checker trusted the cleared bitmap instead of decoding root");
  Require(ContainsIssue(corrupted,
                        eufs::checker::IssueCode::kRootInodeMarkedFree),
          "checker did not report the cleared root inode bit");
  Require(!ContainsIssue(corrupted,
                         eufs::checker::IssueCode::kRootInodeUndecodable),
          "checker incorrectly reported a decodable root inode");

  const int root_type_fd =
      open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(root_type_fd >= 0, "checker fixture could not reopen root inode");
  const std::uint64_t root_inode_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
      eufs::ondisk::kBlockSize;
  eufs::ondisk::InodeBytes root_inode_bytes{};
  Require(pread(root_type_fd, root_inode_bytes.data(), root_inode_bytes.size(),
                static_cast<off_t>(root_inode_offset)) ==
              static_cast<ssize_t>(root_inode_bytes.size()),
          "checker fixture could not read root inode");
  eufs::ondisk::InodeRecord root_inode;
  Require(eufs::ondisk::DecodeInode(root_inode_bytes, superblock.root_inode,
                                    &root_inode, &detail),
          detail.c_str());
  root_inode.mode = S_IFREG | 0755;
  Require(eufs::ondisk::EncodeInode(root_inode, &root_inode_bytes, &detail),
          detail.c_str());
  Require(pwrite(root_type_fd, root_inode_bytes.data(), root_inode_bytes.size(),
                 static_cast<off_t>(root_inode_offset)) ==
              static_cast<ssize_t>(root_inode_bytes.size()),
          "checker fixture could not write non-directory root inode");
  Require(fdatasync(root_type_fd) == 0,
          "checker fixture could not sync non-directory root inode");
  close(root_type_fd);

  eufs::checker::ConsistencyReport non_directory_root;
  Require(eufs::checker::CheckImage(options.image_path,
                                       &non_directory_root, &detail) == 0,
          detail.c_str());
  Require(non_directory_root.status ==
              eufs::checker::ScanStatus::kPartial &&
              !non_directory_root.root_reachability_complete,
          "non-directory root incorrectly produced a complete namespace proof");
  Require(ContainsIssue(non_directory_root,
                        eufs::checker::IssueCode::kRootInodeNotDirectory),
          "checker did not report a non-directory root inode");
  Require(!ContainsIssue(non_directory_root,
                         eufs::checker::IssueCode::kInodeLinkCountMismatch),
          "checker derived link-count semantics for a non-directory root");

  unlink(options.image_path.c_str());
  std::cout << "PASS: checker root seed ignores a false-free inode bitmap\n";
  return 0;
}
