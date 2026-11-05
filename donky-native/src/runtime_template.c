#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
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
#else
#include <inttypes.h>
#include <setjmp.h>
#include <wchar.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <termios.h>
#include <dirent.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <curl/curl.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <libproc.h>
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

// ---- Win32 -> POSIX shims: lets the shared code below compile unchanged ----
typedef int       SOCKET;
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)
#define closesocket(s)  close(s)

typedef uint32_t  DWORD;
typedef uint64_t  ULONGLONG;

typedef void*     HMODULE;
typedef void*     FARPROC;
#define LoadLibraryA(n)       dlopen((n), RTLD_NOW)
#define GetProcAddress(h, n)  dlsym((h), (n))

// gc_lock must be recursive (CRITICAL_SECTION is)
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION* m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
#define EnterCriticalSection(m) pthread_mutex_lock(m)
#define LeaveCriticalSection(m) pthread_mutex_unlock(m)

typedef pthread_cond_t CONDITION_VARIABLE;
#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif
static inline void InitializeConditionVariable(CONDITION_VARIABLE* cv) { pthread_cond_init(cv, NULL); }
#define WakeConditionVariable(cv)     pthread_cond_signal(cv)
#define WakeAllConditionVariable(cv)  pthread_cond_broadcast(cv)
static inline int SleepConditionVariableCS(CONDITION_VARIABLE* cv, CRITICAL_SECTION* cs, DWORD ms) {
    (void)ms;
    return pthread_cond_wait(cv, cs) == 0;
}

static inline void Sleep(DWORD ms) {
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
// ms since boot, like the Win32 call of the same name
static inline ULONGLONG GetTickCount64(void) {
    struct timespec ts;
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)(ts.tv_nsec / 1000000L);
}
#endif

// --- NaN Boxing Tag Definitions ---
#define TAG_MASK 0xFFF8000000000000ULL
#define TAG_BASE 0x7FF8000000000000ULL
#define TAG_BOOL 1ULL
#define TAG_STRING 2ULL
#define TAG_ARRAY 3ULL
#define TAG_MAP 4ULL

/* Canonical tagged-bool word forms (C1). */
#define BOOL_TRUE_BITS  (TAG_BASE | (TAG_BOOL << 48) | 1ULL)
#define BOOL_FALSE_BITS (TAG_BASE | (TAG_BOOL << 48))
/* NaN canonicalization target (D-fix). Must equal heap.rs Value::CANONICAL_NAN.
   Negative quiet NaN: not masked to TAG_BASE, survives arithmetic unchanged. */
#define CANONICAL_NAN 0xFFF8000000000000ULL

/* These MUST match aot.rs's inline tag constants and heap.rs's Value::TAG_*.
   If you change one, change all three. */

/* BEGIN TAG ASSERTIONS */
typedef char dk_tag_check_MASK  [(TAG_MASK   == 0xFFF8000000000000ULL) ? 1 : -1];
typedef char dk_tag_check_BASE  [(TAG_BASE   == 0x7FF8000000000000ULL) ? 1 : -1];
typedef char dk_tag_check_BOOL  [(TAG_BOOL   == 1ULL) ? 1 : -1];
typedef char dk_tag_check_STRING[(TAG_STRING == 2ULL) ? 1 : -1];
typedef char dk_tag_check_ARRAY [(TAG_ARRAY  == 3ULL) ? 1 : -1];
typedef char dk_tag_check_MAP   [(TAG_MAP    == 4ULL) ? 1 : -1];
typedef char dk_tag_check_TRUE  [(BOOL_TRUE_BITS  == 0x7FF9000000000001ULL) ? 1 : -1];
typedef char dk_tag_check_FALSE [(BOOL_FALSE_BITS == 0x7FF9000000000000ULL) ? 1 : -1];
typedef char dk_tag_check_CNAN  [(CANONICAL_NAN   == 0xFFF8000000000000ULL) ? 1 : -1];
/* END TAG ASSERTIONS */

static int rand_seeded = 0;

#define MAX_THREADS 64
static volatile int active_threads = 0;

// --- Garbage Collector & Thread Synchronization ---
CRITICAL_SECTION gc_lock;
static int gc_lock_init = 0;

typedef struct GCHeader {
    int      marked;      // payload-24
    size_t   size;        // payload-16  EXACT requested size (str_len relies on it)
    uint64_t type_tag;    // payload- 8
} GCHeader;               // 24 bytes: field offsets from the payload are identical to the old 32-byte header
typedef char gc_hdr_size_check[(sizeof(GCHeader) == 24) ? 1 : -1];

/* Forward declaration: dk_render_value (and others) use str_len before its definition below. */
static inline size_t str_len(const char* s);

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
#ifdef _WIN32
    char*     mem  = (char*)VirtualAlloc(NULL, cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    char*     mem  = (char*)mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == (char*)MAP_FAILED) mem = NULL;
#endif
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

// Exceptions are per-thread: with the worker pool, a shared global made one task's
// throw visible to (and clearable by) every other thread.
#ifdef _MSC_VER
#define DK_TLS __declspec(thread)
#else
#define DK_TLS __thread
#endif
DK_TLS uint64_t current_exception = 0;
static uint64_t g_globals = 0;

int is_number(uint64_t v) { return (v & TAG_MASK) != TAG_BASE; }
double as_number(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
uint64_t get_tag(uint64_t v) { return is_number(v) ? 0 : ((v >> 48) & 0x7); }
uint64_t as_index(uint64_t v) { return v & 0x0000FFFFFFFFFFFFULL; }
/* Any double whose bits collide with TAG_BASE (null) is rewritten to CANONICAL_NAN, so aarch64's
   default quiet NaN (0x7FF8000000000000) can never be mistaken for null. Mirrors heap.rs Value::number. */
uint64_t make_number(double d) {
    uint64_t v; memcpy(&v, &d, 8);
    if ((v & TAG_MASK) == TAG_BASE) v = CANONICAL_NAN;
    return v;
}
/* For AOT inline fast paths (fadd/fsub/fmul/fdiv) that bypass make_number.
   Agent 2 binds as `donky_make_number: (f64) -> i64` in aot.rs. */
uint64_t donky_make_number(double d) { return make_number(d); }

// Shortest %.Ng (N = 15..17) that parses back to the same double; integers print exactly.

// Shortest %.Ng (N = 15..17) that parses back to the same double; integers print exactly.
static void dk_fmt_num(double d, char* buf, size_t cap) {
    if (d != d) { snprintf(buf, cap, "nan"); return; }
    if (isinf(d)) { snprintf(buf, cap, d < 0 ? "-inf" : "inf"); return; }
    if (fabs(d) < 9.2e18 && d == (double)(long long)d) { snprintf(buf, cap, "%lld", (long long)d); return; }
    for (int p = 15; p <= 17; p++) {
        snprintf(buf, cap, "%.*g", p, d);
        if (strtod(buf, NULL) == d) break;
    }
}

// ADD in runtime_template.c: dk_render_value (immediately BEFORE print_value)
#define DK_RENDER_MAX_DEPTH 32

static void dk_rput(char* out, size_t cap, size_t* pos, const char* s, size_t n) {
    if (out && *pos < cap) {
        size_t room = cap - *pos;
        memcpy(out + *pos, s, n < room ? n : room);
    }
    *pos += n;
}

/* Single source of truth for print() and str(). Strings nested in containers are quoted
   WITHOUT escaping (known limitation — a string containing a quote renders ambiguously).
   Containers deeper than DK_RENDER_MAX_DEPTH render as "..." (also cycle protection).
   Caller must NUL-terminate. */
static void dk_render_value(char* out, size_t cap, size_t* pos, uint64_t val, int depth) {
    if (is_number(val)) {
        char nb[64]; dk_fmt_num(as_number(val), nb, sizeof nb);
        dk_rput(out, cap, pos, nb, strlen(nb));
        return;
    }
    uint64_t tag = get_tag(val);
    if (tag == TAG_BOOL) {
        if (val & 1) dk_rput(out, cap, pos, "true", 4); else dk_rput(out, cap, pos, "false", 5);
    } else if (tag == TAG_STRING) {
        const char* s = (const char*)as_index(val);
        dk_rput(out, cap, pos, "\"", 1);
        dk_rput(out, cap, pos, s, str_len(s));
        dk_rput(out, cap, pos, "\"", 1);
    } else if (tag == TAG_ARRAY) {
        if (depth >= DK_RENDER_MAX_DEPTH) { dk_rput(out, cap, pos, "...", 3); return; }
        Array* arr = (Array*)as_index(val);
        dk_rput(out, cap, pos, "[", 1);
        for (size_t i = 0; i < arr->len; i++) {
            dk_render_value(out, cap, pos, arr->data[i], depth + 1);
            if (i < arr->len - 1) dk_rput(out, cap, pos, ", ", 2);
        }
        dk_rput(out, cap, pos, "]", 1);
    } else if (tag == TAG_MAP) {
        if (depth >= DK_RENDER_MAX_DEPTH) { dk_rput(out, cap, pos, "...", 3); return; }
        Map* map = (Map*)as_index(val);
        dk_rput(out, cap, pos, "{", 1);
        for (size_t i = 0; i < map->len; i++) {
            dk_render_value(out, cap, pos, map->keys[i], depth + 1);
            dk_rput(out, cap, pos, ": ", 2);
            dk_render_value(out, cap, pos, map->vals[i], depth + 1);
            if (i < map->len - 1) dk_rput(out, cap, pos, ", ", 2);
        }
        dk_rput(out, cap, pos, "}", 1);
    } else {
        dk_rput(out, cap, pos, "null", 4);
    }
}

void print_value(uint64_t val) {
    size_t n = 0;
    dk_render_value(NULL, 0, &n, val, 0);
    char* buf = (char*)malloc(n + 1);
    if (!buf) gc_oom();
    size_t w = 0;
    dk_render_value(buf, n + 1, &w, val, 0);
    buf[n] = 0;
    fwrite(buf, 1, n, stdout);
    free(buf);
}

void donky_print(uint64_t val) {
    if (!is_number(val) && get_tag(val) == TAG_STRING) printf("%s", (char*)as_index(val));
    else print_value(val);
    printf("\n");
}

uint64_t donky_truthy(uint64_t val) {
    if (is_number(val)) return make_number(as_number(val) != 0.0 ? 1.0 : 0.0);
    if (get_tag(val) == TAG_BOOL) return make_number((val & 1) ? 1.0 : 0.0);
    if (val == TAG_BASE) return make_number(0.0);        /* null is falsy */
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

#ifdef _WIN32
uint64_t donky_kill_process(uint64_t pid_val) {
    if (!is_number(pid_val)) return BOOL_FALSE_BITS;
    DWORD pid = (DWORD)as_number(pid_val);
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!hProc) return BOOL_FALSE_BITS;
    BOOL res = TerminateProcess(hProc, 1);
    CloseHandle(hProc);
    return res ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
#else
uint64_t donky_kill_process(uint64_t pid_val) {
    if (!is_number(pid_val)) return BOOL_FALSE_BITS;
    pid_t pid = (pid_t)as_number(pid_val);
    if (pid <= 1) return BOOL_FALSE_BITS;   // never signal init, our process group (0) or every process (-1)
    return kill(pid, SIGKILL) == 0 ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
#endif

uint64_t donky_get_uptime() {
    ULONGLONG ms = GetTickCount64();
    return make_number((double)(ms / 1000));
}

#ifdef _WIN32
uint64_t donky_get_cpu_cores() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    return make_number((double)sysInfo.dwNumberOfProcessors);
}
#else
uint64_t donky_get_cpu_cores() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return make_number((double)(n > 0 ? n : 1));
}
#endif

#ifdef _WIN32
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
#else
uint64_t donky_get_disk_info() {
    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        double gb    = 1024.0 * 1024.0 * 1024.0;
        double total = (double)sv.f_blocks * (double)sv.f_frsize / gb;
        double free_ = (double)sv.f_bfree  * (double)sv.f_frsize / gb;
        uint64_t m = donky_alloc_map();
        donky_map_set(m, donky_alloc_string("total_gb"), make_number(total));
        donky_map_set(m, donky_alloc_string("free_gb"),  make_number(free_));
        donky_map_set(m, donky_alloc_string("used_gb"),  make_number(total - free_));
        return m;
    }
    return donky_alloc_map();
}
#endif

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
#ifdef _WIN32
            VirtualFree(c->base, 0, MEM_RELEASE);
#else
            munmap(c->base, c->cap);
#endif
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

#ifdef _WIN32
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
#else
// Highest address of the calling thread's stack (cached per thread).
static uintptr_t dk_stack_high(void) {
    static __thread uintptr_t cached = 0;
    if (cached) return cached;
#if defined(__APPLE__)
    cached = (uintptr_t)pthread_get_stackaddr_np(pthread_self());   // returns the TOP of the stack
#else
    pthread_attr_t attr; void* addr = NULL; size_t size = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &addr, &size);
        pthread_attr_destroy(&attr);
        cached = (uintptr_t)addr + size;
    }
#endif
    return cached;
}

// Scans from its own frame (below every caller local / spilled register) up to the stack top.
static __attribute__((noinline)) void dk_gc_scan(uintptr_t high) {
    volatile uintptr_t here = 0;
    uintptr_t* stack_ptr = (uintptr_t*)&here;
    uintptr_t* top = (uintptr_t*)high;
    while (stack_ptr < top) {
        uint64_t val = *stack_ptr;
        if ((val & TAG_MASK) == TAG_BASE && gc_valid_object(val)) gc_mark(val);
        stack_ptr++;
    }
}

void gc_mark_stack() {
    jmp_buf env;
    setjmp(env);                       // dumps callee-saved registers into this frame
#if defined(__GNUC__) && !defined(__clang__)
    __builtin_unwind_init();           // glibc mangles some jmp_buf slots; this spills ALL callee-saved regs
#endif
    uintptr_t high = dk_stack_high();
    if (!high) return;
    dk_gc_scan(high);
    __asm__ volatile("" : : "r"(&env) : "memory");   // keep `env` alive in the frame
}
#endif

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

static const char* dk_type_name(uint64_t val) {
    if (is_number(val)) return "number";
    uint64_t tag = get_tag(val);
    if (tag == TAG_BOOL)   return "bool";
    if (tag == TAG_STRING) return "string";
    if (tag == TAG_ARRAY)  return "array";
    if (tag == TAG_MAP) {
        Map* m = (Map*)as_index(val);
        for (size_t i = 0; i < m->len; i++) {
            uint64_t k = m->keys[i];
            if (is_number(k) || get_tag(k) != TAG_STRING) continue;
            const char* ks = (const char*)as_index(k);
            if (str_len(ks) == 8 && memcmp(ks, "__type__", 8) == 0) {
                uint64_t v = m->vals[i];
                if (!is_number(v) && get_tag(v) == TAG_STRING)
                    return (const char*)as_index(v);
                return "map";
            }
        }
        return "map";
    }
    return "null";
}

static void dk_throw_binop(const char* verb, uint64_t a, uint64_t b) {
    char msg[512];
    snprintf(msg, sizeof msg, "cannot %s %s and %s", verb, dk_type_name(a), dk_type_name(b));
    donky_throw(donky_alloc_string(msg));
}

/* Thread-safe strtok shim: identical call shape on POSIX and Windows. */
static char* dk_strtok_r(char* s, const char* delim, char** save) {
#ifdef _WIN32
    return strtok_s(s, delim, save);
#else
    return strtok_r(s, delim, save);
#endif
}

uint64_t donky_index_get(uint64_t target, uint64_t index) {
    if (get_tag(target) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(target);
        double di = is_number(index) ? as_number(index) : -1.0;
        if (di >= 0 && di < (double)arr->len) return arr->data[(size_t)di];
        return TAG_BASE;
    } else if (get_tag(target) == TAG_MAP) {
        Map* map = (Map*)as_index(target);
        int64_t f = map_find(map, index);
        if (f >= 0) return map->vals[f];
        return TAG_BASE;
    } else if (get_tag(target) == TAG_STRING) {
        const char* s = (const char*)as_index(target);
        if (is_number(index)) {
            double di = as_number(index);
            if (di >= 0 && di < (double)str_len(s)) {
                char one[2] = { s[(size_t)di], 0 };
                return donky_alloc_string(one);
            }
        }
        return TAG_BASE;
    }
    return 0;
}

uint64_t donky_index_set(uint64_t target, uint64_t index, uint64_t val) {
    if (get_tag(target) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(target);
        if (!is_number(index)) { donky_throw(donky_alloc_string("array index must be an integer")); return 0; }
        double di = as_number(index);
        if (di != floor(di)) { donky_throw(donky_alloc_string("array index must be an integer")); return 0; }
        if (di < 0 || di >= (double)arr->len) { donky_throw(donky_alloc_string("index out of range")); return 0; }
        arr->data[(size_t)di] = val;
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
    dk_throw_binop("add", a, b);
    return TAG_BASE;
}

uint64_t donky_sub(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return make_number(as_number(a) - as_number(b));
    dk_throw_binop("subtract", a, b);
    return TAG_BASE;
}

uint64_t donky_fdiv(uint64_t a, uint64_t b) {
    if (!is_number(a) || !is_number(b)) { dk_throw_binop("divide", a, b); return TAG_BASE; }
    double db = as_number(b);
    if (db == 0.0) return make_number(a > 0 ? 1.0/0.0 : (a < 0 ? -1.0/0.0 : 0.0/0.0)); // IEEE inf/nan
    return make_number(as_number(a) / db);
}

uint64_t donky_mul(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return make_number(as_number(a) * as_number(b));
    dk_throw_binop("multiply", a, b);
    return TAG_BASE;
}

uint64_t donky_div(uint64_t a, uint64_t b) {
    if (!(is_number(a) && is_number(b))) { dk_throw_binop("divide", a, b); return TAG_BASE; }
    double db = as_number(b);
    if (db == 0.0) { donky_throw(donky_alloc_string("division by zero")); return TAG_BASE; }  /* also -0.0 */
    return make_number(trunc(as_number(a) / db));   /* A2: integer division; checked (zero throws above) */
}

uint64_t donky_mod(uint64_t a, uint64_t b) {
    if (!(is_number(a) && is_number(b))) { dk_throw_binop("modulo", a, b); return TAG_BASE; }
    double db = as_number(b);
    if (db == 0.0) { donky_throw(donky_alloc_string("modulo by zero")); return TAG_BASE; }
    return make_number(fmod(as_number(a), db));
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
    if (!is_number(val) && get_tag(val) == TAG_STRING) return val;   /* strings pass through unquoted */
    size_t n = 0;
    dk_render_value(NULL, 0, &n, val, 0);
    char* buf = (char*)malloc(n + 1);
    if (!buf) gc_oom();
    size_t w = 0;
    dk_render_value(buf, n + 1, &w, val, 0);                         /* identical to print_value's rendering */
    buf[n] = 0;
    uint64_t r = donky_alloc_string(buf);
    free(buf);
    return r;
}

uint64_t donky_type(uint64_t val) {
    /* TAG_MAP with a string "__type__" entry returns that string (type(Foo{x:1}) == "Foo"),
       otherwise "map". donky_alloc_string copies, so the caller always owns a fresh string
       (never the interned key/value). */
    return donky_alloc_string(dk_type_name(val));
}

uint64_t donky_num(uint64_t val) {
    if (is_number(val)) return val;
    if (get_tag(val) == TAG_STRING) return make_number(atof((char*)as_index(val)));
    return make_number(0.0);
}

uint64_t donky_read_file(uint64_t path_val) {
    if (get_tag(path_val) != TAG_STRING) {
        donky_throw(donky_alloc_string("read_file: path must be a string"));
        return 0;
    }
    char* path = (char*)as_index(path_val);
    char err_buf[600];
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(err_buf, sizeof(err_buf), "File not found: %s", path);
        donky_throw(donky_alloc_string(err_buf));
        return 0;
    }
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0) len = ftell(f);
    if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(err_buf, sizeof(err_buf), "Cannot read file: %s", path);
        donky_throw(donky_alloc_string(err_buf));
        return 0;
    }
    char* buf = (char*)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        donky_throw(donky_alloc_string("read_file: out of memory"));
        return 0;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = 0;
    uint64_t res = donky_alloc_string(buf);
    free(buf);
    return res;
}

uint64_t donky_write_file(uint64_t path_val, uint64_t content_val) {
    if (get_tag(path_val) != TAG_STRING || get_tag(content_val) != TAG_STRING) return BOOL_FALSE_BITS;
    FILE* f = fopen((char*)as_index(path_val), "wb");
    if (!f) return BOOL_FALSE_BITS;
    char* content = (char*)as_index(content_val);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return BOOL_TRUE_BITS;
}

uint64_t donky_file_exists(uint64_t path_val) {
    if (get_tag(path_val) != TAG_STRING) return BOOL_FALSE_BITS;
    FILE* f = fopen((char*)as_index(path_val), "r");
    if (f) { fclose(f); return BOOL_TRUE_BITS; }
    return BOOL_FALSE_BITS;
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
    if (!copy) gc_oom();
    /* NOTE (preserved strtok semantics): `delim` is a SET of delimiter characters, not a substring,
       and consecutive delimiters do NOT produce empty tokens. split("a,,b", ",") == ["a","b"];
       split("a, b", ", ") splits on ',' and ' ' individually. */
    char* save = NULL;
    char* token = dk_strtok_r(copy, delim, &save);
    while (token != NULL) {
        donky_array_push(arr_val, donky_alloc_string(token));
        token = dk_strtok_r(NULL, delim, &save);
    }
    free(copy);
    return arr_val;
}

uint64_t donky_join(uint64_t arr_val, uint64_t delim_val) {
    if (get_tag(arr_val) != TAG_ARRAY || get_tag(delim_val) != TAG_STRING) return 0;
    Array* arr = (Array*)as_index(arr_val);
    char* delim = (char*)as_index(delim_val);
    size_t dlen = strlen(delim), total_len = 0;
    for (size_t i = 0; i < arr->len; i++) {
        if (is_number(arr->data[i]) || get_tag(arr->data[i]) != TAG_STRING) {
            char msg[64];
            snprintf(msg, sizeof msg, "join: non-string element at index %llu", (unsigned long long)i);
            donky_throw(donky_alloc_string(msg));
            return TAG_BASE;
        }
        total_len += strlen((char*)as_index(arr->data[i]));
        if (i + 1 < arr->len) total_len += dlen;
    }
    char* buf = (char*)malloc(total_len + 1);
    if (!buf) gc_oom();
    size_t pos = 0;
    for (size_t i = 0; i < arr->len; i++) {
        const char* e = (const char*)as_index(arr->data[i]);
        size_t el = strlen(e);
        memcpy(buf + pos, e, el); pos += el;
        if (i + 1 < arr->len) { memcpy(buf + pos, delim, dlen); pos += dlen; }
    }
    buf[pos] = 0;
    uint64_t res = donky_alloc_string(buf); free(buf); return res;
}

uint64_t donky_contains(uint64_t haystack, uint64_t needle) {
    uint64_t h_tag = get_tag(haystack);
    uint64_t n_tag = get_tag(needle);

    if (h_tag == TAG_STRING) {
        if (n_tag == TAG_STRING)
            return strstr((char*)as_index(haystack), (char*)as_index(needle)) != NULL ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
        return BOOL_FALSE_BITS;
    }
    if (h_tag == TAG_ARRAY) {
        Array* arr = (Array*)as_index(haystack);
        for (size_t i = 0; i < arr->len; i++) {
            if (as_number(donky_eq(arr->data[i], needle)) == 1.0) return BOOL_TRUE_BITS;
        }
        return BOOL_FALSE_BITS;
    }
    if (h_tag == TAG_MAP) {
        Map* map = (Map*)as_index(haystack);
        for (size_t i = 0; i < map->len; i++) {
            if (as_number(donky_eq(map->keys[i], needle)) == 1.0) return BOOL_TRUE_BITS;
        }
        return BOOL_FALSE_BITS;
    }
    donky_throw(donky_alloc_string("contains: unsupported haystack type"));
    return TAG_BASE;
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

/* ---- math builtins (throw on non-number; NaN/inf results are canonicalized by make_number) ---- */
uint64_t donky_pow(uint64_t a, uint64_t b) {
    if (!is_number(a) || !is_number(b)) { dk_throw_binop("raise", a, b); return TAG_BASE; }
    return make_number(pow(as_number(a), as_number(b)));
}
uint64_t donky_exp(uint64_t a) {
    if (!is_number(a)) { donky_throw(donky_alloc_string("exp: expected a number")); return TAG_BASE; }
    return make_number(exp(as_number(a)));
}
uint64_t donky_log(uint64_t a) {
    if (!is_number(a)) { donky_throw(donky_alloc_string("log: expected a number")); return TAG_BASE; }
    return make_number(log(as_number(a)));   /* natural log */
}
uint64_t donky_tanh(uint64_t a) {
    if (!is_number(a)) { donky_throw(donky_alloc_string("tanh: expected a number")); return TAG_BASE; }
    return make_number(tanh(as_number(a)));
}
uint64_t donky_max(uint64_t a, uint64_t b) {
    if (!is_number(a) || !is_number(b)) { dk_throw_binop("max", a, b); return TAG_BASE; }
    return make_number(fmax(as_number(a), as_number(b)));
}
uint64_t donky_min(uint64_t a, uint64_t b) {
    if (!is_number(a) || !is_number(b)) { dk_throw_binop("min", a, b); return TAG_BASE; }
    return make_number(fmin(as_number(a), as_number(b)));
}
uint64_t donky_abs(uint64_t a) {
    if (!is_number(a)) { donky_throw(donky_alloc_string("abs: expected a number")); return TAG_BASE; }
    return make_number(fabs(as_number(a)));
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
#ifdef _WIN32
uint64_t donky_cls() { system("cls"); return 0; }
uint64_t donky_getch() { return make_number((double)_getch()); }
uint64_t donky_kbhit() { return _kbhit() ? BOOL_TRUE_BITS : BOOL_FALSE_BITS; }
#else
uint64_t donky_cls() { fputs("\033[2J\033[H", stdout); fflush(stdout); return 0; }

// Blocking single-key read, no echo, no Enter needed (like _getch).
uint64_t donky_getch() {
    struct termios o, n; unsigned char ch = 0;
    int tty = (tcgetattr(STDIN_FILENO, &o) == 0);
    if (tty) { n = o; n.c_lflag &= ~(tcflag_t)(ICANON | ECHO); n.c_cc[VMIN] = 1; n.c_cc[VTIME] = 0; tcsetattr(STDIN_FILENO, TCSANOW, &n); }
    ssize_t r = read(STDIN_FILENO, &ch, 1);
    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &o);
    return make_number(r == 1 ? (double)ch : -1.0);
}

// Non-blocking "is a key waiting?" (like _kbhit).
uint64_t donky_kbhit() {
    struct termios o, n;
    int tty = (tcgetattr(STDIN_FILENO, &o) == 0);
    if (tty) { n = o; n.c_lflag &= ~(tcflag_t)(ICANON | ECHO); n.c_cc[VMIN] = 0; n.c_cc[VTIME] = 0; tcsetattr(STDIN_FILENO, TCSANOW, &n); }
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    int ready = select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv);
    if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &o);
    return ready > 0 ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
#endif

uint64_t donky_input(uint64_t prompt_val) {
    if (get_tag(prompt_val) == TAG_STRING) {
        printf("%s", (char*)as_index(prompt_val));
        fflush(stdout);
    }
    char buf[4096];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        return donky_alloc_string("");
    }
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
        if (left >= 4 && memcmp(jp->p, "null", 4) == 0) { jp->p += 4; *out = TAG_BASE; return 1; }   /* B2 */
        return jp_fail(jp, "invalid literal");
    default:
        return jp_number(jp, out);
    }
}

// json_decode(str) -> value, or 0 on error (check json_error() to tell error from a real null/0)
uint64_t donky_json_decode(uint64_t s_val) {
    json_err_buf[0] = 0;
    if (!JV_IS(s_val, TAG_STRING)) { snprintf(json_err_buf, sizeof json_err_buf, "json: expected a string"); return TAG_BASE; }
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
        result = TAG_BASE;                              /* B2: check json_error() to tell error from a real null */
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
    typedef uint64_t (*DonkyFfiFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    DonkyFfiFn f = (DonkyFfiFn)fn;
    return f(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
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
#ifdef _WIN32
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
#else
#if defined(__APPLE__)
// ---------------- macOS ----------------
static uint64_t dk_mac_memsize(void) {
    uint64_t v = 0; size_t len = sizeof v;
    if (sysctlbyname("hw.memsize", &v, &len, NULL, 0) != 0) return 0;
    return v;
}
uint64_t donky_get_total_mem() { return make_number((double)(dk_mac_memsize() / 1024 / 1024)); }
uint64_t donky_get_used_mem() {
    uint64_t total = dk_mac_memsize();
    vm_statistics64_data_t vm; mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (!total || host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &cnt) != KERN_SUCCESS)
        return make_number(0.0);
    vm_size_t pg = 0; host_page_size(mach_host_self(), &pg);
    uint64_t avail = ((uint64_t)vm.free_count + (uint64_t)vm.inactive_count) * (uint64_t)pg;
    return make_number((double)((total > avail ? total - avail : 0) / 1024 / 1024));
}
uint64_t donky_get_processes() {
    uint64_t arr = donky_alloc_array();
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) return arr;
    int cap = bytes + 64 * (int)sizeof(pid_t);
    pid_t* pids = (pid_t*)malloc((size_t)cap);
    if (!pids) return arr;
    int got = proc_listpids(PROC_ALL_PIDS, 0, pids, cap);
    int n = got > 0 ? got / (int)sizeof(pid_t) : 0;
    for (int i = 0; i < n; i++) {
        if (pids[i] <= 0) continue;
        char name[256];
        if (proc_name(pids[i], name, sizeof name) <= 0) continue;
        struct proc_taskinfo ti;
        double mem = 0.0, thr = 0.0;
        if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof ti) == (int)sizeof ti) {
            mem = (double)(ti.pti_resident_size / 1024 / 1024); thr = (double)ti.pti_threadnum;
        }
        uint64_t m = donky_alloc_map();
        donky_map_set(m, donky_alloc_string("pid"),     make_number((double)pids[i]));
        donky_map_set(m, donky_alloc_string("name"),    donky_alloc_string(name));
        donky_map_set(m, donky_alloc_string("threads"), make_number(thr));
        donky_map_set(m, donky_alloc_string("mem"),     make_number(mem));
        donky_array_push(arr, m);
    }
    free(pids);
    return arr;
}
static int dk_cpu_ticks(uint64_t* idle, uint64_t* total) {
    host_cpu_load_info_data_t ci; mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&ci, &cnt) != KERN_SUCCESS) return 0;
    *idle  = ci.cpu_ticks[CPU_STATE_IDLE];
    *total = (uint64_t)ci.cpu_ticks[CPU_STATE_USER] + ci.cpu_ticks[CPU_STATE_SYSTEM]
           + ci.cpu_ticks[CPU_STATE_IDLE] + ci.cpu_ticks[CPU_STATE_NICE];
    return 1;
}
#else
// ---------------- Linux (/proc) ----------------
static uint64_t dk_meminfo_kb(const char* key) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256]; size_t kl = strlen(key); uint64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, kl) == 0 && line[kl] == ':') { v = strtoull(line + kl + 1, NULL, 10); break; }
    }
    fclose(f);
    return v;
}
uint64_t donky_get_total_mem() { return make_number((double)(dk_meminfo_kb("MemTotal") / 1024)); }
uint64_t donky_get_used_mem() {
    uint64_t total = dk_meminfo_kb("MemTotal"), avail = dk_meminfo_kb("MemAvailable");
    return make_number((double)((total > avail ? total - avail : 0) / 1024));
}
uint64_t donky_get_processes() {
    uint64_t arr = donky_alloc_array();
    DIR* d = opendir("/proc");
    if (!d) return arr;
    long pagesz = sysconf(_SC_PAGESIZE);
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        char path[300], buf[1024];
        snprintf(path, sizeof path, "/proc/%s/stat", de->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
        buf[n] = 0;
        char* lp = strchr(buf, '(');          // comm may contain spaces/parens: first '(' .. last ')'
        char* rp = strrchr(buf, ')');
        if (!lp || !rp || rp < lp) continue;
        *rp = 0;
        char name[64]; snprintf(name, sizeof name, "%s", lp + 1);
        char state = 0; long nthreads = 0;
        sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %ld", &state, &nthreads);
        double mem = 0.0;
        snprintf(path, sizeof path, "/proc/%s/statm", de->d_name);
        f = fopen(path, "r");
        if (f) { unsigned long sz = 0, rss = 0; if (fscanf(f, "%lu %lu", &sz, &rss) == 2) mem = (double)((uint64_t)rss * (uint64_t)pagesz / 1024 / 1024); fclose(f); }
        uint64_t m = donky_alloc_map();
        donky_map_set(m, donky_alloc_string("pid"),     make_number((double)atoi(de->d_name)));
        donky_map_set(m, donky_alloc_string("name"),    donky_alloc_string(name));
        donky_map_set(m, donky_alloc_string("threads"), make_number((double)nthreads));
        donky_map_set(m, donky_alloc_string("mem"),     make_number(mem));
        donky_array_push(arr, m);
    }
    closedir(d);
    return arr;
}
static int dk_cpu_ticks(uint64_t* idle, uint64_t* total) {
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long u = 0, n = 0, s = 0, i = 0, w = 0, irq = 0, sirq = 0, st = 0;
    int ok = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &w, &irq, &sirq, &st) >= 4;
    fclose(f);
    if (!ok) return 0;
    *idle  = i + w;
    *total = u + n + s + i + w + irq + sirq + st;
    return 1;
}
#endif

uint64_t donky_get_cpu_usage() {
    static uint64_t prev_idle = 0, prev_total = 0;
    uint64_t idle, total;
    if (!dk_cpu_ticks(&idle, &total)) return make_number(0.0);
    if (prev_total == 0) { prev_idle = idle; prev_total = total; return make_number(0.0); }
    uint64_t idle_diff = idle - prev_idle, total_diff = total - prev_total;
    prev_idle = idle; prev_total = total;
    if (total_diff == 0) return make_number(0.0);
    double usage = 100.0 - ((double)idle_diff / (double)total_diff * 100.0);
    return make_number(usage < 0.0 ? 0.0 : (usage > 100.0 ? 100.0 : usage));
}
#endif

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

#ifdef _WIN32
static void dk_ws_init(void) {
    if (g_socks_init) return;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    for (int i = 0; i < DK_MAX_SOCK; i++) g_socks[i] = 0;
    g_socks_init = 1;
}
#else
static void dk_ws_init(void) {
    if (g_socks_init) return;
    signal(SIGPIPE, SIG_IGN);   // send() to a closed peer must return -1, not kill the process
    for (int i = 0; i < DK_MAX_SOCK; i++) g_socks[i] = 0;
    g_socks_init = 1;
}
#endif

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

#ifdef _WIN32
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
#else
// ---- libcurl helpers (shared by http_get and http_request) ----
typedef struct { char* b; size_t n; } DkCurlBuf;

static size_t dk_curl_write_cb(char* p, size_t sz, size_t nm, void* ud) {
    DkCurlBuf* cb = (DkCurlBuf*)ud;
    size_t add = sz * nm;
    char* nb = (char*)realloc(cb->b, cb->n + add + 1);
    if (!nb) return 0;                       // abort transfer
    memcpy(nb + cb->n, p, add);
    cb->n += add; nb[cb->n] = 0; cb->b = nb;
    return add;
}

static pthread_once_t dk_curl_once = PTHREAD_ONCE_INIT;
static void dk_curl_global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static CURL* dk_curl_new(const char* url, DkCurlBuf* cb) {
    pthread_once(&dk_curl_once, dk_curl_global_init);
    CURL* h = curl_easy_init();
    if (!h) return NULL;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Donky/1.0");
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);      // WinHTTP follows redirects by default
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 60L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);            // required when used from multiple threads
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, dk_curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, cb);
    return h;
}

uint64_t donky_http_get(uint64_t url_val) {
    if (get_tag(url_val) != TAG_STRING) return donky_alloc_string("");
    DkCurlBuf cb = { NULL, 0 };
    CURL* h = dk_curl_new((char*)as_index(url_val), &cb);
    if (!h) return donky_alloc_string("");
    curl_easy_perform(h);                                 // body (if any) is returned regardless of HTTP status
    curl_easy_cleanup(h);
    uint64_t result = donky_alloc_string(cb.b ? cb.b : "");
    free(cb.b);
    return result;
}
#endif

// --- Argument & Thread Runtime ---
uint64_t donky_args_array = 0;
void donky_set_args(int argc, char** argv) {
    uint64_t arr_val = donky_alloc_array();
    for (int i = 1; i < argc; i++) donky_array_push(arr_val, donky_alloc_string(argv[i]));
    donky_args_array = arr_val;
}
uint64_t donky_get_args() { return donky_args_array; }

typedef struct ThreadArgs {
    uint64_t func_ptr;
    uint64_t arg1;
    struct ThreadArgs* next;
} ThreadArgs;

#define DK_POOL_SIZE   16
#define DK_QUEUE_MAX   100000

static CRITICAL_SECTION   dk_pool_lock;
static CONDITION_VARIABLE dk_pool_cv;
static ThreadArgs* dk_q_head = NULL;
static ThreadArgs* dk_q_tail = NULL;
static int dk_q_len = 0;
static int dk_pool_state = 0;

#ifdef _WIN32
static unsigned __stdcall dk_pool_worker(void* unused) {
#else
static void* dk_pool_worker(void* unused) {
#endif
    (void)unused;
    typedef uint64_t (*DonkyFn)(uint64_t);
    for (;;) {
        EnterCriticalSection(&dk_pool_lock);
        while (dk_q_head == NULL)
            SleepConditionVariableCS(&dk_pool_cv, &dk_pool_lock, INFINITE);
        ThreadArgs* t = dk_q_head;
        dk_q_head = t->next;
        if (dk_q_head == NULL) dk_q_tail = NULL;
        dk_q_len--;
        LeaveCriticalSection(&dk_pool_lock);

        DonkyFn fn = (DonkyFn)(uintptr_t)t->func_ptr;
        fn(t->arg1);
        free(t);

        // Uncaught exception in a task: report it and reset this worker's slot, otherwise
        // every later call on this thread would see a stale exception and bail out.
        if (current_exception != 0) {
            fputs("Uncaught exception in spawned task: ", stderr);
            fflush(stdout); print_value(current_exception); fflush(stdout);
            fputc('\n', stderr);
            current_exception = 0;
        }

        EnterCriticalSection(&dk_pool_lock);
        active_threads--;
        LeaveCriticalSection(&dk_pool_lock);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int dk_pool_init(void) {
    InitializeCriticalSection(&dk_pool_lock);
    InitializeConditionVariable(&dk_pool_cv);

    int made = 0;
    for (int i = 0; i < DK_POOL_SIZE; i++) {
#ifdef _WIN32
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, dk_pool_worker, NULL, 0, NULL);
        if (h) { CloseHandle(h); made++; }
#else
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t tid;
        if (pthread_create(&tid, &attr, dk_pool_worker, NULL) == 0) made++;
        pthread_attr_destroy(&attr);
#endif
    }
    dk_pool_state = made > 0 ? 1 : -1;
    return made > 0;
}

uint64_t donky_spawn(uint64_t func_val, uint64_t arg1) {
    if (dk_pool_state == 0) dk_pool_init();
    if (dk_pool_state < 0) return make_number(-1.0);

    ThreadArgs* t = (ThreadArgs*)malloc(sizeof(ThreadArgs));
    if (!t) return make_number(-1.0);
    t->func_ptr = func_val;
    t->arg1 = arg1;
    t->next = NULL;

    EnterCriticalSection(&dk_pool_lock);
    if (dk_q_len >= DK_QUEUE_MAX) {
        LeaveCriticalSection(&dk_pool_lock);
        free(t);
        return make_number(-1.0);
    }
    if (dk_q_tail) dk_q_tail->next = t; else dk_q_head = t;
    dk_q_tail = t;
    dk_q_len++;
    active_threads++;
    LeaveCriticalSection(&dk_pool_lock);

    WakeConditionVariable(&dk_pool_cv);
    return make_number(0.0);
}

#ifdef _WIN32
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
#else
// --- GUI subsystem: the GDI window API is Windows-only. Stubs keep every symbol linkable. ---
static int dk_gui_warned = 0;
static void dk_gui_unsupported(void) {
    if (!dk_gui_warned) { dk_gui_warned = 1; fprintf(stderr, "[Donky] gui_* is only available on Windows; use gui_webview(url) for a cross-platform UI.\n"); }
}
uint64_t donky_gui_init(uint64_t title_val, uint64_t width_val, uint64_t height_val) { (void)title_val; (void)width_val; (void)height_val; dk_gui_unsupported(); return make_number(0.0); }
uint64_t donky_gui_add_button(uint64_t text_val, uint64_t x, uint64_t y, uint64_t w, uint64_t h) { (void)text_val; (void)x; (void)y; (void)w; (void)h; return make_number(-1.0); }
uint64_t donky_gui_add_label(uint64_t text_val, uint64_t x, uint64_t y, uint64_t w, uint64_t h) { (void)text_val; (void)x; (void)y; (void)w; (void)h; return make_number(-1.0); }
uint64_t donky_gui_update_text(uint64_t handle_val, uint64_t text_val) { (void)handle_val; (void)text_val; return 0; }
uint64_t donky_gui_get_clicked() { return make_number(-1.0); }
uint64_t donky_gui_clear() { return 0; }
uint64_t donky_gui_draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, uint64_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; return 0; }
uint64_t donky_gui_draw_text(uint64_t text_val, uint64_t x, uint64_t y) { (void)text_val; (void)x; (void)y; return 0; }
uint64_t donky_gui_repaint() { return 0; }
uint64_t donky_gui_update() { return make_number(0.0); }

// --- Native WebView Window: open the default browser, then keep the process alive ---
uint64_t donky_gui_webview(uint64_t url_val) {
    if (get_tag(url_val) != TAG_STRING) return make_number(0.0);
    char* url = (char*)as_index(url_val);

    printf("[Donky] Server active. Opening UI at %s\n", url);
    fflush(stdout);

#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid == 0) {                                   // child: double-fork so nothing is left as a zombie
        if (fork() == 0) {
            int dn = open("/dev/null", O_RDWR);
            if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
            execlp(opener, opener, url, (char*)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        int st; waitpid(pid, &st, 0);
    }

    // Keep the main process thread alive so spawned server threads stay active
    while (1) {
        Sleep(100);
    }
    return make_number(1.0);
}
#endif

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
    if (!g_globals) return TAG_BASE;                    /* B2: never set */
    return donky_index_get(g_globals, k);               /* TAG_BASE on miss */
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

// ---- raw memory for FFI (caller must ffi_free what it ffi_alloc'd) ----
uint64_t donky_ffi_alloc(uint64_t n) {
    size_t sz = is_number(n) ? (size_t)as_number(n) : 0;
    void* p = calloc(1, sz ? sz : 1);
    return make_number((double)(uint64_t)(uintptr_t)p);
}
uint64_t donky_ffi_free(uint64_t p) {
    if (is_number(p)) free((void*)(uintptr_t)(uint64_t)as_number(p));
    return 0;
}
uint64_t donky_ffi_peek(uint64_t p, uint64_t size) {
    if (!is_number(p) || !is_number(size)) return make_number(0.0);
    const void* a = (const void*)(uintptr_t)(uint64_t)as_number(p);
    if (!a) return make_number(0.0);
    switch ((int)as_number(size)) {
        case 1: { uint8_t  v; memcpy(&v, a, 1); return make_number((double)v); }
        case 2: { uint16_t v; memcpy(&v, a, 2); return make_number((double)v); }
        case 4: { uint32_t v; memcpy(&v, a, 4); return make_number((double)v); }
        case 8: { uint64_t v; memcpy(&v, a, 8); return make_number((double)v); }
    }
    return make_number(0.0);
}
uint64_t donky_ffi_poke(uint64_t p, uint64_t size, uint64_t val) {
    if (!is_number(p) || !is_number(size) || !is_number(val)) return 0;
    void* a = (void*)(uintptr_t)(uint64_t)as_number(p);
    if (!a) return 0;
    uint64_t v = (uint64_t)(int64_t)as_number(val);
    int n = (int)as_number(size);
    if (n == 1 || n == 2 || n == 4 || n == 8) memcpy(a, &v, (size_t)n); // little-endian
    return 0;
}

// ---- os ----
#ifdef _WIN32
uint64_t donky_sys_cwd(void) {
    char buf[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryA(sizeof buf, buf);
    if (n == 0 || n >= sizeof buf) return donky_alloc_string("");
    return donky_alloc_string(buf);
}
uint64_t donky_sys_chdir(uint64_t p) {
    if (!dk_is_str(p)) return BOOL_FALSE_BITS;
    return SetCurrentDirectoryA(dk_s(p)) ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
// returns null (0) if the variable does not exist
uint64_t donky_sys_getenv(uint64_t name) {
    if (!dk_is_str(name)) return TAG_BASE;              /* B2 */
    SetLastError(0);
    DWORD n = GetEnvironmentVariableA(dk_s(name), NULL, 0);
    if (n == 0) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? TAG_BASE : donky_alloc_string("");  /* B2 */
    char* buf = (char*)malloc(n + 1);
    DWORD m = GetEnvironmentVariableA(dk_s(name), buf, n + 1);
    buf[m < n + 1 ? m : n] = 0;
    uint64_t r = donky_alloc_string(buf);
    free(buf);
    return r;
}
uint64_t donky_sys_setenv(uint64_t name, uint64_t val) {
    if (!dk_is_str(name) || !dk_is_str(val)) return BOOL_FALSE_BITS;
    return SetEnvironmentVariableA(dk_s(name), dk_s(val)) ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
uint64_t donky_sys_remove(uint64_t p) {
    if (!dk_is_str(p)) return BOOL_FALSE_BITS;
    return DeleteFileA(dk_s(p)) ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
uint64_t donky_sys_rename(uint64_t a, uint64_t b) {
    if (!dk_is_str(a) || !dk_is_str(b)) return BOOL_FALSE_BITS;
    return MoveFileExA(dk_s(a), dk_s(b), MOVEFILE_REPLACE_EXISTING) ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
#else
uint64_t donky_sys_cwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return donky_alloc_string("");
    return donky_alloc_string(buf);
}
uint64_t donky_sys_chdir(uint64_t p) {
    if (!dk_is_str(p)) return BOOL_FALSE_BITS;
    return chdir(dk_s(p)) == 0 ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
// returns null (0) if the variable does not exist
uint64_t donky_sys_getenv(uint64_t name) {
    if (!dk_is_str(name)) return TAG_BASE;              /* B2 */
    const char* v = getenv(dk_s(name));
    if (!v) return TAG_BASE;                            /* B2: unset is null */
    return donky_alloc_string(v);
}
uint64_t donky_sys_setenv(uint64_t name, uint64_t val) {
    if (!dk_is_str(name) || !dk_is_str(val)) return BOOL_FALSE_BITS;
    return setenv(dk_s(name), dk_s(val), 1) == 0 ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
uint64_t donky_sys_remove(uint64_t p) {
    if (!dk_is_str(p)) return BOOL_FALSE_BITS;
    return unlink(dk_s(p)) == 0 ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}
uint64_t donky_sys_rename(uint64_t a, uint64_t b) {
    if (!dk_is_str(a) || !dk_is_str(b)) return BOOL_FALSE_BITS;
    return rename(dk_s(a), dk_s(b)) == 0 ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;   // POSIX rename() replaces the target
}
#endif
uint64_t donky_sys_append_file(uint64_t p, uint64_t c) {
    if (!dk_is_str(p) || !dk_is_str(c)) return BOOL_FALSE_BITS;
    FILE* f = fopen(dk_s(p), "ab");
    if (!f) return BOOL_FALSE_BITS;
    fwrite(dk_s(c), 1, str_len(dk_s(c)), f);
    fclose(f);
    return BOOL_TRUE_BITS;
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

#ifdef _WIN32
uint64_t donky_sys_epoch_ms(void) {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return make_number((double)((u.QuadPart - 116444736000000000ULL) / 10000ULL));
}
#else
uint64_t donky_sys_epoch_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return make_number((double)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L)));
}
#endif

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

#ifdef _WIN32
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
#else
uint64_t donky_http_request(uint64_t method_val, uint64_t url_val, uint64_t body_val, uint64_t hdr_val) {
    dk_http_status = 0;
    if (!dk_is_str(method_val) || !dk_is_str(url_val)) return donky_alloc_string("");
    const char* method = dk_s(method_val);
    const char* body = dk_is_str(body_val) ? dk_s(body_val) : "";
    const char* hdrs = dk_is_str(hdr_val) ? dk_s(hdr_val) : "";
    size_t blen = strlen(body);

    DkCurlBuf cb = { NULL, 0 };
    CURL* h = dk_curl_new(dk_s(url_val), &cb);
    if (!h) return donky_alloc_string("");

    if (strcasecmp(method, "HEAD") == 0) curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    else                                 curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);

    // "Name: Value\r\nName2: Value2\r\n" -> curl_slist
    struct curl_slist* hl = NULL;
    if (hdrs[0]) {
        char* copy = strdup(hdrs);
        if (copy) {
            char* sp = NULL;
            for (char* ln = strtok_r(copy, "\r\n", &sp); ln; ln = strtok_r(NULL, "\r\n", &sp))
                if (*ln) hl = curl_slist_append(hl, ln);
            free(copy);
        }
    }
    if (blen) {
        // WinHTTP adds no Content-Type of its own; stop curl from inventing x-www-form-urlencoded
        if (!hdrs[0] || !strcasestr(hdrs, "content-type:")) hl = curl_slist_append(hl, "Content-Type:");
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)blen);
    }
    hl = curl_slist_append(hl, "Expect:");               // never send 100-continue
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);

    CURLcode rc = curl_easy_perform(h);
    if (rc == CURLE_OK) {
        long st = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &st);
        dk_http_status = (int)st;
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(h);

    uint64_t r = donky_alloc_string(cb.b ? cb.b : "");
    free(cb.b);
    return r;
}
#endif
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