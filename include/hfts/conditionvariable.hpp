#ifndef hfts_conditionvariable_h
#define hfts_conditionvariable_h

#include "mutex.hpp"
#include "containers.hpp"
#include "fiber.hpp"

#include <atomic>

namespace hfts {

class Scheduler;

// This class can help different workers share their fibers by ConditionVariable::waiting
class ConditionVariable {
public:
    ConditionVariable(Allocator* allocator = Allocator::Default);

    inline void notify_one();

    inline void notify_all();

    template <typename Predicate>
    inline void wait(hfts::lock& lock, Predicate&& p);


    // Wait for a duration. It's a interface for user.
    template <typename Rep, typename Period, typename Predicate>
    inline bool wait_for(hfts::lock& lock, const std::chrono::duration<Rep, Period>& duration, Predicate&& p);

    // Call by wait_for, convert time duration to time point and wait().
    template <typename Time, typename Predicate>
    inline bool wait_until(hfts::lock& lock, const Time& timeout, Predicate&& p);


private:
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    // Safe waiting.
    hfts::mutex mutex;

    // Store waiting fibers. Need Allocator.
    containers::list<Fiber*> waiting;

    // Compatible with ordinary concurrency outside of the HFTS environment.
    std::condition_variable condition;

    // waitingNum = waiting.size() + waitingOnConditionNum
    std::atomic<int> waitingNum = {0};

    // use for ordinary concurrency outside hfts
    std::atomic<int> waitingOnConditionNum = {0};

};

ConditionVariable::ConditionVariable(Allocator* allocator) : waiting(allocator) {}

void ConditionVariable::notify_one() {
    if (waitingNum == 0) {
        return ;
    }
    {
        hfts::lock lock(mutex);
        if (waiting.size() > 0) {
            (*waiting.begin())->notify();
            return ;
        }
    }
    // Ordinary concurrency outside of the HFTS environment.
    if (waitingOnConditionNum > 0) {
        condition.notify_one();
    }
}

void ConditionVariable::notify_all() {
    if (waitingNum == 0) {
        return ;
    }
    {
        hfts::lock lock(mutex);
        for (auto fiber : waiting) {
            fiber->notify();
        }
    }
    // Ordinary concurrency outside of the HFTS environment.
    if (waitingOnConditionNum > 0) {
        condition.notify_all();
    }
}

// require(marl::mutex)
template <typename Predicate>
void ConditionVariable::wait(hfts::lock& lock, Predicate&& p) {
    if (p()) {
        return ;
    }
    waitingNum++;
    if (auto fiber = Fiber::current()) {
        mutex.lock();
        // iterator point to this fiber in the 'waiting'
        auto it = waiting.emplace_front(fiber);
        mutex.unlock();

        fiber->wait(lock, p);

        mutex.lock();
        waiting.erase(it);
        mutex.unlock();
    } else {
        waitingOnConditionNum++;
        // 'lock' hold mutex to product 'p', but sync by 'condition'.
        lock.wait(condition, p);
        waitingOnConditionNum--;
    }
    waitingNum--;
}

template <typename Rep, typename Period, typename Predicate>
bool ConditionVariable::wait_for(
        hfts::lock& lock,
        const std::chrono::duration<Rep, Period>& duration,
        Predicate&& p) {
    return wait_until(lock, std::chrono::system_clock::now() + duration, std::forward<Predicate>(p));
}

template <typename Time, typename Predicate>
bool ConditionVariable::wait_until(
        hfts::lock& lock,
        const Time& timeout,
        Predicate&& p) {
    if (p()) {
        return true;
    }
    bool res = false;
    waitingNum++;
    if (auto fiber = Fiber::current()) {
        mutex.lock();
        auto it = waiting.emplace_front(fiber);
        mutex.unlock();

        res = fiber->wait(lock, timeout, p);

        mutex.lock();
        waiting.erase(it);
        mutex.unlock();
    } else {
        waitingOnConditionNum++;
        res = lock.wait_until(condition, timeout, p);
        waitingOnConditionNum--;
    }
    waitingNum--;
    return res;
}

} // namespace hfts
#endif // hfts_conditionvariable_h