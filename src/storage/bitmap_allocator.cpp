#include "storage/bitmap_allocator.h"

#include <cerrno>
#include <cstddef>
#include <string_view>
#include <utility>

namespace eufs::storage {
namespace {

// 写入可选错误文本。
void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
}

}  // namespace

// reservation 构造后立即对给定位承担自动回滚责任。
BitmapReservation::BitmapReservation(BitmapAllocator* allocator,
                                     std::uint32_t bit)
    : allocator_(allocator), bit_(bit) {}

// 未 KeepReserved 的临时预留在离开作用域时自动撤销。
BitmapReservation::~BitmapReservation() { RollbackIfActive(); }

// 移动构造转移唯一回滚责任，并把源对象置为 inactive。
BitmapReservation::BitmapReservation(BitmapReservation&& other) noexcept
    : allocator_(other.allocator_), bit_(other.bit_) {
  other.allocator_ = nullptr;
  other.bit_ = 0;
}

// 移动赋值前先回滚本对象原预留，再接管源对象责任。
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

// planner 完整成功后解除自动回滚；bitmap 中已经置 1 的位保持不变。
void BitmapReservation::KeepReserved() {
  allocator_ = nullptr;
}

// 仅 active reservation 才调用 allocator 清位，确保最多回滚一次。
void BitmapReservation::RollbackIfActive() {
  if (allocator_ != nullptr) {
    allocator_->RollbackReservation(bit_);
    allocator_ = nullptr;
    bit_ = 0;
  }
}

// allocator 直接引用 planner 加载的 bitmap 内存副本，不拥有该 vector。
BitmapAllocator::BitmapAllocator(std::vector<std::uint8_t>* bitmap,
                                 std::uint32_t valid_bits,
                                 std::uint32_t first_allocatable_bit)
    : bitmap_(bitmap),
      valid_bits_(valid_bits),
      first_allocatable_bit_(first_allocatable_bit) {}

// 位号按 byte=bit/8、mask=1<<(bit%8) 映射到字节数组。
bool BitmapAllocator::TestBit(const std::vector<std::uint8_t>& bitmap,
                              std::uint32_t bit) {
  return (bitmap[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) !=
         0;
}

// 把指定位设置为已分配。
void BitmapAllocator::SetBit(std::vector<std::uint8_t>* bitmap,
                             std::uint32_t bit) {
  (*bitmap)[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

// 把指定位清为未分配。
void BitmapAllocator::ClearBit(std::vector<std::uint8_t>* bitmap,
                               std::uint32_t bit) {
  (*bitmap)[bit / 8U] &=
      static_cast<std::uint8_t>(~static_cast<std::uint8_t>(1U << (bit % 8U)));
}

// 验证 bitmap 几何以及保留前缀/尾部均已标记占用。
bool BitmapAllocator::Validate(std::string* error) const {
  // 所有公开读写都持有内部锁，防止同一 allocator 并发修改 vector。
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
  // [0, first_allocatable_bit) 是 metadata 或保留编号，必须全部为 1。
  for (std::uint32_t bit = 0; bit < first_allocatable_bit_; ++bit) {
    if (!TestBit(*bitmap_, bit)) {
      SetError(error, "reserved bitmap prefix contains a free bit");
      return false;
    }
  }
  // [valid_bits, capacity_bits) 是字节对齐填充尾部，也必须为 1 防止误分配。
  for (std::uint64_t bit = valid_bits_; bit < capacity_bits; ++bit) {
    if (!TestBit(*bitmap_, static_cast<std::uint32_t>(bit))) {
      SetError(error, "bitmap tail contains an allocatable bit");
      return false;
    }
  }
  return true;
}

// 从可分配范围线性寻找第一个 0 位，并返回带 RAII 回滚的 reservation。
int BitmapAllocator::Reserve(BitmapReservation* reservation,
                             std::string* error) {
  // 输出必须为空闲 reservation，避免覆盖仍承担回滚责任的旧对象。
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
  // v1 使用确定性的 first-fit 分配，便于恢复测试和证据复现。
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

// 释放已经分配的合法位；重复释放视为结构矛盾 EUCLEAN。
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

// 线程安全查询一个有效位当前是否为 1。
bool BitmapAllocator::IsAllocated(std::uint32_t bit) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return bitmap_ != nullptr && bit < valid_bits_ && TestBit(*bitmap_, bit);
}

// 返回锁保护下的一致 bitmap 副本，供 planner 比较 before/after 块。
std::vector<std::uint8_t> BitmapAllocator::Snapshot() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return bitmap_ == nullptr ? std::vector<std::uint8_t>{} : *bitmap_;
}

// Reservation 析构时调用；范围和当前位状态都再次防御检查。
void BitmapAllocator::RollbackReservation(std::uint32_t bit) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (bitmap_ != nullptr && bit >= first_allocatable_bit_ &&
      bit < valid_bits_ && TestBit(*bitmap_, bit)) {
    ClearBit(bitmap_, bit);
  }
}

}  // namespace eufs::storage
