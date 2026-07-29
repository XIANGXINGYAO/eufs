# Stage C image_reader_test 空指针缺陷记录

日期：2026-07-20  
范围：测试代码缺陷，不是 eufs 镜像读取器运行时缺陷。

## 现象

在加入镜像 `flock` 和损坏目录项测试后：

```text
3/3 Test #3: image_reader_test ... SegFault
```

直接运行返回 139。gdb 定位：

```text
Program received signal SIGSEGV
main() at tests/image_reader_test.cpp
reader->superblock().data.start_block

reader = std::unique_ptr<eufs::storage::ImageReader> = {get() = 0x0}
```

## 原因

测试为了释放 shared `flock`，先执行了：

```cpp
reader.reset();
```

随后构造损坏目录块偏移时仍调用 `reader->superblock()`，形成确定的空指针解引用。该问题与 reader 对镜像字节的判断无关，是测试夹具生命周期错误。

## 修复

在 `reset()` 前保存：

```cpp
const std::uint32_t root_directory_block =
    reader->superblock().data.start_block;
```

释放 reader 后只使用保存的纯数值块号。修复后：

```text
normal CTest: 3/3 passed
ASan/UBSan CTest: 3/3 passed
```

## 防止重复

- 需要释放资源再修改底层镜像的测试，必须先复制后续需要的 geometry 值。
- 资源所有者 `reset()` 后不得再从其对象获取偏移或状态。
- 此缺陷只作为真实调试记录，不写成文件系统一致性或恢复成果。
