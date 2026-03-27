#ifndef hfts_scheduler_h
#define hfts_scheduler_h

#include "containers.hpp"
#include "mutex.hpp"
#include "task.hpp"
#include "memory.hpp"
#include "tsa.hpp"
#include "thread_loacl.hpp"
#include "thread.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

namespace hfts {

class OSFiber;
class Worker;
class Fiber;

class Scheduler {
public:
    using ThreadInitializer = std::function<void(int workId)>;
    struct Config{
        // Every Fiber‘s default stack size.
        // Because fiber does not belong woker, so it's placed under config
        static constexpr size_t DefaultFiberStackSize = 1024*1024;

        // Those thread whose identity is worker
        struct WorkerThread {
            int count = 0;
            ThreadInitializer initializer;
        };
        WorkerThread workerThread;

        // Memory  allocator to use for the scheduler and other allocations(workers,fiber...).
        Allocator* allocator = Allocator::Default;
        // Every Fiber‘s stack size.
        size_t fiberStackSize = DefaultFiberStackSize;

        // Set the number of worker threads in the scheduler 
        // to equal the number of logical CPU cores in the system
        // and return this config.
        // The return type is Config, not a reference to avoid dangling references.
        static Config allCores();

        inline Config& setAllocator(Allocator*);
        inline Config& setFiberStackSize(size_t);
        inline Config& setWorkerThreadCount(int);
        inline Config& setWorkerThreadInitializer(const ThreadInitializer&);
    };

    Scheduler(const Config&);
    ~Scheduler();

    // get() return the bound to the current thread.
    static Scheduler* get();

    // bind() bind scheduler to current thread.
    // must be called through an instance.
    void bind();

    // dont need be called through an instance
    // because we can get Schedudler bound to this thread by Scheduler::get().
    static void unbind();

    // enqueue() queues the task for excution
    void enqueue(Task&& task);

    // getConfig() return teh config used to build the scheduler
    const Config& getConfig() const;

private:
    Scheduler(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    friend class Worker;
    friend class Fiber;
    
    // Maxinum number of threads(multiThreadWorkers)
    static constexpr size_t MaxWorkerThreads = 256;
    
    
    // the input of 'from' is a random, be big enough 
    // while input of onBeginSpinning() is a index has small range.
    // stealWork() attempts steak a task from 'from' and store result in 'out'.
    bool stealWork(Worker* thief, uint64_t from, Task& out);

    // the workers here will receive tasks first.
    void onBeginSpinning(int workerId);

    // setBound() sets the scheduler bound to the current thread.
    static void setBound(Scheduler* scheduler);

    // Declare bound.
    HFTS_DECLARE_THREAD_LOCAL(Scheduler*, bound);

    struct SingleThreadWorkers {
        inline SingleThreadWorkers(Allocator*);
        hfts::mutex mutex;
        using WorkerByTid = 
            hfts::containers::unordered_map<std::thread::id,
                                            Allocator::unique_ptr<Worker>>;
        
        GUARDED_BY(mutex) std::condition_variable unbind;
        GUARDED_BY(mutex) WorkerByTid byTid;
    };
    SingleThreadWorkers singleThreadWorkers;

    const Config cfg;

    // Workers which is spinning for tasks.
    std::array<std::atomic<int>, MaxWorkerThreads> spinningWorkers;

    // top point of Spinning worker stack,
    std::atomic<unsigned int>nextSpinningWorkerIdx = {0x8000000};
    
    std::atomic<unsigned int>nextEnqueueIndex = {0};

    // For the multiThreadWorker, index by nextSpinningWorkerIdx, value is workerId.
    std::array<Worker*, MaxWorkerThreads> workerThreads;

    static Config setConfigDefaults(const Config& config);
};

inline void schedule(Task&& t) {
    auto scheduler = Scheduler::get();
    scheduler->enqueue(std::move(t));
}

template <typename Function, typename... Args>
inline void schedule(Function&& f, Args&&... args) {
    auto scheduler = Scheduler::get();
    scheduler->enqueue(Task(std::bind(std::forward<Function>(f), std::forward<Args>(args)...)));
}

template <typename Function>
inline void schedule(Function&& f) {
    auto scheduler = Scheduler::get();
    scheduler->enqueue(Task(std::forward<Function>(f)));
}

////////////////////////////////////////////////////////////////////////////////
// Scheduler::Config
////////////////////////////////////////////////////////////////////////////////

Scheduler::Config& Scheduler::Config::setAllocator(Allocator* a) {
    allocator = a;
    return *this;
}

Scheduler::Config& Scheduler::Config::setFiberStackSize(size_t n) {
    fiberStackSize = n;
    return *this;
}

Scheduler::Config& Scheduler::Config::setWorkerThreadCount(int n) {
    workerThread.count = n;
    return *this;
}

Scheduler::Config& Scheduler::Config::setWorkerThreadInitializer(const ThreadInitializer& ti) {
    workerThread.initializer = ti;
    return *this;
}

} // namespace htfs

#endif // htfs_scheduler_h