#include "hfts/worker.hpp"
#include "hfts/scheduler.hpp"
#include "hfts/fiber.hpp"

inline void nop() {
#if defined(_WIN32)
  __nop();
#else
  __asm__ __volatile__("nop");
#endif
}

namespace hfts {

HFTS_INSTANTIATE_THREAD_LOCAL(Worker*, Worker::currentWorker, nullptr);

Worker::Worker(Scheduler* scheduler, Mode mode, uint32_t id)
    : id(id),
      mode(mode),
      scheduler(scheduler),
      work(scheduler->cfg.allocator),
      idleFibers(scheduler->cfg.allocator) {}

void Worker::setCurrent(Worker* w) {
    currentWorker = w;
}

void Worker::start() {
    switch (mode) {
        case Mode::MultiThreaded: {
            auto allocator = scheduler->cfg.allocator;
            thread = Thread([=] {
                Thread::setName("Thread<%.2d>", int(id));

                if (auto const& initFunc = scheduler->cfg.workerThread.initializer) {
                    initFunc(id);
                }
                Scheduler::setBound(scheduler);

                // 'this' is current worker.
                Worker::setCurrent(this);

                // mainFiber's id is 0.
                // Create mainFiber to store the initial environment of thread
                // so that switch between each other
                mainFiber = Fiber::createFromCurrentThread(scheduler->cfg.allocator, 0);
                
                // mainFiber.get() return a primitive pointer.
                currentFiber = mainFiber.get();
                {
                    hfts::lock lock(work.mutex);
                    run();
                }
                // Destroy the currently managed object and set the pointer to null.
                mainFiber.reset();
                // unbind current worker.
                Worker::setCurrent(nullptr); 
            });
            break;
        }
        case Mode::SingleThreaded: {
            Worker::setCurrent(this);
            // Create mainFiber to store the initial environment of thread
            mainFiber = Fiber::createFromCurrentThread(scheduler->cfg.allocator, 0);
            currentFiber = mainFiber.get();
            // Since it's not worker thread, dont need Worker::run()
            break;
        }
        default:
            break;
    }
}

void Worker::stop() {
    switch (mode) {
        case Mode::MultiThreaded: {
            // This is Worker::enqueue(), no need for SameThread.
            // But if call Scheduelr::enqueue(), must need SameThread.
            enqueue(Task([this] { shutdown = true; }, Task::Flag::SameThread));
            
            // Wait until thread run completed
            thread.join();
            break;
        }
        case Mode::SingleThreaded: {
            hfts::lock lock(work.mutex);
            shutdown = true;
            runUntilShutdown();
            Worker::setCurrent(nullptr);
            break;
        }
        default:
            break;
    }
}

bool Worker::wait(const TimePoint* timeout) {
    {
        hfts::lock lock(work.mutex);
        suspend(timeout);
    }
    return timeout == nullptr || std::chrono::system_clock::now() < *timeout;
}

bool Worker::wait(
        lock& lock,
        const TimePoint* timeout,
        const Predicate& pred) {
    while (!pred()) {
    
        work.mutex.lock();
        // Release lock, allow other thread operate pred.
        lock.primitive_unlock();

        suspend(timeout);

        work.mutex.unlock();
        lock.primitive_lock();

        if (timeout != nullptr && std::chrono::system_clock::now() >= *timeout) {
            return false;
        }
    }
    return true;
}

void Worker::suspend(const TimePoint* timeout) {
    if (timeout != nullptr) {
        //  will wake up when notified or when the time is up.
        changeFiberState(currentFiber, Fiber::State::Running, Fiber::State::Waiting);
        work.waiting.add(*timeout, currentFiber);
    } else {
        // Can wait indefinitely, no timeout, until be notified.
        changeFiberState(currentFiber, Fiber::State::Running, Fiber::State::Yielded);
    }

    // First wait until there's something else this worker can do.
    // only fibers is empty and worker has not task waitForWork wiil spin and block.
    // it's worker's operator not fiber's.
    waitForWork();

    work.numBlockedFibers++;
    
    // Truly block of fiber.
    if (!work.fibers.empty()) {
        work.num--;
        auto to = containers::take(work.fibers);
        switchToFiber(to);
    } else if (!idleFibers.empty()) {
        auto to = containers::take(idleFibers);
        switchToFiber(to);
    } else {
        switchToFiber(createWorkerFiber());
    }

    work.numBlockedFibers--;

    setFiberState(currentFiber, Fiber::State::Running);
}

bool Worker::tryLock() {
    return work.mutex.tryLock();
}

void Worker::enqueue(Fiber* fiber) {
    bool notify = false;
    {
        hfts::lock lock(work.mutex);
        switch (fiber->state) {
            case Fiber::State::Running:
            case Fiber::State::Queued:
                return;
            case Fiber::State::Idle:
            case Fiber::State::Yielded:
                break;
            case Fiber::State::Waiting:
                work.waiting.erase(fiber);
                break;
        }
        notify = work.notifyAdded;
        work.fibers.push_back(fiber);

        // Only this place and enqueueFiberTimeouts() can set state of fiber as Queued
        // means Queued fiber can only be transformed from Waiting fiber.
        setFiberState(fiber, Fiber::State::Queued);
        work.num++;
    }

    if (notify) {
        work.added.notify_one();
    }
}

void Worker::enqueue(Task&& task) {
    work.mutex.lock();
    enqueueAndUnlock(std::move(task));
}

void Worker::enqueueAndUnlock(Task&& task) {
    auto notify = work.notifyAdded;
    work.tasks.push_back(std::move(task));
    work.num++;
    work.mutex.unlock();
    if (notify) {
        work.added.notify_one();
    }
}

bool Worker::steal(Task& out) {
    if (work.num.load() == 0) {
        return false;
    }
    if (!work.mutex.tryLock()) {
        return false;
    }
    // If tasks content is [SameThread, None, None], then return false,
    // so that tasks can be executed sequentially.
    if (work.tasks.empty() || work.tasks.front().is(Task::Flag::SameThread)) {
        work.mutex.unlock();
        return false;
    }
    work.num--;
    out = containers::take(work.tasks);
    work.mutex.unlock();
    return true;
}

void Worker::run() {
    // If there are nothing to execute, MultiThreaded block firstly.
    if (mode == Mode::MultiThreaded) {
        work.wait([this](){
            return work.num > 0 || work.waiting || shutdown;
        });
    }
    // If Mode is SingleThreaded, entering this function must be 
    // due to a large number of tasks requiring single-task distribution
    // so there's no need to block via the if statement firstly.
    runUntilShutdown();
    switchToFiber(mainFiber.get());
}

void Worker::runUntilShutdown() {
    while (!shutdown || work.num > 0 || work.numBlockedFibers > 0U) {
        waitForWork();
        runUntilIdle();
    }
}

void Worker::waitForWork() {
    if (work.num > 0) {
        return;
    }

    if (mode == Mode::MultiThreaded) {
        // singleThread dont need register to the spinning queue.
        // the main difference comes down to MultiThreaded is simply a worker.
        // it means this worker both in onBeginSpinning Stack and working
        // if spinForWorkAndLock sucessfully obtain tasks.
        scheduler->onBeginSpinning(id);

        // Lock() is in start()::run().
        work.mutex.unlock();
        spinForWorkAndLock();
    }

    work.wait([this]() {
        return work.num > 0 || (shutdown && work.numBlockedFibers == 0U);
    });
    if (work.waiting) {
        enqueueFiberTimeouts();
    }
}

void Worker::spinForWorkAndLock() {
    Task stolen;
    constexpr auto duration = std::chrono::milliseconds(1);
    // Just like steady_clock
    auto start = std::chrono::high_resolution_clock::now();
    while (std::chrono::high_resolution_clock::now() - start < duration) {
        for (int i = 0; i < 256; i++) {
            for (int i = 0; i < 32; i++) {
                nop();
            }

            if (work.num > 0) {
                work.mutex.lock();
                if (work.num > 0) {
                    return;
                }
                work.mutex.unlock();
            }
        }
        // rng() is Random Number Generator
        if (scheduler->stealWork(this, rng(), stolen)) {
            work.mutex.lock();
            work.tasks.emplace_back(std::move(stolen));
            work.num++;
            return;
        }
        std::this_thread::yield();
    }
    work.mutex.lock();
}

void Worker::enqueueFiberTimeouts() {
    auto now = std::chrono::system_clock::now();
    while (auto fiber = work.waiting.take(now)) {
        work.fibers.push_back(fiber);
        work.num++;
    }
}

void Worker::runUntilIdle() {
    while (!work.fibers.empty() || !work.tasks.empty()) {
        while (!work.fibers.empty()) {
            work.num--;
            auto fiber = containers::take(work.fibers);
            changeFiberState(currentFiber, Fiber::State::Running, Fiber::State::Idle);
            idleFibers.emplace(currentFiber);
            
            switchToFiber(fiber);

            // Dequeue from idleFibers is performed by suspend().

            changeFiberState(currentFiber, Fiber::State::Idle, Fiber::State::Running);
        }

        if (!work.tasks.empty()) {
            work.num--;
            auto task = containers::take(work.tasks);
            work.mutex.unlock();
            task();

            // Assign a null value so that task can be destroyed.
            task = Task();
            work.mutex.unlock();
        }
    }
}

Fiber* Worker::createWorkerFiber() {
    auto fiberId = static_cast<uint32_t>(workerFibers.size() + 1);
    auto fiber = Fiber::create(scheduler->cfg.allocator, fiberId, scheduler->cfg.fiberStackSize, [&]() { run(); });
    auto ptr = fiber.get();

    // Since fiber have no task and is running, dont need put it in any queue except workerFibers.
    workerFibers.emplace_back(std::move(fiber));
    return ptr;
}

void Worker::switchToFiber(Fiber* to) {
    auto from = currentFiber;
    if (to == from) return;
    currentFiber = to;
    from->switchTo(to);
}
////////////////////////////////////////////////////////////////////////////////
// Scheduler::Worker::Work
////////////////////////////////////////////////////////////////////////////////
Worker::Work::Work(Allocator* allocator) 
    : tasks(allocator), fibers(allocator), waiting(allocator) {}

template <typename F>
void Worker::Work::wait(F&& f) {
    if (waiting) {
        mutex.wait_until_locked(added, waiting.first(), std::forward<F>(f));
    } else {
        mutex.wait_locked(added, std::forward<F>(f));
    }
}


}