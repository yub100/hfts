#include "hfts/memory.hpp"

#include <cstring>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__) || defined(__EMSCRIPTEN__)
#include <sys/mman.h>
#include <unistd.h>
namespace {
// This was a static in pageSize(), but due to the following TSAN false-positive
// bug, this has been moved out to a global.
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68338
const size_t kPageSize = sysconf(_SC_PAGESIZE);
inline size_t pageSize() {
  return kPageSize;
}
inline void* allocatePages(size_t count) {
  auto mapping = mmap(nullptr, count * pageSize(), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  //hfts_ASSERT(mapping != MAP_FAILED, "Failed to allocate %d pages", int(count));
  if (mapping == MAP_FAILED) {
    mapping = nullptr;
  }
  return mapping;
}
inline void freePages(void* ptr, size_t count) {
  auto res = munmap(ptr, count * pageSize());
  (void)res;
  //hfts_ASSERT(res == 0, "Failed to free %d pages at %p", int(count), ptr);
}
inline void protectPage(void* addr) {
  auto res = mprotect(addr, pageSize(), PROT_NONE);
  (void)res;
  //hfts_ASSERT(res == 0, "Failed to protect page at %p", addr);
}
}  // anonymous namespace
#elif defined(__Fuchsia__)
#include <unistd.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>
namespace {
// This was a static in pageSize(), but due to the following TSAN false-positive
// bug, this has been moved out to a global.
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68338
const size_t kPageSize = sysconf(_SC_PAGESIZE);
inline size_t pageSize() {
  return kPageSize;
}
inline void* allocatePages(size_t count) {
  auto length = count * kPageSize;
  zx_handle_t vmo;
  if (zx_vmo_create(length, 0, &vmo) != ZX_OK) {
    return nullptr;
  }
  zx_vaddr_t reservation;
  zx_status_t status =
      zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0,
                  vmo, 0, length, &reservation);
  zx_handle_close(vmo);
  (void)status;
  //hfts_ASSERT(status == ZX_OK, "Failed to allocate %d pages", int(count));
  return reinterpret_cast<void*>(reservation);
}
inline void freePages(void* ptr, size_t count) {
  auto length = count * kPageSize;
  zx_status_t status = zx_vmar_unmap(zx_vmar_root_self(),
                                     reinterpret_cast<zx_vaddr_t>(ptr), length);
  (void)status;
  //hfts_ASSERT(status == ZX_OK, "Failed to free %d pages at %p", int(count), ptr);
}
inline void protectPage(void* addr) {
  zx_status_t status = zx_vmar_protect(
      zx_vmar_root_self(), 0, reinterpret_cast<zx_vaddr_t>(addr), kPageSize);
  (void)status;
  //hfts_ASSERT(status == ZX_OK, "Failed to protect page at %p", addr);
}
}  // anonymous namespace
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN 1
#include <Windows.h>
namespace {
inline size_t pageSize() {
  static auto size = [] {
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    return systemInfo.dwPageSize;
  }();
  return size;
}
inline void* allocatePages(size_t count) {
  auto mapping = VirtualAlloc(nullptr, count * pageSize(),
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  //hfts_ASSERT(mapping != nullptr, "Failed to allocate %d pages", int(count));
  return mapping;
}
inline void freePages(void* ptr, size_t count) {
  (void)count;
  auto res = VirtualFree(ptr, 0, MEM_RELEASE);
  (void)res;
  //hfts_ASSERT(res != 0, "Failed to free %d pages at %p", int(count), ptr);
}
inline void protectPage(void* addr) {
  DWORD oldVal = 0;
  auto res = VirtualProtect(addr, pageSize(), PAGE_NOACCESS, &oldVal);
  (void)res;
  //hfts_ASSERT(res != 0, "Failed to protect page at %p", addr);
}
}  // anonymous namespace
#else
#error "Page based allocation not implemented for this platform"
#endif

namespace {

// pagedMalloc() allocates size bytes of uninitialized storage with the
// specified minimum byte alignment using OS specific page mapping calls.
// If guardLow is true then reads or writes to the page below the returned
// address will cause a page fault.
// If guardHigh is true then reads or writes to the page above the allocated
// block will cause a page fault.
// The pointer returned must be freed with pagedFree().
void* pagedMalloc(size_t alignment,
                  size_t size,
                  bool guardLow,
                  bool guardHigh) {
  (void)alignment;
//   hfts_ASSERT(alignment < pageSize(),
//               "alignment (0x%x) must be less than the page size (0x%x)",
//               int(alignment), int(pageSize()));
  auto numRequestedPages = (size + pageSize() - 1) / pageSize();
  auto numTotalPages =
      numRequestedPages + (guardLow ? 1 : 0) + (guardHigh ? 1 : 0);
  auto mem = reinterpret_cast<uint8_t*>(allocatePages(numTotalPages));
  if (guardLow) {
    protectPage(mem);
    mem += pageSize();
  }
  if (guardHigh) {
    protectPage(mem + numRequestedPages * pageSize());
  }
  return mem;
}

// pagedFree() frees the memory allocated with pagedMalloc().
void pagedFree(void* ptr,
               size_t alignment,
               size_t size,
               bool guardLow,
               bool guardHigh) {
  (void)alignment;
//   hfts_ASSERT(alignment < pageSize(),
//               "alignment (0x%x) must be less than the page size (0x%x)",
//               int(alignment), int(pageSize()));
  auto numRequestedPages = (size + pageSize() - 1) / pageSize();
  auto numTotalPages =
      numRequestedPages + (guardLow ? 1 : 0) + (guardHigh ? 1 : 0);
  if (guardLow) {
    ptr = reinterpret_cast<uint8_t*>(ptr) - pageSize();
  }
  freePages(ptr, numTotalPages);
}

// alignedMalloc() allocates size bytes of uninitialized storage with the
// specified minimum byte alignment. The pointer returned must be freed with
// alignedFree().
inline void* alignedMalloc(size_t alignment, size_t size) {
  size_t allocSize = size + alignment + sizeof(void*);
  auto allocation = malloc(allocSize);
  auto aligned = reinterpret_cast<uint8_t*>(hfts::alignUp(
      reinterpret_cast<uintptr_t>(allocation), alignment));  // align
  memcpy(aligned + size, &allocation, sizeof(void*));  // pointer-to-allocation
  return aligned;
}

// alignedFree() frees memory allocated by alignedMalloc.
inline void alignedFree(void* ptr, size_t size) {
  void* base;
  memcpy(&base, reinterpret_cast<uint8_t*>(ptr) + size, sizeof(void*));
  free(base);
}

class DefaultAllocator : public hfts::Allocator {
 public:
  static DefaultAllocator instance;

  virtual hfts::Allocation allocate(
      const hfts::Allocation::Request& request) override {
    void* ptr = nullptr;

    if (request.useGuards) {
      ptr = ::pagedMalloc(request.alignment, request.size, true, true);
    } else if (request.alignment > 1U) {
      ptr = ::alignedMalloc(request.alignment, request.size);
    } else {
      ptr = ::malloc(request.size);
    }

    // hfts_ASSERT(ptr != nullptr, "Allocation failed");
    // hfts_ASSERT(reinterpret_cast<uintptr_t>(ptr) % request.alignment == 0,
    //             "Allocation gave incorrect alignment");

    hfts::Allocation allocation;
    allocation.ptr = ptr;
    allocation.request = request;
    return allocation;
  }

  virtual void free(const hfts::Allocation& allocation) override {
    if (allocation.request.useGuards) {
      ::pagedFree(allocation.ptr, allocation.request.alignment,
                  allocation.request.size, true, true);
    } else if (allocation.request.alignment > 1U) {
      ::alignedFree(allocation.ptr, allocation.request.size);
    } else {
      ::free(allocation.ptr);
    }
  }
};

DefaultAllocator DefaultAllocator::instance;

}  // anonymous namespace

namespace hfts {

Allocator* Allocator::Default = &DefaultAllocator::instance;

size_t pageSize() {
  return ::pageSize();
}

}  // namespace hfts
