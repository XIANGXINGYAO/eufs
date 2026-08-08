// 验证 eufsck 命令层把扫描结论映射为稳定的输出、退出码和挂载拒绝语义。
// 它测试 CLI 契约，不重复测试底层每一种损坏识别算法。
#include "checker/eufsck_command.h"
#include "journal/ondisk_journal.h"
#include "metadata/ondisk_format.h"
#include "storage/mkfs.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct ImageFixture {
  std::string path;
  eufs::ondisk::Superblock superblock;

  ImageFixture() = default;
  ImageFixture(const ImageFixture&) = delete;
  ImageFixture& operator=(const ImageFixture&) = delete;
  ImageFixture(ImageFixture&& other) noexcept
      : path(std::move(other.path)), superblock(other.superblock) {
    other.path.clear();
  }
  ImageFixture& operator=(ImageFixture&&) = delete;

  ~ImageFixture() {
    if (!path.empty()) {
      unlink(path.c_str());
    }
  }
};

ImageFixture CreateImage() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufsck-command-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  ImageFixture fixture;
  fixture.path = path_template.data();
  eufs::storage::MkfsOptions options;
  options.image_path = fixture.path;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, &fixture.superblock, &detail),
          detail.c_str());
  return fixture;
}

int Run(const std::vector<std::string>& arguments, std::string* output,
        std::string* error) {
  std::ostringstream output_stream;
  std::ostringstream error_stream;
  const int result =
      eufs::checker::RunEufsck(arguments, output_stream, error_stream);
  *output = output_stream.str();
  *error = error_stream.str();
  return result;
}

void ClearRootInodeBitmapBit(const ImageFixture& fixture) {
  const int fd = open(fixture.path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "could not open inode bitmap fixture");
  const std::uint64_t offset =
      static_cast<std::uint64_t>(fixture.superblock.inode_bitmap.start_block) *
      eufs::ondisk::kBlockSize;
  std::uint8_t byte = 0;
  Require(pread(fd, &byte, 1, static_cast<off_t>(offset)) == 1,
          "could not read inode bitmap byte");
  byte &= static_cast<std::uint8_t>(~1U);
  Require(pwrite(fd, &byte, 1, static_cast<off_t>(offset)) == 1,
          "could not clear root inode bitmap bit");
  Require(fdatasync(fd) == 0, "could not sync inode bitmap corruption");
  close(fd);
}

void CorruptRootInodeRecord(const ImageFixture& fixture) {
  const int fd = open(fixture.path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "could not open root inode fixture");
  const std::uint64_t root_inode_offset =
      static_cast<std::uint64_t>(fixture.superblock.inode_table.start_block) *
      eufs::ondisk::kBlockSize;
  eufs::ondisk::InodeBytes inode_bytes{};
  Require(pread(fd, inode_bytes.data(), inode_bytes.size(),
                static_cast<off_t>(root_inode_offset)) ==
              static_cast<ssize_t>(inode_bytes.size()),
          "could not read root inode");
  inode_bytes[0] ^= 1U;
  Require(pwrite(fd, inode_bytes.data(), inode_bytes.size(),
                 static_cast<off_t>(root_inode_offset)) ==
              static_cast<ssize_t>(inode_bytes.size()),
          "could not corrupt root inode checksum");
  Require(fdatasync(fd) == 0, "could not sync root inode corruption");
  close(fd);
}

void ExposeJournalTransaction(const ImageFixture& fixture) {
  const int fd = open(fixture.path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "could not open journal control fixture");
  const std::uint64_t control_a_offset =
      static_cast<std::uint64_t>(fixture.superblock.journal.start_block) *
      eufs::ondisk::kBlockSize;
  eufs::ondisk::Block control_bytes{};
  Require(pread(fd, control_bytes.data(), control_bytes.size(),
                static_cast<off_t>(control_a_offset)) ==
              static_cast<ssize_t>(control_bytes.size()),
          "could not read journal control");
  eufs::journal::JournalControl control;
  std::string detail;
  Require(eufs::journal::DecodeControl(control_bytes, &control, &detail),
          detail.c_str());
  ++control.generation;
  control.tail = control.head;
  control.used_blocks = eufs::ondisk::kMinimumJournalRingBlocks;
  control.head = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(control.tail) + control.used_blocks) %
      control.ring_blocks);
  ++control.next_transaction_id;
  Require(eufs::journal::EncodeControl(control, &control_bytes, nullptr,
                                       &detail),
          detail.c_str());
  Require(pwrite(fd, control_bytes.data(), control_bytes.size(),
                 static_cast<off_t>(control_a_offset)) ==
                  static_cast<ssize_t>(control_bytes.size()) &&
              pwrite(fd, control_bytes.data(), control_bytes.size(),
                     static_cast<off_t>(control_a_offset +
                                        eufs::ondisk::kBlockSize)) ==
                  static_cast<ssize_t>(control_bytes.size()),
          "could not write exposed journal controls");
  Require(fdatasync(fd) == 0, "could not sync exposed journal controls");
  close(fd);
}

}  // namespace

int main() {
  std::string output;
  std::string error;

  ImageFixture healthy = CreateImage();
  Require(Run({healthy.path}, &output, &error) ==
              eufs::checker::kEufsckExitHealthy,
          "healthy image did not return exit 0");
  Require(error.empty() &&
              output.find("结论: 当前检查规则内健康") != std::string::npos &&
              output.find("问题=0") != std::string::npos,
          "healthy human report is incorrect");

  Require(Run({"--json", healthy.path}, &output, &error) ==
              eufs::checker::kEufsckExitHealthy,
          "healthy JSON image did not return exit 0");
  Require(error.empty() &&
              output.find("\"schema_version\":1") != std::string::npos &&
              output.find("\"verdict\":\"healthy\"") !=
                  std::string::npos &&
              output.find("\"issues\":[]") != std::string::npos,
          "healthy JSON report does not satisfy the schema contract");

  ImageFixture inconsistent = CreateImage();
  ClearRootInodeBitmapBit(inconsistent);
  Require(Run({"--json", inconsistent.path}, &output, &error) ==
              eufs::checker::kEufsckExitInconsistent,
          "complete bitmap contradiction did not return exit 1");
  Require(output.find("\"verdict\":\"inconsistent\"") !=
                  std::string::npos &&
              output.find("ROOT_INODE_MARKED_FREE") != std::string::npos,
          "inconsistent JSON report lost the root bitmap evidence");

  ImageFixture incomplete = CreateImage();
  CorruptRootInodeRecord(incomplete);
  Require(Run({incomplete.path}, &output, &error) ==
              eufs::checker::kEufsckExitIncomplete,
          "undecodable root inode did not return exit 2");
  Require(output.find("扫描不完整") != std::string::npos &&
              output.find("ROOT_INODE_UNDECODABLE") !=
                  std::string::npos,
          "incomplete report lost the root decode evidence");

  ImageFixture recovery_required = CreateImage();
  ExposeJournalTransaction(recovery_required);
  Require(Run({"--json", recovery_required.path}, &output, &error) ==
              eufs::checker::kEufsckExitIncomplete,
          "nonempty journal did not return exit 2");
  Require(error.empty() &&
              output.find("\"scan_status\":\"aborted\"") !=
                  std::string::npos &&
              output.find("JOURNAL_RECOVERY_REQUIRED") !=
                  std::string::npos &&
              output.find("\"physical_scan_inodes\":0") !=
                  std::string::npos,
          "journal recovery gate did not abort before metadata scanning");

  ImageFixture busy = CreateImage();
  const int busy_fd = open(busy.path.c_str(), O_RDWR | O_CLOEXEC);
  Require(busy_fd >= 0 && flock(busy_fd, LOCK_EX | LOCK_NB) == 0,
          "could not hold exclusive image lock");
  Require(Run({busy.path}, &output, &error) ==
              eufs::checker::kEufsckExitRuntimeError,
          "locked image did not return exit 3");
  Require(output.empty() && error.find("errno 16") != std::string::npos,
          "locked image runtime error is incorrect");
  close(busy_fd);

  Require(Run({"/tmp/eufsck-image-does-not-exist"}, &output, &error) ==
              eufs::checker::kEufsckExitRuntimeError,
          "missing image did not return exit 3");
  Require(output.empty() && !error.empty(),
          "missing image error was written to the wrong stream");

  Require(Run({}, &output, &error) == eufs::checker::kEufsckExitUsage,
          "missing arguments did not return EX_USAGE");
  Require(output.empty() && error.find("用法: eufsck") != std::string::npos,
          "usage error did not print Chinese help");

  std::cout << "PASS: eufsck command and exit-code contract\n";
  return 0;
}
