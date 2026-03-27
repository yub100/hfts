#ifndef hfts_waitgroup_h
#define hfts_waitgroup_h

#include "conditionvariable.hpp"

#include <atomic>
#include <mutex>

namespace hfts {

class WaitGroup {
 public:
  // Constructs the WaitGroup with the specified initial count.
  inline WaitGroup(unsigned int initialCount = 0,
                                  Allocator* allocator = Allocator::Default);

  // add() increments the internal counter by count.
  inline void add(unsigned int count = 1) const;

  // done() decrements the internal counter by one.
  // Returns true if the internal count has reached zero.
  inline bool done() const;

  // wait() blocks until the WaitGroup counter reaches zero.
  inline void wait() const;

 private:
  struct Data {
    inline Data(Allocator* allocator);

    std::atomic<unsigned int> count = {0};
    ConditionVariable cv;
    hfts::mutex mutex;
  };
  const std::shared_ptr<Data> data;
};

WaitGroup::Data::Data(Allocator* allocator) : cv(allocator) {}

WaitGroup::WaitGroup(unsigned int initialCount /* = 0 */,
                     Allocator* allocator /* = Allocator::Default */)
    : data(std::make_shared<Data>(allocator)) {
  data->count = initialCount;
}

void WaitGroup::add(unsigned int count /* = 1 */) const {
  data->count += count;
}

bool WaitGroup::done() const {
  auto count = --data->count;
  if (count == 0) {
    hfts::lock lock(data->mutex);
    data->cv.notify_all();
    return true;
  }
  return false;
}

void WaitGroup::wait() const {
  hfts::lock lock(data->mutex);
  data->cv.wait(lock, [this] { return data->count == 0; });
}

}  // namespace hfts

#endif  // hfts_waitgroup_h