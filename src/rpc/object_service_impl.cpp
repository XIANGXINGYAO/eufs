#include "rpc/object_service_impl.h"

#include "metadata/ondisk_format.h"
#include "object/request_fingerprint.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <butil/iobuf.h>
#include <butil/time.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <string>
#include <utility>

namespace eufs::rpc {
namespace {

constexpr std::size_t kSha256Size = 32;

bool IsAllZeroRequestId(const std::string& request_id) {
  return std::all_of(request_id.begin(), request_id.end(),
                     [](char value) { return value == 0; });
}

// 两个 Task 回调共享此对象；原子 exchange 是 done 恰好一次的最后一道防线。
class RpcCompletion {
 public:
  void Adopt(google::protobuf::Closure* done) noexcept { done_.store(done); }

  void Run() noexcept {
    google::protobuf::Closure* done = done_.exchange(nullptr);
    if (done != nullptr) {
      brpc::ClosureGuard guard(done);
    }
  }

 private:
  std::atomic<google::protobuf::Closure*> done_{nullptr};
};

struct PutContext {
  brpc::Controller* controller{nullptr};
  protocol::PutObjectResponse* response{nullptr};
  std::shared_ptr<RpcCompletion> completion;
  protocol::PutStatus cancel_status{
      protocol::PUT_STATUS_SERVER_STOPPING};
  const char* cancel_detail{"server is stopping"};
  butil::Timer queue_wait_timer;
};

template <typename Response>
struct ReadContext {
  brpc::Controller* controller{nullptr};
  Response* response{nullptr};
  std::shared_ptr<RpcCompletion> completion;
  protocol::ReadStatus cancel_status{
      protocol::READ_STATUS_SERVER_STOPPING};
  const char* cancel_detail{"server is stopping"};
  butil::Timer queue_wait_timer;
};

void SetResponse(protocol::PutObjectResponse* response,
                 protocol::PutStatus status, std::string detail) {
  response->set_status(status);
  response->set_detail(std::move(detail));
}

void SetVersion(const object_store::ObjectVersion& source,
                protocol::ObjectVersion* destination) {
  destination->set_inode_number(source.inode_number);
  destination->set_generation(source.generation);
}

template <typename Response>
void SetReadResponse(Response* response, protocol::ReadStatus status,
                     std::string detail) {
  response->set_status(status);
  response->set_detail(std::move(detail));
}

void SetMetadata(const object_store::ObjectStat& source,
                 protocol::ObjectMetadata* destination) {
  destination->mutable_version()->set_inode_number(source.inode_number);
  destination->mutable_version()->set_generation(source.generation);
  destination->set_size(source.size);
  destination->set_mtime_ns(source.mtime_ns);
}

template <typename Response>
void MapReadResult(int result, const object_store::ObjectStat& stat,
                   std::string detail, Response* response) {
  if (result == 0) {
    SetReadResponse(response, protocol::READ_STATUS_OK, {});
    SetMetadata(stat, response->mutable_metadata());
  } else if (result == -ENOENT) {
    SetReadResponse(response, protocol::READ_STATUS_NOT_FOUND,
                    std::move(detail));
  } else {
    SetReadResponse(response, protocol::READ_STATUS_STORAGE_ERROR,
                    std::move(detail));
  }
}

// 逐个 IOBuf backing block 计算 SHA-256，不为了校验而提前 flatten 整个 payload。
bool ComputeSha256(const butil::IOBuf& payload,
                   std::array<unsigned char, kSha256Size>* digest) {
  if (digest == nullptr) {
    return false;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (context == nullptr ||
      EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
    return false;
  }

  butil::IOBufBytesIterator iterator(payload);
  const void* data = nullptr;
  std::size_t size = 0;
  while (iterator.forward_one_block(&data, &size)) {
    if (EVP_DigestUpdate(context.get(), data, size) != 1) {
      return false;
    }
  }
  unsigned int digest_size = 0;
  return EVP_DigestFinal_ex(context.get(), digest->data(), &digest_size) == 1 &&
         digest_size == digest->size();
}

void MapMutationResult(int result,
                       const object_store::IdempotentMutationResult& state,
                       std::string detail,
                       protocol::PutObjectResponse* response) {
  if (state.disposition ==
      object_store::RequestDisposition::kRequestIdConflict) {
    SetResponse(response, protocol::PUT_STATUS_REQUEST_ID_CONFLICT,
                std::move(detail));
    return;
  }
  if (state.disposition == object_store::RequestDisposition::kLedgerFull) {
    SetResponse(response, protocol::PUT_STATUS_LEDGER_FULL, std::move(detail));
    return;
  }
  const auto& mutation = state.mutation;
  if (mutation.outcome == object_store::MutationOutcome::kUnknown) {
    SetResponse(response, protocol::PUT_STATUS_RESULT_UNKNOWN,
                std::move(detail));
    return;
  }
  if (mutation.outcome == object_store::MutationOutcome::kCommitted) {
    SetResponse(response, protocol::PUT_STATUS_OK, {});
    SetVersion(mutation.committed_version,
               response->mutable_committed_version());
    return;
  }
  if (result == -EEXIST) {
    SetResponse(response, protocol::PUT_STATUS_ALREADY_EXISTS,
                std::move(detail));
    SetVersion(mutation.current_version, response->mutable_current_version());
    return;
  }
  if (result == -ESTALE) {
    SetResponse(response, protocol::PUT_STATUS_VERSION_MISMATCH,
                std::move(detail));
    SetVersion(mutation.current_version, response->mutable_current_version());
    return;
  }
  if (result == -ENOENT) {
    SetResponse(response, protocol::PUT_STATUS_NOT_FOUND, std::move(detail));
    return;
  }
  SetResponse(response, protocol::PUT_STATUS_STORAGE_ERROR,
              std::move(detail));
}

void RecordMutationDisposition(
    const object_store::IdempotentMutationResult& state,
    ObjectServiceMetrics* metrics) {
  if (state.disposition == object_store::RequestDisposition::kReplayed) {
    metrics->RecordRequestReplay();
  }
}

void RecordPutStatus(protocol::PutStatus status,
                     ObjectServiceMetrics* metrics) {
  switch (status) {
    case protocol::PUT_STATUS_REQUEST_ID_CONFLICT:
      metrics->RecordRequestIdConflict();
      break;
    case protocol::PUT_STATUS_LEDGER_FULL:
      metrics->RecordLedgerFull();
      break;
    case protocol::PUT_STATUS_RESULT_UNKNOWN:
      metrics->RecordResultUnknown();
      break;
    case protocol::PUT_STATUS_STORAGE_ERROR:
      metrics->RecordStorageError();
      break;
    default:
      break;
  }
}

template <typename Response>
void RecordReadStatus(const Response& response, ObjectServiceMetrics* metrics) {
  if (response.status() == protocol::READ_STATUS_STORAGE_ERROR) {
    metrics->RecordStorageError();
  }
}

}  // namespace

ObjectServiceImpl::ObjectServiceImpl(object_store::ObjectBackend* backend,
                                     ObjectServiceOptions options)
    : backend_(backend),
      byte_limiter_(options.max_inflight_bytes),
      write_queue_(options.max_queued_write_tasks),
      read_queue_(options.max_queued_read_tasks),
      metrics_(&byte_limiter_, &write_queue_, &read_queue_),
      write_worker_(&ObjectServiceImpl::WriteWorkerMain, this) {
  read_workers_.reserve(options.read_worker_count);
  for (std::size_t index = 0; index < options.read_worker_count; ++index) {
    read_workers_.emplace_back(&ObjectServiceImpl::ReadWorkerMain, this);
  }
}

ObjectServiceImpl::~ObjectServiceImpl() { Shutdown(); }

void ObjectServiceImpl::PutObject(
    google::protobuf::RpcController* controller_base,
    const protocol::PutObjectRequest* request,
    protocol::PutObjectResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* controller = static_cast<brpc::Controller*>(controller_base);
  if (backend_ == nullptr || request == nullptr || response == nullptr) {
    controller->SetFailed(EINVAL, "invalid PutObject service arguments");
    return;
  }
  if (stopping_.load()) {
    SetResponse(response, protocol::PUT_STATUS_SERVER_STOPPING,
                "server is stopping");
    return;
  }

  const butil::IOBuf& attachment = controller->request_attachment();
  if (request->key().empty() || request->payload_size() != attachment.size() ||
      request->sha256().size() != kSha256Size ||
      request->request_id().size() != object_store::kRequestIdSize ||
      IsAllZeroRequestId(request->request_id()) ||
      request->precondition_case() ==
          protocol::PutObjectRequest::PRECONDITION_NOT_SET) {
    SetResponse(response, protocol::PUT_STATUS_INVALID_ARGUMENT,
                "key, payload size, SHA-256, request ID, and precondition are "
                "required");
    return;
  }
  if (request->key().size() > ondisk::kMaxNameLength ||
      attachment.size() > ondisk::kMaxFileSize) {
    SetResponse(response, protocol::PUT_STATUS_INVALID_ARGUMENT,
                "key or payload exceeds the EUFS v1 format limit");
    return;
  }

  // 先取得资源预算，再执行线性复杂度的摘要计算；超载请求不消耗哈希 CPU。
  auto lease = byte_limiter_.TryAcquire(attachment.size());
  if (!lease.has_value()) {
    SetResponse(response, protocol::PUT_STATUS_OVERLOADED,
                "inflight payload byte limit exceeded");
    metrics_.RecordInflightByteRejection();
    return;
  }

  std::array<unsigned char, kSha256Size> actual_digest{};
  if (!ComputeSha256(attachment, &actual_digest)) {
    SetResponse(response, protocol::PUT_STATUS_STORAGE_ERROR,
                "could not compute request SHA-256");
    metrics_.RecordStorageError();
    return;
  }
  if (CRYPTO_memcmp(actual_digest.data(), request->sha256().data(),
                    actual_digest.size()) != 0) {
    SetResponse(response, protocol::PUT_STATUS_INVALID_ARGUMENT,
                "request SHA-256 does not match attachment");
    return;
  }

  try {
    auto context = std::make_shared<PutContext>();
    context->controller = controller;
    context->response = response;
    context->completion = std::make_shared<RpcCompletion>();

    // IOBuf 复制只复制块引用；真正的连续 payload 副本只在 worker 中产生一次。
    butil::IOBuf payload = attachment;
    const std::string key = request->key();
    const std::uint64_t timestamp_ns = request->timestamp_ns();
    const auto precondition = request->precondition_case();
    object_store::ObjectVersion expected;
    if (precondition == protocol::PutObjectRequest::kExpectedVersion) {
      expected.inode_number = request->expected_version().inode_number();
      expected.generation = request->expected_version().generation();
    }
    object_store::RequestIdentity identity;
    std::copy(request->request_id().begin(), request->request_id().end(),
              identity.request_id.begin());
    object_store::MutationIdentityInput fingerprint_input;
    fingerprint_input.operation =
        precondition == protocol::PutObjectRequest::kCreateIfAbsent
            ? object_store::MutationOperation::kCreateIfAbsent
            : object_store::MutationOperation::kReplaceIfVersion;
    fingerprint_input.key = key;
    fingerprint_input.payload_size = request->payload_size();
    std::copy(actual_digest.begin(), actual_digest.end(),
              fingerprint_input.payload_sha256.begin());
    fingerprint_input.timestamp_ns = timestamp_ns;
    fingerprint_input.expected_inode = expected.inode_number;
    fingerprint_input.expected_generation = expected.generation;
    std::string fingerprint_detail;
    if (object_store::BuildRequestFingerprint(
            fingerprint_input, &identity.fingerprint, &fingerprint_detail) !=
        0) {
      SetResponse(context->response, protocol::PUT_STATUS_INVALID_ARGUMENT,
                  std::move(fingerprint_detail));
      context->completion->Run();
      return;
    }

    // 只把成功入队后等待 worker 的时间归入 queue wait；此前摘要和指纹计算属于准入。
    context->queue_wait_timer.start();
    QueuedTask task(
        std::move(*lease),
        [this, context, payload = std::move(payload), key, timestamp_ns,
         precondition, expected, identity]() mutable {
          context->queue_wait_timer.stop();
          metrics_.RecordWriteQueueWait(
              context->queue_wait_timer.u_elapsed());
          butil::Timer execution_timer(butil::Timer::STARTED);
          try {
            std::string contiguous_payload;
            if (payload.copy_to(&contiguous_payload) != payload.size()) {
              SetResponse(context->response,
                          protocol::PUT_STATUS_STORAGE_ERROR,
                          "could not flatten request attachment");
            } else {
              std::string detail;
              if (precondition ==
                  protocol::PutObjectRequest::kCreateIfAbsent) {
                object_store::IdempotentMutationResult mutation;
                const int result = backend_->PutIfAbsentIdempotent(
                    key, contiguous_payload, timestamp_ns, identity, &mutation,
                    &detail);
                RecordMutationDisposition(mutation, &metrics_);
                MapMutationResult(result, mutation, std::move(detail),
                                  context->response);
              } else {
                object_store::IdempotentMutationResult mutation;
                const int result = backend_->ReplaceIfVersionIdempotent(
                    key, expected, contiguous_payload, timestamp_ns, identity,
                    &mutation, &detail);
                RecordMutationDisposition(mutation, &metrics_);
                MapMutationResult(result, mutation, std::move(detail),
                                  context->response);
              }
            }
          } catch (const std::exception& error) {
            SetResponse(context->response, protocol::PUT_STATUS_STORAGE_ERROR,
                        error.what());
          } catch (...) {
            SetResponse(context->response, protocol::PUT_STATUS_STORAGE_ERROR,
                        "unknown PutObject worker failure");
          }
          RecordPutStatus(context->response->status(), &metrics_);
          execution_timer.stop();
          metrics_.RecordPutExecution(execution_timer.u_elapsed());
          context->completion->Run();
        },
        [context] {
          SetResponse(context->response, context->cancel_status,
                      context->cancel_detail);
          context->completion->Run();
        });

    // Task 已经具备执行和取消两条完成路径后，才从 handler guard 接管 done。
    context->completion->Adopt(done_guard.release());
    const EnqueueResult enqueue_result = write_queue_.TryEnqueue(&task);
    if (enqueue_result == EnqueueResult::kAccepted) {
      return;
    }
    if (enqueue_result == EnqueueResult::kFull) {
      context->cancel_status = protocol::PUT_STATUS_OVERLOADED;
      context->cancel_detail = "bounded task queue is full";
      metrics_.RecordWriteQueueRejection();
    } else if (enqueue_result == EnqueueResult::kResourceExhausted) {
      context->cancel_status = protocol::PUT_STATUS_OVERLOADED;
      context->cancel_detail = "task queue allocation failed";
      metrics_.RecordQueueAllocationFailure();
    } else if (enqueue_result == EnqueueResult::kStopped) {
      context->cancel_status = protocol::PUT_STATUS_SERVER_STOPPING;
      context->cancel_detail = "server is stopping";
    } else {
      context->cancel_status = protocol::PUT_STATUS_STORAGE_ERROR;
      context->cancel_detail = "invalid queued PutObject task";
      metrics_.RecordStorageError();
    }
    task.Cancel();
  } catch (const std::exception& error) {
    SetResponse(response, protocol::PUT_STATUS_STORAGE_ERROR, error.what());
    metrics_.RecordStorageError();
  } catch (...) {
    SetResponse(response, protocol::PUT_STATUS_STORAGE_ERROR,
                "unknown PutObject admission failure");
    metrics_.RecordStorageError();
  }
}

void ObjectServiceImpl::GetObject(
    google::protobuf::RpcController* controller_base,
    const protocol::GetObjectRequest* request,
    protocol::GetObjectResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* controller = static_cast<brpc::Controller*>(controller_base);
  if (backend_ == nullptr || request == nullptr || response == nullptr) {
    controller->SetFailed(EINVAL, "invalid GetObject service arguments");
    return;
  }
  if (stopping_.load()) {
    SetReadResponse(response, protocol::READ_STATUS_SERVER_STOPPING,
                    "server is stopping");
    return;
  }
  if (request->key().empty()) {
    SetReadResponse(response, protocol::READ_STATUS_INVALID_ARGUMENT,
                    "key is required");
    return;
  }

  try {
    auto context =
        std::make_shared<ReadContext<protocol::GetObjectResponse>>();
    context->controller = controller;
    context->response = response;
    context->completion = std::make_shared<RpcCompletion>();
    const std::string key = request->key();

    context->queue_wait_timer.start();
    QueuedTask task(
        [this, context, key] {
          context->queue_wait_timer.stop();
          metrics_.RecordReadQueueWait(
              context->queue_wait_timer.u_elapsed());
          butil::Timer execution_timer(butil::Timer::STARTED);
          try {
            std::string payload;
            object_store::ObjectStat stat;
            std::string detail;
            const int result = backend_->Get(key, &payload, &stat, &detail);
            if (result == 0) {
              context->controller->response_attachment().append(payload);
            }
            MapReadResult(result, stat, std::move(detail), context->response);
          } catch (const std::exception& error) {
            SetReadResponse(context->response,
                            protocol::READ_STATUS_STORAGE_ERROR, error.what());
          } catch (...) {
            SetReadResponse(context->response,
                            protocol::READ_STATUS_STORAGE_ERROR,
                            "unknown GetObject worker failure");
          }
          RecordReadStatus(*context->response, &metrics_);
          execution_timer.stop();
          metrics_.RecordGetExecution(execution_timer.u_elapsed());
          context->completion->Run();
        },
        [context] {
          SetReadResponse(context->response, context->cancel_status,
                          context->cancel_detail);
          context->completion->Run();
        });

    context->completion->Adopt(done_guard.release());
    const EnqueueResult enqueue_result = read_queue_.TryEnqueue(&task);
    if (enqueue_result == EnqueueResult::kAccepted) {
      return;
    }
    if (enqueue_result == EnqueueResult::kFull ||
        enqueue_result == EnqueueResult::kResourceExhausted) {
      context->cancel_status = protocol::READ_STATUS_OVERLOADED;
      context->cancel_detail = enqueue_result == EnqueueResult::kFull
                                   ? "bounded read queue is full"
                                   : "read queue allocation failed";
      if (enqueue_result == EnqueueResult::kFull) {
        metrics_.RecordReadQueueRejection();
      } else {
        metrics_.RecordQueueAllocationFailure();
      }
    } else if (enqueue_result == EnqueueResult::kStopped) {
      context->cancel_status = protocol::READ_STATUS_SERVER_STOPPING;
      context->cancel_detail = "server is stopping";
    } else {
      context->cancel_status = protocol::READ_STATUS_STORAGE_ERROR;
      context->cancel_detail = "invalid queued GetObject task";
      metrics_.RecordStorageError();
    }
    task.Cancel();
  } catch (const std::exception& error) {
    SetReadResponse(response, protocol::READ_STATUS_STORAGE_ERROR,
                    error.what());
    metrics_.RecordStorageError();
  } catch (...) {
    SetReadResponse(response, protocol::READ_STATUS_STORAGE_ERROR,
                    "unknown GetObject admission failure");
    metrics_.RecordStorageError();
  }
}

void ObjectServiceImpl::StatObject(
    google::protobuf::RpcController* controller_base,
    const protocol::StatObjectRequest* request,
    protocol::StatObjectResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* controller = static_cast<brpc::Controller*>(controller_base);
  if (backend_ == nullptr || request == nullptr || response == nullptr) {
    controller->SetFailed(EINVAL, "invalid StatObject service arguments");
    return;
  }
  if (stopping_.load()) {
    SetReadResponse(response, protocol::READ_STATUS_SERVER_STOPPING,
                    "server is stopping");
    return;
  }
  if (request->key().empty()) {
    SetReadResponse(response, protocol::READ_STATUS_INVALID_ARGUMENT,
                    "key is required");
    return;
  }

  try {
    auto context =
        std::make_shared<ReadContext<protocol::StatObjectResponse>>();
    context->controller = controller;
    context->response = response;
    context->completion = std::make_shared<RpcCompletion>();
    const std::string key = request->key();

    context->queue_wait_timer.start();
    QueuedTask task(
        [this, context, key] {
          context->queue_wait_timer.stop();
          metrics_.RecordReadQueueWait(
              context->queue_wait_timer.u_elapsed());
          butil::Timer execution_timer(butil::Timer::STARTED);
          try {
            object_store::ObjectStat stat;
            std::string detail;
            const int result = backend_->Stat(key, &stat, &detail);
            MapReadResult(result, stat, std::move(detail), context->response);
          } catch (const std::exception& error) {
            SetReadResponse(context->response,
                            protocol::READ_STATUS_STORAGE_ERROR, error.what());
          } catch (...) {
            SetReadResponse(context->response,
                            protocol::READ_STATUS_STORAGE_ERROR,
                            "unknown StatObject worker failure");
          }
          RecordReadStatus(*context->response, &metrics_);
          execution_timer.stop();
          metrics_.RecordStatExecution(execution_timer.u_elapsed());
          context->completion->Run();
        },
        [context] {
          SetReadResponse(context->response, context->cancel_status,
                          context->cancel_detail);
          context->completion->Run();
        });

    context->completion->Adopt(done_guard.release());
    const EnqueueResult enqueue_result = read_queue_.TryEnqueue(&task);
    if (enqueue_result == EnqueueResult::kAccepted) {
      return;
    }
    if (enqueue_result == EnqueueResult::kFull ||
        enqueue_result == EnqueueResult::kResourceExhausted) {
      context->cancel_status = protocol::READ_STATUS_OVERLOADED;
      context->cancel_detail = enqueue_result == EnqueueResult::kFull
                                   ? "bounded read queue is full"
                                   : "read queue allocation failed";
      if (enqueue_result == EnqueueResult::kFull) {
        metrics_.RecordReadQueueRejection();
      } else {
        metrics_.RecordQueueAllocationFailure();
      }
    } else if (enqueue_result == EnqueueResult::kStopped) {
      context->cancel_status = protocol::READ_STATUS_SERVER_STOPPING;
      context->cancel_detail = "server is stopping";
    } else {
      context->cancel_status = protocol::READ_STATUS_STORAGE_ERROR;
      context->cancel_detail = "invalid queued StatObject task";
      metrics_.RecordStorageError();
    }
    task.Cancel();
  } catch (const std::exception& error) {
    SetReadResponse(response, protocol::READ_STATUS_STORAGE_ERROR,
                    error.what());
    metrics_.RecordStorageError();
  } catch (...) {
    SetReadResponse(response, protocol::READ_STATUS_STORAGE_ERROR,
                    "unknown StatObject admission failure");
    metrics_.RecordStorageError();
  }
}

void ObjectServiceImpl::WriteWorkerMain() {
  QueuedTask task;
  while (write_queue_.WaitPop(&task)) {
    task.Run();
    task = {};
  }
}

void ObjectServiceImpl::ReadWorkerMain() {
  QueuedTask task;
  while (read_queue_.WaitPop(&task)) {
    task.Run();
    task = {};
  }
}

void ObjectServiceImpl::Shutdown() {
  if (stopping_.exchange(true)) {
    return;
  }
  auto pending_writes = write_queue_.StopAndTakePending();
  auto pending_reads = read_queue_.StopAndTakePending();
  for (auto& task : pending_writes) {
    task.Cancel();
  }
  for (auto& task : pending_reads) {
    task.Cancel();
  }
  pending_writes.clear();
  pending_reads.clear();
  if (write_worker_.joinable()) {
    write_worker_.join();
  }
  for (auto& worker : read_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

std::size_t ObjectServiceImpl::inflight_bytes() const noexcept {
  return byte_limiter_.used_bytes();
}

std::size_t ObjectServiceImpl::queued_write_tasks() const noexcept {
  return write_queue_.size();
}

std::size_t ObjectServiceImpl::queued_read_tasks() const noexcept {
  return read_queue_.size();
}

}  // namespace eufs::rpc
