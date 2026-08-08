// 验证挂载状态按“独占会话 -> 启动恢复 -> 只读解析器”顺序建立。
// 恢复失败或锁冲突必须发生在进入 FUSE 事件循环之前。
#include "fuse/mount_state.h"
#include "metadata/ondisk_format.h"
#include "storage/mkfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string TemporaryPath(const char* pattern) {
  std::array<char, 96> path{};
  std::strncpy(path.data(), pattern, path.size() - 1U);
  const int fd = mkstemp(path.data());
  Require(fd >= 0, "mkstemp failed");
  close(fd);
  return path.data();
}

struct ImageFixture {
  std::string path;
  eufs::ondisk::Superblock superblock;
};

ImageFixture CreateImage(const char* pattern) {
  ImageFixture fixture;
  fixture.path = TemporaryPath(pattern);
  unlink(fixture.path.c_str());

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

bool PreadAll(int fd, std::uint8_t* output, std::size_t size,
              std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
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

bool PwriteAll(int fd, const std::uint8_t* input, std::size_t size,
               std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pwrite(fd, input + completed, size - completed,
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

eufs::ondisk::Block ReadBlock(const std::string& path,
                              std::uint32_t block_number) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "open image for block read failed");
  eufs::ondisk::Block block{};
  Require(PreadAll(fd, block.data(), block.size(),
                   static_cast<std::uint64_t>(block_number) *
                       eufs::ondisk::kBlockSize),
          "pread image block failed");
  close(fd);
  return block;
}

void WriteBlock(const std::string& path, std::uint32_t block_number,
                const eufs::ondisk::Block& block) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "open image for block write failed");
  Require(PwriteAll(fd, block.data(), block.size(),
                    static_cast<std::uint64_t>(block_number) *
                        eufs::ondisk::kBlockSize),
          "pwrite image block failed");
  Require(fdatasync(fd) == 0, "fdatasync image block failed");
  close(fd);
}

struct RecoveryFixture {
  ImageFixture image;
  std::uint64_t old_mtime_ns{0};
  std::uint64_t new_mtime_ns{0};
};

RecoveryFixture CreateRecoveryFixture(bool committed) {
  RecoveryFixture fixture;
  fixture.image =
      CreateImage("/tmp/eufs-stage-c-recovery-startup-XXXXXX");

  auto inode_table = ReadBlock(fixture.image.path,
                               fixture.image.superblock.inode_table.start_block);
  eufs::ondisk::InodeBytes root_bytes{};
  std::copy_n(inode_table.begin(), root_bytes.size(), root_bytes.begin());
  eufs::ondisk::InodeRecord root;
  std::string detail;
  Require(eufs::ondisk::DecodeInode(root_bytes, 1, &root, &detail),
          detail.c_str());
  fixture.old_mtime_ns = root.mtime_ns;
  fixture.new_mtime_ns = root.mtime_ns + 1000000ULL;
  root.mtime_ns = fixture.new_mtime_ns;
  Require(eufs::ondisk::EncodeInode(root, &root_bytes, &detail),
          detail.c_str());
  std::copy(root_bytes.begin(), root_bytes.end(), inode_table.begin());

  std::unique_ptr<eufs::journal::JournalControlStore> store;
  Require(eufs::journal::JournalControlStore::Open(
              fixture.image.path, &store, &detail) == 0,
          detail.c_str());
  std::map<std::uint32_t, eufs::ondisk::Block> metadata_after_images{
      {fixture.image.superblock.inode_table.start_block, inode_table}};
  eufs::journal::RingReservationPlan reservation;
  Require(eufs::journal::PlanRingReservation(
              store->current(), metadata_after_images.size(), &reservation,
              &detail) == 0,
          detail.c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, metadata_after_images, &body,
                                    &detail) == 0,
          detail.c_str());
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  if (committed) {
    Require(store->WriteCommit(&detail) == 0, detail.c_str());
  }
  return fixture;
}

std::uint64_t RootMtime(const eufs::fuse_adapter::FuseMountState& state) {
  eufs::ondisk::InodeRecord root;
  std::string detail;
  Require(state.reader->ReadInode(1, &root, &detail) == 0, detail.c_str());
  return root.mtime_ns;
}

void RequireSessionLock(const std::string& path) {
  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              -EBUSY,
          "successful mount state did not retain the Session lock");
}

void RequireReleasedLock(const std::string& path) {
  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              0,
          detail.c_str());
}

class SyncUnknownIo final : public eufs::journal::JournalControlIo {
 public:
  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    return pwrite(fd, input, size, offset);
  }

  int Fdatasync(int fd) override {
    if (fdatasync(fd) != 0) {
      return -1;
    }
    errno = EIO;
    return -1;
  }
};

void TestEmptyStartupLoadsReaderAndRetainsLock() {
  auto fixture = CreateImage("/tmp/eufs-stage-c-empty-startup-XXXXXX");
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action =
      eufs::journal::RecoveryAction::kDiscarded;
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.path, &state, &action, &detail) == 0,
          detail.c_str());
  Require(action == eufs::journal::RecoveryAction::kNoAction &&
              state != nullptr && state->reader != nullptr &&
              state->session != nullptr,
          "empty startup did not produce a complete mount state");
  RequireSessionLock(fixture.path);
  state.reset();
  RequireReleasedLock(fixture.path);
  unlink(fixture.path.c_str());
}

void TestUncommittedStartupDiscardsBeforeReaderLoad() {
  auto fixture = CreateRecoveryFixture(false);
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.image.path, &state, &action, &detail) == 0,
          detail.c_str());
  Require(action == eufs::journal::RecoveryAction::kDiscarded &&
              RootMtime(*state) == fixture.old_mtime_ns &&
              state->reader->journal_control().used_blocks == 0,
          "post-discard Reader did not load the old home state");
  state.reset();
  unlink(fixture.image.path.c_str());
}

void TestCommittedStartupReplaysBeforeReaderLoad() {
  auto fixture = CreateRecoveryFixture(true);
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.image.path, &state, &action, &detail) == 0,
          detail.c_str());
  Require(action ==
                  eufs::journal::RecoveryAction::kReplayedAndCheckpointed &&
              RootMtime(*state) == fixture.new_mtime_ns &&
              state->reader->journal_control().used_blocks == 0,
          "post-replay Reader did not load the committed home state");
  state.reset();
  unlink(fixture.image.path.c_str());
}

void TestBusyImageNeverProducesState() {
  auto fixture = CreateImage("/tmp/eufs-stage-c-busy-startup-XXXXXX");
  std::unique_ptr<eufs::storage::MountedImageSession> owner;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(fixture.path, &owner,
                                                   &detail) == 0,
          detail.c_str());
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.path, &state, &action, &detail) == -EBUSY &&
              state == nullptr,
          "busy image produced a mountable state");
  owner.reset();
  unlink(fixture.path.c_str());
}

void TestCorruptImageNeverProducesState() {
  const std::string path =
      TemporaryPath("/tmp/eufs-stage-c-corrupt-startup-XXXXXX");
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(path, &state, &action, &detail) ==
                  -EUCLEAN &&
              state == nullptr,
          "corrupt image produced a mountable state");
  RequireReleasedLock(path);
  unlink(path.c_str());
}

void TestRecoverySyncUncertaintyNeverProducesState() {
  auto fixture = CreateRecoveryFixture(false);
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  auto io = std::make_shared<SyncUnknownIo>();
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.image.path, &state, &action, &detail, io) == -EIO &&
              state == nullptr,
          "recovery sync uncertainty produced a mountable state");
  RequireReleasedLock(fixture.image.path);
  unlink(fixture.image.path.c_str());
}

void TestPostRecoveryReaderFailureNeverProducesState() {
  auto fixture =
      CreateImage("/tmp/eufs-stage-c-reader-failure-startup-XXXXXX");
  auto inode_bitmap = ReadBlock(
      fixture.path, fixture.superblock.inode_bitmap.start_block);
  inode_bitmap[0] &= static_cast<std::uint8_t>(~1U);
  WriteBlock(fixture.path, fixture.superblock.inode_bitmap.start_block,
             inode_bitmap);

  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.path, &state, &action, &detail) == -EUCLEAN &&
              state == nullptr,
          "invalid post-recovery Reader produced a mountable state");
  RequireReleasedLock(fixture.path);
  unlink(fixture.path.c_str());
}

void TestRuntimeReloadFailureLatchesFailClosedState() {
  auto fixture =
      CreateImage("/tmp/eufs-stage-c-reload-failure-startup-XXXXXX");
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::fuse_adapter::OpenFuseMountState(
              fixture.path, &state, &action, &detail) == 0,
          detail.c_str());

  // flock is advisory: this deliberately simulates an external writer that
  // ignores the eufs locking protocol and invalidates the next Reader load.
  auto inode_bitmap = ReadBlock(
      fixture.path, fixture.superblock.inode_bitmap.start_block);
  inode_bitmap[0] &= static_cast<std::uint8_t>(~1U);
  WriteBlock(fixture.path, fixture.superblock.inode_bitmap.start_block,
             inode_bitmap);

  Require(state->ReloadReader(&detail) == -EUCLEAN && !state->usable() &&
              state->reader == nullptr && state->fatal_error() == -EUCLEAN,
          "runtime Reader reload failure did not latch fail closed");
  Require(state->ReloadReader(&detail) == -EIO && state->reader == nullptr,
          "fail-closed mount state allowed a later Reader reload");
  RequireSessionLock(fixture.path);
  state.reset();
  unlink(fixture.path.c_str());
}

}  // namespace

int main() {
  TestEmptyStartupLoadsReaderAndRetainsLock();
  TestUncommittedStartupDiscardsBeforeReaderLoad();
  TestCommittedStartupReplaysBeforeReaderLoad();
  TestBusyImageNeverProducesState();
  TestCorruptImageNeverProducesState();
  TestRecoverySyncUncertaintyNeverProducesState();
  TestPostRecoveryReaderFailureNeverProducesState();
  TestRuntimeReloadFailureLatchesFailClosedState();
  std::cout << "PASS: FUSE mount-state startup recovery matrix\n";
  return 0;
}
