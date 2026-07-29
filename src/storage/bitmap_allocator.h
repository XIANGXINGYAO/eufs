#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace eufs::storage {

class BitmapAllocator;

class BitmapReservation {
 public:
  BitmapReservation() = default;
  ~BitmapReservation();

  BitmapReservation(const BitmapReservation&) = delete;
  BitmapReservation& operator=(const BitmapReservation&) = delete;
  BitmapReservation(BitmapReservation&& other) noexcept;
  BitmapReservation& operator=(BitmapReservation&& other) noexcept;

  bool active() const { return allocator_ != nullptr; }
  std::uint32_t bit() const { return bit_; }
  void KeepReserved();

 private:
  friend class BitmapAllocator;
  BitmapReservation(BitmapAllocator* allocator, std::uint32_t bit);
  void RollbackIfActive();

  BitmapAllocator* allocator_{nullptr};
  std::uint32_t bit_{0};
};

class BitmapAllocator {
 public:
  BitmapAllocator(std::vector<std::uint8_t>* bitmap,
                  std::uint32_t valid_bits,
                  std::uint32_t first_allocatable_bit);

  BitmapAllocator(const BitmapAllocator&) = delete;
  BitmapAllocator& operator=(const BitmapAllocator&) = delete;

  bool Validate(std::string* error) const;
  int Reserve(BitmapReservation* reservation, std::string* error);
  int ReleaseAllocated(std::uint32_t bit, std::string* error);
  bool IsAllocated(std::uint32_t bit) const;
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
