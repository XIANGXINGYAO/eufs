#pragma once

#include "object/request_fingerprint.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace eufs::object_store {

// request ledger 使用固定 128 字节记录；一个 4 KiB 块正好容纳 32 条。
constexpr std::size_t kRequestLedgerRecordSize = 128;
constexpr std::uint16_t kRequestLedgerFormatVersion = 1;
// ledger 同时用固定根目录名称和固定 inode 身份，启动扫描时必须交叉验证。
constexpr std::string_view kRequestLedgerName = ".eufs.request-ledger";
constexpr std::uint32_t kRequestLedgerInodeNumber = 2;

using RequestLedgerBytes =
    std::array<std::uint8_t, kRequestLedgerRecordSize>;

// 只持久化结果已经确定的请求；持久化结果未知不能进入 ledger。
enum class LedgerResultKind : std::uint8_t {
  kCommitted = 1,
  kNotApplied = 2,
};

// 这是稳定磁盘 ABI，不直接保存可能随平台变化的 errno 或 protobuf 枚举值。
enum class LedgerResultCode : std::uint32_t {
  kOk = 1,
  kAlreadyExists = 2,
  kVersionMismatch = 3,
  kNotFound = 4,
};

// 一条记录保存重放原请求结果所需的最小确定事实。
struct RequestLedgerRecord {
  MutationOperation operation{MutationOperation::kCreateIfAbsent};
  LedgerResultKind result_kind{LedgerResultKind::kCommitted};
  RequestId request_id{};
  RequestFingerprint fingerprint{};
  LedgerResultCode result_code{LedgerResultCode::kOk};
  std::uint32_t committed_inode{0};
  std::uint64_t committed_generation{0};
  std::uint32_t current_inode{0};
  std::uint64_t current_generation{0};
  // sequence 从 1 开始，并且必须与记录所在的全局槽位序号一致。
  std::uint64_t sequence{0};
};

// 解码必须区分预分配产生的全零空槽和不可解释的损坏记录。
enum class LedgerDecodeStatus {
  kEmpty,
  kRecord,
  kCorrupt,
};

// 编码失败时 output 保持原值。
bool EncodeRequestLedgerRecord(const RequestLedgerRecord& value,
                               RequestLedgerBytes* output,
                               std::string* error);

// expected_sequence 由调用者根据槽位位置计算；空槽或失败时 output 保持原值。
LedgerDecodeStatus DecodeRequestLedgerRecord(
    const RequestLedgerBytes& input, std::uint64_t expected_sequence,
    RequestLedgerRecord* output, std::string* error);

}  // namespace eufs::object_store
