#include "storage/mounted_image_session.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string CreateTemporaryImage() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-mounted-session-test-XXXXXX");
  const int fd = mkstemp(path_template.data());
  Require(fd >= 0, "mkstemp failed");
  close(fd);
  return path_template.data();
}

bool LockWouldBlock(int fd) {
  errno = 0;
  const int result = flock(fd, LOCK_EX | LOCK_NB);
  return result != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
}

void TestSessionAndDuplicateHoldOneLifecycleLock() {
  const std::string path = CreateTemporaryImage();
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) ==
              0,
          detail.c_str());

  const int contender = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(contender >= 0, "independent contender open failed");
  Require(LockWouldBlock(contender),
          "independent open bypassed the mounted session lock");

  int duplicate = -1;
  Require(session->DuplicateFd(&duplicate, &detail) == 0, detail.c_str());
  Require(duplicate >= 0, "session returned an invalid duplicate fd");
  const int descriptor_flags = fcntl(duplicate, F_GETFD);
  Require(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0,
          "session duplicate does not have FD_CLOEXEC");

  session.reset();
  Require(LockWouldBlock(contender),
          "closing the root fd released a lock still held by its duplicate");

  close(duplicate);
  Require(flock(contender, LOCK_EX | LOCK_NB) == 0,
          "closing the final duplicate did not release the lifecycle lock");
  close(contender);
  unlink(path.c_str());
}

void TestSecondSessionIsRejectedUntilLifecycleEnds() {
  const std::string path = CreateTemporaryImage();
  std::unique_ptr<eufs::storage::MountedImageSession> first;
  std::unique_ptr<eufs::storage::MountedImageSession> second;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &first, &detail) == 0,
          detail.c_str());
  Require(eufs::storage::MountedImageSession::Open(path, &second, &detail) ==
              -EBUSY,
          "a second mounted session acquired the same image");
  Require(second == nullptr,
          "failed mounted session open returned an output object");

  first.reset();
  Require(eufs::storage::MountedImageSession::Open(path, &second, &detail) == 0,
          detail.c_str());
  second.reset();
  unlink(path.c_str());
}

void TestInvalidArgumentsFailWithoutOutputs() {
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open("", &session, &detail) ==
              -EINVAL,
          "empty mounted image path was accepted");
  Require(session == nullptr, "invalid open returned a mounted session");

  const std::string path = CreateTemporaryImage();
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) ==
              0,
          detail.c_str());
  Require(session->DuplicateFd(nullptr, &detail) == -EINVAL,
          "null duplicate output was accepted");
  session.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestSessionAndDuplicateHoldOneLifecycleLock();
  TestSecondSessionIsRejectedUntilLifecycleEnds();
  TestInvalidArgumentsFailWithoutOutputs();
  std::cout << "PASS: mounted image session tests\n";
  return 0;
}
