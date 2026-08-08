# `src/` 现行代码边界

本目录只保留当前产品代码，不再保存教学阶段原型或测试专用落盘工具。

| 目录 | 当前职责 | 主要入口 |
|---|---|---|
| `app/` | 三个核心入口和两个可选 brpc 入口 | `eufsd_main.cpp`、`mkfs_main.cpp`、`eufsck_main.cpp`、`eufs_object_*_main.cpp` |
| `fuse/` | 内核 FUSE 请求适配和单次挂载状态 | `operations.cpp`、`mount_state.cpp` |
| `metadata/` | v1 磁盘格式以及 create/write/new-object 纯内存计划器 | `ondisk_format.*`、`*_plan.*` |
| `journal/` | WAL 格式、ring 规划、在线提交和启动恢复 | `journal_transaction_executor.*` |
| `storage/` | 镜像格式化、独占会话、bitmap 分配和只读解析 | `mkfs.*`、`image_reader.*` |
| `checker/` | 离线物理扫描、命名空间图分析和报告输出 | `consistency_checker.*` |
| `object/` | 不经过 FUSE 的对象接口、请求指纹和持久化 ledger 格式 | `object_backend.*`、`request_ledger_*` |
| `rpc/` | 可选 brpc 协议适配、attachment 和有界准入控制 | `object_service_impl.*`、`bounded_task_queue.*` |

测试构造器位于 `tests/support/`。其中的单块写计划和直接落盘函数不会链接进
`eufsd`；它们只负责构造恢复、检查器和旧边界测试所需的镜像状态。

推荐阅读主线：

```text
app/eufsd_main.cpp
-> fuse/mount_state.cpp
-> fuse/operations.cpp
-> storage/image_reader.cpp
-> metadata/file_write_plan.cpp
-> journal/journal_transaction_executor.cpp
-> journal/journal_control_store.cpp
-> journal/journal_recovery.cpp
```
