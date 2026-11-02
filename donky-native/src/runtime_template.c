#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <conio.h>

#define WIN32_LEAN_AND_MEAN
#define _WINSOCKAPI_

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
    struct GCHeader* next;
    int marked;
    size_t size;
    uint64_t type_tag;
} GCHeader;

static GCHeader* gc_head = NULL;
static size_t gc_bytes_allocated = 0;

void gc_init_lock() {
    if (!gc_lock_init) {
        InitializeCriticalSection(&gc_lock);
        gc_lock_init = 1;
    }
}

void* gc_malloc(size_t size) {
    gc_init_lock();
    EnterCriticalSection(&gc_lock);
    
    GCHeader* header = (GCHeader*)malloc(sizeof(GCHeader) + size);
    if (!header) { 
        LeaveCriticalSection(&gc_lock);
        printf("Out of Memory!\n"); 
        exit(1); 
    }
    
    header->next = gc_head;
    header->marked = 0;
    header->size = size;
    header->type_tag = 0;
    gc_head = header;
    gc_bytes_allocated += size;

    LeaveCriticalSection(&gc_lock);
    return (void*)(header + 1);
}

typedef struct { uint64_t* data; size_t len; size_t cap; } Array;
typedef struct { uint64_t* keys; uint64_t* vals; size_t len; size_t cap; } Map;

uint64_t donky_alloc_map(void);
uint64_t donky_map_set(uint64_t map_val, uint64_t key, uint64_t val);

uint64_t current_exception = 0;

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
    map->keys = NULL; map->vals = NULL; map->len = 0; map->cap = 0;
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

void gc_sweep() {
    GCHeader* curr = gc_head;
    GCHeader* prev = NULL;
    
    while (curr != NULL) {
        if (curr->marked) {
            curr->marked = 0;
            prev = curr;
            curr = curr->next;
        } else {
            GCHeader* dead = curr;
            if (prev == NULL) {
                gc_head = curr->next;
                curr = gc_head;
            } else {
                prev->next = curr->next;
                curr = curr->next;
            }
            
            void* payload = (void*)(dead + 1);
            if (dead->type_tag == TAG_ARRAY) {
                Array* arr = (Array*)payload;
                if (arr->data) free(arr->data);
            } else if (dead->type_tag == TAG_MAP) {
                Map* map = (Map*)payload;
                if (map->keys) free(map->keys);
                if (map->vals) free(map->vals);
            }
            free(dead);
        }
    }
}

void gc_mark_stack() {
    jmp_buf env;
    setjmp(env); 
    ULONG_PTR lowLimit = 0, highLimit = 0;
    GetCurrentThreadStackLimits(&lowLimit, &highLimit);
    uintptr_t* stack_ptr = (uintptr_t*)&lowLimit;
    uintptr_t* top = (uintptr_t*)highLimit;

    while (stack_ptr < top) {
        uint64_t val = *stack_ptr;
        if ((val & TAG_MASK) == TAG_BASE) {
            uint64_t tag = get_tag(val);
            if (tag == TAG_STRING || tag == TAG_ARRAY || tag == TAG_MAP) gc_mark(val);
        }
        stack_ptr++;
    }
}

void gc_collect() {
    gc_init_lock();
    EnterCriticalSection(&gc_lock);

    gc_bytes_allocated = 0;
    extern uint64_t donky_args_array;
    if (donky_args_array != 0) gc_mark(donky_args_array);
    if (current_exception != 0) gc_mark(current_exception);
    
    gc_mark_stack();
    gc_sweep();
    
    LeaveCriticalSection(&gc_lock);
}

uint64_t donky_gc() {
    gc_collect();
    return make_number(0.0);
}

// --- Map & Index Operations ---
static int map_key_eq(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return as_number(a) == as_number(b);
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING)
        return strcmp((char*)as_index(a), (char*)as_index(b)) == 0;
    return a == b;
}

uint64_t donky_map_set(uint64_t map_val, uint64_t key, uint64_t val) {
    if (get_tag(map_val) != TAG_MAP) return 0;
    Map* map = (Map*)as_index(map_val);
    for (size_t i = 0; i < map->len; i++) {
        if (map_key_eq(map->keys[i], key)) { map->vals[i] = val; return 0; }
    }
    if (map->len == map->cap) {
        map->cap = map->cap == 0 ? 4 : map->cap * 2;
        map->keys = realloc(map->keys, map->cap * sizeof(uint64_t));
        map->vals = realloc(map->vals, map->cap * sizeof(uint64_t));
    }
    map->keys[map->len] = key;
    map->vals[map->len] = val;
    map->len++;
    return 0;
}

uint64_t donky_index_get(uint64_t target, uint64_t index) {
    if (get_tag(target) == TAG_ARRAY) {
        Array* arr = (Array*)as_index(target);
        size_t i = (size_t)as_number(index);
        if (i < arr->len) return arr->data[i];
    } else if (get_tag(target) == TAG_MAP) {
        Map* map = (Map*)as_index(target);
        for (size_t i = 0; i < map->len; i++) {
            if (map_key_eq(map->keys[i], index)) return map->vals[i];
        }
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
        size_t len1 = strlen(s1); size_t len2 = strlen(s2);
        char* buf = malloc(len1 + len2 + 1);
        strcpy(buf, s1); strcat(buf, s2);
        uint64_t res = donky_alloc_string(buf);
        free(buf);
        return res;
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
        if (i < strlen(s)) return make_number((double)(unsigned char)s[i]);
    }
    return make_number(-1.0);
}

uint64_t donky_substr(uint64_t s_val, uint64_t start_val, uint64_t end_val) {
    if (get_tag(s_val) != TAG_STRING) return 0;
    char* s = (char*)as_index(s_val);
    size_t start = (size_t)as_number(start_val); size_t end = (size_t)as_number(end_val);
    size_t len = strlen(s);
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
    if (get_tag(val) == TAG_STRING) return make_number((double)strlen((char*)as_index(val)));
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
    ResumeThread(active_threads_handles[slot]);
    active_threads++;
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