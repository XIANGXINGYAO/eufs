#pragma once

#include "fuse/mount_state.h"

#include <fuse3/fuse.h>

namespace eufs::fuse_adapter {

// 返回交给 fuse_main 的完整回调函数表。
// 表中的 getattr/read/create/write 等函数定义在 operations.cpp，
// libfuse 收到内核请求后会通过对应函数指针进入我们的实现。
fuse_operations MakeOperations();

}  // namespace eufs::fuse_adapter
