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
#include <process.h> // for _beginthreadex
#include <inttypes.h>
#include <setjmp.h>
#include <winhttp.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winhttp.lib")

static int rand_seeded = 0;

#define MAX_THREADS 64
static volatile int active_threads = 0;
static HANDLE active_threads_handles[MAX_THREADS];

#define TAG_MASK 0xFFF8000000000000ULL
#define TAG_BASE 0x7FF8000000000000ULL
#define TAG_BOOL 1ULL
#define TAG_STRING 2ULL
#define TAG_ARRAY 3ULL
#define TAG_MAP 4ULL

CRITICAL_SECTION gc_lock;

void gc_init() {
    InitializeCriticalSection(&gc_lock);
}

// --- Garbage Collector Structures ---
typedef struct GCHeader {
    struct GCHeader* next;
    int marked;
    size_t size;
} GCHeader;

static GCHeader* gc_head = NULL;
static size_t gc_bytes_allocated = 0;
static size_t gc_threshold = 1024 * 50; // Trigger GC after 1MB of allocations

void gc_collect(); // Forward declaration

// The custom allocator
void* gc_malloc(size_t size) {
    EnterCriticalSection(&gc_lock);
    GCHeader* header = (GCHeader*)malloc(sizeof(GCHeader) + size);
    if (!header) { printf("OOM!\n"); exit(1); }
    
    header->next = gc_head;
    header->marked = 0;
    header->size = size;
    gc_head = header;

    gc_bytes_allocated += size;

    LeaveCriticalSection(&gc_lock);
    return (void*)(header + 1); // Return pointer AFTER the header
}


typedef struct { uint64_t* data; size_t len; size_t cap; } Array;
typedef struct { uint64_t* keys; uint64_t* vals; size_t len; size_t cap; } Map;

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
    if (is_number(val)) {
        print_value(val); 
        printf("\n"); 
    } else if (get_tag(val) == TAG_STRING) {
        printf("%s\n", (char*)as_index(val));
    } else {
        print_value(val); 
        printf("\n"); 
    }
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
    size_t len = strlen(s);
    char* copy = gc_malloc(len + 1);
    memcpy(copy, s, len + 1);
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)copy;
}


// --- TCP (static socket table; box an int id into Donky) ---
#define DK_MAX_SOCK 64
static SOCKET g_socks[DK_MAX_SOCK];
static int g_socks_init = 0;

static int dk_id_from_socket(SOCKET s) {
    for (int i = 0; i < DK_MAX_SOCK; i++) {
        if (g_socks[i] == 0) { g_socks[i] = s; return i + 1; }
    }
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

    // SO_REUSEADDR so restarts don't fail with "address in use"
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
    if (max <= 0 || max > 65536) max = 4096;
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
    if (id > 0 && id <= DK_MAX_SOCK && g_socks[id - 1] != 0) {
        closesocket(g_socks[id - 1]);
        g_socks[id - 1] = 0;
    }
    return 0;
}

uint64_t donky_http_get(uint64_t url_val) {
    if (get_tag(url_val) != TAG_STRING) return 0;
    char* url = (char*)as_index(url_val);

    // 1. Convert the URL to a wide string (WinHTTP requires LPCWSTR)
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    wchar_t* wurl = malloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wlen);

    // 2. Parse the URL into components (host, path, port, scheme)
    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    
    // Allocate buffers for the components
    wchar_t hostName[256];
    wchar_t urlPath[2048];
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl, 0, 0, &urlComp)) {
        free(wurl);
        return donky_alloc_string(""); // Failed to parse URL
    }

    // 3. Initialize WinHTTP session
    HINTERNET hSession = WinHttpOpen(L"Donky/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { free(wurl); return donky_alloc_string(""); }

    // 4. Connect to the host
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); free(wurl); return donky_alloc_string(""); }

    // 5. Open the request (Handling HTTPS vs HTTP)
    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL, 
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { 
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); free(wurl); 
        return donky_alloc_string(""); 
    }

    // 6. Send the request
    BOOL bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    // 7. Read the response body
    char* response_body = NULL;
    size_t total_size = 0;
    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;

    if (bResults) {
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            char* buffer = malloc(dwSize + 1);
            ZeroMemory(buffer, dwSize + 1);

            if (WinHttpReadData(hRequest, (LPVOID)buffer, dwSize, &dwDownloaded)) {
                // Append to our growing response body
                response_body = realloc(response_body, total_size + dwDownloaded + 1);
                memcpy(response_body + total_size, buffer, dwDownloaded);
                total_size += dwDownloaded;
                response_body[total_size] = 0; // Null terminate
            }
            free(buffer);
        } while (dwSize > 0);
    }

    // 8. Clean up handles
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    free(wurl);

    // 9. Return the Donky string
    uint64_t result = donky_alloc_string(response_body ? response_body : "");
    free(response_body);
    return result;
}

uint64_t donky_alloc_array() {
    Array* arr = gc_malloc(sizeof(Array));
    arr->data = NULL; arr->len = 0; arr->cap = 0;
    return TAG_BASE | (TAG_ARRAY << 48) | (uint64_t)arr;
}

uint64_t donky_array_push(uint64_t arr_val, uint64_t val) {
    Array* arr = (Array*)as_index(arr_val);
    if (arr->len == arr->cap) {
        arr->cap = arr->cap == 0 ? 4 : arr->cap * 2;
        arr->data = realloc(arr->data, arr->cap * sizeof(uint64_t));
    }
    arr->data[arr->len++] = val;
    return 0;
}

uint64_t donky_alloc_map() {
    Map* map = gc_malloc(sizeof(Map));
    map->keys = NULL; map->vals = NULL; map->len = 0; map->cap = 0;
    return TAG_BASE | (TAG_MAP << 48) | (uint64_t)map;
}

// --- Mark and Sweep ---
void gc_mark(uint64_t val) {
    if (is_number(val)) return; 
    
    uint64_t tag = get_tag(val);
    void* ptr = (void*)as_index(val);
    
    if (tag != TAG_STRING && tag != TAG_ARRAY && tag != TAG_MAP) return;
    
    GCHeader* header = ((GCHeader*)ptr) - 1;
    
    if (header->marked) return; 
    header->marked = 1;
    
    if (tag == TAG_ARRAY) {
        Array* arr = (Array*)ptr;
        for (size_t i = 0; i < arr->len; i++) {
            gc_mark(arr->data[i]);
        }
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
            free(dead);
        }
    }
}

static int map_key_eq(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) return as_number(a) == as_number(b);
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING)
        return strcmp((char*)as_index(a), (char*)as_index(b)) == 0;
    return a == b;
}

uint64_t donky_map_set(uint64_t map_val, uint64_t key, uint64_t val) {
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
        return TAG_BASE | (TAG_STRING << 48) | (uint64_t)buf;
    }
    return 0;
}

uint64_t donky_mod(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) {
        double db = as_number(b);
        if (db == 0.0) return make_number(0.0); // Avoid divide by zero crash
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
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)buf;
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
        // Build the error message
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf), "File not found: %s", path);
        
        // Throw the exception using the existing runtime function!
        extern void donky_throw(uint64_t val);
        donky_throw(donky_alloc_string(err_buf));
        
        return 0; // Return null, but the exception is now flagged
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
    return 0;
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

    // 1. String in String
    if (h_tag == TAG_STRING && n_tag == TAG_STRING) {
        char* h_str = (char*)as_index(haystack);
        char* n_str = (char*)as_index(needle);
        return make_number(strstr(h_str, n_str) != NULL ? 1.0 : 0.0);
    }

    // 2. Item in Array
    if (h_tag == TAG_ARRAY) {
        Array* arr = (Array*)as_index(haystack);
        for (size_t i = 0; i < arr->len; i++) {
            uint64_t eq_res = donky_eq(arr->data[i], needle);
            if (as_number(eq_res) == 1.0) return make_number(1.0);
        }
        return make_number(0.0);
    }

    // 3. Key in Map
    if (h_tag == TAG_MAP) {
        Map* map = (Map*)as_index(haystack);
        for (size_t i = 0; i < map->len; i++) {
            uint64_t eq_res = donky_eq(map->keys[i], needle);
            if (as_number(eq_res) == 1.0) return make_number(1.0);
        }
        return make_number(0.0);
    }

    return make_number(0.0);
}

uint64_t donky_now() { return make_number((double)time(NULL)); }

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
    for (size_t i = 0; i < map->len; i++) {
        uint64_t k = map->keys[i];
        if (get_tag(k) == TAG_STRING) {
            uint64_t copy = donky_alloc_string((char*)as_index(k));
            donky_array_push(arr, copy);
        } else {
            donky_array_push(arr, k);
        }
    }
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
        if (is_number(arr->data[i])) {
            buf[i] = (char)((int)as_number(arr->data[i]));
        } else {
            buf[i] = '?';
        }
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

// --- Native System Functions ---
uint64_t donky_cls() { system("cls"); return 0; }
uint64_t donky_getch() { return make_number((double)_getch()); }
uint64_t donky_kbhit() { return make_number(_kbhit() ? 1.0 : 0.0); }

uint64_t donky_input(uint64_t prompt_val) {
    if (get_tag(prompt_val) == TAG_STRING) {
        printf("%s", (char*)as_index(prompt_val));
        fflush(stdout); // <-- ADD THIS LINE
    }
    char buf[256];
    if (fgets(buf, 256, stdin) == NULL) {
        return donky_alloc_string(""); // Handle EOF
    }
    buf[strcspn(buf, "\r\n")] = 0;
    return donky_alloc_string(buf);
}

uint64_t donky_sleep(uint64_t ms_val) {
    Sleep((DWORD)as_number(ms_val));
    return 0;
}

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
            
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
            if (hProcess) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                    donky_map_set(m, donky_alloc_string("mem"), make_number((double)(pmc.WorkingSetSize / 1024 / 1024)));
                }
                CloseHandle(hProcess);
            }
            donky_array_push(arr, m);
        } while (Process32Next(hProcessSnap, &pe32));
    }
    CloseHandle(hProcessSnap);
    return arr;
}

// --- Argument Handling ---
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
    
    EnterCriticalSection(&gc_lock);
    CloseHandle(active_threads_handles[slot]);
    active_threads_handles[slot] = NULL;
    active_threads--;
    LeaveCriticalSection(&gc_lock);
    
    free(targs);
    return 0;
}

uint64_t donky_spawn(uint64_t func_val, uint64_t arg1) {
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

// --- Stack Scanning and GC Entry ---
void gc_mark_stack() {
    jmp_buf env;
    setjmp(env); 

    // Get the current stack bounds using Windows API
    ULONG_PTR lowLimit = 0, highLimit = 0;
    GetCurrentThreadStackLimits(&lowLimit, &highLimit);

    uintptr_t* stack_ptr = (uintptr_t*)&lowLimit; // Get a rough pointer to the current stack
    uintptr_t* top = (uintptr_t*)highLimit;

    // Scan the raw stack memory 8 bytes at a time
    while (stack_ptr < top) {
        uint64_t val = *stack_ptr;
        
        // Does this look like a tagged Donky pointer?
        if ((val & TAG_MASK) == TAG_BASE) {
            uint64_t tag = get_tag(val);
            if (tag == TAG_STRING || tag == TAG_ARRAY || tag == TAG_MAP) {
                // To be safe, we only mark it if it's actually inside our heap
                // (We can add a check here later, but for now we just trust the tag)
                gc_mark(val);
            }
        }
        stack_ptr++;
    }
}

void gc_collect() {
    EnterCriticalSection(&gc_lock);

    gc_bytes_allocated = 0;
    
    // 1. Mark globals
    extern uint64_t donky_args_array;
    if (donky_args_array != 0) gc_mark(donky_args_array);
    
    extern uint64_t current_exception;
    if (current_exception != 0) gc_mark(current_exception);
    
    // 2. Scan the main stack
    gc_mark_stack();
    
    // 3. Sweep
    gc_sweep();
    
    LeaveCriticalSection(&gc_lock);
}

uint64_t donky_gc() {
    gc_collect();
    return make_number(0.0);
}

int main(int argc, char** argv) {
    gc_init();
    donky_set_args(argc, argv);
    extern int64_t donky_toplevel();
    donky_toplevel();
    
    // Wait for spawned threads to finish
    while (active_threads > 0) {
        Sleep(10);
    }
    
    if (current_exception != 0) { printf("Uncaught exception: "); print_value(current_exception); printf("\n"); return 1; }
    return 0;
}