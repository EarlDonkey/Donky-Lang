#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <conio.h>

#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <process.h>
#include <inttypes.h>
#include <setjmp.h>
#include <winhttp.h>
#include <exdisp.h>
#include <olectl.h>
#include <wchar.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")

// --- NaN Boxing Tag Definitions ---
#define TAG_MASK 0xFFF8000000000000ULL
#define TAG_BASE 0x7FF8000000000000ULL
#define TAG_BOOL 1ULL
#define TAG_STRING 2ULL
#define TAG_ARRAY 3ULL
#define TAG_MAP 4ULL

static int rand_seeded = 0;

#define MAX_THREADS 64
static volatile int active_threads = 0;
static HANDLE active_threads_handles[MAX_THREADS];

// --- Garbage Collector & Thread Synchronization ---
CRITICAL_SECTION gc_lock;
static int gc_lock_init = 0;

typedef struct GCHeader {
    int      marked;      // payload-24
    size_t   size;        // payload-16  EXACT requested size (str_len relies on it)
    uint64_t type_tag;    // payload- 8
} GCHeader;               // 24 bytes: field offsets from the payload are identical to the old 32-byte header
typedef char gc_hdr_size_check[(sizeof(GCHeader) == 24) ? 1 : -1];

#define GC_CHUNK_SIZE  ((size_t)64 << 20)   // normal arena chunk
#define GC_DEDICATED   ((size_t)4 << 20)    // blocks bigger than this get their own chunk
#define GC_MIN_SPAN    32                   // 24 header + 8 payload (room for a free-list link)
#define GC_SMALL_MAX   512                  // exact-fit size classes up to this many bytes (8-byte steps)
#define GC_NCLASS      (GC_SMALL_MAX / 8 + 1)
#define GC_LINK(h)     (*(GCHeader**)((h) + 1))   // free-list "next" lives in the dead payload

typedef struct GCChunk {
    struct GCChunk* next;
    char*     base;
    size_t    cap;
    size_t    used;       // bump offset; everything in [0, used) is a contiguous run of blocks
    uint64_t* starts;     // 1 bit per 8 bytes: set = a live/allocated object header starts here
} GCChunk;

static GCChunk*  gc_chunks = NULL;                 // replaces gc_head
static GCChunk*  gc_cur = NULL;                    // chunk currently being bump-allocated
static GCHeader* gc_free_small[GC_NCLASS];         // indexed by span/8
static GCHeader* gc_free_large = NULL;
static size_t gc_bytes_allocated = 0;              // live payload bytes (recomputed by every sweep)
static size_t gc_bytes_reserved = 0;               // total arena bytes mapped

void gc_init_lock() {
    if (!gc_lock_init) {
        InitializeCriticalSection(&gc_lock);
        gc_lock_init = 1;
    }
}

static void gc_oom(void) { printf("Out of Memory!\n"); exit(1); }

static inline size_t gc_span(size_t size) {
    size_t s = (sizeof(GCHeader) + size + 7) & ~(size_t)7;
    return s < GC_MIN_SPAN ? GC_MIN_SPAN : s;
}
static inline void gc_bit_set(GCChunk* c, void* p) {
    size_t i = (size_t)((char*)p - c->base) >> 3;
    c->starts[i >> 6] |= (uint64_t)1 << (i & 63);
}
static inline void gc_bit_clr(GCChunk* c, void* p) {
    size_t i = (size_t)((char*)p - c->base) >> 3;
    c->starts[i >> 6] &= ~((uint64_t)1 << (i & 63));
}
static GCChunk* gc_find_chunk(void* p) {
    if (gc_cur && (char*)p >= gc_cur->base && (char*)p < gc_cur->base + gc_cur->cap) return gc_cur;
    for (GCChunk* c = gc_chunks; c; c = c->next)
        if ((char*)p >= c->base && (char*)p < c->base + c->cap) return c;
    return NULL;
}

static GCChunk* gc_chunk_new(size_t span, int dedicated) {
    size_t cap = dedicated ? ((span + 4095) & ~(size_t)4095) : GC_CHUNK_SIZE;
    GCChunk*  c    = (GCChunk*)malloc(sizeof(GCChunk));
    char*     mem  = (char*)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint64_t* bits = (uint64_t*)calloc((cap >> 9) + 1, sizeof(uint64_t));
    if (!c || !mem || !bits) gc_oom();
    c->base = mem; c->cap = cap; c->used = 0; c->starts = bits;
    c->next = gc_chunks; gc_chunks = c;
    gc_bytes_reserved += cap;
    return c;
}

// Turn [h, h+span) into a free block and put it on the right list.
static void gc_free_push(GCHeader* h, size_t span) {
    h->marked = 0; h->size = span - sizeof(GCHeader); h->type_tag = 0;
    if (span <= GC_SMALL_MAX) { GC_LINK(h) = gc_free_small[span >> 3]; gc_free_small[span >> 3] = h; }
    else                      { GC_LINK(h) = gc_free_large;            gc_free_large = h; }
}

// First-fit from the coalesced-block list. Never leaves a remainder < GC_MIN_SPAN
// (header.size must stay exact, so slack cannot be hidden inside a block).
static GCHeader* gc_large_take(size_t span) {
    GCHeader** pp = &gc_free_large;
    while (*pp) {
        GCHeader* f = *pp;
        size_t fspan = gc_span(f->size);
        if (fspan >= span) {
            size_t rem = fspan - span;
            if (rem == 0 || rem >= GC_MIN_SPAN) {
                *pp = GC_LINK(f);
                if (rem) gc_free_push((GCHeader*)((char*)f + span), rem);
                return f;
            }
        }
        pp = &GC_LINK(f);
    }
    return NULL;
}

static void* gc_alloc_locked(size_t size) {
    if (size > ((size_t)1 << 40)) gc_oom();
    size_t span = gc_span(size);
    GCHeader* h = NULL;
    GCChunk*  c = NULL;

    if (span <= GC_SMALL_MAX && gc_free_small[span >> 3]) {          // 1. exact-fit reuse
        h = gc_free_small[span >> 3];
        gc_free_small[span >> 3] = GC_LINK(h);
        c = gc_find_chunk(h);
    } else if (span > GC_DEDICATED) {                                // 2. huge object: own chunk
        c = gc_chunk_new(span, 1);
        h = (GCHeader*)c->base;
        c->used = span;
    } else if (gc_cur && gc_cur->cap - gc_cur->used >= span) {       // 3. bump
        c = gc_cur;
        h = (GCHeader*)(c->base + c->used);
        c->used += span;
    } else if ((h = gc_large_take(span)) != NULL) {                  // 4. arena full: reuse coalesced space
        c = gc_find_chunk(h);
    } else {                                                         // 5. new chunk
        c = gc_cur = gc_chunk_new(span, 0);
        h = (GCHeader*)c->base;
        c->used = span;
    }
    h->marked = 0;
    h->size = size;
    h->type_tag = 0;
    gc_bit_set(c, h);
    gc_bytes_allocated += size;
    return (void*)(h + 1);
}

void* gc_malloc(size_t size) {
    if (active_threads == 0) return gc_alloc_locked(size);   // single-threaded: no lock
    gc_init_lock();
    EnterCriticalSection(&gc_lock);
    void* p = gc_alloc_locked(size);
    LeaveCriticalSection(&gc_lock);
    return p;
}

typedef struct { uint64_t* data; size_t len; size_t cap; } Array;
// hs: cached 64-bit hash per entry. Only allocated once the hash index exists (len > 8).
typedef struct { uint64_t* keys; uint64_t* vals; uint32_t* idx; uint64_t* hs; size_t len; size_t cap; size_t idx_cap; } Map;

uint64_t donky_alloc_map(void);
uint64_t donky_map_set(uint64_t map_val, uint64_t key, uint64_t val);

uint64_t current_exception = 0;
static uint64_t g_globals = 0;

int is_number(uint64_t v) { return (v & TAG_MASK) != TAG_BASE; }
double as_number(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
uint64_t get_tag(uint64_t v) { return (v >> 48) & 0x7; }
uint64_t as_index(uint64_t v) { return v & 0x0000FFFFFFFFFFFFULL; }
uint64_t make_number(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }

void print_value(uint64_t val) {
    if (is_number(val)) {
        double d = as_number(val);
        if (d == (long long)d) printf("%lld", (long long)d);
        else printf("%g", d);
    } else if (get_tag(val) == TAG_BOOL) {
        printf("%s", (val & 1) ? "true" : "false");
    } else if (get_tag(val) == TAG_STRING) {
        printf("\"%s\"", (char*)as_index(val));
    } else if (get_tag(val) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(val);
        printf("[");
        for (size_t i = 0; i < arr->len; i++) {
            print_value(arr->data[i]);
            if (i < arr->len - 1) printf(", ");
        }
        printf("]");
    } else if (get_tag(val) == TAG_MAP) {
        Map* map = (Map*)as_index(val);
        printf("{");
        for (size_t i = 0; i < map->len; i++) {
            print_value(map->keys[i]);
            printf(": ");
            print_value(map->vals[i]);
            if (i < map->len - 1) printf(", ");
        }
        printf("}");
    } else {
        printf("null");
    }
}

void donky_print(uint64_t val) {
    print_value(val);
    printf("\n");
}

uint64_t donky_truthy(uint64_t val) {
    if (is_number(val)) return make_number(as_number(val) != 0.0 ? 1.0 : 0.0);
    if (get_tag(val) == TAG_BOOL) return make_number((val & 1) ? 1.0 : 0.0);
    return make_number(1.0);
}

int donky_has_exception() { return current_exception != 0; }
uint64_t donky_get_exception() { return current_exception; }
void donky_clear_exception() { current_exception = 0; }
void donky_throw(uint64_t val) { current_exception = val; }

static inline size_t str_len(const char* s) { return ((GCHeader*)s - 1)->size - 1; }

uint64_t donky_alloc_string(const char* s) {
    size_t len = s ? strlen(s) : 0;
    char* copy = gc_malloc(len + 1);
    if (s) memcpy(copy, s, len + 1);
    else copy[0] = 0;
    
    GCHeader* h = ((GCHeader*)copy) - 1;
    h->type_tag = TAG_STRING;
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)copy;
}

uint64_t donky_alloc_array() {
    Array* arr = gc_malloc(sizeof(Array));
    arr->data = NULL; arr->len = 0; arr->cap = 0;
    GCHeader* h = ((GCHeader*)arr) - 1;
    h->type_tag = TAG_ARRAY;
    return TAG_BASE | (TAG_ARRAY << 48) | (uint64_t)arr;
}

uint64_t donky_array_push(uint64_t arr_val, uint64_t val) {
    if (get_tag(arr_val) != TAG_ARRAY) return 0;
    Array* arr = (Array*)as_index(arr_val);
    if (arr->len == arr->cap) {
        arr->cap = arr->cap == 0 ? 4 : arr->cap * 2;
        arr->data = realloc(arr->data, arr->cap * sizeof(uint64_t));
    }
    arr->data[arr->len++] = val;
    return 0;
}

// --- Extended System Metrics ---

uint64_t donky_kill_process(uint64_t pid_val) {
    if (!is_number(pid_val)) return make_number(0.0);
    DWORD pid = (DWORD)as_number(pid_val);
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) return make_number(0.0);
    BOOL res = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return make_number(res ? 1.0 : 0.0); 
}

uint64_t donky_get_uptime() {
    ULONGLONG ms = GetTickCount64();
    return make_number((double)(ms / 1000));
}

uint64_t donky_get_cpu_cores() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return make_number((double)sysInfo.dwNumberOfProcessors);
}

uint64_t donky_get_disk_info() {
    ULARGE_INTEGER freeBytes, totalBytes, totalFreeBytes;
    if (GetDiskFreeSpaceExA("C:\\", &freeBytes, &totalBytes, &totalFreeBytes)) {
        uint64_t m = donky_alloc_map();
        donky_map_set(m, donky_alloc_string("total_gb"), make_number((double)(totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0))));
        donky_map_set(m, donky_alloc_string("free_gb"), make_number((double)(totalFreeBytes.QuadPart / (1024.0 * 1024.0 * 1024.0))));
        donky_map_set(m, donky_alloc_string("used_gb"), make_number((double)((totalBytes.QuadPart - totalFreeBytes.QuadPart) / (1024.0 * 1024.0 * 1024.0))));
        return m;
    }
    return donky_alloc_map();
}

uint64_t donky_alloc_map() {
    Map* map = gc_malloc(sizeof(Map));
    map->keys = NULL; map->vals = NULL; map->idx = NULL; map->hs = NULL;
    map->len = 0; map->cap = 0; map->idx_cap = 0;
    GCHeader* h = ((GCHeader*)map) - 1;
    h->type_tag = TAG_MAP;
    return TAG_BASE | (TAG_MAP << 48) | (uint64_t)map;
}

// --- Garbage Collection Mark & Sweep ---
void gc_mark(uint64_t val) {
    if (is_number(val)) return; 
    uint64_t tag = get_tag(val);
    void* ptr = (void*)as_index(val);
    if (tag != TAG_STRING && tag != TAG_ARRAY && tag != TAG_MAP) return;
    if (!ptr) return;
    
    GCHeader* header = ((GCHeader*)ptr) - 1;
    if (header->marked) return; 
    header->marked = 1;
    
    if (tag == TAG_ARRAY) {
        Array* arr = (Array*)ptr;
        for (size_t i = 0; i < arr->len; i++) gc_mark(arr->data[i]);
    } else if (tag == TAG_MAP) {
        Map* map = (Map*)ptr;
        for (size_t i = 0; i < map->len; i++) {
            gc_mark(map->keys[i]);
            gc_mark(map->vals[i]);
        }
    }
}

// Conservative-root filter: true only if `val` points at the header start of a
// currently-allocated object of the matching type. Stale stack words fail this.
static int gc_valid_object(uint64_t val) {
    uint64_t tag = get_tag(val);
    if (tag != TAG_STRING && tag != TAG_ARRAY && tag != TAG_MAP) return 0;
    uintptr_t p = (uintptr_t)as_index(val);
    if (p & 7) return 0;
    for (GCChunk* c = gc_chunks; c; c = c->next) {
        uintptr_t lo = (uintptr_t)c->base + sizeof(GCHeader);
        if (p >= lo && p < (uintptr_t)c->base + c->used) {
            size_t i = (size_t)(p - sizeof(GCHeader) - (uintptr_t)c->base) >> 3;
            if (!((c->starts[i >> 6] >> (i & 63)) & 1)) return 0;
            return ((GCHeader*)p - 1)->type_tag == tag;
        }
    }
    return 0;
}

static void gc_finalize(GCHeader* h) {
    void* payload = (void*)(h + 1);
    if (h->type_tag == TAG_ARRAY) {
        Array* arr = (Array*)payload;
        if (arr->data) free(arr->data);
    } else if (h->type_tag == TAG_MAP) {
        Map* map = (Map*)payload;
        if (map->keys) free(map->keys);
        if (map->vals) free(map->vals);
        if (map->idx) free(map->idx);
        if (map->hs) free(map->hs);
    }
    h->type_tag = 0;
}

// Close a run of dead blocks [run, end). A run touching the bump pointer just
// rewinds it; anything else becomes one coalesced free block.
static void gc_close_run(GCChunk* c, char* run, char* end) {
    if (end == c->base + c->used) c->used = (size_t)(run - c->base);
    else gc_free_push((GCHeader*)run, (size_t)(end - run));
}

void gc_sweep() {
    memset(gc_free_small, 0, sizeof(gc_free_small));   // free lists are rebuilt from scratch
    gc_free_large = NULL;
    size_t live = 0;
    int kept_std = 0;

    GCChunk** link = &gc_chunks;
    while (*link) {
        GCChunk* c = *link;
        char* p = c->base;
        char* end = c->base + c->used;
        char* run = NULL;

        while (p < end) {
            GCHeader* h = (GCHeader*)p;
            size_t span = gc_span(h->size);
            if (h->marked) {
                if (run) { gc_close_run(c, run, p); run = NULL; }
                h->marked = 0;
                live += h->size;
            } else {
                if (h->type_tag) gc_finalize(h);
                gc_bit_clr(c, h);
                if (!run) run = p;
            }
            p += span;
        }
        if (run) gc_close_run(c, run, end);

        // Give fully-empty chunks back to the OS (keep one normal chunk around).
        if (c->used == 0 && (c->cap != GC_CHUNK_SIZE || kept_std)) {
            *link = c->next;
            if (gc_cur == c) gc_cur = NULL;
            gc_bytes_reserved -= c->cap;
            VirtualFree(c->base, 0, MEM_RELEASE);
            free(c->starts);
            free(c);
            continue;
        }
        if (c->cap == GC_CHUNK_SIZE) kept_std = 1;
        link = &c->next;
    }

    if (!gc_cur) {
        for (GCChunk* c = gc_chunks; c; c = c->next)
            if (c->cap == GC_CHUNK_SIZE) { gc_cur = c; break; }
    }
    gc_bytes_allocated = live;
}

void gc_mark_stack() {
    jmp_buf env;
    setjmp(env); 
    ULONG_PTR lowLimit = 0, highLimit = 0;
    GetCurrentThreadStackLimits(&lowLimit, &highLimit);
    // Start at whichever of `env` (saved registers) and `lowLimit` is lower, so
    // callee-saved registers captured by setjmp are always inside the scanned range.
    uintptr_t* pa = (uintptr_t*)&env;
    uintptr_t* pb = (uintptr_t*)&lowLimit;
    uintptr_t* stack_ptr = pa < pb ? pa : pb;
    uintptr_t* top = (uintptr_t*)highLimit;

    while (stack_ptr < top) {
        uint64_t val = *stack_ptr;
        if ((val & TAG_MASK) == TAG_BASE && gc_valid_object(val)) gc_mark(val);
        stack_ptr++;
    }
}

void gc_collect() {
    // Only the calling thread's stack is scanned. With other threads alive their
    // roots are invisible and a sweep would free live objects, so refuse.
    if (active_threads > 0) return;

    extern uint64_t donky_args_array;
    if (donky_args_array != 0) gc_mark(donky_args_array);
    if (current_exception != 0) gc_mark(current_exception);
    if (g_globals != 0) gc_mark(g_globals);

    gc_mark_stack();
    gc_sweep();
}

uint64_t donky_gc() {
    gc_collect();
    return make_number(0.0);
}

// --- Map & Index Operations ---
static int map_key_eq(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return as_number(a) == as_number(b);
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING)
        return a == b || strcmp((char*)as_index(a), (char*)as_index(b)) == 0;   // pointer-equal fast path (interned keys)
    return a == b;
}

static inline uint64_t mix64(uint64_t b) {
    b ^= b >> 33; b *= 0xff51afd7ed558ccdULL;
    b ^= b >> 33; b *= 0xc4ceb9fe1a85ec53ULL;
    b ^= b >> 33; return b;
}

// 8 bytes at a time instead of FNV's one byte at a time.
static uint64_t hash_bytes(const unsigned char* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)n * 0xff51afd7ed558ccdULL), t = 0;
    while (n >= 8) {
        uint64_t w; memcpy(&w, p, 8);
        h = (h ^ w) * 0x9FB21C651E98DF25ULL;
        h ^= h >> 32;
        p += 8; n -= 8;
    }
    if (n) memcpy(&t, p, n);
    return mix64((h ^ t) * 0x9FB21C651E98DF25ULL);
}

static uint64_t map_hash(uint64_t k) {
    if (is_number(k)) {
        double d = as_number(k);
        if (d == 0.0) d = 0.0; // fold -0.0 into +0.0 to match map_key_eq
        uint64_t b; memcpy(&b, &d, 8);
        return mix64(b);
    }
    if (get_tag(k) == TAG_STRING) {
        const char* s = (const char*)as_index(k);
        return hash_bytes((const unsigned char*)s, str_len(s));
    }
    return mix64(k);
}

// h is only used when the map has a hash index (m->idx != NULL).
static int64_t map_find_h(Map* m, uint64_t key, uint64_t h) {
    if (!m->idx) {
        for (size_t i = 0; i < m->len; i++)
            if (map_key_eq(m->keys[i], key)) return (int64_t)i;
        return -1;
    }
    size_t mask = m->idx_cap - 1;
    size_t p = (size_t)h & mask;
    for (;;) {
        uint32_t e = m->idx[p];
        if (e == 0) return -1;
        if (m->hs[e - 1] == h && map_key_eq(m->keys[e - 1], key)) return (int64_t)(e - 1);
        p = (p + 1) & mask;
    }
}

static int64_t map_find(Map* m, uint64_t key) {
    return map_find_h(m, key, m->idx ? map_hash(key) : 0);
}

static void map_index_insert(Map* m, size_t entry) {
    size_t mask = m->idx_cap - 1;
    size_t p = (size_t)m->hs[entry] & mask;
    while (m->idx[p]) p = (p + 1) & mask;
    m->idx[p] = (uint32_t)(entry + 1);
}

static void map_rebuild_index(Map* m) {
    size_t need = 16;
    while (need < m->cap * 2) need <<= 1; // load factor <= 0.5, table never fills
    if (m->idx) free(m->idx);
    m->idx = (uint32_t*)calloc(need, sizeof(uint32_t));
    m->idx_cap = need;
    if (!m->hs) {                         // first time: hash every existing key once
        m->hs = (uint64_t*)malloc(m->cap * sizeof(uint64_t));
        for (size_t i = 0; i < m->len; i++) m->hs[i] = map_hash(m->keys[i]);
    }                                     // otherwise hs is already current: no rehashing
    for (size_t i = 0; i < m->len; i++) map_index_insert(m, i);
}

uint64_t donky_map_set(uint64_t map_val, uint64_t key, uint64_t val) {
    if (get_tag(map_val) != TAG_MAP) return 0;
    Map* map = (Map*)as_index(map_val);
    uint64_t h = map->idx ? map_hash(key) : 0;
    int64_t f = map_find_h(map, key, h);
    if (f >= 0) { map->vals[f] = val; return 0; }
    int grew = 0;
    if (map->len == map->cap) {
        map->cap = map->cap == 0 ? 4 : map->cap * 2;
        map->keys = realloc(map->keys, map->cap * sizeof(uint64_t));
        map->vals = realloc(map->vals, map->cap * sizeof(uint64_t));
        if (map->hs) map->hs = realloc(map->hs, map->cap * sizeof(uint64_t));
        grew = 1;
    }
    map->keys[map->len] = key;
    map->vals[map->len] = val;
    if (map->idx) {
        map->hs[map->len] = h;
        map->len++;
        if (grew) map_rebuild_index(map); else map_index_insert(map, map->len - 1);
    } else {
        map->len++;
        if (map->len > 8) map_rebuild_index(map);
    }
    return 0;
}

uint64_t donky_index_get(uint64_t target, uint64_t index) {
    if (get_tag(target) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(target);
        size_t i = (size_t)as_number(index);
        if (i < arr->len) return arr->data[i];
    } else if (get_tag(target) == TAG_MAP) {
        Map* map = (Map*)as_index(target);
        int64_t f = map_find(map, index);
        if (f >= 0) return map->vals[f];
    }
    return 0;
}

uint64_t donky_index_set(uint64_t target, uint64_t index, uint64_t val) {
    if (get_tag(target) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(target);
        size_t i = (size_t)as_number(index);
        if (i < arr->len) arr->data[i] = val;
    } else if (get_tag(target) == TAG_MAP) {
        donky_map_set(target, index, val);
    }
    return 0;
}

uint64_t donky_eq(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return make_number(as_number(a) == as_number(b) ? 1.0 : 0.0);
    if (get_tag(a) == TAG_BOOL && get_tag(b) == TAG_BOOL) return make_number((a & 1) == (b & 1) ? 1.0 : 0.0);
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING)
        return make_number(strcmp((char*)as_index(a), (char*)as_index(b)) == 0 ? 1.0 : 0.0);
    return make_number(a == b ? 1.0 : 0.0);
}

uint64_t donky_add(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return make_number(as_number(a) + as_number(b));
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING) {
        char* s1 = (char*)as_index(a); char* s2 = (char*)as_index(b);
        size_t len1 = str_len(s1), len2 = str_len(s2);
        char* out = gc_malloc(len1 + len2 + 1);
        memcpy(out, s1, len1);
        memcpy(out + len1, s2, len2 + 1);
        ((GCHeader*)out - 1)->type_tag = TAG_STRING;
        return TAG_BASE | (TAG_STRING << 48) | (uint64_t)out;
    }
    return 0;
}

uint64_t donky_sub(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return make_number(as_number(a) - as_number(b));
    return make_number(0.0);
}

uint64_t donky_mul(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return make_number(as_number(a) * as_number(b));
    return make_number(0.0);
}

uint64_t donky_div(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) {
        double db = as_number(b);
        if (db == 0.0) return make_number(0.0);
        return make_number(as_number(a) / db);
    }
    return make_number(0.0);
}

uint64_t donky_mod(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) {
        double db = as_number(b);
        if (db == 0.0) return make_number(0.0);
        return make_number(fmod(as_number(a), db));
    }
    return make_number(0.0);
}

uint64_t donky_byte_at(uint64_t s_val, uint64_t i_val) {
    if (get_tag(s_val) == TAG_STRING && is_number(i_val)) {
        char* s = (char*)as_index(s_val);
        size_t i = (size_t)as_number(i_val);
        if (i < str_len(s)) return make_number((double)(unsigned char)s[i]);
    }
    return make_number(-1.0);
}

uint64_t donky_substr(uint64_t s_val, uint64_t start_val, uint64_t end_val) {
    if (get_tag(s_val) != TAG_STRING) return 0;
    char* s = (char*)as_index(s_val);
    size_t start = (size_t)as_number(start_val); size_t end = (size_t)as_number(end_val);
    size_t len = str_len(s);
    if (start > len) start = len; if (end > len) end = len; if (start > end) start = end;
    size_t sub_len = end - start;
    char* buf = malloc(sub_len + 1);
    memcpy(buf, s + start, sub_len);
    buf[sub_len] = 0;
    uint64_t res = donky_alloc_string(buf);
    free(buf);
    return res;
}

uint64_t donky_byte_substr(uint64_t s_val, uint64_t start_val, uint64_t end_val) {
    return donky_substr(s_val, start_val, end_val);
}

uint64_t donky_len(uint64_t val) {
    if (get_tag(val) == TAG_ARRAY) return make_number((double)((Array*)as_index(val))->len);
    if (get_tag(val) == TAG_STRING) return make_number((double)str_len((char*)as_index(val)));
    if (get_tag(val) == TAG_MAP) return make_number((double)((Map*)as_index(val))->len);
    return make_number(0.0);
}

uint64_t donky_push(uint64_t arr, uint64_t val) { return donky_array_push(arr, val); }

uint64_t donky_pop(uint64_t arr_val) {
    if (get_tag(arr_val) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(arr_val);
        if (arr->len > 0) return arr->data[--arr->len];
    }
    return 0;
}

uint64_t donky_str(uint64_t val) {
    char buf[256];
    if (is_number(val)) {
        double d = as_number(val);
        if (d == (long long)d) sprintf(buf, "%lld", (long long)d); else sprintf(buf, "%g", d);
    } else if (get_tag(val) == TAG_STRING) return val;
    else if (get_tag(val) == TAG_BOOL) strcpy(buf, (val & 1) ? "true" : "false");
    else if (get_tag(val) == TAG_ARRAY) strcpy(buf, "[array]");
    else if (get_tag(val) == TAG_MAP) strcpy(buf, "{map}");
    else strcpy(buf, "null");
    return donky_alloc_string(buf);
}

uint64_t donky_type(uint64_t val) {
    if (is_number(val)) return donky_alloc_string("number");
    if (get_tag(val) == TAG_BOOL) return donky_alloc_string("bool");
    if (get_tag(val) == TAG_STRING) return donky_alloc_string("string");
    if (get_tag(val) == TAG_ARRAY) return donky_alloc_string("array");
    if (get_tag(val) == TAG_MAP) return donky_alloc_string("map");
    return donky_alloc_string("null");
}

uint64_t donky_num(uint64_t val) {
    if (is_number(val)) return val;
    if (get_tag(val) == TAG_STRING) return make_number(atof((char*)as_index(val)));
    return make_number(0.0);
}

uint64_t donky_read_file(uint64_t path_val) {
    if (get_tag(path_val) != TAG_STRING) return 0;
    char* path = (char*)as_index(path_val);
    FILE* f = fopen(path, "rb");
    if (!f) {
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf), "File not found: %s", path);
        donky_throw(donky_alloc_string(err_buf));
        return 0;
    }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = malloc(len + 1); fread(buf, 1, len, f); buf[len] = 0; fclose(f);
    uint64_t res = donky_alloc_string(buf); free(buf); return res;
}

uint64_t donky_write_file(uint64_t path_val, uint64_t content_val) {
    if (get_tag(path_val) != TAG_STRING || get_tag(content_val) != TAG_STRING) return 0;
    FILE* f = fopen((char*)as_index(path_val), "wb");
    if (!f) return 0;
    char* content = (char*)as_index(content_val);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return make_number(1.0);
}

uint64_t donky_file_exists(uint64_t path_val) {
    if (get_tag(path_val) != TAG_STRING) return make_number(0.0);
    FILE* f = fopen((char*)as_index(path_val), "r");
    if (f) { fclose(f); return make_number(1.0); }
    return make_number(0.0);
}

uint64_t donky_upper(uint64_t s_val) {
    if (get_tag(s_val) != TAG_STRING) return 0;
    char* s = strdup((char*)as_index(s_val));
    for (int i = 0; s[i]; i++) s[i] = toupper(s[i]);
    uint64_t res = donky_alloc_string(s); free(s); return res;
}

uint64_t donky_lower(uint64_t s_val) {
    if (get_tag(s_val) != TAG_STRING) return 0;
    char* s = strdup((char*)as_index(s_val));
    for (int i = 0; s[i]; i++) s[i] = tolower(s[i]);
    uint64_t res = donky_alloc_string(s); free(s); return res;
}

uint64_t donky_split(uint64_t s_val, uint64_t delim_val) {
    if (get_tag(s_val) != TAG_STRING || get_tag(delim_val) != TAG_STRING) return 0;
    char* s = (char*)as_index(s_val);
    char* delim = (char*)as_index(delim_val);
    uint64_t arr_val = donky_alloc_array();
    char* copy = strdup(s);
    char* token = strtok(copy, delim);
    while (token != NULL) {
        donky_array_push(arr_val, donky_alloc_string(token));
        token = strtok(NULL, delim);
    }
    free(copy);
    return arr_val;
}

uint64_t donky_join(uint64_t arr_val, uint64_t delim_val) {
    if (get_tag(arr_val) != TAG_ARRAY || get_tag(delim_val) != TAG_STRING) return 0;
    Array* arr = (Array*)as_index(arr_val);
    char* delim = (char*)as_index(delim_val);
    size_t total_len = 0;
    for (size_t i = 0; i < arr->len; i++) {
        if (get_tag(arr->data[i]) == TAG_STRING) total_len += strlen((char*)as_index(arr->data[i]));
        if (i < arr->len - 1) total_len += strlen(delim);
    }
    char* buf = malloc(total_len + 1); buf[0] = 0;
    for (size_t i = 0; i < arr->len; i++) {
        if (get_tag(arr->data[i]) == TAG_STRING) strcat(buf, (char*)as_index(arr->data[i]));
        if (i < arr->len - 1) strcat(buf, delim);
    }
    uint64_t res = donky_alloc_string(buf); free(buf); return res;
}

uint64_t donky_contains(uint64_t haystack, uint64_t needle) {
    uint64_t h_tag = get_tag(haystack);
    uint64_t n_tag = get_tag(needle);

    if (h_tag == TAG_STRING && n_tag == TAG_STRING) {
        return make_number(strstr((char*)as_index(haystack), (char*)as_index(needle)) != NULL ? 1.0 : 0.0);
    }
    if (h_tag == TAG_ARRAY) {
        Array* arr = (Array*)as_index(haystack);
        for (size_t i = 0; i < arr->len; i++) {
            if (as_number(donky_eq(arr->data[i], needle)) == 1.0) return make_number(1.0);
        }
        return make_number(0.0);
    }
    if (h_tag == TAG_MAP) {
        Map* map = (Map*)as_index(haystack);
        for (size_t i = 0; i < map->len; i++) {
            if (as_number(donky_eq(map->keys[i], needle)) == 1.0) return make_number(1.0);
        }
        return make_number(0.0);
    }
    return make_number(0.0);
}

uint64_t donky_now() { return make_number((double)time(NULL)); }

uint64_t donky_now_ms() {
    return make_number((double)GetTickCount64());
}

uint64_t donky_random(uint64_t max_val) {
    if (!rand_seeded) { srand((unsigned)time(NULL)); rand_seeded = 1; }
    int max = (int)as_number(max_val);
    if (max <= 0) return make_number(0.0);
    return make_number((double)(rand() % max));
}

uint64_t donky_keys(uint64_t map_val) {
    if (get_tag(map_val) != TAG_MAP) return donky_alloc_array();
    Map* map = (Map*)as_index(map_val);
    uint64_t arr = donky_alloc_array();
    for (size_t i = 0; i < map->len; i++) donky_array_push(arr, map->keys[i]);
    return arr;
}

uint64_t donky_floor(uint64_t val) {
    if (is_number(val)) return make_number(floor(as_number(val)));
    return make_number(0.0);
}

uint64_t donky_bytes_to_str(uint64_t arr_val) {
    if (get_tag(arr_val) != TAG_ARRAY) return 0;
    Array* arr = (Array*)as_index(arr_val);
    char* buf = malloc(arr->len + 1);
    for (size_t i = 0; i < arr->len; i++) {
        buf[i] = is_number(arr->data[i]) ? (char)((int)as_number(arr->data[i])) : '?';
    }
    buf[arr->len] = 0;
    uint64_t res = donky_alloc_string(buf);
    free(buf);
    return res;
}

uint64_t donky_char_code(uint64_t s_val) {
    if (get_tag(s_val) != TAG_STRING) return 0;
    char* s = (char*)as_index(s_val);
    if (strlen(s) > 0) return make_number((double)(unsigned char)s[0]);
    return 0;
}

// --- Console Native Functions ---
uint64_t donky_cls() { system("cls"); return 0; }
uint64_t donky_getch() { return make_number((double)_getch()); }
uint64_t donky_kbhit() { return make_number(_kbhit() ? 1.0 : 0.0); }

uint64_t donky_input(uint64_t prompt_val) {
    if (get_tag(prompt_val) == TAG_STRING) {
        printf("%s", (char*)as_index(prompt_val));
        fflush(stdout);
    }
    char buf[256];
    if (fgets(buf, 256, stdin) == NULL) return donky_alloc_string("");
    buf[strcspn(buf, "\r\n")] = 0;
    return donky_alloc_string(buf);
}

uint64_t donky_sleep(uint64_t ms_val) {
    Sleep((DWORD)as_number(ms_val));
    return 0;
}

// ===================== Native JSON (decode / encode / pretty / error) =====================
#ifdef _MSC_VER
#define DONKY_TLS __declspec(thread)
#else
#define DONKY_TLS __thread
#endif

// Recursion depth cap. ~2048 levels fits comfortably in the default 1 MB Windows stack.
// To go deeper, add "-Wl,--stack,16777216" to the gcc link args in main.rs and raise this.
#define JSON_MAX_DEPTH 2048
#define JV_IS(v, t) (!is_number(v) && get_tag(v) == (t))

static DONKY_TLS char json_err_buf[192];

static uint64_t jp_mkstr(const char* s, size_t n) {
    char* d = (char*)gc_malloc(n + 1);   // GCHeader.size must be exactly len+1 (str_len relies on it)
    memcpy(d, s, n);
    d[n] = 0;
    ((GCHeader*)d - 1)->type_tag = TAG_STRING;
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)d;
}

#define JP_KEY_CACHE 256      // direct-mapped; power of two
#define JP_KEY_MAXLEN 32      // only short keys are interned

typedef struct {
    const char* p; const char* end;
    char* scratch; size_t scap;
    const char* err; const char* err_at;
    uint64_t kc[JP_KEY_CACHE];   // interned object-key strings (zero-initialised = empty)
} JsonParser;

static int jp_fail(JsonParser* jp, const char* msg) {
    if (!jp->err) { jp->err = msg; jp->err_at = jp->p; }
    return 0;
}

static void jp_ws(JsonParser* jp) {
    while (jp->p < jp->end) {
        char c = *jp->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') jp->p++; else break;
    }
}

static int jp_reserve(JsonParser* jp, size_t need) {
    if (need <= jp->scap) return 1;
    size_t ncap = jp->scap ? jp->scap : 256;
    while (ncap < need) ncap *= 2;
    char* n = (char*)realloc(jp->scratch, ncap);
    if (!n) return 0;
    jp->scratch = n; jp->scap = ncap;
    return 1;
}

static int jp_hex4(const char* p, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static size_t jp_utf8(char* o, unsigned cp) {
    if (cp < 0x80)         { o[0] = (char)cp; return 1; }
    if (cp < 0x800)        { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000)      { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63));
    o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

// is_key != 0: short escape-free strings are interned in jp->kc so repeated object
// keys share one GC string (and hit the pointer-equality fast path in map_key_eq).
// Interned strings are never mutated, and no GC runs during a decode.
static int jp_string(JsonParser* jp, uint64_t* out, int is_key) {
    const char* start = jp->p + 1;
    const char* p = start;
    while (p < jp->end) {                       // fast path: scan for quote/backslash
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') break;
        if (c < 0x20) { jp->p = p; return jp_fail(jp, "control character in string"); }
        p++;
    }
    if (p >= jp->end) { jp->p = p; return jp_fail(jp, "unterminated string"); }
    if (*p == '"') {
        size_t n = (size_t)(p - start);
        if (is_key && n <= JP_KEY_MAXLEN) {
            size_t slot = (size_t)(hash_bytes((const unsigned char*)start, n) & (JP_KEY_CACHE - 1));
            uint64_t c = jp->kc[slot];
            if (c) {
                const char* cs = (const char*)as_index(c);
                if (str_len(cs) == n && memcmp(cs, start, n) == 0) { *out = c; jp->p = p + 1; return 1; }
            }
            uint64_t s = jp_mkstr(start, n);
            jp->kc[slot] = s;
            *out = s; jp->p = p + 1; return 1;
        }
        *out = jp_mkstr(start, n); jp->p = p + 1; return 1;
    }

    size_t used = (size_t)(p - start);          // slow path: has escapes
    if (!jp_reserve(jp, used + 8)) { jp->p = p; return jp_fail(jp, "out of memory"); }
    memcpy(jp->scratch, start, used);
    while (p < jp->end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') { *out = jp_mkstr(jp->scratch, used); jp->p = p + 1; return 1; }
        if (c < 0x20) { jp->p = p; return jp_fail(jp, "control character in string"); }
        if (!jp_reserve(jp, used + 8)) { jp->p = p; return jp_fail(jp, "out of memory"); }
        if (c != '\\') { jp->scratch[used++] = (char)c; p++; continue; }
        p++;
        if (p >= jp->end) break;
        switch (*p) {
            case '"':  jp->scratch[used++] = '"';  break;
            case '\\': jp->scratch[used++] = '\\'; break;
            case '/':  jp->scratch[used++] = '/';  break;
            case 'b':  jp->scratch[used++] = '\b'; break;
            case 'f':  jp->scratch[used++] = '\f'; break;
            case 'n':  jp->scratch[used++] = '\n'; break;
            case 'r':  jp->scratch[used++] = '\r'; break;
            case 't':  jp->scratch[used++] = '\t'; break;
            case 'u': {
                unsigned cp, lo;
                if (!jp_hex4(p + 1, &cp)) { jp->p = p; return jp_fail(jp, "bad \\u escape"); }
                p += 4;                          // p now on last hex digit
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (p[1] == '\\' && p[2] == 'u' && jp_hex4(p + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    } else cp = 0xFFFD;          // lone high surrogate
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;  // lone low surrogate
                if (cp == 0) { jp->p = p; return jp_fail(jp, "\\u0000 is not supported in strings"); }
                used += jp_utf8(jp->scratch + used, cp);
                break;
            }
            default: jp->p = p; return jp_fail(jp, "bad escape");
        }
        p++;
    }
    jp->p = p;
    return jp_fail(jp, "unterminated string");
}

// Exact powers of ten representable as doubles (1e0..1e22).
static const double JP_P10[23] = {
    1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,
    1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22
};

// Clinger fast path: when the decimal mantissa is < 2^53 and the power-of-ten exponent
// is within +-22, (double)m * or / 10^|e| is a single correctly-rounded IEEE operation,
// so the result is bit-identical to strtod. Anything else falls back to strtod.
static int jp_number(JsonParser* jp, uint64_t* out) {
    const char* st = jp->p; const char* p = st;
    int neg = 0, nd = 0, e10 = 0, fast = 1;
    uint64_t m = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p == '0') p++;
    else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') {
            if (nd < 19) { m = m * 10 + (uint64_t)(*p - '0'); nd++; }
            else { fast = 0; e10++; }
            p++;
        }
    } else { jp->p = p; return jp_fail(jp, "invalid value"); }
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9')) { jp->p = p; return jp_fail(jp, "bad number"); }
        while (*p >= '0' && *p <= '9') {
            if (nd < 19) { m = m * 10 + (uint64_t)(*p - '0'); nd++; e10--; }
            else fast = 0;
            p++;
        }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        int es = 1, ev = 0;
        if (*p == '+') p++;
        else if (*p == '-') { es = -1; p++; }
        if (!(*p >= '0' && *p <= '9')) { jp->p = p; return jp_fail(jp, "bad number"); }
        while (*p >= '0' && *p <= '9') { if (ev < 10000) ev = ev * 10 + (*p - '0'); p++; }
        e10 += es * ev;
    }
    double d;
    if (fast && m < (1ULL << 53) && e10 >= -22 && e10 <= 22) {
        d = e10 < 0 ? (double)m / JP_P10[-e10] : (double)m * JP_P10[e10];
        if (neg) d = -d;
    } else {
        d = strtod(st, NULL);        // st includes the '-', so the sign is already applied
    }
    jp->p = p;
    *out = make_number(d);
    return 1;
}

static int jp_value(JsonParser* jp, uint64_t* out, int depth) {
    if (depth > JSON_MAX_DEPTH) return jp_fail(jp, "nesting too deep");
    jp_ws(jp);
    if (jp->p >= jp->end) return jp_fail(jp, "unexpected end of input");
    char c = *jp->p;
    size_t left = (size_t)(jp->end - jp->p);
    switch (c) {
    case '"': return jp_string(jp, out, 0);
    case '{': {
        jp->p++;
        uint64_t m = donky_alloc_map();
        jp_ws(jp);
        if (jp->p < jp->end && *jp->p == '}') { jp->p++; *out = m; return 1; }
        for (;;) {
            jp_ws(jp);
            if (jp->p >= jp->end || *jp->p != '"') return jp_fail(jp, "expected string key");
            uint64_t k, v;
            if (!jp_string(jp, &k, 1)) return 0;
            jp_ws(jp);
            if (jp->p >= jp->end || *jp->p != ':') return jp_fail(jp, "expected ':'");
            jp->p++;
            if (!jp_value(jp, &v, depth + 1)) return 0;
            donky_map_set(m, k, v);
            jp_ws(jp);
            if (jp->p >= jp->end) return jp_fail(jp, "unterminated object");
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == '}') { jp->p++; *out = m; return 1; }
            return jp_fail(jp, "expected ',' or '}'");
        }
    }
    case '[': {
        jp->p++;
        uint64_t a = donky_alloc_array();
        jp_ws(jp);
        if (jp->p < jp->end && *jp->p == ']') { jp->p++; *out = a; return 1; }
        for (;;) {
            uint64_t v;
            if (!jp_value(jp, &v, depth + 1)) return 0;
            donky_array_push(a, v);
            jp_ws(jp);
            if (jp->p >= jp->end) return jp_fail(jp, "unterminated array");
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == ']') { jp->p++; *out = a; return 1; }
            return jp_fail(jp, "expected ',' or ']'");
        }
    }
    case 't':
        if (left >= 4 && memcmp(jp->p, "true", 4) == 0) { jp->p += 4; *out = TAG_BASE | (TAG_BOOL << 48) | 1; return 1; }
        return jp_fail(jp, "invalid literal");
    case 'f':
        if (left >= 5 && memcmp(jp->p, "false", 5) == 0) { jp->p += 5; *out = TAG_BASE | (TAG_BOOL << 48); return 1; }
        return jp_fail(jp, "invalid literal");
    case 'n':
        if (left >= 4 && memcmp(jp->p, "null", 4) == 0) { jp->p += 4; *out = 0; return 1; }
        return jp_fail(jp, "invalid literal");
    default:
        return jp_number(jp, out);
    }
}

// json_decode(str) -> value, or 0 on error (check json_error() to tell error from a real null/0)
uint64_t donky_json_decode(uint64_t s_val) {
    json_err_buf[0] = 0;
    if (!JV_IS(s_val, TAG_STRING)) { snprintf(json_err_buf, sizeof json_err_buf, "json: expected a string"); return 0; }
    const char* s = (const char*)as_index(s_val);
    JsonParser jp = { s, s + str_len(s), NULL, 0, NULL, NULL };   // kc[] zero-initialised
    if (jp.end - jp.p >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) jp.p += 3;  // UTF-8 BOM
    uint64_t result = 0;
    int ok = jp_value(&jp, &result, 0);
    if (ok) {
        jp_ws(&jp);
        if (jp.p < jp.end) ok = jp_fail(&jp, "trailing characters after value");
    }
    if (!ok) {
        snprintf(json_err_buf, sizeof json_err_buf, "json: %s at offset %lld", jp.err ? jp.err : "parse error", (long long)(jp.err_at - s));
        result = 0;
    }
    free(jp.scratch);
    return result;
}

uint64_t donky_json_error(void) { return donky_alloc_string(json_err_buf); }

// ---------- encoder ----------
typedef struct { char* b; size_t len, cap; int oom; } JsonBuf;

static int jb_reserve(JsonBuf* j, size_t extra) {
    if (j->oom) return 0;
    if (j->len + extra + 1 <= j->cap) return 1;
    size_t ncap = j->cap ? j->cap : 256;
    while (ncap < j->len + extra + 1) ncap *= 2;
    char* n = (char*)realloc(j->b, ncap);
    if (!n) { j->oom = 1; return 0; }
    j->b = n; j->cap = ncap;
    return 1;
}
static void jb_put(JsonBuf* j, const char* s, size_t n) {
    if (n == 0 || !jb_reserve(j, n)) return;
    memcpy(j->b + j->len, s, n); j->len += n;
}
static void jb_ch(JsonBuf* j, char c) { jb_put(j, &c, 1); }
static void jb_nl(JsonBuf* j, int step, int level) {
    if (step <= 0) return;
    size_t n = (size_t)step * (size_t)level;
    if (!jb_reserve(j, n + 1)) return;
    j->b[j->len++] = '\n';
    memset(j->b + j->len, ' ', n); j->len += n;
}

static void jb_str(JsonBuf* j, const char* s, size_t n) {
    jb_ch(j, '"');
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 0x20 && c != '"' && c != '\\') continue;
        jb_put(j, s + run, i - run); run = i + 1;
        switch (c) {
            case '"':  jb_put(j, "\\\"", 2); break;
            case '\\': jb_put(j, "\\\\", 2); break;
            case '\n': jb_put(j, "\\n", 2);  break;
            case '\r': jb_put(j, "\\r", 2);  break;
            case '\t': jb_put(j, "\\t", 2);  break;
            case '\b': jb_put(j, "\\b", 2);  break;
            case '\f': jb_put(j, "\\f", 2);  break;
            default: { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); jb_put(j, u, 6); }
        }
    }
    jb_put(j, s + run, n - run);
    jb_ch(j, '"');
}

static size_t json_fmt_number(double d, char* buf, size_t cap) {
    if (d != d || d - d != 0) return (size_t)snprintf(buf, cap, "null");   // NaN / Inf
    if ((double)(long long)d == d && d < 1e15 && d > -1e15) return (size_t)snprintf(buf, cap, "%lld", (long long)d);
    int n = snprintf(buf, cap, "%.15g", d);
    if (strtod(buf, NULL) != d) n = snprintf(buf, cap, "%.17g", d);        // shortest round-trip
    return (size_t)n;
}

static int je_value(JsonBuf* j, uint64_t v, int step, int level) {
    if (level > JSON_MAX_DEPTH) return 0;              // deep nesting or cycle
    if (is_number(v)) {
        char buf[48]; size_t n = json_fmt_number(as_number(v), buf, sizeof buf);
        jb_put(j, buf, n); return 1;
    }
    uint64_t tag = get_tag(v);
    if (tag == TAG_BOOL) { if (v & 1) jb_put(j, "true", 4); else jb_put(j, "false", 5); return 1; }
    if (tag == TAG_STRING) { const char* s = (const char*)as_index(v); jb_str(j, s, str_len(s)); return 1; }
    if (tag == TAG_ARRAY) {
        Array* a = (Array*)as_index(v);
        if (a->len == 0) { jb_put(j, "[]", 2); return 1; }
        jb_ch(j, '[');
        for (size_t i = 0; i < a->len; i++) {
            if (i) jb_ch(j, ',');
            jb_nl(j, step, level + 1);
            if (!je_value(j, a->data[i], step, level + 1)) return 0;
        }
        jb_nl(j, step, level);
        jb_ch(j, ']');
        return 1;
    }
    if (tag == TAG_MAP) {
        Map* m = (Map*)as_index(v);
        if (m->len == 0) { jb_put(j, "{}", 2); return 1; }
        jb_ch(j, '{');
        for (size_t i = 0; i < m->len; i++) {
            if (i) jb_ch(j, ',');
            jb_nl(j, step, level + 1);
            uint64_t k = m->keys[i];
            if (JV_IS(k, TAG_STRING)) { const char* ks = (const char*)as_index(k); jb_str(j, ks, str_len(ks)); }
            else if (is_number(k)) { char buf[48]; size_t n = json_fmt_number(as_number(k), buf, sizeof buf); jb_str(j, buf, n); }
            else jb_str(j, "null", 4);
            jb_ch(j, ':');
            if (step > 0) jb_ch(j, ' ');
            if (!je_value(j, m->vals[i], step, level + 1)) return 0;
        }
        jb_nl(j, step, level);
        jb_ch(j, '}');
        return 1;
    }
    jb_put(j, "null", 4);
    return 1;
}

static uint64_t json_encode_impl(uint64_t v, int step) {
    json_err_buf[0] = 0;
    JsonBuf j = { NULL, 0, 0, 0 };
    int ok = je_value(&j, v, step, 0);
    if (!ok || j.oom) {
        snprintf(json_err_buf, sizeof json_err_buf, ok ? "json: out of memory" : "json: nesting too deep (or cyclic structure)");
        free(j.b); return 0;
    }
    uint64_t r = jp_mkstr(j.b ? j.b : "", j.len);
    free(j.b);
    return r;
}

uint64_t donky_json_encode(uint64_t v) { return json_encode_impl(v, 0); }
uint64_t donky_json_pretty(uint64_t v) { return json_encode_impl(v, 2); }

// --- Native FFI ---
// Pointers are passed as numbers (user-space pointers fit in 48 bits, so they are exact as doubles).
// Args: number -> int64, string -> char*, bool -> 0/1. Up to 8 args (Win64: extra args are harmless).
typedef uint64_t (*DonkyFfiFn)(uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);

static uint64_t ffi_convert_arg(uint64_t v) {
    if (is_number(v)) return (uint64_t)(int64_t)as_number(v);
    if (get_tag(v) == TAG_BOOL) return v & 1;
    if (get_tag(v) == TAG_STRING) return as_index(v);
    return 0;
}

static uint64_t ffi_invoke(uint64_t fn_val, uint64_t args_val) {
    if (!is_number(fn_val)) return 0;
    uint64_t fn = (uint64_t)as_number(fn_val);
    if (!fn) return 0;
    uint64_t a[8] = {0,0,0,0,0,0,0,0};
    if (get_tag(args_val) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(args_val);
        for (size_t i = 0; i < arr->len && i < 8; i++) a[i] = ffi_convert_arg(arr->data[i]);
    }
    return ((DonkyFfiFn)fn)(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
}

uint64_t donky_dll_load(uint64_t name_val) {
    if (get_tag(name_val) != TAG_STRING) return make_number(0.0);
    HMODULE h = LoadLibraryA((char*)as_index(name_val));
    return make_number((double)(uint64_t)h);
}

uint64_t donky_dll_sym(uint64_t lib_val, uint64_t name_val) {
    if (!is_number(lib_val) || get_tag(name_val) != TAG_STRING) return make_number(0.0);
    HMODULE h = (HMODULE)(uint64_t)as_number(lib_val);
    if (!h) return make_number(0.0);
    FARPROC p = GetProcAddress(h, (char*)as_index(name_val));
    return make_number((double)(uint64_t)p);
}

// Returns full 64-bit rax as a number (use for pointers/handles)
uint64_t donky_dll_call(uint64_t fn_val, uint64_t args_val) {
    return make_number((double)(int64_t)ffi_invoke(fn_val, args_val));
}

// Returns eax sign-extended (use for functions returning int/BOOL/DWORD)
uint64_t donky_dll_calli(uint64_t fn_val, uint64_t args_val) {
    return make_number((double)(int32_t)ffi_invoke(fn_val, args_val));
}

// Read a NUL-terminated char* (as returned by dll_call) into a Donky string
uint64_t donky_ffi_cstr(uint64_t ptr_val) {
    if (!is_number(ptr_val)) return donky_alloc_string("");
    const char* p = (const char*)(uint64_t)as_number(ptr_val);
    return donky_alloc_string(p ? p : "");
}

// --- System Telemetry Metrics ---
uint64_t donky_get_total_mem() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return make_number((double)(memInfo.ullTotalPhys / 1024 / 1024));
}

uint64_t donky_get_used_mem() {
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    return make_number((double)((memInfo.ullTotalPhys - memInfo.ullAvailPhys) / 1024 / 1024));
}

uint64_t donky_get_processes() {
    uint64_t arr = donky_alloc_array();
    HANDLE hProcessSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hProcessSnap == INVALID_HANDLE_VALUE) return arr;
    
    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);
    
    if (Process32First(hProcessSnap, &pe32)) {
        do {
            uint64_t m = donky_alloc_map();
            donky_map_set(m, donky_alloc_string("pid"), make_number((double)pe32.th32ProcessID));
            donky_map_set(m, donky_alloc_string("name"), donky_alloc_string(pe32.szExeFile));
            donky_map_set(m, donky_alloc_string("threads"), make_number((double)pe32.cntThreads));
            
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
            if (hProcess) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                    donky_map_set(m, donky_alloc_string("mem"), make_number((double)(pmc.WorkingSetSize / 1024 / 1024)));
                } else {
                    donky_map_set(m, donky_alloc_string("mem"), make_number(0.0));
                }
                CloseHandle(hProcess);
            } else {
                donky_map_set(m, donky_alloc_string("mem"), make_number(0.0));
            }
            donky_array_push(arr, m);
        } while (Process32Next(hProcessSnap, &pe32));
    }
    CloseHandle(hProcessSnap);
    return arr;
}

uint64_t donky_get_cpu_usage() {
    static FILETIME prev_idle = {0}, prev_kernel = {0}, prev_user = {0};
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return make_number(0.0);
    
    if (prev_kernel.dwLowDateTime == 0) {
        prev_idle = idle; prev_kernel = kernel; prev_user = user;
        return make_number(0.0);
    }
    
    ULARGE_INTEGER i, k, u, pi, pk, pu;
    i.LowPart = idle.dwLowDateTime; i.HighPart = idle.dwHighDateTime;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    
    pi.LowPart = prev_idle.dwLowDateTime; pi.HighPart = prev_idle.dwHighDateTime;
    pk.LowPart = prev_kernel.dwLowDateTime; pk.HighPart = prev_kernel.dwHighDateTime;
    pu.LowPart = prev_user.dwLowDateTime; pu.HighPart = prev_user.dwHighDateTime;
    
    ULONGLONG idle_diff = i.QuadPart - pi.QuadPart;
    ULONGLONG total_diff = (k.QuadPart - pk.QuadPart) + (u.QuadPart - pu.QuadPart);
    
    prev_idle = idle; prev_kernel = kernel; prev_user = user;
    if (total_diff == 0) return make_number(0.0);
    double usage = 100.0 - ((double)idle_diff / (double)total_diff * 100.0);
    return make_number(usage < 0.0 ? 0.0 : (usage > 100.0 ? 100.0 : usage));
}

// --- Dynamic Socket API ---
#define DK_MAX_SOCK 128
static SOCKET g_socks[DK_MAX_SOCK];
static int g_socks_init = 0;

static int dk_id_from_socket(SOCKET s) {
    gc_init_lock();
    EnterCriticalSection(&gc_lock);
    for (int i = 0; i < DK_MAX_SOCK; i++) {
        if (g_socks[i] == 0) { 
            g_socks[i] = s; 
            LeaveCriticalSection(&gc_lock);
            return i + 1; 
        }
    }
    LeaveCriticalSection(&gc_lock);
    return 0;
}

static SOCKET dk_socket_from_id(uint64_t v) {
    int id = (int)as_number(v);
    if (id <= 0 || id > DK_MAX_SOCK) return INVALID_SOCKET;
    return g_socks[id - 1];
}

static void dk_ws_init(void) {
    if (g_socks_init) return;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    for (int i = 0; i < DK_MAX_SOCK; i++) g_socks[i] = 0;
    g_socks_init = 1;
}

uint64_t donky_tcp_listen(uint64_t port_val) {
    dk_ws_init();
    int port = (int)as_number(port_val);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return make_number(-1.0);

    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { closesocket(s); return make_number(-1.0); }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) { closesocket(s); return make_number(-1.0); }
    return make_number((double)dk_id_from_socket(s));
}

uint64_t donky_tcp_accept(uint64_t server_val) {
    dk_ws_init();
    SOCKET srv = dk_socket_from_id(server_val);
    if (srv == INVALID_SOCKET) return make_number(-1.0);
    SOCKET c = accept(srv, NULL, NULL);
    if (c == INVALID_SOCKET) return make_number(-1.0);
    return make_number((double)dk_id_from_socket(c));
}

uint64_t donky_tcp_read(uint64_t client_val, uint64_t max_val) {
    SOCKET c = dk_socket_from_id(client_val);
    if (c == INVALID_SOCKET) return donky_alloc_string("");
    int max = (int)as_number(max_val);
    if (max <= 0 || max > 1048576) max = 65536;
    char* buf = malloc(max + 1);
    int n = recv(c, buf, max, 0);
    if (n <= 0) { free(buf); return donky_alloc_string(""); }
    buf[n] = 0;
    uint64_t res = donky_alloc_string(buf);
    free(buf);
    return res;
}

uint64_t donky_tcp_write(uint64_t client_val, uint64_t data_val) {
    SOCKET c = dk_socket_from_id(client_val);
    if (c == INVALID_SOCKET || get_tag(data_val) != TAG_STRING) return make_number(-1.0);
    char* data = (char*)as_index(data_val);
    int total = 0, len = (int)strlen(data);
    while (total < len) {
        int n = send(c, data + total, len - total, 0);
        if (n <= 0) return make_number(-1.0);
        total += n;
    }
    return make_number((double)total);
}

uint64_t donky_tcp_close(uint64_t client_val) {
    int id = (int)as_number(client_val);
    if (id > 0 && id <= DK_MAX_SOCK) {
        gc_init_lock();
        EnterCriticalSection(&gc_lock);
        if (g_socks[id - 1] != 0) {
            closesocket(g_socks[id - 1]);
            g_socks[id - 1] = 0;
        }
        LeaveCriticalSection(&gc_lock);
    }
    return 0;
}

uint64_t donky_http_get(uint64_t url_val) {
    if (get_tag(url_val) != TAG_STRING) return donky_alloc_string("");
    char* url = (char*)as_index(url_val);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    wchar_t* wurl = malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wlen);

    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256], urlPath[2048];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl, 0, 0, &urlComp)) { free(wurl); return donky_alloc_string(""); }

    HINTERNET hSession = WinHttpOpen(L"Donky/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { free(wurl); return donky_alloc_string(""); }

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); free(wurl); return donky_alloc_string(""); }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); free(wurl); return donky_alloc_string(""); }

    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) bResults = WinHttpReceiveResponse(hRequest, NULL);

    char* response_body = NULL;
    size_t total_size = 0;
    DWORD dwSize = 0, dwDownloaded = 0;

    if (bResults) {
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
            char* buffer = malloc(dwSize + 1);
            ZeroMemory(buffer, dwSize + 1);
            if (WinHttpReadData(hRequest, (LPVOID)buffer, dwSize, &dwDownloaded)) {
                response_body = realloc(response_body, total_size + dwDownloaded + 1);
                memcpy(response_body + total_size, buffer, dwDownloaded);
                total_size += dwDownloaded;
                response_body[total_size] = 0;
            }
            free(buffer);
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); free(wurl);
    uint64_t result = donky_alloc_string(response_body ? response_body : "");
    if (response_body) free(response_body);
    return result;
}

// --- Argument & Thread Runtime ---
uint64_t donky_args_array = 0;
void donky_set_args(int argc, char** argv) {
    uint64_t arr_val = donky_alloc_array();
    for (int i = 1; i < argc; i++) donky_array_push(arr_val, donky_alloc_string(argv[i]));
    donky_args_array = arr_val;
}
uint64_t donky_get_args() { return donky_args_array; }

typedef struct {
    uint64_t func_ptr;
    uint64_t arg1;
    int slot;
} ThreadArgs;

unsigned __stdcall donky_thread_runner(void* raw_args) {
    ThreadArgs* targs = (ThreadArgs*)raw_args;
    uintptr_t fp = (uintptr_t)targs->func_ptr;
    uint64_t a1 = targs->arg1;
    int slot = targs->slot;
    
    typedef uint64_t (*DonkyFn)(uint64_t);
    DonkyFn fn = (DonkyFn)fp;
    fn(a1); 
    
    gc_init_lock();
    EnterCriticalSection(&gc_lock);
    if (active_threads_handles[slot]) {
        CloseHandle(active_threads_handles[slot]);
        active_threads_handles[slot] = NULL;
    }
    active_threads--;
    LeaveCriticalSection(&gc_lock);
    
    free(targs);
    return 0;
}

uint64_t donky_spawn(uint64_t func_val, uint64_t arg1) {
    gc_init_lock();
    EnterCriticalSection(&gc_lock);
    if (active_threads >= MAX_THREADS) {
        LeaveCriticalSection(&gc_lock);
        return make_number(-1.0);
    }
    
    int slot = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (active_threads_handles[i] == NULL) { slot = i; break; }
    }
    if (slot == -1) {
        LeaveCriticalSection(&gc_lock);
        return make_number(-1.0);
    }
    
    ThreadArgs* targs = malloc(sizeof(ThreadArgs));
    targs->func_ptr = func_val;
    targs->arg1 = arg1;
    targs->slot = slot;
    
    active_threads_handles[slot] = (HANDLE)_beginthreadex(NULL, 0, donky_thread_runner, targs, CREATE_SUSPENDED, NULL);
    active_threads++;
    ResumeThread(active_threads_handles[slot]);
    LeaveCriticalSection(&gc_lock);
    return make_number(0.0);
}

// --- Modern GUI Subsystem (GDI Double-Buffered & Control UI) ---
HWND g_hwnd = NULL;
HDC g_mem_dc = NULL;
HBITMAP g_mem_bmp = NULL;
int g_win_w = 0, g_win_h = 0;
int g_is_dragging = 0;
POINT g_drag_start = {0, 0};

#define MAX_SHAPES 2048
typedef struct {
    int type; // 0 = rect, 1 = text
    int x, y, w, h;
    uint32_t color;
    char text[256];
} Shape;

Shape g_shapes[MAX_SHAPES];
int g_shape_count = 0;
int g_clicked_id = -1;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_COMMAND:
            g_clicked_id = LOWORD(wParam);
            return 0;
        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam), my = HIWORD(lParam);
            if (my < 40 && mx < g_win_w - 40) {
                g_is_dragging = 1;
                GetCursorPos(&g_drag_start);
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_is_dragging) {
                POINT pt; GetCursorPos(&pt);
                int dx = pt.x - g_drag_start.x, dy = pt.y - g_drag_start.y;
                RECT wr; GetWindowRect(hwnd, &wr);
                SetWindowPos(hwnd, NULL, wr.left + dx, wr.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW);
                g_drag_start = pt;
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g_is_dragging) { g_is_dragging = 0; ReleaseCapture(); }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (g_mem_dc) BitBlt(hdc, 0, 0, g_win_w, g_win_h, g_mem_dc, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE: {
            g_win_w = LOWORD(lParam); g_win_h = HIWORD(lParam);
            if (g_mem_dc) DeleteDC(g_mem_dc);
            if (g_mem_bmp) DeleteObject(g_mem_bmp);
            HDC hdc = GetDC(hwnd);
            g_mem_dc = CreateCompatibleDC(hdc);
            g_mem_bmp = CreateCompatibleBitmap(hdc, g_win_w, g_win_h);
            SelectObject(g_mem_dc, g_mem_bmp);
            ReleaseDC(hwnd, hdc);
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

uint64_t donky_gui_init(uint64_t title_val, uint64_t width_val, uint64_t height_val) {
    if (get_tag(title_val) != TAG_STRING) return make_number(0.0);
    char* title = (char*)as_index(title_val);
    g_win_w = (int)as_number(width_val);
    g_win_h = (int)as_number(height_val);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DonkyUIClass";
    RegisterClass(&wc);

    g_hwnd = CreateWindowExA(0, "DonkyUIClass", title, WS_POPUP | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, g_win_w, g_win_h, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g_hwnd) return make_number(0.0);

    ShowWindow(g_hwnd, SW_SHOW);
    return make_number(1.0);
}

uint64_t donky_gui_add_button(uint64_t text_val, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_hwnd || get_tag(text_val) != TAG_STRING) return make_number(-1.0);
    static int btn_id = 100;
    CreateWindowA("BUTTON", (char*)as_index(text_val), WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        (int)as_number(x), (int)as_number(y), (int)as_number(w), (int)as_number(h),
        g_hwnd, (HMENU)(UINT_PTR)btn_id, GetModuleHandle(NULL), NULL);
    return make_number((double)(btn_id++));
}

uint64_t donky_gui_add_label(uint64_t text_val, uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (!g_hwnd || get_tag(text_val) != TAG_STRING) return make_number(-1.0);
    static int lbl_id = 500;
    HWND hLbl = CreateWindowA("STATIC", (char*)as_index(text_val), WS_VISIBLE | WS_CHILD,
        (int)as_number(x), (int)as_number(y), (int)as_number(w), (int)as_number(h),
        g_hwnd, (HMENU)(UINT_PTR)lbl_id, GetModuleHandle(NULL), NULL);
    return make_number((double)(intptr_t)hLbl);
}

uint64_t donky_gui_update_text(uint64_t handle_val, uint64_t text_val) {
    if (get_tag(text_val) != TAG_STRING) return 0;
    HWND hCtrl = (HWND)(intptr_t)as_number(handle_val);
    if (hCtrl && IsWindow(hCtrl)) SetWindowTextA(hCtrl, (char*)as_index(text_val));
    return 0;
}

uint64_t donky_gui_get_clicked() {
    int id = g_clicked_id;
    g_clicked_id = -1;
    return make_number((double)id);
}

uint64_t donky_gui_clear() {
    g_shape_count = 0;
    return 0;
}

uint64_t donky_gui_draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t color) {
    if (g_shape_count < MAX_SHAPES) {
        Shape s = {0};
        s.type = 0;
        s.x = (int)as_number(x); s.y = (int)as_number(y);
        s.w = (int)as_number(w); s.h = (int)as_number(h);
        s.color = (uint32_t)as_number(color);
        g_shapes[g_shape_count++] = s;
    }
    return 0;
}

uint64_t donky_gui_draw_text(uint64_t text_val, uint64_t x, uint64_t y) {
    if (get_tag(text_val) != TAG_STRING) return 0;
    if (g_shape_count < MAX_SHAPES) {
        Shape s = {0};
        s.type = 1;
        s.x = (int)as_number(x); s.y = (int)as_number(y);
        s.color = 0xFFFFFF;
        strncpy(s.text, (char*)as_index(text_val), 255);
        g_shapes[g_shape_count++] = s;
    }
    return 0;
}

uint64_t donky_gui_repaint() {
    if (!g_mem_dc) return 0;
    HBRUSH bg = CreateSolidBrush(RGB(10, 12, 18));
    RECT r = {0, 0, g_win_w, g_win_h};
    FillRect(g_mem_dc, &r, bg);
    DeleteObject(bg);

    for (int i = 0; i < g_shape_count; i++) {
        Shape* s = &g_shapes[i];
        if (s->type == 0) {
            HBRUSH brush = CreateSolidBrush(RGB((s->color >> 16) & 0xFF, (s->color >> 8) & 0xFF, s->color & 0xFF));
            RECT rect = {s->x, s->y, s->x + s->w, s->y + s->h};
            FillRect(g_mem_dc, &rect, brush);
            DeleteObject(brush);
        } else if (s->type == 1) {
            SetTextColor(g_mem_dc, RGB(255, 255, 255));
            SetBkMode(g_mem_dc, TRANSPARENT);
            TextOutA(g_mem_dc, s->x, s->y, s->text, (int)strlen(s->text));
        }
    }
    
    HDC hdc = GetDC(g_hwnd);
    BitBlt(hdc, 0, 0, g_win_w, g_win_h, g_mem_dc, 0, 0, SRCCOPY);
    ReleaseDC(g_hwnd, hdc);
    return 0;
}

uint64_t donky_gui_update() {
    if (!g_hwnd) return 0;
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return make_number(0.0);
}

#include <shellapi.h>
// --- Native WebView Window ---
uint64_t donky_gui_webview(uint64_t url_val) {
    if (get_tag(url_val) != TAG_STRING) return make_number(0.0);
    char* url = (char*)as_index(url_val);

    printf("[Donky] Server active. Opening UI at %s\n", url);
    fflush(stdout);

    // Launch default system browser (Edge/Chrome/Firefox)
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);

    // Keep the main process thread alive so spawned server threads stay active
    while (1) {
        Sleep(100);
    }
    return make_number(1.0);
}

// ===================== Standard library builtins =====================
static inline int dk_is_str(uint64_t v) { return !is_number(v) && get_tag(v) == TAG_STRING; }
static inline const char* dk_s(uint64_t v) { return (const char*)as_index(v); }

// ---- globals (shared state for libs) ----
uint64_t donky_global_set(uint64_t k, uint64_t v) {
    if (!g_globals) g_globals = donky_alloc_map();
    donky_map_set(g_globals, k, v);
    return 0;
}
uint64_t donky_global_get(uint64_t k) {
    if (!g_globals) return 0;
    return donky_index_get(g_globals, k);
}

// ---- console / process ----
uint64_t donky_sys_puts(uint64_t s) {
    uint64_t sv = dk_is_str(s) ? s : donky_str(s);
    printf("%s\n", dk_s(sv)); fflush(stdout);
    return 0;
}
uint64_t donky_sys_eputs(uint64_t s) {
    uint64_t sv = dk_is_str(s) ? s : donky_str(s);
    fprintf(stderr, "%s\n", dk_s(sv)); fflush(stderr);
    return 0;
}
uint64_t donky_sys_exit(uint64_t code) {
    fflush(stdout); fflush(stderr);
    exit(is_number(code) ? (int)as_number(code) : 0);
    return 0;
}

// ---- os ----
uint64_t donky_sys_cwd(void) {
    char buf[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryA(sizeof buf, buf);
    if (n == 0 || n >= sizeof buf) return donky_alloc_string("");
    return donky_alloc_string(buf);
}
uint64_t donky_sys_chdir(uint64_t p) {
    if (!dk_is_str(p)) return make_number(0.0);
    return make_number(SetCurrentDirectoryA(dk_s(p)) ? 1.0 : 0.0);
}
// returns null (0) if the variable does not exist
uint64_t donky_sys_getenv(uint64_t name) {
    if (!dk_is_str(name)) return 0;
    SetLastError(0);
    DWORD n = GetEnvironmentVariableA(dk_s(name), NULL, 0);
    if (n == 0) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? 0 : donky_alloc_string("");
    char* buf = (char*)malloc(n + 1);
    DWORD m = GetEnvironmentVariableA(dk_s(name), buf, n + 1);
    buf[m < n + 1 ? m : n] = 0;
    uint64_t r = donky_alloc_string(buf);
    free(buf);
    return r;
}
uint64_t donky_sys_setenv(uint64_t name, uint64_t val) {
    if (!dk_is_str(name) || !dk_is_str(val)) return make_number(0.0);
    return make_number(SetEnvironmentVariableA(dk_s(name), dk_s(val)) ? 1.0 : 0.0);
}
uint64_t donky_sys_remove(uint64_t p) {
    if (!dk_is_str(p)) return make_number(0.0);
    return make_number(DeleteFileA(dk_s(p)) ? 1.0 : 0.0);
}
uint64_t donky_sys_rename(uint64_t a, uint64_t b) {
    if (!dk_is_str(a) || !dk_is_str(b)) return make_number(0.0);
    return make_number(MoveFileExA(dk_s(a), dk_s(b), MOVEFILE_REPLACE_EXISTING) ? 1.0 : 0.0);
}
uint64_t donky_sys_append_file(uint64_t p, uint64_t c) {
    if (!dk_is_str(p) || !dk_is_str(c)) return make_number(0.0);
    FILE* f = fopen(dk_s(p), "ab");
    if (!f) return make_number(0.0);
    fwrite(dk_s(c), 1, str_len(dk_s(c)), f);
    fclose(f);
    return make_number(1.0);
}

// ---- time (all UTC, no CRT time functions so no Win32 strftime assertions) ----
static int64_t dk_floor_div(int64_t a, int64_t b) { int64_t q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) q--; return q; }

static int64_t dk_days_from_civil(int64_t y, int m, int d) {
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void dk_civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}
typedef struct { int64_t Y; int M, D, h, m, s, wd, yd; } DkTm;
static void dk_gmtime(int64_t ts, DkTm* t) {
    int64_t days = dk_floor_div(ts, 86400);
    int64_t rem = ts - days * 86400;
    dk_civil_from_days(days, &t->Y, &t->M, &t->D);
    t->h = (int)(rem / 3600); t->m = (int)((rem % 3600) / 60); t->s = (int)(rem % 60);
    t->wd = (int)(((days % 7) + 11) % 7);               // 1970-01-01 was a Thursday (4)
    t->yd = (int)(days - dk_days_from_civil(t->Y, 1, 1));
}

uint64_t donky_sys_epoch_ms(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return make_number((double)((u.QuadPart - 116444736000000000ULL) / 10000ULL));
}

// -> [year, month, day, hour, minute, second, weekday(0=Sun), yearday(0-based)]
uint64_t donky_sys_gmtime(uint64_t ts) {
    DkTm t; dk_gmtime((int64_t)floor(is_number(ts) ? as_number(ts) : 0.0), &t);
    uint64_t a = donky_alloc_array();
    donky_array_push(a, make_number((double)t.Y)); donky_array_push(a, make_number(t.M));
    donky_array_push(a, make_number(t.D));         donky_array_push(a, make_number(t.h));
    donky_array_push(a, make_number(t.m));         donky_array_push(a, make_number(t.s));
    donky_array_push(a, make_number(t.wd));        donky_array_push(a, make_number(t.yd));
    return a;
}

uint64_t donky_sys_timegm(uint64_t Y, uint64_t Mo, uint64_t D, uint64_t H, uint64_t Mi, uint64_t S) {
    int64_t y = (int64_t)as_number(Y), mo = (int64_t)as_number(Mo);
    y += dk_floor_div(mo - 1, 12);
    mo = ((mo - 1) % 12 + 12) % 12 + 1;
    int64_t days = dk_days_from_civil(y, (int)mo, 1) + ((int64_t)as_number(D) - 1);
    int64_t ts = days * 86400 + (int64_t)as_number(H) * 3600 + (int64_t)as_number(Mi) * 60 + (int64_t)as_number(S);
    return make_number((double)ts);
}

// Supported: %Y %y %m %d %e %H %I %M %S %p %j %a %A %b %B %u %w %Z %z %s %F %T %R %D %%
uint64_t donky_sys_strftime(uint64_t ts_val, uint64_t fmt_val) {
    if (!dk_is_str(fmt_val)) return donky_alloc_string("");
    static const char* WD[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    static const char* MN[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
    int64_t ts = (int64_t)floor(is_number(ts_val) ? as_number(ts_val) : 0.0);
    DkTm t; dk_gmtime(ts, &t);
    const char* f = dk_s(fmt_val);
    char out[1024]; size_t o = 0; const size_t cap = sizeof out;
#define APP(...) do { if (o + 64 < cap) o += (size_t)snprintf(out + o, cap - o, __VA_ARGS__); } while (0)
    for (; *f; f++) {
        if (*f != '%' || !f[1]) { if (o + 2 < cap) out[o++] = *f; continue; }
        f++;
        int h12 = (t.h % 12 == 0) ? 12 : t.h % 12;
        switch (*f) {
            case 'Y': APP("%lld", (long long)t.Y); break;
            case 'y': APP("%02d", (int)(((t.Y % 100) + 100) % 100)); break;
            case 'm': APP("%02d", t.M); break;
            case 'd': APP("%02d", t.D); break;
            case 'e': APP("%2d", t.D); break;
            case 'H': APP("%02d", t.h); break;
            case 'I': APP("%02d", h12); break;
            case 'M': APP("%02d", t.m); break;
            case 'S': APP("%02d", t.s); break;
            case 'p': APP("%s", t.h < 12 ? "AM" : "PM"); break;
            case 'j': APP("%03d", t.yd + 1); break;
            case 'a': APP("%.3s", WD[t.wd]); break;
            case 'A': APP("%s", WD[t.wd]); break;
            case 'b': APP("%.3s", MN[t.M - 1]); break;
            case 'B': APP("%s", MN[t.M - 1]); break;
            case 'u': APP("%d", t.wd == 0 ? 7 : t.wd); break;
            case 'w': APP("%d", t.wd); break;
            case 'Z': APP("UTC"); break;
            case 'z': APP("+0000"); break;
            case 's': APP("%lld", (long long)ts); break;
            case 'F': APP("%lld-%02d-%02d", (long long)t.Y, t.M, t.D); break;
            case 'T': APP("%02d:%02d:%02d", t.h, t.m, t.s); break;
            case 'R': APP("%02d:%02d", t.h, t.m); break;
            case 'D': APP("%02d/%02d/%02d", t.M, t.D, (int)(((t.Y % 100) + 100) % 100)); break;
            case '%': APP("%%"); break;
            default:  APP("%%%c", *f); break;
        }
    }
#undef APP
    out[o] = 0;
    return donky_alloc_string(out);
}

// ---- hashing ----
#define DK_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define DK_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static uint8_t* dk_pad(const uint8_t* m, size_t L, int big_endian, size_t* outn) {
    size_t n = ((L + 8) / 64 + 1) * 64;
    uint8_t* b = (uint8_t*)calloc(n, 1);
    if (L) memcpy(b, m, L);
    b[L] = 0x80;
    uint64_t bits = (uint64_t)L * 8;
    for (int i = 0; i < 8; i++) b[n - 8 + i] = (uint8_t)(big_endian ? (bits >> (56 - 8 * i)) : (bits >> (8 * i)));
    *outn = n;
    return b;
}

static void dk_sha256(const uint8_t* m, size_t L, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t n; uint8_t* b = dk_pad(m, L, 1, &n);
    for (size_t off = 0; off < n; off += 64) {
        uint32_t w[64];
        for (int t = 0; t < 16; t++)
            w[t] = ((uint32_t)b[off+4*t] << 24) | ((uint32_t)b[off+4*t+1] << 16) | ((uint32_t)b[off+4*t+2] << 8) | (uint32_t)b[off+4*t+3];
        for (int t = 16; t < 64; t++) {
            uint32_t s0 = DK_ROTR(w[t-15], 7) ^ DK_ROTR(w[t-15], 18) ^ (w[t-15] >> 3);
            uint32_t s1 = DK_ROTR(w[t-2], 17) ^ DK_ROTR(w[t-2], 19) ^ (w[t-2] >> 10);
            w[t] = w[t-16] + s0 + w[t-7] + s1;
        }
        uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int t = 0; t < 64; t++) {
            uint32_t S1 = DK_ROTR(e,6) ^ DK_ROTR(e,11) ^ DK_ROTR(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[t] + w[t];
            uint32_t S0 = DK_ROTR(a,2) ^ DK_ROTR(a,13) ^ DK_ROTR(a,22);
            uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
        }
        h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    free(b);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16); out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i]; }
}

static void dk_sha1(const uint8_t* m, size_t L, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    size_t n; uint8_t* b = dk_pad(m, L, 1, &n);
    for (size_t off = 0; off < n; off += 64) {
        uint32_t w[80];
        for (int t = 0; t < 16; t++)
            w[t] = ((uint32_t)b[off+4*t] << 24) | ((uint32_t)b[off+4*t+1] << 16) | ((uint32_t)b[off+4*t+2] << 8) | (uint32_t)b[off+4*t+3];
        for (int t = 16; t < 80; t++) w[t] = DK_ROTL(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);
        uint32_t a=h[0],bb=h[1],c=h[2],d=h[3],e=h[4];
        for (int t = 0; t < 80; t++) {
            uint32_t f, k;
            if (t < 20)      { f = (bb & c) | (~bb & d);           k = 0x5A827999; }
            else if (t < 40) { f = bb ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (t < 60) { f = (bb & c) | (bb & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = bb ^ c ^ d;                     k = 0xCA62C1D6; }
            uint32_t tmp = DK_ROTL(a,5) + f + e + k + w[t];
            e = d; d = c; c = DK_ROTL(bb,30); bb = a; a = tmp;
        }
        h[0]+=a; h[1]+=bb; h[2]+=c; h[3]+=d; h[4]+=e;
    }
    free(b);
    for (int i = 0; i < 5; i++) { out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16); out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i]; }
}

static void dk_md5(const uint8_t* m, size_t L, uint8_t out[16]) {
    static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                              4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    static uint32_t K[64]; static int kinit = 0;
    if (!kinit) { for (int i = 0; i < 64; i++) K[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0); kinit = 1; }
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    size_t n; uint8_t* b = dk_pad(m, L, 0, &n);
    for (size_t off = 0; off < n; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++)
            M[i] = (uint32_t)b[off+4*i] | ((uint32_t)b[off+4*i+1] << 8) | ((uint32_t)b[off+4*i+2] << 16) | ((uint32_t)b[off+4*i+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B; B = B + DK_ROTL(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    free(b);
    uint32_t r[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; i++) { out[4*i] = (uint8_t)r[i]; out[4*i+1] = (uint8_t)(r[i] >> 8); out[4*i+2] = (uint8_t)(r[i] >> 16); out[4*i+3] = (uint8_t)(r[i] >> 24); }
}

static void dk_hmac_sha256(const uint8_t* key, size_t kl, const uint8_t* msg, size_t ml, uint8_t out[32]) {
    uint8_t k[64]; memset(k, 0, 64);
    if (kl > 64) dk_sha256(key, kl, k); else if (kl) memcpy(k, key, kl);
    uint8_t* buf = (uint8_t*)malloc(64 + (ml > 32 ? ml : 32));
    uint8_t inner[32];
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x36;
    if (ml) memcpy(buf + 64, msg, ml);
    dk_sha256(buf, 64 + ml, inner);
    for (int i = 0; i < 64; i++) buf[i] = k[i] ^ 0x5c;
    memcpy(buf + 64, inner, 32);
    dk_sha256(buf, 96, out);
    free(buf);
}

// Accepts a string or an array of byte values.
static const uint8_t* dk_in_bytes(uint64_t v, size_t* n, uint8_t** owned) {
    *owned = NULL; *n = 0;
    if (dk_is_str(v)) { const char* s = dk_s(v); *n = str_len(s); return (const uint8_t*)s; }
    if (!is_number(v) && get_tag(v) == TAG_ARRAY) {
        Array* a = (Array*)as_index(v);
        uint8_t* b = (uint8_t*)malloc(a->len + 1);
        for (size_t i = 0; i < a->len; i++) b[i] = is_number(a->data[i]) ? (uint8_t)(int)as_number(a->data[i]) : 0;
        *owned = b; *n = a->len;
        return b;
    }
    return (const uint8_t*)"";
}
static uint64_t dk_hex(const uint8_t* d, size_t n) {
    static const char* H = "0123456789abcdef";
    char* b = (char*)malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) { b[2*i] = H[d[i] >> 4]; b[2*i+1] = H[d[i] & 15]; }
    b[n * 2] = 0;
    uint64_t r = donky_alloc_string(b);
    free(b);
    return r;
}
uint64_t donky_sys_md5(uint64_t v)    { size_t n; uint8_t* o; const uint8_t* p = dk_in_bytes(v, &n, &o); uint8_t d[16]; dk_md5(p, n, d);    free(o); return dk_hex(d, 16); }
uint64_t donky_sys_sha1(uint64_t v)   { size_t n; uint8_t* o; const uint8_t* p = dk_in_bytes(v, &n, &o); uint8_t d[20]; dk_sha1(p, n, d);   free(o); return dk_hex(d, 20); }
uint64_t donky_sys_sha256(uint64_t v) { size_t n; uint8_t* o; const uint8_t* p = dk_in_bytes(v, &n, &o); uint8_t d[32]; dk_sha256(p, n, d); free(o); return dk_hex(d, 32); }
uint64_t donky_sys_hmac_sha256(uint64_t key, uint64_t msg) {
    size_t kn, mn; uint8_t *ko, *mo;
    const uint8_t* kp = dk_in_bytes(key, &kn, &ko);
    const uint8_t* mp = dk_in_bytes(msg, &mn, &mo);
    uint8_t d[32]; dk_hmac_sha256(kp, kn, mp, mn, d);
    free(ko); free(mo);
    return dk_hex(d, 32);
}

// ---- generic HTTP: http_request(method, url, body, headers_string) -> response body ----
// headers_string is "Name: Value\r\nName2: Value2\r\n". Status is available via http_status().
static DONKY_TLS int dk_http_status = 0;
uint64_t donky_http_status(void) { return make_number((double)dk_http_status); }

static wchar_t* dk_widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*)malloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}

uint64_t donky_http_request(uint64_t method_val, uint64_t url_val, uint64_t body_val, uint64_t hdr_val) {
    dk_http_status = 0;
    if (!dk_is_str(method_val) || !dk_is_str(url_val)) return donky_alloc_string("");
    const char* body = dk_is_str(body_val) ? dk_s(body_val) : "";
    const char* hdrs = dk_is_str(hdr_val) ? dk_s(hdr_val) : "";
    size_t blen = strlen(body);

    wchar_t* wurl = dk_widen(dk_s(url_val));
    wchar_t* wmeth = dk_widen(dk_s(method_val));
    wchar_t* whdr = hdrs[0] ? dk_widen(hdrs) : NULL;
    wchar_t *host = NULL, *path = NULL;
    HINTERNET hS = NULL, hC = NULL, hR = NULL;
    char* resp = NULL; size_t total = 0;

    URL_COMPONENTS uc; ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1; uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1; uc.dwExtraInfoLength = (DWORD)-1;

    if (WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        host = (wchar_t*)malloc((uc.dwHostNameLength + 1) * sizeof(wchar_t));
        wmemcpy(host, uc.lpszHostName, uc.dwHostNameLength); host[uc.dwHostNameLength] = 0;
        path = (wchar_t*)malloc((uc.dwUrlPathLength + uc.dwExtraInfoLength + 2) * sizeof(wchar_t));
        size_t o = 0;
        if (uc.dwUrlPathLength == 0) path[o++] = L'/';
        else { wmemcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength); o += uc.dwUrlPathLength; }
        if (uc.dwExtraInfoLength) { wmemcpy(path + o, uc.lpszExtraInfo, uc.dwExtraInfoLength); o += uc.dwExtraInfoLength; }
        path[o] = 0;

        hS = WinHttpOpen(L"Donky/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hS) hC = WinHttpConnect(hS, host, uc.nPort, 0);
        if (hC) {
            DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            hR = WinHttpOpenRequest(hC, wmeth, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        }
        if (hR) {
            BOOL ok = WinHttpSendRequest(hR,
                whdr ? whdr : WINHTTP_NO_ADDITIONAL_HEADERS, whdr ? (DWORD)-1L : 0,
                blen ? (LPVOID)body : WINHTTP_NO_REQUEST_DATA, (DWORD)blen, (DWORD)blen, 0);
            if (ok) ok = WinHttpReceiveResponse(hR, NULL);
            if (ok) {
                DWORD st = 0, sz = sizeof st;
                if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX))
                    dk_http_status = (int)st;
                for (;;) {
                    DWORD avail = 0, got = 0;
                    if (!WinHttpQueryDataAvailable(hR, &avail) || avail == 0) break;
                    char* nb = (char*)realloc(resp, total + avail + 1);
                    if (!nb) break;
                    resp = nb;
                    if (!WinHttpReadData(hR, resp + total, avail, &got)) break;
                    total += got;
                    resp[total] = 0;
                }
            }
        }
    }
    if (hR) WinHttpCloseHandle(hR);
    if (hC) WinHttpCloseHandle(hC);
    if (hS) WinHttpCloseHandle(hS);
    free(wurl); free(wmeth); free(whdr); free(host); free(path);
    uint64_t r = donky_alloc_string(resp ? resp : "");
    free(resp);
    return r;
}
// ===================== end standard library builtins =====================

int main(int argc, char** argv) {
    gc_init_lock();

    donky_set_args(argc, argv);
    extern int64_t donky_toplevel();
    donky_toplevel();
    
    while (active_threads > 0) Sleep(10);
    
    if (current_exception != 0) { 
        printf("Uncaught Exception: "); 
        print_value(current_exception); 
        printf("\n"); 
        return 1; 
    }
    return 0;
}