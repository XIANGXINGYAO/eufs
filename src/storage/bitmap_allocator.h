#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace eufs::storage {

class BitmapAllocator;

// 一次尚未提交的 bitmap 位预留。采用 RAII：对象析构时如果仍 active，
// 自动把该位清回 0；planner 只有完整成功后才能调用 KeepReserved。
class BitmapReservation {
 public:
  BitmapReservation() = default;
  ~BitmapReservation();

  // 禁止复制，避免两个对象重复回滚同一位；允许移动以放入容器。
  BitmapReservation(const BitmapReservation&) = delete;
  BitmapReservation& operator=(const BitmapReservation&) = delete;
  BitmapReservation(BitmapReservation&& other) noexcept;
  BitmapReservation& operator=(BitmapReservation&& other) noexcept;

  // active 表示析构时仍会回滚；bit 返回本次预留的全局位号。
  bool active() const { return allocator_ != nullptr; }
  std::uint32_t bit() const { return bit_; }
  // 确认计划完整成功，解除自动回滚责任但保留 bitmap 中的 1。
  void KeepReserved();

 private:
  friend class BitmapAllocator;
  BitmapReservation(BitmapAllocator* allocator, std::uint32_t bit);
  void RollbackIfActive();

  BitmapAllocator* allocator_{nullptr};
  std::uint32_t bit_{0};
};

// 只修改调用者提供的内存 bitmap，不直接写磁盘。
// 内部 mutex 保护同一个 allocator 的并发预留；跨事务一致性仍由更外层挂载锁保证。
class BitmapAllocator {
 public:
  // valid_bits 排除 bitmap 尾部填充位；first_allocatable_bit 跳过保留区域。
  BitmapAllocator(std::vector<std::uint8_t>* bitmap,
                  std::uint32_t valid_bits,
                  std::uint32_t first_allocatable_bit);

  BitmapAllocator(const BitmapAllocator&) = delete;
  BitmapAllocator& operator=(const BitmapAllocator&) = delete;

  // 验证容量、尾部保留位和可分配起点是否满足分配器不变量。
  bool Validate(std::string* error) const;
  // 找到第一个空闲合法位，立即在内存置 1，并返回可自动回滚的 reservation。
  int Reserve(BitmapReservation* reservation, std::string* error);
  // 释放一个当前为 1 的合法位；重复释放或越界返回错误。
  int ReleaseAllocated(std::uint32_t bit, std::string* error);
  bool IsAllocated(std::uint32_t bit) const;
  // 取得当前内存 bitmap 副本，用于 planner 生成 after-image。
  std::vector<std::uint8_t> Snapshot() const;

 private:
  friend class BitmapReservation;
  void RollbackReservation(std::uint32_t bit);

  static bool TestBit(const std::vector<std::uint8_t>& bitmap,
                      std::uint32_t bit);
  static void SetBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit);
  static void ClearBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit);

  std::vector<std::uint8_t>* bitmap_;
  std::uint32_t valid_bits_;
  std::uint32_t first_allocatable_bit_;
  mutable std::mutex mutex_;
};

}  // namespace eufs::storage
