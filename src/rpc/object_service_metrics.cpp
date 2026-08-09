#include "rpc/object_service_metrics.h"

namespace eufs::rpc {
namespace {

constexpr char kMetricPrefix[] = "eufs_object_service";

}  // namespace

ObjectServiceMetrics::ObjectServiceMetrics(
    const InflightByteLimiter* byte_limiter,
    const BoundedTaskQueue* write_queue,
    const BoundedTaskQueue* read_queue)
    : byte_limiter_(byte_limiter),
      write_queue_(write_queue),
      read_queue_(read_queue),
      write_queue_wait_latency_(kMetricPrefix, "write_queue_wait"),
      read_queue_wait_latency_(kMetricPrefix, "read_queue_wait"),
      put_execution_latency_(kMetricPrefix, "put_execution"),
      get_execution_latency_(kMetricPrefix, "get_execution"),
      stat_execution_latency_(kMetricPrefix, "stat_execution"),
      inflight_byte_rejection_count_(kMetricPrefix,
                                     "inflight_byte_rejection_count"),
      write_queue_rejection_count_(kMetricPrefix,
                                    "write_queue_rejection_count"),
      read_queue_rejection_count_(kMetricPrefix,
                                   "read_queue_rejection_count"),
      queue_allocation_failure_count_(kMetricPrefix,
                                      "queue_allocation_failure_count"),
      request_replay_count_(kMetricPrefix, "request_replay_count"),
      request_id_conflict_count_(kMetricPrefix,
                                 "request_id_conflict_count"),
      ledger_full_count_(kMetricPrefix, "ledger_full_count"),
      result_unknown_count_(kMetricPrefix, "result_unknown_count"),
      storage_error_count_(kMetricPrefix, "storage_error_count"),
      inflight_payload_bytes_(kMetricPrefix, "inflight_payload_bytes",
                              &ObjectServiceMetrics::ReadInflightBytes, this),
      write_queue_depth_(kMetricPrefix, "write_queue_depth",
                         &ObjectServiceMetrics::ReadWriteQueueDepth, this),
      read_queue_depth_(kMetricPrefix, "read_queue_depth",
                        &ObjectServiceMetrics::ReadReadQueueDepth, this) {}

void ObjectServiceMetrics::RecordWriteQueueWait(std::int64_t latency_us) {
  write_queue_wait_latency_ << latency_us;
}

void ObjectServiceMetrics::RecordReadQueueWait(std::int64_t latency_us) {
  read_queue_wait_latency_ << latency_us;
}

void ObjectServiceMetrics::RecordPutExecution(std::int64_t latency_us) {
  put_execution_latency_ << latency_us;
}

void ObjectServiceMetrics::RecordGetExecution(std::int64_t latency_us) {
  get_execution_latency_ << latency_us;
}

void ObjectServiceMetrics::RecordStatExecution(std::int64_t latency_us) {
  stat_execution_latency_ << latency_us;
}

void ObjectServiceMetrics::RecordInflightByteRejection() {
  inflight_byte_rejection_count_ << 1;
}

void ObjectServiceMetrics::RecordWriteQueueRejection() {
  write_queue_rejection_count_ << 1;
}

void ObjectServiceMetrics::RecordReadQueueRejection() {
  read_queue_rejection_count_ << 1;
}

void ObjectServiceMetrics::RecordQueueAllocationFailure() {
  queue_allocation_failure_count_ << 1;
}

void ObjectServiceMetrics::RecordRequestReplay() {
  request_replay_count_ << 1;
}

void ObjectServiceMetrics::RecordRequestIdConflict() {
  request_id_conflict_count_ << 1;
}

void ObjectServiceMetrics::RecordLedgerFull() { ledger_full_count_ << 1; }

void ObjectServiceMetrics::RecordResultUnknown() {
  result_unknown_count_ << 1;
}

void ObjectServiceMetrics::RecordStorageError() { storage_error_count_ << 1; }

std::size_t ObjectServiceMetrics::ReadInflightBytes(void* argument) {
  const auto* metrics = static_cast<const ObjectServiceMetrics*>(argument);
  return metrics->byte_limiter_->used_bytes();
}

std::size_t ObjectServiceMetrics::ReadWriteQueueDepth(void* argument) {
  const auto* metrics = static_cast<const ObjectServiceMetrics*>(argument);
  return metrics->write_queue_->size();
}

std::size_t ObjectServiceMetrics::ReadReadQueueDepth(void* argument) {
  const auto* metrics = static_cast<const ObjectServiceMetrics*>(argument);
  return metrics->read_queue_->size();
}

}  // namespace eufs::rpc
