#include "storage/bitmap_allocator.h"

#include <cerrno>
#include <cstddef>
#include <string_view>
#include <utility>

namespace eufs::storage {
namespace {

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
}

}  // namespace

BitmapReservation::BitmapReservation(BitmapAllocator* allocator,
                                     std::uint32_t bit)
    : allocator_(allocator), bit_(bit) {}

BitmapReservation::~BitmapReservation() { RollbackIfActive(); }

BitmapReservation::BitmapReservation(BitmapReservation&& other) noexcept
    : allocator_(other.allocator_), bit_(other.bit_) {
  other.allocator_ = nullptr;
  other.bit_ = 0;
}

BitmapReservation& BitmapReservation::operator=(
    BitmapReservation&& other) noexcept {
  if (this != &other) {
    RollbackIfActive();
    allocator_ = other.allocator_;
    bit_ = other.bit_;
    other.allocator_ = nullptr;
    other.bit_ = 0;
  }
  return *this;
}

void BitmapReservation::KeepReserved() {
  allocator_ = nullptr;
}

void BitmapReservation::RollbackIfActive() {
  if (allocator_ != nullptr) {
    allocator_->RollbackReservation(bit_);
    allocator_ = nullptr;
    bit_ = 0;
  }
}

BitmapAllocator::BitmapAllocator(std::vector<std::uint8_t>* bitmap,
                                 std::uint32_t valid_bits,
                                 std::uint32_t first_allocatable_bit)
    : bitmap_(bitmap),
      valid_bits_(valid_bits),
      first_allocatable_bit_(first_allocatable_bit) {}

bool BitmapAllocator::TestBit(const std::vector<std::uint8_t>& bitmap,
                              std::uint32_t bit) {
  return (bitmap[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) !=
         0;
}

void BitmapAllocator::SetBit(std::vector<std::uint8_t>* bitmap,
                             std::uint32_t bit) {
  (*bitmap)[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

void BitmapAllocator::ClearBit(std::vector<std::uint8_t>* bitmap,
                               std::uint32_t bit) {
  (*bitmap)[bit / 8U] &=
      static_cast<std::uint8_t>(~static_cast<std::uint8_t>(1U << (bit % 8U)));
}

bool BitmapAllocator::Validate(std::string* error) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (bitmap_ == nullptr || valid_bits_ == 0 ||
      first_allocatable_bit_ > valid_bits_) {
    SetError(error, "bitmap allocator geometry is invalid");
    return false;
  }
  const std::uint64_t capacity_bits = bitmap_->size() * 8ULL;
  if (capacity_bits < valid_bits_) {
    SetError(error, "bitmap storage is smaller than its valid range");
    return false;
  }
  for (std::uint32_t bit = 0; bit < first_allocatable_bit_; ++bit) {
    if (!TestBit(*bitmap_, bit)) {
      SetError(error, "reserved bitmap prefix contains a free bit");
      return false;
    }
  }
  for (std::uint64_t bit = valid_bits_; bit < capacity_bits; ++bit) {
    if (!TestBit(*bitmap_, static_cast<std::uint32_t>(bit))) {
      SetError(error, "bitmap tail contains an allocatable bit");
      return false;
    }
  }
  return true;
}

int BitmapAllocator::Reserve(BitmapReservation* reservation,
                             std::string* error) {
  if (reservation == nullptr || reservation->active()) {
    SetError(error, "reservation output is null or already active");
    return -EINVAL;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  if (bitmap_ == nullptr || bitmap_->size() * 8ULL < valid_bits_ ||
      first_allocatable_bit_ > valid_bits_) {
    SetError(error, "bitmap allocator geometry is invalid");
    return -EINVAL;
  }
  for (std::uint32_t bit = first_allocatable_bit_; bit < valid_bits_; ++bit) {
    if (!TestBit(*bitmap_, bit)) {
      SetBit(bitmap_, bit);
      *reservation = BitmapReservation(this, bit);
      return 0;
    }
  }
  SetError(error, "bitmap has no free allocatable bit");
  return -ENOSPC;
}

int BitmapAllocator::ReleaseAllocated(std::uint32_t bit,
                                      std::string* error) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (bitmap_ == nullptr || bitmap_->size() * 8ULL < valid_bits_ ||
      first_allocatable_bit_ > valid_bits_ || bit < first_allocatable_bit_ ||
      bit >= valid_bits_) {
    SetError(error, "released bitmap bit is outside the allocatable range");
    return -EINVAL;
  }
  if (!TestBit(*bitmap_, bit)) {
    SetError(error, "released bitmap bit is already free");
    return -EUCLEAN;
  }
  ClearBit(bitmap_, bit);
  return 0;
}

bool BitmapAllocator::IsAllocated(std::uint32_t bit) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return bitmap_ != nullptr && bit < valid_bits_ && TestBit(*bitmap_, bit);
}

std::vector<std::uint8_t> BitmapAllocator::Snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return bitmap_ == nullptr ? std::vector<std::uint8_t>{} : *bitmap_;
}

void BitmapAllocator::RollbackReservation(std::uint32_t bit) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (bitmap_ != nullptr && bit >= first_allocatable_bit_ &&
      bit < valid_bits_ && TestBit(*bitmap_, bit)) {
    ClearBit(bitmap_, bit);
  }
}

}  // namespace eufs::storage
