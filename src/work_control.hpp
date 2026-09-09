#pragma once
#include <atomic>
#include <functional>
#include <stdexcept>

namespace cslu {
// Per-job cooperative cancellation, independent of the GUI and platform.
// A browser caller can supply a yielding callback; desktop callers need only
// an atomic flag. No worker is ever terminated while writing a file.
class OperationCancelled final : public std::runtime_error {
public:
    OperationCancelled() : std::runtime_error("Cancelled") {}
};
struct WorkControl {
    const std::atomic<bool>* cancelled = nullptr;
    std::function<void()> yield;
    void check() const {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) throw OperationCancelled{};
        if (yield) yield();
        if (cancelled && cancelled->load(std::memory_order_relaxed)) throw OperationCancelled{};
    }
};
inline thread_local const WorkControl* activeWorkControl = nullptr;
class ScopedWorkControl final {
    const WorkControl* previous_;
public:
    explicit ScopedWorkControl(const WorkControl& control) : previous_(activeWorkControl) {
        activeWorkControl = &control;
    }
    ~ScopedWorkControl() { activeWorkControl = previous_; }
    ScopedWorkControl(const ScopedWorkControl&) = delete;
    ScopedWorkControl& operator=(const ScopedWorkControl&) = delete;
};
inline void checkWork() { if (activeWorkControl) activeWorkControl->check(); }
} // namespace cslu
