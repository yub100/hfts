#ifndef hfts_worker_h
#define hfts_worker_h

#include "memory.hpp"
#include "tsa.hpp"
#include "mutex.hpp"
#include "thread_loacl.hpp"
#include "thread.hpp"
#include "waitingfiber.hpp"
#include "task.hpp"



namespace hfts {

class Scheduler;
class Fiber;

class Worker {
private:
    // provide data structure for self-use
    using TimePoint = std::chrono::system_clock::time_point;
    using Predicate = std::function<bool()>;

    using TaskQueue = containers::deque<Task>;
    using FiberQueue = containers::deque<Fiber*>;
    using FiberSet = containers::unordered_set<Fiber*>;

public:
    enum class Mode {
        // Worker will spawn a background thread to process tasks.
        MultiThreaded,

        // Worker will execute tasks when it yields.
        SingleThreaded,
    };

    Worker(Scheduler* scheduler, Mode mode, uint32_t id);

    // begin execution of the worker.
    void start() EXCLUDES(work.mutex);

    // stop execution of the worker.
    void stop() EXCLUDES(work.mutex);

    // wait() suspends execution of the current task until the predicate pred
    // returns true or the optional timeout is reached.
    // Fiber::wait()->Worker::wait().
    // Worker::wait()'s second parameters 'timeout' use ptr
    // so that can compatible with nullptr and valid parameters.
    // Fiber::wait()'s 'timeout' use reference, since it's user-oriented,
    // it's incompatible; every possible scenario must be implemented.
    bool wait(hfts::lock& lock, const TimePoint* timeout, const Predicate& pred) EXCLUDES(work.mutex);

    // wait() suspends execution of the current task until the fiber is
    // notified, or the optional timeout is reached.
    // Fiber::wait()->Worker::wait().
    bool wait(const TimePoint* timeout) EXCLUDES(work.mutex);

    // suspend() suspends the currently executing Fiebr and switch to other fiebr.
    void suspend(const TimePoint* timeout) REQUIRES(work.mutex);

    // enqueue(Fiber*) enqueues resuming of a suspend fiber to work.fibers.
    // only 
    void enqueue(Fiber* fiber) EXCLUDES(work.mutex);

    // enqueue(Task&&) enqueues a new, unstarted task.
    void enqueue(Task&& task) EXCLUDES(work.mutex);

    // tryLock() attempts to lock the worker for task enqueuing.
    // If the lock was successful then true is returned, and the caller must
    // call enqueueAndUnlock().
    bool tryLock() EXCLUDES(work.mutex);

    // enqueueAndUnlock() enqueues the task and unlocks the worker.
    // Must only be called after a call to tryLock() which returned true.
    void enqueueAndUnlock(Task&& task) REQUIRES(work.mutex);

    // runUntilShutdown() processes all tasks and fibers until there are no more
    // and shutdown is true, upon runUntilShutdown() returns.
    void runUntilShutdown() REQUIRES(work.mutex);

    // steal task from 'this' worker to other worker, result store in 'out'.
    bool steal(Task& out) EXCLUDES(work.mutex);

    static inline void setCurrent(Worker*);

    // return the Worker currently bound to the current thread.
    static inline Worker* getCurrent();

    // return the fiber currently being executed.
    inline Fiber* getCurrentFiber() const;

    // Unique id of this Worker.
    const uint32_t id;

private:
    friend class Scheduler;
    // run() processes tasks until stop() is called.
    void run() REQUIRES(work.mutex);

    // createWorkerFiber() creates a new fiber.
    Fiber* createWorkerFiber() REQUIRES(work.mutex);

    // switch execution to the given fiber.
    // The fiber must belong to this worker.
    void switchToFiber(Fiber*) REQUIRES(work.mutex);

    // execute all pending tasks include tasks in work.fibers
    void runUntilIdle() REQUIRES(work.mutex);

    // block until new tasks arrive.
    // But instead of blocking immediately, poll for a period of time first.
    void waitForWork() REQUIRES(work.mutex);

    // spinForWorkAndLock() attempts to steal work from another Worker, and keeps
    // the thread awake for a short duration. This reduces overheads of
    // frequently putting the thread to sleep and re-waking. It locks the mutex
    // before returning so that a stolen task cannot be re-stolen by other workers.
    void spinForWorkAndLock() ACQUIRE(work.mutex);

    void enqueueFiberTimeouts() REQUIRES(work.mutex);

    // REQUIRES(work.mutex) avoid lose notify.
    // example: state:running -> enqueue fiber to waiting, may loss signal occur in the middle
    inline void setFiberState(Fiber* fiber, Fiber::State to) const REQUIRES(work.mutex);

    inline void changeFiberState(Fiber* fiber, Fiber::State from, Fiber::State to) const REQUIRES(work.mutex);

    // Work holds tasks and fibers that are enqueued on the Worker
    struct Work {
        inline Work(Allocator*);

        // tasks.size() + fibers.size().
        std::atomic<uint64_t> num = {0};

        // numBlockedFibers >= waiting.size().
        GUARDED_BY(mutex) uint64_t numBlockedFibers = {0};
        GUARDED_BY(mutex) TaskQueue tasks;

        // fibers hold tasks and ready to run.
        GUARDED_BY(mutex) FiberQueue fibers;

        // WaitingFibers holds all the fibers waiting on a timeout.
        // Not be ready to run.
        GUARDED_BY(mutex) WaitingFibers waiting;

        // means if notify those worker threads that 
        // there are new tasks or fibers can run.
        GUARDED_BY(mutex) bool notifyAdded = true;

        hfts::mutex mutex;
        // unlike unbind is Disposable and cannot be lost, so added needn't GUARDED_BY(mutex)
        std::condition_variable added;

        template <typename F>
        inline void wait(F&&) REQUIRES(mutex);
    };

    class FastRnd {
    public:
        inline uint64_t operator()() {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            return x;
        }

    private:
        uint64_t x = std::chrono::system_clock::now().time_since_epoch().count();
    };

    HFTS_DECLARE_THREAD_LOCAL(Worker*, currentWorker);

    // generate random number
    FastRnd rng;
    Mode const mode;
    Scheduler* const scheduler;
    // worker owns mainFiber, thus use smartptr so that automatically destroy
    Allocator::unique_ptr<Fiber> mainFiber;
    // it's a reference for other resource
    Fiber* currentFiber = nullptr;
    // a 'index' : find the thread used by this worker
    Thread thread;
    Work work;
    // Those idle fibers without tasks and ready to run.
    FiberSet idleFibers;
    // All fibers created by this worker, but no mainFiber.
    // workerFibers.size() = idleFibers + work.fibers + work.waiting + ?current
    containers::vector<Allocator::unique_ptr<Fiber>, 16> workerFibers;
    bool shutdown = false;
};

Worker* Worker::getCurrent() {
    return Worker::currentWorker;
}

Fiber* Worker::getCurrentFiber() const {
    return currentFiber;
}
void Worker::setFiberState(Fiber* fiber, Fiber::State to) const {
    fiber->state = to;
}

void Worker::changeFiberState(Fiber* fiber, Fiber::State from, Fiber::State to) const {
    fiber->state = to;
}

}

#endif