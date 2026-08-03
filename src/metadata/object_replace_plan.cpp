#include "metadata/object_replace_plan.h"

namespace eufs::metadata {

// 对象全量替换入口只增加完整替换和 expected-generation 条件。
int PrepareObjectReplace(const storage::ImageReader& image,
                         std::uint32_t inode_number,
                         std::uint64_t expected_generation,
                         std::string_view data,
                         std::uint64_t timestamp_ns,
                         ObjectReplacePlan* output,
                         std::string* error_detail) {
  const detail::FileMutationRequest request{
      detail::FileMutationMode::kReplaceAll, 0, data, timestamp_ns,
      expected_generation};
  return detail::PrepareFileMutation(image, inode_number, request, output,
                                     error_detail);
}

}  // namespace eufs::metadata
