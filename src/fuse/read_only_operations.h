#pragma once

#include "fuse/stage_c_state.h"

#include <fuse3/fuse.h>

namespace eufs::fuse_adapter {

fuse_operations MakeReadOnlyOperations();

}  // namespace eufs::fuse_adapter
