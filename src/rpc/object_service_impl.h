#pragma once

#include "object/object_backend.h"
#include "object_service.pb.h"
#include "rpc/bounded_task_queue.h"
#include "rpc/inflight_byte_limiter.h"
#include "rpc/object_service_metrics.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace eufs::rpc {

struct ObjectServiceOptions {
  std::size_t max_inflight_bytes{64U * 1024U * 1024U};
  std::size_t max_queued_write_tasks{128};
  std::size_t max_queued_read_tasks{256};
  std::size_t read_worker_count{4};
};

// brpc 只负责协议、准入和异步生命周期；所有持久化语义仍由 ObjectBackend 提供。
class ObjectServiceImpl final : public protocol::ObjectService {
 public:
  ObjectServiceImpl(object_store::ObjectBackend* backend,
                    ObjectServiceOptions options);
  ObjectServiceImpl(const ObjectServiceImpl&) = delete;
  ObjectServiceImpl& operator=(const ObjectServiceImpl&) = delete;
  ~ObjectServiceImpl() override;

  void PutObject(google::protobuf::RpcController* controller,
                 const protocol::PutObjectRequest* request,
                 protocol::PutObjectResponse* response,
                 google::protobuf::Closure* done) override;
  void GetObject(google::protobuf::RpcController* controller,
                 const protocol::GetObjectRequest* request,
                 protocol::GetObjectResponse* response,
                 google::protobuf::Closure* done) override;
  void StatObject(google::protobuf::RpcController* controller,
                  const protocol::StatObjectRequest* request,
                  protocol::StatObjectResponse* response,
                  google::protobuf::Closure* done) override;

  // 拒绝新任务，完成所有排队任务，再等待正在执行的 Backend 调用退出。
  void Shutdown();

  std::size_t inflight_bytes() const noexcept;
  std::size_t queued_write_tasks() const noexcept;
  std::size_t queued_read_tasks() const noexcept;

 private:
  void WriteWorkerMain();
  void ReadWorkerMain();

  object_store::ObjectBackend* backend_;
  InflightByteLimiter byte_limiter_;
  BoundedTaskQueue write_queue_;
  BoundedTaskQueue read_queue_;
  // 必须声明在 limiter/queue 之后，保证三个 PassiveStatus 的数据源先构造、后析构。
  ObjectServiceMetrics metrics_;
  std::atomic<bool> stopping_{false};
  std::thread write_worker_;
  std::vector<std::thread> read_workers_;
};

}  // namespace eufs::rpc
