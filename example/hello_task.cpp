#include "hfts/defer.hpp"
#include "hfts/event.hpp"
#include "hfts/scheduler.hpp"
#include "hfts/waitgroup.hpp"

#include <cstdio>

int main() {
  // Create a hfts scheduler using the 4 hardware threads.
  // Bind this scheduler to the main thread so we can call hfts::schedule()
  hfts::Scheduler::Config cfg;
  cfg.setWorkerThreadCount(4);

  hfts::Scheduler scheduler(cfg);
  scheduler.bind();
  defer(scheduler.unbind());  // Automatically unbind before returning.

  constexpr int numTasks = 10;

  // Create an event that is manually reset.
  hfts::Event sayHello(hfts::Event::Mode::Manual);

  // Create a WaitGroup with an initial count of numTasks.
  hfts::WaitGroup saidHello(numTasks);

  // Schedule some tasks to run asynchronously.
  for (int i = 0; i < numTasks; i++) {
    // Each task will run on one of the 4 worker threads.
    hfts::schedule([=] {  // All hfts primitives are capture-by-value.
      // Decrement the WaitGroup counter when the task has finished.
      defer(saidHello.done());

      printf("Task %d waiting to say hello...\n", i);

      // Blocking in a task?
      // The scheduler will find something else for this thread to do.
      sayHello.wait();

      printf("Hello from task %d!\n", i);
    });
  }

  sayHello.signal();  // Unblock all the tasks.

  saidHello.wait();  // Wait for all tasks to complete.

  printf("All tasks said hello.\n");

  // All tasks are guaranteed to complete before the scheduler is destructed.
}
