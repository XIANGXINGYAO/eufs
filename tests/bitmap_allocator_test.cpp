// 验证 bitmap 分配器只选择空闲位，并正确处理保留区、越界和空间耗尽。
// 这些断言保护 metadata planner 的最底层资源分配前提。
#include "storage/bitmap_allocator.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void SetBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit) {
  (*bitmap)[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

void FillReservedTail(std::vector<std::uint8_t>* bitmap,
                      std::uint32_t valid_bits) {
  for (std::uint32_t bit = valid_bits;
       bit < bitmap->size() * 8U; ++bit) {
    SetBit(bitmap, bit);
  }
}

void TestTouchReservations() {
  std::vector<std::uint8_t> inode_bitmap(2, 0);
  SetBit(&inode_bitmap, 0);
  FillReservedTail(&inode_bitmap, 8);
  eufs::storage::BitmapAllocator inode_allocator(&inode_bitmap, 8, 0);

  std::vector<std::uint8_t> block_bitmap(40, 0);
  for (std::uint32_t block = 0; block < 291; ++block) {
    SetBit(&block_bitmap, block);
  }
  FillReservedTail(&block_bitmap, 300);
  eufs::storage::BitmapAllocator block_allocator(&block_bitmap, 300, 291);

  std::string error;
  Require(inode_allocator.Validate(&error), error.c_str());
  Require(block_allocator.Validate(&error), error.c_str());

  eufs::storage::BitmapReservation inode;
  eufs::storage::BitmapReservation block;
  Require(inode_allocator.Reserve(&inode, &error) == 0 && inode.bit() == 1,
          "touch did not reserve inode bitmap bit 1 for inode 2");
  Require(block_allocator.Reserve(&block, &error) == 0 && block.bit() == 291,
          "touch did not reserve the first data block 291");
  inode.KeepReserved();
  block.KeepReserved();
  Require(inode_allocator.IsAllocated(1), "committed inode reservation vanished");
  Require(block_allocator.IsAllocated(291), "committed block reservation vanished");
  Require(block_allocator.ReleaseAllocated(291, &error) == 0 &&
              !block_allocator.IsAllocated(291),
          "allocated block bitmap bit was not released");
  Require(block_allocator.ReleaseAllocated(291, &error) == -EUCLEAN,
          "double release did not report inconsistent bitmap state");
  Require(block_allocator.ReleaseAllocated(290, &error) == -EINVAL,
          "reserved bitmap prefix was released");
}

void TestRollback() {
  std::vector<std::uint8_t> bitmap(2, 0);
  FillReservedTail(&bitmap, 8);
  eufs::storage::BitmapAllocator allocator(&bitmap, 8, 0);
  std::string error;
  {
    eufs::storage::BitmapReservation reservation;
    Require(allocator.Reserve(&reservation, &error) == 0 &&
                reservation.bit() == 0,
            "rollback test could not reserve bit zero");
    Require(allocator.IsAllocated(0), "active reservation is not visible");
  }
  Require(!allocator.IsAllocated(0),
          "uncommitted reservation was not rolled back on destruction");
}

void TestConcurrentReservationsAreUnique() {
  constexpr std::uint32_t kReservations = 32;
  std::vector<std::uint8_t> bitmap(8, 0);
  FillReservedTail(&bitmap, kReservations);
  eufs::storage::BitmapAllocator allocator(&bitmap, kReservations, 0);
  std::array<std::uint32_t, kReservations> allocated{};
  std::vector<std::thread> threads;
  threads.reserve(kReservations);
  for (std::uint32_t index = 0; index < kReservations; ++index) {
    threads.emplace_back([&allocator, &allocated, index]() {
      eufs::storage::BitmapReservation reservation;
      std::string error;
      const int result = allocator.Reserve(&reservation, &error);
      Require(result == 0, "concurrent reservation unexpectedly failed");
      allocated[index] = reservation.bit();
      reservation.KeepReserved();
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const std::set<std::uint32_t> unique(allocated.begin(), allocated.end());
  Require(unique.size() == kReservations,
          "concurrent reservations returned a duplicate bit");

  eufs::storage::BitmapReservation extra;
  std::string error;
  Require(allocator.Reserve(&extra, &error) == -ENOSPC,
          "full bitmap did not return ENOSPC");
}

void TestValidationRejectsFreeReservedBits() {
  std::vector<std::uint8_t> bitmap(2, 0);
  FillReservedTail(&bitmap, 8);
  eufs::storage::BitmapAllocator allocator(&bitmap, 8, 3);
  std::string error;
  Require(!allocator.Validate(&error),
          "allocator accepted a free bit in its reserved prefix");
}

}  // namespace

int main() {
  TestTouchReservations();
  TestRollback();
  TestConcurrentReservationsAreUnique();
  TestValidationRejectsFreeReservedBits();
  std::cout << "PASS: bitmap allocator reservation tests\n";
  return 0;
}
