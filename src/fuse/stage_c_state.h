#pragma once

#include "journal/journal_control_store.h"
#include "storage/image_reader.h"
#include "storage/mounted_image_session.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace eufs::fuse_adapter {

struct StageCState {
  StageCState(std::string image_path_value,
              std::unique_ptr<storage::MountedImageSession> session_value,
              std::unique_ptr<storage::ImageReader> reader_value,
              std::shared_ptr<journal::DurableStageObserver>
                  mutation_observer_value = nullptr);

  StageCState(const StageCState&) = delete;
  StageCState& operator=(const StageCState&) = delete;

  bool usable() const;
  int ReloadReader(std::string* detail);
  void FailClosed(int error, std::string_view detail);
  int fatal_error() const { return fatal_error_; }
  const std::string& fatal_detail() const { return fatal_detail_; }

  std::string image_path;
  std::unique_ptr<storage::MountedImageSession> session;
  std::unique_ptr<storage::ImageReader> reader;
  std::shared_ptr<journal::DurableStageObserver> mutation_observer;
  std::mutex mutex;

 private:
  int fatal_error_{0};
  std::string fatal_detail_;
};

int OpenStageCState(
    const std::string& image_path, std::unique_ptr<StageCState>* output,
    journal::RecoveryAction* recovery_action, std::string* detail,
    std::shared_ptr<journal::JournalControlIo> recovery_io = nullptr,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer = nullptr);

}  // namespace eufs::fuse_adapter
