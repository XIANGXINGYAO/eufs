#pragma once

#include "rpc/inflight_byte_limiter.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace eufs::rpc {

// Task 把执行路径、停机取消路径和字节额度绑成一个可移动所有权单元。
// 后续 brpc 层把 done 放进两个回调后，Run/Cancel 会强制其中一条路径恰好执行一次。
class QueuedTask {
 public:
  using Callback = std::function<void()>;

  QueuedTask() = default;
  QueuedTask(Callback execute, Callback cancel);
  QueuedTask(InflightByteLimiter::Lease byte_lease, Callback execute,
             Callback cancel);
  QueuedTask(const QueuedTask&) = delete;
  QueuedTask& operator=(const QueuedTask&) = delete;
  QueuedTask(QueuedTask&& other) noexcept;
  QueuedTask& operator=(QueuedTask&& other) noexcept;
  ~QueuedTask();

  // 只有第一条完成路径会返回 true；后续重复调用不再执行回调。
  bool Run() noexcept;
  bool Cancel() noexcept;
  bool active() const noexcept;
  std::size_t lease_bytes() const noexcept;

 private:
  void MoveFrom(QueuedTask&& other) noexcept;
  bool Finish(bool run) noexcept;

  InflightByteLimiter::Lease byte_lease_;
  Callback execute_;
  Callback cancel_;
  bool active_{false};
};

enum class EnqueueResult {
  kAccepted,
  kFull,
  kStopped,
  kResourceExhausted,
  kInvalid,
};

// 这里只管理排队任务，不创建 worker。服务层可以明确控制 worker 和 shutdown 顺序。
class BoundedTaskQueue {
 public:
  explicit BoundedTaskQueue(std::size_t capacity);
  BoundedTaskQueue(const BoundedTaskQueue&) = delete;
  BoundedTaskQueue& operator=(const BoundedTaskQueue&) = delete;

  // 只有 kAccepted 才移动 *task；其他结果下调用者仍完整拥有 Task。
  EnqueueResult TryEnqueue(QueuedTask* task);
  // 阻塞等待任务；队列停止且已无任务时返回 false。
  bool WaitPop(QueuedTask* output);
  // 原子拒绝新任务并取走全部排队任务，调用者随后逐个执行 cancel。
  std::deque<QueuedTask> StopAndTakePending();

  std::size_t size() const noexcept;
  bool stopped() const noexcept;

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<QueuedTask> tasks_;
  bool stopped_{false};
};

}  // namespace eufs::rpc
