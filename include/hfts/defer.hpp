#pragma once

#include <type_traits>
#include <utility>

namespace hfts {

template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F&& f) : func(std::forward<F>(f)), active(true) {}
  ScopeExit(ScopeExit&& other)
      : func(std::move(other.func)), active(other.active) {
    other.active = false;
  }
  ~ScopeExit() {
    if (active) {
      func();
    }
  }

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

  void release() { active = false; }

 private:
  F func;
  bool active;
};

template <typename F>
auto make_scope_exit(F&& f) -> ScopeExit<typename std::decay<F>::type> {
  return ScopeExit<typename std::decay<F>::type>(std::forward<F>(f));
}

}  // namespace hfts

#define HFTS_CONCAT_(a, b) a##b
#define HFTS_CONCAT(a, b) HFTS_CONCAT_(a, b)

#define defer(x) \
  auto HFTS_CONCAT(hfts_defer_, __LINE__) = ::hfts::make_scope_exit([&] { x; })
