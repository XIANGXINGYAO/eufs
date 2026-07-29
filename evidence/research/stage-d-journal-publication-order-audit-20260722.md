# Stage D 日志发布顺序一手来源审计

日期：2026-07-22

## 1. 审计问题

确定 eufs v1 中 ordered data、descriptor/payload、A/B control exposure、
COMMIT、home writeback 和 checkpoint 的顺序。重点不是证明 eufs 与 ext4
兼容，而是区分：一手来源直接支持的机制，以及 eufs 自定义 control 协议必须
自行证明的推导。

## 2. 一手来源与可核查观察

### xv6-riscv

固定版本：`b6dd660d4903947e5eb75ae9a457854f3707eb14`

来源：

- `kernel/log.c` 180-205：
  `https://github.com/mit-pdos/xv6-riscv/blob/b6dd660d4903947e5eb75ae9a457854f3707eb14/kernel/log.c#L180-L205`
- `kernel/log.c` 101-124：
  `https://github.com/mit-pdos/xv6-riscv/blob/b6dd660d4903947e5eb75ae9a457854f3707eb14/kernel/log.c#L101-L124`

直接观察：`commit()` 先执行 `write_log()` 写完整日志块，再执行
`write_head()`；源码明确把后者称为 real commit。随后才 `install_trans()`
写 home location，最后清空 header。恢复读取 header，只安装 header 声明的块。

支持范围：日志 body 先于提交记录；提交后写 home；重复安装 complete-block
image 可实现 redo。它不支持 eufs 的 A/B control 字段或 circular range 顺序，
因为 xv6 使用固定 header，磁盘协议不同。

### Linux ext4/JBD2 官方格式文档

来源：`https://docs.kernel.org/filesystems/ext4/journal.html`

直接观察：文档说明 transaction 内容完整写盘并 flush 后才写 commit record；
transaction 由 descriptor/data 开始并以 commit 结束；没有 commit 或 checksum
不匹配的 transaction 在 replay 时被丢弃；commit block 表示 transaction 已完整
写入 journal，之后才可写 final locations。文档还说明 ext4 默认 `data=ordered`
只把 metadata 写入 journal，并明确对比 `data=writeback` 不要求 dirty data 在
metadata journal write 前 flush；`data=journal` 则把 data 和 metadata 都写入日志。

支持范围：descriptor + payload + commit 的事务形状、COMMIT 界定可恢复事务、
metadata ordered 模式和 checksum。该文档不定义 eufs 的 A/B control，也不能单独
证明用户态 `fdatasync` 在所有硬件上的掉电保证。

### Linux JBD2 主线源码

固定版本：`248951ddc14de84de3910f9b13f51491a8cd91df`

来源：

- `fs/jbd2/commit.c` 650-915：
  `https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/fs/jbd2/commit.c#L650-L915`
- `fs/jbd2/recovery.c` 588-930：
  `https://github.com/torvalds/linux/blob/248951ddc14de84de3910f9b13f51491a8cd91df/fs/jbd2/recovery.c#L588-L930`

直接观察：commit path 先提交 descriptor/metadata journal buffers，等待这些 I/O，
同步 commit 模式随后提交并等待 commit record。recovery 从 superblock 给出的起点和
预期 sequence 开始扫描；magic 或 sequence 不匹配时停止；descriptor、payload 和
commit checksum 参与验证；只在 scan pass 确定的 transaction 边界内 replay。

支持范围：body I/O 先完成再确认 commit、按预期 txid/sequence 扫描、遇到尾部
不完整/旧记录停止、checksum 区分可重放内容。JBD2 有 async commit、revoke、
多阶段 recovery 和更复杂的损坏判断；eufs v1 不得声称复现这些能力。

## 3. eufs 自定义协议推导

eufs 的 A/B control 定义 `[tail, head)` 为 recovery 唯一可扫描范围，而 COMMIT
决定范围内事务是否已提交。由这两个本项目定义可推出：

1. descriptor/payload 必须先写并同步；否则 control 暴露后会产生正常崩溃即可
   导致的空/torn body，恢复无法区分实现违规和未完成写入。
2. control exposure 必须早于 COMMIT 持久化；否则 COMMIT 可能已经 durable，
   但旧 control 的扫描范围不包含它，恢复会漏掉已提交事务。
3. 因此唯一顺序是 `body sync -> control exposure sync -> COMMIT sync`。
4. control 暴露后 body 损坏不再属于正常 crash 尾部，应返回 `EUCLEAN`；只有
   COMMIT 缺失、撕裂或为不同 txid 的旧残留才表示合法未提交。

这是从 eufs 自定义扫描边界和 WAL 提交语义推出的协议，不是 xv6/JBD2 原格式。

## 4. v1 调度取舍

现有设计让 `journal commit lock` 持有到同步 checkpoint 完成。因此 planner 的
合法入口是 `used_blocks=0`；非零状态必须先由 recovery/checkpoint 处理，并返回
`-EBUSY`。此前“已有 `used=7` 再追加”只演示通用 ring 算术，不符合当前 v1
调度器，已从下一步要求中删除。

支持多笔 committed-but-uncheckpointed 事务需要异步 checkpoint 队列、逐事务
block 生命周期、延迟复用以及 revoke 或等价机制。v1 明确不做，不能通过放宽
`used_blocks` 检查伪装成支持多事务。

## 5. 可证伪条件与待实现证据

以下任一事实会推翻或要求修改当前协议：

- 实际 recovery 不以 selected control 的 `[tail, head)` 为唯一扫描边界；
- COMMIT 可以在 control 未暴露的位置被 recovery 可靠定位；
- planner 允许第二笔事务时已经存在经过证明的 checkpoint/revoke 生命周期；
- fault-injection 显示成功的 body sync 后、成功的 control exposure 下，正常进程
  崩溃仍可稳定产生 torn descriptor/payload。

当前已有 codec、control 初始化/选择、inactive-copy update、纯 ring-reservation
planner 和未暴露 journal body writer 的测试。body-before-control exposure 已由
Store API 强制，真实 descriptor/payload 字节、CRC、回卷、short write、`EINTR`、
write/sync 错误和未修改 control/COMMIT 均有测试。first-write 还已分类为 ordered
data `{292}` 与 metadata `{2,3}`；Store 在同一 fd/锁下验证 UUID、容量、完整
before-image 和 block 所有权，然后执行 data sync、body sync 和 control exposure。
exact-body-bound COMMIT writer 还会在 exposure 后从 Store 内部 body 生成绑定字段，
覆盖唯一 COMMIT 槽并同步；旧槽、回卷、partial write 和 sync error 已测试。仍无
recovery classifier、home replay、checkpoint、mount recovery 或 crash failpoint，
因此本审计不能当作崩溃一致性已完成的证据。
