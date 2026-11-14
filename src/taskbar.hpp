#pragma once

#include <memory>

namespace cb {

struct BuildProgress;

namespace platform {

class Taskbar {
public:
    Taskbar();
    ~Taskbar();
    Taskbar(const Taskbar&) = delete;
    Taskbar& operator=(const Taskbar&) = delete;

    void bind(void* nativeHandle);
    void update(bool building, bool succeeded, const BuildProgress* progress);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace platform
} // namespace cb
