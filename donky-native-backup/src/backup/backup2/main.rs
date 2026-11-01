mod lexer;
mod parser;
mod ast;
mod interpreter;
mod vm;
mod heap;
mod package;
mod repl;
mod profiler;
mod codegen;
mod aot;

use std::env;
use std::fs;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage();
        return;
    }

    match args[1].as_str() {
        "install" => {
            if let Err(e) = package::install_from_manifest() { eprintln!("Package Error: {}", e); }
        }
        "add" => {
            if args.len() < 4 {
                eprintln!("Usage: donky-native add <name> <url-or-path>");
                return;
            }
            if let Err(e) = package::add_dependency(&args[2], &args[3]) {
                eprintln!("Package Error: {}", e);
            }
        }
        "repl" => {
            let script_args: Vec<String> = args[2..].to_vec();
            let mut interp = interpreter::Interpreter::new(script_args);
            repl::run(&mut interp);
        }
        "test" => {
            let path = args.get(2).map(|s| s.as_str());
            if let Err(e) = cmd_test(path) {
                eprintln!("Test runner: {}", e);
                std::process::exit(1);
            }
        }
        "bench" => {
            let path = args.get(2).map(|s| s.as_str());
            if let Err(e) = cmd_bench(path) {
                eprintln!("Bench: {}", e);
                std::process::exit(1);
            }
        }
        "profile" => {
            if args.len() < 3 {
                eprintln!("Usage: donky-native profile <file.donk>");
                return;
            }
            if let Err(e) = cmd_profile(&args[2]) {
                eprintln!("Profile: {}", e);
                std::process::exit(1);
            }
        }
        "build-native" => {
            if args.len() < 3 {
                eprintln!("Usage: donky-native build-native <file.donk>");
                return;
            }
            let path = &args[2];
            let source = match fs::read_to_string(path) {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("Runtime Error: build-native: {}", e);
                    return;
                }
            };
            let tokens = match lexer::Lexer::new(&source).tokenize() {
                Ok(t) => t,
                Err(e) => { eprintln!("Lex Error: {}", e); return; }
            };
            let ast = match parser::Parser::new(tokens).parse() {
                Ok(a) => a,
                Err(e) => { eprintln!("Parse Error: {}", e); return; }
            };
            
            let mut compiler = codegen::NativeCompiler::new();
            println!("Compiling natively...");
            let start = std::time::Instant::now();
            compiler.compile_and_run(&ast);
            println!("Native execution finished in {:?}", start.elapsed());
        }
        "build-exe" => {
            if args.len() < 3 {
                eprintln!("Usage: donky-native build-exe <file.donk>");
                return;
            }
            let path = &args[2];
            let source = match fs::read_to_string(path) {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("Runtime Error: build-exe: {}", e);
                    return;
                }
            };
            let tokens = match lexer::Lexer::new(&source).tokenize() {
                Ok(t) => t,
                Err(e) => { eprintln!("Lex Error: {}", e); return; }
            };
            let ast = match parser::Parser::new(tokens).parse() {
                Ok(a) => a,
                Err(e) => { eprintln!("Parse Error: {}", e); return; }
            };
            
            println!("Compiling to object file...");
            let aot = aot::AotCompiler::new();
            let obj_path = "donky_app.obj";
            aot.compile_to_object(&ast, obj_path).unwrap();
            
            println!("Writing C runtime...");
            let c_rt = r#"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG_MASK 0xFFF8000000000000ULL
#define TAG_BASE 0x7FF8000000000000ULL
#define TAG_BOOL 1ULL
#define TAG_STRING 2ULL
#define TAG_ARRAY 3ULL
#define TAG_MAP 4ULL

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
    print_value(val);
    printf("\n");
}

uint64_t donky_truthy(uint64_t val) {
    if (is_number(val)) return make_number(as_number(val) != 0.0 ? 1.0 : 0.0);
    if (get_tag(val) == TAG_BOOL) return make_number((val & 1) ? 1.0 : 0.0);
    return make_number(1.0); // Arrays, Maps, Strings are truthy
}

int donky_has_exception() { return current_exception != 0; }
uint64_t donky_get_exception() { return current_exception; }
void donky_clear_exception() { current_exception = 0; }

void donky_throw(uint64_t val) {
    current_exception = val;
}

uint64_t donky_alloc_string(const char* s) {
    char* copy = strdup(s);
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)copy;
}

uint64_t donky_alloc_array() {
    Array* arr = malloc(sizeof(Array));
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
    Map* map = malloc(sizeof(Map));
    map->keys = NULL; map->vals = NULL; map->len = 0; map->cap = 0;
    return TAG_BASE | (TAG_MAP << 48) | (uint64_t)map;
}

uint64_t donky_map_set(uint64_t map_val, uint64_t key, uint64_t val) {
    Map* map = (Map*)as_index(map_val);
    for (size_t i = 0; i < map->len; i++) {
        if (map->keys[i] == key) { map->vals[i] = val; return 0; }
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
            if (map->keys[i] == index) return map->vals[i];
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
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING) {
        return make_number(strcmp((char*)as_index(a), (char*)as_index(b)) == 0 ? 1.0 : 0.0);
    }
    return make_number(a == b ? 1.0 : 0.0);
}

uint64_t donky_add(uint64_t a, uint64_t b) {
    if (is_number(a) && is_number(b)) {
        return make_number(as_number(a) + as_number(b));
    }
    if (get_tag(a) == TAG_STRING && get_tag(b) == TAG_STRING) {
        char* s1 = (char*)as_index(a);
        char* s2 = (char*)as_index(b);
        size_t len1 = strlen(s1);
        size_t len2 = strlen(s2);
        char* buf = malloc(len1 + len2 + 1);
        strcpy(buf, s1);
        strcat(buf, s2);
        return TAG_BASE | (TAG_STRING << 48) | (uint64_t)buf;
    }
    return 0;
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
    size_t start = (size_t)as_number(start_val);
    size_t end = (size_t)as_number(end_val);
    size_t len = strlen(s);
    
    if (start > len) start = len;
    if (end > len) end = len;
    if (start > end) start = end;
    
    size_t sub_len = end - start;
    char* buf = malloc(sub_len + 1);
    memcpy(buf, s + start, sub_len);
    buf[sub_len] = 0;
    
    return TAG_BASE | (TAG_STRING << 48) | (uint64_t)buf;
}

uint64_t donky_len(uint64_t val) {
    if (get_tag(val) == TAG_ARRAY) {
        return make_number((double)((Array*)as_index(val))->len);
    } else if (get_tag(val) == TAG_STRING) {
        return make_number((double)strlen((char*)as_index(val)));
    } else if (get_tag(val) == TAG_MAP) {
        return make_number((double)((Map*)as_index(val))->len);
    }
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
        if (d == (long long)d) sprintf(buf, "%lld", (long long)d);
        else sprintf(buf, "%g", d);
    } else if (get_tag(val) == TAG_STRING) {
        return val;
    } else if (get_tag(val) == TAG_BOOL) {
        strcpy(buf, (val & 1) ? "true" : "false");
    } else if (get_tag(val) == TAG_ARRAY) { strcpy(buf, "[array]"); }
    else if (get_tag(val) == TAG_MAP) { strcpy(buf, "{map}"); }
    else { strcpy(buf, "null"); }
    return donky_alloc_string(buf);
}

uint64_t donky_num(uint64_t val) {
    if (is_number(val)) return val;
    if (get_tag(val) == TAG_STRING) return make_number(atof((char*)as_index(val)));
    return make_number(0.0);
}

uint64_t donky_read_file(uint64_t path_val) {
    if (get_tag(path_val) != TAG_STRING) return 0;
    FILE* f = fopen((char*)as_index(path_val), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = 0;
    fclose(f);
    uint64_t res = donky_alloc_string(buf);
    free(buf);
    return res;
}

// --- Argument Handling ---
uint64_t donky_args_array = 0;

void donky_set_args(int argc, char** argv) {
    uint64_t arr_val = donky_alloc_array();
    for (int i = 1; i < argc; i++) { // Skip argv[0] (exe name)
        donky_array_push(arr_val, donky_alloc_string(argv[i]));
    }
    donky_args_array = arr_val;
}

uint64_t donky_get_args() {
    return donky_args_array;
}

int main(int argc, char** argv) {
    donky_set_args(argc, argv);
    extern int64_t donky_toplevel();
    donky_toplevel();
    
    if (current_exception != 0) {
        printf("Uncaught exception: ");
        print_value(current_exception);
        printf("\n");
        return 1;
    }
    return 0;
}
"#;
            fs::write("runtime.c", c_rt).unwrap();
            
            println!("Linking with GCC...");
            let status = std::process::Command::new("gcc")
                .args(&[obj_path, "runtime.c", "-o", "donky_app.exe"])
                .status()
                .expect("Failed to run gcc. Make sure GCC/MinGW is in your PATH.");
                
            if status.success() {
                println!("Successfully built donky_app.exe!");
            } else {
                eprintln!("Linker failed.");
            }
        }
        "build" => {
            if args.len() < 3 {
                eprintln!("Usage: donky-native build <file.donk>");
                return;
            }
            if let Err(e) = cmd_build(&args[2]) {
                eprintln!("Build Error: {}", e);
            }
        }
        "run" | "vm" => {
            if args.len() < 3 {
                eprintln!("Usage: donky-native run <file.donk|file.donk.bc> [args...]");
                return;
            }
            let script_args: Vec<String> = args[3..].to_vec();
            if let Err(e) = cmd_run(&args[2], script_args) {
                eprintln!("Runtime Error: {}", e);
            }
        }
        _ => {
            let path = &args[1];
            let script_args: Vec<String> = args[2..].to_vec();
            if let Err(e) = cmd_tree(path, script_args) {
                eprintln!("Runtime Error: {}", e);
            }
        }
    }
}

fn print_usage() {
    println!("Usage:");
    println!("  donky-native <file.donk>                 tree-walker (legacy)");
    println!("  donky-native run <file.donk|file.bc>     VM (default)");
    println!("  donky-native build <file.donk>           compile to <file.donk.bc>");
    println!("  donky-native build-native <file.donk>    JIT compile & run");
    println!("  donky-native build-exe <file.donk>       compile to standalone .exe");
    println!("  donky-native test [dir|file]             run tests (default: ./tests)");
    println!("  donky-native bench [dir]                 benchmark (default: ./bench)");
    println!("  donky-native profile <file.donk>         opcode profile + folded stacks");
    println!("  donky-native repl                        interactive REPL");
    println!("  donky-native install                     install from donky.json");
    println!("  donky-native add <name> <url|path>       add dependency");
}

fn cmd_tree(path: &str, script_args: Vec<String>) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| format!("{}: {}", path, e))?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let mut interp = interpreter::Interpreter::new(script_args);
    interp.interpret(ast)
}

fn cmd_run(path: &str, script_args: Vec<String>) -> Result<(), String> {
    if path.ends_with(".bc") {
        let bundle = vm::load_bytecode_auto(path)?;
        let mut machine = vm::VM::new();
        machine.run(bundle.code, bundle.funcs, bundle.string_pool, script_args)?;
        return Ok(());
    }
    let source = fs::read_to_string(path).map_err(|e| format!("{}: {}", path, e))?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let compiler = vm::Compiler::new();
    let (code, funcs, pool) = compiler.compile(ast);
    let mut machine = vm::VM::new();
    machine.run(code, funcs, pool, script_args)?;
    Ok(())
}

fn cmd_build(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| format!("{}: {}", path, e))?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let compiler = vm::Compiler::new();
    let (code, funcs, pool) = compiler.compile(ast);
    let n_main = code.len();
    let n_funcs = funcs.len();
    let n_pool = pool.len();
    let out_path = format!("{}.bc", path);
    vm::save_bytecode_bin(&out_path, code, funcs, pool)?;
    let file_size = fs::metadata(&out_path).map(|m| m.len()).unwrap_or(0);
    println!("Built {} ({} main ops, {} functions, {} strings, {} bytes)",
        out_path, n_main, n_funcs, n_pool, file_size);
    Ok(())
}

fn cmd_profile(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| format!("{}: {}", path, e))?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let compiler = vm::Compiler::new();
    let (code, funcs, pool) = compiler.compile(ast);
    let mut machine = vm::VM::new();
    machine.silent = true;
    let report = machine.run_profiled(code, funcs, pool, vec![])?;
    println!("{}", report.ascii_report());

    let folded_path = "profile.folded";
    fs::write(folded_path, report.folded_stacks()).map_err(|e| e.to_string())?;
    println!("Folded stacks written to {}\n", folded_path);
    println!("To generate an SVG flame graph:");
    println!("  cargo install inferno");
    println!("  inferno-flamegraph {} > profile.svg", folded_path);
    Ok(())
}

fn cmd_test(arg: Option<&str>) -> Result<(), String> {
    let targets: Vec<std::path::PathBuf> = match arg {
        Some(p) if std::path::Path::new(p).is_file() => vec![std::path::PathBuf::from(p)],
        Some(p) if std::path::Path::new(p).is_dir() => collect_donk_files(p)?,
        Some(p) => return Err(format!("path not found: {}", p)),
        None => collect_donk_files("tests")?,
    };
    if targets.is_empty() {
        return Err("no .donk test files found".to_string());
    }
    let mut passed = 0usize;
    let mut failed = 0usize;
    let mut failures: Vec<(String, String)> = Vec::new();
    println!("Running {} test file(s)...\n", targets.len());
    for path in &targets {
        let name = path.strip_prefix("tests").unwrap_or(path)
            .to_string_lossy().trim_start_matches(['/', '\\']).to_string();
        match run_file(path.to_str().unwrap_or("")) {
            Ok(_) => { println!("  \x1b[32mPASS\x1b[0m  {}", name); passed += 1; }
            Err(e) => { println!("  \x1b[31mFAIL\x1b[0m  {}", name); failures.push((name, e)); failed += 1; }
        }
    }
    println!("\n{} passed, {} failed ({} total)", passed, failed, passed + failed);
    if !failures.is_empty() {
        println!("\nFailures:");
        for (name, err) in failures {
            println!("  {}:", name);
            for line in err.lines() { println!("    {}", line); }
        }
        return Err(format!("{} test(s) failed", failed));
    }
    Ok(())
}

fn collect_donk_files(dir: &str) -> Result<Vec<std::path::PathBuf>, String> {
    let mut files = Vec::new();
    let entries = fs::read_dir(dir).map_err(|e| format!("{}: {}", dir, e))?;
    for entry in entries.flatten() {
        let p = entry.path();
        if p.is_file() && p.extension().and_then(|s| s.to_str()) == Some("donk") {
            files.push(p);
        }
    }
    files.sort();
    Ok(files)
}

fn run_file(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| e.to_string())?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let compiler = vm::Compiler::new();
    let (code, funcs, pool) = compiler.compile(ast);
    let mut machine = vm::VM::new();
    machine.run(code, funcs, pool, vec![])
}

fn cmd_bench(arg: Option<&str>) -> Result<(), String> {
    use std::time::Instant;
    let dir = arg.unwrap_or("bench");
    let files = collect_donk_files(dir)?;
    if files.is_empty() {
        return Err(format!("no .donk files in {}", dir));
    }

    const ITERS: usize = 10;
    println!("Benchmarking {} file(s), {} iterations each\n", files.len(), ITERS);
    println!("{:<20} {:>9} {:>11} {:>13} {:>10} {:>12}", "file", "compile", "run(src)", "load+run(.bc)", "ns/op", "ns/call");
    println!("{}", "-".repeat(82));
    println!("  (ns/op = nanoseconds per VM instruction, ns/call = nanoseconds per function call)\n");

    let mut total_compile = 0.0f64;
    let mut total_run = 0.0f64;
    let mut total_bc = 0.0f64;

    for path in &files {
        let p = path.to_str().unwrap_or("");
        let name = path.file_name().unwrap_or_default().to_string_lossy().to_string();
        let source = fs::read_to_string(p).map_err(|e| e.to_string())?;

        let (total_ops, total_calls) = {
            let toks = lexer::Lexer::new(&source).tokenize()?;
            let ast = parser::Parser::new(toks).parse()?;
            let (code, funcs, pool) = vm::Compiler::new().compile(ast);
            let mut m = vm::VM::new();
            m.silent = true;
            let report = m.run_profiled(code, funcs, pool, vec![])?;
            (
                report.total_instructions,
                report.call_counts.values().sum::<u64>(),
            )
        };

        let t = Instant::now();
        for _ in 0..ITERS {
            let toks = lexer::Lexer::new(&source).tokenize()?;
            let ast = parser::Parser::new(toks).parse()?;
            let _ = vm::Compiler::new().compile(ast);
        }
        let compile_ms = t.elapsed().as_secs_f64() * 1000.0 / ITERS as f64;

        let t = Instant::now();
        for _ in 0..ITERS {
            let toks = lexer::Lexer::new(&source).tokenize()?;
            let ast = parser::Parser::new(toks).parse()?;
            let (code, funcs, pool) = vm::Compiler::new().compile(ast);
            let mut m = vm::VM::new();
            m.silent = true;
            let _ = m.run(code, funcs, pool, vec![]);
        }
        let run_ms = t.elapsed().as_secs_f64() * 1000.0 / ITERS as f64;

        let exec_only_ms = (run_ms - compile_ms).max(0.0);
        
        let ns_per_op = if total_ops > 0 { (exec_only_ms * 1_000_000.0) / (total_ops as f64) } else { 0.0 };
        let ns_per_call = if total_calls > 0 { (exec_only_ms * 1_000_000.0) / (total_calls as f64) } else { 0.0 };

        let toks = lexer::Lexer::new(&source).tokenize()?;
        let ast = parser::Parser::new(toks).parse()?;
        let (code, funcs, pool) = vm::Compiler::new().compile(ast);
        let tmp = format!("{}/.tmp_{}.bc", dir, name);
        vm::save_bytecode_bin(&tmp, code, funcs, pool)?;

        {
            let b = vm::load_bytecode_auto(&tmp)?;
            let mut m = vm::VM::new();
            m.silent = true;
            let _ = m.run(b.code, b.funcs, b.string_pool, vec![]);
        }

        let bc_bytes = fs::read(&tmp).map_err(|e| e.to_string())?;
        let t = Instant::now();
        for _ in 0..ITERS {
            let bundle: vm::CompiledBundle = if !bc_bytes.is_empty() && bc_bytes[0] == 0xBC {
                bincode::deserialize(&bc_bytes[1..]).map_err(|e| e.to_string())?
            } else {
                serde_json::from_slice(&bc_bytes).map_err(|e| e.to_string())?
            };
            let mut m = vm::VM::new();
            m.silent = true;
            let _ = m.run(bundle.code, bundle.funcs, bundle.string_pool, vec![]);
        }
        let bc_ms = t.elapsed().as_secs_f64() * 1000.0 / ITERS as f64;

        let _ = fs::remove_file(&tmp);

        total_compile += compile_ms;
        total_run += run_ms;
        total_bc += bc_ms;

        println!("{:<20} {:>7.2}ms {:>9.2}ms {:>11.2}ms {:>8.1}ns {:>10.1}ns", name, compile_ms, run_ms, bc_ms, ns_per_op, ns_per_call);
    }

    let n = files.len() as f64;
    println!("{}", "-".repeat(82));
    println!("{:<20} {:>7.2}ms {:>9.2}ms {:>11.2}ms", "MEAN", total_compile/n, total_run/n, total_bc/n);
    Ok(())
}