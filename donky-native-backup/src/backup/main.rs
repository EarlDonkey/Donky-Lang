mod lexer;
mod parser;
mod ast;
mod interpreter;
mod vm;
mod heap;
mod package;

use std::env;
use std::fs;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        println!("Usage:");
        println!("  donky-native <file.donk>          (Tree-Walker)");
        println!("  donky-native vm <file.donk>       (Bytecode VM)");
        println!("  donky-native install              (Install from donky.json)");
        return;
    }

    if args[1] == "install" {
        if let Err(e) = package::install_from_manifest() { 
            eprintln!("Package Error: {}", e); 
        }
        return;
    }

    let use_vm = args[1] == "vm";
    let path = if use_vm { &args[2] } else { &args[1] };
    let script_args: Vec<String> = if use_vm { args[3..].to_vec() } else { args[2..].to_vec() };

    let source = match fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => { eprintln!("Error reading file: {}", e); return; }
    };
    
    let mut lex = lexer::Lexer::new(&source);
    let tokens = match lex.tokenize() {
        Ok(t) => t,
        Err(e) => { eprintln!("Lex Error: {}", e); return; }
    };

    let mut parser = parser::Parser::new(tokens);
    let ast = match parser.parse() {
        Ok(a) => a,
        Err(e) => { eprintln!("Parse Error: {}", e); return; }
    };

    if use_vm {
        let compiler = vm::Compiler::new();
        let (code, funcs) = compiler.compile(ast);
        let mut machine = vm::VM::new();
        if let Err(e) = machine.run(code, funcs) { eprintln!("Runtime Error: {}", e); }
    } else {
        let mut interp = interpreter::Interpreter::new(script_args);
        if let Err(e) = interp.interpret(ast) { eprintln!("Runtime Error: {}", e); }
    }
}