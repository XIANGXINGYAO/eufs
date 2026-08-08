// 验证 G3 的两个并发基础不变量：超限不改计数，Task 只在成功入队时转移所有权。
#include "rpc/bounded_task_queue.h"
#include "rpc/inflight_byte_limiter.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

eufs::rpc::QueuedTask MakeTask(eufs::rpc::InflightByteLimiter::Lease lease,
                               std::atomic<int>* executed,
                               std::atomic<int>* cancelled) {
  return eufs::rpc::QueuedTask(std::move(lease),
                               [executed] { ++*executed; },
                               [cancelled] { ++*cancelled; });
}

void TestByteLeaseAdmissionAndMove() {
  eufs::rpc::InflightByteLimiter limiter(8);
  auto six = limiter.TryAcquire(6);
  Require(six.has_value() && limiter.used_bytes() == 6,
          "initial byte lease was not charged");
  Require(!limiter.TryAcquire(3).has_value() && limiter.used_bytes() == 6,
          "overload changed the byte counter");

  auto two = limiter.TryAcquire(2);
  Require(two.has_value() && limiter.used_bytes() == 8,
          "exact-limit admission failed");
  eufs::rpc::InflightByteLimiter::Lease moved = std::move(*two);
  Require(!static_cast<bool>(*two) && moved.bytes() == 2,
          "moving a lease did not transfer ownership");
  moved.Reset();
  six.reset();
  Require(limiter.used_bytes() == 0, "destroying leases leaked byte quota");
}

void TestTaskWithoutByteLease() {
  std::atomic<int> executed{0};
  std::atomic<int> cancelled{0};
  eufs::rpc::QueuedTask task([&] { ++executed; }, [&] { ++cancelled; });
  Require(task.active() && task.lease_bytes() == 0,
          "read task without a byte lease was not active");
  Require(task.Run() && !task.Cancel() && executed == 1 && cancelled == 0,
          "lease-free task did not preserve exactly-once completion");
}

void TestQueueMovesOnlyAcceptedTask() {
  eufs::rpc::InflightByteLimiter limiter(8);
  eufs::rpc::BoundedTaskQueue queue(1);
  std::atomic<int> executed{0};
  std::atomic<int> cancelled{0};

  auto first_lease = limiter.TryAcquire(3);
  Require(first_lease.has_value(), "first lease failed");
  auto first = MakeTask(std::move(*first_lease), &executed, &cancelled);
  Require(queue.TryEnqueue(&first) == eufs::rpc::EnqueueResult::kAccepted &&
              !first.active() && queue.size() == 1,
          "accepted task ownership did not move into queue");

  auto second_lease = limiter.TryAcquire(1);
  Require(second_lease.has_value(), "second lease failed");
  auto second = MakeTask(std::move(*second_lease), &executed, &cancelled);
  Require(queue.TryEnqueue(&second) == eufs::rpc::EnqueueResult::kFull &&
              second.active() && queue.size() == 1,
          "full queue consumed the rejected task");

  eufs::rpc::QueuedTask popped;
  Require(queue.WaitPop(&popped), "accepted task could not be dequeued");
  Require(popped.Run() && !popped.Run() && !popped.Cancel(),
          "one task executed more than one completion path");
  popped = {};
  Require(second.Cancel() && !second.Run() && !second.Cancel(),
          "cancelled task executed more than one completion path");
  second = {};
  Require(executed == 1 && cancelled == 1 && limiter.used_bytes() == 0,
          "execute/cancel path did not release each task exactly once");
}

void TestStopReturnsPendingTasks() {
  eufs::rpc::InflightByteLimiter limiter(8);
  eufs::rpc::BoundedTaskQueue queue(2);
  std::atomic<int> executed{0};
  std::atomic<int> cancelled{0};

  for (int index = 0; index < 2; ++index) {
    auto lease = limiter.TryAcquire(2);
    Require(lease.has_value(), "shutdown test lease failed");
    auto task = MakeTask(std::move(*lease), &executed, &cancelled);
    Require(queue.TryEnqueue(&task) == eufs::rpc::EnqueueResult::kAccepted,
            "shutdown test enqueue failed");
  }

  auto pending = queue.StopAndTakePending();
  Require(queue.stopped() && pending.size() == 2,
          "stop did not atomically detach pending tasks");
  for (auto& task : pending) {
    Require(task.Cancel(), "pending task could not be cancelled");
  }
  pending.clear();
  Require(executed == 0 && cancelled == 2 && limiter.used_bytes() == 0,
          "shutdown lost a task or leaked its byte quota");

  auto late_lease = limiter.TryAcquire(1);
  Require(late_lease.has_value(), "late request lease failed");
  auto late = MakeTask(std::move(*late_lease), &executed, &cancelled);
  Require(queue.TryEnqueue(&late) == eufs::rpc::EnqueueResult::kStopped &&
              late.active(),
          "stopped queue consumed a late request");
}

void TestShutdownSeparatesExecutingAndPendingTasks() {
  eufs::rpc::InflightByteLimiter limiter(8);
  eufs::rpc::BoundedTaskQueue queue(2);
  std::atomic<int> executed{0};
  std::atomic<int> cancelled{0};
  std::mutex gate_mutex;
  std::condition_variable gate;
  bool first_started = false;
  bool allow_first_to_finish = false;

  auto first_lease = limiter.TryAcquire(2);
  Require(first_lease.has_value(), "executing task lease failed");
  eufs::rpc::QueuedTask first;
  first = eufs::rpc::QueuedTask(
      std::move(*first_lease),
      [&] {
        std::unique_lock<std::mutex> lock(gate_mutex);
        first_started = true;
        ++executed;
        gate.notify_all();
        gate.wait(lock, [&] { return allow_first_to_finish; });
      },
      [&] { ++cancelled; });
  Require(queue.TryEnqueue(&first) == eufs::rpc::EnqueueResult::kAccepted,
          "executing task enqueue failed");

  auto second_lease = limiter.TryAcquire(2);
  Require(second_lease.has_value(), "pending task lease failed");
  auto second = MakeTask(std::move(*second_lease), &executed, &cancelled);
  Require(queue.TryEnqueue(&second) == eufs::rpc::EnqueueResult::kAccepted,
          "pending task enqueue failed");

  std::thread worker([&] {
    eufs::rpc::QueuedTask task;
    while (queue.WaitPop(&task)) {
      Require(task.Run(), "worker received an already completed task");
      task = {};
    }
  });
  {
    std::unique_lock<std::mutex> lock(gate_mutex);
    gate.wait(lock, [&] { return first_started; });
  }

  auto pending = queue.StopAndTakePending();
  Require(pending.size() == 1 && executed == 1 && cancelled == 0,
          "shutdown confused the executing task with the pending task");
  Require(pending.front().Cancel(), "pending shutdown task was inactive");
  pending.clear();

  {
    const std::lock_guard<std::mutex> lock(gate_mutex);
    allow_first_to_finish = true;
  }
  gate.notify_all();
  worker.join();
  Require(executed == 1 && cancelled == 1 && limiter.used_bytes() == 0,
          "shutdown lost completion ownership or leaked byte quota");
}

}  // namespace

int main() {
  TestByteLeaseAdmissionAndMove();
  TestTaskWithoutByteLease();
  TestQueueMovesOnlyAcceptedTask();
  TestStopReturnsPendingTasks();
  TestShutdownSeparatesExecutingAndPendingTasks();
  std::cout << "rpc_admission_queue_test: PASS\n";
  return 0;
}
