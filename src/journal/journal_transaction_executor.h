#pragma once

#include "journal/journal_control_store.h"
#include "storage/mounted_image_session.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace eufs::journal {

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
