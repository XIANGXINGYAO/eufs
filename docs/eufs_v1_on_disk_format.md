# eufs v1 磁盘格式规范

状态：v1 已评审基线；实现与测试必须以本文字段偏移为准。
范围：superblock、inode、目录项和布局公式。journal 的记录格式在 Stage D 单独评审。

## 1. 依据与边界

本格式借鉴而不复制以下开源机制：

- xv6 `fs.h`：磁盘按 block 编址，inode 保存 direct 和 single-indirect block，目录是包含 dirent 的文件。
- Linux ext4 文档：典型 block size 为 4 KiB，结构位置按 block number 表示，ext4 主文件系统字段使用 little-endian。
- ext4 directory 文档：目录项通过 `record_length` 和 `name_length` 在目录块中顺序遍历。
- CRC32C：采用 Castagnoli 多项式；测试使用标准输入 `123456789 -> 0xE3069283`。

本项目不兼容 ext4、JBD2 或 xv6 的磁盘格式。magic、字段偏移、checksum 范围和恢复协议都属于 eufs 自己的版本化接口。

来源：

- `https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/fs.h`
- `https://docs.kernel.org/filesystems/ext4/overview.html`
- `https://docs.kernel.org/filesystems/ext4/blocks.html`
- `https://docs.kernel.org/filesystems/ext4/directory.html`
- `https://www.rfc-editor.org/rfc/rfc3720.html`

## 2. v1 全局规则

| 项目 | v1 决策 | 原因 |
|---|---|---|
| block size | 4096 bytes | 与当前 Linux page size、宿主文件系统基本块及 ext4 典型值一致；v1 只实现这一种大小 |
| byte order | little-endian | 固定跨进程格式，禁止直接写宿主 C++ struct |
| block number | `uint32_t` | 最大可表示 2^32 个 4 KiB block，即 16 TiB；超过项目范围且与 inode 指针宽度一致 |
| inode number | `uint32_t`，从 1 开始 | 0 作为无效/空目录项标记；bitmap bit 与 table slot 使用 `inode - 1` |
| inode record | 128 bytes | 每个 4 KiB inode-table block 恰好容纳 32 条记录 |
| block mapping | 12 direct + 1 single-indirect | 复用 xv6 的可验证映射机制；v1 最大文件为 `(12 + 1024) * 4096 = 4,243,456` bytes |
| filename | 1..255 bytes | 名称按原始字节保存；禁止 NUL、`/`、`.` 和 `..` |
| directory entry | 变长、4-byte aligned | 能复用删除后的空间，并允许检查 record 越界和 name 越界 |
| checksum | CRC32C | superblock header 和每条 inode 独立检测 bit corruption |

`open_count`、mutex、缓存和 C++ 容器地址不进入磁盘。`.`、`..` 由 FUSE `readdir` 合成，v1 目录块只保存真实子项。

## 3. 镜像区域布局

所有区域连续排列，v1 不允许隐藏空洞：

```text
block 0
  superblock

block 1 ...
  inode bitmap
  block bitmap
  inode table
  journal reserved region
  data region
```

布局由 `mkfs` 按以下公式计算：

```text
inode_bitmap_blocks = ceil(total_inodes / (4096 * 8))
block_bitmap_blocks = ceil(total_blocks / (4096 * 8))
inode_table_blocks  = ceil(total_inodes * 128 / 4096)

inode_bitmap_start = 1
block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks
inode_table_start  = block_bitmap_start + block_bitmap_blocks
journal_start      = inode_table_start + inode_table_blocks
data_start         = journal_start + journal_blocks
data_blocks        = total_blocks - data_start
journal_blocks     >= 5
```

block bitmap 覆盖整个镜像。`mkfs` 必须把 superblock、两个 bitmap、inode table 和 journal 区对应的 bit 预置为已占用。
journal 的 5 块下限来自 2 个 A/B control 加最小 3 槽事务
（descriptor、一个 metadata payload、commit）；更小的布局虽然能保留名字为
journal 的区域，却无法容纳任何合法 v1 事务，因此在 superblock 层直接拒绝。

inode `N` 的定位公式：

```text
index = N - 1
bitmap_block = inode_bitmap_start + index / 32768
bitmap_bit   = index % 32768
inode_offset = inode_table_start * 4096 + index * 128
```

## 4. Superblock block

superblock 占用 block 0。前 256 bytes 是受 CRC32C 保护的 header，其余 bytes 必须为 0。

| Offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII magic `EUFSIMG1` |
| 8 | 4 | format version = 1 |
| 12 | 4 | header size = 256 |
| 16 | 4 | block size = 4096 |
| 20 | 4 | inode record size = 128 |
| 24 | 4 | total blocks |
| 28 | 4 | total inodes |
| 32 | 4 | root inode |
| 36 | 4 | reserved = 0 |
| 40 | 8 | inode bitmap start/count，各 `uint32_t` |
| 48 | 8 | block bitmap start/count |
| 56 | 8 | inode table start/count |
| 64 | 8 | journal start/count |
| 72 | 8 | data start/count |
| 80 | 8 | compatible feature flags |
| 88 | 8 | read-only compatible feature flags |
| 96 | 8 | incompatible feature flags |
| 104 | 16 | filesystem UUID |
| 120 | 8 | creation time，Unix epoch nanoseconds |
| 128 | 4 | CRC32C；计算时本字段置 0，覆盖 bytes `[0, 256)` |
| 132 | 4 | direct block count = 12 |
| 136 | 4 | indirect levels = 1 |
| 140 | 4 | maximum name length = 255 |
| 144 | 112 | reserved = 0 |

任何 magic、版本、固定参数、CRC、区域顺序或范围校验失败都必须拒绝挂载，而不是猜测格式。

## 5. Inode record

每条 inode 固定 128 bytes，独立带 CRC32C。

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | POSIX mode，包含文件类型和权限 |
| 4 | 4 | uid |
| 8 | 4 | gid |
| 12 | 4 | link count；允许临时为 0 的已打开未链接 inode |
| 16 | 8 | logical file size |
| 24 | 8 | atime，Unix epoch nanoseconds |
| 32 | 8 | mtime，Unix epoch nanoseconds |
| 40 | 8 | ctime，Unix epoch nanoseconds |
| 48 | 4 | inode flags |
| 52 | 4 | inode number；参与 CRC；decoder 还必须与当前 table slot 的期望编号比较 |
| 56 | 8 | inode generation |
| 64 | 48 | 12 个 direct block number |
| 112 | 4 | single-indirect block number，0 表示未分配 |
| 116 | 8 | reserved = 0 |
| 124 | 4 | CRC32C；计算时本字段置 0，覆盖完整 128 bytes |

single-indirect block 包含 1024 个 little-endian `uint32_t` block number。普通文件的数据块属于 data；目录内容块和 indirect block 物理上也来自 data 区，但语义上属于 metadata，Stage D 必须把它们纳入 metadata journal。

## 6. Directory entry

目录块由若干条记录连续覆盖，不允许记录跨越 4 KiB block 边界。

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | inode number；0 表示 free record |
| 4 | 2 | record length，至少 8，4-byte aligned |
| 6 | 1 | name length |
| 7 | 1 | file type：0 unknown/free，1 regular，2 directory |
| 8 | N | name bytes，不带结尾 NUL |
| ... | padding | 清零直到 record length |

最短记录长度：

```text
align_up(8 + name_length, 4)
```

删除目录项时可以把记录变成 `inode=0, name_length=0, type=0`，后续插入可复用；目录管理器还需要定义相邻 free record 合并规则。decoder 必须拒绝越界、未对齐、非法名称和未知活动类型。

## 7. `/d/a.txt` 的读取链

```text
读取 block 0 并校验 superblock
-> 根据 root_inode 定位 inode table 记录
-> 扫描根目录 block，得到 "d" 的 inode
-> 校验该 inode bitmap bit，并确认 inode 类型为 directory
-> 扫描 /d 的目录 block，得到 "a.txt" 的 inode
-> 读取文件 inode
-> offset / 4096 选择 direct 或 single-indirect 项
-> physical_block * 4096 + offset_in_block 读取内容
```

目录中没有目标名称返回 `ENOENT`；目录项指向未分配/越界 inode、inode 指向未分配/越界 block、CRC 错误或记录越界返回 `EUCLEAN`。

## 8. 当前验收与未完成项

本阶段当前只允许声称：

- v1 字段宽度、字节序、偏移和布局公式已经固定；
- superblock、inode、目录项有独立 codec 和损坏拒绝单元测试。
- `eufs-mkfs` 能预分配镜像，初始化两个 bitmap、root inode 和两份 clean journal control，并在 body `fdatasync` 成功后最后写入 superblock；64 MiB 样例镜像已按原始偏移检查。
- `ImageReader` 会校验镜像长度、journal control UUID/几何、metadata/tail bitmap、inode CRC/槽位、目录记录、direct 和 single-indirect 引用；mkfs、reader 与 control store 使用 advisory `flock` 防止遵守协议的 writer 并发改写镜像。
- 只读 `eufsd` 已把同一空镜像挂载两次，root inode/权限/link count 均来自磁盘，挂载前后 SHA-256 相同。
- 当前磁盘版 `eufsd` 已通过 FUSE 持久化创建 `/a.txt` 并写入 `hello`，卸载后重挂载仍能读取，普通和 sanitizer 链路均通过。
- Stage D 已完成 A/B control store、事务 body/control staging、COMMIT writer、只读 recovery classifier 和同步 executor；executor 已用完整 after-image 执行 home replay、home sync 与 clean checkpoint，但尚未接入自动挂载和 FUSE 写路径。

尚不能声称：

- 完整 journal 执行链、home recovery、checkpoint 或 `eufsck` 已经实现；
- 当前直接 home-block create/write 具备崩溃一致性；
- 完整多块写入、目录空间复用或完整 POSIX 语义已经实现；
- 当前 codec 已在 openEuler 复跑。
