#include "metadata/empty_file_create_plan.h"
#include "metadata/first_block_write_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "storage/mounted_image_session.h"
#include "storage/writable_image.h"

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

void RequireClosed(int fd, const char* message) {
  errno = 0;
  Require(fcntl(fd, F_GETFD) == -1 && errno == EBADF, message);
}

std::string CreateImage() {
  char path_template[] = "/tmp/eufs-mutation-adoption-test-XXXXXX";
  const int temporary_fd = mkstemp(path_template);
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template);

  eufs::storage::MkfsOptions options;
  options.image_path = path_template;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());
  return options.image_path;
}

std::unique_ptr<eufs::storage::ImageReader> AdoptReader(
    eufs::storage::MountedImageSession* session) {
  int reader_fd = -1;
  std::string detail;
  Require(session->DuplicateFd(&reader_fd, &detail) == 0, detail.c_str());
  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::AdoptLockedFd(reader_fd, &reader,
                                                    &detail) == 0,
          detail.c_str());
  return reader;
}

void TestCreateAndWriteConsumeDuplicatesButKeepSessionLock() {
  const std::string path = CreateImage();
  std::unique_ptr<eufs::storage::ImageReader> planning_reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &planning_reader, &detail) ==
              0,
          detail.c_str());
  eufs::metadata::EmptyFileCreatePlan create_plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *planning_reader, "a.txt", 0644, 1000, 1000, 123456789ULL,
              &create_plan, &detail) == 0,
          detail.c_str());
  planning_reader.reset();

  std::unique_ptr<eufs::storage::MountedImageSession> session;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) ==
              0,
          detail.c_str());

  int mutation_fd = -1;
  Require(session->DuplicateFd(&mutation_fd, &detail) == 0, detail.c_str());
  Require(eufs::storage::ApplyCreatePlanOnLockedFd(
              mutation_fd, create_plan, &detail) == 0,
          detail.c_str());
  RequireClosed(mutation_fd, "create applier did not consume its duplicate");

  auto reader = AdoptReader(session.get());
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  Require(reader->ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0 &&
              inode_number == create_plan.inode_number,
          "adopted reader cannot see the created file");

  eufs::metadata::FirstBlockWritePlan write_plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, inode_number, "hello", 223456789ULL, &write_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();

  mutation_fd = -1;
  Require(session->DuplicateFd(&mutation_fd, &detail) == 0, detail.c_str());
  Require(eufs::storage::ApplyFirstBlockWritePlanOnLockedFd(
              mutation_fd, write_plan, &detail) == 0,
          detail.c_str());
  RequireClosed(mutation_fd, "write applier did not consume its duplicate");

  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              -EBUSY,
          "mutation duplicate destruction released the Session root lock");

  reader = AdoptReader(session.get());
  std::uint8_t output[5]{};
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(inode_number, 0, output, sizeof(output), &bytes_read,
                           &detail) == 0 &&
              bytes_read == sizeof(output) &&
              std::memcmp(output, "hello", sizeof(output)) == 0,
          "adopted mutation path did not persist hello");
  reader.reset();
  session.reset();

  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          "final Session duplicate did not release the lifecycle lock");
  reader.reset();
  unlink(path.c_str());
}

void TestValidationFailureConsumesDuplicateAndKeepsSessionLock() {
  const std::string path = CreateImage();
  std::unique_ptr<eufs::storage::MountedImageSession> session;
  std::string detail;
  Require(eufs::storage::MountedImageSession::Open(path, &session, &detail) ==
              0,
          detail.c_str());

  int mutation_fd = -1;
  Require(session->DuplicateFd(&mutation_fd, &detail) == 0, detail.c_str());
  eufs::metadata::EmptyFileCreatePlan invalid_plan;
  Require(eufs::storage::ApplyCreatePlanOnLockedFd(
              mutation_fd, invalid_plan, &detail) == -EINVAL,
          "invalid adopted mutation plan was accepted");
  RequireClosed(mutation_fd,
                "mutation validation failure leaked the consumed duplicate");

  std::unique_ptr<eufs::storage::MountedImageSession> contender;
  Require(eufs::storage::MountedImageSession::Open(path, &contender, &detail) ==
              -EBUSY,
          "mutation validation failure released the Session root lock");
  session.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestCreateAndWriteConsumeDuplicatesButKeepSessionLock();
  TestValidationFailureConsumesDuplicateAndKeepsSessionLock();
  std::cout << "PASS: mounted image mutation adoption tests\n";
  return 0;
}
