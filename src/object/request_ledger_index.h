#pragma once

#include "object/request_ledger_format.h"
#include "storage/image_reader.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace eufs::object_store {

// 启动扫描得到的只读内存索引；后续写入闭环再增加受锁保护的追加接口。
class RequestLedgerIndex {
 public:
  std::uint64_t capacity() const { return capacity_; }
  std::size_t size() const { return records_.size(); }
  std::uint64_t next_sequence() const { return next_sequence_; }
  bool full() const { return records_.size() == capacity_; }

  // 找不到返回 nullptr；返回指针只在当前索引不被移动/销毁期间有效。
  const RequestLedgerRecord* Find(const RequestId& request_id) const;

  // 只在包含该记录的磁盘事务已经确定 COMMIT 后调用。拒绝跳号、重复 ID 和
  // 容量越界；分配失败返回 ENOMEM，调用方必须 fail-closed 并依靠重启重扫。
  int AppendCommitted(const RequestLedgerRecord& record,
                      std::string* detail);

 private:
  friend int ScanRequestLedger(const storage::ImageReader& image,
                               RequestLedgerIndex* output,
                               std::string* detail);

  std::uint64_t capacity_{0};
  std::uint64_t next_sequence_{1};
  // 最大容量仅 33152，使用有序表换取确定扫描结果和无需自定义哈希。
  std::map<RequestId, RequestLedgerRecord> records_;
};

// 扫描必须形成“合法记录连续前缀 + 全零后缀”；失败时 output 保持原值。
int ScanRequestLedger(const storage::ImageReader& image,
                      RequestLedgerIndex* output, std::string* detail);

}  // namespace eufs::object_store
