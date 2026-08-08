#pragma once

#include <cstddef>
#include <memory>
#include <optional>

namespace eufs::rpc {

// 对所有已准入但尚未完成的请求按 payload 字节收费。
// Lease 是额度所有权：移动它会转移责任，销毁它会自动归还额度。
class InflightByteLimiter {
 private:
  struct State;

 public:
  class Lease {
   public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    ~Lease();

    explicit operator bool() const noexcept;
    std::size_t bytes() const noexcept;
    void Reset() noexcept;

   private:
    friend class InflightByteLimiter;
    Lease(std::shared_ptr<State> state, std::size_t bytes) noexcept;

    std::shared_ptr<State> state_;
    std::size_t bytes_{0};
  };

  explicit InflightByteLimiter(std::size_t limit_bytes);
  InflightByteLimiter(const InflightByteLimiter&) = delete;
  InflightByteLimiter& operator=(const InflightByteLimiter&) = delete;

  // 成功时原子增加 used_bytes 并返回 Lease；超限时状态完全不变。
  std::optional<Lease> TryAcquire(std::size_t bytes);
  std::size_t limit_bytes() const noexcept;
  std::size_t used_bytes() const noexcept;

 private:
  std::shared_ptr<State> state_;
};

}  // namespace eufs::rpc
