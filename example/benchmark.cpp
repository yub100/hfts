#include "hfts/defer.hpp"
#include "hfts/event.hpp"
#include "hfts/scheduler.hpp"
#include "hfts/thread.hpp"
#include "hfts/waitgroup.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  unsigned int taskCount = 200000;
  int taskWork = 32;
  unsigned int shortTaskCount = 1000000;
  int shortTaskWork = 1;
  unsigned int blockingTaskCount = 50000;
  int blockingTaskWork = 1;
  int blockingWarmupMs = 20;
  uint64_t reduceItems = 1ULL << 21;
  int reduceWork = 32;
  uint64_t chunkSize = 4096;
  std::vector<int> workerCounts;
};

struct TaskBenchmarkResult {
  int workers = 0;
  double seconds = 0.0;
  uint64_t checksum = 0;
};

struct ReduceBenchmarkResult {
  int workers = 0;
  double seconds = 0.0;
  double speedup = 0.0;
  uint64_t checksum = 0;
  size_t chunks = 0;
};

struct ShortTaskComparisonResult {
  int workers = 0;
  double hftsSeconds = 0.0;
  double poolSeconds = 0.0;
  double hftsTasksPerSecond = 0.0;
  double poolTasksPerSecond = 0.0;
  double advantage = 0.0;
  uint64_t hftsChecksum = 0;
  uint64_t poolChecksum = 0;
};

enum class ParseResult {
  Ok,
  Help,
  Error,
};

uint64_t xorshift64(uint64_t x) {
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return x;
}

uint64_t doWork(uint64_t seed, int rounds) {
  uint64_t value = seed + 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < rounds; ++i) {
    value = xorshift64(value + static_cast<uint64_t>(i + 1));
  }
  return value;
}

uint64_t reduceRange(uint64_t begin, uint64_t end, int work) {
  uint64_t acc = 0;
  for (uint64_t i = begin; i < end; ++i) {
    acc += doWork(i + 1, work);
  }
  return acc;
}

class PlainThreadPool {
 public:
  explicit PlainThreadPool(int workerCount) {
    threads.reserve(static_cast<size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) {
      threads.emplace_back([this] { workerLoop(); });
    }
  }

  ~PlainThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      shutdown = true;
    }
    ready.notify_all();
    for (size_t i = 0; i < threads.size(); ++i) {
      threads[i].join();
    }
  }

  void enqueue(std::function<void()>&& task) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      queue.emplace_back(std::move(task));
      pending++;
    }
    ready.notify_one();
  }

  void waitUntilIdle() {
    std::unique_lock<std::mutex> lock(mutex);
    drained.wait(lock, [this] { return pending == 0; });
  }

 private:
  void workerLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this] { return shutdown || !queue.empty(); });
        if (shutdown && queue.empty()) {
          return;
        }
        task = std::move(queue.front());
        queue.pop_front();
      }

      task();

      {
        std::lock_guard<std::mutex> lock(mutex);
        pending--;
        if (pending == 0) {
          drained.notify_all();
        }
      }
    }
  }

  std::mutex mutex;
  std::condition_variable ready;
  std::condition_variable drained;
  std::deque<std::function<void()> > queue;
  std::vector<std::thread> threads;
  uint64_t pending = 0;
  bool shutdown = false;
};

template <typename F>
double measureSeconds(F&& fn) {
  auto start = Clock::now();
  fn();
  auto end = Clock::now();
  return std::chrono::duration_cast<std::chrono::duration<double> >(end - start).count();
}

bool parseUnsigned(const char* text, uint64_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  unsigned long long value = std::strtoull(text, &end, 10);
  if (end == text || *end != '\0') {
    return false;
  }
  out = static_cast<uint64_t>(value);
  return true;
}

bool parseInt(const char* text, int& out) {
  uint64_t value = 0;
  if (!parseUnsigned(text, value) || value > static_cast<uint64_t>(INT_MAX)) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

std::vector<int> defaultWorkerCounts() {
  unsigned int logical = std::max(1u, hfts::Thread::numLogicalCPUs());
  std::vector<int> counts;
  counts.push_back(1);
  if (logical > 2) {
    counts.push_back(static_cast<int>(logical / 2));
  }
  if (logical > 1) {
    counts.push_back(static_cast<int>(logical));
  }
  std::sort(counts.begin(), counts.end());
  counts.erase(std::unique(counts.begin(), counts.end()), counts.end());
  return counts;
}

bool parseWorkerList(const char* text, std::vector<int>& out) {
  if (text == nullptr) {
    return false;
  }

  std::string list(text);
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find(',', start);
    auto token = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
    int workerCount = 0;
    if (!parseInt(token.c_str(), workerCount) || workerCount <= 0) {
      return false;
    }
    out.push_back(workerCount);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return !out.empty();
}

void printUsage(const char* argv0) {
  std::printf("Usage: %s [options]\n", argv0);
  std::printf("  --tasks N          Number of scheduled tasks in throughput benchmark\n");
  std::printf("  --task-work N      Xorshift rounds per task\n");
  std::printf("  --short-tasks N    Number of very short tasks in comparison benchmark\n");
  std::printf("  --short-work N     Xorshift rounds per short task\n");
  std::printf("  --blocking-tasks N Number of tasks in blocking benchmark\n");
  std::printf("  --blocking-work N  Xorshift rounds before/after blocking wait\n");
  std::printf("  --blocking-warmup N  Milliseconds to hold the gate closed before release\n");
  std::printf("  --reduce-items N   Number of items in parallel reduce benchmark\n");
  std::printf("  --reduce-work N    Xorshift rounds per reduce item\n");
  std::printf("  --chunk-size N     Items handled by one reduce task\n");
  std::printf("  --workers a,b,c    Worker counts to benchmark\n");
  std::printf("  --help             Show this message\n");
}

ParseResult parseArgs(int argc, char** argv, Options& options) {
  options.workerCounts = defaultWorkerCounts();

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (std::strcmp(arg, "--help") == 0) {
      printUsage(argv[0]);
      return ParseResult::Help;
    }
    if (i + 1 >= argc) {
      std::fprintf(stderr, "Missing value for %s\n", arg);
      return ParseResult::Error;
    }

    const char* value = argv[++i];
    uint64_t parsedUnsigned = 0;
    int parsedInt = 0;

    if (std::strcmp(arg, "--tasks") == 0) {
      if (!parseUnsigned(value, parsedUnsigned) || parsedUnsigned == 0 || parsedUnsigned > 100000000ULL) {
        std::fprintf(stderr, "Invalid --tasks value: %s\n", value);
        return ParseResult::Error;
      }
      options.taskCount = static_cast<unsigned int>(parsedUnsigned);
    } else if (std::strcmp(arg, "--task-work") == 0) {
      if (!parseInt(value, parsedInt) || parsedInt <= 0) {
        std::fprintf(stderr, "Invalid --task-work value: %s\n", value);
        return ParseResult::Error;
      }
      options.taskWork = parsedInt;
    } else if (std::strcmp(arg, "--short-tasks") == 0) {
      if (!parseUnsigned(value, parsedUnsigned) || parsedUnsigned == 0 || parsedUnsigned > 100000000ULL) {
        std::fprintf(stderr, "Invalid --short-tasks value: %s\n", value);
        return ParseResult::Error;
      }
      options.shortTaskCount = static_cast<unsigned int>(parsedUnsigned);
    } else if (std::strcmp(arg, "--short-work") == 0) {
      if (!parseInt(value, parsedInt) || parsedInt <= 0) {
        std::fprintf(stderr, "Invalid --short-work value: %s\n", value);
        return ParseResult::Error;
      }
      options.shortTaskWork = parsedInt;
    } else if (std::strcmp(arg, "--blocking-tasks") == 0) {
      if (!parseUnsigned(value, parsedUnsigned) || parsedUnsigned == 0 || parsedUnsigned > 100000000ULL) {
        std::fprintf(stderr, "Invalid --blocking-tasks value: %s\n", value);
        return ParseResult::Error;
      }
      options.blockingTaskCount = static_cast<unsigned int>(parsedUnsigned);
    } else if (std::strcmp(arg, "--blocking-work") == 0) {
      if (!parseInt(value, parsedInt) || parsedInt <= 0) {
        std::fprintf(stderr, "Invalid --blocking-work value: %s\n", value);
        return ParseResult::Error;
      }
      options.blockingTaskWork = parsedInt;
    } else if (std::strcmp(arg, "--blocking-warmup") == 0) {
      if (!parseInt(value, parsedInt) || parsedInt < 0) {
        std::fprintf(stderr, "Invalid --blocking-warmup value: %s\n", value);
        return ParseResult::Error;
      }
      options.blockingWarmupMs = parsedInt;
    } else if (std::strcmp(arg, "--reduce-items") == 0) {
      if (!parseUnsigned(value, parsedUnsigned) || parsedUnsigned == 0) {
        std::fprintf(stderr, "Invalid --reduce-items value: %s\n", value);
        return ParseResult::Error;
      }
      options.reduceItems = parsedUnsigned;
    } else if (std::strcmp(arg, "--reduce-work") == 0) {
      if (!parseInt(value, parsedInt) || parsedInt <= 0) {
        std::fprintf(stderr, "Invalid --reduce-work value: %s\n", value);
        return ParseResult::Error;
      }
      options.reduceWork = parsedInt;
    } else if (std::strcmp(arg, "--chunk-size") == 0) {
      if (!parseUnsigned(value, parsedUnsigned) || parsedUnsigned == 0) {
        std::fprintf(stderr, "Invalid --chunk-size value: %s\n", value);
        return ParseResult::Error;
      }
      options.chunkSize = parsedUnsigned;
    } else if (std::strcmp(arg, "--workers") == 0) {
      std::vector<int> workerCounts;
      if (!parseWorkerList(value, workerCounts)) {
        std::fprintf(stderr, "Invalid --workers value: %s\n", value);
        return ParseResult::Error;
      }
      options.workerCounts.swap(workerCounts);
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", arg);
      return ParseResult::Error;
    }
  }

  return ParseResult::Ok;
}

uint64_t sumValues(const std::vector<uint64_t>& values) {
  uint64_t sum = 0;
  for (size_t i = 0; i < values.size(); ++i) {
    sum += values[i];
  }
  return sum;
}

TaskBenchmarkResult runTaskBenchmark(int workers, unsigned int taskCount, int taskWork) {
  hfts::Scheduler::Config cfg;
  cfg.setWorkerThreadCount(workers);

  hfts::Scheduler scheduler(cfg);
  scheduler.bind();
  defer(scheduler.unbind());

  hfts::WaitGroup wg(taskCount);
  std::vector<uint64_t> outputs(taskCount, 0);

  TaskBenchmarkResult result;
  result.workers = workers;
  result.seconds = measureSeconds([&] {
    for (unsigned int i = 0; i < taskCount; ++i) {
      hfts::schedule([&, i] {
        outputs[i] = doWork(static_cast<uint64_t>(i) + 1, taskWork);
        wg.done();
      });
    }
    wg.wait();
  });
  result.checksum = sumValues(outputs);
  return result;
}

TaskBenchmarkResult runPlainPoolTaskBenchmark(int workers, unsigned int taskCount, int taskWork) {
  PlainThreadPool pool(workers);
  std::vector<uint64_t> outputs(taskCount, 0);

  TaskBenchmarkResult result;
  result.workers = workers;
  result.seconds = measureSeconds([&] {
    for (unsigned int i = 0; i < taskCount; ++i) {
      pool.enqueue([&, i] {
        outputs[i] = doWork(static_cast<uint64_t>(i) + 1, taskWork);
      });
    }
    pool.waitUntilIdle();
  });
  result.checksum = sumValues(outputs);
  return result;
}

ShortTaskComparisonResult runShortTaskComparison(
    int workers,
    unsigned int shortTaskCount,
    int shortTaskWork) {
  ShortTaskComparisonResult result;
  result.workers = workers;

  TaskBenchmarkResult hftsResult =
      runTaskBenchmark(workers, shortTaskCount, shortTaskWork);
  TaskBenchmarkResult poolResult =
      runPlainPoolTaskBenchmark(workers, shortTaskCount, shortTaskWork);

  result.hftsSeconds = hftsResult.seconds;
  result.poolSeconds = poolResult.seconds;
  result.hftsTasksPerSecond = static_cast<double>(shortTaskCount) / hftsResult.seconds;
  result.poolTasksPerSecond = static_cast<double>(shortTaskCount) / poolResult.seconds;
  result.advantage = poolResult.seconds / hftsResult.seconds;
  result.hftsChecksum = hftsResult.checksum;
  result.poolChecksum = poolResult.checksum;
  return result;
}

double runFiberBlockingBenchmark(
    int workers,
    unsigned int taskCount,
    int taskWork,
    int warmupMs,
    unsigned int& parkedBeforeRelease,
    uint64_t& checksum) {
  hfts::Scheduler::Config cfg;
  cfg.setWorkerThreadCount(workers);

  hfts::Scheduler scheduler(cfg);
  scheduler.bind();
  defer(scheduler.unbind());

  hfts::Event gate(hfts::Event::Mode::Manual, false);
  hfts::WaitGroup wg(taskCount);
  std::vector<uint64_t> outputs(taskCount, 0);
  std::atomic<unsigned int> parked(0);

  for (unsigned int i = 0; i < taskCount; ++i) {
    hfts::schedule([&, i] {
      outputs[i] = doWork(static_cast<uint64_t>(i) + 1, taskWork);
      parked.fetch_add(1);
      gate.wait();
      parked.fetch_sub(1);
      outputs[i] ^= doWork(static_cast<uint64_t>(i) + 1000003ULL, taskWork);
      wg.done();
    });
  }

  if (warmupMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs));
  }

  parkedBeforeRelease = parked.load();

  auto release = Clock::now();
  gate.signal();
  wg.wait();
  auto end = Clock::now();

  checksum = sumValues(outputs);
  return std::chrono::duration_cast<std::chrono::duration<double> >(end - release).count();
}

double runPlainPoolBlockingBenchmark(
    int workers,
    unsigned int taskCount,
    int taskWork,
    int warmupMs,
    unsigned int& parkedBeforeRelease,
    uint64_t& checksum) {
  PlainThreadPool pool(workers);
  std::vector<uint64_t> outputs(taskCount, 0);
  std::mutex gateMutex;
  std::condition_variable gateCv;
  bool released = false;
  std::atomic<unsigned int> parked(0);

  for (unsigned int i = 0; i < taskCount; ++i) {
    pool.enqueue([&, i] {
      outputs[i] = doWork(static_cast<uint64_t>(i) + 1, taskWork);
      {
        std::unique_lock<std::mutex> lock(gateMutex);
        parked.fetch_add(1);
        gateCv.wait(lock, [&] { return released; });
      }
      parked.fetch_sub(1);
      outputs[i] ^= doWork(static_cast<uint64_t>(i) + 1000003ULL, taskWork);
    });
  }

  if (warmupMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs));
  }

  parkedBeforeRelease = parked.load();

  auto release = Clock::now();
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    released = true;
  }
  gateCv.notify_all();
  pool.waitUntilIdle();
  auto end = Clock::now();

  checksum = sumValues(outputs);
  return std::chrono::duration_cast<std::chrono::duration<double> >(end - release).count();
}

ReduceBenchmarkResult runParallelReduce(
    int workers,
    uint64_t reduceItems,
    int reduceWork,
    uint64_t chunkSize,
    double baselineSeconds) {
  const size_t chunkCount = static_cast<size_t>((reduceItems + chunkSize - 1) / chunkSize);

  hfts::Scheduler::Config cfg;
  cfg.setWorkerThreadCount(workers);

  hfts::Scheduler scheduler(cfg);
  scheduler.bind();
  defer(scheduler.unbind());

  hfts::WaitGroup wg(static_cast<unsigned int>(chunkCount));
  std::vector<uint64_t> partials(chunkCount, 0);

  ReduceBenchmarkResult result;
  result.workers = workers;
  result.chunks = chunkCount;
  result.seconds = measureSeconds([&] {
    for (size_t chunk = 0; chunk < chunkCount; ++chunk) {
      uint64_t begin = static_cast<uint64_t>(chunk) * chunkSize;
      uint64_t end = std::min(begin + chunkSize, reduceItems);
      hfts::schedule([&, chunk, begin, end] {
        partials[chunk] = reduceRange(begin, end, reduceWork);
        wg.done();
      });
    }
    wg.wait();
  });
  result.checksum = sumValues(partials);
  result.speedup = baselineSeconds / result.seconds;
  return result;
}

TaskBenchmarkResult runSequentialReduce(uint64_t reduceItems, int reduceWork) {
  TaskBenchmarkResult result;
  result.workers = 1;
  result.seconds = measureSeconds([&] {
    result.checksum = reduceRange(0, reduceItems, reduceWork);
  });
  return result;
}

void printWorkerSweep(const std::vector<int>& workerCounts) {
  std::printf("worker sweep : ");
  for (size_t i = 0; i < workerCounts.size(); ++i) {
    std::printf("%d%s", workerCounts[i], i + 1 == workerCounts.size() ? "\n" : ", ");
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  ParseResult parseResult = parseArgs(argc, argv, options);
  if (parseResult == ParseResult::Help) {
    return 0;
  }
  if (parseResult == ParseResult::Error) {
    return 1;
  }

  std::printf("HFTS benchmark\n");
  std::printf("logical cpus : %u\n", hfts::Thread::numLogicalCPUs());
  printWorkerSweep(options.workerCounts);
  std::printf("task bench   : %u tasks, %d rounds/task\n", options.taskCount, options.taskWork);
  std::printf("short bench  : %u tasks, %d rounds/task\n", options.shortTaskCount, options.shortTaskWork);
  std::printf("blocking bench : %u tasks, %d rounds/phase, warmup=%d ms\n",
              options.blockingTaskCount,
              options.blockingTaskWork,
              options.blockingWarmupMs);
  std::printf("reduce bench : %llu items, %d rounds/item, chunk=%llu\n\n",
              static_cast<unsigned long long>(options.reduceItems),
              options.reduceWork,
              static_cast<unsigned long long>(options.chunkSize));

  std::printf("Task throughput\n");
  std::printf("%-8s %-12s %-18s %-18s\n", "workers", "time(ms)", "tasks/sec", "checksum");
  for (size_t i = 0; i < options.workerCounts.size(); ++i) {
    TaskBenchmarkResult result = runTaskBenchmark(
        options.workerCounts[i], options.taskCount, options.taskWork);
    double tasksPerSecond = static_cast<double>(options.taskCount) / result.seconds;
    std::printf("%-8d %-12.2f %-18.0f %-18llu\n",
                result.workers,
                result.seconds * 1000.0,
                tasksPerSecond,
                static_cast<unsigned long long>(result.checksum));
  }

  std::printf("\nShort task throughput vs plain thread pool\n");
  std::printf("%-8s %-12s %-12s %-16s %-16s %-10s\n",
              "workers",
              "hfts(ms)",
              "pool(ms)",
              "hfts tasks/s",
              "pool tasks/s",
              "hfts/pool");
  double bestAdvantage = 0.0;
  int bestAdvantageWorkers = 0;
  double bestHftsTasksPerSecond = 0.0;
  int bestHftsWorkers = 0;
  for (size_t i = 0; i < options.workerCounts.size(); ++i) {
    ShortTaskComparisonResult result = runShortTaskComparison(
        options.workerCounts[i], options.shortTaskCount, options.shortTaskWork);
    if (result.advantage > bestAdvantage) {
      bestAdvantage = result.advantage;
      bestAdvantageWorkers = result.workers;
    }
    if (result.hftsTasksPerSecond > bestHftsTasksPerSecond) {
      bestHftsTasksPerSecond = result.hftsTasksPerSecond;
      bestHftsWorkers = result.workers;
    }
    std::printf("%-8d %-12.2f %-12.2f %-16.0f %-16.0f %-10.2f",
                result.workers,
                result.hftsSeconds * 1000.0,
                result.poolSeconds * 1000.0,
                result.hftsTasksPerSecond,
                result.poolTasksPerSecond,
                result.advantage);
    if (result.hftsChecksum != result.poolChecksum) {
      std::printf("  checksum-mismatch");
    }
    std::printf("\n");
  }
  std::printf("summary    : best hfts throughput %.0f tasks/s at %d workers; "
              "best gain vs plain pool %.2fx at %d workers\n",
              bestHftsTasksPerSecond,
              bestHftsWorkers,
              bestAdvantage,
              bestAdvantageWorkers);

  std::printf("\nBlocking wait fan-out vs plain thread pool\n");
  std::printf("%-8s %-14s %-14s %-14s %-14s\n",
              "workers",
              "hfts parked",
              "pool parked",
              "hfts rel(ms)",
              "pool rel(ms)");
  for (size_t i = 0; i < options.workerCounts.size(); ++i) {
    unsigned int hftsParked = 0;
    unsigned int poolParked = 0;
    uint64_t hftsChecksum = 0;
    uint64_t poolChecksum = 0;
    double hftsRelease = runFiberBlockingBenchmark(
        options.workerCounts[i],
        options.blockingTaskCount,
        options.blockingTaskWork,
        options.blockingWarmupMs,
        hftsParked,
        hftsChecksum);
    double poolRelease = runPlainPoolBlockingBenchmark(
        options.workerCounts[i],
        options.blockingTaskCount,
        options.blockingTaskWork,
        options.blockingWarmupMs,
        poolParked,
        poolChecksum);
    std::printf("%-8d %-14u %-14u %-14.2f %-14.2f",
                options.workerCounts[i],
                hftsParked,
                poolParked,
                hftsRelease * 1000.0,
                poolRelease * 1000.0);
    if (hftsChecksum != poolChecksum) {
      std::printf("  checksum-mismatch");
    }
    std::printf("\n");
  }

  std::printf("\nParallel reduce\n");
  TaskBenchmarkResult baseline = runSequentialReduce(options.reduceItems, options.reduceWork);
  std::printf("sequential : %.2f ms  checksum=%llu\n",
              baseline.seconds * 1000.0,
              static_cast<unsigned long long>(baseline.checksum));
  std::printf("%-8s %-12s %-12s %-10s %-18s\n",
              "workers", "time(ms)", "speedup", "chunks", "checksum");

  for (size_t i = 0; i < options.workerCounts.size(); ++i) {
    ReduceBenchmarkResult result = runParallelReduce(
        options.workerCounts[i],
        options.reduceItems,
        options.reduceWork,
        options.chunkSize,
        baseline.seconds);
    std::printf("%-8d %-12.2f %-12.2f %-10zu %-18llu",
                result.workers,
                result.seconds * 1000.0,
                result.speedup,
                result.chunks,
                static_cast<unsigned long long>(result.checksum));
    if (result.checksum != baseline.checksum) {
      std::printf("  checksum-mismatch");
    }
    std::printf("\n");
  }

  return 0;
}
