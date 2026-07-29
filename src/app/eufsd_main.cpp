#define FUSE_USE_VERSION 31

#include "fuse/read_only_operations.h"

#include <fuse3/fuse.h>

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

void PrintUsage() {
  std::cerr << "Usage: eufsd --image IMAGE "
               "[--crash-after STAGE] [FUSE options] MOUNTPOINT\n";
}

const char* DurableStageName(eufs::journal::DurableStage stage) {
  switch (stage) {
    case eufs::journal::DurableStage::kOrderedData:
      return "ordered-data";
    case eufs::journal::DurableStage::kJournalBody:
      return "journal-body";
    case eufs::journal::DurableStage::kControlExposure:
      return "control-exposure";
    case eufs::journal::DurableStage::kCommit:
      return "commit";
    case eufs::journal::DurableStage::kHomeBlocks:
      return "home-blocks";
    case eufs::journal::DurableStage::kCheckpoint:
      return "checkpoint";
  }
  return "unknown";
}

bool ParseDurableStage(std::string_view value,
                       eufs::journal::DurableStage* output) {
  for (const auto stage : {eufs::journal::DurableStage::kOrderedData,
                           eufs::journal::DurableStage::kJournalBody,
                           eufs::journal::DurableStage::kControlExposure,
                           eufs::journal::DurableStage::kCommit,
                           eufs::journal::DurableStage::kHomeBlocks,
                           eufs::journal::DurableStage::kCheckpoint}) {
    if (value == DurableStageName(stage)) {
      *output = stage;
      return true;
    }
  }
  return false;
}

class ProcessCrashObserver final
    : public eufs::journal::DurableStageObserver {
 public:
  explicit ProcessCrashObserver(eufs::journal::DurableStage target)
      : target_(target) {}

  void OnDurableStage(eufs::journal::DurableStage stage) override {
    if (stage != target_) {
      return;
    }
    ::dprintf(STDERR_FILENO, "eufsd: crash failpoint reached: %s\n",
              DurableStageName(stage));
    _exit(200);
  }

 private:
  eufs::journal::DurableStage target_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string image_path;
  std::shared_ptr<eufs::journal::DurableStageObserver> mutation_observer;
  std::vector<char*> fuse_arguments;
  fuse_arguments.reserve(static_cast<std::size_t>(argc) + 1U);
  fuse_arguments.push_back(argv[0]);

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--image") {
      if (index + 1 >= argc || !image_path.empty()) {
        PrintUsage();
        return 2;
      }
      image_path = argv[++index];
      continue;
    }
    constexpr std::string_view kImagePrefix = "--image=";
    if (argument.substr(0, kImagePrefix.size()) == kImagePrefix) {
      if (!image_path.empty() || argument.size() == kImagePrefix.size()) {
        PrintUsage();
        return 2;
      }
      image_path.assign(argument.substr(kImagePrefix.size()));
      continue;
    }
    if (argument == "--crash-after") {
      if (index + 1 >= argc || mutation_observer != nullptr) {
        PrintUsage();
        return 2;
      }
      eufs::journal::DurableStage stage{};
      if (!ParseDurableStage(argv[++index], &stage)) {
        PrintUsage();
        return 2;
      }
      mutation_observer = std::make_shared<ProcessCrashObserver>(stage);
      continue;
    }
    constexpr std::string_view kCrashPrefix = "--crash-after=";
    if (argument.substr(0, kCrashPrefix.size()) == kCrashPrefix) {
      eufs::journal::DurableStage stage{};
      if (mutation_observer != nullptr ||
          !ParseDurableStage(argument.substr(kCrashPrefix.size()), &stage)) {
        PrintUsage();
        return 2;
      }
      mutation_observer = std::make_shared<ProcessCrashObserver>(stage);
      continue;
    }
    fuse_arguments.push_back(argv[index]);
  }
  if (image_path.empty()) {
    PrintUsage();
    return 2;
  }

  std::unique_ptr<eufs::fuse_adapter::StageCState> state;
  eufs::journal::RecoveryAction recovery_action{};
  std::string detail;
  const int open_result = eufs::fuse_adapter::OpenStageCState(
      image_path, &state, &recovery_action, &detail, nullptr,
      std::move(mutation_observer));
  if (open_result != 0) {
    std::cerr << "eufsd: " << detail << " (errno " << -open_result << ")\n";
    return 1;
  }

  fuse_operations operations = eufs::fuse_adapter::MakeReadOnlyOperations();
  const int fuse_argc = static_cast<int>(fuse_arguments.size());
  fuse_arguments.push_back(nullptr);
  return fuse_main(fuse_argc, fuse_arguments.data(), &operations, state.get());
}
