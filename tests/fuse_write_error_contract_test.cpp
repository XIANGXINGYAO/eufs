#define FUSE_USE_VERSION 31

// 从 FUSE 回调层验证写失败返回负 errno，并在持久化边界不确定时关闭后续写服务。
// 同时检查错误路径不会把部分修改伪装成一次成功 write。
#include "checker/consistency_checker.h"
#include "fuse/operations.h"
#include "fuse/mount_state.h"
#include "metadata/ondisk_format.h"
#include "storage/mkfs.h"

#include <array>
#include <cerrno>
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

fuse_context g_fuse_context{};

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
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

std::string CreateImage() {
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-write-errors-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 1ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());
  return options.image_path;
}

std::unique_ptr<eufs::fuse_adapter::FuseMountState> OpenState(
    const std::string& path) {
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(
              path, &state, &action, &detail) == 0 &&
              action == eufs::journal::RecoveryAction::kNoAction,
          detail.c_str());
  return state;
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
          "write error contract left an inconsistent image");
}

void RequireFileState(const eufs::fuse_adapter::FuseMountState& state,
                      std::uint32_t inode_number, std::uint64_t expected_size,
                      char expected_first_byte) {
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(state.reader->ReadInode(inode_number, &inode, &detail) == 0,
          detail.c_str());
  std::uint8_t first_byte = 0;
  std::size_t bytes_read = 0;
  Require(inode.size == expected_size &&
              state.reader->ReadFile(inode_number, 0, &first_byte, 1,
                                     &bytes_read, &detail) == 0 &&
              bytes_read == 1 && first_byte == expected_first_byte,
          "file state does not match the completed callbacks");
}

void TestWriteErrorsAreNonFatalAndNonMutating() {
  const std::string path = CreateImage();
  auto state = OpenState(path);
  g_fuse_context.private_data = state.get();
  g_fuse_context.uid = getuid();
  g_fuse_context.gid = getgid();

  const fuse_operations operations =
      eufs::fuse_adapter::MakeOperations();
  Require(operations.create != nullptr && operations.write != nullptr,
          "production FUSE create/write callbacks are missing");

  fuse_file_info file_info{};
  file_info.flags = O_WRONLY;
  Require(operations.create("/a.txt", 0644, &file_info) == 0 &&
              file_info.fh != 0,
          "production FUSE create callback failed");

  const std::string block(eufs::ondisk::kBlockSize, 'A');
  Require(operations.write("/a.txt", block.data(), block.size(), 0,
                           &file_info) ==
              static_cast<int>(block.size()),
          "initial production FUSE write failed");

  const auto before_efbig = ReadWholeImage(path);
  Require(operations.write(
              "/a.txt", "X", 1,
              static_cast<off_t>(eufs::ondisk::kMaxFileSize),
              &file_info) == -EFBIG,
          "maximum-file write did not return EFBIG");
  Require(state->usable() && ReadWholeImage(path) == before_efbig,
          "EFBIG changed the image or poisoned the mounted state");
  RequireFileState(*state, static_cast<std::uint32_t>(file_info.fh),
                   eufs::ondisk::kBlockSize, 'A');

  std::uint32_t existing_blocks = 1;
  std::vector<std::uint8_t> before_enospc;
  for (;; ++existing_blocks) {
    Require(existing_blocks < eufs::ondisk::kMaxFileSize /
                                  eufs::ondisk::kBlockSize,
            "test reached the format limit before physical ENOSPC");
    before_enospc = ReadWholeImage(path);
    const int result = operations.write(
        "/a.txt", block.data(), block.size(),
        static_cast<off_t>(
            static_cast<std::uint64_t>(existing_blocks) *
            eufs::ondisk::kBlockSize),
        &file_info);
    if (result == -ENOSPC) {
      break;
    }
    Require(result == static_cast<int>(block.size()),
            "fill callback returned neither a complete write nor ENOSPC");
  }

  const std::uint64_t full_size =
      static_cast<std::uint64_t>(existing_blocks) *
      eufs::ondisk::kBlockSize;
  Require(existing_blocks > eufs::ondisk::kDirectBlockCount &&
              state->usable() && ReadWholeImage(path) == before_enospc,
          "ENOSPC changed the image or poisoned the mounted state");
  RequireFileState(*state, static_cast<std::uint32_t>(file_info.fh),
                   full_size, 'A');

  Require(operations.write("/a.txt", "Z", 1, 0, &file_info) == 1 &&
              state->usable(),
          "valid direct COW overwrite failed after ENOSPC");
  RequireFileState(*state, static_cast<std::uint32_t>(file_info.fh),
                   full_size, 'Z');

  g_fuse_context.private_data = nullptr;
  state.reset();
  RequireHealthy(path);

  state = OpenState(path);
  RequireFileState(*state, static_cast<std::uint32_t>(file_info.fh),
                   full_size, 'Z');
  state.reset();

  std::cout << "full_blocks=" << existing_blocks
            << " full_size=" << full_size << '\n';
  unlink(path.c_str());
}

}  // namespace

// The production callbacks obtain FuseMountState through this libfuse boundary.
// Owning it here keeps the callback test runnable on CI hosts without /dev/fuse.
extern "C" struct fuse_context* fuse_get_context() {
  return &g_fuse_context;
}

int main() {
  TestWriteErrorsAreNonFatalAndNonMutating();
  std::cout << "PASS: FUSE write EFBIG/ENOSPC error contract\n";
  return 0;
}
