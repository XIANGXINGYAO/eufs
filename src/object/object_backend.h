#pragma once

#include "journal/journal_control_store.h"
#include "storage/image_reader.h"
#include "storage/mounted_image_session.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace eufs::object_store {

struct ObjectBackendOptions {
  std::uint32_t permissions{0644};
  std::uint32_t uid{0};
  std::uint32_t gid{0};
};

struct ObjectStat {
  std::uint32_t inode_number{0};
  std::uint64_t size{0};
  std::uint64_t mtime_ns{0};
  std::uint64_t generation{0};
};

class ObjectBackend {
 public:
  ObjectBackend(const ObjectBackend&) = delete;
  ObjectBackend& operator=(const ObjectBackend&) = delete;

  static int Open(
      const std::string& image_path, const ObjectBackendOptions& options,
      std::unique_ptr<ObjectBackend>* output,
      journal::RecoveryAction* recovery_action, std::string* detail,
      std::shared_ptr<journal::JournalControlIo> recovery_io = nullptr,
      std::shared_ptr<journal::JournalControlIo> mutation_io = nullptr,
      std::shared_ptr<journal::DurableStageObserver> mutation_observer =
          nullptr);

  int PutIfAbsent(std::string_view name, std::string_view data,
                  std::uint64_t timestamp_ns, std::string* detail);
  int Get(std::string_view name, std::string* output, std::string* detail);
  int Stat(std::string_view name, ObjectStat* output, std::string* detail);
  bool usable() const;

 private:
  ObjectBackend(
      ObjectBackendOptions options,
      std::unique_ptr<storage::MountedImageSession> session,
      std::unique_ptr<storage::ImageReader> reader,
      std::shared_ptr<journal::JournalControlIo> mutation_io,
      std::shared_ptr<journal::DurableStageObserver> mutation_observer);

  int CheckUsableLocked(std::string* detail) const;
  int ResolveRegularLocked(std::string_view name, std::uint32_t* inode_number,
                           ondisk::InodeRecord* inode,
                           std::string* detail) const;
  int ApplyLocked(
      const std::map<std::uint32_t, ondisk::Block>& before_images,
      const std::map<std::uint32_t, ondisk::Block>&
          ordered_data_after_images,
      const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
      std::uint32_t total_blocks,
      const std::array<std::uint8_t, 16>& filesystem_uuid,
      std::string* detail);
  int ReloadReaderLocked(std::string* detail);
  void FailClosedLocked(int error, std::string_view detail);

  ObjectBackendOptions options_;
  std::unique_ptr<storage::MountedImageSession> session_;
  std::unique_ptr<storage::ImageReader> reader_;
  std::shared_ptr<journal::JournalControlIo> mutation_io_;
  std::shared_ptr<journal::DurableStageObserver> mutation_observer_;
  mutable std::mutex mutex_;
  int fatal_error_{0};
  std::string fatal_detail_;
};

}  // namespace eufs::object_store
