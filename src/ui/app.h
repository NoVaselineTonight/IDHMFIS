#pragma once
// IDHMFIS — Application entry-point class
// UI runs on its own thread; engine state is consumed via double-buffered
// snapshot. Commands are posted through CommandQueue<AppCommand>.

#include <memory>

namespace idhmfis {

class Application {
public:
    Application();
    ~Application();

    // Blocks until the window is closed. Returns 0 on clean exit.
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace idhmfis
