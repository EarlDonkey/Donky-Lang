# The Donky Programming Language Reference Manual

**Version:** 1.0.0
**Author:** EarlDonkey
**Repository:** https://github.com/EarlDonkey/Donky-Lang
**License:** MIT

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Installation and Tooling](#2-installation-and-tooling)
3. [Lexical Structure](#3-lexical-structure)
4. [Variables and Types](#4-variables-and-types)
5. [Operators](#5-operators)
6. [Control Flow](#6-control-flow)
7. [Functions and Closures](#7-functions-and-closures)
8. [Data Structures](#8-data-structures)
9. [Error Handling](#9-error-handling)
10. [Modules and the Package Manager](#10-modules-and-the-package-manager)
11. [Standard Library Reference](#11-standard-library-reference)
12. [Networking and Concurrency](#12-networking-and-concurrency)
13. [The Bytecode Compiler and VM](#13-the-bytecode-compiler-and-vm)
14. [The Self-Hosted Compiler](#14-the-self-hosted-compiler)
15. [Appendix A: EBNF Grammar](#15-appendix-a-ebnf-grammar)
16. [Appendix B: Opcode Reference](#16-appendix-b-opcode-reference)
17. [Appendix C: Architecture and Internals](#17-appendix-c-architecture-and-internals)
18. [Appendix D: Known Limitations](#18-appendix-d-known-limitations)

---

## 1. Introduction

Donky is a small, dynamic, general-purpose programming language designed to be easily embedded, extended, and understood. It was built entirely from scratch, featuring a custom lexer, a Pratt parser, a static type checker, a tree-walking interpreter, a stack-based bytecode virtual machine, and a self-hosted compiler.

### 1.1 Philosophy
- **Explicit over Implicit:** There is no truthy/falsy coercion. `if` conditions must strictly evaluate to `bool`.
- **Network First:** TCP, HTTP, and WebSocket primitives are baked directly into the standard library.
- **Tooling is Part of the Language:** Donky is not complete without its REPL, its package manager, and its Language Server Protocol (LSP) implementation for VS Code.
- **Self-Hosting:** The language is capable of compiling and interpreting itself.

### 1.2 Architecture Overview
When you execute `donky main.donk`, the following pipeline occurs:
1. **Lexer:** Scans the source file and produces a list of `Token` objects.
2. **Parser:** Consumes tokens and builds an Abstract Syntax Tree (AST).
3. **Module Expander:** Recursively resolves `import` statements and merges ASTs.
4. **Type Checker:** Walks the AST to enforce types, check variable definitions, and verify function arities.
5. **Execution:** The AST is handed to either the Tree-Walking Interpreter or the Bytecode Compiler + VM.

---

## 2. Installation and Tooling

### 2.1 Installation
Donky provides a Windows Installer and a portable ZIP.
- **Installer (`Donky-v1.0.0-Setup.exe`):** Installs to `Program Files`, adds `donky` to the system PATH, and associates `.donk` files.
- **Portable (`Donky-v1.0.0-Windows.zip`):** Extract anywhere. No PATH modifications.

### 2.2 The CLI Commands
- `donky <file.donk>`: Executes the file using the tree-walking interpreter.
- `donky --bytecode <file.donk>`: Executes the file by compiling to bytecode first.
- `donky --check-only <file.donk>`: Runs the parser and type checker but does not execute.
- `donky install`: Runs the package manager.
- `donky`: Opens the interactive Read-Eval-Print Loop (REPL).

### 2.3 The REPL
The REPL maintains a persistent environment.
```text
donky> let x = 10;
donky> x + 5
15
donky> fn greet() { print("hi"); }
donky> greet()
hi
donky> :quit
```

---

## 3. Lexical Structure

### 3.1 Comments
Comments start with `//` and extend to the end of the line.
```donky
// This is a comment
let x = 10; // This is inline
```

### 3.2 Identifiers
Identifiers start with a letter or underscore `_`, and contain letters, numbers, and underscores.
```donky
let valid_name = 1;
let _internal = 2;
```

### 3.3 Keywords
The following words are reserved:
`let`, `fn`, `if`, `else`, `while`, `for`, `in`, `return`, `print`, `true`, `false`, `div`, `break`, `continue`, `try`, `catch`, `throw`, `import`, `struct`.

### 3.4 Literals
- **Integers:** `42`, `-7`
- **Floats:** `3.14`, `-0.5`
- **Strings:** `"hello"`, `"line\nbreak"`, `"interpolated ${x}"`
- **Booleans:** `true`, `false`
- **Arrays:** `[1, 2, 3]`, `[]`
- **Maps:** `{ "key": "value" }`, `{}`

### 3.5 String Interpolation
Strings can contain `${expression}`. The expression is evaluated and concatenated.
```donky
let name = "Alice";
print("Hello, ${name}!");
```

---

## 4. Variables and Types

### 4.1 Variable Declaration
Variables are created with `let`.
```donky
let x = 10;
let name = "Bob";
```

### 4.2 Assignment
Variables can be reassigned using `=`. Reassignment changes the type if necessary.
```donky
let x = 10;
x = "Now a string";
```

### 4.3 Type System
Donky features dynamic typing at runtime but a static type checker at compile time.

| Type | Description | Example |
|------|-------------|---------|
| `number` | Integers and Floats | `42`, `3.14` |
| `string` | UTF-8 text | `"Hello"` |
| `bool` | True/False | `true` |
| `array` | Mutable list | `[1, 2, 3]` |
| `map` | Key-value store | `{"a": 1}` |
| `function` | First-class callable | `fn(x) { ... }` |
| `struct` | Named record | `Point { x: 10 }` |

### 4.4 Type Annotations
Functions can declare return types. The type checker enforces these.
```donky
fn add(a, b) -> num {
    return a + b;
}
```
Valid annotations: `num`, `str`, `bool`, `array`, `map`, `any`, `void`.

---

## 5. Operators

### 5.1 Arithmetic
| Operator | Description |
|----------|-------------|
| `+` | Addition or String Concatenation |
| `-` | Subtraction |
| `*` | Multiplication |
| `/` | Division (always returns a float) |
| `div` | Integer division (truncates towards zero) |
| `%` | Modulo (remainder) |

### 5.2 Comparison
`==`, `!=`, `<`, `<=`, `>`, `>=`

### 5.3 Logical
`&&` (AND), `||` (OR), `!` (NOT)
*Logical operators short-circuit and require boolean operands.*

### 5.4 Precedence
Lowest to Highest:
1. `||`
2. `&&`
3. `==` `!=`
4. `<` `<=` `>` `>=`
5. `+` `-`
6. `*` `/` `%` `div`
7. Unary `-` `!`
8. Postfix `.` `[]`

---

## 6. Control Flow

### 6.1 If / Else
```donky
if x > 10 {
    print("Big");
} else if x > 5 {
    print("Medium");
} else {
    print("Small");
}
```
Donky requires conditions to be `bool`. `if 1` is an error.

### 6.2 While Loops
```donky
let i = 0;
while i < 5 {
    print(i);
    i = i + 1;
}
```

### 6.3 For Loops
For loops iterate over arrays, strings, and maps.
```donky
// Arrays
for item in [1, 2, 3] { print(item); }

// Strings
for char in "hello" { print(char); }

// Maps (iterates over keys)
for key in {"a": 1, "b": 2} { print(key); }
```

### 6.4 Break and Continue
```donky
for i in [1, 2, 3, 4] {
    if i == 2 { continue; }
    if i == 4 { break; }
    print(i);
}
```

---

## 7. Functions and Closures

### 7.1 Declarations
```donky
fn greet(name) -> void {
    print("Hello, " + name);
}
```

### 7.2 Anonymous Functions
```donky
let square = fn(x) -> num { return x * x; };
```

### 7.3 Closures
Closures capture their environment by reference.
```donky
fn make_counter() -> any {
    let count = 0;
    return fn() -> num {
        count = count + 1;
        return count;
    };
}

let c = make_counter();
print(c()); // 1
print(c()); // 2
```

---

## 8. Data Structures

### 8.1 Arrays
```donky
let arr = [1, 2, 3];
push(arr, 4);       // [1, 2, 3, 4]
pop(arr);           // [1, 2, 3]
len(arr);           // 3
arr[0] = 99;
```

### 8.2 Maps
```donky
let m = {"name": "Alice"};
m["age"] = 30;
print(contains(m, "name")); // true
let all_keys = keys(m);
```

### 8.3 Structs
Structs are statically checked maps.
```donky
struct Point {
    x: num,
    y: num
}

let p = Point { x: 10, y: 20 };
p.x = 15;
```

---

## 9. Error Handling

### 9.1 Throw
```donky
fn divide(a, b) -> num {
    if b == 0 { throw "Division by zero"; }
    return a / b;
}
```

### 9.2 Try / Catch
```donky
try {
    print(divide(10, 0));
} catch e {
    print("Caught: " + e);
}
```

---

## 10. Modules and the Package Manager

### 10.1 Imports
Imports must be at the top level.
```donky
import "lib/strings.donk";
```

### 10.2 Package Manager
Create a `donky.json` file:
```json
{
  "name": "my-app",
  "dependencies": {
    "strings": "https://raw.githubusercontent.com/EarlDonkey/Donkey-Lang/main/lib/strings.donk"
  }
}
```
Run `donky install`. Dependencies download to `donky_modules/`.

---

## 11. Standard Library Reference

### 11.1 `lib/list.donk`
- `map(arr, f)`: Apply `f` to each element.
- `filter(arr, f)`: Keep elements where `f` is true.
- `reduce(arr, f, init)`: Fold the array.
- `sort_by(arr, f)`: Stable sort by a derived key.
- `reverse(arr)`: Reverse a new array.
- `unique(arr)`: Remove duplicates.
- `any(arr, f)`: True if any pass.
- `all(arr, f)`: True if all pass.
- `count(arr, f)`: Count passing elements.

### 11.2 `lib/strings.donk`
- `starts_with(s, prefix)`
- `ends_with(s, suffix)`
- `slice(s, start, end)`
- `index_of(s, needle)`

### 11.3 `lib/format.donk`
- `format_num(x, places)`
- `pad_left(s, width, ch)`
- `pad_right(s, width, ch)`
- `repeat(s, n)`

### 11.4 `lib/math.donk`
- `min(a, b)`, `max(a, b)`, `abs(n)`
- `pow(base, exp)`
- `sqrt(n)`

### 11.5 `lib/csv.donk`
- `parse_line(line)`
- `format_line(fields)`
- `parse_table(text)`
- `format_table(rows)`

---

## 12. Networking and Concurrency

### 12.1 TCP Sockets
- `tcp_listen(port) -> num`
- `tcp_accept(server_id) -> num`
- `tcp_read(client_id, max_bytes) -> string`
- `tcp_write(client_id, data) -> void`
- `tcp_close(socket_id) -> void`
- `tcp_connect(ip, port, timeout) -> bool`

### 12.2 Web & HTTP
- `http_get(url) -> string`
- `ws_accept(client_id, key) -> void`
- `ws_read(client_id) -> string`
- `ws_write(client_id, msg) -> void`

### 12.3 Utilities
- `get_local_ip() -> string`
- `ip_to_list(cidr) -> array`
- `dns_lookup(ip) -> string`
- `get_mac(ip) -> string`
- `mime_type(filepath) -> string`
- `url_decode(str) -> string`
- `sleep(seconds) -> void`
- `time() -> number`

### 12.4 Concurrency (`spawn`)
`spawn(fn)` runs a function in a background thread.
```donky
spawn(fn() -> void {
    print("Running in background");
});
```

---

## 13. The Bytecode Compiler and VM

The `compiler/` directory contains a self-hosted Donky compiler.

### 13.1 Compilation Pipeline
1. Source Code → `compiler/lexer.donk`
2. Tokens → `compiler/parser.donk`
3. AST → `compiler/bytecode_compiler.donk`
4. Flat Instructions → `compiler/bytecode_vm.donk`

### 13.2 Using the Compiler
```powershell
donky compiler/compile.donk examples/simple.donk compiled.json
donky compiler/run_json.donk compiled.json
```

---

## 14. Appendix A: EBNF Grammar

```ebnf
program        → statement* EOF

statement      → letStmt | printStmt | ifStmt | whileStmt | forStmt
               | fnStmt | returnStmt | breakStmt | continueStmt
               | tryStmt | throwStmt | importStmt | structStmt
               | block | exprStmt

letStmt        → "let" IDENT "=" expression ";"
printStmt      → "print" "(" expression ")" ";"
ifStmt         → "if" expression block ("else" (ifStmt | block))?
whileStmt      → "while" expression block
forStmt        → "for" IDENT "in" expression block
fnStmt         → "fn" IDENT "(" params? ")" "->" IDENT block
returnStmt     → "return" expression? ";"
breakStmt      → "break" ";"
continueStmt   → "continue" ";"
tryStmt        → "try" block "catch" IDENT block
throwStmt      → "throw" expression ";"
importStmt     → "import" STRING ";"
structStmt     → "struct" IDENT "{" structFields "}"
block          → "{" statement* "}"
exprStmt       → expression ";"

expression     → assignment
assignment     → (call ".")? IDENT "=" assignment | logic_or

logic_or       → logic_and ("||" logic_and)*
logic_and      → equality ("&&" equality)*
equality       → comparison (("==" | "!=") comparison)*
comparison     → term ((">" | ">=" | "<" | "<=") term)*
term           → factor (("-" | "+") factor)*
factor         → unary (("/" | "*" | "%" | "div") unary)*
unary          → ("!" | "-") unary | call
call           → primary ("(" arguments? ")" | "." IDENT | "[" expression "]")*
primary        → NUMBER | STRING | "true" | "false" | "(" expression ")"
               | IDENT | array | map | fnExpr
array          → "[" (expression ("," expression)*)? "]"
map            → "{" (expression ":" expression ("," expression ":" expression)*)? "}"
fnExpr         → "fn" "(" params? ")" "->" IDENT block
```

---

## 15. Appendix B: Opcode Reference

| Opcode | Stack Effect | Description |
|--------|--------------|-------------|
| `CONST idx` | `→ val` | Push constant. |
| `TRUE` / `FALSE` | `→ bool` | Push boolean. |
| `POP` | `val →` | Discard top. |
| `LOAD_LOCAL slot` | `→ val` | Load local var. |
| `STORE_LOCAL slot` | `val →` | Store local var. |
| `LOAD_GLOBAL name` | `→ val` | Load global var. |
| `STORE_GLOBAL name` | `val →` | Store global var. |
| `ADD` / `SUB` / `MUL` / `DIV` | `a, b → c` | Arithmetic. |
| `EQ` / `NE` / `LT` / `LE` / `GT` / `GE` | `a, b → bool` | Comparison. |
| `JUMP idx` | `→` | Unconditional jump. |
| `JUMP_IF_FALSE idx` | `bool →` | Pop, jump if false. |
| `CLOSURE idx, n` | `→ closure` | Create closure. |
| `CALL argc` | `args → result` | Call function. |
| `RETURN` | `val →` | Return from function. |
| `PRINT` | `val →` | Pop and print. |

---

## 16. Appendix C: Architecture and Internals

### 16.1 The Lexer
The lexer is a hand-written state machine. It scans characters, skipping whitespace and `//` comments. It groups characters into `Token` structs. Strings are handled specially to support `\n`, `\t`, and `${}` interpolation.

### 16.2 The Parser
The parser is a **Pratt Parser** (Top-Down Operator Precedence). It assigns a binding power to each infix operator to determine precedence. Statements are parsed using standard recursive descent.

### 16.3 The Type Checker
The type checker performs a single pass over the AST. It maintains a scope tree. It ensures that:
- Variables are declared before use.
- Functions are called with the correct number of arguments.
- Binary operations are only performed on compatible types (e.g., `number + number`).

### 16.4 The VM (Virtual Machine)
The bytecode VM uses a `while` loop with an Instruction Pointer (`ip`). It reads an instruction, modifies the stack, and increments the `ip`. Jumps are handled by directly modifying the `ip`.

---

## 17. Appendix D: Known Limitations

1. **No Garbage Collection:** Donky relies on Python's memory management.
2. **No Static Typing for Variables:** Only function returns and structs enforce types.
3. **No Classes:** Donky uses Structs and Closures instead of Object-Oriented classes.
4. **Single-threaded Compiler:** The self-hosted compiler does not support concurrent compilation.
5. **No Regex:** The standard library has string manipulation, but no regular expressions.
6. **Windows Only Pre-built:** The installer is compiled for Windows x64. Mac/Linux users must run via Python.
