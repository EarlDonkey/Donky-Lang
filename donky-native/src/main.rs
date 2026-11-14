mod lexer;
mod parser;
mod ast;
mod interpreter;
mod vm;
mod heap;
mod package;
mod repl;
mod profiler;
mod aot;
mod typecheck;


use std::env;
use std::fs;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 { print_usage(); return; }

    match args[1].as_str() {
        "--help" | "-h" => print_usage(),
        "--version" | "-V" => println!("Donky {}", env!("CARGO_PKG_VERSION")),
        "install" => { if let Err(e) = package::install_from_manifest() { eprintln!("Package Error: {}", e); } }
        "add" => {
            if args.len() < 4 { eprintln!("Usage: donky-native add <name> <url-or-path>"); return; }
            if let Err(e) = package::add_dependency(&args[2], &args[3]) { eprintln!("Package Error: {}", e); }
        }
        "repl" => {
            let script_args: Vec<String> = args[2..].to_vec();
            let mut interp = interpreter::Interpreter::new(script_args);
            repl::run(&mut interp);
        }
        "test" => { if let Err(e) = cmd_test(args.get(2).map(|s| s.as_str())) { eprintln!("Test runner: {}", e); std::process::exit(1); } }
        "bench" => { if let Err(e) = cmd_bench(args.get(2).map(|s| s.as_str())) { eprintln!("Bench: {}", e); std::process::exit(1); } }
        "profile" => {
            if args.len() < 3 { eprintln!("Usage: donky-native profile <file.donk>"); return; }
            if let Err(e) = cmd_profile(&args[2]) { eprintln!("Profile: {}", e); std::process::exit(1); }
        }
        "build-exe" => {
            if args.len() < 3 { eprintln!("Usage: donky-native build-exe <file.donk>"); return; }
            let path = &args[2];
            let source = match fs::read_to_string(path) { Ok(s) => s, Err(e) => { eprintln!("Runtime Error: build-exe: {}", e); return; } };
            let tokens = match lexer::Lexer::new(&source).tokenize() { Ok(t) => t, Err(e) => { eprintln!("Lex Error: {}", e); return; } };
            let ast = match parser::Parser::new(tokens).parse() { Ok(a) => a, Err(e) => { eprintln!("Parse Error: {}", e); return; } };
            if let Err(e) = typecheck::check_program(&ast, path, &source) { eprint!("{}", e); return; }

            println!("Compiling to object file...");
            let aot = aot::AotCompiler::new();
            let obj_path = "donky_app.obj";
            if let Err(e) = aot.compile_to_object(&ast, obj_path) { eprintln!("Error: {}", e); return; }

            println!("Writing C runtime and linking with GCC...");
            let exe = if cfg!(target_os = "windows") { "donky_app.exe" } else { "donky_app" };
            match link_executable(obj_path, exe) {
                Ok(()) => println!("Successfully built {}!", exe),
                Err(e) => { eprintln!("{}", e); std::process::exit(1); }
            }
        }
        "build" => {
            if args.len() < 3 { eprintln!("Usage: donky-native build <file.donk>"); return; }
            if let Err(e) = cmd_build(&args[2]) { eprintln!("Build Error: {}", e); }
        }
        "run" | "vm" => {
            if args.len() < 3 { eprintln!("Usage: donky-native run <file.donk|file.donk.bc> [args...]"); return; }
            let script_args: Vec<String> = args[3..].to_vec();
            if let Err(e) = cmd_run(&args[2], script_args) { eprintln!("Runtime Error: {}", e); }
        }
        _ => {
            let path = &args[1];
            let script_args: Vec<String> = args[2..].to_vec();
            if let Err(e) = cmd_tree(path, script_args) { eprintln!("Runtime Error: {}", e); }
        }
    }
}

fn print_usage() {
    println!("Donky {}", env!("CARGO_PKG_VERSION"));
    println!();
    println!("Usage:");
    println!("  donky <file.donk>                 tree-walker");
    println!("  donky run <file.donk|file.bc>     VM");
    println!("  donky build <file.donk>           compile to .bc");
    println!("  donky build-exe <file.donk>       standalone .exe (needs gcc on PATH)");
    println!("  donky test [dir|file]             run tests");
    println!("  donky bench [dir]                 benchmark");
    println!("  donky profile <file.donk>         profile");
    println!("  donky repl                        REPL");
    println!("  donky install                     install packages");
    println!("  donky add <name> <url|path>       add dependency");
    println!("  donky --help | --version");
}

#[cfg(target_os = "windows")]
fn install_root() -> Option<std::path::PathBuf> {
    // <root>\bin\donky.exe -> <root>
    std::env::current_exe().ok()?.parent()?.parent().map(|p| p.to_path_buf())
}

/// Search order for the pre-compiled C runtime (best to worst):
///   1. {exe_dir}/../lib/runtime.o   installed layout   (C:\Program Files\Donky\bin\donky.exe)
///   2. {exe_dir}/runtime.o          portable / dev layout
///   3. ./runtime.o                  dev layout from the repo root
///   4. ./dist/lib/runtime.o         build.ps1 staging
#[cfg(target_os = "windows")]
fn find_runtime_obj() -> Result<std::path::PathBuf, String> {
    let mut c: Vec<std::path::PathBuf> = Vec::new();
    if let Some(root) = install_root() { c.push(root.join("lib").join("runtime.o")); }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(d) = exe.parent() { c.push(d.join("runtime.o")); }
    }
    c.push("runtime.o".into());
    c.push("dist/lib/runtime.o".into());
    for p in c { if p.is_file() { return Ok(p); } }
    Err("runtime.o not found (expected <install>\\lib\\runtime.o). Run build.ps1.".to_string())
}

#[cfg(target_os = "windows")]
fn find_gcc() -> String {
    if let Some(root) = install_root() {
        let g = root.join("toolchain").join("bin").join("gcc.exe");
        if g.exists() { return g.to_string_lossy().into_owned(); }
    }
    "gcc".to_string()
}

/// Writes the embedded C runtime next to the object file and links the final executable.
/// Shared by `build-exe` and `test` (the test runner used to hard-code Windows-only link flags
/// and `test_app.exe`, so `donky-native test` could not work on Linux/macOS).
fn link_executable(obj_path: &str, exe_path: &str) -> Result<(), String> {
    // Installed/staged layout: link the pre-compiled runtime.o (see find_runtime_obj).
    // Dev checkout, or DONKY_RUNTIME=source: compile the C runtime embedded in this binary.
    #[cfg(target_os = "windows")]
    let prebuilt: Option<std::path::PathBuf> =
        if env::var("DONKY_RUNTIME").map(|v| v == "source").unwrap_or(false) { None } else { find_runtime_obj().ok() };
    #[cfg(not(target_os = "windows"))]
    let prebuilt: Option<std::path::PathBuf> = None;

    let runtime_input: String = match &prebuilt {
        Some(p) => {
            println!("Linking prebuilt runtime: {}", p.display());
            p.to_string_lossy().into_owned()
        }
        None => {
            fs::write("runtime.c", include_str!("runtime_template.c"))
                .map_err(|e| format!("Could not write runtime.c: {}", e))?;
            "runtime.c".to_string()
        }
    };

    #[cfg(target_os = "windows")]
    let gcc_bin = find_gcc();
    #[cfg(not(target_os = "windows"))]
    let gcc_bin = "gcc".to_string();

    let mut args: Vec<String> = vec![
        obj_path.to_string(), runtime_input,
        "-o".to_string(), exe_path.to_string(),
    ];
    #[cfg(target_os = "windows")]
    {
        args.push("-mstackrealign".to_string());
        // Static libffi: ffi.h must not use dllimport (matters when DONKY_RUNTIME=source compiles runtime.c here).
        args.push("-DFFI_STATIC_BUILD".to_string());
        // Installed layout: the installer ships <root>\lib\libffi.a, so users whose MinGW has no libffi still link.
        if let Some(root) = install_root() {
            let lib = root.join("lib");
            if lib.join("libffi.a").is_file() {
                args.push(format!("-L{}", lib.to_string_lossy().replace('\\', "/")));
            }
        }
        for l in ["-lws2_32", "-lpsapi", "-lm", "-lwinhttp", "-lgdi32", "-lole32", "-loleaut32", "-luuid", "-lshell32", "-lbcrypt"] {
            args.push(l.to_string());
        }
        // -Bstatic => only libffi.a is considered, so the exe never needs libffi-8.dll.
        for l in ["-Wl,-Bstatic", "-lffi", "-Wl,-Bdynamic"] { args.push(l.to_string()); }
    }
    #[cfg(target_os = "linux")]
    {
        for l in ["-lpthread", "-lcurl", "-lffi", "-lm", "-ldl"] { args.push(l.to_string()); }
    }
    #[cfg(target_os = "macos")]
    {
        args.push("-lcurl".to_string());
        args.push("-lffi".to_string());
    }

    let status = std::process::Command::new(&gcc_bin)
        .args(&args)
        .status()
        .map_err(|e| format!("Failed to run {}: {}. donky build-exe needs GCC (MinGW-w64) on your PATH to link executables.", gcc_bin, e))?;
    if status.success() { Ok(()) } else { Err("Linker failed.".to_string()) }
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
    let (code, funcs, pool) = vm::Compiler::new().compile(ast);
    vm::VM::new().run(code, funcs, pool, script_args)
}

fn cmd_build(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| format!("{}: {}", path, e))?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let (code, funcs, pool) = vm::Compiler::new().compile(ast);
    let out_path = format!("{}.bc", path);
    vm::save_bytecode_bin(&out_path, code, funcs, pool)
}

fn cmd_profile(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| format!("{}: {}", path, e))?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    let (code, funcs, pool) = vm::Compiler::new().compile(ast);
    let mut machine = vm::VM::new();
    machine.silent = true;
    let report = machine.run_profiled(code, funcs, pool, vec![])?;
    println!("{}", report.ascii_report());
    fs::write("profile.folded", report.folded_stacks()).map_err(|e| e.to_string())
}

fn cmd_test(arg: Option<&str>) -> Result<(), String> {
    let targets = match arg {
        Some(p) if std::path::Path::new(p).is_file() => vec![std::path::PathBuf::from(p)],
        Some(p) if std::path::Path::new(p).is_dir() => collect_donk_files(p)?,
        Some(p) => return Err(format!("path not found: {}", p)),
        None => collect_donk_files("tests")?,
    };
    if targets.is_empty() { return Err("no .donk test files found".to_string()); }
    let mut passed = 0usize; let mut failed = 0usize;
    for path in &targets {
        let name = path.to_string_lossy().to_string();
        match run_file(path.to_str().unwrap_or("")) {
            Ok(_) => { println!("  \x1b[32mPASS\x1b[0m  {}", name); passed += 1; }
            Err(e) => { println!("  \x1b[31mFAIL\x1b[0m  {}\n    {}", name, e); failed += 1; }
        }
    }
    println!("\n{} passed, {} failed", passed, failed);
    if failed > 0 { return Err(format!("{} failed", failed)); }
    Ok(())
}

fn collect_donk_files(dir: &str) -> Result<Vec<std::path::PathBuf>, String> {
    let mut files = Vec::new();
    let entries = fs::read_dir(dir).map_err(|e| format!("{}: {}", dir, e))?;
    for entry in entries.flatten() {
        let p = entry.path();
        if p.is_file() && p.extension().and_then(|s| s.to_str()) == Some("donk") { files.push(p); }
    }
    files.sort(); Ok(files)
}

fn run_file(path: &str) -> Result<(), String> {
    let source = fs::read_to_string(path).map_err(|e| e.to_string())?;
    let tokens = lexer::Lexer::new(&source).tokenize()?;
    let ast = parser::Parser::new(tokens).parse()?;
    typecheck::check_program(&ast, path, &source)?;

    let aot = aot::AotCompiler::new();
    let obj_path = "test_app.obj";
    if let Err(e) = aot.compile_to_object(&ast, obj_path) {
        return Err(format!("Compile error: {}", e));
    }

    let exe = if cfg!(target_os = "windows") { "test_app.exe" } else { "test_app" };
    link_executable(obj_path, exe)?;

    // A bare name is not searched in the current directory on Unix.
    let run_path = if cfg!(target_os = "windows") { exe.to_string() } else { format!("./{}", exe) };
    let run_status = std::process::Command::new(&run_path)
        .status()
        .map_err(|e| e.to_string())?;

    if !run_status.success() { return Err("Program exited with error".to_string()); }
    Ok(())
}

fn cmd_bench(arg: Option<&str>) -> Result<(), String> {
    use std::time::Instant;
    let dir = arg.unwrap_or("bench");
    let files = collect_donk_files(dir)?;
    if files.is_empty() { return Err(format!("no .donk files in {}", dir)); }
    const ITERS: usize = 10;
    println!("{:<20} {:>9} {:>11}", "file", "compile", "run(src)");
    println!("{}", "-".repeat(42));
    for path in &files {
        let p = path.to_str().unwrap_or("");
        let name = path.file_name().unwrap_or_default().to_string_lossy().to_string();
        let source = fs::read_to_string(p).map_err(|e| e.to_string())?;
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
            let mut m = vm::VM::new(); m.silent = true;
            let _ = m.run(code, funcs, pool, vec![]);
        }
        let run_ms = t.elapsed().as_secs_f64() * 1000.0 / ITERS as f64;
        println!("{:<20} {:>7.2}ms {:>9.2}ms", name, compile_ms, run_ms);
    }
    Ok(())
}