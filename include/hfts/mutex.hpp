#ifndef hfts_mutex_h
#define hfts_mutex_h


#include "tsa.hpp"

#include <mutex>
#include <condition_variable>

namespace hfts {
class mutex {
public:
    inline void lock() ACQUIRE() { m.lock(); }
    inline void unlock() RELEASE() { m.unlock(); }
    inline bool tryLock() { return m.try_lock(); }

    template <typename Prediction>
    void inline wait_locked(std::condition_variable& cv, Prediction&& p) REQUIRES(this) {
        std::unique_lock<std::mutex> lock(m, std::adopt_lock);
        cv.wait(lock, std::forward<Prediction>(p));

        // release unique_lock but not unlock()
        lock.release();
    }

    template <typename Time, typename Prediction>
    inline bool wait_until_locked(std::condition_variable& cv, Time&& timepoint, Prediction&& p) {
        std::unique_lock<std::mutex> lock(m, std::adopt_lock);
        bool res = cv.wait_until(lock, std::forward<Time>(timepoint), std::forward<Prediction>(p));
        lock.release();
        // if p become true before timeout, return true.
        return res;
    }

private:
    friend class lock;
    std::mutex m;
};

class lock {
public:
    inline lock(mutex& mt) ACQUIRE(mt) : lock_m(mt.m) {}
    inline ~lock() = default;
    
    template <typename Prediction>
    inline void wait(std::condition_variable& cv, Prediction&& p) {
        cv.wait(lock_m, std::forward<Prediction>(p));
    }

    template <typename Time, typename Prediction>
    inline bool wait_until(std::condition_variable& cv, Time&& timepoint, Prediction&& p) {
        bool res = cv.wait_until(lock_m, std::forward<Time>(timepoint), std::forward<Prediction>(p));
        return res;
    }

    // Query whether currently owns mutex.
    inline bool owns_lock() { return lock_m.owns_lock(); }

    // primitive lock without tsa
    inline void primitive_lock() { lock_m.lock(); }

    // primitive unlock without tsa
    inline void primitive_unlock() { lock_m.unlock(); }

private:
    std::unique_lock<std::mutex> lock_m;
};
}

#endif