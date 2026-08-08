#include "journal/durable_stage_failpoint.h"

#include <cstdio>
#include <initializer_list>
#include <utility>
#include <unistd.h>

namespace eufs::journal {
namespace {

class ProcessCrashObserver final : public DurableStageObserver {
 public:
  ProcessCrashObserver(DurableStage target, std::string process_name)
      : target_(target), process_name_(std::move(process_name)) {}

  void OnDurableStage(DurableStage stage) override {
    if (stage != target_) {
      return;
    }
    ::dprintf(STDERR_FILENO, "%s: crash failpoint reached: %s\n",
              process_name_.c_str(), DurableStageName(stage));
    _exit(200);
  }

 private:
  DurableStage target_;
  std::string process_name_;
};

}  // namespace

const char* DurableStageName(DurableStage stage) {
  switch (stage) {
    case DurableStage::kOrderedData:
      return "ordered-data";
    case DurableStage::kJournalBody:
      return "journal-body";
    case DurableStage::kControlExposure:
      return "control-exposure";
    case DurableStage::kCommit:
      return "commit";
    case DurableStage::kHomeBlocks:
      return "home-blocks";
    case DurableStage::kCheckpoint:
      return "checkpoint";
  }
  return "unknown";
}

bool ParseDurableStage(std::string_view value, DurableStage* output) {
  if (output == nullptr) {
    return false;
  }
  for (const auto stage : {DurableStage::kOrderedData,
                           DurableStage::kJournalBody,
                           DurableStage::kControlExposure,
                           DurableStage::kCommit,
                           DurableStage::kHomeBlocks,
                           DurableStage::kCheckpoint}) {
    if (value == DurableStageName(stage)) {
      *output = stage;
      return true;
    }
  }
  return false;
}

std::shared_ptr<DurableStageObserver> MakeProcessCrashObserver(
    DurableStage target, std::string process_name) {
  return std::make_shared<ProcessCrashObserver>(target,
                                                 std::move(process_name));
}

}  // namespace eufs::journal
