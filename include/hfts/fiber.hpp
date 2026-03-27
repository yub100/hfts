#ifndef hfts_fiber_h
#define hfts_fiber_h

#include "memory.hpp"
#include "osfiberm.hpp"
#include "mutex.hpp"


#include <functional>
#include <chrono>

namespace hfts {
class Worker;

class Fiber {
private:
    using Predicate = std::function<bool()>;
    using TimePoint = std::chrono::system_clock::time_point;
    
public:
    // return currently executing fiber or nullptr
    static Fiber* current();

    // this 'lock' is business lock, is to product 'pred', not is the work.mutex
    void wait(hfts::lock& lock, const Predicate& pred);

    template <typename Clock, typename Duration>
    bool wait(
        hfts::lock& lock,
        const std::chrono::time_point<Clock, Duration>& timeout,
        const Predicate& pred);
    
    // no timeout, thus it's a yield action.
    void wait();

    // Waiting until timeout but can't be notify. State become 'Waiting'
    template <typename Clock, typename Duration>
    bool wait(const std::chrono::time_point<Clock, Duration>& timeout);

    // FiberB gets this FiberA's Point, call FiberA's notify() notify itself.
    // FiberA in ConditionVariable::waiting.
    void notify();

    // The unique identifier of the Fiber.
    uint32_t const id;

private:
    friend class Scheduler;
    friend class Worker;
    friend class Allocator;

    enum class State {
        // The Fibers sit in Woker::idleFibers.
        Idle,

        // The Fibers sit in Worker::Work::waiting.
        Waiting,

        // The Fiber blocked on a wait() without timeout.
        Yielded,

        // The Fiber blocked on a wait() with timeout.
        Queued,

        Running,
    };

    Fiber(Allocator::unique_ptr<OSFiber>&&, uint32_t id);

    void switchTo(Fiber*);

    // Factory function, create work fiber.
    static Allocator::unique_ptr<Fiber> create(
        Allocator* allocator,
        uint32_t id,
        size_t stackSize,
        const std::function<void()>& func);
    
    // Factory function.
    // create for current thread, it's the mainFiber.
    static Allocator::unique_ptr<Fiber> createFromCurrentThread(
        Allocator* allocator,
        uint32_t id);

    // the OSFiber bound to this fiber.
    Allocator::unique_ptr<OSFiber> const OSF;
    // the worker bound to this fiber.
    Worker* const worker;
    State state = State::Running;
};

}

#endif



