#include "journal/durable_stage_failpoint.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  using eufs::journal::DurableStage;
  for (const auto stage : {DurableStage::kOrderedData,
                           DurableStage::kJournalBody,
                           DurableStage::kControlExposure,
                           DurableStage::kCommit,
                           DurableStage::kHomeBlocks,
                           DurableStage::kCheckpoint}) {
    DurableStage parsed{};
    const std::string_view name = eufs::journal::DurableStageName(stage);
    Require(name != "unknown", "known stage mapped to unknown");
    Require(eufs::journal::ParseDurableStage(name, &parsed),
            "known stage name was rejected");
    Require(parsed == stage, "stage name did not round-trip");
  }

  DurableStage untouched = DurableStage::kCommit;
  Require(!eufs::journal::ParseDurableStage("invalid", &untouched),
          "invalid stage name was accepted");
  Require(untouched == DurableStage::kCommit,
          "failed parse modified the caller output");
  Require(!eufs::journal::ParseDurableStage("commit", nullptr),
          "null parse output was accepted");
  std::cout << "PASS: durable stage command names round-trip\n";
  return 0;
}
