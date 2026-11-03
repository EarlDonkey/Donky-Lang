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
    if args.len() < 2 { print_usage(); return; }

    match args[1].as_str() {
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
        "build-native" => {
            if args.len() < 3 { eprintln!("Usage: donky-native build-native <file.donk>"); return; }
            let path = &args[2];
            let source = match fs::read_to_string(path) { Ok(s) => s, Err(e) => { eprintln!("Runtime Error: build-native: {}", e); return; } };
            let tokens = match lexer::Lexer::new(&source).tokenize() { Ok(t) => t, Err(e) => { eprintln!("Lex Error: {}", e); return; } };
            let ast = match parser::Parser::new(tokens).parse() { Ok(a) => a, Err(e) => { eprintln!("Parse Error: {}", e); return; } };
            let mut compiler = codegen::NativeCompiler::new();
            println!("Compiling natively...");
            let start = std::time::Instant::now();
            compiler.compile_and_run(&ast);
            println!("Native execution finished in {:?}", start.elapsed());
        }
        "build-exe" => {
            if args.len() < 3 { eprintln!("Usage: donky-native build-exe <file.donk>"); return; }
            let path = &args[2];
            let source = match fs::read_to_string(path) { Ok(s) => s, Err(e) => { eprintln!("Runtime Error: build-exe: {}", e); return; } };
            let tokens = match lexer::Lexer::new(&source).tokenize() { Ok(t) => t, Err(e) => { eprintln!("Lex Error: {}", e); return; } };
            let ast = match parser::Parser::new(tokens).parse() { Ok(a) => a, Err(e) => { eprintln!("Parse Error: {}", e); return; } };

            println!("Compiling to object file...");
            let aot = aot::AotCompiler::new();
            let obj_path = "donky_app.obj";
            aot.compile_to_object(&ast, obj_path).unwrap();

            #[cfg(target_os = "windows")]
            let (gcc_bin, gcc_args): (String, Vec<String>) = {
                let rt = match find_runtime_obj() { Ok(p) => p, Err(e) => { eprintln!("{}", e); return; } };
                let mut a: Vec<String> = vec![
                    obj_path.to_string(),
                    rt.to_string_lossy().into_owned(),
                    "-o".to_string(), "donky_app.exe".to_string(),
                    "-static".to_string(), "-static-libgcc".to_string(),
                ];
                for l in ["-lws2_32", "-lpsapi", "-lm", "-lwinhttp", "-lgdi32", "-Wl,--stack,67108864"] { a.push(l.to_string()); }
                (find_gcc(), a)
            };

            #[cfg(not(target_os = "windows"))]
            let (gcc_bin, gcc_args): (String, Vec<String>) = {
                println!("Writing C runtime...");
                fs::write("runtime.c", include_str!("runtime_template.c")).unwrap();
                #[allow(unused_mut)]
                let mut a: Vec<String> = vec![
                    obj_path.to_string(), "runtime.c".to_string(),
                    "-o".to_string(), "donky_app".to_string(),
                ];
                #[cfg(target_os = "linux")]
                { for l in ["-lpthread", "-lcurl", "-lm", "-ldl"] { a.push(l.to_string()); } }
                #[cfg(target_os = "macos")]
                { a.push("-lcurl".to_string()); }
                ("gcc".to_string(), a)
            };

            println!("Linking with GCC...");
            let status = std::process::Command::new(&gcc_bin)
                .args(&gcc_args)
                .status()
                .expect("Failed to run gcc. Make sure GCC/MinGW is in your PATH.");

            if status.success() { println!("Successfully built donky_app!"); }
            else { eprintln!("Linker failed."); }
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
    println!("Usage:");
    println!("  donky-native <file.donk>                 tree-walker");
    println!("  donky-native run <file.donk|file.bc>     VM");
    println!("  donky-native build <file.donk>           compile to .bc");
    println!("  donky-native build-native <file.donk>    JIT");
    println!("  donky-native build-exe <file.donk>       standalone .exe");
    println!("  donky-native test [dir|file]             run tests");
    println!("  donky-native bench [dir]                 benchmark");
    println!("  donky-native profile <file.donk>         profile");
    println!("  donky-native repl                        REPL");
    println!("  donky-native install                     install packages");
    println!("  donky-native add <name> <url|path>       add dependency");
}

#[cfg(target_os = "windows")]
fn install_root() -> Option<std::path::PathBuf> {
    // <root>\bin\donky.exe -> <root>
    std::env::current_exe().ok()?.parent()?.parent().map(|p| p.to_path_buf())
}

#[cfg(target_os = "windows")]
fn find_runtime_obj() -> Result<std::path::PathBuf, String> {
    let mut c: Vec<std::path::PathBuf> = Vec::new();
    if let Some(root) = install_root() { c.push(root.join("lib").join("runtime.o")); }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(d) = exe.parent() { c.push(d.join("runtime.o")); }
    }
    c.push("runtime.o".into());
    c.push("dist/lib/runtime.o".into());
    for p in c { if p.exists() { return Ok(p); } }
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
    let (code, funcs, pool) = vm::Compiler::new().compile(ast);
    vm::VM::new().run(code, funcs, pool, vec![])
}

fn cmd_bench(arg: Option<&str>) -> Result<(), String> {
    use std::time::Instant;
    let dir = arg.unwrap_or("bench");
    let files = collect_donk_files(dir)?;
    if files.is_empty() { return Err(format!("no .donk files in {}", dir)); }
    const ITERS: usize = 10;
    println!("{:<20} {:>9} {:>11} {:>13} {:>10} {:>12}", "file", "compile", "run(src)", "load+run(.bc)", "ns/op", "ns/call");
    println!("{}", "-".repeat(82));
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