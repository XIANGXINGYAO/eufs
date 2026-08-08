#pragma once

#include "metadata/ondisk_format.h"
#include "object/request_ledger_index.h"
#include "storage/image_reader.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>

namespace eufs::object_store {

// 一次 ledger 追加的块级计划。ledger 记录只有 128 字节，但 journal 以
// 4 KiB 块为最小回放单位，因此计划必须保存目标块的完整前后镜像。
struct RequestLedgerAppendPlan {
  std::uint64_t sequence{0};
  std::uint32_t logical_block{0};
  std::uint32_t physical_block{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  RequestLedgerRecord record;
  ondisk::Block before_image{};
  ondisk::Block after_image{};
};

// 根据启动扫描得到的 index 选择唯一的下一空槽。调用者不能指定 sequence；
// 路径、inode、容量或槽位与 index 矛盾时拒绝生成计划。失败不修改 output。
int PrepareRequestLedgerAppend(const storage::ImageReader& image,
                               const RequestLedgerIndex& index,
                               RequestLedgerRecord record_without_sequence,
                               RequestLedgerAppendPlan* output,
                               std::string* detail);

// 把 ledger 块加入现有对象事务。ledger 必须作为 journal metadata，不能作为
// COMMIT 前直写 home 的 ordered data。任何块所有权或镜像来源冲突都会失败，
// 并保证三张调用者 map 全部保持原值。
int MergeRequestLedgerAppend(
    const RequestLedgerAppendPlan& ledger, std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    std::map<std::uint32_t, ondisk::Block>* before_images,
    std::map<std::uint32_t, ondisk::Block>* ordered_data_after_images,
    std::map<std::uint32_t, ondisk::Block>* metadata_after_images,
    std::string* detail);

}  // namespace eufs::object_store
