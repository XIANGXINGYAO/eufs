#include "journal/journal_control_store.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <utility>
#include <unistd.h>

namespace eufs::journal {
namespace {

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return value_; }
  int Release() {
    const int value = value_;
    value_ = -1;
    return value;
  }

 private:
  int value_;
};

class SystemJournalControlIo final : public JournalControlIo {
 public:
  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    return pwrite(fd, input, size, offset);
  }

  int Fdatasync(int fd) override { return fdatasync(fd); }
};

std::shared_ptr<JournalControlIo> SystemIo() {
  static const auto io = std::make_shared<SystemJournalControlIo>();
  return io;
}

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

int PreadAll(int fd, std::uint8_t* output, std::size_t size,
             std::uint64_t offset, std::string_view operation,
             std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, operation, error_number);
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

int PwriteAll(JournalControlIo* io, int fd, const std::uint8_t* input,
              std::size_t size, std::uint64_t offset,
              std::string_view operation, std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    errno = 0;
    const auto result = io->Pwrite(
        fd, input + completed, size - completed,
        static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 && errno != 0 ? errno : EIO;
      SetSystemDetail(detail, operation, error_number);
      return -error_number;
    }
    if (static_cast<std::size_t>(result) > size - completed) {
      if (detail != nullptr) {
        detail->assign(operation);
        detail->append(" returned an invalid length");
      }
      return -EIO;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

bool HasSameIdentity(const JournalControl& first,
                     const JournalControl& second) {
  return first.ring_blocks == second.ring_blocks &&
         first.filesystem_uuid == second.filesystem_uuid &&
         first.state_flags == second.state_flags &&
         first.feature_compat == second.feature_compat &&
         first.feature_ro_compat == second.feature_ro_compat &&
         first.feature_incompat == second.feature_incompat;
}

bool SameControlState(const JournalControl& first,
                      const JournalControl& second) {
  return HasSameIdentity(first, second) &&
         first.filesystem_uuid == second.filesystem_uuid &&
         first.generation == second.generation && first.head == second.head &&
         first.tail == second.tail &&
         first.used_blocks == second.used_blocks &&
         first.next_transaction_id == second.next_transaction_id;
}

bool SameReservation(const RingReservationPlan& first,
                     const RingReservationPlan& second) {
  return first.transaction_id == second.transaction_id &&
         first.descriptor_ring_index == second.descriptor_ring_index &&
         first.payload_ring_indices == second.payload_ring_indices &&
         first.commit_ring_index == second.commit_ring_index &&
         SameControlState(first.exposed_control, second.exposed_control);
}

bool BlockIsInRegion(std::uint32_t block, const ondisk::Region& region) {
  return block >= region.start_block &&
         static_cast<std::uint64_t>(block) <
             static_cast<std::uint64_t>(region.start_block) +
                 region.block_count;
}

bool IsValidMetadataTarget(const ondisk::Superblock& superblock,
                           std::uint32_t block) {
  return block != 0 && block < superblock.total_blocks &&
         !BlockIsInRegion(block, superblock.journal);
}

bool RingOffset(const ondisk::Superblock& superblock,
                std::uint32_t ring_index, std::uint64_t* output,
                std::string* detail) {
  const std::uint32_t ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  if (output == nullptr || ring_index >= ring_blocks) {
    SetDetail(detail, "journal ring index is outside the transaction ring");
    return false;
  }
  const std::uint64_t physical_block =
      static_cast<std::uint64_t>(superblock.journal.start_block) +
      ondisk::kJournalControlBlockCount + ring_index;
  const std::uint64_t journal_end =
      static_cast<std::uint64_t>(superblock.journal.start_block) +
      superblock.journal.block_count;
  if (physical_block >= journal_end ||
      physical_block >= superblock.total_blocks) {
    SetDetail(detail, "journal ring position maps outside the image region");
    return false;
  }
  *output = physical_block * ondisk::kBlockSize;
  return true;
}

int LoadControlStateFromFd(int fd, ondisk::Superblock* superblock_output,
                           JournalControl* current_output,
                           ControlCopy* current_copy_output,
                           std::string* detail) {
  struct stat image_stat {};
  if (fstat(fd, &image_stat) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "fstat journal image", error_number);
    return -error_number;
  }
  if (image_stat.st_size < static_cast<off_t>(ondisk::kBlockSize) ||
      image_stat.st_size % ondisk::kBlockSize != 0) {
    SetDetail(detail, "journal image size is not a positive 4 KiB multiple");
    return -EUCLEAN;
  }

  ondisk::Block superblock_bytes{};
  int result = PreadAll(fd, superblock_bytes.data(), superblock_bytes.size(), 0,
                        "read superblock", detail);
  if (result != 0) {
    return result;
  }
  ondisk::Superblock superblock;
  if (!ondisk::DecodeSuperblock(superblock_bytes, &superblock, detail)) {
    return -EUCLEAN;
  }
  if (superblock.feature_incompat != 0) {
    SetDetail(detail, "image requires unsupported incompatible features");
    return -EOPNOTSUPP;
  }
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(superblock.total_blocks) *
      ondisk::kBlockSize;
  if (expected_size != static_cast<std::uint64_t>(image_stat.st_size)) {
    SetDetail(detail, "image size does not match the superblock");
    return -EUCLEAN;
  }

  ondisk::Block control_a{};
  ondisk::Block control_b{};
  const std::uint64_t control_a_offset =
      static_cast<std::uint64_t>(superblock.journal.start_block) *
      ondisk::kBlockSize;
  result = PreadAll(fd, control_a.data(), control_a.size(),
                    control_a_offset, "read journal control A", detail);
  if (result == 0) {
    result = PreadAll(fd, control_b.data(), control_b.size(),
                      control_a_offset + ondisk::kBlockSize,
                      "read journal control B", detail);
  }
  if (result != 0) {
    return result;
  }

  JournalControl current;
  ControlCopy current_copy{};
  const std::uint32_t expected_ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  if (!SelectControl(control_a, control_b, superblock.filesystem_uuid,
                     expected_ring_blocks, &current, &current_copy, detail)) {
    return -EUCLEAN;
  }

  *superblock_output = superblock;
  *current_output = current;
  *current_copy_output = current_copy;
  return 0;
}

}  // namespace

JournalControlStore::JournalControlStore(
    int fd, const ondisk::Superblock& superblock,
    const JournalControl& current, ControlCopy current_copy,
    std::shared_ptr<JournalControlIo> io,
    std::shared_ptr<DurableStageObserver> observer)
    : fd_(fd),
      superblock_(superblock),
      current_(current),
      current_copy_(current_copy),
      io_(io),
      observer_(std::move(observer)) {}

JournalControlStore::~JournalControlStore() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

int JournalControlStore::Open(
    const std::string& image_path,
    std::unique_ptr<JournalControlStore>* output, std::string* detail,
    std::shared_ptr<JournalControlIo> io,
    std::shared_ptr<DurableStageObserver> observer) {
  if (image_path.empty() || output == nullptr) {
    SetDetail(detail, "image path and control store output are required");
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  const int raw_fd = open(image_path.c_str(), O_RDWR | O_CLOEXEC);
  if (raw_fd < 0) {
    SetSystemDetail(detail, "open journal image", errno);
    return -errno;
  }
  FileDescriptor fd(raw_fd);
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "lock journal image", error_number);
    return error_number == EWOULDBLOCK ? -EBUSY : -error_number;
  }

  ondisk::Superblock superblock;
  JournalControl current;
  ControlCopy current_copy{};
  const int result = LoadControlStateFromFd(
      fd.get(), &superblock, &current, &current_copy, detail);
  if (result != 0) {
    return result;
  }
  output->reset(new JournalControlStore(
      fd.Release(), superblock, current, current_copy,
      io == nullptr ? SystemIo() : std::move(io), std::move(observer)));
  return 0;
}

int JournalControlStore::AdoptLockedFd(
    int locked_fd, std::unique_ptr<JournalControlStore>* output,
    std::string* detail, std::shared_ptr<JournalControlIo> io,
    std::shared_ptr<DurableStageObserver> observer) {
  FileDescriptor fd(locked_fd);
  if (output == nullptr || locked_fd < 0) {
    SetDetail(detail, "valid locked fd and control store output are required");
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  const int status_flags = fcntl(fd.get(), F_GETFL);
  if (status_flags < 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "inspect adopted journal fd", error_number);
    return -error_number;
  }
  if ((status_flags & O_ACCMODE) != O_RDWR) {
    SetDetail(detail, "adopted journal fd is not open for read and write");
    return -EACCES;
  }

  ondisk::Superblock superblock;
  JournalControl current;
  ControlCopy current_copy{};
  const int result = LoadControlStateFromFd(
      fd.get(), &superblock, &current, &current_copy, detail);
  if (result != 0) {
    return result;
  }
  output->reset(new JournalControlStore(
      fd.Release(), superblock, current, current_copy,
      io == nullptr ? SystemIo() : std::move(io), std::move(observer)));
  return 0;
}

int JournalControlStore::WriteOrderedDataAndUnexposedBody(
    std::uint32_t expected_total_blocks,
    const std::array<std::uint8_t, 16>& expected_filesystem_uuid,
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>& ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    DurableJournalBody* output, std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr || metadata_after_images.empty()) {
    SetDetail(detail,
              "ordered data, metadata, and durable body output are required");
    return -EINVAL;
  }
  if (reload_required_) {
    SetDetail(detail,
              "previous journal write has uncertain durability; reopen and "
              "reselect before any further write");
    return -EIO;
  }
  if (current_.used_blocks != 0 || durable_body_.has_value()) {
    SetDetail(detail,
              "v1 already has an exposed or durable journal transaction");
    return -EBUSY;
  }
  if (expected_total_blocks != superblock_.total_blocks ||
      expected_filesystem_uuid != superblock_.filesystem_uuid) {
    SetDetail(detail,
              "first-write transaction plan belongs to another image");
    return -ESTALE;
  }

  std::set<std::uint32_t> expected_before_blocks;
  for (const auto& [block, after_image] : ordered_data_after_images) {
    (void)after_image;
    expected_before_blocks.insert(block);
    if (!BlockIsInRegion(block, superblock_.data) ||
        before_images.find(block) == before_images.end() ||
        metadata_after_images.find(block) != metadata_after_images.end()) {
      SetDetail(detail,
                "ordered-data target is invalid, unvalidated, or metadata");
      return -EINVAL;
    }
  }
  for (const auto& [block, after_image] : metadata_after_images) {
    (void)after_image;
    expected_before_blocks.insert(block);
    if (!IsValidMetadataTarget(superblock_, block) ||
        before_images.find(block) == before_images.end()) {
      SetDetail(detail, "metadata target is invalid or lacks a before-image");
      return -EINVAL;
    }
  }
  if (before_images.size() != expected_before_blocks.size()) {
    SetDetail(detail,
              "before-images must exactly cover the distinct changed home "
              "blocks");
    return -EINVAL;
  }
  for (const auto& [block, before_image] : before_images) {
    (void)before_image;
    if (expected_before_blocks.find(block) == expected_before_blocks.end()) {
      SetDetail(detail, "before-images contain an unrelated home block");
      return -EINVAL;
    }
  }

  RingReservationPlan reservation;
  int result = PlanRingReservation(current_, metadata_after_images.size(),
                                   &reservation, detail);
  if (result != 0) {
    return result;
  }

  for (const auto& [block, expected] : before_images) {
    if (block >= superblock_.total_blocks) {
      SetDetail(detail, "before-image target is outside the image");
      return -EINVAL;
    }
    ondisk::Block current{};
    result = PreadAll(fd_, current.data(), current.size(),
                      static_cast<std::uint64_t>(block) * ondisk::kBlockSize,
                      "pread transaction before-image", detail);
    if (result != 0) {
      return result;
    }
    if (current != expected) {
      SetDetail(detail,
                "home block changed after first-write transaction planning");
      return -ESTALE;
    }
  }

  if (!ordered_data_after_images.empty()) {
    for (const auto& [block, after_image] : ordered_data_after_images) {
      result = PwriteAll(
          io_.get(), fd_, after_image.data(), after_image.size(),
          static_cast<std::uint64_t>(block) * ondisk::kBlockSize,
          "pwrite ordered data", detail);
      if (result != 0) {
        reload_required_ = true;
        return result;
      }
    }
    errno = 0;
    if (io_->Fdatasync(fd_) != 0) {
      const int error_number = errno != 0 ? errno : EIO;
      SetSystemDetail(detail, "fdatasync ordered data", error_number);
      reload_required_ = true;
      return -error_number;
    }
    if (observer_ != nullptr) {
      observer_->OnDurableStage(DurableStage::kOrderedData);
    }
  }

  return WriteUnexposedBody(reservation, metadata_after_images, output,
                            detail);
}

int JournalControlStore::WriteUnexposedBody(
    const RingReservationPlan& reservation,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    DurableJournalBody* output, std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "durable journal body output is required");
    return -EINVAL;
  }
  if (reload_required_) {
    SetDetail(detail,
              "previous journal write has uncertain durability; reopen and "
              "reselect before any further write");
    return -EIO;
  }
  if (current_.used_blocks != 0 || durable_body_.has_value()) {
    SetDetail(detail,
              "v1 already has an exposed or durable journal transaction");
    return -EBUSY;
  }

  RingReservationPlan expected;
  int result = PlanRingReservation(current_, metadata_after_images.size(),
                                   &expected, detail);
  if (result != 0) {
    return result;
  }
  if (!SameReservation(reservation, expected)) {
    SetDetail(detail,
              "ring reservation no longer matches the selected control");
    return -ESTALE;
  }

  DescriptorRecord descriptor;
  descriptor.transaction_id = reservation.transaction_id;
  descriptor.filesystem_uuid = superblock_.filesystem_uuid;
  descriptor.transaction_block_count =
      static_cast<std::uint32_t>(metadata_after_images.size() + 2U);
  descriptor.entries.reserve(metadata_after_images.size());

  std::size_t payload_index = 0;
  for (const auto& [home_block, payload] : metadata_after_images) {
    if (!IsValidMetadataTarget(superblock_, home_block)) {
      SetDetail(detail,
                "metadata after-image targets a forbidden home block");
      return -EINVAL;
    }
    descriptor.entries.push_back(DescriptorEntry{
        home_block, reservation.payload_ring_indices[payload_index],
        ondisk::Crc32c(payload.data(), payload.size()), 0});
    ++payload_index;
  }

  ondisk::Block descriptor_bytes{};
  std::uint32_t descriptor_checksum = 0;
  if (!EncodeDescriptor(descriptor, &descriptor_bytes, &descriptor_checksum,
                        detail)) {
    return -EINVAL;
  }
  descriptor.checksum = descriptor_checksum;

  std::uint64_t descriptor_offset = 0;
  if (!RingOffset(superblock_, reservation.descriptor_ring_index,
                  &descriptor_offset, detail)) {
    return -EINVAL;
  }
  result = PwriteAll(io_.get(), fd_, descriptor_bytes.data(),
                     descriptor_bytes.size(), descriptor_offset,
                     "pwrite journal descriptor", detail);
  if (result != 0) {
    reload_required_ = true;
    return result;
  }

  payload_index = 0;
  for (const auto& [home_block, payload] : metadata_after_images) {
    (void)home_block;
    std::uint64_t payload_offset = 0;
    if (!RingOffset(superblock_,
                    reservation.payload_ring_indices[payload_index],
                    &payload_offset, detail)) {
      reload_required_ = true;
      return -EINVAL;
    }
    result = PwriteAll(io_.get(), fd_, payload.data(), payload.size(),
                       payload_offset, "pwrite journal payload", detail);
    if (result != 0) {
      reload_required_ = true;
      return result;
    }
    ++payload_index;
  }

  errno = 0;
  if (io_->Fdatasync(fd_) != 0) {
    const int error_number = errno != 0 ? errno : EIO;
    SetSystemDetail(detail, "fdatasync journal body", error_number);
    reload_required_ = true;
    return -error_number;
  }
  if (observer_ != nullptr) {
    observer_->OnDurableStage(DurableStage::kJournalBody);
  }

  DurableJournalBody candidate;
  candidate.reservation = reservation;
  candidate.entry_count =
      static_cast<std::uint32_t>(metadata_after_images.size());
  candidate.descriptor_crc32c = descriptor.checksum;
  durable_body_ = candidate;
  metadata_after_images_ = metadata_after_images;
  *output = std::move(candidate);
  return 0;
}

int JournalControlStore::ExposeDurableBody(std::string* detail) {
  if (reload_required_) {
    SetDetail(detail,
              "previous journal write has uncertain durability; reopen and "
              "reselect before any further write");
    return -EIO;
  }
  if (current_.used_blocks != 0) {
    SetDetail(detail, "v1 already has an exposed journal transaction");
    return -EBUSY;
  }
  if (!durable_body_.has_value()) {
    SetDetail(detail,
              "journal body must be durable before control exposure");
    return -EPERM;
  }
  const int result =
      PersistNext(durable_body_->reservation.exposed_control, detail);
  if (result == 0 && observer_ != nullptr) {
    observer_->OnDurableStage(DurableStage::kControlExposure);
  }
  return result;
}

int JournalControlStore::WriteCommit(std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (reload_required_) {
    SetDetail(detail,
              "previous journal write has uncertain durability; reopen and "
              "reselect before any further write");
    return -EIO;
  }
  if (!durable_body_.has_value()) {
    SetDetail(detail, "durable journal body is required before COMMIT");
    return -EPERM;
  }
  if (commit_durable_) {
    SetDetail(detail, "journal COMMIT is already durable");
    return -EALREADY;
  }
  if (!SameControlState(
          current_, durable_body_->reservation.exposed_control)) {
    SetDetail(detail,
              "exact durable journal body must be exposed before COMMIT");
    return -EPERM;
  }

  CommitRecord commit;
  commit.transaction_id = durable_body_->reservation.transaction_id;
  commit.filesystem_uuid = superblock_.filesystem_uuid;
  commit.entry_count = durable_body_->entry_count;
  commit.transaction_block_count = durable_body_->entry_count + 2U;
  commit.descriptor_ring_index =
      durable_body_->reservation.descriptor_ring_index;
  commit.descriptor_crc32c = durable_body_->descriptor_crc32c;

  ondisk::Block encoded{};
  if (!EncodeCommit(commit, &encoded, detail)) {
    return -EINVAL;
  }
  std::uint64_t commit_offset = 0;
  if (!RingOffset(superblock_,
                  durable_body_->reservation.commit_ring_index,
                  &commit_offset, detail)) {
    return -EINVAL;
  }
  int result = PwriteAll(io_.get(), fd_, encoded.data(), encoded.size(),
                         commit_offset, "pwrite journal COMMIT", detail);
  if (result != 0) {
    reload_required_ = true;
    return result;
  }
  errno = 0;
  if (io_->Fdatasync(fd_) != 0) {
    const int error_number = errno != 0 ? errno : EIO;
    SetSystemDetail(detail, "fdatasync journal COMMIT", error_number);
    reload_required_ = true;
    return -error_number;
  }
  if (observer_ != nullptr) {
    observer_->OnDurableStage(DurableStage::kCommit);
  }

  commit_durable_ = true;
  return 0;
}

int JournalControlStore::CompleteCommittedTransaction(std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (reload_required_) {
    SetDetail(detail,
              "previous journal write has uncertain durability; reopen and "
              "reselect before any further write");
    return -EIO;
  }
  if (!durable_body_.has_value() || !commit_durable_ ||
      metadata_after_images_.empty()) {
    SetDetail(detail,
              "a durable body and COMMIT are required before checkpoint");
    return -EPERM;
  }
  if (checkpointed_) {
    SetDetail(detail, "journal transaction is already checkpointed");
    return -EALREADY;
  }

  for (const auto& [home_block, after_image] : metadata_after_images_) {
    if (!IsValidMetadataTarget(superblock_, home_block)) {
      SetDetail(detail,
                "committed transaction targets a forbidden home block");
      return -EUCLEAN;
    }
    const int result = PwriteAll(
        io_.get(), fd_, after_image.data(), after_image.size(),
        static_cast<std::uint64_t>(home_block) * ondisk::kBlockSize,
        "pwrite committed home block", detail);
    if (result != 0) {
      reload_required_ = true;
      return result;
    }
  }

  errno = 0;
  if (io_->Fdatasync(fd_) != 0) {
    const int error_number = errno != 0 ? errno : EIO;
    SetSystemDetail(detail, "fdatasync committed home blocks", error_number);
    reload_required_ = true;
    return -error_number;
  }
  if (observer_ != nullptr) {
    observer_->OnDurableStage(DurableStage::kHomeBlocks);
  }

  JournalControl clean = current_;
  clean.generation += std::uint64_t{1};
  clean.tail = clean.head;
  clean.used_blocks = 0;
  clean.checksum = 0;
  const int result = PersistNext(clean, detail);
  if (result != 0) {
    return result;
  }
  if (observer_ != nullptr) {
    observer_->OnDurableStage(DurableStage::kCheckpoint);
  }
  checkpointed_ = true;
  return 0;
}

int JournalControlStore::PersistNext(const JournalControl& next,
                                     std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (reload_required_) {
    SetDetail(detail,
              "previous journal control update has uncertain durability; "
              "reopen and reselect before any further write");
    return -EIO;
  }
  if (!HasSameIdentity(current_, next) ||
      next.filesystem_uuid != superblock_.filesystem_uuid ||
      next.ring_blocks !=
          superblock_.journal.block_count -
              ondisk::kJournalControlBlockCount) {
    SetDetail(detail, "next journal control changes immutable identity fields");
    return -EINVAL;
  }
  if (next.generation != current_.generation + std::uint64_t{1}) {
    SetDetail(detail,
              "next journal control generation is not the exact successor");
    return -EINVAL;
  }

  ondisk::Block encoded{};
  std::uint32_t checksum = 0;
  if (!EncodeControl(next, &encoded, &checksum, detail)) {
    return -EINVAL;
  }

  const ControlCopy target_copy = current_copy_ == ControlCopy::kA
                                      ? ControlCopy::kB
                                      : ControlCopy::kA;
  const std::uint32_t relative_block =
      target_copy == ControlCopy::kA ? 0U : 1U;
  const std::uint64_t target_offset =
      static_cast<std::uint64_t>(superblock_.journal.start_block +
                                 relative_block) *
      ondisk::kBlockSize;
  int result = PwriteAll(io_.get(), fd_, encoded.data(), encoded.size(),
                         target_offset, "pwrite journal control", detail);
  if (result != 0) {
    reload_required_ = true;
    return result;
  }
  errno = 0;
  if (io_->Fdatasync(fd_) != 0) {
    const int error_number = errno != 0 ? errno : EIO;
    SetSystemDetail(detail, "fdatasync journal control", error_number);
    reload_required_ = true;
    return -error_number;
  }

  current_ = next;
  current_.checksum = checksum;
  current_copy_ = target_copy;
  return 0;
}

}  // namespace eufs::journal
