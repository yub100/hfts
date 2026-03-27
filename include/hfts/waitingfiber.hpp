#ifndef hfts_waitingfiber_h
#define hfts_waitingfiber_h

#include "memory.hpp"
#include "fiber.hpp"
#include "containers.hpp"


namespace hfts {

struct WaitingFibers {
public:
    using TimePoint = std::chrono::system_clock::time_point;

    inline WaitingFibers(Allocator*);
    
    // Refactor function:(), return bool if there are any wait fibers.
    inline operator bool() const;

    // Returns the next fiber that has exceeded its timeout or nullptr.
    inline Fiber* take(const TimePoint& timeout);

    // Return the timepoint of the first member of timeouts
    inline TimePoint first() const;

    // Add timeout-fiber.
    inline void add(const TimePoint& timeout, Fiber* fiber);

    inline void erase(Fiber* fiber);

    // Returns true if fiber is waiting.
    inline bool contains(Fiber* fiber) const;

private:
    // The timeout fiber
    struct Timeout {
        TimePoint timepoint;
        Fiber* fiber;
        inline bool operator<(const Timeout&) const;
    };

    // Store all timeout node, order by timepoint,desc.
    // Convenient for retrieving recently timed-out fibers。
    containers::set<Timeout, std::less<Timeout>> timeouts;

    // Store all timeout-fibers.
    containers::unordered_map<Fiber*, TimePoint> fibers;
};

WaitingFibers::WaitingFibers(Allocator* allocator)
    : timeouts(allocator), fibers(allocator) {}

WaitingFibers::operator bool() const {
    return !fibers.empty();
}

inline Fiber* WaitingFibers::take(const TimePoint& timeout) {
    // fibers.empty() is true.
    if (!*this) {
        return nullptr;
    }
    auto it = timeouts.begin();
    if (it->timepoint > timeout) {
        return nullptr;
    }
    auto fiber = it->fiber;
    fibers.erase(fiber);
    return fiber;
}

WaitingFibers::TimePoint WaitingFibers::first() const {
    return timeouts.begin()->timepoint;
}

void WaitingFibers::add(const TimePoint& timeout, Fiber* fiber) {
    timeouts.emplace(Timeout{timeout, fiber});
    fibers.emplace(fiber, timeout);
}

void WaitingFibers::erase(Fiber* fiber) {
    // If not find this fiber, return fiber.end()
    auto it = fibers.find(fiber);
    if (it != fibers.end()) {
        auto timeout = it->second;
        timeouts.erase(Timeout{timeout, fiber});
    }
}

bool WaitingFibers::contains(Fiber* fiber) const {
    return fibers.count(fiber) != 0;
}

bool WaitingFibers::Timeout::operator<(const Timeout& t) const {
    if (timepoint != t.timepoint) {
        return timepoint < t.timepoint;
    }
    return fiber < t.fiber;
}

}
#endif