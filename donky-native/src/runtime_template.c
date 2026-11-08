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
#include <ffi.h>
#ifdef _WIN32
#include <conio.h>

#define _WIN32_WINNT 0x0602

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
#include <pwd.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sched.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <sys/syscall.h>
#endif
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
#define TAG_FUNCTION 6ULL  

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
typedef char dk_tag_check_FUNC  [(TAG_FUNCTION == 6ULL) ? 1 : -1];
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
    } else if (tag == TAG_FUNCTION) {
        dk_rput(out, cap, pos, "<function>", 10);
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

// --- First-class functions (TAG_FUNCTION) ---
// A function value is a heap object whose 8-byte payload is the raw code address. It owns no
// children and needs no finalizer; gc_mark / gc_valid_object just have to accept the tag.
uint64_t donky_alloc_function(uint64_t code_addr) {
    uint64_t* cell = (uint64_t*)gc_malloc(sizeof(uint64_t));
    *cell = code_addr;
    GCHeader* h = ((GCHeader*)cell) - 1;
    h->type_tag = TAG_FUNCTION;
    return TAG_BASE | (TAG_FUNCTION << 48) | (uint64_t)(uintptr_t)cell;
}

// Raw code address of a function value; any other word is returned unchanged, so code that used
// to receive a raw address (spawn, dll_callback) accepts both forms.
static inline uint64_t dk_fn_raw(uint64_t v) {
    if (!is_number(v) && get_tag(v) == TAG_FUNCTION) return *(uint64_t*)as_index(v);
    return v;
}

typedef uint64_t (*DkFuncFn)(uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t, uint64_t);
#define DK_CALL_MAX_ARGS 8

// call_function(f, args): args is an array of up to 8 values (or null for none). Missing
// arguments are 0.0, the same padding aot.rs's pad_args uses, so a callee of any arity <= 8 can be
// called through the 8-slot pointer (the caller cleans the stack on every supported ABI). If the
// callee left an exception pending this returns null and leaves it pending for the caller's
// has_exception check.
uint64_t donky_call_function(uint64_t func_val, uint64_t args_array_val) {
    if (is_number(func_val) || get_tag(func_val) != TAG_FUNCTION) {
        donky_throw(donky_alloc_string("call_function: not a function"));
        return TAG_BASE;
    }
    uint64_t addr = *(uint64_t*)as_index(func_val);
    if (!addr) {
        donky_throw(donky_alloc_string("call_function: null function address"));
        return TAG_BASE;
    }
    uint64_t a[DK_CALL_MAX_ARGS] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (args_array_val != TAG_BASE) {
        if (is_number(args_array_val) || get_tag(args_array_val) != TAG_ARRAY) {
            donky_throw(donky_alloc_string("call_function: args must be an array (or null)"));
            return TAG_BASE;
        }
        Array* arr = (Array*)as_index(args_array_val);
        if (arr->len > DK_CALL_MAX_ARGS) {
            donky_throw(donky_alloc_string("call_function: too many arguments (max 8)"));
            return TAG_BASE;
        }
        for (size_t i = 0; i < arr->len; i++) a[i] = arr->data[i];
    }
    uint64_t r = ((DkFuncFn)(uintptr_t)addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if (donky_has_exception()) return TAG_BASE;
    return r;
}

// --- Garbage Collection Mark & Sweep ---
void gc_mark(uint64_t val) {
    if (is_number(val)) return; 
    uint64_t tag = get_tag(val);
    void* ptr = (void*)as_index(val);
    if (tag != TAG_STRING && tag != TAG_ARRAY && tag != TAG_MAP && tag != TAG_FUNCTION) return;
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
    if (tag != TAG_STRING && tag != TAG_ARRAY && tag != TAG_MAP && tag != TAG_FUNCTION) return 0;
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
    if (tag == TAG_FUNCTION) return "function";
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
    double da = as_number(a);
    if (db == 0.0) return make_number(da > 0.0 ? 1.0/0.0 : (da < 0.0 ? -1.0/0.0 : 0.0/0.0)); // IEEE inf/nan
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

// A handle is a Donky number (dll_call_sig 'p' returns pointers/HANDLEs as numbers), so compare its
// integer value, not the raw NaN-box word: INVALID_HANDLE_VALUE (-1 / 0xFFFFFFFFFFFFFFFF) arrives
// as -1 or 2^64 and the old bit test (word == ~0) never matched it. Fractions, NaN and
// non-numbers are not handles.
static int dk_handle_bits(uint64_t v, uint64_t* out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (d != d || d != floor(d)) return 0;
    if (d >= 18446744073709551615.0)       *out = UINT64_MAX;               // 2^64: an all-ones pointer
    else if (d >= 9223372036854775808.0)   *out = (uint64_t)d;
    else if (d <= -9223372036854775808.0)  *out = (uint64_t)INT64_MIN;
    else                                   *out = (uint64_t)(int64_t)d;     // negatives wrap: -1 -> ~0
    return 1;
}

// true for null and for the number 0 (a NULL pointer / NULL handle); everything else is false.
uint64_t donky_is_null_handle(uint64_t handle_val) {
    uint64_t bits;
    if (handle_val == TAG_BASE) return BOOL_TRUE_BITS;
    if (dk_handle_bits(handle_val, &bits) && bits == 0) return BOOL_TRUE_BITS;
    return BOOL_FALSE_BITS;
}

// true for a number that is neither 0 (NULL) nor -1 / 0xFFFFFFFFFFFFFFFF (INVALID_HANDLE_VALUE).
uint64_t donky_is_valid_handle(uint64_t handle_val) {
        uint64_t bits;
    if (!dk_handle_bits(handle_val, &bits) || bits == UINT64_MAX || bits == 0) {
        return BOOL_FALSE_BITS; 
    }
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

// donky_secure_random(n): CSPRNG, returns n whole bytes as a Donky array of numbers 0-255.
// Windows: BCryptGenRandom. POSIX: getrandom(2) on Linux, arc4random_buf on macOS/BSD.
// This is for MAC spoofing, hostname entropy, and any security-sensitive randomness.
// Do NOT use donky_random() (rand()-backed) for these.
#ifdef _WIN32
#pragma comment(lib, "bcrypt.lib")
#include <bcrypt.h>
uint64_t donky_secure_random(uint64_t n_val) {
    if (!is_number(n_val)) { donky_throw(donky_alloc_string("secure_random: n must be a number")); return TAG_BASE; }
    double d = as_number(n_val);
    if (!(d >= 0.0) || d > 1048576.0 || d != floor(d)) {
        donky_throw(donky_alloc_string("secure_random: n must be a whole number in [0, 1048576]"));
        return TAG_BASE;
    }
    size_t n = (size_t)d;
    uint64_t arr = donky_alloc_array();
    if (n == 0) return arr;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { donky_throw(donky_alloc_string("secure_random: out of memory")); return TAG_BASE; }
    NTSTATUS st = BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        free(buf);
        donky_throw(donky_alloc_string("secure_random: BCryptGenRandom failed"));
        return TAG_BASE;
    }
    for (size_t i = 0; i < n; i++) donky_array_push(arr, make_number((double)buf[i]));
    free(buf);
    return arr;
}
#else
#include <sys/random.h>
uint64_t donky_secure_random(uint64_t n_val) {
    if (!is_number(n_val)) { donky_throw(donky_alloc_string("secure_random: n must be a number")); return TAG_BASE; }
    double d = as_number(n_val);
    if (!(d >= 0.0) || d > 1048576.0 || d != floor(d)) {
        donky_throw(donky_alloc_string("secure_random: n must be a whole number in [0, 1048576]"));
        return TAG_BASE;
    }
    size_t n = (size_t)d;
    uint64_t arr = donky_alloc_array();
    if (n == 0) return arr;
    uint8_t* buf = (uint8_t*)malloc(n);
    if (!buf) { donky_throw(donky_alloc_string("secure_random: out of memory")); return TAG_BASE; }
#ifdef __APPLE__
    arc4random_buf(buf, n);
#else
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(buf + got, n - got, 0);
        if (r <= 0) {
            free(buf);
            donky_throw(donky_alloc_string("secure_random: getrandom failed"));
            return TAG_BASE;
        }
        got += (size_t)r;
    }
#endif
    for (size_t i = 0; i < n; i++) donky_array_push(arr, make_number((double)buf[i]));
    free(buf);
    return arr;
}
#endif

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
uint64_t donky_sin(uint64_t a) {
    if (!is_number(a)) { donky_throw(donky_alloc_string("sin: expected a number")); return TAG_BASE; }
    return make_number(sin(as_number(a)));
}
uint64_t donky_cos(uint64_t a) {
    if (!is_number(a)) { donky_throw(donky_alloc_string("cos: expected a number")); return TAG_BASE; }
    return make_number(cos(as_number(a)));
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

// --- libffi-backed typed FFI (donky_dll_call_sig / donky_dll_callback) ---
// Type chars:  v void   i int32   u uint32   l int64   L uint64   f float   d double
//              s char* (Donky string or null)   p pointer-as-number
// Numbers cross the boundary as doubles, so 'l'/'L' values above 2^53 lose precision and
// pointers must fit in 48 bits (true for user-space pointers on x64/arm64).
#define DK_FFI_MAX_ARGS 32     // dll_call_sig
#define DK_CB_MAX_ARGS   8     // dll_callback

typedef union {
    int32_t i; uint32_t u; int64_t l; uint64_t L;
    float f; double d; void* p;
    ffi_arg a; ffi_sarg sa;     // widened integer returns (libffi writes a full ffi_arg)
} DkFfiSlot;

static uint64_t dk_ffi_err(const char* msg) {
    donky_throw(donky_alloc_string(msg));
    return TAG_BASE;
}

static ffi_type* dk_ffi_type_of(char c) {
    switch (c) {
        case 'v': return &ffi_type_void;
        case 'i': return &ffi_type_sint32;
        case 'u': return &ffi_type_uint32;
        case 'l': return &ffi_type_sint64;
        case 'L': return &ffi_type_uint64;
        case 'f': return &ffi_type_float;
        case 'd': return &ffi_type_double;
        case 's': case 'p': return &ffi_type_pointer;
        default:  return NULL;
    }
}

// number/bool -> int64, saturating (no UB on NaN / out-of-range doubles). Anything else -> 0.
static int64_t dk_ffi_to_i64(uint64_t v) {
    if (is_number(v)) {
        double d = as_number(v);
        if (d != d) return 0;
        if (d >= 9223372036854775807.0)  return INT64_MAX;
        if (d <= -9223372036854775808.0) return INT64_MIN;
        return (int64_t)d;
    }
    if (get_tag(v) == TAG_BOOL) return (int64_t)(v & 1);
    return 0;
}

// number/bool -> uint64. Negative numbers wrap (two's complement), so -1 -> 0xFFFF...FFFF.
static uint64_t dk_ffi_to_u64(uint64_t v) {
    if (is_number(v)) {
        double d = as_number(v);
        if (d >= 18446744073709551615.0) return UINT64_MAX;
        if (d >= 9223372036854775808.0)  return (uint64_t)d;
    }
    return (uint64_t)dk_ffi_to_i64(v);
}

static int dk_ffi_is_numeric(uint64_t v) { return is_number(v) || get_tag(v) == TAG_BOOL; }

// 'p' accepts a number (address), a string (its char*), a bool (0/1) or null (NULL).
static void* dk_ffi_to_ptr(uint64_t v) {
    if (is_number(v))               return (void*)(uintptr_t)dk_ffi_to_u64(v);
    if (get_tag(v) == TAG_STRING)   return (void*)(uintptr_t)as_index(v);
    if (get_tag(v) == TAG_BOOL)     return (void*)(uintptr_t)(v & 1);
    return NULL;
}

// Formats "<who>: ..." into a 256-byte buffer and throws it (used by the FFI entry points).
#define DK_FFI_FAIL(...) do { char dk_fail[256]; snprintf(dk_fail, sizeof dk_fail, __VA_ARGS__); return dk_ffi_err(dk_fail); } while (0)

// Shared by dll_call_sig (nfixed < 0) and dll_call_var_sig (nfixed = number of named parameters of
// the variadic callee; arg_types / args then hold the fixed arguments followed by the variadic ones).
// `who` prefixes every error. Errors name the types involved ("arg 0 expects string, got number").
static uint64_t dk_ffi_call_core(const char* who, int nfixed, uint64_t fn_val, uint64_t ret_type_val, uint64_t arg_types_val, uint64_t args_val) {
    // Keep the args array (and the strings it owns) visible to the conservative GC stack scan
    // for the whole call: a callback may allocate, and the compiler could otherwise drop args_val.
    volatile uint64_t gc_keep = args_val;
    char msg[96];

    if (!is_number(fn_val)) DK_FFI_FAIL("%s: function must be a pointer from dll_sym or dll_callback, got %s", who, dk_type_name(fn_val));
    uintptr_t fnaddr = (uintptr_t)dk_ffi_to_u64(fn_val);
    if (!fnaddr) DK_FFI_FAIL("%s: null function pointer", who);

    if (get_tag(ret_type_val) != TAG_STRING || strlen((const char*)as_index(ret_type_val)) != 1)
        DK_FFI_FAIL("%s: return type must be a 1-char string (v i u l L f d s p), got %s", who,
                    get_tag(ret_type_val) == TAG_STRING ? "a string of another length" : dk_type_name(ret_type_val));
    char rt = ((const char*)as_index(ret_type_val))[0];
    ffi_type* rtype = dk_ffi_type_of(rt);
    if (!rtype) DK_FFI_FAIL("%s: bad return type '%c'", who, rt);

    const char* at = "";                                   // null arg-type string = no arguments
    if (get_tag(arg_types_val) == TAG_STRING) at = (const char*)as_index(arg_types_val);
    else if (arg_types_val != TAG_BASE) DK_FFI_FAIL("%s: arg types must be a string (or null for none), got %s", who, dk_type_name(arg_types_val));
    size_t n = strlen(at);
    if (n > DK_FFI_MAX_ARGS) DK_FFI_FAIL("%s: too many arguments (max 32)", who);

    Array* arr = NULL;
    size_t have = 0;
    if (get_tag(args_val) == TAG_ARRAY) { arr = (Array*)as_index(args_val); have = arr->len; }
    else if (n != 0) DK_FFI_FAIL("%s: args must be an array, got %s", who, dk_type_name(args_val));
    if (have != n) {
        snprintf(msg, sizeof msg, "%s: signature has %zu args but %zu were given", who, n, have);
        return dk_ffi_err(msg);
    }

    ffi_type* atypes[DK_FFI_MAX_ARGS + 1];
    void*     aptrs[DK_FFI_MAX_ARGS + 1];
    DkFfiSlot slots[DK_FFI_MAX_ARGS + 1];
    for (size_t i = 0; i < n; i++) {
        char t = at[i];
        uint64_t v = arr->data[i];
        memset(&slots[i], 0, sizeof slots[i]);
        atypes[i] = (t == 'v') ? NULL : dk_ffi_type_of(t);
        if (!atypes[i]) DK_FFI_FAIL("%s: bad arg type '%c' at position %zu", who, t, i);
        if (nfixed >= 0 && i >= (size_t)nfixed && t == 'f')
            DK_FFI_FAIL("%s: arg %zu: 'f' is not allowed in the variadic part (C promotes float to double); use 'd'", who, i);
        switch (t) {
            case 'i': case 'u': case 'l': case 'L': case 'f': case 'd':
                if (!dk_ffi_is_numeric(v)) {
                    snprintf(msg, sizeof msg, "%s: arg %zu expects number, got %s", who, i, dk_type_name(v));
                    return dk_ffi_err(msg);
                }
                if      (t == 'i') slots[i].i = (int32_t)dk_ffi_to_i64(v);
                else if (t == 'u') slots[i].u = (uint32_t)dk_ffi_to_u64(v);
                else if (t == 'l') slots[i].l = dk_ffi_to_i64(v);
                else if (t == 'L') slots[i].L = dk_ffi_to_u64(v);
                else if (t == 'f') slots[i].f = is_number(v) ? (float)as_number(v) : (float)(v & 1);
                else               slots[i].d = is_number(v) ? as_number(v) : (double)(v & 1);
                break;
            case 's':
                if (get_tag(v) == TAG_STRING) slots[i].p = (void*)(uintptr_t)as_index(v);
                else if (v == TAG_BASE)       slots[i].p = NULL;
                else {
                    snprintf(msg, sizeof msg, "%s: arg %zu expects string, got %s", who, i, dk_type_name(v));
                    return dk_ffi_err(msg);
                }
                break;
            case 'p':
                slots[i].p = dk_ffi_to_ptr(v);
                break;
        }
        aptrs[i] = &slots[i];
    }

    ffi_cif cif;
    if ((nfixed < 0 ? ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)n, rtype, atypes)
                    : ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, (unsigned)nfixed, (unsigned)n, rtype, atypes)) != FFI_OK)
        return dk_ffi_err(nfixed < 0 ? "dll_call_sig: ffi_prep_cif failed" : "dll_call_var_sig: ffi_prep_cif_var failed");

    DkFfiSlot rv;
    memset(&rv, 0, sizeof rv);
    ffi_call(&cif, (void (*)(void))fnaddr, &rv, aptrs);
    (void)gc_keep;

    switch (rt) {
        case 'v': return TAG_BASE;
        case 'i': return make_number((double)(int32_t)rv.sa);
        case 'u': return make_number((double)(uint32_t)rv.a);
        case 'l': return make_number((double)rv.l);
        case 'L': return make_number((double)rv.L);
        case 'f': return make_number((double)rv.f);
        case 'd': return make_number(rv.d);
        case 's': return rv.p ? donky_alloc_string((const char*)rv.p) : TAG_BASE;
        case 'p': return make_number((double)(uint64_t)(uintptr_t)rv.p);
    }
    return TAG_BASE;
}

uint64_t donky_dll_call_sig(uint64_t fn_val, uint64_t ret_type_val, uint64_t arg_types_val, uint64_t args_val) {
    return dk_ffi_call_core("dll_call_sig", -1, fn_val, ret_type_val, arg_types_val, args_val);
}

// dll_call_var_sig(fn, ret, fixed_types, var_types, fixed_args, var_args): call a C variadic function
// (printf, snprintf, open, ioctl, ...) through ffi_prep_cif_var, which the plain dll_call_sig cannot do
// correctly (variadic calls use a different ABI on x86-64 SysV and on Apple arm64).
//   fixed_types / fixed_args  the named parameters, e.g. "pLs" and [buf, size, fmt]
//   var_types   / var_args    the "..." part, e.g. "isd" and [42, "hi", 3.5]
// Type chars are the dll_call_sig ones, except 'f' is rejected in the variadic part (C passes float
// as double there: use 'd'). A null types string / null args means none. Error positions count the
// fixed arguments first (the first variadic argument is arg <number of fixed args>).
uint64_t donky_dll_call_var_sig(uint64_t fn_val, uint64_t ret_type_val, uint64_t fixed_types_val,
                                uint64_t var_types_val, uint64_t fixed_args_val, uint64_t var_args_val) {
    volatile uint64_t gc_keep[2] = { fixed_args_val, var_args_val };   // keep both arrays visible to the GC scan
    const char* ft = "";
    const char* vt = "";
    if (get_tag(fixed_types_val) == TAG_STRING) ft = (const char*)as_index(fixed_types_val);
    else if (fixed_types_val != TAG_BASE) DK_FFI_FAIL("dll_call_var_sig: fixed types must be a string (or null for none), got %s", dk_type_name(fixed_types_val));
    if (get_tag(var_types_val) == TAG_STRING) vt = (const char*)as_index(var_types_val);
    else if (var_types_val != TAG_BASE) DK_FFI_FAIL("dll_call_var_sig: variadic types must be a string (or null for none), got %s", dk_type_name(var_types_val));
    size_t nf = strlen(ft), nv = strlen(vt);
    if (nf + nv > DK_FFI_MAX_ARGS) DK_FFI_FAIL("dll_call_var_sig: too many arguments (max 32)");

    Array* fa = NULL; Array* va = NULL;
    size_t nfa = 0, nva = 0;
    if (get_tag(fixed_args_val) == TAG_ARRAY) { fa = (Array*)as_index(fixed_args_val); nfa = fa->len; }
    else if (fixed_args_val != TAG_BASE) DK_FFI_FAIL("dll_call_var_sig: fixed args must be an array (or null), got %s", dk_type_name(fixed_args_val));
    if (get_tag(var_args_val) == TAG_ARRAY) { va = (Array*)as_index(var_args_val); nva = va->len; }
    else if (var_args_val != TAG_BASE) DK_FFI_FAIL("dll_call_var_sig: variadic args must be an array (or null), got %s", dk_type_name(var_args_val));
    if (nfa != nf) DK_FFI_FAIL("dll_call_var_sig: fixed signature has %zu args but %zu were given", nf, nfa);
    if (nva != nv) DK_FFI_FAIL("dll_call_var_sig: variadic signature has %zu args but %zu were given", nv, nva);

    char types[DK_FFI_MAX_ARGS + 1];
    memcpy(types, ft, nf);
    memcpy(types + nf, vt, nv);
    types[nf + nv] = 0;
    uint64_t all = donky_alloc_array();
    for (size_t i = 0; i < nfa; i++) donky_array_push(all, fa->data[i]);
    for (size_t i = 0; i < nva; i++) donky_array_push(all, va->data[i]);
    uint64_t r = dk_ffi_call_core("dll_call_var_sig", (int)nf, fn_val, ret_type_val, donky_alloc_string(types), all);
    (void)gc_keep;
    return r;
}
#undef DK_FFI_FAIL

// ---- callbacks: Donky function -> C function pointer ----
typedef struct DkCallback {
    ffi_cif   cif;
    ffi_type* atypes[DK_CB_MAX_ARGS];
    void*     code;                       // the C-callable entry point handed to the library
    uint64_t  fn;                         // raw Donky code address (AOT convention, as in donky_spawn)
    int       nargs;
    char      rt;
    char      at[DK_CB_MAX_ARGS + 1];
} DkCallback;

typedef uint64_t (*DkCbFn)(uint64_t, uint64_t, uint64_t, uint64_t,
                           uint64_t, uint64_t, uint64_t, uint64_t);

static uint64_t dk_ffi_arg_to_donky(char t, void* p) {
    switch (t) {
        case 'i': return make_number((double)*(int32_t*)p);
        case 'u': return make_number((double)*(uint32_t*)p);
        case 'l': return make_number((double)*(int64_t*)p);
        case 'L': return make_number((double)*(uint64_t*)p);
        case 'f': return make_number((double)*(float*)p);
        case 'd': return make_number(*(double*)p);
        case 's': { const char* s = *(const char**)p; return s ? donky_alloc_string(s) : TAG_BASE; }
        case 'p': return make_number((double)(uint64_t)(uintptr_t)*(void**)p);
    }
    return TAG_BASE;
}

// Called by libffi on the C caller's thread, inside the C caller's frame.
static void dk_ffi_trampoline(ffi_cif* cif, void* ret, void** args, void* userdata) {
    (void)cif;
    DkCallback* cb = (DkCallback*)userdata;
    if (cb->rt != 'v') memset(ret, 0, sizeof(ffi_arg));      // C sees 0 if we bail out below
    if (donky_has_exception()) return;                       // an earlier call already threw: don't run more Donky code

    uint64_t a[DK_CB_MAX_ARGS];
    for (int i = 0; i < DK_CB_MAX_ARGS; i++)
        a[i] = i < cb->nargs ? dk_ffi_arg_to_donky(cb->at[i], args[i]) : TAG_BASE;   // unused params read as null

    uint64_t r = ((DkCbFn)(uintptr_t)cb->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if (donky_has_exception()) return;                       // stays pending; surfaces when the C call returns

    switch (cb->rt) {
        case 'v': break;
        case 'i': *(ffi_sarg*)ret = (int32_t)dk_ffi_to_i64(r); break;
        case 'u': *(ffi_arg*)ret  = (uint32_t)dk_ffi_to_u64(r); break;
        case 'l': *(int64_t*)ret  = dk_ffi_to_i64(r); break;
        case 'L': *(uint64_t*)ret = dk_ffi_to_u64(r); break;
        case 'f': *(float*)ret    = is_number(r) ? (float)as_number(r) : 0.0f; break;
        case 'd': *(double*)ret   = is_number(r) ? as_number(r) : 0.0; break;
        case 'p': *(void**)ret    = is_number(r) ? (void*)(uintptr_t)dk_ffi_to_u64(r) : NULL; break;
    }
}

// func_val is the RAW code address the AOT produces for a function name (same as donky_spawn's
// func_val), not a boxed number. Returns the C entry point as a number (use with type 'p').
// Callbacks are never freed (they are meant to live for the whole program).
uint64_t donky_dll_callback(uint64_t ret_type_val, uint64_t arg_types_val, uint64_t func_val) {
    char msg[96];
    func_val = dk_fn_raw(func_val);
    if (func_val == 0 || (func_val >> 48) != 0)
        return dk_ffi_err("dll_callback: third argument must be a function");

    if (get_tag(ret_type_val) != TAG_STRING || strlen((const char*)as_index(ret_type_val)) != 1)
        return dk_ffi_err("dll_callback: return type must be a 1-char string (v i u l L f d p)");
    char rt = ((const char*)as_index(ret_type_val))[0];
    if (rt == 's') return dk_ffi_err("dll_callback: return type 's' is not supported (a Donky string may be freed by the GC); use 'p'");
    ffi_type* rtype = dk_ffi_type_of(rt);
    if (!rtype) { snprintf(msg, sizeof msg, "dll_callback: bad return type '%c'", rt); return dk_ffi_err(msg); }

    const char* at = "";
    if (get_tag(arg_types_val) == TAG_STRING) at = (const char*)as_index(arg_types_val);
    else if (arg_types_val != TAG_BASE) return dk_ffi_err("dll_callback: arg types must be a string (or null for none)");
    size_t n = strlen(at);
    if (n > DK_CB_MAX_ARGS) return dk_ffi_err("dll_callback: too many arguments (max 8)");

    DkCallback* cb = (DkCallback*)calloc(1, sizeof *cb);
    if (!cb) return dk_ffi_err("dll_callback: out of memory");
    for (size_t i = 0; i < n; i++) {
        ffi_type* t = at[i] == 'v' ? NULL : dk_ffi_type_of(at[i]);
        if (!t) {
            snprintf(msg, sizeof msg, "dll_callback: bad arg type '%c' at position %zu", at[i], i);
            free(cb); return dk_ffi_err(msg);
        }
        cb->atypes[i] = t;
        cb->at[i] = at[i];
    }
    cb->nargs = (int)n; cb->rt = rt; cb->fn = func_val;

    if (ffi_prep_cif(&cb->cif, FFI_DEFAULT_ABI, (unsigned)n, rtype, cb->atypes) != FFI_OK) {
        free(cb); return dk_ffi_err("dll_callback: ffi_prep_cif failed");
    }
    ffi_closure* closure = (ffi_closure*)ffi_closure_alloc(sizeof(ffi_closure), &cb->code);
    if (!closure) { free(cb); return dk_ffi_err("dll_callback: ffi_closure_alloc failed"); }
    if (ffi_prep_closure_loc(closure, &cb->cif, dk_ffi_trampoline, cb, cb->code) != FFI_OK) {
        ffi_closure_free(closure); free(cb);
        return dk_ffi_err("dll_callback: ffi_prep_closure_loc failed");
    }
    return make_number((double)(uint64_t)(uintptr_t)cb->code);
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
    // dk_id_from_socket returns 0 when the DK_MAX_SOCK table is full. In that
    // case the fd we just accepted has no handle, so nothing on the Donky side
    // can reach it: close it here or it leaks at the OS level (the Donky accept
    // loop only sees a -1 and keeps spinning). Drop the connection and report
    // failure, the same way a failed accept() does.
    int id = dk_id_from_socket(c);
    if (id == 0) { closesocket(c); return make_number(-1.0); }
    return make_number((double)id);
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
    t->func_ptr = dk_fn_raw(func_val);
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

// sys_write_binary(path, bytes): binary-safe write. bytes is an array of numbers; each is
// truncated to one byte (value & 0xFF, NaN -> 0). Unlike write_file, 0x00 bytes are written.
// The array is checked before the file is opened, so a bad array does not truncate an existing
// file. Returns true on success, false on any failure (bad argument, open, write or close error).
uint64_t donky_sys_write_binary(uint64_t path_val, uint64_t array_val) {
    if (!dk_is_str(path_val)) return BOOL_FALSE_BITS;
    if (is_number(array_val) || get_tag(array_val) != TAG_ARRAY) return BOOL_FALSE_BITS;
    Array* arr = (Array*)as_index(array_val);
    for (size_t i = 0; i < arr->len; i++)
        if (!is_number(arr->data[i])) return BOOL_FALSE_BITS;
    FILE* f = fopen((const char*)as_index(path_val), "wb");
    if (!f) return BOOL_FALSE_BITS;
    int ok = 1;
    for (size_t i = 0; i < arr->len; i++) {
        double d = as_number(arr->data[i]);
        uint8_t b = 0;
        if (d == d) {                                   // NaN stays 0
            if (d >= 9.0e18)       b = 0xFF;            // huge: avoid UB in the cast
            else if (d <= -9.0e18) b = 0;
            else                   b = (uint8_t)((int64_t)d & 0xFF);
        }
        if (fputc(b, f) == EOF) { ok = 0; break; }
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? BOOL_TRUE_BITS : BOOL_FALSE_BITS;
}

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

// ---- raw syscalls + executable memory ------------------------------------------------------
// All values are NaN-boxed Donky values (like every other runtime function): numbers in,
// numbers out. Bad ARGUMENTS throw a Donky error; an OS-level failure is reported through the
// return value (0 / 0.0), the way C reports it.
//
// Typical JIT flow:   p = mem_alloc(4096); mem_write(p, code); mem_protect(p, 4096, 2);
//                     r = dll_call_sig(p, "i", "", []); mem_free(p, 4096);
// Pointers travel as doubles, so they must fit in 48 bits (true for user-space on x64/arm64).

// Make freshly written code visible to the instruction fetcher (required on arm64, a no-op on x86).
static void dk_icache_flush(void* p, size_t n) {
#if defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), p, n);
#elif defined(__GNUC__)
    __builtin___clear_cache((char*)p, (char*)p + n);
#else
    (void)p; (void)n;
#endif
}

// A usable address: a whole number in [1, 2^48).
static int dk_mem_ptr(uint64_t v, void** out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (!(d >= 1.0) || d >= 281474976710656.0 || d != floor(d)) return 0;
    *out = (void*)(uintptr_t)(uint64_t)d;
    return 1;
}

// A byte count: a whole number in [1, 2^52].
static int dk_mem_size(uint64_t v, size_t* out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (!(d >= 1.0) || d > 4503599627370496.0 || d != floor(d)) return 0;
    *out = (size_t)d;
    return 1;
}

// syscall(nr, [a0..a5]): POSIX only. args is an array of up to 6 values (or null for none).
// Numbers/bools are passed as 64-bit integers (negatives wrap, so -1 -> 0xFFFF...FFFF);
// a string is passed as its char* (handy for write/open). Returns the raw result as a signed
// number: -1 when the call failed. The syscall number is NOT validated: a bad number is the
// kernel's problem, exactly as in C.
uint64_t donky_syscall(uint64_t nr_val, uint64_t args_val) {
#if defined(_WIN32)
    (void)nr_val; (void)args_val;
    return dk_ffi_err("syscall is not supported on Windows; use dll_call_sig with ntdll.dll");
#elif defined(__linux__) || defined(__APPLE__)
    if (!is_number(nr_val)) return dk_ffi_err("syscall: number must be a number");
    uint64_t a[6] = {0, 0, 0, 0, 0, 0};
    if (args_val != TAG_BASE) {
        if (get_tag(args_val) != TAG_ARRAY) return dk_ffi_err("syscall: args must be an array (or null)");
        Array* arr = (Array*)as_index(args_val);
        if (arr->len > 6) return dk_ffi_err("syscall: too many arguments (max 6)");
        for (size_t i = 0; i < arr->len; i++) {
            uint64_t v = arr->data[i];
            if (dk_ffi_is_numeric(v))             a[i] = dk_ffi_to_u64(v);
            else if (get_tag(v) == TAG_STRING)    a[i] = (uint64_t)(uintptr_t)as_index(v);
            else {
                char msg[64];
                snprintf(msg, sizeof msg, "syscall: arg %zu must be a number or string", i);
                return dk_ffi_err(msg);
            }
        }
    }
#ifdef __APPLE__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"   // syscall() is deprecated on macOS
#endif
    long r = (long)syscall((long)dk_ffi_to_u64(nr_val), a[0], a[1], a[2], a[3], a[4], a[5]);
#ifdef __APPLE__
#pragma GCC diagnostic pop
#endif
    return make_number((double)(int64_t)r);
#else
    (void)nr_val; (void)args_val;
    return dk_ffi_err("syscall is not supported on this platform");
#endif
}

// Page-aligned, zero-filled, read/write block. Returns the address, or 0 if the OS refused.
uint64_t donky_mem_alloc(uint64_t size_val) {
    size_t sz;
    if (!dk_mem_size(size_val, &sz)) return dk_ffi_err("mem_alloc: size must be a positive whole number");
#ifdef _WIN32
    void* p = VirtualAlloc(NULL, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    return make_number((double)(uint64_t)(uintptr_t)p);
}

// Copies a string's bytes, or an array of numbers 0-255, to ptr. Returns the byte count.
// The array is validated in full before the first byte is written. The target must be writable
// (protection 1 or 3), otherwise the process faults like any C store would.
uint64_t donky_mem_write(uint64_t ptr_val, uint64_t bytes_val) {
    void* dst;
    if (!dk_mem_ptr(ptr_val, &dst)) return dk_ffi_err("mem_write: ptr must be a non-null address from mem_alloc");
    size_t n = 0;
    if (!is_number(bytes_val) && get_tag(bytes_val) == TAG_STRING) {
        const char* s = (const char*)as_index(bytes_val);
        n = str_len(s);
        if (n) memcpy(dst, s, n);
    } else if (!is_number(bytes_val) && get_tag(bytes_val) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(bytes_val);
        n = arr->len;
        for (size_t i = 0; i < n; i++) {
            uint64_t v = arr->data[i];
            double d = is_number(v) ? as_number(v) : -1.0;
            if (!(d >= 0.0 && d <= 255.0 && d == floor(d))) {
                char msg[80];
                snprintf(msg, sizeof msg, "mem_write: element %zu is not a byte (0-255)", i);
                return dk_ffi_err(msg);
            }
        }
        uint8_t* o = (uint8_t*)dst;
        for (size_t i = 0; i < n; i++) o[i] = (uint8_t)as_number(arr->data[i]);
    } else {
        return dk_ffi_err("mem_write: bytes must be a string or an array of numbers (0-255)");
    }
    if (n) dk_icache_flush(dst, n);
    return make_number((double)n);
}

// prot: 1 = read/write (also used when prot is null), 2 = read/execute, 3 = read/write/execute.
// Returns 1.0 on success, 0.0 if the OS refused (e.g. RWX forbidden by SELinux / hardened runtime,
// or a non-page-aligned ptr on POSIX). Pass pointers returned by mem_alloc.
uint64_t donky_mem_protect(uint64_t ptr_val, uint64_t size_val, uint64_t prot_val) {
    void* p; size_t sz;
    if (!dk_mem_ptr(ptr_val, &p))      return dk_ffi_err("mem_protect: ptr must be a non-null address from mem_alloc");
    if (!dk_mem_size(size_val, &sz))   return dk_ffi_err("mem_protect: size must be a positive whole number");
    int prot = 1;
    if (prot_val != TAG_BASE) {
        double d = is_number(prot_val) ? as_number(prot_val) : 0.0;
        if (d != 1.0 && d != 2.0 && d != 3.0) return dk_ffi_err("mem_protect: prot must be 1 (RW), 2 (RX) or 3 (RWX)");
        prot = (int)d;
    }
#ifdef _WIN32
    DWORD np = prot == 1 ? PAGE_READWRITE : prot == 2 ? PAGE_EXECUTE_READ : PAGE_EXECUTE_READWRITE;
    DWORD old = 0;
    int ok = VirtualProtect(p, sz, np, &old) != 0;
#else
    int np = prot == 1 ? (PROT_READ | PROT_WRITE)
           : prot == 2 ? (PROT_READ | PROT_EXEC)
                       : (PROT_READ | PROT_WRITE | PROT_EXEC);
    int ok = mprotect(p, sz, np) == 0;
#endif
    if (ok && prot >= 2) dk_icache_flush(p, sz);
    return make_number(ok ? 1.0 : 0.0);
}

// Releases a mem_alloc block. size must be the size given to mem_alloc (munmap needs it; Windows
// ignores it). A bad ptr/size is a silent no-op, like free(NULL). Always returns 0.
uint64_t donky_mem_free(uint64_t ptr_val, uint64_t size_val) {
    void* p; size_t sz = 0;
    if (!dk_mem_ptr(ptr_val, &p)) return 0;
    (void)dk_mem_size(size_val, &sz);
#ifdef _WIN32
    (void)sz;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    if (sz) munmap(p, sz);
#endif
    return 0;
}

// ---- POSIX process control: fork / exec / wait / pipe / dup2 / mmap / munmap ---------------
// Windows has no equivalent of these (CreateProcessA & co. are reachable through dll_call_sig
// on kernel32.dll), so every function below throws there. As elsewhere in this file: bad
// ARGUMENTS and failed OS calls throw a Donky error (OS errors carry strerror(errno) text).
#ifdef _WIN32
static uint64_t dk_proc_unsupported(void) {
    return dk_ffi_err("not supported on Windows; use dll_call_sig");
}
#else
static uint64_t dk_proc_errno(const char* what, int e) {
    char msg[320];
    snprintf(msg, sizeof msg, "%s: %s", what, strerror(e));
    return dk_ffi_err(msg);
}

// A whole number in [lo, hi] as an int (no UB on NaN / out-of-range doubles).
static int dk_proc_int(uint64_t v, double lo, double hi, int* out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (!(d >= lo && d <= hi) || d != floor(d)) return 0;
    *out = (int)d;
    return 1;
}
#endif

// fork(): returns the child's pid in the parent and 0 in the child.
// Only the calling thread exists in the child, so the thread pool must not be trusted there:
//  - both locks are held across fork() so the child never inherits one that is locked by a
//    thread that no longer exists;
//  - in the child the (now thread-less) pool is reset: queued/running task counts go to 0
//    (otherwise main() would wait forever on active_threads) and the next spawn() builds a
//    fresh pool.
// stdio is flushed first, otherwise buffered output would be written twice.
// Call fork() from the main program, not from inside a spawn()ed task: in the child the task's
// worker thread would be the only thread and would park in the pool loop instead of exiting.
uint64_t donky_fork(void) {
#ifdef _WIN32
    return dk_proc_unsupported();
#else
    fflush(NULL);
    int pooled = (dk_pool_state == 1);
    if (pooled) { gc_init_lock(); EnterCriticalSection(&dk_pool_lock); EnterCriticalSection(&gc_lock); }
    pid_t pid = fork();
    int e = errno;
    if (pid == 0 && pooled) {
        dk_q_head = NULL; dk_q_tail = NULL; dk_q_len = 0;   // tasks queued in the parent are dropped (leaked)
        active_threads = 0;
        dk_pool_state = 0;
    }
    if (pooled) { LeaveCriticalSection(&gc_lock); LeaveCriticalSection(&dk_pool_lock); }
    if (pid < 0) return dk_proc_errno("fork", e);
    return make_number((double)pid);
#endif
}

// exec(path, argv): execvp(). argv is an array of strings and includes argv[0]; null or an empty
// array means [path]. PATH is searched when path has no '/'. On success it never returns; if it
// returns, the call failed and a Donky error is thrown (in a forked child, handle it and exit).
uint64_t donky_exec(uint64_t path_val, uint64_t args_val) {
#ifdef _WIN32
    (void)path_val; (void)args_val;
    return dk_proc_unsupported();
#else
    if (!dk_is_str(path_val)) return dk_ffi_err("exec: path must be a string");
    const char* path = dk_s(path_val);
    Array* arr = NULL;
    size_t argc = 0;
    if (args_val != TAG_BASE) {
        if (get_tag(args_val) != TAG_ARRAY) return dk_ffi_err("exec: args must be an array of strings (or null)");
        arr = (Array*)as_index(args_val);
        argc = arr->len;
        for (size_t i = 0; i < argc; i++) {
            if (!dk_is_str(arr->data[i])) {
                char msg[64];
                snprintf(msg, sizeof msg, "exec: arg %zu must be a string", i);
                return dk_ffi_err(msg);
            }
        }
    }
    char** argv = (char**)malloc(((argc ? argc : 1) + 1) * sizeof(char*));
    if (!argv) return dk_ffi_err("exec: out of memory");
    if (argc == 0) { argv[0] = (char*)path; argv[1] = NULL; }
    else {
        for (size_t i = 0; i < argc; i++) argv[i] = (char*)dk_s(arr->data[i]);   // Donky strings are NUL-terminated
        argv[argc] = NULL;
    }
    fflush(NULL);                       // exec discards unflushed stdio buffers
    execvp(path, argv);
    int e = errno;                      // only reached on failure
    free(argv);
    char msg[320];
    snprintf(msg, sizeof msg, "exec: %s: %s", path, strerror(e));
    return dk_ffi_err(msg);
#endif
}

// wait(pid): waitpid(pid, &status, 0), retried on EINTR. pid must be >= 1 (a specific child).
// Returns the exit code (0-255) of a child that exited, or -N if it was killed by signal N
// (e.g. -9 for SIGKILL).
uint64_t donky_wait(uint64_t pid_val) {
#ifdef _WIN32
    (void)pid_val;
    return dk_proc_unsupported();
#else
    int pid;
    if (!dk_proc_int(pid_val, 1.0, 2147483647.0, &pid)) return dk_ffi_err("wait: pid must be a positive whole number");
    int status = 0;
    pid_t r;
    do { r = waitpid((pid_t)pid, &status, 0); } while (r < 0 && errno == EINTR);
    if (r < 0) return dk_proc_errno("wait", errno);
    if (WIFEXITED(status))   return make_number((double)WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return make_number(-(double)WTERMSIG(status));
    return make_number((double)status);
#endif
}

// donky_exec_wait(path, argv, timeout_ms) -> { code, stdout, stderr }
// Runs a child process to completion (or timeout) and captures its output.
// Unlike exec(), this RETURNS and does not replace the Donky process.
// Windows: CreateProcessW + WaitForSingleObject. POSIX: fork/execvp/pipe/waitpid.
// timeout_ms <= 0 means "wait forever". On timeout the child is killed; code is -9.
#ifdef _WIN32
uint64_t donky_exec_wait(uint64_t path_val, uint64_t argv_val, uint64_t timeout_val) {
    if (!dk_is_str(path_val)) return dk_ffi_err("exec_wait: path must be a string");
    Array* arr = NULL; size_t argc = 0;
    if (argv_val != TAG_BASE) {
        if (get_tag(argv_val) != TAG_ARRAY) return dk_ffi_err("exec_wait: argv must be an array of strings (or null)");
        arr = (Array*)as_index(argv_val); argc = arr->len;
    }
    // Build the command line: quote each arg; Windows requires this.
    size_t cap = 4096 + argc * 260; char* cmd = (char*)malloc(cap); if (!cmd) return dk_ffi_err("exec_wait: out of memory");
    size_t o = 0;
    o += (size_t)snprintf(cmd + o, cap - o, "\"%s\"", dk_s(path_val));
    for (size_t i = 0; i < argc; i++) {
        if (!dk_is_str(arr->data[i])) { free(cmd); return dk_ffi_err("exec_wait: every argv element must be a string"); }
        o += (size_t)snprintf(cmd + o, cap - o, " \"%s\"", dk_s(arr->data[i]));
    }
    // Pipes: [0] = read end, [1] = write end, for stdout then stderr.
    SECURITY_ATTRIBUTES sa = {0}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE out_r = NULL, out_w = NULL, err_r = NULL, err_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        free(cmd); return dk_ffi_err("exec_wait: CreatePipe failed");
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {0}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w; si.hStdError = err_w; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(out_w); CloseHandle(err_w);
    free(cmd);
    if (!ok) {
        CloseHandle(out_r); CloseHandle(err_r);
        char msg[128]; snprintf(msg, sizeof msg, "exec_wait: CreateProcess failed (err %lu)", GetLastError());
        return dk_ffi_err(msg);
    }
    // Read both pipes until EOF (with a timeout on WaitForSingleObject).
    DWORD timeout = (is_number(timeout_val) && as_number(timeout_val) > 0.0) ? (DWORD)as_number(timeout_val) : INFINITE;
    int timed_out = 0;
    char* outbuf = NULL; size_t outn = 0, outcap = 0;
    char* errbuf = NULL; size_t errn = 0, errcap = 0;
    DWORD start = GetTickCount();
    for (;;) {
        DWORD remaining = INFINITE;
        if (timeout != INFINITE) { DWORD el = GetTickCount() - start; remaining = (el >= timeout) ? 0 : (timeout - el); }
        DWORD wr = WaitForSingleObject(pi.hProcess, 50);
        if (wr == WAIT_TIMEOUT && remaining == 0) { timed_out = 1; break; }
        if (wr == WAIT_OBJECT_0) break;
        // Drain pipes
        DWORD avail = 0;
        if (PeekNamedPipe(out_r, NULL, 0, NULL, &avail, NULL) && avail) {
            if (outn + avail + 1 > outcap) { outcap = (outcap ? outcap * 2 : 4096) + avail; outbuf = (char*)realloc(outbuf, outcap); }
            DWORD rd = 0; ReadFile(out_r, outbuf + outn, avail, &rd, NULL); outn += rd;
        }
        if (PeekNamedPipe(err_r, NULL, 0, NULL, &avail, NULL) && avail) {
            if (errn + avail + 1 > errcap) { errcap = (errcap ? errcap * 2 : 4096) + avail; errbuf = (char*)realloc(errbuf, errcap); }
            DWORD rd = 0; ReadFile(err_r, errbuf + errn, avail, &rd, NULL); errn += rd;
        }
    }
    if (timed_out) { TerminateProcess(pi.hProcess, 9); WaitForSingleObject(pi.hProcess, 1000); }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    // Drain anything left after process exit
    for (;;) { DWORD avail = 0; if (!PeekNamedPipe(out_r, NULL, 0, NULL, &avail, NULL) || !avail) break;
        if (outn + avail + 1 > outcap) { outcap = (outcap ? outcap * 2 : 4096) + avail; outbuf = (char*)realloc(outbuf, outcap); }
        DWORD rd = 0; ReadFile(out_r, outbuf + outn, avail, &rd, NULL); outn += rd; }
    for (;;) { DWORD avail = 0; if (!PeekNamedPipe(err_r, NULL, 0, NULL, &avail, NULL) || !avail) break;
        if (errn + avail + 1 > errcap) { errcap = (errcap ? errcap * 2 : 4096) + avail; errbuf = (char*)realloc(errbuf, errcap); }
        DWORD rd = 0; ReadFile(err_r, errbuf + errn, avail, &rd, NULL); errn += rd; }
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(out_r); CloseHandle(err_r);
    if (!outbuf) { outbuf = (char*)malloc(1); }
    if (!errbuf) { errbuf = (char*)malloc(1); }
    outbuf[outn] = 0; errbuf[errn] = 0;
    uint64_t m = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("code"),   make_number(timed_out ? -9.0 : (double)code));
    donky_map_set(m, donky_alloc_string("stdout"), donky_alloc_string(outbuf));
    donky_map_set(m, donky_alloc_string("stderr"), donky_alloc_string(errbuf));
    free(outbuf); free(errbuf);
    return m;
}
#else
uint64_t donky_exec_wait(uint64_t path_val, uint64_t argv_val, uint64_t timeout_val) {
    if (!dk_is_str(path_val)) return dk_ffi_err("exec_wait: path must be a string");
    Array* arr = NULL; size_t argc = 0;
    if (argv_val != TAG_BASE) {
        if (get_tag(argv_val) != TAG_ARRAY) return dk_ffi_err("exec_wait: argv must be an array of strings (or null)");
        arr = (Array*)as_index(argv_val); argc = arr->len;
    }
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) return dk_ffi_err("exec_wait: pipe failed");
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]); return dk_ffi_err("exec_wait: fork failed"); }
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
        char** argv = (char**)malloc((argc + 2) * sizeof(char*));
        argv[0] = (char*)dk_s(path_val);
        for (size_t i = 0; i < argc; i++) argv[i + 1] = (char*)dk_s(arr->data[i]);
        argv[argc + 1] = NULL;
        execvp(argv[0], argv);
        _exit(127);
    }
    close(out_pipe[1]); close(err_pipe[1]);
    // Read both pipes with select() and a timeout.
    char* outbuf = NULL; size_t outn = 0, outcap = 0;
    char* errbuf = NULL; size_t errn = 0, errcap = 0;
    struct timeval tv_start, tv_now;
    gettimeofday(&tv_start, NULL);
    double timeout_ms = (is_number(timeout_val) && as_number(timeout_val) > 0.0) ? as_number(timeout_val) : 0.0;
    int timed_out = 0;
    int open_r = 2; int fds[2] = { out_pipe[0], err_pipe[0] };
    while (open_r > 0) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < 2; i++) if (fds[i] >= 0) { FD_SET(fds[i], &rfds); if (fds[i] > maxfd) maxfd = fds[i]; }
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000;
        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (timeout_ms > 0) {
            gettimeofday(&tv_now, NULL);
            double elapsed = (tv_now.tv_sec - tv_start.tv_sec) * 1000.0 + (tv_now.tv_usec - tv_start.tv_usec) / 1000.0;
            if (elapsed > timeout_ms) { timed_out = 1; break; }
        }
        if (sel <= 0) continue;
        for (int i = 0; i < 2; i++) {
            if (fds[i] < 0 || !FD_ISSET(fds[i], &rfds)) continue;
            char buf[4096]; ssize_t r = read(fds[i], buf, sizeof buf);
            if (r <= 0) { close(fds[i]); fds[i] = -1; open_r--; continue; }
            if (i == 0) {
                if (outn + (size_t)r + 1 > outcap) { outcap = (outcap ? outcap * 2 : 4096) + (size_t)r; outbuf = (char*)realloc(outbuf, outcap); }
                memcpy(outbuf + outn, buf, (size_t)r); outn += (size_t)r;
            } else {
                if (errn + (size_t)r + 1 > errcap) { errcap = (errcap ? errcap * 2 : 4096) + (size_t)r; errbuf = (char*)realloc(errbuf, errcap); }
                memcpy(errbuf + errn, buf, (size_t)r); errn += (size_t)r;
            }
        }
    }
    if (timed_out) kill(pid, SIGKILL);
    int status = 0; waitpid(pid, &status, 0);
    int code = 0;
    if (timed_out) code = -9;
    else if (WIFEXITED(status)) code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) code = -WTERMSIG(status);
    if (!outbuf) { outbuf = (char*)malloc(1); }
    if (!errbuf) { errbuf = (char*)malloc(1); }
    outbuf[outn] = 0; errbuf[errn] = 0;
    uint64_t m = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("code"),   make_number((double)code));
    donky_map_set(m, donky_alloc_string("stdout"), donky_alloc_string(outbuf));
    donky_map_set(m, donky_alloc_string("stderr"), donky_alloc_string(errbuf));
    free(outbuf); free(errbuf);
    return m;
}
#endif

// pipe(): returns [read_fd, write_fd]. The fds are plain (inherited across fork/exec); close
// the ends you do not use or the reader never sees EOF.
uint64_t donky_pipe(void) {
#ifdef _WIN32
    return dk_proc_unsupported();
#else
    int fds[2];
    if (pipe(fds) < 0) return dk_proc_errno("pipe", errno);
    uint64_t arr = donky_alloc_array();
    donky_array_push(arr, make_number((double)fds[0]));
    donky_array_push(arr, make_number((double)fds[1]));
    return arr;
#endif
}

// dup2(old_fd, new_fd): makes new_fd a copy of old_fd (closing new_fd first). Returns 0.
uint64_t donky_dup2(uint64_t old_fd, uint64_t new_fd) {
#ifdef _WIN32
    (void)old_fd; (void)new_fd;
    return dk_proc_unsupported();
#else
    int a, b;
    if (!dk_proc_int(old_fd, 0.0, 2147483647.0, &a)) return dk_ffi_err("dup2: old_fd must be a non-negative whole number");
    if (!dk_proc_int(new_fd, 0.0, 2147483647.0, &b)) return dk_ffi_err("dup2: new_fd must be a non-negative whole number");
    int r;
    do { r = dup2(a, b); } while (r < 0 && errno == EINTR);
    if (r < 0) return dk_proc_errno("dup2", errno);
    return 0;
#endif
}

// mmap(len, prot, flags): mmap(NULL, len, prot, flags, -1, 0). prot and flags are the raw
// platform values (PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4; MAP_SHARED=1, MAP_PRIVATE=2, and
// MAP_ANONYMOUS = 0x20 on Linux / MAP_ANON = 0x1000 on macOS). The fd is always -1, so flags
// must include the anonymous bit. MAP_SHARED|anonymous memory is shared with forked children.
// Returns the address; release it with munmap(ptr, len).
uint64_t donky_mmap(uint64_t len_val, uint64_t prot_val, uint64_t flags_val) {
#ifdef _WIN32
    (void)len_val; (void)prot_val; (void)flags_val;
    return dk_proc_unsupported();
#else
    size_t len; int prot, flags;
    if (!dk_mem_size(len_val, &len)) return dk_ffi_err("mmap: len must be a positive whole number");
    if (!dk_proc_int(prot_val, 0.0, 2147483647.0, &prot)) return dk_ffi_err("mmap: prot must be a whole number (PROT_* bits)");
    if (!dk_proc_int(flags_val, 0.0, 2147483647.0, &flags)) return dk_ffi_err("mmap: flags must be a whole number (MAP_* bits)");
    void* p = mmap(NULL, len, prot, flags, -1, 0);
    if (p == MAP_FAILED) return dk_proc_errno("mmap", errno);
    return make_number((double)(uint64_t)(uintptr_t)p);
#endif
}

// munmap(ptr, len): len must match the mapping. Returns 0.
uint64_t donky_munmap(uint64_t ptr_val, uint64_t len_val) {
#ifdef _WIN32
    (void)ptr_val; (void)len_val;
    return dk_proc_unsupported();
#else
    void* p; size_t len;
    if (!dk_mem_ptr(ptr_val, &p))     return dk_ffi_err("munmap: ptr must be a non-null address from mmap");
    if (!dk_mem_size(len_val, &len))  return dk_ffi_err("munmap: len must be a positive whole number");
    if (munmap(p, len) != 0) return dk_proc_errno("munmap", errno);
    return 0;
#endif
}

// ---- POSIX security & OS configuration -------------------------------------------------------
// setuid/setgid/seteuid/setegid, chroot, unshare, prctl, seccomp, getpwnam/getpwuid, uname,
// getrlimit/setrlimit. Windows has no equivalent, so every function below throws "not supported on
// Windows" there; unshare, prctl and seccomp are Linux-only and throw on macOS. As in the process
// control section above, bad ARGUMENTS and failed OS calls throw a Donky error (OS errors carry
// strerror(errno) text). Success returns the number 0 unless documented, or a map.
#ifdef _WIN32
static uint64_t dk_sec_unsupported(void) { return dk_ffi_err("not supported on Windows"); }
#else
// A uid/gid: a whole number in [0, 4294967294] (4294967295 is (uid_t)-1, the "leave unchanged" value).
static int dk_sec_u32(uint64_t v, uint32_t* out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (!(d >= 0.0 && d <= 4294967294.0) || d != floor(d)) return 0;
    *out = (uint32_t)d;
    return 1;
}

#ifdef __linux__
// A raw unsigned-long argument: a whole number in [-2^63, 2^64). Negatives wrap (-1 -> ~0).
static int dk_sec_u64(uint64_t v, uint64_t* out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (!(d == d) || d != floor(d)) return 0;                 // NaN or fraction
    if (d >= 0.0) {
        if (d >= 18446744073709551616.0) return 0;
        *out = (uint64_t)d;
        return 1;
    }
    if (d < -9223372036854775808.0) return 0;
    *out = (uint64_t)(int64_t)d;
    return 1;
}
#endif

// which: 0 setuid, 1 setgid, 2 seteuid, 3 setegid
static uint64_t dk_sec_setid(const char* what, uint64_t v, int which) {
    uint32_t id;
    if (!dk_sec_u32(v, &id)) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s: id must be a whole number in [0, 4294967294]", what);
        return dk_ffi_err(msg);
    }
    int r;
    switch (which) {
        case 0:  r = setuid((uid_t)id); break;
        case 1:  r = setgid((gid_t)id); break;
        case 2:  r = seteuid((uid_t)id); break;
        default: r = setegid((gid_t)id); break;
    }
    if (r < 0) return dk_proc_errno(what, errno);
    return make_number(0.0);
}
#endif

// Drop privileges in this order: setgid first, then setuid (once the uid is gone the process can
// no longer change its gid). Returns 0.
uint64_t donky_setuid(uint64_t id) {
#ifdef _WIN32
    (void)id; return dk_sec_unsupported();
#else
    return dk_sec_setid("setuid", id, 0);
#endif
}
uint64_t donky_setgid(uint64_t id) {
#ifdef _WIN32
    (void)id; return dk_sec_unsupported();
#else
    return dk_sec_setid("setgid", id, 1);
#endif
}
uint64_t donky_seteuid(uint64_t id) {
#ifdef _WIN32
    (void)id; return dk_sec_unsupported();
#else
    return dk_sec_setid("seteuid", id, 2);
#endif
}
uint64_t donky_setegid(uint64_t id) {
#ifdef _WIN32
    (void)id; return dk_sec_unsupported();
#else
    return dk_sec_setid("setegid", id, 3);
#endif
}

// chroot(path): needs root (CAP_SYS_CHROOT). It does NOT change the working directory, so follow it
// with sys_chdir("/") or the process can still reach the old tree through a relative path. Returns 0.
uint64_t donky_chroot(uint64_t path_val) {
#ifdef _WIN32
    (void)path_val; return dk_sec_unsupported();
#else
    if (!dk_is_str(path_val)) return dk_ffi_err("chroot: path must be a string");
    if (chroot(dk_s(path_val)) < 0) return dk_proc_errno("chroot", errno);
    return make_number(0.0);
#endif
}

// unshare(flags): Linux only. flags is the raw CLONE_NEW* bit mask, e.g. CLONE_NEWNS 0x20000,
// CLONE_NEWUTS 0x4000000, CLONE_NEWIPC 0x8000000, CLONE_NEWUSER 0x10000000, CLONE_NEWPID
// 0x20000000, CLONE_NEWNET 0x40000000. CLONE_NEWUSER fails with EINVAL once the process has more
// than one thread (after spawn()); CLONE_NEWPID only affects children created afterwards. Returns 0.
uint64_t donky_unshare(uint64_t flags_val) {
#ifdef _WIN32
    (void)flags_val; return dk_sec_unsupported();
#elif defined(__linux__)
    int flags;
    if (!dk_proc_int(flags_val, 0.0, 2147483647.0, &flags))
        return dk_ffi_err("unshare: flags must be a non-negative whole number");
    if (unshare(flags) < 0) return dk_proc_errno("unshare", errno);
    return make_number(0.0);
#else
    (void)flags_val;
    return dk_ffi_err("unshare: not supported on this platform (Linux only)");
#endif
}

// prctl(option, a2, a3, a4, a5): Linux only. All five are numbers (raw values; pass 0 for unused
// ones; a pointer is a number from mem_alloc/ffi_alloc). Returns prctl()'s result: 0 for most
// options, the queried value for PR_GET_* options (e.g. PR_GET_DUMPABLE = 3).
uint64_t donky_prctl(uint64_t option, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
#ifdef _WIN32
    (void)option; (void)a2; (void)a3; (void)a4; (void)a5;
    return dk_sec_unsupported();
#elif defined(__linux__)
    int opt;
    if (!dk_proc_int(option, 0.0, 2147483647.0, &opt))
        return dk_ffi_err("prctl: option must be a non-negative whole number");
    uint64_t in[4] = { a2, a3, a4, a5 };
    uint64_t a[4];
    for (int i = 0; i < 4; i++) {
        if (!dk_sec_u64(in[i], &a[i])) {
            char msg[64];
            snprintf(msg, sizeof msg, "prctl: arg %d must be a whole number", i + 2);
            return dk_ffi_err(msg);
        }
    }
    int r = prctl(opt, (unsigned long)a[0], (unsigned long)a[1], (unsigned long)a[2], (unsigned long)a[3]);
    if (r < 0) return dk_proc_errno("prctl", errno);
    return make_number((double)r);
#else
    (void)option; (void)a2; (void)a3; (void)a4; (void)a5;
    return dk_ffi_err("prctl: not supported on this platform (Linux only)");
#endif
}

// seccomp(op, flags, args): Linux only; the raw seccomp(2) syscall. op: 0 SET_MODE_STRICT,
// 1 SET_MODE_FILTER, 2 GET_ACTION_AVAIL, 3 GET_NOTIF_SIZES. args is the address of the kernel
// structure (e.g. a struct sock_fprog built in mem_alloc memory) or null for strict mode. A bad
// address is the kernel's EFAULT, not a crash. Installing a filter needs no_new_privs first:
// prctl(38, 1, 0, 0, 0) (PR_SET_NO_NEW_PRIVS) unless the process has CAP_SYS_ADMIN. Returns the
// syscall result (0, or e.g. a notification fd with SECCOMP_FILTER_FLAG_NEW_LISTENER).
uint64_t donky_seccomp(uint64_t op_val, uint64_t flags_val, uint64_t args_val) {
#ifdef _WIN32
    (void)op_val; (void)flags_val; (void)args_val;
    return dk_sec_unsupported();
#elif defined(__linux__) && defined(SYS_seccomp)
    int op; uint32_t flags; uint64_t ptr = 0;
    if (!dk_proc_int(op_val, 0.0, 2147483647.0, &op))
        return dk_ffi_err("seccomp: operation must be a non-negative whole number");
    if (!dk_sec_u32(flags_val, &flags))
        return dk_ffi_err("seccomp: flags must be a whole number in [0, 4294967294]");
    if (args_val != TAG_BASE && !dk_sec_u64(args_val, &ptr))
        return dk_ffi_err("seccomp: args must be an address (number) or null");
    long r = syscall(SYS_seccomp, (long)op, (unsigned long)flags, (void*)(uintptr_t)ptr);
    if (r < 0) return dk_proc_errno("seccomp", errno);
    return make_number((double)r);
#else
    (void)op_val; (void)flags_val; (void)args_val;
    return dk_ffi_err("seccomp: not supported on this platform (Linux only)");
#endif
}

#ifndef _WIN32
static uint64_t dk_sec_pw_map(const struct passwd* pw) {
    uint64_t m = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("name"),  donky_alloc_string(pw->pw_name  ? pw->pw_name  : ""));
    donky_map_set(m, donky_alloc_string("uid"),   make_number((double)pw->pw_uid));
    donky_map_set(m, donky_alloc_string("gid"),   make_number((double)pw->pw_gid));
    donky_map_set(m, donky_alloc_string("dir"),   donky_alloc_string(pw->pw_dir   ? pw->pw_dir   : ""));
    donky_map_set(m, donky_alloc_string("shell"), donky_alloc_string(pw->pw_shell ? pw->pw_shell : ""));
    return m;
}

// getpwnam_r / getpwuid_r with a buffer that grows on ERANGE. "Not found" is null (SEMANTICS 6.5);
// POSIX lets implementations report it as ENOENT/ESRCH/EBADF/EPERM, so those are null too.
static uint64_t dk_sec_pw_lookup(const char* what, const char* name, uid_t uid) {
    long sz = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t cap = sz > 0 ? (size_t)sz : 1024;
    for (;;) {
        char* buf = (char*)malloc(cap);
        if (!buf) return dk_ffi_err("getpw: out of memory");
        struct passwd pwd, *res = NULL;
        int rc = name ? getpwnam_r(name, &pwd, buf, cap, &res) : getpwuid_r(uid, &pwd, buf, cap, &res);
        if (rc == ERANGE && cap < ((size_t)1 << 20)) { free(buf); cap *= 2; continue; }
        uint64_t out;
        if (rc != 0 && rc != ENOENT && rc != ESRCH && rc != EBADF && rc != EPERM) out = dk_proc_errno(what, rc);
        else if (!res) out = TAG_BASE;
        else out = dk_sec_pw_map(res);
        free(buf);
        return out;
    }
}
#endif

// getpwnam(name) / getpwuid(uid): map {name, uid, gid, dir, shell}, or null if no such user.
uint64_t donky_getpwnam(uint64_t name_val) {
#ifdef _WIN32
    (void)name_val; return dk_sec_unsupported();
#else
    if (!dk_is_str(name_val)) return dk_ffi_err("getpwnam: name must be a string");
    return dk_sec_pw_lookup("getpwnam", dk_s(name_val), 0);
#endif
}
uint64_t donky_getpwuid(uint64_t uid_val) {
#ifdef _WIN32
    (void)uid_val; return dk_sec_unsupported();
#else
    uint32_t id;
    if (!dk_sec_u32(uid_val, &id)) return dk_ffi_err("getpwuid: uid must be a whole number in [0, 4294967294]");
    return dk_sec_pw_lookup("getpwuid", NULL, (uid_t)id);
#endif
}

// uname(): map {sysname, nodename, release, version, machine}.
uint64_t donky_uname(void) {
#ifdef _WIN32
    return dk_sec_unsupported();
#else
    struct utsname u;
    if (uname(&u) < 0) return dk_proc_errno("uname", errno);
    uint64_t m = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("sysname"),  donky_alloc_string(u.sysname));
    donky_map_set(m, donky_alloc_string("nodename"), donky_alloc_string(u.nodename));
    donky_map_set(m, donky_alloc_string("release"),  donky_alloc_string(u.release));
    donky_map_set(m, donky_alloc_string("version"),  donky_alloc_string(u.version));
    donky_map_set(m, donky_alloc_string("machine"),  donky_alloc_string(u.machine));
    return m;
#endif
}

#ifndef _WIN32
// resource: a portable name ("cpu" "fsize" "data" "stack" "core" "rss" "nproc" "nofile" "memlock" "as")
// or the raw platform RLIMIT_* number (the numbers differ between Linux and macOS; names do not).
static int dk_sec_rlimit_res(uint64_t v, int* out) {
    if (is_number(v)) return dk_proc_int(v, 0.0, 2147483647.0, out);
    if (!dk_is_str(v)) return 0;
    static const struct { const char* n; int r; } tab[] = {
        { "cpu", RLIMIT_CPU },     { "fsize", RLIMIT_FSIZE }, { "data", RLIMIT_DATA },
        { "stack", RLIMIT_STACK }, { "core", RLIMIT_CORE },   { "rss", RLIMIT_RSS },
        { "nproc", RLIMIT_NPROC }, { "nofile", RLIMIT_NOFILE }, { "memlock", RLIMIT_MEMLOCK },
        { "as", RLIMIT_AS },
    };
    const char* s = dk_s(v);
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strcmp(s, tab[i].n) == 0) { *out = tab[i].r; return 1; }
    return 0;
}

// RLIM_INFINITY is the number inf; on input -1 is accepted as "unlimited" too.
static double dk_sec_rlim_to_num(rlim_t v) { return v == RLIM_INFINITY ? INFINITY : (double)v; }
static int dk_sec_num_to_rlim(uint64_t v, rlim_t* out) {
    if (!is_number(v)) return 0;
    double d = as_number(v);
    if (d == INFINITY || d == -1.0) { *out = RLIM_INFINITY; return 1; }
    if (!(d >= 0.0) || d != floor(d) || d >= 18446744073709551616.0) return 0;
    *out = (rlim_t)d;
    return 1;
}
#endif

// getrlimit(resource): map {soft, hard}; unlimited is inf (values above 2^53 lose precision).
uint64_t donky_getrlimit(uint64_t res_val) {
#ifdef _WIN32
    (void)res_val; return dk_sec_unsupported();
#else
    int res;
    if (!dk_sec_rlimit_res(res_val, &res))
        return dk_ffi_err("getrlimit: resource must be a name (cpu fsize data stack core rss nproc nofile memlock as) or a raw RLIMIT_* number");
    struct rlimit rl;
    if (getrlimit(res, &rl) < 0) return dk_proc_errno("getrlimit", errno);
    uint64_t m = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("soft"), make_number(dk_sec_rlim_to_num(rl.rlim_cur)));
    donky_map_set(m, donky_alloc_string("hard"), make_number(dk_sec_rlim_to_num(rl.rlim_max)));
    return m;
#endif
}

// setrlimit(resource, soft, hard): soft/hard are whole numbers, inf or -1 (unlimited), or null to
// keep the current value (setrlimit("nofile", 4096, null)). Pass BOTH numbers: a missing argument
// is padded with 0.0 by the compiler and would lower that limit to 0. Lowering a hard limit cannot
// be undone without privilege. Returns 0.
uint64_t donky_setrlimit(uint64_t res_val, uint64_t soft_val, uint64_t hard_val) {
#ifdef _WIN32
    (void)res_val; (void)soft_val; (void)hard_val; return dk_sec_unsupported();
#else
    int res;
    if (!dk_sec_rlimit_res(res_val, &res))
        return dk_ffi_err("setrlimit: resource must be a name (cpu fsize data stack core rss nproc nofile memlock as) or a raw RLIMIT_* number");
    struct rlimit rl;
    if (getrlimit(res, &rl) < 0) return dk_proc_errno("setrlimit", errno);
    if (soft_val != TAG_BASE && !dk_sec_num_to_rlim(soft_val, &rl.rlim_cur))
        return dk_ffi_err("setrlimit: soft must be a whole number, inf, -1 or null");
    if (hard_val != TAG_BASE && !dk_sec_num_to_rlim(hard_val, &rl.rlim_max))
        return dk_ffi_err("setrlimit: hard must be a whole number, inf, -1 or null");
    if (setrlimit(res, &rl) < 0) return dk_proc_errno("setrlimit", errno);
    return make_number(0.0);
#endif
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

// ---- tensors: flat arrays of numbers -----------------------------------------------------------
// A tensor is a plain Donky array of numbers in row-major order; a shape is passed explicitly where it
// matters (matmul). Every function validates ALL its inputs before it allocates or mutates anything, so
// a throw never leaves a half-written result or a half-updated weight array. Results are fresh arrays,
// except sgd_step, which updates `weights` in place and returns it. Errors throw a string naming the
// function, the argument (and element index) and the offending type.
#define DK_TENSOR_MAX_DIM 2147483647.0

// `v` must be an array whose every element is a number. On failure: throws, returns 0.
static int dk_tensor_view(const char* who, const char* name, uint64_t v, Array** out) {
    char msg[128];
    if (is_number(v) || get_tag(v) != TAG_ARRAY) {
        snprintf(msg, sizeof msg, "%s: %s must be an array of numbers, got %s", who, name, dk_type_name(v));
        dk_ffi_err(msg);
        return 0;
    }
    Array* a = (Array*)as_index(v);
    for (size_t i = 0; i < a->len; i++) {
        if (!is_number(a->data[i])) {
            snprintf(msg, sizeof msg, "%s: %s[%zu] must be a number, got %s", who, name, i, dk_type_name(a->data[i]));
            dk_ffi_err(msg);
            return 0;
        }
    }
    *out = a;
    return 1;
}

// A whole number in [0, 2^31-1]. On failure: throws, returns 0.
static int dk_tensor_dim(const char* who, const char* name, uint64_t v, size_t* out) {
    char msg[96];
    if (!is_number(v)) {
        snprintf(msg, sizeof msg, "%s: %s must be a whole number, got %s", who, name, dk_type_name(v));
        dk_ffi_err(msg);
        return 0;
    }
    double d = as_number(v);
    if (!(d >= 0.0 && d <= DK_TENSOR_MAX_DIM) || d != floor(d)) {
        snprintf(msg, sizeof msg, "%s: %s must be a whole number in [0, 2147483647]", who, name);
        dk_ffi_err(msg);
        return 0;
    }
    *out = (size_t)d;
    return 1;
}

// ------------------------------------------------------------------ dk_tensor_alloc (shared helper)
// New array with exactly n slots and len == n. The slots are ZERO-filled (all-zero bits are +0.0, a valid
// number), so the array is always safe for the GC to scan, even if the caller fills it slowly or throws
// part-way. Returns TAG_BASE (with a pending throw) if the memory cannot be had.
static uint64_t dk_tensor_alloc(const char* who, size_t n, uint64_t** data) {
    uint64_t av = donky_alloc_array();
    Array* arr = (Array*)as_index(av);
    if (n) {
        char msg[64];
        if (n > SIZE_MAX / sizeof(uint64_t) || !(arr->data = (uint64_t*)calloc(n, sizeof(uint64_t)))) {
            arr->data = NULL;
            snprintf(msg, sizeof msg, "%s: out of memory", who);
            return dk_ffi_err(msg);
        }
        arr->cap = n;
        arr->len = n;
    }
    *data = arr->data;
    return av;
}

// op 0: a + b, op 1: a * b (element-wise). Either operand may be a lone number (broadcast).
static uint64_t dk_tensor_binop(const char* who, uint64_t a, uint64_t b, int op) {
    Array* pa = NULL; Array* pb = NULL;
    double sa = 0.0, sb = 0.0;
    char msg[128];
    if (is_number(a)) sa = as_number(a); else if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    if (is_number(b)) sb = as_number(b); else if (!dk_tensor_view(who, "b", b, &pb)) return TAG_BASE;
    if (!pa && !pb) {
        snprintf(msg, sizeof msg, "%s: at least one operand must be an array", who);
        return dk_ffi_err(msg);
    }
    if (pa && pb && pa->len != pb->len) {
        snprintf(msg, sizeof msg, "%s: shape mismatch: a has %zu elements, b has %zu", who, pa->len, pb->len);
        return dk_ffi_err(msg);
    }
    size_t n = pa ? pa->len : pb->len;
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    for (size_t i = 0; i < n; i++) {
        double x = pa ? as_number(pa->data[i]) : sa;
        double y = pb ? as_number(pb->data[i]) : sb;
        out[i] = make_number(op == 0 ? x + y : x * y);
    }
    return r;
}

// tensor_add(a, b): element-wise sum of two equal-length arrays (or an array and a number).
uint64_t donky_tensor_add(uint64_t a, uint64_t b) { return dk_tensor_binop("tensor_add", a, b, 0); }

// tensor_mul(a, b): element-wise (Hadamard) product; use tensor_matmul for the matrix product.
uint64_t donky_tensor_mul(uint64_t a, uint64_t b) { return dk_tensor_binop("tensor_mul", a, b, 1); }

// tensor_matmul(a, b, r_a, c_a, c_b): a is r_a x c_a, b is c_a x c_b (both row-major); returns the
// r_a x c_b product as a flat array. c_a must be positive; the array lengths must match the dims.
uint64_t donky_tensor_matmul(uint64_t a, uint64_t b, uint64_t r_a, uint64_t c_a, uint64_t c_b) {
    const char* who = "tensor_matmul";
    Array* pa; Array* pb;
    size_t R, K, C;
    char msg[160];
    if (!dk_tensor_view(who, "a", a, &pa) || !dk_tensor_view(who, "b", b, &pb)) return TAG_BASE;
    if (!dk_tensor_dim(who, "r_a", r_a, &R) || !dk_tensor_dim(who, "c_a", c_a, &K) || !dk_tensor_dim(who, "c_b", c_b, &C)) return TAG_BASE;
    if (K == 0) { snprintf(msg, sizeof msg, "%s: c_a (the inner dimension) must be positive", who); return dk_ffi_err(msg); }
    if (pa->len != R * K) {
        snprintf(msg, sizeof msg, "%s: a has %zu elements but r_a * c_a = %zu * %zu = %zu", who, pa->len, R, K, R * K);
        return dk_ffi_err(msg);
    }
    if (pb->len != K * C) {
        snprintf(msg, sizeof msg, "%s: b has %zu elements but c_a * c_b = %zu * %zu = %zu", who, pb->len, K, C, K * C);
        return dk_ffi_err(msg);
    }
    size_t n = R * C;                      // <= 2^62: cannot overflow
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    if (n) memset(out, 0, n * sizeof(uint64_t));          // all-zero bits == +0.0
    for (size_t i = 0; i < R; i++) {
        for (size_t k = 0; k < K; k++) {
            double aik = as_number(pa->data[i * K + k]);
            uint64_t* orow = out + i * C;
            const uint64_t* brow = pb->data + k * C;
            for (size_t j = 0; j < C; j++) {
                double cur;
                memcpy(&cur, &orow[j], sizeof cur);
                cur += aik * as_number(brow[j]);
                memcpy(&orow[j], &cur, sizeof cur);
            }
        }
    }
    for (size_t i = 0; i < n; i++) {                       // a NaN must not collide with the null tag
        double d; memcpy(&d, &out[i], sizeof d);
        out[i] = make_number(d);
    }
    return r;
}

// tensor_matmul_f32(a, b, r_a, c_a, c_b): same contract as tensor_matmul, but the multiply/
// accumulate runs in native float (roughly 2x faster and half the memory for the temporaries;
// the Donky-visible result is still an array of f64 values, matching every other tensor op).
// Inputs are unboxed once into float buffers, the product is accumulated in float, then each
// output element is re-boxed with make_number. A NaN produced by the accumulation is canonicalized
// by make_number, so it can never collide with the null tag.
uint64_t donky_tensor_matmul_f32(uint64_t a, uint64_t b, uint64_t r_a, uint64_t c_a, uint64_t c_b) {
    const char* who = "tensor_matmul_f32";
    Array* pa; Array* pb;
    size_t R, K, C;
    char msg[160];
    if (!dk_tensor_view(who, "a", a, &pa) || !dk_tensor_view(who, "b", b, &pb)) return TAG_BASE;
    if (!dk_tensor_dim(who, "r_a", r_a, &R) || !dk_tensor_dim(who, "c_a", c_a, &K) || !dk_tensor_dim(who, "c_b", c_b, &C)) return TAG_BASE;
    if (K == 0) { snprintf(msg, sizeof msg, "%s: c_a (the inner dimension) must be positive", who); return dk_ffi_err(msg); }
    if (pa->len != R * K) {
        snprintf(msg, sizeof msg, "%s: a has %zu elements but r_a * c_a = %zu * %zu = %zu", who, pa->len, R, K, R * K);
        return dk_ffi_err(msg);
    }
    if (pb->len != K * C) {
        snprintf(msg, sizeof msg, "%s: b has %zu elements but c_a * c_b = %zu * %zu = %zu", who, pb->len, K, C, K * C);
        return dk_ffi_err(msg);
    }
    size_t na = R * K, nb = K * C, n = R * C;             // <= 2^62, no overflow
    // Temporary float buffers. Kept null-safe: malloc(0) is allowed to return NULL.
    float* fa = (float*)malloc(na * sizeof(float));
    float* fb = (float*)malloc(nb * sizeof(float));
    float* fo = (float*)calloc(n ? n : 1, sizeof(float));
    if (!fa || !fb || !fo) {
        free(fa); free(fb); free(fo);
        snprintf(msg, sizeof msg, "%s: out of memory", who);
        return dk_ffi_err(msg);
    }
    for (size_t i = 0; i < na; i++) fa[i] = (float)as_number(pa->data[i]);
    for (size_t i = 0; i < nb; i++) fb[i] = (float)as_number(pb->data[i]);
    for (size_t i = 0; i < R; i++) {
        for (size_t k = 0; k < K; k++) {
            float aik = fa[i * K + k];
            float* orow = fo + i * C;
            const float* brow = fb + k * C;
            for (size_t j = 0; j < C; j++) orow[j] += aik * brow[j];
        }
    }
    // Box the float results back into a fresh Donky array of f64 values.
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) { free(fa); free(fb); free(fo); return TAG_BASE; }
    for (size_t i = 0; i < n; i++) out[i] = make_number((double)fo[i]);
    free(fa); free(fb); free(fo);
    return r;
}

// tensor_relu(a): max(x, 0) per element (NaN stays NaN).
uint64_t donky_tensor_relu(uint64_t a) {
    const char* who = "tensor_relu";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, pa->len, &out);
    if (r == TAG_BASE) return TAG_BASE;
    for (size_t i = 0; i < pa->len; i++) {
        double x = as_number(pa->data[i]);
        out[i] = make_number(x < 0.0 ? 0.0 : x);
    }
    return r;
}

// tensor_softmax(a): softmax over the whole array, computed as exp(x - max) / sum so large inputs
// do not overflow. An empty array gives an empty array.
uint64_t donky_tensor_softmax(uint64_t a) {
    const char* who = "tensor_softmax";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t n = pa->len;
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE || n == 0) return r;
    double m = -INFINITY;
    for (size_t i = 0; i < n; i++) { double x = as_number(pa->data[i]); if (x > m) m = x; }
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        double e = exp(as_number(pa->data[i]) - m);
        out[i] = make_number(e);
        sum += e;
    }
    for (size_t i = 0; i < n; i++) out[i] = make_number(as_number(out[i]) / sum);
    return r;
}

// tensor_relu_f32(a): max(x, 0) per element, computed in float. Same contract as tensor_relu;
// the returned array is an array of f64 values.
uint64_t donky_tensor_relu_f32(uint64_t a) {
    const char* who = "tensor_relu_f32";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t n = pa->len;
    float* fo = (float*)malloc(n ? n : 1);
    if (!fo) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s: out of memory", who);
        return dk_ffi_err(msg);
    }
    for (size_t i = 0; i < n; i++) {
        float x = (float)as_number(pa->data[i]);
        fo[i] = x < 0.0f ? 0.0f : x;
    }
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) { free(fo); return TAG_BASE; }
    for (size_t i = 0; i < n; i++) out[i] = make_number((double)fo[i]);
    free(fo);
    return r;
}

// tensor_softmax_f32(a): softmax over the whole array, computed in float as
// exp(x - max) / sum (the shift keeps large inputs from overflowing). Same contract as
// tensor_softmax; the returned array is an array of f64 values.
uint64_t donky_tensor_softmax_f32(uint64_t a) {
    const char* who = "tensor_softmax_f32";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t n = pa->len;
    if (n == 0) {
        uint64_t* out = NULL;
        return dk_tensor_alloc(who, 0, &out);
    }
    float* fo = (float*)malloc(n * sizeof(float));
    if (!fo) {
        char msg[64];
        snprintf(msg, sizeof msg, "%s: out of memory", who);
        return dk_ffi_err(msg);
    }
    float m = -INFINITY;
    for (size_t i = 0; i < n; i++) {
        float x = (float)as_number(pa->data[i]);
        if (x > m) m = x;
    }
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float e = expf((float)as_number(pa->data[i]) - m);
        fo[i] = e;
        sum += e;
    }
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) { free(fo); return TAG_BASE; }
    for (size_t i = 0; i < n; i++) out[i] = make_number((double)(fo[i] / sum));
    free(fo);
    return r;
}

// tensor_sgd_step(weights, grads, lr): weights[i] -= lr * grads[i], in place. Returns `weights`.
uint64_t donky_tensor_sgd_step(uint64_t weights, uint64_t grads, uint64_t lr) {
    const char* who = "tensor_sgd_step";
    Array* pw; Array* pg;
    char msg[128];
    if (!dk_tensor_view(who, "weights", weights, &pw) || !dk_tensor_view(who, "grads", grads, &pg)) return TAG_BASE;
    if (!is_number(lr)) {
        snprintf(msg, sizeof msg, "%s: lr must be a number, got %s", who, dk_type_name(lr));
        return dk_ffi_err(msg);
    }
    if (pw->len != pg->len) {
        snprintf(msg, sizeof msg, "%s: shape mismatch: weights has %zu elements, grads has %zu", who, pw->len, pg->len);
        return dk_ffi_err(msg);
    }
    double rate = as_number(lr);
    for (size_t i = 0; i < pw->len; i++)
        pw->data[i] = make_number(as_number(pw->data[i]) - rate * as_number(pg->data[i]));
    return weights;
}

// ---- Transformer primitives -------------------------------------------------
// All four take flat, row-major Donky arrays of numbers (NaN-boxed f64, as every tensor
// op does) and return a fresh flat array. Scratch buffers come from gc_malloc, not
// mem_alloc: the GC is the only allocator in this file that reclaims an orphaned block
// (the next sweep frees anything unreachable), so a throw between alloc and return
// cannot leak. Each function still frees its scratch explicitly on the success and
// error paths, so steady-state memory does not wait for a sweep.
//
// Shared helpers (dk_tensor_view / dk_tensor_dim / dk_tensor_alloc / dk_ffi_err /
// dk_type_name / make_number / as_number) are defined above in the tensor section.

// ------------------------------------------------------------------ donky_tensor_rms_norm  [+ dk_rms_norm_impl, optional donky_tensor_rms_norm_eps]
// RMSNorm over the whole vector: out[i] = x[i] / sqrt(mean(x^2) + eps), accumulated in double
// (n = 4096 cannot overflow unless |x| > ~1e152). NaN/inf in the input propagate as IEEE says; they
// are NOT masked. Note: inf in the input makes rms = inf, so every FINITE element becomes 0.
// This does not multiply by the learned norm weight; do that with tensor_mul afterwards.
static uint64_t dk_rms_norm_impl(const char* who, uint64_t a, double eps) {
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t n = pa->len;
    if (n == 0) {                                   // empty in, empty out
        uint64_t* out = NULL;
        return dk_tensor_alloc(who, 0, &out);
    }
    double ss = 0.0;
    for (size_t i = 0; i < n; i++) {
        double x = as_number(pa->data[i]);
        ss += x * x;
    }
    double rms = sqrt(ss / (double)n + eps);
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    for (size_t i = 0; i < n; i++)
        out[i] = make_number(as_number(pa->data[i]) / rms);
    return r;
}
 
// tensor_rms_norm(a): eps = 1e-5, the rms_norm_eps of Llama-2/3 and Mistral.
uint64_t donky_tensor_rms_norm(uint64_t a) {
    return dk_rms_norm_impl("tensor_rms_norm", a, 1e-5);
}
 
// tensor_rms_norm_eps(a, eps): same, with the epsilon from the model (GGUF
// "<arch>.attention.layer_norm_rms_epsilon"). OPTIONAL: needs registering as a 2-arg builtin.
uint64_t donky_tensor_rms_norm_eps(uint64_t a, uint64_t eps_val) {
    const char* who = "tensor_rms_norm_eps";
    if (!is_number(eps_val) || !(as_number(eps_val) >= 0.0) || !isfinite(as_number(eps_val))) {
        char msg[96];
        snprintf(msg, sizeof msg, "%s: eps must be a finite number >= 0, got %s", who, dk_type_name(eps_val));
        return dk_ffi_err(msg);
    }
    return dk_rms_norm_impl(who, a, as_number(eps_val));
}

// tensor_rope(a, pos): rotary positional embedding, applied to one vector `a` of even
// length d at token position `pos`. Uses the standard Llama/Mistral interleaved-pair
// layout: for each pair (x[2i], x[2i+1]),
//     theta_i = pos * base^(-2i/d),  base = 10000
//     y[2i]   = x[2i]   * cos(theta_i) - x[2i+1] * sin(theta_i)
//     y[2i+1] = x[2i]   * sin(theta_i) + x[2i+1] * cos(theta_i)
// `pos` must be a non-negative whole number. Odd-length vectors are rejected (a
// rotation is only defined on pairs).
uint64_t donky_tensor_rope(uint64_t a, uint64_t pos_val) {
    const char* who = "tensor_rope";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    if (!is_number(pos_val) || pos_val == TAG_BASE) {
        char msg[64]; snprintf(msg, sizeof msg, "%s: pos must be a number", who);
        return dk_ffi_err(msg);
    }
    double posd = as_number(pos_val);
    if (!(posd >= 0.0) || posd != floor(posd)) {
        char msg[64]; snprintf(msg, sizeof msg, "%s: pos must be a non-negative whole number", who);
        return dk_ffi_err(msg);
    }
    size_t n = pa->len;
    if (n & 1) {
        char msg[96]; snprintf(msg, sizeof msg, "%s: vector length must be even (got %zu)", who, n);
        return dk_ffi_err(msg);
    }
    if (n == 0) { uint64_t* out = NULL; return dk_tensor_alloc(who, 0, &out); }
    const double base = 10000.0;
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    // theta_i = pos * base^(-2i/n). Incremental: theta_{i+1}/theta_i = base^(-2/n).
    double step = pow(base, -2.0 / (double)n);
    double theta = posd;                            // theta_0 = pos * base^0
    for (size_t i = 0; i < n; i += 2) {
        double c = cos(theta), s = sin(theta);
        double x0 = as_number(pa->data[i]);
        double x1 = as_number(pa->data[i + 1]);
        out[i]     = make_number(x0 * c - x1 * s);
        out[i + 1] = make_number(x0 * s + x1 * c);
        theta *= step;
    }
    return r;
}

// tensor_rope_base(a, pos, base): same as tensor_rope, but with a configurable base
// frequency. Llama-3 uses base 500000, whereas Llama-2/Mistral use 10000. Positional
// embeddings computed with the wrong base produce subtly degraded outputs (not garbage,
// just less accurate reasoning on longer contexts).
uint64_t donky_tensor_rope_base(uint64_t a, uint64_t pos_val, uint64_t base_val) {
    const char* who = "tensor_rope_base";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    if (!is_number(pos_val) || pos_val == TAG_BASE) return dk_ffi_err("tensor_rope_base: pos must be a number");
    if (!is_number(base_val) || base_val == TAG_BASE) return dk_ffi_err("tensor_rope_base: base must be a number");
    double posd = as_number(pos_val);
    double base = as_number(base_val);
    if (!(posd >= 0.0) || posd != floor(posd)) return dk_ffi_err("tensor_rope_base: pos must be a non-negative whole number");
    if (!(base > 0.0)) return dk_ffi_err("tensor_rope_base: base must be positive");
    size_t n = pa->len;
    if (n & 1) return dk_ffi_err("tensor_rope_base: vector length must be even");
    if (n == 0) { uint64_t* out = NULL; return dk_tensor_alloc(who, 0, &out); }
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    double step = pow(base, -2.0 / (double)n);
    double theta = posd;
    for (size_t i = 0; i < n; i += 2) {
        double c = cos(theta), s = sin(theta);
        double x0 = as_number(pa->data[i]);
        double x1 = as_number(pa->data[i + 1]);
        out[i]     = make_number(x0 * c - x1 * s);
        out[i + 1] = make_number(x0 * s + x1 * c);
        theta *= step;
    }
    return r;
}

// ------------------------------------------------------------------ donky_tensor_attention
// tensor_attention(q, k, v, seq, d): single-head causal self-attention, flat row-major.
//     S[t][u] = dot(Q[t], K[u]) / sqrt(d)   for u <= t   (u > t is masked: contributes exactly 0)
//     P[t]    = softmax(S[t][0..t])          (max subtracted, so exp never overflows)
//     O[t]    = sum_u P[t][u] * V[u]
// Scratch is plain malloc/free (two buffers, freed on every path): nothing here is left for the GC.
// NaN/inf in q/k/v propagate; they are not masked.
uint64_t donky_tensor_attention(uint64_t q, uint64_t k, uint64_t v, uint64_t seq_val, uint64_t d_val) {
    const char* who = "tensor_attention";
    Array* pq; Array* pk; Array* pv;
    size_t seq, d;
    char msg[160];
    if (!dk_tensor_view(who, "q", q, &pq) ||
        !dk_tensor_view(who, "k", k, &pk) ||
        !dk_tensor_view(who, "v", v, &pv)) return TAG_BASE;
    if (!dk_tensor_dim(who, "seq", seq_val, &seq) || !dk_tensor_dim(who, "d", d_val, &d)) return TAG_BASE;
    if (d == 0) { snprintf(msg, sizeof msg, "%s: d must be positive", who); return dk_ffi_err(msg); }
    if (seq > 65536) { snprintf(msg, sizeof msg, "%s: seq too large (max 65536)", who); return dk_ffi_err(msg); }
    size_t n = seq * d;                            // seq<=65536, d<=2^31: fits size_t
    if (pq->len != n || pk->len != n || pv->len != n) {
        snprintf(msg, sizeof msg, "%s: q/k/v must each have seq*d = %zu elements (got %zu/%zu/%zu)",
                 who, n, pq->len, pk->len, pv->len);
        return dk_ffi_err(msg);
    }
    if (n == 0) { uint64_t* out = NULL; return dk_tensor_alloc(who, 0, &out); }
 
    double* srow = (double*)malloc(seq * sizeof(double));      // one score row
    double* orow = (double*)malloc(d * sizeof(double));        // one output row
    if (!srow || !orow) {
        free(srow); free(orow);
        snprintf(msg, sizeof msg, "%s: out of memory", who);
        return dk_ffi_err(msg);
    }
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) { free(srow); free(orow); return TAG_BASE; }
 
    const double scale = 1.0 / sqrt((double)d);
    for (size_t t = 0; t < seq; t++) {
        const uint64_t* qt = pq->data + t * d;
        double rowmax = -INFINITY;
        for (size_t u = 0; u <= t; u++) {              // causal: keys 0..t only
            const uint64_t* ku = pk->data + u * d;
            double dot = 0.0;
            for (size_t j = 0; j < d; j++) dot += as_number(qt[j]) * as_number(ku[j]);
            double sc = dot * scale;
            srow[u] = sc;
            if (sc > rowmax) rowmax = sc;
        }
        double sum = 0.0;
        for (size_t u = 0; u <= t; u++) {              // stable softmax: exp(s - max), the max entry gives exactly 1
            double e = exp(srow[u] - rowmax);
            srow[u] = e;
            sum += e;
        }
        for (size_t j = 0; j < d; j++) orow[j] = 0.0;
        for (size_t u = 0; u <= t; u++) {
            double p = srow[u] / sum;
            const uint64_t* vu = pv->data + u * d;
            for (size_t j = 0; j < d; j++) orow[j] += p * as_number(vu[j]);
        }
        for (size_t j = 0; j < d; j++) out[t * d + j] = make_number(orow[j]);
    }
    free(srow); free(orow);
    return r;
}

// tensor_attention_decode(q, kc, vc, n_pos, n_heads, n_heads_kv, d_head):
//   Single-token causal attention. The query is the newest token; all cached
//   positions are visible (no mask). Grouped-query: query head h reads KV head
//   floor(h / (n_heads / n_heads_kv)).
//   q   [n_heads * d_head]              flat
//   kc  [n_pos * n_heads_kv * d_head]   flat (RoPE already applied)
//   vc  [n_pos * n_heads_kv * d_head]   flat
//   Returns [n_heads * d_head] flat.
//
// Uses AVX2 for the Q·K dot product and the P·V accumulation (dk_dot_f32 /
// dk_axpy_f32, defined with the other SIMD helpers further down); without
// AVX2 it falls back to the same double-accumulator scalar path as dk_dot256.
// The two-phase loop (all scores, then the weighted sum) unboxes each cached
// K/V row exactly once even when several query heads share a KV head.
//
// n_pos == 0 is legal: no cached keys means nothing to attend to, so the
// result is a zero vector (the query is scaled by 0).
static double dk_dot_f32(const float* a, const float* b, size_t n);          // defined with the SIMD helpers
static void   dk_axpy_f32(float alpha, const float* x, float* y, size_t n);  // defined with the SIMD helpers

uint64_t donky_tensor_attention_decode(uint64_t q, uint64_t kc, uint64_t vc,
                                       uint64_t n_pos_val, uint64_t n_heads_val,
                                       uint64_t n_heads_kv_val, uint64_t d_head_val) {
    const char* who = "tensor_attention_decode";
    Array* pq; Array* pkc; Array* pvc;
    size_t n_pos, n_heads, n_heads_kv, d_head;
    char msg[200];
    if (!dk_tensor_view(who, "q",  q,  &pq)  ||
        !dk_tensor_view(who, "kc", kc, &pkc) ||
        !dk_tensor_view(who, "vc", vc, &pvc)) return TAG_BASE;
    if (!dk_tensor_dim(who, "n_pos",      n_pos_val,      &n_pos)      ||
        !dk_tensor_dim(who, "n_heads",    n_heads_val,    &n_heads)    ||
        !dk_tensor_dim(who, "n_heads_kv", n_heads_kv_val, &n_heads_kv) ||
        !dk_tensor_dim(who, "d_head",     d_head_val,     &d_head))    return TAG_BASE;

    // n_heads*d_head and n_pos*n_heads_kv*d_head must not overflow size_t.
    if (n_heads != 0 && d_head > SIZE_MAX / n_heads) {
        snprintf(msg, sizeof msg, "%s: n_heads * d_head is too large", who);
        return dk_ffi_err(msg);
    }
    size_t out_n = n_heads * d_head;
    if (n_heads_kv != 0 && d_head > SIZE_MAX / n_heads_kv) {
        snprintf(msg, sizeof msg, "%s: n_heads_kv * d_head is too large", who);
        return dk_ffi_err(msg);
    }
    size_t kv_dim = n_heads_kv * d_head;
    if (n_pos != 0 && kv_dim > SIZE_MAX / n_pos) {
        snprintf(msg, sizeof msg, "%s: n_pos * n_heads_kv * d_head is too large", who);
        return dk_ffi_err(msg);
    }
    size_t kv_n = n_pos * kv_dim;
    // n_heads * n_pos floats for the score table.
    if (n_pos != 0 && n_heads > (SIZE_MAX / sizeof(float)) / n_pos) {
        snprintf(msg, sizeof msg, "%s: n_heads * n_pos is too large", who);
        return dk_ffi_err(msg);
    }

    if (out_n == 0) { uint64_t* out = NULL; return dk_tensor_alloc(who, 0, &out); }
    if (n_heads_kv == 0) {
        snprintf(msg, sizeof msg, "%s: n_heads_kv must be positive", who);
        return dk_ffi_err(msg);
    }
    if (n_heads % n_heads_kv != 0) {
        snprintf(msg, sizeof msg,
                 "%s: n_heads (%zu) must be a multiple of n_heads_kv (%zu)",
                 who, n_heads, n_heads_kv);
        return dk_ffi_err(msg);
    }
    if (pq->len != out_n) {
        snprintf(msg, sizeof msg,
                 "%s: q has %zu elements but n_heads * d_head = %zu * %zu = %zu",
                 who, pq->len, n_heads, d_head, out_n);
        return dk_ffi_err(msg);
    }
    if (pkc->len != kv_n || pvc->len != kv_n) {
        snprintf(msg, sizeof msg,
                 "%s: kc/vc must each have n_pos * n_heads_kv * d_head = %zu elements (got %zu/%zu)",
                 who, kv_n, pkc->len, pvc->len);
        return dk_ffi_err(msg);
    }

    // No cached positions: attend to nothing, so every output element is 0.
    if (n_pos == 0) { uint64_t* out = NULL; return dk_tensor_alloc(who, out_n, &out); }

    // Scratch: the query unboxed once, one score per (head, position), and one
    // K and one V row (d_head floats each) unboxed per cached position, plus a
    // float accumulator for the P·V weighted sum.
    float* fq     = (float*)malloc(out_n * sizeof(float));
    float* scores = (float*)malloc(n_heads * n_pos * sizeof(float));
    float* krow   = (float*)malloc(d_head * sizeof(float));
    float* vrow   = (float*)malloc(d_head * sizeof(float));
    float* facc   = (float*)calloc(out_n, sizeof(float));   // all-zero
    if (!fq || !scores || !krow || !vrow || !facc) {
        free(fq); free(scores); free(krow); free(vrow); free(facc);
        snprintf(msg, sizeof msg, "%s: out of memory", who);
        return dk_ffi_err(msg);
    }
    for (size_t i = 0; i < out_n; i++) fq[i] = (float)as_number(pq->data[i]);

    const size_t group = n_heads / n_heads_kv;
    const double scale = 1.0 / sqrt((double)d_head);

    // Phase 1: Q·K logits. Each cached K row is unboxed once and reused by the
    // `group` query heads that share its KV head. scores[h * n_pos + p].
    for (size_t p = 0; p < n_pos; p++) {
        for (size_t kvh = 0; kvh < n_heads_kv; kvh++) {
            const size_t base = (p * n_heads_kv + kvh) * d_head;
            for (size_t j = 0; j < d_head; j++) krow[j] = (float)as_number(pkc->data[base + j]);
            for (size_t g = 0; g < group; g++) {
                const size_t h = kvh * group + g;
                const double dot = dk_dot_f32(fq + h * d_head, krow, d_head);
                scores[h * n_pos + p] = (float)(dot * scale);
            }
        }
    }

    // Softmax per query head (max subtracted, so exp never overflows).
    for (size_t h = 0; h < n_heads; h++) {
        float* row = scores + h * n_pos;
        float mx = row[0];
        for (size_t p = 1; p < n_pos; p++) if (row[p] > mx) mx = row[p];
        double sum = 0.0;
        for (size_t p = 0; p < n_pos; p++) {
            const float e = (float)exp((double)row[p] - (double)mx);
            row[p] = e;
            sum += e;
        }
        const float inv = (float)(1.0 / sum);
        for (size_t p = 0; p < n_pos; p++) row[p] *= inv;
    }

    // Phase 2: P·V. Each cached V row is unboxed once and FMA'd into every query
    // head that shares its KV head.
    for (size_t p = 0; p < n_pos; p++) {
        for (size_t kvh = 0; kvh < n_heads_kv; kvh++) {
            const size_t base = (p * n_heads_kv + kvh) * d_head;
            for (size_t j = 0; j < d_head; j++) vrow[j] = (float)as_number(pvc->data[base + j]);
            for (size_t g = 0; g < group; g++) {
                const size_t h = kvh * group + g;
                dk_axpy_f32(scores[h * n_pos + p], vrow, facc + h * d_head, d_head);
            }
        }
    }

    // Box the float accumulator into the Donky result (NaN gets canonicalized by make_number).
    uint64_t* out = NULL;
    uint64_t res = dk_tensor_alloc(who, out_n, &out);
    if (res == TAG_BASE) {
        free(fq); free(scores); free(krow); free(vrow); free(facc);
        return TAG_BASE;
    }
    for (size_t i = 0; i < out_n; i++) out[i] = make_number((double)facc[i]);
    free(fq); free(scores); free(krow); free(vrow); free(facc);
    return res;
}

// tensor_silu(a): x * sigmoid(x) = x / (1 + exp(-x)), element-wise.
// Uses the overflow-safe form: for large positive x, exp(-x) underflows to 0 -> x;
// for large negative x, exp(-x) overflows to +inf -> x/inf = -0, which is 0.0.
uint64_t donky_tensor_silu(uint64_t a) {
    const char* who = "tensor_silu";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t n = pa->len;
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    for (size_t i = 0; i < n; i++) {
        double x = as_number(pa->data[i]);
        double e = exp(-x);
        double y = x / (1.0 + e);                  // NaN in -> NaN out (canonicalised by make_number)
        out[i] = make_number(y);
    }
    return r;
}

// mmap_model(path) -> { addr, size } | null
// Opens `path`, maps it read-only (shared), and returns the base address and file size as
// numbers. The mapping is NOT GC-tracked and is intentionally never unmapped: a model is
// mapped for the process lifetime. Any handle returned by dll_call / ffi_alloc is also a
// plain number, so the map slots in with the existing FFI conventions.
uint64_t donky_mmap_model(uint64_t path_val) {
    if (!dk_is_str(path_val)) return dk_ffi_err("mmap_model: path must be a string");
    const char* path = dk_s(path_val);
#ifdef _WIN32
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE)
        return dk_ffi_err("mmap_model: cannot open file");
    LARGE_INTEGER sz; sz.QuadPart = 0;
    if (!GetFileSizeEx(hf, &sz) || sz.QuadPart <= 0) {
        CloseHandle(hf);
        return dk_ffi_err("mmap_model: empty or unreadable file");
    }
    HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hm) { CloseHandle(hf); return dk_ffi_err("mmap_model: CreateFileMapping failed"); }
    void* base = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hm); CloseHandle(hf);           // mapping keeps the file alive
    if (!base) return dk_ffi_err("mmap_model: MapViewOfFile failed");
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return dk_ffi_err("mmap_model: cannot open file");
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return dk_ffi_err("mmap_model: empty or unreadable file");
    }
    void* base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);                                  // mapping keeps the file alive
    if (base == MAP_FAILED) return dk_ffi_err("mmap_model: mmap failed");
#endif
    uint64_t m = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("addr"), make_number((double)(uint64_t)(uintptr_t)base));
    #ifdef _WIN32
    donky_map_set(m, donky_alloc_string("size"), make_number((double)(uint64_t)sz.QuadPart));
#else
    donky_map_set(m, donky_alloc_string("size"), make_number((double)(uint64_t)st.st_size));
#endif
    return m;
}

// GGUF constants (v1..v3 all share this layout for the header + KV block)
#define GGUF_MAGIC   0x46554747u            // "GGUF" little-endian on disk
#define GGUF_MAX_KV  65536u                 // sanity cap: no real model has more
#define GGUF_MAX_STR (64u * 1024u * 1024u)  // sanity cap: strings, not blob data

typedef struct { const uint8_t* p; const uint8_t* end; } GgufRd;
static int gguf_take(GgufRd* r, size_t n, const uint8_t** out) {
    if ((size_t)(r->end - r->p) < n) return 0;
    *out = r->p; r->p += n; return 1;
}
static int gguf_u32(GgufRd* r, uint32_t* v) {
    const uint8_t* q; if (!gguf_take(r, 4, &q)) return 0;
    *v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
    return 1;
}
static int gguf_u64(GgufRd* r, uint64_t* v) {
    const uint8_t* q; if (!gguf_take(r, 8, &q)) return 0;
    *v = (uint64_t)q[0] | ((uint64_t)q[1] << 8) | ((uint64_t)q[2] << 16) | ((uint64_t)q[3] << 24)
       | ((uint64_t)q[4] << 32) | ((uint64_t)q[5] << 40) | ((uint64_t)q[6] << 48) | ((uint64_t)q[7] << 56);
    return 1;
}
// GGUF string: u64 length + bytes. Returns a NUL-terminated copy the caller frees.
static char* gguf_string(GgufRd* r) {
    uint64_t n; if (!gguf_u64(r, &n) || n > GGUF_MAX_STR) return NULL;
    const uint8_t* q; if (!gguf_take(r, (size_t)n, &q)) return NULL;
    char* s = (char*)malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, q, (size_t)n); s[n] = 0; return s;
}

// gguf_type codes (GGUF v2/v3)
enum { GGUF_T_U8=0, GGUF_T_I8, GGUF_T_U16, GGUF_T_I16, GGUF_T_U32, GGUF_T_I32,
       GGUF_T_F32, GGUF_T_BOOL, GGUF_T_STRING, GGUF_T_ARRAY, GGUF_T_U64, GGUF_T_I64, GGUF_T_F64 };

// Skips a value of the given type. For arrays, reads the element type + length and walks.
static int gguf_skip(GgufRd* r, uint32_t type) {
    const uint8_t* q; uint32_t et; uint64_t n;
    switch (type) {
        case GGUF_T_U8: case GGUF_T_I8: case GGUF_T_BOOL: return gguf_take(r, 1, &q);
        case GGUF_T_U16: case GGUF_T_I16: return gguf_take(r, 2, &q);
        case GGUF_T_U32: case GGUF_T_I32: case GGUF_T_F32: return gguf_take(r, 4, &q);
        case GGUF_T_U64: case GGUF_T_I64: case GGUF_T_F64: return gguf_take(r, 8, &q);
        case GGUF_T_STRING: { char* s = gguf_string(r); if (!s) return 0; free(s); return 1; }
        case GGUF_T_ARRAY: {
            if (!gguf_u32(r, &et) || !gguf_u64(r, &n)) return 0;
            if (n > GGUF_MAX_STR) return 0;         // arrays of scalars/strings, not blob bodies
            for (uint64_t i = 0; i < n; i++) if (!gguf_skip(r, et)) return 0;
            return 1;
        }
    }
    return 0;
}

// Returns the integer/floating value of the next scalar as a double (used for metadata).
static int gguf_scalar_as_double(GgufRd* r, uint32_t type, double* out) {
    const uint8_t* q;
    switch (type) {
        case GGUF_T_U8:  { if (!gguf_take(r,1,&q)) return 0; *out = (double)q[0]; return 1; }
        case GGUF_T_I8:  { if (!gguf_take(r,1,&q)) return 0; *out = (double)(int8_t)q[0]; return 1; }
        case GGUF_T_U16: { if (!gguf_take(r,2,&q)) return 0; *out = (double)((uint32_t)q[0]|((uint32_t)q[1]<<8)); return 1; }
        case GGUF_T_I16: { if (!gguf_take(r,2,&q)) return 0; *out = (double)(int16_t)((uint32_t)q[0]|((uint32_t)q[1]<<8)); return 1; }
        case GGUF_T_U32: { uint32_t v; if (!gguf_u32(r,&v)) return 0; *out=(double)v; return 1; }
        case GGUF_T_I32: { uint32_t v; if (!gguf_u32(r,&v)) return 0; *out=(double)(int32_t)v; return 1; }
        case GGUF_T_U64: { uint64_t v; if (!gguf_u64(r,&v)) return 0; *out=(double)v; return 1; }
        case GGUF_T_I64: { uint64_t v; if (!gguf_u64(r,&v)) return 0; *out=(double)(int64_t)v; return 1; }
        case GGUF_T_F32: { uint32_t v; if (!gguf_u32(r,&v)) return 0; float f; memcpy(&f,&v,4); *out=(double)f; return 1; }
        case GGUF_T_F64: { uint64_t v; if (!gguf_u64(r,&v)) return 0; double d; memcpy(&d,&v,8); *out=d; return 1; }
        case GGUF_T_BOOL: { if (!gguf_take(r,1,&q)) return 0; *out = q[0] ? 1.0 : 0.0; return 1; }
    }
    return 0;
}

// ===== Tasks 2+3: replace the whole donky_read_gguf_metadata (and its comment) =====
// read_gguf_metadata(addr) -> { "d_model","n_heads","n_heads_kv","layers","context","ffn",
//                               "rope_base","arch",
//                               "tensor_data_offset",   // aligned file offset of the data section
//                               "tensor_table" } | throws
// tensor_table: name -> { "type": ggml type code (0=F32 1=F16 2=Q4_0 8=Q8_0 12=Q4_K 14=Q6_K ...),
//                         "offset": byte offset RELATIVE to the data section (as stored in the file),
//                         "abs_offset": tensor_data_offset + offset (add to the mmap base for the pointer),
//                         "dims": [ne0, ne1, ...] }   // GGUF order: dims[0] is the contiguous/input dim,
//                                                    // so a weight with dims [4096, 14336] is 14336 rows x 4096 cols.
uint64_t donky_read_gguf_metadata(uint64_t addr_val) {
    if (!is_number(addr_val)) return dk_ffi_err("read_gguf_metadata: addr must be a number from mmap_model");
    const uint8_t* base = (const uint8_t*)(uintptr_t)(uint64_t)as_number(addr_val);
    if (!base) return dk_ffi_err("read_gguf_metadata: null address");
    GgufRd r = { base, base + (64u * 1024u * 1024u) };   // conservative cap
 
    uint32_t magic, ver; uint64_t n_tensors, n_kv;
    if (!gguf_u32(&r, &magic) || magic != GGUF_MAGIC) return dk_ffi_err("read_gguf_metadata: bad magic");
    if (!gguf_u32(&r, &ver) || ver < 1 || ver > 3)    return dk_ffi_err("read_gguf_metadata: unsupported version");
    if (!gguf_u64(&r, &n_tensors) || n_tensors > (1u << 20)) return dk_ffi_err("read_gguf_metadata: bad n_tensors");
    if (!gguf_u64(&r, &n_kv) || n_kv > GGUF_MAX_KV)   return dk_ffi_err("read_gguf_metadata: bad n_kv");
 
    char arch[64] = "";
    double d_model = 0, n_heads = 0, n_heads_kv = 0, layers = 0, ctx = 0, ffn = 0, rope = 0;
    double align_kv = 0; // general.alignment, if present
    const uint8_t* vocab_hdr = NULL; double n_vocab = 0; // tokenizer.ggml.tokens: array header address + count                                  // general.alignment, if present
    for (uint64_t i = 0; i < n_kv; i++) {
        char* key = gguf_string(&r);
        if (!key) return dk_ffi_err("read_gguf_metadata: bad key");
        uint32_t type; if (!gguf_u32(&r, &type)) { free(key); return dk_ffi_err("read_gguf_metadata: truncated KV"); }
        double v = 0; int scalar = 0;
        if (type == GGUF_T_STRING) {
            char* s = gguf_string(&r);
            if (!s) { free(key); return dk_ffi_err("read_gguf_metadata: bad string value"); }
            if (strcmp(key, "general.architecture") == 0) {
                strncpy(arch, s, sizeof arch - 1); arch[sizeof arch - 1] = 0;
            }
            free(s);
        } else if (type == GGUF_T_ARRAY) {
            const uint8_t* arr_start = r.p;
            if (!gguf_skip(&r, type)) { free(key); return dk_ffi_err("read_gguf_metadata: bad array"); }
            if (strcmp(key, "tokenizer.ggml.tokens") == 0) {
                GgufRd h = { arr_start, r.p };
                uint32_t et; uint64_t cnt;
                if (gguf_u32(&h, &et) && gguf_u64(&h, &cnt) && et == GGUF_T_STRING) { vocab_hdr = arr_start; n_vocab = (double)cnt; }
            }
        } else {
            if (!gguf_scalar_as_double(&r, type, &v)) { free(key); return dk_ffi_err("read_gguf_metadata: bad scalar"); }
            scalar = 1;
        }
        if (scalar) {
            if      (strcmp(key, "general.alignment") == 0)                 align_kv  = v;
            else if (strstr(key, ".embedding_length"))                      d_model   = v;
            else if (strstr(key, ".attention.head_count_kv"))               n_heads_kv= v;
            else if (strstr(key, ".attention.head_count"))                  n_heads   = v;
            else if (strstr(key, ".block_count"))                           layers    = v;
            else if (strstr(key, ".context_length"))                        ctx       = v;
            else if (strstr(key, ".feed_forward_length"))                   ffn       = v;
            else if (strstr(key, ".rope.freq_base"))                        rope      = v;
        }
        free(key);
    }
 
    // Result map first, so everything allocated below is reachable from it.
    uint64_t m = donky_alloc_map();
    uint64_t tensor_table = donky_alloc_map();
    donky_map_set(m, donky_alloc_string("tensor_table"), tensor_table);
 
    // Tensor-info records: name (u64 len + bytes), n_dims (u32), dims (u64 * n_dims),
    // type (u32), offset (u64, relative to the data section). Offsets are stored raw here;
    // abs_offset is filled in once the aligned data start is known (second pass below).
    // Truncation is an error: a silent break would give a wrong tensor_data_offset.
    typedef struct { char* name; uint32_t type; uint64_t off; uint32_t nd; uint64_t dims[8]; } DkGgufTI;
    DkGgufTI* ti = (DkGgufTI*)calloc(n_tensors ? (size_t)n_tensors : 1, sizeof(DkGgufTI));
    if (!ti) return dk_ffi_err("read_gguf_metadata: out of memory");
    const char* bad = NULL;
    uint64_t got = 0;
    for (; got < n_tensors; got++) {
        DkGgufTI* t = &ti[got];
        t->name = gguf_string(&r);
        if (!t->name)                                   { bad = "truncated tensor name"; break; }
        if (!gguf_u32(&r, &t->nd) || t->nd == 0 || t->nd > 8) { got++; bad = "bad tensor n_dims"; break; }
        int ok = 1;
        for (uint32_t d = 0; d < t->nd && ok; d++) ok = gguf_u64(&r, &t->dims[d]);
        if (!ok || !gguf_u32(&r, &t->type) || !gguf_u64(&r, &t->off)) { got++; bad = "truncated tensor info"; break; }
    }
    size_t alignment = 32;
    if (align_kv >= 1 && align_kv <= 1048576 && align_kv == floor(align_kv)) alignment = (size_t)align_kv;
    size_t hdr_end = (size_t)(r.p - base);
    size_t aligned = ((hdr_end + alignment - 1) / alignment) * alignment;
 
    if (!bad) {
        for (uint64_t i = 0; i < n_tensors; i++) {
            DkGgufTI* t = &ti[i];
            uint64_t info = donky_alloc_map();
            uint64_t* dd = NULL;
            uint64_t dims_v = dk_tensor_alloc("read_gguf_metadata", t->nd, &dd);
            if (dims_v == TAG_BASE) { bad = "out of memory"; break; }
            for (uint32_t d = 0; d < t->nd; d++) dd[d] = make_number((double)t->dims[d]);
            donky_map_set(info, donky_alloc_string("type"),       make_number((double)t->type));
            donky_map_set(info, donky_alloc_string("offset"),     make_number((double)t->off));
            donky_map_set(info, donky_alloc_string("abs_offset"), make_number((double)((uint64_t)aligned + t->off)));
            donky_map_set(info, donky_alloc_string("dims"),       dims_v);
            donky_map_set(tensor_table, donky_alloc_string(t->name), info);
        }
    }
    for (uint64_t i = 0; i < got && i < n_tensors; i++) free(ti[i].name);
    free(ti);
    if (bad) {
        char msg[96]; snprintf(msg, sizeof msg, "read_gguf_metadata: %s", bad);
        return dk_ffi_err(msg);
    }
 
    if (d_model)    donky_map_set(m, donky_alloc_string("d_model"),    make_number(d_model));
    if (n_heads)    donky_map_set(m, donky_alloc_string("n_heads"),    make_number(n_heads));
    if (n_heads_kv) donky_map_set(m, donky_alloc_string("n_heads_kv"), make_number(n_heads_kv));
    if (layers)     donky_map_set(m, donky_alloc_string("layers"),     make_number(layers));
    if (ctx)        donky_map_set(m, donky_alloc_string("context"),    make_number(ctx));
    if (ffn)        donky_map_set(m, donky_alloc_string("ffn"),        make_number(ffn));
    if (rope)       donky_map_set(m, donky_alloc_string("rope_base"),  make_number(rope));
    if (arch[0])    donky_map_set(m, donky_alloc_string("arch"),       donky_alloc_string(arch));
    if (vocab_hdr) {
        donky_map_set(m, donky_alloc_string("vocab_addr"), make_number((double)(uint64_t)(uintptr_t)vocab_hdr));
        donky_map_set(m, donky_alloc_string("n_vocab"), make_number(n_vocab));
    }
    donky_map_set(m, donky_alloc_string("tensor_data_offset"), make_number((double)aligned));
    return m;
}

// ---- Q4_K dequantization ---------------------------------------------------
// llama.cpp block_q4_K: 144 bytes, 256 weights. d/dmin are IEEE fp16.
typedef struct __attribute__((packed)) {
    uint16_t d;          // fp16 super-block scale
    uint16_t dmin;       // fp16 super-block min
    uint8_t  scales[12]; // 6-bit packed sub-block scales/mins (8 each)
    uint8_t  qs[128];    // 256 nibbles
} DkQ4KBlock;
typedef char dk_q4k_size_check[(sizeof(DkQ4KBlock) == 144) ? 1 : -1];

static inline float dk_fp16_to_fp32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) & 1;
    uint32_t exp  = (uint32_t)(h >> 10) & 0x1F;
    uint32_t mant = (uint32_t)h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) { f = sign << 31; }
        else {                            // subnormal: normalize
            int e = -1;
            do { e++; mant <<= 1; } while ((mant & 0x400) == 0);
            mant &= 0x3FF;
            f = (sign << 31) | ((uint32_t)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {             // inf / nan
        f = (sign << 31) | 0x7F800000u | (mant << 13);
    } else {
        f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out; memcpy(&out, &f, 4); return out;
}

// Unpack the 8 6-bit scales and 8 6-bit mins from the 12 packed bytes (llama.cpp's
// get_scale_min_k4). Result: sc[0..7], mn[0..7] in [0,63], scaled by d / dmin.
static inline void get_scale_min_k4(int j, const uint8_t* q, uint8_t* d, uint8_t* m) {
    if (j < 4) {
        *d = (uint8_t)(q[j]     & 63);
        *m = (uint8_t)(q[j + 4] & 63);
    } else {
        *d = (uint8_t)((q[j + 4] & 0x0F) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4)   | ((q[j]     >> 6) << 4));
    }
}

// Dequantize one 256-weight Q4_K block (ggml dequantize_row_q4_K).
// Each 32-byte qs segment holds TWO sub-blocks: low nibbles -> weights [j, j+32),
// high nibbles -> weights [j+32, j+64); then j += 64, q += 32, is += 2.
static void dk_q4k_dequant(const DkQ4KBlock* blk, float* dst) {
    const float d    = dk_fp16_to_fp32(blk->d);
    const float dmin = dk_fp16_to_fp32(blk->dmin);
    const uint8_t* q = blk->qs;
    int is = 0;
    for (int j = 0; j < 256; j += 64) {
        uint8_t sc0, mn0, sc1, mn1;
        get_scale_min_k4(is + 0, blk->scales, &sc0, &mn0);
        get_scale_min_k4(is + 1, blk->scales, &sc1, &mn1);
        const float d1 = d * (float)sc0, m1 = dmin * (float)mn0;
        const float d2 = d * (float)sc1, m2 = dmin * (float)mn1;
        for (int l = 0; l < 32; ++l) dst[j + l]      = d1 * (float)(q[l] & 0x0F) - m1;
        for (int l = 0; l < 32; ++l) dst[j + 32 + l] = d2 * (float)(q[l] >> 4)   - m2;
        q += 32;
        is += 2;
    }
}

// ffi_peek_q4_row(ptr, row_id, n_embd): extracts a single row of Q4_K weights
// from a quantized matrix and dequantizes it to a fresh f64 Donky array.
uint64_t donky_ffi_peek_q4_row(uint64_t ptr_val, uint64_t row_id_val, uint64_t n_embd_val) {
    if (!is_number(ptr_val) || !is_number(row_id_val) || !is_number(n_embd_val)) return TAG_BASE;
    size_t row_id = (size_t)as_number(row_id_val);
    size_t n_embd = (size_t)as_number(n_embd_val);
    if (n_embd == 0 || (n_embd % 256) != 0) return dk_ffi_err("ffi_peek_q4_row: n_embd must be a multiple of 256");
    
    const uint8_t* base = (const uint8_t*)(uintptr_t)(uint64_t)as_number(ptr_val);
    if (!base) return dk_ffi_err("ffi_peek_q4_row: null pointer");
    
    size_t blocks_per_row = n_embd / 256;
    // The row starts at: base + (row_id * blocks_per_row * sizeof(DkQ4KBlock))
    const uint8_t* rowp = base + (row_id * blocks_per_row * sizeof(DkQ4KBlock));
    
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc("ffi_peek_q4_row", n_embd, &out);
    if (r == TAG_BASE) return TAG_BASE;
    
    float block[256];
    for (size_t b = 0; b < blocks_per_row; b++) {
        const DkQ4KBlock* blk = (const DkQ4KBlock*)(rowp + b * sizeof(DkQ4KBlock));
        dk_q4k_dequant(blk, block);
        for (int i = 0; i < 256; i++) {
            out[b * 256 + i] = make_number((double)block[i]);
        }
    }
    return r;
}

// ------------------------------------------------------------------ donky_tensor_matmul_q4  [+ dk_dot256]
// ==== BEGIN dk_simd_mt (SIMD dot product + fork/join row parallelism for the GEMM kernels) ====

// 256-element dot product of a dequantized block against an activation slice.
//
// Scalar path: four independent double accumulators (deterministic, double precision).
static double dk_dot256_scalar(const float* w, const float* x) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    for (int i = 0; i < 256; i += 4) {
        s0 += (double)w[i]     * (double)x[i];
        s1 += (double)w[i + 1] * (double)x[i + 1];
        s2 += (double)w[i + 2] * (double)x[i + 2];
        s3 += (double)w[i + 3] * (double)x[i + 3];
    }
    return (s0 + s1) + (s2 + s3);
}

// AVX2+FMA path: 8 float multiply-adds per instruction. target("avx2,fma") enables the instructions for
// this one function only (no -mavx2 build flag), and dk_use_avx2() checks the CPU at run time, so a single
// runtime.o still runs on CPUs without AVX2. x86 with GCC/Clang only; other targets use the scalar path.
// Accumulation is in float (8 lanes x 32 steps, then one float horizontal sum): about 1e-6 relative error per
// block against the double path, and results can differ in the last bits between AVX2 and non-AVX2 machines.
// Set DONKY_NO_AVX2=1 to force the scalar path (bit-identical results everywhere).
#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#define DK_HAVE_AVX2_PATH 1
#include <immintrin.h>

__attribute__((target("avx2,fma")))
static double dk_dot256_avx2(const float* w, const float* x) {
    __m256 sum = _mm256_setzero_ps();
    for (int i = 0; i < 256; i += 8) {
        __m256 vw = _mm256_loadu_ps(w + i);
        __m256 vx = _mm256_loadu_ps(x + i);
        sum = _mm256_fmadd_ps(vw, vx, sum);
    }
    // Horizontal sum of the 8 floats in 'sum'
    __m128 low = _mm256_castps256_ps128(sum);
    __m128 high = _mm256_extractf128_ps(sum, 1);
    __m128 sum128 = _mm_add_ps(low, high);
    sum128 = _mm_hadd_ps(sum128, sum128);
    sum128 = _mm_hadd_ps(sum128, sum128);
    return (double)_mm_cvtss_f32(sum128);
}

static int dk_use_avx2(void) {
    static int cached = -1;                       // every thread computes the same value, so a relaxed race is harmless
    int v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (v < 0) {
        __builtin_cpu_init();
        v = (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) ? 1 : 0;
        const char* off = getenv("DONKY_NO_AVX2");
        if (off && off[0] && off[0] != '0') v = 0;
        __atomic_store_n(&cached, v, __ATOMIC_RELAXED);
    }
    return v;
}
#endif

static inline double dk_dot256(const float* w, const float* x) {
#ifdef DK_HAVE_AVX2_PATH
    if (dk_use_avx2()) return dk_dot256_avx2(w, x);
#endif
    return dk_dot256_scalar(w, x);
}

// General float dot product and scaled add, for kernels whose length is not the
// fixed 256 of a quant block (tensor_attention_decode uses d_head: 128 for
// Llama-3-8B). Same scheme as dk_dot256: AVX2+FMA 8-wide when the CPU has it
// and n >= 8, otherwise a plain loop; DONKY_NO_AVX2=1 forces the scalar path.
static double dk_dot_f32_scalar(const float* a, const float* b, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += (double)a[i] * (double)b[i];
    return s;
}

#ifdef DK_HAVE_AVX2_PATH
__attribute__((target("avx2,fma")))
static double dk_dot_f32_avx2(const float* a, const float* b, size_t n) {
    __m256 sum = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 va = _mm256_loadu_ps(a + i);
        __m256 vb = _mm256_loadu_ps(b + i);
        sum = _mm256_fmadd_ps(va, vb, sum);
    }
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 s  = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    double tail = (double)_mm_cvtss_f32(s);
    for (; i < n; i++) tail += (double)a[i] * (double)b[i];   // n % 8 remainder
    return tail;
}
#endif

static double dk_dot_f32(const float* a, const float* b, size_t n) {
#ifdef DK_HAVE_AVX2_PATH
    if (n >= 8 && dk_use_avx2()) return dk_dot_f32_avx2(a, b, n);
#endif
    return dk_dot_f32_scalar(a, b, n);
}

static void dk_axpy_f32_scalar(float alpha, const float* x, float* y, size_t n) {
    for (size_t i = 0; i < n; i++) y[i] += alpha * x[i];
}

#ifdef DK_HAVE_AVX2_PATH
__attribute__((target("avx2,fma")))
static void dk_axpy_f32_avx2(float alpha, const float* x, float* y, size_t n) {
    const __m256 va = _mm256_set1_ps(alpha);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 vy = _mm256_loadu_ps(y + i);
        __m256 vx = _mm256_loadu_ps(x + i);
        vy = _mm256_fmadd_ps(va, vx, vy);
        _mm256_storeu_ps(y + i, vy);
    }
    for (; i < n; i++) y[i] += alpha * x[i];                  // n % 8 remainder
}
#endif

static void dk_axpy_f32(float alpha, const float* x, float* y, size_t n) {
#ifdef DK_HAVE_AVX2_PATH
    if (n >= 8 && dk_use_avx2()) { dk_axpy_f32_avx2(alpha, x, y, n); return; }
#endif
    dk_axpy_f32_scalar(alpha, x, y, n);
}

// ---- fork/join over output rows -------------------------------------------------------------------
// The kernels below write acc[bi * R + row]; each row belongs to exactly one thread, so there is no
// sharing, no locking, and every element is still summed in ascending block order: results do not
// depend on the thread count. Workers touch only malloc'd buffers and the (read-only) weights. They
// never allocate Donky objects, never throw, and never call the GC, so they are safe next to the
// Donky task pool. Needs gcc/clang (__sync/__atomic builtins), like the rest of the runtime build.
#ifdef _WIN32
#define DK_PAR_RET unsigned __stdcall
#define DK_PAR_OK  0
#else
#define DK_PAR_RET void*
#define DK_PAR_OK  NULL
#endif
#define DK_PAR_MAX 64              // also WaitForMultipleObjects' limit; we join one handle at a time anyway

typedef void (*DkRowFn)(void* ctx, size_t row_start, size_t row_end);
typedef struct { DkRowFn fn; void* ctx; size_t row_start, row_end; } DkParTask;

static DK_PAR_RET dk_par_trampoline(void* p) {
    DkParTask* t = (DkParTask*)p;
    t->fn(t->ctx, t->row_start, t->row_end);
    return DK_PAR_OK;
}

// Threads to use: DONKY_THREADS if set (1..64), else min(8, logical CPUs).
static int dk_gemm_max_threads(void) {
    static int cached = 0;
    int v = __atomic_load_n(&cached, __ATOMIC_RELAXED);
    if (v > 0) return v;
    const char* env = getenv("DONKY_THREADS");
    int n = env ? atoi(env) : 0;
    if (n <= 0) {
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si); n = (int)si.dwNumberOfProcessors;
#else
        long c = sysconf(_SC_NPROCESSORS_ONLN); n = c > 0 ? (int)c : 1;
#endif
        if (n > 8) n = 8;
    }
    if (n < 1) n = 1;
    if (n > DK_PAR_MAX) n = DK_PAR_MAX;
    __atomic_store_n(&cached, n, __ATOMIC_RELAXED);
    return n;
}

// Starting a thread costs tens of microseconds, so small matrices stay on the calling thread:
// each thread must get at least DK_GEMM_MIN_BLOCKS_PER_THREAD (dequantize + dot) block-steps.
#define DK_GEMM_MIN_BLOCKS_PER_THREAD 2048
static int dk_gemm_threads(size_t R, size_t k_blocks) {
    int nt = dk_gemm_max_threads();
    if (nt < 2) return 1;
    size_t by_work = (R * k_blocks) / DK_GEMM_MIN_BLOCKS_PER_THREAD;
    if ((size_t)nt > R) nt = (int)R;
    if ((size_t)nt > by_work) nt = (int)by_work;
    return nt < 1 ? 1 : nt;
}

// Runs fn over rows [0, R) split into nt contiguous ranges. The calling thread takes the last range, so
// only nt-1 threads are created; if a thread cannot be created its range runs on the caller instead, so the
// work is always completed.
static void dk_par_rows(DkRowFn fn, void* ctx, size_t R, int nt) {
    if (nt > DK_PAR_MAX) nt = DK_PAR_MAX;
    if (nt <= 1 || R < 2) { fn(ctx, 0, R); return; }
    if ((size_t)nt > R) nt = (int)R;

    DkParTask tasks[DK_PAR_MAX];
    int started[DK_PAR_MAX];
#ifdef _WIN32
    HANDLE th[DK_PAR_MAX];
#else
    pthread_t th[DK_PAR_MAX];
#endif
    for (int i = 0; i < nt; i++) {
        tasks[i].fn = fn;
        tasks[i].ctx = ctx;
        tasks[i].row_start = (size_t)(((uint64_t)i * R) / (uint64_t)nt);
        tasks[i].row_end   = (size_t)(((uint64_t)(i + 1) * R) / (uint64_t)nt);
        started[i] = 0;
    }
    for (int i = 0; i < nt - 1; i++) {
#ifdef _WIN32
        uintptr_t h = _beginthreadex(NULL, 0, dk_par_trampoline, &tasks[i], 0, NULL);
        if (h) { th[i] = (HANDLE)h; started[i] = 1; }
#else
        if (pthread_create(&th[i], NULL, dk_par_trampoline, &tasks[i]) == 0) started[i] = 1;
#endif
    }
    fn(ctx, tasks[nt - 1].row_start, tasks[nt - 1].row_end);
    for (int i = 0; i < nt - 1; i++) {
        if (!started[i]) { fn(ctx, tasks[i].row_start, tasks[i].row_end); continue; }
#ifdef _WIN32
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
#else
        pthread_join(th[i], NULL);
#endif
    }
}

// Q4_K worker: output rows [row_start, row_end) of the GEMM (this is the old loop body, unchanged).
typedef struct {
    const uint8_t* wp;      // Q4_K weights, [R rows x k_blocks blocks]
    const float*   fa;      // activations, [B x C]
    double*        acc;     // partial sums, [B x R]; this worker only writes columns row_start..row_end-1
    size_t B, C, R, k_blocks;
} DkQ4Ctx;

static void dk_q4_rows(void* p, size_t row_start, size_t row_end) {
    const DkQ4Ctx* a = (const DkQ4Ctx*)p;
    float block[256];                                         // one dequantized block (1 KiB, stays in L1)
    for (size_t row = row_start; row < row_end; row++) {      // weight output row
        const uint8_t* rowp = a->wp + row * a->k_blocks * sizeof(DkQ4KBlock);
        for (size_t b = 0; b < a->k_blocks; b++) {            // 256-weight block, in ascending k order
            const DkQ4KBlock* blk = (const DkQ4KBlock*)(rowp + b * sizeof(DkQ4KBlock));
            dk_q4k_dequant(blk, block);                       // dequantize ONCE
            for (size_t bi = 0; bi < a->B; bi++)
                a->acc[bi * a->R + row] += dk_dot256(block, a->fa + bi * a->C + b * 256);
        }
    }
}
// ==== END dk_simd_mt ====
 
// tensor_matmul_q4(a, w_ptr, batch, c, r): GEMM of a [batch x c] f64 activation matrix against an
// [r x c] Q4_K weight matrix (row-major, c a multiple of 256). Returns [batch x r] row-major:
//     out[i * r + j] = sum_k a[i * c + k] * dequant(W[j * c + k])
// Weight row j starts at w_ptr + j * (c/256) * 144 bytes. Each block is dequantized once and dotted
// against every batch row. Partial sums live in a plain double buffer (zeroed); results are boxed
// into the Donky array once, at the end. The weights MUST really be Q4_K (ggml type 12): Q4_K_M files
// also contain Q6_K (type 14) tensors, and feeding those here produces garbage / NaN.
uint64_t donky_tensor_matmul_q4(uint64_t a, uint64_t w_ptr,
                                uint64_t batch_val, uint64_t c_val, uint64_t r_val) {
    const char* who = "tensor_matmul_q4";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t B, C, R;
    if (!dk_tensor_dim(who, "batch", batch_val, &B) ||
        !dk_tensor_dim(who, "c",     c_val,     &C) ||
        !dk_tensor_dim(who, "r",     r_val,     &R)) return TAG_BASE;
    if (C == 0 || (C % 256) != 0) {
        char msg[96]; snprintf(msg, sizeof msg,
            "%s: c must be a positive multiple of 256", who);
        return dk_ffi_err(msg);
    }
    if (B == 0 || R == 0) {
        uint64_t* out = NULL;
        return dk_tensor_alloc(who, 0, &out);      // empty in, empty out
    }
    if (pa->len != B * C) {
        char msg[160]; snprintf(msg, sizeof msg,
            "%s: a has %zu elements but batch*c = %zu * %zu = %zu",
            who, pa->len, B, C, B * C);
        return dk_ffi_err(msg);
    }
    if (!is_number(w_ptr)) return dk_ffi_err("tensor_matmul_q4: w_ptr must be a number");
    const uint8_t* wp = (const uint8_t*)(uintptr_t)dk_ffi_to_u64(w_ptr);
    if (!wp) return dk_ffi_err("tensor_matmul_q4: null weight pointer");
 
    size_t k_blocks = C / 256;
    size_t n_out    = B * R;                       // dims <= 2^31, so no size_t overflow
    if (n_out > SIZE_MAX / sizeof(double) || B * C > SIZE_MAX / sizeof(float))
        return dk_ffi_err("tensor_matmul_q4: output too large");
 
    float*  fa  = (float*)malloc(B * C * sizeof(float));      // unboxed activations
    double* acc = (double*)calloc(n_out, sizeof(double));     // zeroed partial sums, [batch x r]
    if (!fa || !acc) {
        free(fa); free(acc);
        return dk_ffi_err("tensor_matmul_q4: out of memory");
    }
    for (size_t i = 0; i < B * C; i++) fa[i] = (float)as_number(pa->data[i]);
 
    uint64_t* out = NULL;
    uint64_t res = dk_tensor_alloc(who, n_out, &out);
    if (res == TAG_BASE) { free(fa); free(acc); return TAG_BASE; }
 
    DkQ4Ctx ctx = { wp, fa, acc, B, C, R, k_blocks };
    dk_par_rows(dk_q4_rows, &ctx, R, dk_gemm_threads(R, k_blocks));   // rows split across threads

    for (size_t i = 0; i < n_out; i++) out[i] = make_number(acc[i]);   // box once, at the end
    free(fa); free(acc);
    return res;
}

// ===== Task 1: Q6_K (GGML type 14). Paste right after dk_q4k_dequant / donky_tensor_matmul_q4. =====
// llama.cpp block_q6_K: 210 bytes = 128 (ql, low 4 bits) + 64 (qh, high 2 bits)
//                                   + 16 (int8 scales) + 2 (fp16 d, LAST) for 256 weights.
typedef struct __attribute__((packed)) {
    uint8_t  ql[128];
    uint8_t  qh[64];
    int8_t   scales[16];
    uint16_t d;
} DkQ6KBlock;
typedef char dk_q6k_size_check[(sizeof(DkQ6KBlock) == 210) ? 1 : -1];
 
// Matches ggml dequantize_row_q6_K: per 128-weight half, 4 quarters of 32 weights;
// quarter 0/1 use the low nibbles of ql[l] / ql[l+32], quarter 2/3 the high nibbles;
// qh[l] supplies 2 bits per quarter; scale index = l/16 + 2*quarter (+8 for 2nd half).
static void dk_q6k_dequant(const DkQ6KBlock* blk, float* dst) {
    const float d = dk_fp16_to_fp32(blk->d);
    const uint8_t* ql = blk->ql;
    const uint8_t* qh = blk->qh;
    const int8_t*  sc = blk->scales;
    for (int n = 0; n < 256; n += 128) {
        for (int l = 0; l < 32; ++l) {
            const int is = l / 16;
            const int q1 = (int)((ql[l]      & 0x0F) | (((qh[l] >> 0) & 3) << 4)) - 32;
            const int q2 = (int)((ql[l + 32] & 0x0F) | (((qh[l] >> 2) & 3) << 4)) - 32;
            const int q3 = (int)((ql[l]      >> 4)   | (((qh[l] >> 4) & 3) << 4)) - 32;
            const int q4 = (int)((ql[l + 32] >> 4)   | (((qh[l] >> 6) & 3) << 4)) - 32;
            dst[l]      = d * (float)sc[is + 0] * (float)q1;
            dst[l + 32] = d * (float)sc[is + 2] * (float)q2;
            dst[l + 64] = d * (float)sc[is + 4] * (float)q3;
            dst[l + 96] = d * (float)sc[is + 6] * (float)q4;
        }
        dst += 128; ql += 64; qh += 32; sc += 8;
    }
}

// ==== BEGIN dk_q6_mt ====
// Q6_K worker. Unlike Q4 it can fail (a non-finite block scale means the data is not Q6_K / wrong offset),
// and a worker must not throw or allocate Donky objects, so it records the lowest failing
// (row * k_blocks + block) index with an atomic min and the caller reports it after the join. That is the same
// block the serial loop stopped at, whatever the thread count.
typedef struct {
    const uint8_t* wp;
    const float*   fa;
    double*        acc;
    size_t B, C, R, k_blocks;
    uint64_t       bad_idx;          // UINT64_MAX = no bad block
} DkQ6Ctx;

static void dk_atomic_min_u64(uint64_t* p, uint64_t v) {
    uint64_t cur = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (v < cur) {
        if (__atomic_compare_exchange_n(p, &cur, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    }
}

static void dk_q6_rows(void* p, size_t row_start, size_t row_end) {
    DkQ6Ctx* a = (DkQ6Ctx*)p;
    float block[256];
    for (size_t row = row_start; row < row_end; row++) {
        const uint8_t* rowp = a->wp + row * a->k_blocks * sizeof(DkQ6KBlock);
        for (size_t b = 0; b < a->k_blocks; b++) {
            const DkQ6KBlock* blk = (const DkQ6KBlock*)(rowp + b * sizeof(DkQ6KBlock));
            if (!isfinite(dk_fp16_to_fp32(blk->d))) {
                dk_atomic_min_u64(&a->bad_idx, (uint64_t)row * a->k_blocks + b);
                return;                                   // this row range is poisoned; the caller reports it
            }
            dk_q6k_dequant(blk, block);
            for (size_t bi = 0; bi < a->B; bi++) {
                const float* xb = a->fa + bi * a->C + b * 256;
                double s = 0.0;
                for (int i = 0; i < 256; i++) s += (double)block[i] * (double)xb[i];
                a->acc[bi * a->R + row] += s;
            }
        }
    }
}
// ==== END dk_q6_mt ====
 
// tensor_matmul_q6(a, w_ptr, batch, c, r): same contract as tensor_matmul_q4, for Q6_K weights
//   out[i*r + j] = sum_k a[i*c + k] * dequant(W[j*c + k])      W: [r rows x c cols], c % 256 == 0
// Use for output.weight and the Q6_K attn_v / ffn_down tensors of Q4_K_M files.
uint64_t donky_tensor_matmul_q6(uint64_t a, uint64_t w_ptr,
                                uint64_t batch_val, uint64_t c_val, uint64_t r_val) {
    const char* who = "tensor_matmul_q6";
    Array* pa;
    if (!dk_tensor_view(who, "a", a, &pa)) return TAG_BASE;
    size_t B, C, R;
    if (!dk_tensor_dim(who, "batch", batch_val, &B) ||
        !dk_tensor_dim(who, "c",     c_val,     &C) ||
        !dk_tensor_dim(who, "r",     r_val,     &R)) return TAG_BASE;
    if (C == 0 || (C % 256) != 0) {
        char msg[96]; snprintf(msg, sizeof msg, "%s: c must be a positive multiple of 256", who);
        return dk_ffi_err(msg);
    }
    if (B == 0 || R == 0) { uint64_t* out0 = NULL; return dk_tensor_alloc(who, 0, &out0); }
    if (pa->len != B * C) {
        char msg[160]; snprintf(msg, sizeof msg,
            "%s: a has %zu elements but batch*c = %zu * %zu = %zu", who, pa->len, B, C, B * C);
        return dk_ffi_err(msg);
    }
    if (!is_number(w_ptr)) return dk_ffi_err("tensor_matmul_q6: w_ptr must be a number");
    const uint8_t* wp = (const uint8_t*)(uintptr_t)(uint64_t)as_number(w_ptr);
    if (!wp) return dk_ffi_err("tensor_matmul_q6: null weight pointer");
 
    const size_t k_blocks = C / 256;
    const size_t n_out    = B * R;
    float*  fa  = (float*)malloc(B * C * sizeof(float));
    double* acc = (double*)malloc(n_out * sizeof(double));
    if (!fa || !acc) { free(fa); free(acc); return dk_ffi_err("tensor_matmul_q6: out of memory"); }
    memset(acc, 0, n_out * sizeof(double));
    for (size_t i = 0; i < B * C; i++) fa[i] = (float)as_number(pa->data[i]);
 
    DkQ6Ctx ctx = { wp, fa, acc, B, C, R, k_blocks, UINT64_MAX };
    dk_par_rows(dk_q6_rows, &ctx, R, dk_gemm_threads(R, k_blocks));   // rows split across threads
    if (ctx.bad_idx != UINT64_MAX) {
        char msg[200]; snprintf(msg, sizeof msg,
            "%s: non-finite block scale at row %zu block %zu (weight is not Q6_K, "
            "or wrong data offset)", who, (size_t)(ctx.bad_idx / k_blocks), (size_t)(ctx.bad_idx % k_blocks));
        free(fa); free(acc);
        return dk_ffi_err(msg);
    }
    free(fa);
    uint64_t* out = NULL;
    uint64_t res = dk_tensor_alloc(who, n_out, &out);
    if (res == TAG_BASE) { free(acc); return TAG_BASE; }
    for (size_t i = 0; i < n_out; i++) out[i] = make_number(acc[i]);
    free(acc);
    return res;
}

// ------------------------------------------------------------------ donky_ffi_peek_f32
// ffi_peek_f32(ptr, n): copy n float32 values from ptr into a new Donky array of f64.
// Reads go through memcpy, so ptr need not be 4-byte aligned. A null/invalid ptr or a bad n THROWS
// (it used to return null / an empty array, which hides a wrong tensor offset until much later).
// There is no bounds check against the mapped file: the caller must pass a range inside the mapping.
uint64_t donky_ffi_peek_f32(uint64_t ptr_val, uint64_t n_val) {
    const char* who = "ffi_peek_f32";
    char msg[96];
    if (!is_number(ptr_val)) { snprintf(msg, sizeof msg, "%s: ptr must be a number, got %s", who, dk_type_name(ptr_val)); return dk_ffi_err(msg); }
    if (!is_number(n_val))   { snprintf(msg, sizeof msg, "%s: n must be a number, got %s", who, dk_type_name(n_val));     return dk_ffi_err(msg); }
    double dn = as_number(n_val);
    if (!(dn >= 0.0) || dn > 1e9) return dk_ffi_err("ffi_peek_f32: n out of range");
    size_t n = (size_t)dn;
    uint64_t* out = NULL;
    if (n == 0) return dk_tensor_alloc(who, 0, &out);
    const unsigned char* src = (const unsigned char*)(uintptr_t)dk_ffi_to_u64(ptr_val);
    if (!src) return dk_ffi_err("ffi_peek_f32: null pointer");
    uint64_t r = dk_tensor_alloc(who, n, &out);
    if (r == TAG_BASE) return TAG_BASE;
    for (size_t i = 0; i < n; i++) {
        float f;
        memcpy(&f, src + i * sizeof(float), sizeof f);
        out[i] = make_number((double)f);
    }
    return r;
}
 
// ------------------------------------------------------------------ donky_tensor_stats  (OPTIONAL diagnostic, new)
// tensor_stats(a) -> [n_nan, n_inf, min, max] over the array. min/max cover the FINITE elements only
// (0, 0 if there are none). Meant for hunting where a NaN/inf first enters a forward pass.
// OPTIONAL: needs registering as a 1-arg builtin.
uint64_t donky_tensor_stats(uint64_t a) {
    Array* pa;
    if (!dk_tensor_view("tensor_stats", "a", a, &pa)) return TAG_BASE;
    double mn = 0.0, mx = 0.0, nan_n = 0.0, inf_n = 0.0;
    int seen = 0;
    for (size_t i = 0; i < pa->len; i++) {
        double x = as_number(pa->data[i]);
        if (x != x) { nan_n += 1.0; continue; }
        if (isinf(x)) { inf_n += 1.0; continue; }
        if (!seen) { mn = mx = x; seen = 1; }
        else { if (x < mn) mn = x; if (x > mx) mx = x; }
    }
    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc("tensor_stats", 4, &out);
    if (r == TAG_BASE) return TAG_BASE;
    out[0] = make_number(nan_n); out[1] = make_number(inf_n); out[2] = make_number(mn); out[3] = make_number(mx);
    return r;
}

// ffi_peek_str(ptr, n): copy n bytes from ptr into a new Donky string.
// This is the fast path for reading GGUF tensor names and metadata keys.
uint64_t donky_ffi_peek_str(uint64_t ptr_val, uint64_t n_val) {
    if (!is_number(ptr_val) || !is_number(n_val)) return TAG_BASE;
    double dn = as_number(n_val);
    if (!(dn >= 0.0) || dn > 1e9) return dk_ffi_err("ffi_peek_str: n out of range");
    size_t n = (size_t)dn;
    if (n == 0) return donky_alloc_string("");
    const char* src = (const char*)(uintptr_t)(uint64_t)as_number(ptr_val);
    if (!src) return donky_alloc_string("");
    char* copy = gc_malloc(n + 1);
    memcpy(copy, src, n);
    copy[n] = 0;
    ((GCHeader*)copy - 1)->type_tag = TAG_STRING;
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)copy;
}

// ---- Tokenizer primitives ---------------------------------------------------------------------

#define DK_VOCAB_MAX_TOKENS  (1u << 24)     // 16M tokens: far above any real vocab
#define DK_VOCAB_MAX_TOKLEN  65536u         // a longer "token" means we are reading at the wrong address

static inline uint64_t dk_rd_u64le(const uint8_t* p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

// ffi_peek_vocab(addr, n_tokens) -> array of n_tokens strings, vocab[id] = token text.
// `addr` is an absolute address (number) of EITHER the first string of a GGUF string array
// (u64 length + bytes, repeated) OR the array header in front of it (u32 element type = 8, u64 count);
// the header form is recognised automatically when its count equals n_tokens.
// There is no bounds check against the mapping (the address is all we get): a token length over 64 KiB
// is treated as "wrong address" and throws, which catches nearly every misaligned start immediately.
// A token containing a NUL byte reads back truncated at the NUL (Donky strings are C strings).
uint64_t donky_ffi_peek_vocab(uint64_t addr_val, uint64_t n_val) {
    const char* who = "ffi_peek_vocab";
    char msg[160];
    if (!is_number(addr_val)) { snprintf(msg, sizeof msg, "%s: addr must be a number, got %s", who, dk_type_name(addr_val)); return dk_ffi_err(msg); }
    if (!is_number(n_val))    { snprintf(msg, sizeof msg, "%s: n_tokens must be a number, got %s", who, dk_type_name(n_val)); return dk_ffi_err(msg); }
    double dn = as_number(n_val);
    if (!(dn >= 0.0) || dn > (double)DK_VOCAB_MAX_TOKENS || dn != floor(dn)) {
        snprintf(msg, sizeof msg, "%s: n_tokens must be a whole number in [0, %u]", who, DK_VOCAB_MAX_TOKENS);
        return dk_ffi_err(msg);
    }
    size_t n = (size_t)dn;
    const uint8_t* p = (const uint8_t*)(uintptr_t)dk_ffi_to_u64(addr_val);
    if (!p) { snprintf(msg, sizeof msg, "%s: null address", who); return dk_ffi_err(msg); }

    uint64_t av = donky_alloc_array();
    Array* arr = (Array*)as_index(av);
    if (n == 0) return av;
    if (n > SIZE_MAX / sizeof(uint64_t) || !(arr->data = (uint64_t*)calloc(n, sizeof(uint64_t)))) {
        arr->data = NULL;
        snprintf(msg, sizeof msg, "%s: out of memory", who);
        return dk_ffi_err(msg);
    }
    arr->cap = n;                                   // len stays 0 and grows as slots are filled: the GC never sees a half-built slot

    if ((uint32_t)dk_rd_u64le(p) == 8u && dk_rd_u64le(p + 4) == (uint64_t)n) p += 12;   // skip a GGUF array header

    for (size_t i = 0; i < n; i++) {
        uint64_t len = dk_rd_u64le(p);
        if (len > DK_VOCAB_MAX_TOKLEN) {
            snprintf(msg, sizeof msg,
                "%s: token %zu has implausible length %llu: addr is not the start of the string array (pass the address of the first string or of the array header)",
                who, i, (unsigned long long)len);
            return dk_ffi_err(msg);
        }
        p += 8;
        char* s = (char*)gc_malloc((size_t)len + 1);
        memcpy(s, p, (size_t)len);
        s[len] = 0;
        ((GCHeader*)s - 1)->type_tag = TAG_STRING;
        arr->data[i] = TAG_BASE | (TAG_STRING << 48) | (uint64_t)(uintptr_t)s;
        arr->len = i + 1;
        p += len;
    }
    return av;
}

// ---- tokenize_bpe ----
typedef struct { const char* s; uint32_t len; uint32_t id; } DkTokEnt;      // s == NULL: empty slot

#define DK_FNV_OFF   0xcbf29ce484222325ULL
#define DK_FNV_PRIME 0x100000001b3ULL
#define DK_TOK_MAXLEN 65535u

static inline size_t dk_tok_slot(uint64_t h, int shift) { return (size_t)((h * 0x9E3779B97F4A7C15ULL) >> shift); }

static const DkTokEnt* dk_tok_find(const DkTokEnt* tab, size_t mask, int shift, uint64_t h, const char* s, size_t len) {
    for (size_t i = dk_tok_slot(h, shift);; i = (i + 1) & mask) {
        const DkTokEnt* e = &tab[i];
        if (!e->s) return NULL;
        if (e->len == len && memcmp(e->s, s, len) == 0) return e;
    }
}

// GPT-2 "bytes_to_unicode": printable bytes (0x21-0x7E, 0xA1-0xAC, 0xAE-0xFF) map to themselves; every
// other byte b maps to U+0100 + (its rank among those bytes), e.g. space -> U+0120 'Ġ', newline -> U+010A 'Ċ'.
static uint16_t dk_gpt2_cp[256];
static void dk_gpt2_init(void) {
    if (dk_gpt2_cp[33]) return;                                  // already built (entry 33 is never 0)
    unsigned n = 0;
    for (unsigned b = 0; b < 256; b++) {
        int printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        dk_gpt2_cp[b] = (uint16_t)(printable ? b : 256 + n++);
    }
}

// tokenize_bpe(text, vocab) -> array of token ids (numbers). Greedy longest match, left to right:
// at each position take the longest vocab entry that equals the text there. NOT real BPE (no merge
// ranks, no Llama-3 regex pre-split), so ids can differ from the reference tokenizer; it is an MVP.
//  * Llama-3 / GPT-2 style vocabularies store text byte-level-mapped (space is "Ġ", not 0x20). If the
//    vocab contains "Ġ" the text bytes are mapped the same way before matching, so " world" finds
//    "Ġworld". Any other vocab is matched against the raw bytes.
//  * If a duplicate token string appears, the lowest id wins. Empty tokens are ignored.
//  * Special tokens (e.g. "<|begin_of_text|>") match when they appear in the text, which is how you
//    put BOS in a prompt, and also means user text can inject control tokens.
//  * A byte no token covers throws (silently dropping it would corrupt the prompt). With a byte-level
//    vocab every byte is covered, so this only happens with a toy/raw vocab.
// The lookup table is rebuilt on every call (~10 ms for a 128k vocab): tokenize a whole prompt per call.
uint64_t donky_tokenize_bpe(uint64_t text_val, uint64_t vocab_val) {
    const char* who = "tokenize_bpe";
    char msg[192];
    if (get_tag(text_val) != TAG_STRING) { snprintf(msg, sizeof msg, "%s: text must be a string, got %s", who, dk_type_name(text_val)); return dk_ffi_err(msg); }
    if (get_tag(vocab_val) != TAG_ARRAY) { snprintf(msg, sizeof msg, "%s: vocab must be an array of strings, got %s", who, dk_type_name(vocab_val)); return dk_ffi_err(msg); }
    const char* text = (const char*)as_index(text_val);
    Array* vocab = (Array*)as_index(vocab_val);
    size_t nv = vocab->len, tlen = strlen(text);
    for (size_t i = 0; i < nv; i++) {
        if (get_tag(vocab->data[i]) != TAG_STRING) {
            snprintf(msg, sizeof msg, "%s: vocab[%zu] must be a string, got %s", who, i, dk_type_name(vocab->data[i]));
            return dk_ffi_err(msg);
        }
    }
    if (nv > 0xFFFFFFFEu) return dk_ffi_err("tokenize_bpe: vocab too large");

    // Open-addressing table, load <= 0.5.
    size_t cap = 16; int bits = 4;
    while (cap < nv * 2 + 2) { cap <<= 1; bits++; }
    int shift = 64 - bits;
    DkTokEnt* tab = (DkTokEnt*)calloc(cap, sizeof(DkTokEnt));
    uint32_t* maxlen_first = (uint32_t*)calloc(256, sizeof(uint32_t));
    if (!tab || !maxlen_first) { free(tab); free(maxlen_first); return dk_ffi_err("tokenize_bpe: out of memory"); }
    uint32_t maxlen = 0;
    for (size_t i = 0; i < nv; i++) {
        const char* s = (const char*)as_index(vocab->data[i]);
        size_t len = strlen(s);
        if (len == 0 || len > DK_TOK_MAXLEN) continue;
        uint64_t h = DK_FNV_OFF;
        for (size_t k = 0; k < len; k++) h = (h ^ (uint8_t)s[k]) * DK_FNV_PRIME;
        size_t j = dk_tok_slot(h, shift);
        int dup = 0;
        for (;; j = (j + 1) & (cap - 1)) {
            if (!tab[j].s) break;
            if (tab[j].len == len && memcmp(tab[j].s, s, len) == 0) { dup = 1; break; }
        }
        if (dup) continue;                                        // lowest id already stored
        tab[j].s = s; tab[j].len = (uint32_t)len; tab[j].id = (uint32_t)i;
        if (len > maxlen) maxlen = (uint32_t)len;
        if (len > maxlen_first[(uint8_t)s[0]]) maxlen_first[(uint8_t)s[0]] = (uint32_t)len;
    }

    // Byte-level (GPT-2 / Llama-3) vocab? It contains "Ġ" = C4 A0.
    int bytelevel = 0;
    {
        uint64_t h = (DK_FNV_OFF ^ 0xC4) * DK_FNV_PRIME; h = (h ^ 0xA0) * DK_FNV_PRIME;
        bytelevel = dk_tok_find(tab, cap - 1, shift, h, "\xC4\xA0", 2) != NULL;
    }
    const char* t = text;
    char* mapped = NULL;
    size_t mlen = tlen;
    if (bytelevel) {
        dk_gpt2_init();
        mapped = (char*)malloc(tlen * 2 + 1);
        if (!mapped) { free(tab); free(maxlen_first); return dk_ffi_err("tokenize_bpe: out of memory"); }
        size_t o = 0;
        for (size_t i = 0; i < tlen; i++) {
            unsigned cp = dk_gpt2_cp[(uint8_t)text[i]];
            if (cp < 0x80) mapped[o++] = (char)cp;
            else { mapped[o++] = (char)(0xC0 | (cp >> 6)); mapped[o++] = (char)(0x80 | (cp & 0x3F)); }   // cp < 0x800 always
        }
        mapped[o] = 0; mlen = o; t = mapped;
    }

    uint32_t* ids = (uint32_t*)malloc((tlen ? tlen : 1) * sizeof(uint32_t));       // <= 1 token per input byte
    uint64_t* hs = (uint64_t*)malloc(((size_t)maxlen + 2) * sizeof(uint64_t));
    if (!ids || !hs) { free(tab); free(maxlen_first); free(mapped); free(ids); free(hs); return dk_ffi_err("tokenize_bpe: out of memory"); }

    size_t nids = 0, i = 0;
    int ok = 1;
    while (i < mlen) {
        size_t maxl = maxlen_first[(uint8_t)t[i]];
        if (maxl > mlen - i) maxl = mlen - i;
        uint64_t h = DK_FNV_OFF;
        for (size_t l = 1; l <= maxl; l++) { h = (h ^ (uint8_t)t[i + l - 1]) * DK_FNV_PRIME; hs[l] = h; }
        size_t l = maxl;
        const DkTokEnt* e = NULL;
        for (; l >= 1; l--) { e = dk_tok_find(tab, cap - 1, shift, hs[l], t + i, l); if (e) break; }
        if (!e) {
            snprintf(msg, sizeof msg, "%s: no vocab entry matches byte 0x%02X at offset %zu%s", who, (uint8_t)t[i], i,
                     bytelevel ? " (of the byte-level-mapped text)" : "");
            ok = 0; break;
        }
        ids[nids++] = e->id;
        i += l;
    }
    free(tab); free(maxlen_first); free(mapped); free(hs);
    if (!ok) { free(ids); return dk_ffi_err(msg); }

    uint64_t* out = NULL;
    uint64_t r = dk_tensor_alloc(who, nids, &out);
    if (r == TAG_BASE) { free(ids); return TAG_BASE; }
    for (size_t k = 0; k < nids; k++) out[k] = make_number((double)ids[k]);
    free(ids);
    return r;
}
// ============================================================================
// Voxel world: byte grid in mem_alloc memory, not a Donky array.
// ============================================================================

#define BLK_AIR      0
#define BLK_DIRT     1
#define BLK_STONE    2
#define BLK_GRASS    3
#define BLK_WATER    4
#define BLK_SAND     5
#define BLK_GRAVEL   6
#define BLK_ORE_IRON 7
#define BLK_ORE_GOLD 8
#define DK_NUM_BLOCKS 9

#define DK_CHUNK     128

static const uint32_t DK_PALETTE[9] = {
    0x00000000,  // 0 air
    0xFF6B4A2E,  // 1 dirt
    0xFF5A5A64,  // 2 stone
    0xFF3E8B3E,  // 3 grass
    0x80468ECC,  // 4 water
    0xFFD8C48B,  // 5 sand
    0xFF8B6E4B,  // 6 gravel
    0xFFC08C4A,  // 7 iron ore
    0xFFFFD700,  // 8 gold ore
};

// Deterministic 2D hash in [0, 1).
static inline double dk_world_rand(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return (double)h / 4294967296.0;
}

// Bilinear-interpolated value noise in [0, 1).
static inline double dk_world_noise(double fx, double fy, uint32_t seed) {
    int x0 = (int)floor(fx), y0 = (int)floor(fy);
    double tx = fx - x0, ty = fy - y0;
    double n00 = dk_world_rand(x0,     y0,     seed);
    double n10 = dk_world_rand(x0 + 1, y0,     seed);
    double n01 = dk_world_rand(x0,     y0 + 1, seed);
    double n11 = dk_world_rand(x0 + 1, y0 + 1, seed);
    double nx0 = n00 + (n10 - n00) * tx;
    double nx1 = n01 + (n11 - n01) * tx;
    return nx0 + (nx1 - nx0) * ty;
}

// Fractal (4-octave) noise in [0, 1).
static inline double dk_world_fbm(double fx, double fy, uint32_t seed) {
    double total = 0.0, amp = 1.0, freq = 1.0, norm = 0.0;
    for (int i = 0; i < 4; i++) {
        total += dk_world_noise(fx * freq, fy * freq, seed + i * 9176u) * amp;
        norm += amp;
        amp *= 0.5;
        freq *= 2.0;
    }
    return total / norm;
}

uint64_t donky_world_gen(uint64_t ptr_val, uint64_t width_val, uint64_t height_val,
                         uint64_t seed_val, uint64_t chunk_x_val, uint64_t chunk_y_val) {
    const char* who = "world_gen";
    if (!is_number(ptr_val) || !is_number(width_val) || !is_number(height_val)
        || !is_number(seed_val) || !is_number(chunk_x_val) || !is_number(chunk_y_val)) {
        return dk_ffi_err("world_gen: all arguments must be numbers");
    }
    int width = (int)as_number(width_val);
    int height = (int)as_number(height_val);
    uint32_t seed = (uint32_t)as_number(seed_val);
    int cx = (int)as_number(chunk_x_val);
    int cy = (int)as_number(chunk_y_val);
    if (width <= 0 || height <= 0) return dk_ffi_err("world_gen: width/height must be positive");
    if (cx < 0 || cx >= width || cy < 0 || cy >= height) return make_number(0.0);

    uint8_t* grid = (uint8_t*)(uintptr_t)(uint64_t)as_number(ptr_val);
    if (!grid) return dk_ffi_err("world_gen: null pointer");

    int x_end = cx + DK_CHUNK; if (x_end > width)  x_end = width;
    int y_end = cy + DK_CHUNK; if (y_end > height) y_end = height;

    double sea_level = height * 0.5;
    double surface_base = height * 0.35;
    double surface_amp = height * 0.25;

    for (int y = cy; y < y_end; y++) {
        for (int x = cx; x < x_end; x++) {
            double n = dk_world_fbm(x * 0.02, 0.0, seed);
            int surface_y = (int)(surface_base + (n - 0.5) * surface_amp * 2.0);
            if (surface_y < 1) surface_y = 1;
            if (surface_y > height - 2) surface_y = height - 2;

            uint8_t block = BLK_AIR;
            if (y < surface_y - 3)        block = BLK_AIR;
            else if (y < surface_y)       block = BLK_GRASS;
            else if (y < surface_y + 4)   block = BLK_DIRT;
            else                          block = BLK_STONE;

            if (block == BLK_STONE) {
                double c = dk_world_fbm(x * 0.06, y * 0.06, seed + 100u);
                if (c > 0.62) block = BLK_AIR;
            }
            if (block == BLK_STONE && y > surface_y + 20) {
                double o = dk_world_fbm(x * 0.1, y * 0.1, seed + 200u);
                if (o > 0.75) {
                    double g = dk_world_fbm(x * 0.1, y * 0.1, seed + 300u);
                    block = (g > 0.5) ? BLK_ORE_GOLD : BLK_ORE_IRON;
                }
            }
            if ((block == BLK_GRASS || block == BLK_DIRT)
                && y >= (int)sea_level - 2 && y <= (int)sea_level + 2) {
                block = BLK_SAND;
            }
            if (block == BLK_AIR && y > (int)sea_level) {
                block = BLK_WATER;
            }

            grid[y * width + x] = block;
        }
    }
    return make_number(1.0);
}

// ----------------------------------------------------------------- world_mesh
// world_mesh(ptr, width, height, cam_x, cam_y, view_w, view_h, out_ptr) -> pixels written
// Fills out_ptr[0..view_w*view_h*4) with RGBA. Blocks outside the grid render
// as stone; rows fully above the grid render as black. Colors come from
// DK_PALETTE via a per-block uint32 lookup (autovectorized by the compiler; a
// memcpy-of-uniform-rows fast path would need a scan to prove uniformity and
// is not implemented — see the handoff note).
uint64_t donky_world_mesh(uint64_t ptr_val, uint64_t width_val, uint64_t height_val,
                          uint64_t cam_x_val, uint64_t cam_y_val,
                          uint64_t view_w_val, uint64_t view_h_val, uint64_t out_ptr_val) {
    const char* who = "world_mesh";
    char msg[128];
    if (!is_number(ptr_val) || !is_number(out_ptr_val))
        return dk_ffi_err("world_mesh: ptr and out_ptr must be numbers");
    if (!is_number(width_val) || !is_number(height_val) ||
        !is_number(cam_x_val) || !is_number(cam_y_val) ||
        !is_number(view_w_val) || !is_number(view_h_val))
        return dk_ffi_err("world_mesh: width, height, cam_x, cam_y, view_w, view_h must be numbers");
    double dw = as_number(width_val), dh = as_number(height_val);
    double dvw = as_number(view_w_val), dvh = as_number(view_h_val);
    if (!(dw >= 1.0) || dw > 1.0e7 || !(dh >= 1.0) || dh > 1.0e7 ||
        !(dvw >= 1.0) || dvw > 1.0e7 || !(dvh >= 1.0) || dvh > 1.0e7)
        return dk_ffi_err("world_mesh: width, height, view_w, view_h must be whole numbers in [1, 1e7]");
    if (dvw != floor(dvw) || dvh != floor(dvh))
        return dk_ffi_err("world_mesh: view_w/view_h must be whole numbers");
    size_t W = (size_t)dw, H = (size_t)dh, VW = (size_t)dvw, VH = (size_t)dvh;

    long long camx = (long long)as_number(cam_x_val);
    long long camy = (long long)as_number(cam_y_val);

    const uint8_t* grid = (const uint8_t*)(uintptr_t)(uint64_t)as_number(ptr_val);
    uint8_t* out = (uint8_t*)(uintptr_t)(uint64_t)as_number(out_ptr_val);
    if (!grid || !out) return dk_ffi_err("world_mesh: null grid or out buffer");

    // Lookup table: 256 uint32 entries, indexed by block id (only 0..8 are used;
    // every other block id in the grid is treated as stone by world_set, so it
    // cannot appear here, but init the tail defensively to opaque black).
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = (i < DK_NUM_BLOCKS) ? DK_PALETTE[i] : 0xFF000000u;

    size_t pixels = 0;
    for (size_t py = 0; py < VH; py++) {
        long long wy = camy + (long long)py;
        uint8_t* out_row = out + py * VW * 4;
        if (wy < 0 || wy >= (long long)H) {
            // Row outside the grid: solid black.
            for (size_t px = 0; px < VW; px++) {
                out_row[px * 4 + 0] = 0;
                out_row[px * 4 + 1] = 0;
                out_row[px * 4 + 2] = 0;
                out_row[px * 4 + 3] = 0xFF;
            }
            pixels += VW;
            continue;
        }
        const uint8_t* row = grid + (size_t)wy * W;
        for (size_t px = 0; px < VW; px++) {
            long long wx = camx + (long long)px;
            uint8_t blk;
            if (wx < 0 || wx >= (long long)W) blk = BLK_STONE;
            else                             blk = row[(size_t)wx];
            uint32_t c = lut[blk];
            out_row[px * 4 + 0] = (uint8_t)(c        & 0xFF);
            out_row[px * 4 + 1] = (uint8_t)((c >> 8) & 0xFF);
            out_row[px * 4 + 2] = (uint8_t)((c >> 16) & 0xFF);
            out_row[px * 4 + 3] = (uint8_t)((c >> 24) & 0xFF);
        }
        pixels += VW;
    }
    return make_number((double)pixels);
}

// ------------------------------------------------------------------ world_set
// world_set(ptr, width, height, x, y, block_id) -> 1 on success, 0 on out-of-bounds
// or an invalid block id. Called per edit, so it does not throw on the
// ordinary "user clicked outside the map" / "block id out of range" cases:
// 0 is the documented, expected return value there. It throws only on a null
// address or a non-number argument, which are programming errors, not data.
uint64_t donky_world_set(uint64_t ptr_val, uint64_t width_val, uint64_t height_val,
                         uint64_t x_val, uint64_t y_val, uint64_t block_val) {
    if (!is_number(ptr_val) || !is_number(width_val) || !is_number(height_val) ||
        !is_number(x_val) || !is_number(y_val) || !is_number(block_val))
        return dk_ffi_err("world_set: ptr, width, height, x, y, block must be numbers");
    uint8_t* grid = (uint8_t*)(uintptr_t)(uint64_t)as_number(ptr_val);
    if (!grid) return dk_ffi_err("world_set: null grid");
    double dw = as_number(width_val), dh = as_number(height_val);
    if (!(dw >= 1.0) || dw > 1.0e7 || dw != floor(dw) ||
        !(dh >= 1.0) || dh > 1.0e7 || dh != floor(dh))
        return dk_ffi_err("world_set: width/height must be whole numbers in [1, 1e7]");
    size_t W = (size_t)dw, H = (size_t)dh;
    double dx = as_number(x_val), dy = as_number(y_val);
    double db = as_number(block_val);
    if (!(dx == floor(dx)) || !(dy == floor(dy)) || !(db == floor(db)))
        return make_number(0.0);           // non-integer coordinates or id: "outside" by contract
    long long xx = (long long)dx, yy = (long long)dy;
    long long bid = (long long)db;
    if (xx < 0 || xx >= (long long)W || yy < 0 || yy >= (long long)H) return make_number(0.0);
    if (bid < 0 || bid >= DK_NUM_BLOCKS)                             return make_number(0.0);
    grid[(size_t)yy * W + (size_t)xx] = (uint8_t)bid;
    return make_number(1.0);
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