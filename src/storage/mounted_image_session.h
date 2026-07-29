#pragma once

#include <memory>
#include <string>

namespace eufs::storage {

class MountedImageSession {
 public:
  ~MountedImageSession();

  MountedImageSession(const MountedImageSession&) = delete;
  MountedImageSession& operator=(const MountedImageSession&) = delete;

  static int Open(const std::string& image_path,
                  std::unique_ptr<MountedImageSession>* output,
                  std::string* detail);

  int DuplicateFd(int* output, std::string* detail) const;

 private:
  explicit MountedImageSession(int fd) : fd_(fd) {}

  int fd_;
};

}  // namespace eufs::storage
