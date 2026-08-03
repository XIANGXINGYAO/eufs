#include "metadata/file_write_plan.h"

#include <cerrno>

namespace eufs::metadata {

// POSIX 局部写入口只描述“不截断、保留未覆盖字节”的语义。
int PrepareFileWrite(const storage::ImageReader& image,
                     std::uint32_t inode_number, std::uint64_t offset,
                     std::string_view data, std::uint64_t timestamp_ns,
                     FileWritePlan* output, std::string* error_detail) {
  if (data.empty()) {
    if (error_detail != nullptr) {
      error_detail->assign("file write requires non-empty data");
    }
    return -EINVAL;
  }
  const detail::FileMutationRequest request{
      detail::FileMutationMode::kWriteRange, offset, data, timestamp_ns, 0};
  return detail::PrepareFileMutation(image, inode_number, request, output,
                                     error_detail);
}

}  // namespace eufs::metadata
