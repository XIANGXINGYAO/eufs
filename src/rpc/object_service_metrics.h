#pragma once

#include "rpc/bounded_task_queue.h"
#include "rpc/inflight_byte_limiter.h"

#include <bvar/bvar.h>

#include <cstddef>
#include <cstdint>

namespace eufs::rpc {

// 只负责暴露服务内部阶段指标，不参与请求准入、调度或持久化决策。
// limiter 和 queue 必须比本对象活得更久，PassiveStatus 查询时会读取它们的实时状态。
class ObjectServiceMetrics {
 public:
  ObjectServiceMetrics(const InflightByteLimiter* byte_limiter,
                       const BoundedTaskQueue* write_queue,
                       const BoundedTaskQueue* read_queue);
  ObjectServiceMetrics(const ObjectServiceMetrics&) = delete;
  ObjectServiceMetrics& operator=(const ObjectServiceMetrics&) = delete;

  // LatencyRecorder 统一接收微秒，生成 latency、分位值、max、qps 和 count。
  void RecordWriteQueueWait(std::int64_t latency_us);
  void RecordReadQueueWait(std::int64_t latency_us);
  void RecordPutExecution(std::int64_t latency_us);
  void RecordGetExecution(std::int64_t latency_us);
  void RecordStatExecution(std::int64_t latency_us);

  void RecordInflightByteRejection();
  void RecordWriteQueueRejection();
  void RecordReadQueueRejection();
  void RecordQueueAllocationFailure();
  void RecordRequestReplay();
  void RecordRequestIdConflict();
  void RecordLedgerFull();
  void RecordResultUnknown();
  void RecordStorageError();

 private:
  static std::size_t ReadInflightBytes(void* argument);
  static std::size_t ReadWriteQueueDepth(void* argument);
  static std::size_t ReadReadQueueDepth(void* argument);

  const InflightByteLimiter* byte_limiter_;
  const BoundedTaskQueue* write_queue_;
  const BoundedTaskQueue* read_queue_;

  bvar::LatencyRecorder write_queue_wait_latency_;
  bvar::LatencyRecorder read_queue_wait_latency_;
  bvar::LatencyRecorder put_execution_latency_;
  bvar::LatencyRecorder get_execution_latency_;
  bvar::LatencyRecorder stat_execution_latency_;

  bvar::Adder<std::int64_t> inflight_byte_rejection_count_;
  bvar::Adder<std::int64_t> write_queue_rejection_count_;
  bvar::Adder<std::int64_t> read_queue_rejection_count_;
  bvar::Adder<std::int64_t> queue_allocation_failure_count_;
  bvar::Adder<std::int64_t> request_replay_count_;
  bvar::Adder<std::int64_t> request_id_conflict_count_;
  bvar::Adder<std::int64_t> ledger_full_count_;
  bvar::Adder<std::int64_t> result_unknown_count_;
  bvar::Adder<std::int64_t> storage_error_count_;

  // 三个 gauge 不复制状态；/vars 查询发生时才调用上面的只读回调。
  bvar::PassiveStatus<std::size_t> inflight_payload_bytes_;
  bvar::PassiveStatus<std::size_t> write_queue_depth_;
  bvar::PassiveStatus<std::size_t> read_queue_depth_;
};

}  // namespace eufs::rpc
