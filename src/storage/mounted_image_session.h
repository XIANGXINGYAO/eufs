#pragma once

#include <memory>
#include <string>

namespace eufs::storage {

// 一次在线挂载或对象 Backend 对镜像的唯一写者会话。
// Open 使用 O_RDWR 打开并取得非阻塞独占 flock；对象析构才释放 fd/锁。
class MountedImageSession {
 public:
  // 关闭持有锁的主 fd。
  ~MountedImageSession();

  // 唯一锁所有权禁止复制。
  MountedImageSession(const MountedImageSession&) = delete;
  MountedImageSession& operator=(const MountedImageSession&) = delete;

  // 成功返回持有独占锁的 session；镜像已被另一会话占用时返回 EBUSY。
  static int Open(const std::string& image_path,
                  std::unique_ptr<MountedImageSession>* output,
                  std::string* detail);

  // 复制 fd 给 reader/journal store 使用；复制描述符共享同一 flock 身份。
  int DuplicateFd(int* output, std::string* detail) const;

 private:
  // 只有 Open 成功取得 fd 和锁后才能构造。
  explicit MountedImageSession(int fd) : fd_(fd) {}

  // 持有整个会话生命周期的主文件描述符。
  int fd_;
};

}  // namespace eufs::storage
