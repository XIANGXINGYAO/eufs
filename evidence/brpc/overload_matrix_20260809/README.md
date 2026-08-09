# brpc 过载隔离与重启一致性证据

日期：2026-08-09

## 验证问题

本矩阵只回答三个问题：

1. 写队列满和在途 payload 字节耗尽时，服务是否明确返回
   `PUT_STATUS_OVERLOADED`，而不是超时、崩溃或存储错误；
2. 已接受请求和已拒绝请求在服务重启后是否仍保持各自结果；
3. 过载结束后额度、队列和磁盘一致性是否回到干净状态。

它不用于声明真实机器吞吐、P95 延迟或横向扩展能力。

## 实验设计

两组实验都发送 16 个唯一 key、唯一 16-byte Request-ID 的同步 Put RPC，
并用 16 个线程同时发起。brpc 自动重试被关闭，正式请求前用只读 Stat RPC
完成 Channel 预热。

| 场景 | payload | 写队列容量 | 在途字节上限 | 隔离变量 |
|---|---:|---:|---:|---|
| `queue-overload` | 4,096 B | 2 | 67,108,864 B | 只允许队列成为拒绝原因 |
| `byte-overload` | 4,000,000 B | 32 | 8,000,000 B | 只允许字节额度成为拒绝原因 |

每组均执行：并发负载、抓取 `/vars`、正常停服、重启、逐 key 校验、再次停服、
运行 `eufsck`。成功请求的内容必须逐字节一致；被拒绝请求必须返回
`READ_STATUS_NOT_FOUND`。

## v9 结果

| 场景 | OK | OVERLOADED | 字节拒绝计数 | 队列拒绝计数 | 采样峰值字节 | 采样峰值队列 |
|---|---:|---:|---:|---:|---:|---:|
| `queue-overload` | 3 | 13 | 0 | 13 | 8,192 | 1 |
| `byte-overload` | 2 | 14 | 14 | 0 | 8,000,000 | 1 |

两组均满足：

- RPC 错误和非预期状态均为 0；
- 指标中的拒绝原因与逐请求结果数量完全一致；
- 最终在途字节和队列深度均为 0；
- 重启后所有 OK key 内容正确，所有 OVERLOADED key 均不存在；
- `eufsck` 报告 0 个问题。

拒绝计数是服务内部对每次拒绝的累计记录；峰值列来自每 20 ms 抓取一次
`/vars` 的外部轮询，不是服务端高水位。队列场景采到的峰值 1 小于容量 2，说明
轮询遗漏了短暂满队列时刻；13 次队列拒绝仍由内部计数和逐请求结果交叉确认。

## 准入顺序缺陷与修复

旧实现先对整个 attachment 计算 SHA-256，再申请在途字节额度。v5 的
`byte-overload` 因此让最终被拒绝的 4 MB 请求也执行了线性摘要计算。修复后的
顺序为：廉价字段检查、EUFS v1 格式上限检查、申请字节额度、计算 SHA-256、
生成请求指纹、进入有界队列。

旧 v5 原始摘要保存在：

```text
evidence/debug/brpc_pre_admission_order_20260809/
```

v5 到 v9 的拒绝延迟观测值为：

| 场景 | v5 OVERLOADED P50 | v9 OVERLOADED P50 |
|---|---:|---:|
| `queue-overload` | 383,146 us | 73,091 us |
| `byte-overload` | 2,967,003 us | 1,078,943 us |

这些数字来自 2 vCPU QEMU TCG guest 的单轮实验，且两轮成功请求数量并不完全
相同。它们只作为发现准入顺序问题的诊断记录，不能用于声称延迟提升比例或真实性能。
“被字节门拒绝的请求不再执行 SHA-256”由当前代码控制流保证，不依赖这组噪声较大的
计时数据。

## 原始证据

每个场景目录保留：

- `results.tsv`：逐请求状态、key、Request-ID 和延迟；
- `load-summary.log`：状态分类和分组延迟摘要；
- `metric-snapshots.log`、`metrics-final.log`：运行中峰值样本和最终 bvar；
- `server-load.log`、`server-verify.log`：负载阶段与重启校验阶段服务日志；
- `eufsck.log`：最终磁盘一致性检查；
- `summary.log`：场景级结果摘要。

128 MiB 测试镜像、重复的逐 key Get 日志及临时 expected/actual 文件未纳入 Git；
它们可由 `tests/smoke_brpc_overload_matrix.sh` 重新生成。

## 环境边界

- guest：openEuler 24.03 LTS-SP4，x86_64，2 vCPU QEMU TCG；
- Apache brpc commit：`5f228c03040df8a1d98cfe77c8d4abd2ebf574fa`；
- 本矩阵证明单实例有界准入、拒绝隔离和重启一致性，不证明多实例扩展、
  多写 worker 并行或生产级性能。
