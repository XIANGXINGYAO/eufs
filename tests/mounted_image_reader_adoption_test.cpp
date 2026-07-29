#include "storage/image_reader.h"
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

std::string TemporaryPath(const char* pattern) {
  std::array<char, 80> path{};
  std::strncpy(path.data(), pattern, path.size() - 1U);
  const int fd = mkstemp(path.data());
  Require(fd >= 0, "mkstemp failed");
  close(fd);
  return path.data();
}

std::string CreateImage() {
  const std::string path =
      TemporaryPath("/tmp/eufs-reader-adoption-test-XXXXXX");
  unlink(path.c_str());

  eufs::storage::MkfsOptions options;
  options.image_path = path;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail), detail.c_str());
  return path;
}

void RequireClosed(int fd, const char* message) {
  errno = 0;
  Require(fcntl(fd, F_GETFD) == -1 && errno == EBADF, message);
}

void TestSuccessfulAdoptionKeepsSessionLock() {
  const std::string path = CreateImage();
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) == 0,
          detail.c_str());

  int adopted_fd = -1;
  Require(session->DuplicateFd(&adopted_fd, &detail) == 0, detail.c_str());
  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::AdoptLockedFd(adopted_fd, &reader,
                                                    &detail) == 0,
          detail.c_str());
  Require(reader->superblock().root_inode == 1,
          "adopted Reader did not load the formatted image");

  reader.reset();
  RequireClosed(adopted_fd, "Reader destructor did not close its adopted fd");

  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              -EBUSY,
          "closing Reader released the Session lifecycle lock");
  session.reset();
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              0,
          detail.c_str());
  contender.reset();
  unlink(path.c_str());
}

void TestValidationFailureConsumesDuplicate() {
  const std::string path =
      TemporaryPath("/tmp/eufs-reader-adoption-corrupt-test-XXXXXX");
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) == 0,
          detail.c_str());

  int adopted_fd = -1;
  Require(session->DuplicateFd(&adopted_fd, &detail) == 0, detail.c_str());
  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::AdoptLockedFd(adopted_fd, &reader,
                                                    &detail) == -EUCLEAN,
          "malformed adopted image was accepted");
  Require(reader == nullptr,
          "failed Reader validation returned an output object");
  RequireClosed(adopted_fd,
                "Reader validation failure leaked the consumed duplicate fd");

  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              -EBUSY,
          "failed Reader adoption released the Session lifecycle lock");
  session.reset();
  unlink(path.c_str());
}

void TestInvalidArgumentsConsumeValidFd() {
  const std::string path =
      TemporaryPath("/tmp/eufs-reader-adoption-invalid-test-XXXXXX");
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "invalid-argument fixture open failed");
  std::string detail;
  Require(eufs::storage::ImageReader::AdoptLockedFd(fd, nullptr, &detail) ==
              -EINVAL,
          "null Reader output was accepted");
  RequireClosed(fd, "invalid argument path did not consume the adopted fd");
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestSuccessfulAdoptionKeepsSessionLock();
  TestValidationFailureConsumesDuplicate();
  TestInvalidArgumentsConsumeValidFd();
  std::cout << "PASS: mounted image Reader adoption tests\n";
  return 0;
}
