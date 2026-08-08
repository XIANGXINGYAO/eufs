#pragma once

#include "journal/journal_control_store.h"
#include "storage/mounted_image_session.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace eufs::journal {

// 在线写路径唯一允许调用的事务编排入口。
// planner 只负责生成三类块镜像，本函数负责按 ordered-data WAL 协议持久化：
// 新数据 -> 日志体 -> control 暴露 -> COMMIT -> home replay -> checkpoint。
//
// before_images：事务涉及块的旧内容，用于验证计划基于当前镜像生成。
// ordered_data_after_images：COMMIT 前必须先落盘的新 COW 数据块。
// metadata_after_images：写入日志并在 COMMIT 后回放的元数据块。
// failure_requires_fail_closed：失败后当前进程能否安全继续服务；一旦接管事务 fd，
// 持久化边界就可能不确定，因此失败通常要求上层关闭 reader 并拒绝后续请求。
int ExecuteJournalTransaction(
    storage::MountedImageSession& session,
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>&
        ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    std::shared_ptr<JournalControlIo> io,
    std::shared_ptr<DurableStageObserver> observer,
    bool* failure_requires_fail_closed, std::string* detail);

}  // namespace eufs::journal
