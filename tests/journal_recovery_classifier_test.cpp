#include "journal/journal_control_store.h"
#include "journal/ondisk_journal.h"
#include "journal/ring_reservation.h"
#include "metadata/ondisk_format.h"
#include "storage/mkfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool PreadAll(int fd, std::uint8_t* output, std::size_t size,
              std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

bool PwriteAll(int fd, const std::uint8_t* input, std::size_t size,
               std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pwrite(fd, input + completed, size - completed,
                               static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

std::uint64_t BlockOffset(std::uint32_t block) {
  return static_cast<std::uint64_t>(block) * eufs::ondisk::kBlockSize;
}

std::uint32_t RingBlocks(const eufs::ondisk::Superblock& superblock) {
  return superblock.journal.block_count -
         eufs::ondisk::kJournalControlBlockCount;
}

std::uint32_t Advance(std::uint32_t start, std::size_t distance,
                      std::uint32_t ring_blocks) {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(start) + distance) % ring_blocks);
}

std::uint32_t RingPhysicalBlock(
    const eufs::ondisk::Superblock& superblock, std::uint32_t ring_index) {
  return superblock.journal.start_block +
         eufs::ondisk::kJournalControlBlockCount + ring_index;
}

eufs::ondisk::Block ReadBlock(const std::string& path,
                              std::uint32_t block_number) {
  eufs::ondisk::Block block{};
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "open image for block read failed");
  Require(PreadAll(fd, block.data(), block.size(), BlockOffset(block_number)),
          "pread image block failed");
  close(fd);
  return block;
}

void WriteBlock(const std::string& path, std::uint32_t block_number,
                const eufs::ondisk::Block& block) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "open image for block write failed");
  Require(PwriteAll(fd, block.data(), block.size(), BlockOffset(block_number)),
          "pwrite image block failed");
  Require(fdatasync(fd) == 0, "fdatasync image block failed");
  close(fd);
}

std::vector<std::uint8_t> ReadImage(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "open image snapshot failed");
  struct stat image_stat {};
  Require(fstat(fd, &image_stat) == 0 && image_stat.st_size > 0,
          "fstat image snapshot failed");
  std::vector<std::uint8_t> bytes(
      static_cast<std::size_t>(image_stat.st_size));
  Require(PreadAll(fd, bytes.data(), bytes.size(), 0),
          "pread image snapshot failed");
  close(fd);
  return bytes;
}

std::string CreateImage(eufs::ondisk::Superblock* superblock) {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-recovery-test-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 8;
  std::string detail;
  Require(eufs::storage::FormatImage(options, superblock, &detail),
          detail.c_str());
  return options.image_path;
}

std::map<std::uint32_t, eufs::ondisk::Block> MakeMetadataImages() {
  std::map<std::uint32_t, eufs::ondisk::Block> images;
  images[1].fill(0x31);
  images[3].fill(0x73);
  return images;
}

std::unique_ptr<eufs::journal::JournalControlStore> OpenStore(
    const std::string& path) {
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  return store;
}

void WriteControlCopies(const std::string& path,
                        const eufs::ondisk::Superblock& superblock,
                        const eufs::journal::JournalControl& control) {
  eufs::ondisk::Block encoded{};
  std::string detail;
  Require(eufs::journal::EncodeControl(control, &encoded, nullptr, &detail),
          detail.c_str());
  WriteBlock(path, superblock.journal.start_block, encoded);
  WriteBlock(path, superblock.journal.start_block + 1U, encoded);
}

void RewriteCleanControl(const std::string& path,
                         const eufs::ondisk::Superblock& superblock,
                         std::uint32_t position) {
  eufs::journal::JournalControl control;
  control.ring_blocks = RingBlocks(superblock);
  control.filesystem_uuid = superblock.filesystem_uuid;
  control.generation = 9;
  control.head = position;
  control.tail = position;
  control.next_transaction_id = 17;
  WriteControlCopies(path, superblock, control);
}

eufs::journal::DurableJournalBody WriteExposedTransaction(
    const std::string& path,
    const std::map<std::uint32_t, eufs::ondisk::Block>& images,
    bool write_commit) {
  auto store = OpenStore(path);
  std::string detail;
  eufs::journal::RingReservationPlan reservation;
  Require(eufs::journal::PlanRingReservation(
              store->current(), images.size(), &reservation, &detail) == 0,
          detail.c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) == 0,
          detail.c_str());
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  if (write_commit) {
    Require(store->WriteCommit(&detail) == 0, detail.c_str());
  }
  return body;
}

eufs::journal::DescriptorRecord ReadDescriptor(
    const std::string& path, const eufs::ondisk::Superblock& superblock,
    std::uint32_t ring_index) {
  eufs::journal::DescriptorRecord descriptor;
  std::string detail;
  Require(eufs::journal::DecodeDescriptor(
              ReadBlock(path, RingPhysicalBlock(superblock, ring_index)),
              &descriptor, &detail),
          detail.c_str());
  return descriptor;
}

void WriteDescriptor(const std::string& path,
                     const eufs::ondisk::Superblock& superblock,
                     std::uint32_t ring_index,
                     const eufs::journal::DescriptorRecord& descriptor) {
  eufs::ondisk::Block encoded{};
  std::string detail;
  Require(eufs::journal::EncodeDescriptor(descriptor, &encoded, nullptr,
                                          &detail),
          detail.c_str());
  WriteBlock(path, RingPhysicalBlock(superblock, ring_index), encoded);
}

void WriteCommit(const std::string& path,
                 const eufs::ondisk::Superblock& superblock,
                 std::uint32_t ring_index,
                 const eufs::journal::CommitRecord& commit) {
  eufs::ondisk::Block encoded{};
  std::string detail;
  Require(eufs::journal::EncodeCommit(commit, &encoded, &detail),
          detail.c_str());
  WriteBlock(path, RingPhysicalBlock(superblock, ring_index), encoded);
}

void RequireReadOnlyClassification(
    const std::string& path, eufs::journal::RecoveryState expected,
    const std::map<std::uint32_t, eufs::ondisk::Block>* expected_images) {
  const auto before = ReadImage(path);
  auto store = OpenStore(path);
  eufs::journal::RecoveryState state = eufs::journal::RecoveryState::kEmpty;
  std::string detail;
  Require(store->ClassifyRecovery(&state, &detail) == 0, detail.c_str());
  Require(state == expected, "recovery classifier returned the wrong state");
  if (expected_images == nullptr) {
    Require(!store->validated_transaction().has_value(),
            "non-committed state exposed a validated transaction");
  } else {
    Require(store->validated_transaction().has_value(),
            "committed state omitted its validated transaction");
    Require(store->validated_transaction()->metadata_after_images() ==
                *expected_images,
            "validated payload images differ from the journal bytes");
  }
  Require(ReadImage(path) == before,
          "recovery classification modified the image");
}

void RequireCorruption(const std::string& path, const char* expected_detail) {
  const auto before = ReadImage(path);
  auto store = OpenStore(path);
  eufs::journal::RecoveryState state =
      eufs::journal::RecoveryState::kCommitted;
  std::string detail;
  Require(store->ClassifyRecovery(&state, &detail) == -EUCLEAN,
          "corrupt journal was not rejected with EUCLEAN");
  Require(state == eufs::journal::RecoveryState::kCommitted,
          "failed classification modified its output");
  Require(!store->validated_transaction().has_value(),
          "failed classification retained a validated transaction");
  Require(detail.find(expected_detail) != std::string::npos,
          "corruption detail did not identify the failed invariant");
  Require(ReadImage(path) == before,
          "failed recovery classification modified the image");
}

void TestEmptyAndCommitStates() {
  {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    RequireReadOnlyClassification(path, eufs::journal::RecoveryState::kEmpty,
                                  nullptr);
    unlink(path.c_str());
  }

  {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto images = MakeMetadataImages();
    const auto body = WriteExposedTransaction(path, images, true);
    auto store = OpenStore(path);
    eufs::journal::RecoveryState state{};
    std::string detail;
    Require(store->ClassifyRecovery(&state, &detail) == 0 &&
                state == eufs::journal::RecoveryState::kCommitted,
            detail.c_str());
    Require(store->validated_transaction().has_value() &&
                store->validated_transaction()->transaction_id() ==
                    body.reservation.transaction_id &&
                store->validated_transaction()->descriptor_ring_index() ==
                    body.reservation.descriptor_ring_index &&
                store->validated_transaction()->commit_ring_index() ==
                    body.reservation.commit_ring_index &&
                store->validated_transaction()->metadata_after_images() ==
                    images,
            "matching COMMIT did not produce the exact validated transaction");
    store.reset();
    RequireReadOnlyClassification(
        path, eufs::journal::RecoveryState::kCommitted, &images);
    unlink(path.c_str());
  }
}

void TestMissingTornAndStaleCommitAreUncommitted() {
  for (int mode = 0; mode < 3; ++mode) {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto images = MakeMetadataImages();

    if (mode == 2) {
      eufs::journal::CommitRecord stale;
      stale.transaction_id = 99;
      stale.filesystem_uuid = superblock.filesystem_uuid;
      stale.entry_count = 2;
      stale.transaction_block_count = 4;
      stale.descriptor_ring_index = 0;
      stale.descriptor_crc32c = 0x12345678U;
      WriteCommit(path, superblock, 3, stale);
    }

    const auto body = WriteExposedTransaction(path, images, false);
    if (mode == 1) {
      const auto descriptor =
          ReadDescriptor(path, superblock,
                         body.reservation.descriptor_ring_index);
      eufs::journal::CommitRecord matching;
      matching.transaction_id = descriptor.transaction_id;
      matching.filesystem_uuid = descriptor.filesystem_uuid;
      matching.entry_count =
          static_cast<std::uint32_t>(descriptor.entries.size());
      matching.transaction_block_count = descriptor.transaction_block_count;
      matching.descriptor_ring_index =
          body.reservation.descriptor_ring_index;
      matching.descriptor_crc32c = descriptor.checksum;
      eufs::ondisk::Block encoded{};
      std::string detail;
      Require(eufs::journal::EncodeCommit(matching, &encoded, &detail),
              detail.c_str());
      eufs::ondisk::Block torn{};
      std::copy_n(encoded.begin(), 32, torn.begin());
      WriteBlock(path,
                 RingPhysicalBlock(superblock,
                                   body.reservation.commit_ring_index),
                 torn);
    }

    RequireReadOnlyClassification(
        path, eufs::journal::RecoveryState::kUncommitted, nullptr);
    unlink(path.c_str());
  }
}

void TestExposedBodyCorruption() {
  {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto body = WriteExposedTransaction(path, MakeMetadataImages(), false);
    auto bytes = ReadBlock(
        path, RingPhysicalBlock(superblock,
                                body.reservation.descriptor_ring_index));
    bytes[200] ^= 0x80U;
    WriteBlock(path,
               RingPhysicalBlock(superblock,
                                 body.reservation.descriptor_ring_index),
               bytes);
    RequireCorruption(path, "descriptor");
    unlink(path.c_str());
  }

  {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto body = WriteExposedTransaction(path, MakeMetadataImages(), false);
    auto payload = ReadBlock(
        path, RingPhysicalBlock(superblock,
                                body.reservation.payload_ring_indices[1]));
    payload[17] ^= 0x01U;
    WriteBlock(path,
               RingPhysicalBlock(superblock,
                                 body.reservation.payload_ring_indices[1]),
               payload);
    RequireCorruption(path, "payload checksum");
    unlink(path.c_str());
  }
}

void TestStrictGrammarAndControlLength() {
  {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto body = WriteExposedTransaction(path, MakeMetadataImages(), false);
    auto descriptor = ReadDescriptor(
        path, superblock, body.reservation.descriptor_ring_index);
    descriptor.entries[1].payload_ring_index =
        body.reservation.commit_ring_index;
    WriteDescriptor(path, superblock, body.reservation.descriptor_ring_index,
                    descriptor);
    RequireCorruption(path, "payload position");
    unlink(path.c_str());
  }

  {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    WriteExposedTransaction(path, MakeMetadataImages(), false);
    auto store = OpenStore(path);
    auto control = store->current();
    store.reset();
    control.generation += 1U;
    control.used_blocks = 3;
    control.head = Advance(control.tail, control.used_blocks,
                           control.ring_blocks);
    control.checksum = 0;
    WriteControlCopies(path, superblock, control);
    RequireCorruption(path, "length");
    unlink(path.c_str());
  }
}

void TestDescriptorContextBinding() {
  for (int mode = 0; mode < 3; ++mode) {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto body = WriteExposedTransaction(path, MakeMetadataImages(), false);
    auto descriptor = ReadDescriptor(
        path, superblock, body.reservation.descriptor_ring_index);
    const char* expected_detail = nullptr;
    if (mode == 0) {
      descriptor.filesystem_uuid[0] ^= 0x01U;
      expected_detail = "UUID";
    } else if (mode == 1) {
      descriptor.transaction_id += 1U;
      expected_detail = "transaction id";
    } else {
      descriptor.entries[0].home_block = superblock.journal.start_block;
      expected_detail = "forbidden";
    }
    WriteDescriptor(path, superblock, body.reservation.descriptor_ring_index,
                    descriptor);
    RequireCorruption(path, expected_detail);
    unlink(path.c_str());
  }
}

void TestCurrentCommitBindingMismatchIsCorruption() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  const auto body = WriteExposedTransaction(path, MakeMetadataImages(), false);
  const auto descriptor =
      ReadDescriptor(path, superblock, body.reservation.descriptor_ring_index);

  eufs::journal::CommitRecord commit;
  commit.transaction_id = descriptor.transaction_id;
  commit.filesystem_uuid = descriptor.filesystem_uuid;
  commit.entry_count = static_cast<std::uint32_t>(descriptor.entries.size());
  commit.transaction_block_count = descriptor.transaction_block_count;
  commit.descriptor_ring_index = body.reservation.descriptor_ring_index;
  commit.descriptor_crc32c = descriptor.checksum ^ 0x01U;
  WriteCommit(path, superblock, body.reservation.commit_ring_index, commit);

  RequireCorruption(path, "COMMIT");
  unlink(path.c_str());
}

void TestClassifierDoesNotScanForAnotherDescriptor() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  const std::uint32_t ring_blocks = RingBlocks(superblock);

  eufs::ondisk::Block payload{};
  payload.fill(0xA5);
  eufs::journal::DescriptorRecord descriptor;
  descriptor.transaction_id = 1;
  descriptor.filesystem_uuid = superblock.filesystem_uuid;
  descriptor.transaction_block_count = 3;
  descriptor.entries.push_back(eufs::journal::DescriptorEntry{
      1, 2, eufs::ondisk::Crc32c(payload.data(), payload.size()), 0});
  eufs::ondisk::Block descriptor_bytes{};
  std::uint32_t descriptor_crc = 0;
  std::string detail;
  Require(eufs::journal::EncodeDescriptor(
              descriptor, &descriptor_bytes, &descriptor_crc, &detail),
          detail.c_str());

  eufs::journal::CommitRecord commit;
  commit.transaction_id = 1;
  commit.filesystem_uuid = superblock.filesystem_uuid;
  commit.entry_count = 1;
  commit.transaction_block_count = 3;
  commit.descriptor_ring_index = 1;
  commit.descriptor_crc32c = descriptor_crc;
  eufs::ondisk::Block commit_bytes{};
  Require(eufs::journal::EncodeCommit(commit, &commit_bytes, &detail),
          detail.c_str());

  eufs::ondisk::Block invalid_tail{};
  invalid_tail.fill(0xCC);
  WriteBlock(path, RingPhysicalBlock(superblock, 0), invalid_tail);
  WriteBlock(path, RingPhysicalBlock(superblock, 1), descriptor_bytes);
  WriteBlock(path, RingPhysicalBlock(superblock, 2), payload);
  WriteBlock(path, RingPhysicalBlock(superblock, 3), commit_bytes);

  eufs::journal::JournalControl control;
  control.ring_blocks = ring_blocks;
  control.filesystem_uuid = superblock.filesystem_uuid;
  control.generation = 10;
  control.tail = 0;
  control.used_blocks = 4;
  control.head = 4 % ring_blocks;
  control.next_transaction_id = 2;
  WriteControlCopies(path, superblock, control);

  RequireCorruption(path, "descriptor");
  unlink(path.c_str());
}

void TestWrappedCommittedTransaction() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  const std::uint32_t ring_blocks = RingBlocks(superblock);
  RewriteCleanControl(path, superblock, ring_blocks - 1U);
  const auto images = MakeMetadataImages();
  const auto body = WriteExposedTransaction(path, images, true);
  Require(body.reservation.descriptor_ring_index == ring_blocks - 1U &&
              body.reservation.payload_ring_indices ==
                  std::vector<std::uint32_t>({0, 1}) &&
              body.reservation.commit_ring_index == 2,
          "wrapped recovery fixture did not cross the ring boundary");
  RequireReadOnlyClassification(
      path, eufs::journal::RecoveryState::kCommitted, &images);
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestEmptyAndCommitStates();
  TestMissingTornAndStaleCommitAreUncommitted();
  TestExposedBodyCorruption();
  TestStrictGrammarAndControlLength();
  TestDescriptorContextBinding();
  TestCurrentCommitBindingMismatchIsCorruption();
  TestClassifierDoesNotScanForAnotherDescriptor();
  TestWrappedCommittedTransaction();
  std::cout << "empty=ok uncommitted=missing_torn_stale committed=validated "
               "grammar=strict context=bound no_magic_scan=ok readonly=ok\n";
  std::cout << "PASS: journal recovery classifier test\n";
  return 0;
}
