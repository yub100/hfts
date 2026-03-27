#include "memory.hpp"
#include "containers.hpp"

#include <functional>
#include <stdint.h>

// Convenient for use in assembly code.
struct hfts_fiber_context {
  // LR's parameter registers
  uintptr_t r0;
  uintptr_t r1;

  // special purpose registers
  uintptr_t r16;
  uintptr_t r17;
  uintptr_t r18;  // platform specific (maybe inter-procedural state)

  // callee-saved registers
  uintptr_t r19;
  uintptr_t r20;
  uintptr_t r21;
  uintptr_t r22;
  uintptr_t r23;
  uintptr_t r24;
  uintptr_t r25;
  uintptr_t r26;
  uintptr_t r27;
  uintptr_t r28;
  uintptr_t r29;

  uintptr_t v8;
  uintptr_t v9;
  uintptr_t v10;
  uintptr_t v11;
  uintptr_t v12;
  uintptr_t v13;
  uintptr_t v14;
  uintptr_t v15;

  uintptr_t SP;  // stack pointer
  uintptr_t LR;  // link register (R30)
};


extern "C" {
    static inline void hfts_fiber_trampoline(void (*target)(void*), void* arg) {
        target(arg);
    }

    inline void hfts_main_fiber_init(hfts_fiber_context* ctx) {}
    
    static inline void hfts_fiber_set_run(
            hfts_fiber_context* ctx,
            void* stack,
            uint32_t stack_size,
            void (*func)(void*),
            void* arg) {
        // uint8_t means stepsize is 8bits, actually stack_top = stack + 8*stack_size.
        uintptr_t* stack_top = (uintptr_t*)((uint8_t*)(stack) + stack_size);
        ctx->LR = (uintptr_t)&hfts_fiber_trampoline;
        ctx->r0 = (uintptr_t)func;
        ctx->r1 = (uintptr_t)arg;
        ctx->SP = ((uintptr_t)stack_top) & ~(uintptr_t)15;
    }
    
    extern void hfts_fiber_swap(hfts_fiber_context* from, const hfts_fiber_context* to);
}

namespace hfts {

class OSFiber {
public:
    inline OSFiber(Allocator*);
    inline ~OSFiber();

    inline void switchTo(OSFiber*);

    static inline Allocator::unique_ptr<OSFiber> createFiber(Allocator*, size_t stackSize, const std::function<void()>& func);

    static inline Allocator::unique_ptr<OSFiber> createFiberFromCurrentThread(Allocator*);

private:
    static inline void run(OSFiber* self);

    Allocator* allocator;
    std::function<void()> target;
    hfts_fiber_context context;
    Allocation stack;
};

OSFiber::OSFiber(Allocator* a) : allocator(a) {}

OSFiber::~OSFiber() {
    if (stack.ptr != nullptr) {
        allocator->free(stack);
    }
}

Allocator::unique_ptr<OSFiber> OSFiber::createFiberFromCurrentThread(Allocator* allocator) {
    auto out = allocator->make_unique<OSFiber>(allocator);
    out->context = {};
    hfts_main_fiber_init(&out->context);
    return out;
}

Allocator::unique_ptr<OSFiber> OSFiber::createFiber(
        Allocator* allocator,
        size_t stackSize,
        const std::function<void()>& func) {
    Allocation::Request request;
    request.size = stackSize;
    // Memory blocks are aligned to 16B.
    request.alignment = 16;
    request.usage = Allocation::Usage::Stack;
    auto out = allocator->make_unique<OSFiber>(allocator);
    // Initialize.
    out->context = {}; // Initialize all register members to 0
    out->stack = allocator->allocate(request);
    out->target = func;

    hfts_fiber_set_run(&out->context, &out->stack.ptr, static_cast<uint32_t>(stackSize),
                          reinterpret_cast<void(*)(void*)>(&OSFiber::run), out.get());
    return out;
}

void OSFiber::run(OSFiber* self) {
    self->target();
}

void OSFiber::switchTo(OSFiber* to) {
    hfts_fiber_swap(&context, &to->context);
}

}

