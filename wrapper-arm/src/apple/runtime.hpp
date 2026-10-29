// Minimal runtime for the 6.5.3 decrypt-only daemon.
//
// The 3.6.0 flow initialised FootHillConfig / RequestContext /
// LeaseManager by hand; 6.5.3 removed that whole storeservicescore layer
// from the decrypt path, and the singleton's own init (directories, device
// state) is what the library does internally on first instance(). What
// still matters here is setting up the process environment the library
// assumes (DNS mode, data dir) before the first Apple call.

#pragma once

#include <mutex>
#include <string>

#include "apple/loader.hpp"

namespace wrapper::apple {

class Runtime {
public:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Prepares the process environment and grabs the FootHill singleton.
    bool initialize(const Loader& loader);

    bool initialized() const { return initialized_; }
    bool decrypt_ready() const { return initialized_ && foothill_ != nullptr; }

    // The process-wide FootHill session controller. Never destroyed.
    void* foothill_session() const { return foothill_; }

    // Serialises decrypt batches (Apple's state is not thread-safe in this
    // path, and the listener may drive two sockets).
    std::mutex& decrypt_mutex() { return decrypt_mutex_; }

private:
    void* foothill_ = nullptr;
    bool initialized_{false};
    std::mutex decrypt_mutex_;
};

}  // namespace wrapper::apple
