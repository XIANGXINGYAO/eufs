// 验证 JournalControlStore 接管会话复制 fd 后仍处于同一个独占锁所有权范围。
// store 关闭派生 fd 时不能让另一个进程提前取得镜像写锁。
#include "journal/journal_control_store.h"
#include "storage/mkfs.h"
#include "storage/mounted_image_session.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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

std::string CreateImage() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-store-adoption-test-XXXXXX");
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
  return options.image_path;
}

std::string CreateMalformedImage() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(),
              "/tmp/eufs-store-adoption-corrupt-test-XXXXXX");
  const int fd = mkstemp(path_template.data());
  Require(fd >= 0, "malformed fixture mkstemp failed");
  close(fd);
  return path_template.data();
}

void RequireClosed(int fd, const char* message) {
  errno = 0;
  Require(fcntl(fd, F_GETFD) == -1 && errno == EBADF, message);
}

void TestSuccessfulAdoptionTransfersFdOwnership() {
  const std::string path = CreateImage();
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) ==
              0,
          detail.c_str());

  int adopted_fd = -1;
  Require(session->DuplicateFd(&adopted_fd, &detail) == 0, detail.c_str());
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  Require(eufs::journal::JournalControlStore::AdoptLockedFd(
              adopted_fd, &store, &detail) == 0,
          detail.c_str());
  Require(store->current().used_blocks == 0,
          "adopted Store did not load the clean control");

  store.reset();
  RequireClosed(adopted_fd, "Store destructor did not close its adopted fd");

  std::unique_ptr<eufs::journal::JournalControlStore> contender;
  Require(eufs::journal::JournalControlStore::Open(path, &contender, &detail) ==
              -EBUSY,
          "closing Store released the Session lifecycle lock");
  session.reset();
  Require(eufs::journal::JournalControlStore::Open(path, &contender, &detail) ==
              0,
          detail.c_str());
  contender.reset();
  unlink(path.c_str());
}

void TestValidationFailureConsumesDuplicate() {
  const std::string path = CreateMalformedImage();
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) ==
              0,
          detail.c_str());

  int adopted_fd = -1;
  Require(session->DuplicateFd(&adopted_fd, &detail) == 0, detail.c_str());
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  Require(eufs::journal::JournalControlStore::AdoptLockedFd(
              adopted_fd, &store, &detail) == -EUCLEAN,
          "malformed adopted image was accepted");
  Require(store == nullptr,
          "failed adopted Store validation returned an output object");
  RequireClosed(adopted_fd,
                "validation failure leaked the consumed duplicate fd");

  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              -EBUSY,
          "failed Store adoption released the Session root lock");
  session.reset();
  unlink(path.c_str());
}

void TestInvalidArgumentsStillConsumeValidFd() {
  const std::string path = CreateMalformedImage();
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "invalid-argument fixture open failed");
  std::string detail;
  Require(eufs::journal::JournalControlStore::AdoptLockedFd(
              fd, nullptr, &detail) == -EINVAL,
          "null Store output was accepted");
  RequireClosed(fd, "invalid argument path did not consume the adopted fd");
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestSuccessfulAdoptionTransfersFdOwnership();
  TestValidationFailureConsumesDuplicate();
  TestInvalidArgumentsStillConsumeValidFd();
  std::cout << "PASS: mounted image Store adoption tests\n";
  return 0;
}
