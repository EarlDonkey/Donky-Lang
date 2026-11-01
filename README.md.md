# Donky Programming Language

**Donky** is a high-performance, lightweight, dynamically typed scripting language built in Rust. It features a versatile execution architecture capable of tree-walk interpretation, stack-based bytecode VM execution, Cranelift JIT compilation, and Ahead-Of-Time (AOT) compilation into standalone native executables (`.exe`).

---

## Key Highlights & Features

### 🚀 Execution Engines
- **Tree-Walker Interpreter**: Instant script execution ideal for rapid prototyping and interactive REPL sessions.
- **Bytecode VM**: Fast stack-based virtual machine with binary bytecode serialization (`.bc`) and a multi-pass peephole optimizer.
- **Cranelift JIT Compiler**: Just-In-Time native code compilation for high-performance in-memory execution (`build-native`).
- **AOT Native Compiler**: Compiles `.donk` source code down to native object files (`.obj`) and links them with a low-overhead C runtime via GCC into standalone Windows executables (`build-exe`).

### 🧮 Data Representation & Garbage Collection
- **64-bit NaN Boxing**: All primitive types, object pointers, string indices, and booleans are encoded into NaN-boxed `u64` values.
- **Mark-and-Sweep Garbage Collector**: Built-in precise/stack-scanning memory management with automatic collection cycles.

### 🎨 Native Win32 GUI Subsystem
- **Double-Buffered Graphics Engine**: Render vector shapes (`gui_draw_rect`), text (`gui_draw_text`), and custom UI frames without flicker.
- **Native Windows Controls**: Instantiate native buttons (`gui_add_button`), labels (`gui_add_label`), update text dynamically (`gui_update_text`), and handle click event loops (`gui_get_clicked`).
- **WebView Integration**: Embedded web UI launcher (`gui_webview`) for hybrid web/desktop apps.

### 📊 System Telemetry & Process Management
- Query host CPU usage percentage (`cpu_usage`), active CPU cores (`cpu_cores`), and system uptime (`uptime`).
- Inspect process tables (`get_processes`), inspect memory usage (`total_mem`, `used_mem`), disk space (`disk_info`), and terminate processes (`kill_process`).

### 🌐 Concurrency & Networking
- **TCP Sockets**: Non-blocking/blocking TCP server listening (`tcp_listen`), client connection accepting (`tcp_accept`), reading (`tcp_read`), writing (`tcp_write`), and socket closing (`tcp_close`).
- **HTTP Client**: Built-in HTTP GET requests (`http_get`) powered by WinHTTP.
- **Multithreading**: Spawn concurrent background threads (`spawn`) with synchronized execution locks.

---

## Command Line Interface (CLI)

```bash
# Execute scripts via the Tree-Walker Interpreter
donky-native <file.donk> [args...]

# Run source or pre-compiled binary bytecode on the VM
donky-native run <file.donk | file.bc> [args...]

# Compile source to optimized binary bytecode (.bc)
donky-native build <file.donk>

# JIT-compile and execute natively using Cranelift
donky-native build-native <file.donk>

# AOT-compile into a standalone Windows executable (.exe)
donky-native build-exe <file.donk>

# Run test runner over a directory or file
donky-native test [dir | file]

# Benchmark execution speed across test suites
donky-native bench [dir]

# Profile VM execution and output stack traces
donky-native profile <file.donk>

# Interactive REPL session
donky-native repl

# Package Management
donky-native install
donky-native add <name> <url|path>
```

---

## Language Syntax & Code Examples

### 1. Variables, Control Flow & String Interpolation
```donky
let name = "Donky";
let version = 1.0;

if version >= 1.0 {
    print("Welcome to ${name} v${version}!");
} else {
    print("Pre-release version.");
}

// Loop over arrays
let numbers = [10, 20, 30, 40];
for n in numbers {
    print("Value: ${n}");
}
```

### 2. Structs, Methods & Pattern Matching
```donky
struct Dog {
    name: string,
    age: number
}

fn Dog.speak(self) {
    print("${self.name} says Woof!");
}

let pet = Dog{ name: "Barnaby", age: 3 };
pet.speak();

// Pattern matching
let status = 200;
match status {
    200 { print("Success"); }
    404 { print("Not Found"); }
    _   { print("Unknown status"); }
}
```

### 3. Error Handling (`try / catch / throw`)
```donky
try {
    let content = read_file("missing_file.txt");
    print(content);
} catch err {
    print("Handled Exception: ${err}");
}
```

### 4. Native GUI & Window Creation
```donky
// Initialize a 800x600 window
gui_init("Haven & Hope Sanctuary UI", 800, 600);

let btn_id = gui_add_button("Click Me", 50, 50, 120, 40);
let lbl_handle = gui_add_label("Status: Ready", 50, 110, 200, 30);

while true {
    gui_update();
    let clicked = gui_get_clicked();
    if clicked == btn_id {
        gui_update_text(lbl_handle, "Status: Button Clicked!");
    }
    sleep(16); // ~60 FPS
}
```

### 5. System Telemetry & Metrics
```donky
let cpu = cpu_usage();
let cores = cpu_cores();
let memory = used_mem();
let disk = disk_info();

print("CPU Usage: ${cpu}% across ${cores} cores");
print("Memory Used: ${memory} MB");
print("Disk Free: ${disk.free_gb} GB / ${disk.total_gb} GB");
```

---

## Compiler Architecture & Optimizations

```
  +-------------------------------------------------------+
  |                   .donk Source Code                   |
  +-------------------------------------------------------+
                              |
                     [Lexer & Parser]
                              |
                        [AST Generation]
                              |
        +---------------------+---------------------+
        |                     |                     |
[Tree-Walker]          [VM Bytecode]       [Cranelift Codegen]
  Interpreter            Compiler              & AOT Linker
        |                     |                     |
        v                     v                     v
 Direct Execution     Peephole Optimizer      DCE Optimization
                     Binary Bytecode (.bc)   Object File (.obj)
                              |                     |
                              v               [GCC Linking]
                           Stack VM                 |
                           Execution                v
                                            Standalone (.exe)
```

1. **Dead Code Elimination (DCE)**: The AOT compiler performs static variable read-analysis, safely stripping unreferenced variables before Cranelift IR generation.
2. **Bytecode Peephole Optimizer**: Fuses repeated patterns like `GetLocal` + `GetLocal` + `BinaryOp` into single fused opcodes (`BinLocals`, `AddLocalMulLocal`, `IncLocalJump`, `PushLocalAddConst`), drastically cutting VM dispatch cycles.
3. **Native Linkage**: Standalone builds automatically bundle required C runtime libraries (`ws2_32`, `psapi`, `winhttp`, `gdi32`, `ole32`, `shell32`).

---

## License
Distributed under the MIT License. Developed for software research, systems programming, and application development.