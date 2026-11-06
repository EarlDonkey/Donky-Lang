# Donky

**A small, dynamically typed systems language that compiles to native executables, with C-level access to the OS and a built-in model server.**

**Version:** 3.1.0 · **Author:** EarlDonkey · **License:** MIT · **Repository:** https://github.com/EarlDonkey/Donky-Lang

---

## Serve a model in 7 lines

```donky
import "lib/router.donk";
import "lib/ai_server.donk";

fn forward(x, w) { return tensor_relu(tensor_matmul(x, w, 1, 3, 2)); }   // (1x3) . (3x2)

let router = router_new();
let weights = [0.5, -1.0, 0.25, 2.0, 1.5, -0.5];                          // 3x2, row-major
http_serve_model(router, "/predict", weights, forward);
router_listen(router, 8080);
```

<!-- TODO(release): confirm the router constructor / listen names (router_new, router_listen) against lib/router.donk. -->

```text
$ donky build-exe server.donk
$ ./donky_app                      # donky_app.exe on Windows
$ curl -X POST localhost:8080/predict -d "[1, 2, 3]"
[5.5,1.5]
```

`http_serve_model(router, path, weights, forward)` registers `POST <path>`. For each request it parses
the JSON body as a tensor, calls `forward(input, weights)`, and returns the result as JSON.

- **Tensors are flat arrays of numbers**, row-major. Matrix shapes are passed to the function that needs
  them: `tensor_matmul(a, b, m, k, n)` multiplies an `m x k` by a `k x n`. Others are `tensor_add`,
  `tensor_mul`, `tensor_relu`, `tensor_softmax` and `tensor_sgd_step`.
- **Weights arrive as the second argument** of `forward`. Donky has no closures
  ([see limitations](#7-honest-limitations)), so a model function cannot capture them.
- **Input is validated.** Bad JSON, non-arrays, empty arrays, non-numbers and `NaN`/`inf` get `400`. A
  `forward` that throws, or returns anything but finite numbers, gets `500` and the reason goes to the
  server log, not to the client. Bodies over 8 MiB get `413`.
- **`tensor_batch(list_of_1d_tensors)`** stacks equal-length tensors into one flat 2D tensor for batched
  inference; `tensor_unbatch(flat, rows)` splits it back.
- **Long-running servers grow in memory** (no automatic GC, and `gc()` does not run while worker threads
  are active). See [limitations](#7-honest-limitations).
- **`tcp_listen` binds every network interface.** A model server is reachable from the network unless
  you firewall it.

---

## Contents

1. [Installation](#1-installation)
2. [Running Donky](#2-running-donky)
3. [Syntax guide](#3-syntax-guide)
4. [C-parity: native interop](#4-c-parity-native-interop)
5. [Standard library at a glance](#5-standard-library-at-a-glance)
6. [Concurrency and networking](#6-concurrency-and-networking)
7. [Honest limitations](#7-honest-limitations)
8. [Built with Donky](#8-built-with-donky)

---

## 1. Installation

### Windows: `DonkySetup.exe`

Run `DonkySetup.exe` (64-bit Windows only).

- Installs to `C:\Program Files\Donky` and needs administrator rights. For a per-user install, run
  `DonkySetup.exe /CURRENTUSER`; it installs to `%LOCALAPPDATA%\Programs\Donky` instead.
- Installs `bin\donky.exe`, the pre-compiled C runtime (`lib\runtime.o`), the standard library
  (`lib\*.donk`) and the examples.
- Adds `bin` to your `PATH` (the option is checked by default). Open a **new** terminal afterwards. The
  Start Menu also gets a "Donky Command Prompt" with `donky` already on the path.
- Standard Inno Setup switches work for scripted installs, e.g. `DonkySetup.exe /VERYSILENT`.

**You also need GCC (MinGW-w64) on your `PATH`.** The installer does not bundle a compiler, and
`donky build-exe` links your program with GCC. If the link step fails with `cannot find -lffi`, install
libffi for your MinGW (MSYS2: `pacman -S mingw-w64-x86_64-libffi`).

### Linux and macOS: `install.sh`

```sh
sh install.sh
```

<!-- TODO(release): document install.sh's install prefix and options once confirmed. -->

See the comments at the top of the script for the install prefix and options.

On Linux and macOS, every `build-exe` compiles the C runtime that is embedded in the `donky` binary, so
you need a C toolchain and the libraries the runtime links against:

| Platform | Needs | Debian/Ubuntu | Fedora |
|---|---|---|---|
| Linux | `gcc`, libcurl and libffi headers | `sudo apt install gcc libcurl4-openssl-dev libffi-dev` | `sudo dnf install gcc libcurl-devel libffi-devel` |
| macOS | Xcode Command Line Tools | `xcode-select --install` | n/a |

Verify the install with `donky --version`.

---

## 2. Running Donky

The shipped engine is the **ahead-of-time (AOT) compiler**: `donky build-exe` compiles your program to
an object file and links it with the C runtime into a standalone native executable. Everything in this
README, including FFI, raw memory, process control, tensors and `http_serve_model`, is defined by the AOT
engine. Build from your project's root directory, because `import` paths resolve against the working
directory.

```text
$ donky build-exe hello.donk
$ ./donky_app            # donky_app.exe on Windows
```

`build-exe` always writes `donky_app` (or `donky_app.exe`) into the current directory.

| Command | What it does |
|---|---|
| `donky build-exe <file.donk>` | Compile to a standalone native executable (needs GCC). **This is the shipped engine.** |
| `donky test [dir\|file]` | Build each `.donk` file (default: `tests/`) with the AOT toolchain, run it, and report PASS/FAIL. A non-zero exit code is a failure. Needs GCC. |
| `donky <file.donk>` | Run with the **frozen** tree-walking interpreter. |
| `donky run <file.donk\|file.bc>` | Run on the **frozen** bytecode VM. |
| `donky build <file.donk>` | Compile to bytecode (`<file>.donk.bc`) for the frozen VM. |
| `donky repl` | Interactive REPL (frozen interpreter). |
| `donky bench [dir]` | Time compile and run on the frozen VM (default: `bench/`). |
| `donky profile <file.donk>` | Profile the frozen VM; writes `profile.folded`. |
| `donky install` | Install the packages listed in the project manifest. |
| `donky add <name> <url\|path>` | Add a dependency. |
| `donky --help`, `donky --version` | Usage and version. |

> **The tree-walker, the VM and the REPL are frozen.** They are not updated for new builtins and their
> behaviour differs from the AOT engine (for example, they raise errors where the AOT engine returns
> `null`). The normative behaviour is whatever `build-exe` produces; it is specified in `SEMANTICS.md`.
> Use the REPL for quick experiments only, and verify with `build-exe`.

There is no `build-native` command.

---

## 3. Syntax guide

This section describes the AOT engine. `SEMANTICS.md` is the full normative reference.

### Values

```donky
let n = 42;                     // every number is a 64-bit float; there is no integer type
let s = "héllo";                // strings are BYTES: len(s) is 6, s[1] is one byte
let ok = 1 < 2;                 // a tagged boolean: prints "true". It is not the number 1
let nothing = null;
let xs = [1, "two", [3]];       // arrays are mutable
let m = {"host": "localhost", "port": 8080};
let p = Point { x: 1, y: 2 };   // a struct is a map with a "__type__" entry
```

There is one boolean type (`true` / `false`). Comparisons, `!`, `&&` and `||` all produce tagged
booleans, never `1`/`0`. Numbers print as integers when they are whole (`5`, not `5.0`), and `nan`, `inf`
and `-inf` print as written. `type(v)` returns a type name such as `"number"`, `"array"`, `"map"`,
`"null"` or `"function"` (a struct's name for structs).

### Truthiness

`if`, `while`, `?:`, `&&`, `||` and `!` all use the same rule. **Only `false`, `null` and the number `0`
are falsy.**

| Value | Truthy? |
|---|---|
| `false`, `null`, `0` | **no** |
| any other number (including `NaN`) | yes |
| `true` | yes |
| `""` (empty string), `[]`, `{}` | **yes**, unlike JavaScript and Python |
| any non-empty string, array or map; any function | yes |

```donky
if "" { print("an empty string is truthy"); }
if null { print("never printed"); } else { print("null is falsy"); }
print(0 || "hi");               // true: || and && return a boolean, not an operand
```

Conditions do **not** need to be booleans. (Older documentation claimed they must be.)

### Operators

| Operator | Behaviour |
|---|---|
| `+` | number + number, or string + string. Any other mix **throws** `cannot add <type> and <type>`. |
| `-` `*` `/` | `/` is raw IEEE division: `5 / 2` is `2.5` and `x / 0` is `inf` or `nan`, never an error. |
| `div` | Checked integer division, truncating toward zero: `7 div 2` is `3`, `-7 div 2` is `-3`. Division by zero **throws**. |
| `%` | `fmod`; the sign follows the dividend (`-7 % 3` is `-1`). Modulo by zero **throws**. |
| `==` `!=` | Numbers by value; strings by **content**; booleans by value. Arrays and maps by **identity**: `[1] == [1]` is `false`. Mixed types are never equal: `1 == "1"` is `false`. |
| `<` `<=` `>` `>=` | Numbers only. On strings they compare meaningless raw values. |
| `&&` `\|\|` `!` | Short-circuiting; the result is always a tagged boolean. |
| `c ? a : b` | Ternary. |

### Indexing

```donky
let a = [10, 20, 30];
print(a[99]);          // null: out of range never throws
a[1] = 99;             // ok
push(a, 40);           // the only way to grow an array
a[10] = 1;             // THROWS "index out of range" (so does a non-integer index)
let m = {};
print(m["missing"]);   // null
```

### Control flow

```donky
if x > 10 { print("big"); } else if x > 5 { print("medium"); } else { print("small"); }

while i < 5 { i = i + 1; }

for item in [1, 2, 3] { print(item); }
for i in 0 .. 10 { print(i); }       // 0 to 9; the upper bound is evaluated once

while true { if done { break; } continue; }
```

`match` is also available (literal patterns on numbers, strings and booleans, plus `_`).

### Strings

```donky
let name = "Donky";
print("Hello, ${name}! ${1 + 2}");   // interpolation: "Hello, Donky! 3"
print(upper(name));                   // ASCII case mapping
print(substr(name, 0, 3));            // "Don": byte offsets, clamped to the string
print(split("a,b;c", ",;"));          // ["a", "b", "c"]: the delimiter is a SET of characters
print(join(["a", "b"], "-"));         // "a-b"; throws if an element is not a string
print(num("12abc"));                  // 12: num() never fails; "abc" is 0
```

`print(array)` quotes strings inside the array; `print("hi")` does not.

### Functions

```donky
fn add(a, b) { return a + b; }
let f = add;                    // functions are first-class values
print(f(2, 3));                 // 5
fn apply(g, x) { return g(x); } // pass them as arguments
print(apply(add, 1));           // see "arity" below
```

- `fn` declarations are **top-level only**. A `fn` nested inside another function, an `if` or a block is
  silently discarded.
- A function that falls off the end returns `null`.
- **Arity is not checked.** Missing arguments are padded with `0`, and extra arguments are dropped:
  `add(1)` runs as `add(1, 0)`.

### Errors

Any value can be thrown, and exceptions are per-thread.

```donky
try {
    let r = 10 div 0;
} catch e {
    print("caught: " + str(e));     // caught: division by zero
}
```

An exception that nothing catches makes the program print `Uncaught Exception: <value>` and exit with
code 1. Builtins that signal failure by returning a value instead (for example `json_decode` and
`json_encode`) are checked with `json_error()`.

### Structs

`struct Name { ... }` declarations are parsed and ignored: field names and types are **not checked**,
and `Point { z: 3 }` is accepted. A struct value is a map with a `"__type__"` entry, so
`len(Point { x: 1, y: 2 })` is `3`.

### Imports

```donky
import "lib/math.donk";
```

Paths resolve against the build's working directory, not the importing file's directory. A path that
cannot be found is **not an error**: the build prints a `DEBUG:` line and carries on, and the failure
shows up later as `unknown function 'X'`.

---

## 4. C-parity: native interop

**Donky is as powerful as C at the OS boundary.** It can call any C function with exact types, hold raw
memory, change page protections, execute machine code it writes itself, make raw system calls, and
fork and exec. If C can reach it, Donky can reach it.

That is parity of *reach*, not of speed: Donky values are boxed 64-bit floats and programs are not
optimised like C. Handles and pointers are carried as numbers, so a 64-bit integer above 2^53 loses
precision, and pointers must fit in 48 bits (true for user-space addresses on x64 and arm64).

### Calling C: `dll_load`, `dll_sym`, `dll_call_sig`

```donky
let k32 = dll_load("kernel32.dll");
let get_pid = dll_sym(k32, "GetCurrentProcessId");
print(dll_call_sig(get_pid, "u", "", []));        // this process's id
```

`dll_call_sig(fn, ret_sig, arg_sig, args)` takes one letter per type:

| Letter | C type | Letter | C type |
|---|---|---|---|
| `v` | `void` (return only) | `f` | `float` |
| `i` | `int32_t` | `d` | `double` |
| `u` | `uint32_t` | `s` | `char*` (a Donky string, or `null`) |
| `l` | `int64_t` | `p` | `void*` / handle, as a number |
| `L` | `uint64_t` | | |

- `dll_call_var_sig(fn, ret_sig, fixed_sig, var_sig, fixed_args, var_args)` calls variadic C functions
  such as `printf`:
  `dll_call_var_sig(printf, "i", "s", "id", ["%d items, %f total\n"], [3, 9.5]);`
- `dll_callback(ret_sig, arg_sig, func)` turns a Donky function into a C function pointer, for APIs
  that take callbacks.
- `dll_load`, `dll_sym`, `mem_alloc` and `mmap` return `0` on failure. Check before use; `is_valid_handle(h)`
  and `is_null_handle(h)` do this for you.

### Raw memory and executable code

```donky
let code = mem_alloc(4096);                         // read/write, page-aligned
if code == 0 { throw "mem_alloc failed"; }
mem_write(code, [184, 42, 0, 0, 0, 195]);           // x86-64: mov eax, 42 ; ret
mem_protect(code, 4096, 2);                         // 1 = RW, 2 = RX, 3 = RWX
print(dll_call_sig(code, "i", "", []));             // 42: Donky just ran machine code it wrote
mem_free(code, 4096);
```

- `mem_alloc`, `mem_write`, `mem_protect`, `mem_free` work on Windows and POSIX. Hardened systems
  (SELinux, Apple Silicon) may refuse RWX; write, then switch to RX instead.
- `ffi_alloc`, `ffi_free`, `ffi_peek`, `ffi_poke` and `ffi_cstr` read and write raw bytes at an address.

### Raw syscalls (POSIX)

```donky
syscall(1, [1, "hello from write(2)\n", 20]);       // Linux x86-64: write(1, buf, 20)
```

`syscall(nr, args)` takes up to six numbers or strings (a string is passed as its `char*`) and returns the
raw result, `-1` on failure. The number is not validated: a bad one is the kernel's problem, as in C.
Syscall numbers are specific to each architecture.

### Process control

**POSIX:** `fork`, `exec`, `wait`, `pipe`, `dup2`, `mmap`, `munmap`.

```donky
let pid = fork();                                   // 0 in the child
if pid == 0 {
    exec("echo", ["echo", "hello from the child"]); // returns only by throwing, if it fails
    sys_exit(127);
}
print(wait(pid));                                   // exit code; -N if killed by signal N
```

Call `fork()` from the main program, not from inside a `spawn`ed task.

**Windows:** reach `CreateProcessA`, `OpenProcess`, `TerminateProcess`, `WriteProcessMemory` and the
rest of `kernel32.dll` through `dll_call_sig`. The `kill_process(pid)` builtin and `lib/proc.donk`
(`kill_proc`) are examples built that way.

### Security and OS identity (POSIX)

`setuid`, `setgid`, `seteuid`, `setegid`, `chroot`, `unshare`, `prctl`, `seccomp`, `getpwnam`,
`getpwuid`, `uname`, `getrlimit`, `setrlimit`. On Windows these throw an error.

---

## 5. Standard library at a glance

The builtins below are available without an import; `SEMANTICS.md` lists the full set and exact
behaviour.

| Area | Builtins |
|---|---|
| Containers | `len` `push` `pop` `keys` `contains` |
| Strings | `str` `num` `upper` `lower` `substr` `split` `join` `char_code` `byte_at` `bytes_to_str` |
| Math | `floor` `pow` `exp` `log` `tanh` `max` `min` `abs` |
| JSON | `json_decode` `json_encode` `json_pretty` `json_error` |
| Files and OS | `read_file` `write_file` `file_exists` `sys_getenv` `sys_setenv` `sys_cwd` `sys_exit` `now` `now_ms` `sleep` |
| Hashing | `sys_md5` `sys_sha1` `sys_sha256` `sys_hmac_sha256` |
| System | `cpu_usage` `cpu_cores` `uptime` `total_mem` `used_mem` `get_processes` `kill_process` |
| Networking | `tcp_listen` `tcp_accept` `tcp_read` `tcp_write` `tcp_close` `http_get` `http_request` |
| Tensors | `tensor_add` `tensor_mul` `tensor_matmul` `tensor_relu` `tensor_softmax` `tensor_sgd_step` |
| Native | `dll_*` `ffi_*` `mem_*` `syscall` and the process-control set in [section 4](#4-c-parity-native-interop) |
| GUI (Windows) | `gui_init` `gui_add_button` `gui_add_label` `gui_update_text` `gui_get_clicked` `gui_draw_rect` `gui_draw_text` `gui_update` `gui_webview` |

Libraries in `lib/` are imported, not built in: `lib/ai_server.donk` (`http_serve_model`, `tensor_batch`),
`lib/micrograd.donk` (a scalar autograd engine and the repository's worked numerical example),
`lib/winreg.donk` (Windows registry), `lib/proc.donk` (process termination on Windows).

> **Pass every argument.** Because arity is not checked, a forgotten argument becomes `0`. For example
> `tensor_sgd_step(params, grads)` with a missing `lr` silently does no update.

> **`lib/math.donk` shadows builtins** (`abs`, `min`, `max`, `pow`, `floor`) with slower Donky versions
> for any program that imports it. Prefer the builtins.

---

## 6. Concurrency and networking

```donky
fn worker(id) {
    print("worker " + str(id) + " running");
    return 0;
}
spawn(worker, 1);       // spawn(function, one_argument); returns 0 on success, -1 on failure
spawn(worker, 2);
```

- `spawn` runs a function on a thread pool. A spawned function takes **one** argument. Because there
  are no closures, pass state in that argument or share it through `global_set` / `global_get`.
- The program waits for running tasks before it exits.
- Exceptions are per-thread. An uncaught exception in a task ends that task.
- `global_set` / `global_get` are **not locked**. Set shared values before you start tasks, and treat them
  as read-only afterwards.
- `tcp_listen(port)` listens on **all** network interfaces.

---

## 7. Honest limitations

These are facts about the current engine, not plans.

- **Closures are not implemented.** Functions are values you can store and pass around, but a function
  literal cannot capture variables from the enclosing function. A name it cannot resolve evaluates to
  `null` (the build prints a warning), so `fn(x) { return x + n; }` fails at run time with
  `cannot add number and null`. Pass the state in as an argument, or use `global_set` / `global_get`.
- **Struct methods are not implemented.** `fn Point.area(self) { ... }` is not supported; use free
  functions (`fn point_area(p)`). The call syntax `p.area()` is only sugar for the free function
  `area(p)`.
- **There is no automatic garbage collector.** Loops that keep allocating arrays, maps or strings grow
  the heap until the process exits. Call `gc()` yourself at a safe point:

  ```donky
  let i = 0;
  while i < 1000000 {
      process(i);
      if i % 10000 == 0 { gc(); }
      i = i + 1;
  }
  ```

  `gc()` scans only the calling thread, so it **does nothing while any `spawn`ed task is running**. A
  server that handles requests in worker threads (including `http_serve_model`) therefore never
  collects; restart it periodically.
- **Top-level `let` is limited.** A top-level `let` in an **imported file does not execute**, so
  libraries must not rely on file-level constants; return them from a function instead
  (`fn pi() { return 3.14159265358979; }`). In any file, a top-level `let` is also **not visible inside
  functions**; the name evaluates to `null`. Pass values in as arguments.
- **Functions only at top level.** A nested `fn` declaration is silently discarded.
- **Arity is not checked.** Missing arguments become `0`; extra arguments are dropped.
- **Several failures are silent.** A missing import, a struct with a wrong field name, and an undefined
  variable (it becomes `null` plus a build warning) do not stop the build.
- **Oddities that remain:** `null[0]` and `null.f` give `0` or `null` instead of an error; `<` and `>`
  are meaningless on strings; `json_encode` returns `0` on error (check `json_error()`); `split`'s
  delimiter is a character *set*; there is no integer type, so whole numbers are exact only up to 2^53.
- **Strings are bytes**, not Unicode characters.
- **`build-exe` needs a C toolchain** (GCC) and always writes `donky_app` into the current directory.
- **The tree-walker, VM and REPL are frozen** and disagree with the AOT engine. Do not use them to
  check AOT behaviour.
- **Tensors are plain Donky arrays and scalar loops.** They are fine for small models. Large models
  should call native code through `dll_call_sig`.
- **POSIX-only primitives** (`fork`, `exec`, `syscall`, `setuid` and friends) throw
  `not supported on Windows; use dll_call_sig` on Windows.

---

## 8. Built with Donky

Real programs written in Donky itself:

- **Sentinel V2**: a Task Manager and software-inventory tool. A collector samples live process and
  system data every second, reads the installed-software list from the Windows registry through
  `advapi32.dll` (`lib/winreg.donk`), terminates processes through `kernel32.dll` (`lib/proc.donk`), and
  serves it all as a JSON API (`/api/software`, `/api/kill`, ...) behind a web UI. Source:
  `examples/sentinel/`.
- **Donky Shell**: a command shell written in Donky.
- **Donky Defender**: an endpoint detection and response (EDR) tool written in Donky.

All of them rely on the same primitives described above: the FFI, raw memory, process control and the
built-in networking.

---

## License

MIT. See `LICENSE`.
