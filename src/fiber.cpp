#include "hfts/fiber.hpp"
#include "hfts/worker.hpp"


namespace hfts {

Fiber::Fiber(Allocator::unique_ptr<OSFiber>&& impl, uint32_t id)
    : id(id), OSF(std::move(impl)), worker(Worker::getCurrent()) {}

Fiber* Fiber::current() {
    auto worker = Worker::getCurrent();
    return worker != nullptr ? worker->getCurrentFiber() : nullptr;
}


void Fiber::wait(hfts::lock& lock, const Predicate& pred) {
    worker->wait(lock, nullptr, pred);
}

template <typename Clock, typename Duration>
bool Fiber::wait(
        hfts::lock& lock,
        const std::chrono::time_point<Clock, Duration>& timeout,
        const Predicate& pred) {
    using ToDuration = typename TimePoint::duration;
    using ToClock = typename TimePoint::clock;
    auto tp = std::chrono::time_point_cast<ToDuration, ToClock>(timeout);
    return worker->wait(lock, &tp, pred);
}

void Fiber::wait() {
  worker->wait(nullptr);
}

template <typename Clock, typename Duration>
bool Fiber::wait(const std::chrono::time_point<Clock, Duration>& timeout) {
    using ToDuration = typename TimePoint::duration;
    using ToClock = typename TimePoint::clock;
    auto tp = std::chrono::time_point_cast<ToDuration, ToClock>(timeout);
    return worker->wait(&tp);
}

void Fiber::notify() {
    worker->enqueue(this);
}
Allocator::unique_ptr<Fiber> Fiber::create(
        Allocator* allocator,
        uint32_t id,
        size_t stackSize,
        const std::function<void()>& func) {
    return allocator->make_unique<Fiber>(OSFiber::createFiber(allocator, stackSize, func), id);
}

Allocator::unique_ptr<Fiber>
Fiber::createFromCurrentThread(Allocator* allocator, uint32_t id) {
    return allocator->make_unique<Fiber>(OSFiber::createFiberFromCurrentThread(allocator), id);
}

void Fiber::switchTo(Fiber* to) {
    if (to != this) {
        OSF->switchTo(to->OSF.get());
    }
}


}