# eufs

`eufs` 是一个面向 Linux/openEuler 的崩溃一致性用户态文件系统与对象服务工程原型。
项目使用 C++17、FUSE3、Apache brpc 和自定义磁盘格式，重点验证写时复制、元数据事务、
崩溃恢复、持久化请求幂等、有界资源准入和离线一致性检查，而不是追求完整的 POSIX
文件系统兼容性。

## 当前能力

- 固定宽度磁盘格式：superblock、inode/block bitmap、inode table、目录项和数据块。
- 文件寻址：12 个 direct block 加 single-indirect block。
- 通用 COW 写计划：覆盖、追加、跨块写、部分块 RMW、EOF gap 零填充和 ENOSPC 回滚。
- metadata redo WAL：ordered data、descriptor/payload/COMMIT、A/B control 和 checkpoint。
- 挂载前恢复：严格解析当前日志边界，区分未提交、已提交和结构损坏事务。
- `eufsck`：只读扫描 inode、目录图、bitmap、块引用、link count 和不可达 inode。
- Request Ledger：固定记录格式和 CRC、mkfs 预分配、启动索引重建、对象结果与
  ledger 单 COMMIT、Request-ID 重放/冲突/容量耗尽状态机及 `eufsck` 损坏检查。
- FUSE 挂载路径：镜像生命周期独占锁、create、read、write 和恢复后重新加载。
- 直接对象后端：`PutIfAbsent/Get/Stat/ReplaceIfVersion`，带版本条件的完整对象替换、
  明确变更结果和 fail-closed，不经 FUSE 数据路径。
- 可选 brpc 服务：protobuf Request-ID 接口、attachment 数据路径、按并发字节和
  队列容量背压、异步完成所有权和 bvar 运行指标；已完成 openEuler 原生构建、六阶段
  进程崩溃/Request-ID 重试矩阵和过载隔离矩阵。

## 数据路径

```text
PutObject + attachment
  -> 廉价字段与格式上限检查
  -> 在途 payload 字节准入
  -> SHA-256 与 Request-ID 指纹
  -> 有界写队列（单写 worker 串行化磁盘事务）
  -> ObjectBackend
  -> direct/single-indirect COW planner
  -> ordered data + metadata redo WAL + A/B control
  -> home metadata replay + checkpoint
  -> PutObject 响应

GetObject / StatObject
  -> 有界读队列（多 read worker）
  -> 同一 MountedImageSession 下的只读镜像视图
```

`InflightByteLimiter::Lease` 从准入成功一直存活到任务执行或取消结束，保证排队和执行中的
payload 总量有上界；`QueuedTask` 绑定执行、取消和额度归还三种责任。写 worker 只有一个是
当前磁盘事务模型的明确串行化边界，不宣称多写 worker 扩展能力。

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

当前本地基线包含 46 个 CTest 测试。测试覆盖磁盘编解码、bitmap 分配、COW 写计划、
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

启用 `EUFS_BUILD_BRPC_SERVICE` 并提供 brpc 构建产物后，可执行真实 RPC 崩溃矩阵：

```bash
./tests/smoke_brpc_request_id_crash_matrix.sh \
  ./build-brpc/eufs-mkfs \
  ./build-brpc/eufs_object_server \
  ./build-brpc/eufs_object_client \
  /tmp/eufs-brpc-request-id-crash-matrix \
  127.0.0.1:8027
```

过载隔离、重启校验和 `eufsck` 矩阵：

```bash
./tests/smoke_brpc_overload_matrix.sh \
  ./build-brpc/eufs-mkfs \
  ./build-brpc/eufs_object_server \
  ./build-brpc/eufs_object_load_client \
  ./build-brpc/eufs_object_client \
  ./build-brpc/eufsck \
  /tmp/eufs-brpc-overload-matrix \
  127.0.0.1:8027
```

## 最短崩溃演示

完整矩阵有六个持久化边界；第一次阅读只需跑 `journal-body` 和 `commit` 两个对照：

```bash
./tests/smoke_brpc_request_id_crash_matrix.sh \
  ./build-brpc/eufs-mkfs \
  ./build-brpc/eufs_object_server \
  ./build-brpc/eufs_object_client \
  /tmp/eufs-brpc-two-stage-demo \
  127.0.0.1:8027 \
  journal-body commit

./build-brpc/eufsck /tmp/eufs-brpc-two-stage-demo/journal-body/eufs.img
./build-brpc/eufsck /tmp/eufs-brpc-two-stage-demo/commit/eufs.img
```

预期结果：`journal-body` 在 COMMIT 前退出，重启后对象不存在，同一 Request-ID 重试会
真正执行；`commit` 在 COMMIT 持久化后退出，重启后对象已经存在，同一 Request-ID 重试
只返回 ledger 中的既有结果且镜像摘要不变。两个镜像最终都必须通过 `eufsck`。

## 已验证结果

| 证据单元 | 已验证结果 | 原始证据 |
|---|---|---|
| 普通构建回归 | CTest `46/46` | GitHub Actions 与本地同一测试入口 |
| Request-ID 崩溃矩阵 | COMMIT 前 3 阶段恢复旧状态并执行重试；COMMIT 后 3 阶段恢复新状态并重放结果 | [`summary.log`](evidence/brpc/request_id_crash_matrix_20260808/summary.log) |
| 写队列过载 | 16 请求中 3 OK、13 OVERLOADED；13 次均由队列拒绝；重启校验和 `eufsck` 通过 | [`summary.log`](evidence/brpc/overload_matrix_20260809/queue-overload/summary.log) |
| 在途字节过载 | 16 请求中 2 OK、14 OVERLOADED；14 次均由字节门拒绝；重启校验和 `eufsck` 通过 | [`summary.log`](evidence/brpc/overload_matrix_20260809/byte-overload/summary.log) |

过载实验运行在 2 vCPU QEMU TCG guest；延迟只用于诊断准入顺序，不作为真实性能结论。

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
| 11 | `src/rpc/object_service_metrics.cpp` | bvar 计数、延迟和实时队列/字节 gauge |
| 12 | `src/app/eufs_object_load_client_main.cpp` | 唯一请求并发负载、状态分类和逐请求证据 |

早期“空 inode 写一个块”的受限计划器已经移到 `tests/support/`，只用于构造分配器、
日志和恢复测试，不再与 `eufsd` 的生产源码混放。

## 最小运行示例

```bash
./build/eufs-mkfs --image ./eufs.img --size 64M \
  --inodes 1024 --journal-blocks 256 --request-ledger-entries 1024 --force
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
- [项目答辩与简历事实表](docs/项目答辩与简历事实表_20260809.md)
- [日志发布顺序审计](evidence/research/stage-d-journal-publication-order-audit-20260722.md)
- [openEuler 集成审计](evidence/research/openeuler-integration-audit-20260710.md)
- [openEuler brpc Request-ID 六阶段崩溃矩阵](evidence/brpc/request_id_crash_matrix_20260808/summary.log)
- [openEuler brpc 过载隔离与重启一致性矩阵](evidence/brpc/overload_matrix_20260809/README.md)
- `evidence/debug/`：真实缺陷定位记录

每次 push 和 pull request 都由 GitHub Actions 重新构建并执行 CTest。普通本地测试以
可重复脚本为准；CI 暂不具备的 openEuler/brpc 环境只选择性保留端到端原始证据。

## 已知边界

- 已证明的挂载写原子单位是一次实际 FUSE write callback，不是整个用户态
  `write(2)`/`pwrite(2)` 系统调用；内核可能按页边界拆分请求。
- brpc 服务已经完成 openEuler 端到端构建、bvar、受控并发过载和进程崩溃后的客户端
  重试证据；尚未完成网络分区/丢包故障注入、裸机性能基线或多实例扩展验证。
- 当前持久化语义只能宣称 ledger 保留期间的 at-most-once mutation effect 与确定性
  replay，不宣称网络 exactly-once。
- 当前尚未实现 Delete/tombstone，版本令牌不宣称覆盖 delete/recreate ABA。
- 当前证据证明受控进程崩溃下的恢复语义，不等价于真实硬件断电认证。
- 尚未宣称完整 POSIX 兼容、稀疏文件、double-indirect block 或在线修复。

## 许可证

本项目采用 [Apache License 2.0](LICENSE)。
