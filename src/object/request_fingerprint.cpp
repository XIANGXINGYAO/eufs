#include "object/request_fingerprint.h"

#include "metadata/ondisk_format.h"

#include <cerrno>
#include <limits>
#include <openssl/evp.h>
#include <vector>

namespace eufs::object_store {
namespace {

constexpr std::uint8_t kFingerprintFormatVersion = 1;

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void AppendLe16(std::uint16_t value, std::vector<std::uint8_t>* output) {
  output->push_back(static_cast<std::uint8_t>(value));
  output->push_back(static_cast<std::uint8_t>(value >> 8U));
}

void AppendLe32(std::uint32_t value, std::vector<std::uint8_t>* output) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output->push_back(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void AppendLe64(std::uint64_t value, std::vector<std::uint8_t>* output) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output->push_back(
        static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

}  // namespace

int BuildRequestFingerprint(const MutationIdentityInput& input,
                            RequestFingerprint* output,
                            std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr || input.key.empty() ||
      input.key.size() > ondisk::kMaxNameLength ||
      input.key.size() > std::numeric_limits<std::uint16_t>::max() ||
      input.payload_size > ondisk::kMaxFileSize) {
    SetDetail(detail, "fingerprint output, key, and payload size are invalid");
    return -EINVAL;
  }

  const bool create =
      input.operation == MutationOperation::kCreateIfAbsent;
  const bool replace =
      input.operation == MutationOperation::kReplaceIfVersion;
  if ((!create && !replace) ||
      (create &&
       (input.expected_inode != 0 || input.expected_generation != 0)) ||
      (replace &&
       (input.expected_inode == 0 || input.expected_generation == 0))) {
    SetDetail(detail, "operation and expected version are inconsistent");
    return -EINVAL;
  }

  std::vector<std::uint8_t> canonical;
  canonical.reserve(57U + input.key.size());
  canonical.push_back(kFingerprintFormatVersion);
  canonical.push_back(static_cast<std::uint8_t>(input.operation));
  AppendLe16(static_cast<std::uint16_t>(input.key.size()), &canonical);
  canonical.insert(canonical.end(), input.key.begin(), input.key.end());
  AppendLe64(input.payload_size, &canonical);
  canonical.insert(canonical.end(), input.payload_sha256.begin(),
                   input.payload_sha256.end());
  canonical.push_back(create ? 1U : 2U);
  AppendLe32(input.expected_inode, &canonical);
  AppendLe64(input.expected_generation, &canonical);

  RequestFingerprint candidate{};
  unsigned int digest_size = 0;
  if (EVP_Digest(canonical.data(), canonical.size(), candidate.data(),
                 &digest_size, EVP_sha256(), nullptr) != 1 ||
      digest_size != candidate.size()) {
    SetDetail(detail, "OpenSSL could not compute request fingerprint");
    return -EIO;
  }
  *output = candidate;
  return 0;
}

}  // namespace eufs::object_store
