// 验证进程内 ObjectBackend 的新建/条件替换/读取语义、并发互斥和持久化结果。
// 这是后续 RPC 层可复用的业务接口证据；测试本身不假装已经实现网络协议。
#include "checker/consistency_checker.h"
#include "object/object_backend.h"
#include "storage/mkfs.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string TemporaryPath() {
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-object-backend-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());
  return path_template.data();
}

std::string CreateImage() {
  eufs::storage::MkfsOptions options;
  options.image_path = TemporaryPath();
  options.image_size_bytes = 16ULL * 1024ULL * 1024ULL;
  options.total_inodes = 512;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());
  return options.image_path;
}

bool PreadAll(int fd, std::uint8_t* output, std::size_t size,
              std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const ssize_t result =
        pread(fd, output + completed, size - completed,
              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

std::vector<std::uint8_t> ReadWholeImage(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image snapshot");
  struct stat attributes {};
  Require(fstat(fd, &attributes) == 0 && attributes.st_size > 0,
          "could not stat image snapshot");
  std::vector<std::uint8_t> output(
      static_cast<std::size_t>(attributes.st_size));
  Require(PreadAll(fd, output.data(), output.size(), 0),
          "could not read image snapshot");
  close(fd);
  return output;
}

void RequireHealthy(const std::string& path) {
  eufs::checker::ConsistencyReport report;
  std::string detail;
  Require(eufs::checker::CheckImage(path, &report, &detail) == 0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete &&
              report.root_reachability_complete &&
              report.block_reference_scan_complete &&
              report.inode_reference_scan_complete && report.issues.empty(),
          "object backend left a globally inconsistent image");
}

eufs::object_store::ObjectBackendOptions BackendOptions() {
  eufs::object_store::ObjectBackendOptions options;
  options.permissions = 0640;
  options.uid = 1000;
  options.gid = 1001;
  return options;
}

std::unique_ptr<eufs::object_store::ObjectBackend> OpenBackend(
    const std::string& path,
    std::shared_ptr<eufs::journal::DurableStageObserver> observer = nullptr) {
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::object_store::ObjectBackend::Open(
              path, BackendOptions(), &backend, &action, &detail, nullptr,
              nullptr, std::move(observer)) == 0 &&
              action == eufs::journal::RecoveryAction::kNoAction,
          detail.c_str());
  return backend;
}

void TestDirectApiAndFailureContracts() {
  const std::string path = CreateImage();
  auto backend = OpenBackend(path);
  Require(backend->usable(), "new object backend is unusable");

  std::unique_ptr<eufs::object_store::ObjectBackend> contender;
  eufs::journal::RecoveryAction contender_action{};
  std::string detail;
  Require(eufs::object_store::ObjectBackend::Open(
              path, BackendOptions(), &contender, &contender_action,
              &detail) == -EBUSY &&
              contender == nullptr,
          "second object backend bypassed the image lifecycle lock");

  const std::string payload("alpha\0beta", 10);
  Require(backend->PutIfAbsent("binary.bin", payload, 100, &detail) == 0,
          detail.c_str());
  std::string contents;
  Require(backend->Get("binary.bin", &contents, &detail) == 0 &&
              contents == payload,
          detail.c_str());
  eufs::object_store::ObjectStat stat;
  Require(backend->Stat("binary.bin", &stat, &detail) == 0 &&
              stat.inode_number != 0 && stat.size == payload.size() &&
              stat.mtime_ns == 100 && stat.generation == 1,
          detail.c_str());

  const auto before_duplicate = ReadWholeImage(path);
  Require(backend->PutIfAbsent("binary.bin", "replacement", 200, &detail) ==
                  -EEXIST &&
              ReadWholeImage(path) == before_duplicate,
          "duplicate PutIfAbsent changed the persistent image");

  std::string missing = "unchanged";
  eufs::object_store::ObjectStat missing_stat{77, 88, 99, 111};
  Require(backend->Get("missing", &missing, &detail) == -ENOENT &&
              missing == "unchanged" &&
              backend->Stat("missing", &missing_stat, &detail) == -ENOENT &&
              missing_stat.inode_number == 77 && missing_stat.size == 88,
          "missing-object lookup changed an output object");
  Require(backend->Get("bad/name", &missing, &detail) == -EINVAL &&
              missing == "unchanged",
          "invalid object key was accepted or changed output");

  backend.reset();
  backend = OpenBackend(path);
  contents.clear();
  Require(backend->Get("binary.bin", &contents, &detail) == 0 &&
              contents == payload,
          "object did not survive backend restart");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

void TestConditionalReplacementAndRestart() {
  const std::string path = CreateImage();
  auto backend = OpenBackend(path);
  std::string detail;
  Require(backend->PutIfAbsent("replace", "old-payload-tail", 10, &detail) ==
              0,
          detail.c_str());
  eufs::object_store::ObjectStat before;
  Require(backend->Stat("replace", &before, &detail) == 0,
          detail.c_str());
  const eufs::object_store::ObjectVersion old_version{
      before.inode_number, before.generation};

  eufs::object_store::MutationResult mutation;
  Require(backend->ReplaceIfVersion("replace", old_version, "new", 20,
                                    &mutation, &detail) == 0 &&
              mutation.outcome ==
                  eufs::object_store::MutationOutcome::kCommitted &&
              mutation.committed_version.inode_number == before.inode_number &&
              mutation.committed_version.generation == before.generation + 1U,
          detail.c_str());
  std::string contents;
  eufs::object_store::ObjectStat after;
  Require(backend->Get("replace", &contents, &detail) == 0 &&
              contents == "new" &&
              backend->Stat("replace", &after, &detail) == 0 &&
              after.size == 3 && after.generation == before.generation + 1U,
          "conditional replacement retained old tail or generation");

  const auto image_after_success = ReadWholeImage(path);
  eufs::object_store::MutationResult stale;
  Require(backend->ReplaceIfVersion("replace", old_version, "loser", 30,
                                    &stale, &detail) == -ESTALE &&
              stale.outcome ==
                  eufs::object_store::MutationOutcome::kNotApplied &&
              stale.current_version.inode_number == after.inode_number &&
              stale.current_version.generation == after.generation &&
              ReadWholeImage(path) == image_after_success,
          "stale replacement changed the image or omitted current version");

  backend.reset();
  backend = OpenBackend(path);
  contents.clear();
  Require(backend->Get("replace", &contents, &detail) == 0 &&
              contents == "new" &&
              backend->Stat("replace", &after, &detail) == 0 &&
              after.generation == mutation.committed_version.generation,
          "replacement did not survive backend restart");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

class BlockingCommitObserver final
    : public eufs::journal::DurableStageObserver {
 public:
  BlockingCommitObserver()
      : entered_(entered_promise_.get_future().share()),
        release_(release_promise_.get_future().share()) {}

  void Arm() {
    armed_.store(true);
  }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) {
    return entered_.wait_for(timeout) == std::future_status::ready;
  }

  void Release() { release_promise_.set_value(); }

  void OnDurableStage(eufs::journal::DurableStage stage) override {
    if (stage != eufs::journal::DurableStage::kCommit ||
        !armed_.exchange(false)) {
      return;
    }
    entered_promise_.set_value();
    release_.wait();
  }

 private:
  std::atomic<bool> armed_{false};
  std::promise<void> entered_promise_;
  std::shared_future<void> entered_;
  std::promise<void> release_promise_;
  std::shared_future<void> release_;
};

void TestMutexCoversCommitThroughCheckpoint() {
  const std::string path = CreateImage();
  auto observer = std::make_shared<BlockingCommitObserver>();
  auto backend = OpenBackend(path, observer);
  std::string detail;
  Require(backend->PutIfAbsent("old", "stable", 10, &detail) == 0,
          detail.c_str());

  const std::string payload(13U * eufs::ondisk::kBlockSize, 'N');
  observer->Arm();
  auto writer = std::async(std::launch::async, [&] {
    std::string writer_detail;
    return backend->PutIfAbsent("new", payload, 20, &writer_detail);
  });
  Require(observer->WaitUntilEntered(std::chrono::seconds(5)),
          "writer did not reach the durable COMMIT boundary");

  std::string read_contents;
  auto reader = std::async(std::launch::async, [&] {
    std::string reader_detail;
    return backend->Get("new", &read_contents, &reader_detail);
  });
  const bool reader_blocked =
      reader.wait_for(std::chrono::milliseconds(100)) ==
      std::future_status::timeout;
  observer->Release();

  Require(reader_blocked,
          "reader entered home metadata while writer held the backend mutex");
  Require(writer.get() == 0, "writer failed after the COMMIT gate was released");
  Require(reader.get() == 0 && read_contents == payload,
          "reader did not observe the complete checkpointed object");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

class FailFirstSyncIo final : public eufs::journal::JournalControlIo {
 public:
  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    return pwrite(fd, input, size, offset);
  }

  int Fdatasync(int fd) override {
    if (fdatasync(fd) != 0) {
      return -1;
    }
    if (sync_calls_++ == 0) {
      errno = EIO;
      return -1;
    }
    return 0;
  }

 private:
  std::size_t sync_calls_{0};
};

void TestUncertainDurabilityFailsClosed() {
  const std::string path = CreateImage();
  auto fault_io = std::make_shared<FailFirstSyncIo>();
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::object_store::ObjectBackend::Open(
              path, BackendOptions(), &backend, &action, &detail, nullptr,
              fault_io) == 0 &&
              action == eufs::journal::RecoveryAction::kNoAction,
          detail.c_str());

  Require(backend->PutIfAbsent("uncertain", "payload", 30, &detail) == -EIO &&
              !backend->usable(),
          "uncertain ordered-data durability did not fail the backend closed");
  std::string output = "unchanged";
  Require(backend->Get("uncertain", &output, &detail) == -EIO &&
              output == "unchanged",
          "failed backend continued serving object reads");
  backend.reset();

  backend = OpenBackend(path);
  Require(backend->Get("uncertain", &output, &detail) == -ENOENT &&
              output == "unchanged",
          "uncommitted object became visible after backend restart");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

void TestReplacementUncertaintyIsReported() {
  const std::string path = CreateImage();
  {
    auto seed = OpenBackend(path);
    std::string detail;
    Require(seed->PutIfAbsent("uncertain-replace", "old", 10, &detail) == 0,
            detail.c_str());
  }

  auto fault_io = std::make_shared<FailFirstSyncIo>();
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(eufs::object_store::ObjectBackend::Open(
              path, BackendOptions(), &backend, &action, &detail, nullptr,
              fault_io) == 0,
          detail.c_str());
  eufs::object_store::ObjectStat stat;
  Require(backend->Stat("uncertain-replace", &stat, &detail) == 0,
          detail.c_str());
  eufs::object_store::MutationResult mutation;
  Require(backend->ReplaceIfVersion(
              "uncertain-replace", {stat.inode_number, stat.generation},
              "new", 20, &mutation, &detail) == -EIO &&
              mutation.outcome ==
                  eufs::object_store::MutationOutcome::kUnknown &&
              mutation.current_version.inode_number == 0 &&
              mutation.current_version.generation == 0 &&
              !backend->usable(),
          "uncertain replacement was reported as safely not applied");
  backend.reset();

  // 重启恢复后结果才能重新确定；此注入点位于 COMMIT 前，所以本例恢复为旧值。
  backend = OpenBackend(path);
  std::string contents;
  Require(backend->Get("uncertain-replace", &contents, &detail) == 0 &&
              contents == "old",
          "pre-COMMIT uncertain replacement did not recover to old value");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

struct StartGate {
  std::mutex mutex;
  std::condition_variable condition;
  int ready{0};
  bool go{false};
};

void TestConcurrentSameNameHasOneWinner() {
  const std::string path = CreateImage();
  auto backend = OpenBackend(path);
  StartGate gate;
  const std::array<std::string, 2> payloads{"first", "second"};
  std::array<int, 2> results{};
  std::array<std::future<void>, 2> workers;

  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::async(std::launch::async, [&, index] {
      {
        std::unique_lock<std::mutex> lock(gate.mutex);
        ++gate.ready;
        gate.condition.notify_all();
        gate.condition.wait(lock, [&] { return gate.go; });
      }
      std::string thread_detail;
      results[index] = backend->PutIfAbsent(
          "race", payloads[index], 100 + index, &thread_detail);
    });
  }
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.condition.wait(lock, [&] { return gate.ready == 2; });
    gate.go = true;
    gate.condition.notify_all();
  }
  for (auto& worker : workers) {
    worker.get();
  }

  const bool first_won = results[0] == 0 && results[1] == -EEXIST;
  const bool second_won = results[1] == 0 && results[0] == -EEXIST;
  Require(first_won || second_won,
          "same-name race did not produce one success and one EEXIST");
  std::string contents;
  std::string detail;
  Require(backend->Get("race", &contents, &detail) == 0 &&
              contents == (first_won ? payloads[0] : payloads[1]),
          "same-name race published the losing payload or a partial object");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

void TestConcurrentSameVersionHasOneWinner() {
  const std::string path = CreateImage();
  auto backend = OpenBackend(path);
  std::string detail;
  Require(backend->PutIfAbsent("replace-race", "old", 10, &detail) == 0,
          detail.c_str());
  eufs::object_store::ObjectStat stat;
  Require(backend->Stat("replace-race", &stat, &detail) == 0,
          detail.c_str());
  const eufs::object_store::ObjectVersion expected{stat.inode_number,
                                                   stat.generation};

  StartGate gate;
  const std::array<std::string, 2> payloads{"winner-one", "winner-two"};
  std::array<int, 2> results{};
  std::array<eufs::object_store::MutationResult, 2> mutations{};
  std::array<std::future<void>, 2> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::async(std::launch::async, [&, index] {
      {
        std::unique_lock<std::mutex> lock(gate.mutex);
        ++gate.ready;
        gate.condition.notify_all();
        gate.condition.wait(lock, [&] { return gate.go; });
      }
      std::string thread_detail;
      results[index] = backend->ReplaceIfVersion(
          "replace-race", expected, payloads[index], 20 + index,
          &mutations[index], &thread_detail);
    });
  }
  {
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.condition.wait(lock, [&] { return gate.ready == 2; });
    gate.go = true;
    gate.condition.notify_all();
  }
  for (auto& worker : workers) {
    worker.get();
  }

  const bool first_won = results[0] == 0 && results[1] == -ESTALE;
  const bool second_won = results[1] == 0 && results[0] == -ESTALE;
  Require(first_won || second_won,
          "same-version race did not produce one commit and one ESTALE");
  const std::size_t winner = first_won ? 0U : 1U;
  const std::size_t loser = 1U - winner;
  Require(mutations[winner].outcome ==
                  eufs::object_store::MutationOutcome::kCommitted &&
              mutations[loser].outcome ==
                  eufs::object_store::MutationOutcome::kNotApplied &&
              mutations[loser].current_version.generation ==
                  expected.generation + 1U,
          "same-version race returned wrong mutation outcomes");
  std::string contents;
  Require(backend->Get("replace-race", &contents, &detail) == 0 &&
              contents == payloads[winner],
          "same-version race published the losing payload");
  backend.reset();
  RequireHealthy(path);
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestDirectApiAndFailureContracts();
  TestConditionalReplacementAndRestart();
  TestMutexCoversCommitThroughCheckpoint();
  TestUncertainDurabilityFailsClosed();
  TestReplacementUncertaintyIsReported();
  TestConcurrentSameNameHasOneWinner();
  TestConcurrentSameVersionHasOneWinner();
  std::cout << "PASS: direct object backend and serialization contracts\n";
  return 0;
}
