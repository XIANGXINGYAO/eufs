#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace eufs::object_store {

constexpr std::size_t kRequestIdSize = 16;
constexpr std::size_t kSha256Size = 32;

using RequestId = std::array<std::uint8_t, kRequestIdSize>;
using PayloadDigest = std::array<std::uint8_t, kSha256Size>;
using RequestFingerprint = std::array<std::uint8_t, kSha256Size>;

enum class MutationOperation : std::uint8_t {
  kCreateIfAbsent = 1,
  kReplaceIfVersion = 2,
};

// 只包含一次写操作的稳定业务语义，不包含 protobuf 或 brpc 对象。
struct MutationIdentityInput {
  MutationOperation operation{MutationOperation::kCreateIfAbsent};
  std::string_view key;
  std::uint64_t payload_size{0};
  PayloadDigest payload_sha256{};
  // timestamp 会持久化为 inode mtime，因此属于请求身份，不能在重试比较中忽略。
  std::uint64_t timestamp_ns{0};
  std::uint32_t expected_inode{0};
  std::uint64_t expected_generation{0};
};

// 按版本化 canonical encoding 生成跨进程、跨编译器稳定的 SHA-256。
// 失败时 output 保持原值。
int BuildRequestFingerprint(const MutationIdentityInput& input,
                            RequestFingerprint* output,
                            std::string* detail);

}  // namespace eufs::object_store
