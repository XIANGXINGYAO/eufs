#include "rpc/inflight_byte_limiter.h"

#include <mutex>
#include <utility>

namespace eufs::rpc {

struct InflightByteLimiter::State {
  explicit State(std::size_t limit) : limit_bytes(limit) {}

  mutable std::mutex mutex;
  const std::size_t limit_bytes;
  std::size_t used_bytes{0};
};

InflightByteLimiter::Lease::Lease(std::shared_ptr<State> state,
                                 std::size_t bytes) noexcept
    : state_(std::move(state)), bytes_(bytes) {}

InflightByteLimiter::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), bytes_(other.bytes_) {
  other.bytes_ = 0;
}

InflightByteLimiter::Lease& InflightByteLimiter::Lease::operator=(
    Lease&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    bytes_ = other.bytes_;
    other.bytes_ = 0;
  }
  return *this;
}

InflightByteLimiter::Lease::~Lease() { Reset(); }

InflightByteLimiter::Lease::operator bool() const noexcept {
  return state_ != nullptr;
}

std::size_t InflightByteLimiter::Lease::bytes() const noexcept {
  return bytes_;
}

void InflightByteLimiter::Lease::Reset() noexcept {
  if (state_ == nullptr) {
    return;
  }
  {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->used_bytes -= bytes_;
  }
  state_.reset();
  bytes_ = 0;
}

InflightByteLimiter::InflightByteLimiter(std::size_t limit_bytes)
    : state_(std::make_shared<State>(limit_bytes)) {}

std::optional<InflightByteLimiter::Lease> InflightByteLimiter::TryAcquire(
    std::size_t bytes) {
  const std::lock_guard<std::mutex> lock(state_->mutex);
  // 这种写法同时避免 used_bytes + bytes 的整数溢出。
  if (bytes > state_->limit_bytes - state_->used_bytes) {
    return std::nullopt;
  }
  state_->used_bytes += bytes;
  return Lease(state_, bytes);
}

std::size_t InflightByteLimiter::limit_bytes() const noexcept {
  return state_->limit_bytes;
}

std::size_t InflightByteLimiter::used_bytes() const noexcept {
  const std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->used_bytes;
}

}  // namespace eufs::rpc
