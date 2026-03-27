#include "hfts/scheduler.hpp"
#include "hfts/fiber.hpp"
#include "hfts/worker.hpp"

namespace hfts {

////////////////////////////////////////////////////////////////////////////////
// Scheduler
////////////////////////////////////////////////////////////////////////////////
HFTS_INSTANTIATE_THREAD_LOCAL(Scheduler*, Scheduler::bound, nullptr);

// Because cfg is a const-variable which should be directly dupulicate or reference,
// thus we use setConfigDefaults() directly income address, and modify some parameters by it.
// The default constructor is called to initialize the Scheduler::workerThreads'element to null.
Scheduler::Scheduler(const Config& config)
        : cfg(setConfigDefaults(config)),
          workerThreads{},
          singleThreadWorkers(config.allocator) {
    // Create MultiThreaded woker.
    for (int i = 0; i < cfg.workerThread.count; i++) {
        // Initialize spinningWorkers
        spinningWorkers[i] = -1;
        workerThreads[i] = cfg.allocator->create<Worker>(this, Worker::Mode::MultiThreaded, i);
    }
    // Activate all MultiThreaded woker.
    for (int i = 0; i < cfg.workerThread.count; i++) {
        workerThreads[i]->start();
    }
}

Scheduler::~Scheduler() {
    // Wait for all singleThreadWorkers to die, then destory Scheduler.
    {
        hfts::lock lock(singleThreadWorkers.mutex);
        lock.wait(singleThreadWorkers.unbind, [this]() {
            return singleThreadWorkers.byTid.empty();
        });
    }
    for (int i = cfg.workerThread.count - 1; i >= 0; i--) {
        workerThreads[i]->stop();
    }
    for (int i = cfg.workerThread.count - 1; i >= 0; i--) {
        cfg.allocator->destroy(workerThreads[i]);
    }
}

Scheduler* Scheduler::get() {
    return bound;
}

void Scheduler::setBound(Scheduler* scheduler) {
    bound = scheduler;
}

void Scheduler::bind() {
    setBound(this);

    // Create SingleThreadWorker for this thread 
    auto worker = cfg.allocator->make_unique<Worker>(this, Worker::Mode::SingleThreaded, -1);
    worker->start();
    
    // Insert Worker in SingleThreadWorkers::WorkerByTid.
    {
        hfts::lock lock(singleThreadWorkers.mutex);
        auto tid = std::this_thread::get_id();

        // Must use std::move, because make_unique pointer is can be hold by multiple places.
        singleThreadWorkers.byTid.emplace(tid, std::move(worker));
    }
}

void Scheduler::unbind() {
    auto worker = Worker::getCurrent();
    auto tid = std::this_thread::get_id();
    worker->stop();
    {
        hfts::lock lock(get()->singleThreadWorkers.mutex);

        auto& singleTWorkers = get()->singleThreadWorkers.byTid;

        // Get current thread's singleThreadWorker by threadId
        auto it = singleTWorkers.find(tid);

        singleTWorkers.erase(it);

        // Notify ~scheduler can work
        if (singleTWorkers.empty()) {
            get()->singleThreadWorkers.unbind.notify_one();
        }
    }
    setBound(nullptr);
}

void Scheduler::enqueue(Task&& task) {
    if (task.is(Task::Flag::SameThread) || cfg.workerThread.count == 0) {
        if (auto w =  Worker::getCurrent()) {
            w->enqueue(std::move(task));
        }
        return ;
    }
    while (true) {
        // Priority use spinningWorks.
        auto i = --nextSpinningWorkerIdx % cfg.workerThread.count;
        auto idx = spinningWorkers[i].exchange(-1);

        // If there are not spinningWorks, select a worker fairly by nextEnqueueIndex.
        if (idx < 0) {
            idx = nextEnqueueIndex++ % cfg.workerThread.count;
        }

        auto worker = workerThreads[idx];
        if (worker->tryLock()) {
            // Instead of calling Work::enqueue(), call enqueueAndUnlock() directly
            // because only tryLock() has already successfully obtain mutex and 
            // needn't obtain mutex by enqueue().
            worker->enqueueAndUnlock(std::move(task));
            return ;
        }
    }
}

const Scheduler::Config& Scheduler::getConfig() const {
    return cfg;
}

bool Scheduler::stealWork(Worker* thief, uint64_t from, Task& out) {
    if (cfg.workerThread.count > 0) {
        auto fromWork = workerThreads[from % cfg.workerThread.count];
        if (fromWork != thief) {
            if (fromWork->steal(out)) {
                return true;
            }
        }
    }
    return false;
}

void Scheduler::onBeginSpinning(int workerId) {
    auto idx = nextSpinningWorkerIdx++ % cfg.workerThread.count;
    spinningWorkers[idx] = workerId;
}

Scheduler::SingleThreadWorkers::SingleThreadWorkers(Allocator* allocator) : byTid(allocator) {}

Scheduler::Config Scheduler::setConfigDefaults(const Scheduler::Config& config) {
    Scheduler::Config cfg{config};
    // configure your own parameters
    return cfg;
}

Scheduler::Config Scheduler::Config::allCores() {
    return Config().setWorkerThreadCount(Thread::numLogicalCPUs());
}

}