# eufs

`eufs` 是一个面向 Linux/openEuler 的崩溃一致性用户态文件系统工程原型。项目使用
C++17、FUSE3 和自定义磁盘格式，重点验证元数据事务、崩溃恢复、写时复制和离线一致性
检查，而不是追求完整的 POSIX 文件系统兼容性。

## 当前能力

- 固定宽度磁盘格式：superblock、inode/block bitmap、inode table、目录项和数据块。
- 文件寻址：12 个 direct block 加 single-indirect block。
- 通用 COW 写计划：覆盖、追加、跨块写、部分块 RMW、EOF gap 零填充和 ENOSPC 回滚。
- metadata redo WAL：ordered data、descriptor/payload/COMMIT、A/B control 和 checkpoint。
- 挂载前恢复：严格解析当前日志边界，区分未提交、已提交和结构损坏事务。
- `eufsck`：只读扫描 inode、目录图、bitmap、块引用、link count 和不可达 inode。
- Request Ledger 基础：固定记录格式和 CRC、mkfs 预分配、启动索引重建及
  `eufsck` 专用损坏检查；尚未接入对象事务追加。
- FUSE 挂载路径：镜像生命周期独占锁、create、read、write 和恢复后重新加载。
- 直接对象后端：`PutIfAbsent/Get/Stat/ReplaceIfVersion`，带版本条件的完整对象替换、
  明确变更结果和 fail-closed，不经 FUSE 数据路径。
- 可选 brpc 服务骨架：protobuf 接口、attachment 数据路径、按并发字节和队列容量
  背压；默认构建不要求 brpc，端到端持久化幂等尚未完成。

## 持久化顺序

一次已挂载写入的核心顺序是：

```text
COW data blocks
  -> fdatasync
  -> journal descriptor + metadata after-images
  -> fdatasync
  -> publish A/B control
  -> durable COMMIT
  -> replay home metadata
  -> checkpoint
```

COMMIT 前恢复到旧版本；COMMIT 持久化后恢复并重放新版本。恢复逻辑只接受 control
声明范围内、结构和 CRC 均合法的事务，不扫描日志区寻找“看起来像”COMMIT 的字节。

## 构建与测试

依赖：CMake 3.16+、支持 C++17 的编译器、`pkg-config`、FUSE3 和 OpenSSL 开发包。

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j2
(cd build && ctest --output-on-failure)
```

当前本地基线包含 42 个 CTest 测试。测试覆盖磁盘编解码、bitmap 分配、COW 写计划、
完整对象新建/替换、日志发布与恢复、挂载会话锁、全局一致性检查、RPC 背压以及
Request Ledger 编解码和启动扫描。

真实 FUSE 冒烟与崩溃矩阵：

```bash
./tests/smoke_disk_create_remount.sh
./tests/smoke_journal_crash_matrix.sh
./tests/smoke_cow_write_remount.sh
./tests/smoke_cow_overwrite_crash_matrix.sh
```

这些脚本需要可用的 `/dev/fuse` 和挂载权限。

## 代码阅读入口

早期纯内存 FUSE 原型已经从生产源码删除。当前工程按下面顺序阅读：

| 顺序 | 文件 | 作用 |
|---|---|---|
| 1 | `src/app/eufsd_main.cpp` | 当前磁盘版守护进程入口，建立状态并进入 `fuse_main` |
| 2 | `src/fuse/mount_state.h/.cpp` | 镜像独占会话、启动恢复、reader 生命周期和 fail-closed |
| 3 | `src/fuse/operations.cpp` | FUSE 回调到 reader/planner/事务执行器的适配层 |
| 4 | `src/storage/image_reader.cpp` | 解析路径、inode、目录项和 direct/indirect 数据块 |
| 5 | `src/metadata/file_write_plan.cpp` | 当前通用 COW 写计划；只生成 before/after images |
| 6 | `src/journal/journal_transaction_executor.cpp` | 唯一在线事务提交顺序 |
| 7 | `src/journal/journal_control_store.cpp` | 日志 body、A/B control、COMMIT、home replay、checkpoint |
| 8 | `src/journal/journal_recovery.cpp` | 挂载前事务分类、严格校验和恢复决策 |
| 9 | `src/object/object_backend.cpp` | brpc 服务复用的新建、读取、条件替换和并发边界 |
| 10 | `src/rpc/object_service_impl.cpp` | 可选 brpc 协议适配、attachment 和准入控制 |

早期“空 inode 写一个块”的受限计划器已经移到 `tests/support/`，只用于构造分配器、
日志和恢复测试，不再与 `eufsd` 的生产源码混放。

## 最小运行示例

```bash
./build/eufs-mkfs --image ./eufs.img --size 64M \
  --inodes 1024 --journal-blocks 256 --force
mkdir -p /tmp/eufs-mnt
./build/eufsd --image ./eufs.img -f /tmp/eufs-mnt
```

另一个终端可通过普通文件 API 访问挂载点。卸载后可以执行：

```bash
./build/eufsck --image ./eufs.img
fusermount3 -u /tmp/eufs-mnt
```

## 设计与证据

- [磁盘格式](docs/eufs_v1_on_disk_format.md)
- [日志事务格式](docs/eufs_v1_journal_transaction_format.md)
- [日志发布顺序审计](evidence/research/stage-d-journal-publication-order-audit-20260722.md)
- [openEuler 集成审计](evidence/research/openeuler-integration-audit-20260710.md)
- `evidence/debug/`：真实缺陷定位记录

每次 push 和 pull request 都由 GitHub Actions 重新构建并执行 CTest。本地原始日志不进入
源码历史，避免用不可独立复核的旧输出代替可重复测试。

## 已知边界

- 已证明的挂载写原子单位是一次实际 FUSE write callback，不是整个用户态
  `write(2)`/`pwrite(2)` 系统调用；内核可能按页边界拆分请求。
- brpc 服务源代码和进程内背压已经建立，但尚未完成 EUFS-brpc 端到端构建证据、
  bvar/压测，以及 Request-ID 的 RPC 接入。
- Request Ledger 当前只能格式化、解码和重建索引；对象修改与 ledger 记录尚未进入
  同一个 journal COMMIT，因此不能宣称持久化幂等或 exactly-once。
- 当前尚未实现 Delete/tombstone，版本令牌不宣称覆盖 delete/recreate ABA。
- 当前证据证明受控进程崩溃下的恢复语义，不等价于真实硬件断电认证。
- 尚未宣称完整 POSIX 兼容、稀疏文件、double-indirect block 或在线修复。

## 许可证

本项目采用 [Apache License 2.0](LICENSE)。
