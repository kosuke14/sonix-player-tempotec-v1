// Stdio IPC worker for the mipsel listener (amplocal). Same 20-byte WV2I
// framing as wrapper-v2's daemon; only the decrypt opcodes exist.

#pragma once

#include "apple/loader.hpp"
#include "apple/runtime.hpp"

namespace wrapper {

int run_ipc_worker(apple::Runtime& rt, apple::Loader& loader);

}  // namespace wrapper
