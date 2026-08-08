# G2C openEuler 复现证据

日期：2026-08-03

## 结论

公开提交 `f0bd60eeeed1f2dd73c43507e26a3c3b88b81d7d` 已在 openEuler
24.03 LTS-SP4 guest 中从干净 detached HEAD 完成 G2C 原生构建。以下三个核心测试
全部通过：

- `object_replace_plan_test`
- `object_replace_recovery_test`
- `object_backend_test`

这证明该提交在 openEuler 的 GCC 12.3.1、FUSE 3.16.2 环境中可以编译，并通过
统一文件变更 planner、完整对象六阶段恢复和直接 Backend 条件替换证据。它不等于
openEuler 全量 CTest、真实 FUSE 挂载或 EUFS-brpc 集成已经完成。

## 源码来源

guest 直接访问 GitHub 时发生 `Recv failure: Connection reset by peer`，因此没有把
网络错误混入项目结论。宿主在确认 `main` 与 `origin/main` 同步后，从 Git 对象库生成
只含公开历史的 bundle：

```bash
git bundle create /tmp/eufs-f0bd60e.bundle HEAD
```

bundle 传入 guest 后克隆为 `/root/eufs-g2c-f0bd60e`。guest 中核验结果为：

```text
f0bd60eeeed1f2dd73c43507e26a3c3b88b81d7d
## HEAD (no branch)
```

该方式不会携带宿主未提交工作区内容。

## 环境与命令

```text
OS: openEuler 24.03 (LTS-SP4), x86_64
Kernel: 6.6.0-159.4.3.154.oe2403sp4.x86_64
GCC/G++: 12.3.1
CMake: 3.31.12
Ninja: 1.13.2
FUSE development package: 3.16.2
```

```bash
cmake \
  -S /root/eufs-g2c-f0bd60e \
  -B /root/eufs-g2c-f0bd60e/build-oe \
  -G Ninja \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

ninja -C /root/eufs-g2c-f0bd60e/build-oe -j2 \
  object_replace_plan_test \
  object_replace_recovery_test \
  object_backend_test

ctest --test-dir /root/eufs-g2c-f0bd60e/build-oe \
  --output-on-failure \
  -R '^(object_replace_plan_test|object_replace_recovery_test|object_backend_test)$'
```

配置和构建退出码均为 `0`；CTest 结果为 `3/3` 通过，退出码为 `0`。

## 原始日志

- [g2c-openeuler-build-20260803.log](g2c-openeuler-build-20260803.log)
- [g2c-openeuler-tests-20260803.log](g2c-openeuler-tests-20260803.log)

## G2 关闭依据与边界

G2C 同一公开提交已经具备：

- 独立干净 worktree 的正常全回归 `38/38`；
- G2C ASan/UBSan `3/3` 和 Backend clang TSan `1/1`；
- 六阶段 old-or-new、二次恢复和 `eufsck`；
- GitHub Actions run `30815601598` 成功；
- 本文件记录的 openEuler 原生构建和核心测试 `3/3`。

因此 G2 文件系统/直接 Backend 冻结门关闭。仍未完成的 `request_id + checksum`
持久化幂等、按字节背压、RPC 生命周期、bvar 和压测属于 G3，不回填为 G2 能力。
