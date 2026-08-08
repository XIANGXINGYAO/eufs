#include "storage/mounted_image_session.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/file.h>
#include <unistd.h>

namespace eufs::storage {
namespace {

// 设置普通契约错误文本。
void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

// 把操作名称和 strerror(errno) 组合成可诊断系统错误。
void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

}  // namespace

// 主 fd 析构关闭时，内核同时释放该 open-file-description 持有的 flock。
MountedImageSession::~MountedImageSession() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

// 以在线写者身份打开并独占锁定镜像。
int MountedImageSession::Open(
    const std::string& image_path,
    std::unique_ptr<MountedImageSession>* output, std::string* detail) {
  if (image_path.empty() || output == nullptr) {
    SetDetail(detail, "image path and mounted session output are required");
    return -EINVAL;
  }
  // 先清空输出，保证失败不会留下旧 session。
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  // O_CLOEXEC 防止未来启动子进程时意外继承镜像写 fd。
  const int fd = open(image_path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    SetSystemDetail(detail, "open mounted image", errno);
    return -errno;
  }
  // 非阻塞独占 flock：已有在线写者时立即返回 EBUSY，不等待死锁。
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int error_number = errno;
    close(fd);
    SetSystemDetail(detail, "lock mounted image", error_number);
    return error_number == EWOULDBLOCK ? -EBUSY : -error_number;
  }

  // 打开和加锁都成功后才发布 session 所有权。
  output->reset(new MountedImageSession(fd));
  return 0;
}

// 为 reader 或 journal store 复制 fd，同时保持同一 flock 身份。
int MountedImageSession::DuplicateFd(int* output, std::string* detail) const {
  if (output == nullptr) {
    SetDetail(detail, "duplicate fd output is required");
    return -EINVAL;
  }
  // 先写 -1，失败时调用者不会误关其他有效 fd。
  *output = -1;
  if (detail != nullptr) {
    detail->clear();
  }

  // F_DUPFD_CLOEXEC 原子复制并设置 close-on-exec。
  const int duplicate = fcntl(fd_, F_DUPFD_CLOEXEC, 0);
  if (duplicate < 0) {
    SetSystemDetail(detail, "duplicate mounted image fd", errno);
    return -errno;
  }
  *output = duplicate;
  return 0;
}

}  // namespace eufs::storage
