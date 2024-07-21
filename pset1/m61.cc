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
#include <set>

// Utilities
size_t offset_to_next_aligned_size(size_t size) {
    auto offset = (size % alignof(std::max_align_t));
    return offset + size;
}

bool is_add_wraparound(size_t sum, size_t part) {
    if (sum < part) 
        return true;
    else 
        return false;
}

bool check_if_available_in_default_buffer(size_t pos, size_t buffer_sz, size_t size) {
    const size_t total_size = pos+size;
    if (is_add_wraparound((total_size), pos) || is_add_wraparound((total_size), size)) {
        return false;
    }
    else if (total_size > buffer_sz) {
        return false;
    }
    return true;
}

// Chunk header
struct chunk_header {
    size_t size;
    bool used;
    const char* func;
    int line;
    void *next_chunk;
};

// Memory buffer
struct m61_memory_buffer {
    char* buffer;
    size_t pos = 0;
    size_t size = 8 << 20; /* 8 MiB */

    void* get_next_chunk();
    m61_memory_buffer();
    ~m61_memory_buffer();
};



static m61_memory_buffer default_buffer;
static m61_statistics default_stats;
static std::map<size_t, std::stack<void*>> free_pool;
static std::set<void*> current_allocation;
static std::set<void*> freed_allocations;

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

void *m61_memory_buffer::get_next_chunk() {
    return &this->buffer[this->pos];
}

m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}

// Chunk Header
void fill_chunk_header(void *ptr, size_t sz, bool used, void *next_chunk, 
        [[maybe_unused]]const char* file, [[maybe_unused]]int line) {
    chunk_header* hdr = reinterpret_cast<chunk_header*>(ptr); 
    hdr->size = sz;
    hdr->used = used;
    hdr->func = file;
    hdr->line = line;
    hdr->next_chunk = next_chunk;
}

void* get_payload_ptr(void *ptr) {
    void *payload_ptr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ptr) + 
            offset_to_next_aligned_size(sizeof(chunk_header)));
    return payload_ptr;
}

chunk_header* extract_chunk_header(void *ptr) {
    return reinterpret_cast<chunk_header*>(static_cast<char*>(ptr)-offset_to_next_aligned_size(sizeof(chunk_header)));                                                                                                                                                                                                                       
}

// Freed allocations buffer

void split_current_chunk(void* ptr, size_t requested_size, size_t extra_memory) {
    chunk_header* current_chunk_header = extract_chunk_header(ptr);
    void *new_ptr = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ptr) + requested_size + sizeof(chunk_header));
    // Fill tail chunk header
    fill_chunk_header(new_ptr, extra_memory, false, current_chunk_header->next_chunk, nullptr, 0);
    // Update head chunk header, reusing same chunk header
    fill_chunk_header(current_chunk_header, requested_size, true, new_ptr, nullptr, 0);
    free_pool[extra_memory].push(new_ptr);
}

void free_extra_memory(void *ptr, size_t requested_size, size_t available_size) {
    const int64_t extra_memory = ((available_size - requested_size) - 2*sizeof(chunk_header));   
    // Is there space left to allocate one more chunk+header, after allocating this payload + header
    if (extra_memory > 0) {
        split_current_chunk(ptr, requested_size, extra_memory);
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

void merge_contiguous_free_chunks(chunk_header* hdr) {
    chunk_header* current_hdr = reinterpret_cast<chunk_header*>(hdr->next_chunk);
    while (current_hdr) {
        if (hdr->used) {
            break;
        }
        hdr->size += current_hdr->size + offset_to_next_aligned_size(sizeof(chunk_header));
        hdr->next_chunk = current_hdr->next_chunk;
        current_hdr = reinterpret_cast<chunk_header*>(current_hdr->next_chunk);
    }
}



// Statistics
void m61_statistics::update_successful_allocation(uintptr_t ptr, size_t requested_sz, size_t allocated_sz) {
    default_stats.ntotal++;
    default_stats.nactive++;
    default_stats.total_size += requested_sz;
    default_stats.active_size += requested_sz;
    if (ptr < default_stats.heap_min) {
        default_stats.heap_min = ptr;
    }
    if ((ptr+allocated_sz) > default_stats.heap_max ) {
        default_stats.heap_max = (ptr+allocated_sz);
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

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    const size_t aligned_header_size = offset_to_next_aligned_size(sizeof(chunk_header));
    const size_t aligned_chunk_size = offset_to_next_aligned_size(sz);
    const size_t total_size = aligned_chunk_size + aligned_header_size;
    if (is_add_wraparound(total_size, sz)) {
        default_stats.update_failed_allocation(sz);
        return nullptr;

    }
    if (!check_if_available_in_default_buffer(default_buffer.pos, default_buffer.size, total_size)) {
        void *ptr = nullptr;
        if (nullptr != (ptr = allocate_from_free_pool(sz))) {
            void *payload_ptr = get_payload_ptr(ptr);
            default_stats.update_successful_allocation(reinterpret_cast<uintptr_t>(ptr), sz, total_size);
            current_allocation.insert(payload_ptr);
            freed_allocations.erase(payload_ptr);
            return payload_ptr;
        }
        default_stats.update_failed_allocation(sz);
        return nullptr;
    }

    // Otherwise there is enough space; claim the next `sz` bytes
    void* ptr = default_buffer.get_next_chunk();
    default_buffer.pos += total_size;

    // Fill header for chunk being allocated
    fill_chunk_header(ptr, sz, true, default_buffer.get_next_chunk(), file, line);
    // Fill header for next chunk
    fill_chunk_header(default_buffer.get_next_chunk(), default_buffer.size - default_buffer.pos, false, nullptr, file, line);
    void *payload_ptr = get_payload_ptr(ptr);
    current_allocation.insert(payload_ptr);
    default_stats.update_successful_allocation(reinterpret_cast<uintptr_t>(ptr), sz, total_size);
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
 
    if (ptr < default_buffer.buffer+offset_to_next_aligned_size(sizeof(chunk_header)) || ptr > default_buffer.buffer + default_buffer.size) {
        fprintf(stderr, "MEMORY BUG: invalid free of pointer %p, not in heap", ptr);
        exit(EXIT_FAILURE);
    }

    if (current_allocation.find(ptr) == current_allocation.end()) {
        // Check if it was recently freed, then mark as a double free
        if (freed_allocations.find(ptr) != freed_allocations.end()) {
            fprintf(stderr, "MEMORY BUG: %s:%d: invalid free of pointer %p, double free", file, line, ptr);
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "MEMORY BUG: %s:%d: invalid free of pointer %p, not allocated", file, line, ptr);
        exit(EXIT_FAILURE);
    }

    chunk_header* hdr = extract_chunk_header(ptr);
    default_stats.update_free(reinterpret_cast<uintptr_t>(ptr), hdr->size);
    merge_contiguous_free_chunks(hdr);
    hdr->used = false;
    free_pool[hdr->size].push(reinterpret_cast<void*>(hdr));
    freed_allocations.insert(ptr);
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
