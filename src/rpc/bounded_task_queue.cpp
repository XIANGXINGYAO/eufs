#include "rpc/bounded_task_queue.h"

#include <new>
#include <utility>

namespace eufs::rpc {

QueuedTask::QueuedTask(Callback execute, Callback cancel)
    : execute_(std::move(execute)),
      cancel_(std::move(cancel)),
      active_(static_cast<bool>(execute_) && static_cast<bool>(cancel_)) {}

QueuedTask::QueuedTask(InflightByteLimiter::Lease byte_lease,
                       Callback execute, Callback cancel)
    : byte_lease_(std::move(byte_lease)),
      execute_(std::move(execute)),
      cancel_(std::move(cancel)),
      active_(static_cast<bool>(byte_lease_) && static_cast<bool>(execute_) &&
              static_cast<bool>(cancel_)) {}

QueuedTask::QueuedTask(QueuedTask&& other) noexcept {
  MoveFrom(std::move(other));
}

QueuedTask& QueuedTask::operator=(QueuedTask&& other) noexcept {
  if (this != &other) {
    Cancel();
    MoveFrom(std::move(other));
  }
  return *this;
}

QueuedTask::~QueuedTask() { Cancel(); }

void QueuedTask::MoveFrom(QueuedTask&& other) noexcept {
  byte_lease_ = std::move(other.byte_lease_);
  execute_ = std::move(other.execute_);
  cancel_ = std::move(other.cancel_);
  active_ = other.active_;
  other.active_ = false;
  other.execute_ = {};
  other.cancel_ = {};
}

bool QueuedTask::Finish(bool run) noexcept {
  if (!active_) {
    return false;
  }
  // 先撤销另一条路径的资格，再调用外部完成逻辑，防止回调重入造成二次完成。
  active_ = false;
  Callback selected = run ? std::move(execute_) : std::move(cancel_);
  execute_ = {};
  cancel_ = {};
  selected();
  byte_lease_.Reset();
  return true;
}

bool QueuedTask::Run() noexcept { return Finish(true); }

bool QueuedTask::Cancel() noexcept { return Finish(false); }

bool QueuedTask::active() const noexcept { return active_; }

std::size_t QueuedTask::lease_bytes() const noexcept {
  return byte_lease_.bytes();
}

BoundedTaskQueue::BoundedTaskQueue(std::size_t capacity)
    : capacity_(capacity) {}

EnqueueResult BoundedTaskQueue::TryEnqueue(QueuedTask* task) {
  if (task == nullptr || !task->active()) {
    return EnqueueResult::kInvalid;
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    return EnqueueResult::kStopped;
  }
  if (tasks_.size() >= capacity_) {
    return EnqueueResult::kFull;
  }
  try {
    tasks_.push_back(std::move(*task));
  } catch (const std::bad_alloc&) {
    // QueuedTask 的 move 构造为 noexcept；分配失败时 deque 保持原状，Task 仍归调用者。
    return EnqueueResult::kResourceExhausted;
  }
  ready_.notify_one();
  return EnqueueResult::kAccepted;
}

bool BoundedTaskQueue::WaitPop(QueuedTask* output) {
  if (output == nullptr) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  ready_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
  if (tasks_.empty()) {
    return false;
  }
  *output = std::move(tasks_.front());
  tasks_.pop_front();
  return true;
}

std::deque<QueuedTask> BoundedTaskQueue::StopAndTakePending() {
  std::deque<QueuedTask> pending;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    pending.swap(tasks_);
  }
  ready_.notify_all();
  return pending;
}

std::size_t BoundedTaskQueue::size() const noexcept {
  const std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

bool BoundedTaskQueue::stopped() const noexcept {
  const std::lock_guard<std::mutex> lock(mutex_);
  return stopped_;
}

}  // namespace eufs::rpc
