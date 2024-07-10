#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>
#include <map>
#include <stack>


struct m61_memory_buffer {
    char* buffer;
    size_t pos = 0;
    size_t size = 8 << 20; /* 8 MiB */

    m61_memory_buffer();
    ~m61_memory_buffer();
};

struct chunk_header {
    size_t size;
};

static m61_memory_buffer default_buffer;
static m61_statistics default_stats;
static std::map<size_t, std::stack<void*>> free_pool;

// Memory buffer
m61_memory_buffer::m61_memory_buffer() {
    void* buf = mmap(nullptr,    // Place the buffer at a random address
        this->size,              // Buffer should be 8 MiB big
        PROT_WRITE,              // We want to read and write the buffer
        MAP_ANON | MAP_PRIVATE, -1, 0);
                                 // We want memory freshly allocated by the OS
    assert(buf != MAP_FAILED);
    this->buffer = (char*) buf;
}

m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}

// Freed allocations buffer
void free_extra_memory(void *ptr, size_t requested_sz, size_t allocated_sz) {
    const int64_t extra_memory = ((allocated_sz - requested_sz) - 2*sizeof(chunk_header));   
    // Is there space left to allocate one more chunk+header, after allocating this payload + header
    if (extra_memory > 0) {
        void *new_ptr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ptr) + requested_sz + sizeof(chunk_header));
        free_pool[extra_memory].push(new_ptr);
    }
}
void* allocate_from_free_pool(size_t sz) {
    // Search for a best-fit strategy through various pool sizes
    for (auto &pool_size : free_pool) {
        const auto& size =  pool_size.first;
        auto& free_stack =  pool_size.second;
        if (size >= sz) {
            if (!free_stack.empty()) {
                void *ptr = free_stack.top();
                free_stack.pop();
                free_extra_memory(ptr, sz, size);
                return ptr;
            }
        }
    }
    return nullptr;
}


// Statistics

void m61_statistics::update_successful_allocation(uintptr_t ptr, size_t sz) {
    default_stats.ntotal++;
    default_stats.nactive++;
    default_stats.total_size += sz;
    default_stats.active_size += sz;
    if (ptr < default_stats.heap_min) {
        default_stats.heap_min = ptr;
    }
    if ((ptr+sz+sizeof(chunk_header)) > default_stats.heap_max ) {
        default_stats.heap_max = (ptr+sz+sizeof(chunk_header));
    }
}

void m61_statistics::update_failed_allocation(size_t sz) {
    default_stats.nfail++;
    default_stats.fail_size += sz;
}

void m61_statistics::update_free([[maybe_unused]]uintptr_t ptr, size_t sz) {
    default_stats.nactive--;
    default_stats.active_size -= sz;
}

// Utilities
size_t offset_to_next_aligned_size(size_t size) {
    auto offset = (size % alignof(std::max_align_t));
    return offset + size;
}

bool check_if_available_in_default_buffer(size_t pos, size_t buffer_sz, size_t size) {
    // check for wraparound
    if (pos + size < pos) {
        return true;
    }
    else if (pos + size > buffer_sz) {
        return true;
    }
    return false;
}

void* fill_chunk_header(void *ptr, size_t sz, [[maybe_unused]]const char* file, [[maybe_unused]]int line) {
    chunk_header* hdr = reinterpret_cast<chunk_header*>(ptr); 
    hdr->size = sz;

    void *payload_ptr = (char *)ptr + sizeof(chunk_header);
    return payload_ptr;
}

chunk_header* extract_chunk_header(void *ptr) {
    return reinterpret_cast<chunk_header*>(static_cast<char*>(ptr)-sizeof(chunk_header));                                                                                                                                                                                                                       
}




/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    size_t total_size = sz + sizeof(chunk_header);
    if (check_if_available_in_default_buffer(default_buffer.pos, default_buffer.size, total_size)) {
        void *ptr = nullptr;
        if (nullptr != (ptr = allocate_from_free_pool(sz))) {
            void *payload_ptr = fill_chunk_header(ptr, sz, file, line);
            default_stats.update_successful_allocation(reinterpret_cast<uintptr_t>(ptr), sz);
            return payload_ptr;
        }
        default_stats.update_failed_allocation(sz);
        return nullptr;
    }

    // Otherwise there is enough space; claim the next `sz` bytes
    void* ptr = &default_buffer.buffer[default_buffer.pos];
    default_buffer.pos += offset_to_next_aligned_size(total_size);

    void *payload_ptr = fill_chunk_header(ptr, sz, file, line);
    default_stats.update_successful_allocation(reinterpret_cast<uintptr_t>(ptr), sz);
    return payload_ptr;
}


/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.

void m61_free(void* ptr, const char* file, int line) {
    // avoid uninitialized variable warnings
    (void) ptr, (void) file, (void) line;
    if (ptr == nullptr) {
        return;
    }

    chunk_header* hdr = extract_chunk_header(ptr);
    free_pool[hdr->size].push(ptr);

    default_stats.update_free(reinterpret_cast<uintptr_t>(ptr), hdr->size);
    // Your code here. The handout code does nothing!
}


/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.

void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    size_t total_size = count * sz;
    if (count == 0 || sz == 0 || (sz != 0 && (total_size/sz != count))) {
        // count/size is zero, or total size has wraparound
        default_stats.update_failed_allocation(sz);
        return nullptr;
    }
    // Your code here (not needed for first tests).
    if (check_if_available_in_default_buffer(default_buffer.pos, default_buffer.size, total_size)) {
        // Not enough space left in default buffer for allocation
        default_stats.update_failed_allocation(sz);
        return nullptr;
    }
    void* ptr = m61_malloc(total_size, file, line);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}


/// m61_get_statistics()
///    Return the current memory statistics.

m61_statistics m61_get_statistics() {
    // Your code here.
    // The handout code sets all statistics to enormous numbers.
    return default_stats;
}


/// m61_print_statistics()
///    Prints the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats = m61_get_statistics();
    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Prints a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    // Your code here.
}
