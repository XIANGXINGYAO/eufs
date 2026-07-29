#include "storage/mounted_image_session.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/file.h>
#include <unistd.h>

namespace eufs::storage {
namespace {

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

}  // namespace

MountedImageSession::~MountedImageSession() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

int MountedImageSession::Open(
    const std::string& image_path,
    std::unique_ptr<MountedImageSession>* output, std::string* detail) {
  if (image_path.empty() || output == nullptr) {
    SetDetail(detail, "image path and mounted session output are required");
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  const int fd = open(image_path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    SetSystemDetail(detail, "open mounted image", errno);
    return -errno;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int error_number = errno;
    close(fd);
    SetSystemDetail(detail, "lock mounted image", error_number);
    return error_number == EWOULDBLOCK ? -EBUSY : -error_number;
  }

  output->reset(new MountedImageSession(fd));
  return 0;
}

int MountedImageSession::DuplicateFd(int* output, std::string* detail) const {
  if (output == nullptr) {
    SetDetail(detail, "duplicate fd output is required");
    return -EINVAL;
  }
  *output = -1;
  if (detail != nullptr) {
    detail->clear();
  }

  const int duplicate = fcntl(fd_, F_DUPFD_CLOEXEC, 0);
  if (duplicate < 0) {
    SetSystemDetail(detail, "duplicate mounted image fd", errno);
    return -errno;
  }
  *output = duplicate;
  return 0;
}

}  // namespace eufs::storage
