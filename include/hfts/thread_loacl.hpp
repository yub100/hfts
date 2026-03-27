// A wrapper around a thread_local variable, or a pthread key

#ifndef hfts_thread_local_h
#define hfts_thread_local_h

#ifdef HFTS_USE_PTHREAD_THREAD_LOCAL
#include "debug.h"

#include <pthread.h>
#include <type_traits>

template <typename T>
class ThreadLocal {
  static_assert(std::is_pointer<T>::value,
                "The current implementation of ThreadLocal requires that T "
                "must be a pointer");

 public:
  inline ThreadLocal(T v) {
    pthread_key_create(&key, NULL);
    pthread_setspecific(key, v);
  }
  inline ~ThreadLocal() { pthread_key_delete(key); }
  inline operator T() const { return static_cast<T>(pthread_getspecific(key)); }
  inline ThreadLocal& operator=(T v) {
    pthread_setspecific(key, v);
    return *this;
  }

 private:
  ThreadLocal(const ThreadLocal&) = delete;
  ThreadLocal& operator=(const ThreadLocal&) = delete;

  pthread_key_t key;
};

#define HFTS_DECLARE_THREAD_LOCAL(TYPE, NAME) static ThreadLocal<TYPE> NAME
#define HFTS_INSTANTIATE_THREAD_LOCAL(TYPE, NAME, VALUE) \
  ThreadLocal<TYPE> NAME {                               \
    VALUE                                                \
  }

#else

#define HFTS_DECLARE_THREAD_LOCAL(TYPE, NAME) static thread_local TYPE NAME
#define HFTS_INSTANTIATE_THREAD_LOCAL(TYPE, NAME, VALUE) \
  thread_local TYPE NAME {                               \
    VALUE                                                \
  }

#endif

#endif  // hfts_thread_local_h
