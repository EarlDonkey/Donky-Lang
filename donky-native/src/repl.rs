// repl.rs — interactive Donky REPL
// Uses the tree-walker (interpreter.rs) so state persists across lines.

use std::io::{self, BufRead, Write};
use crate::lexer::Lexer;
use crate::parser::Parser;
use crate::ast::Stmt;
use crate::interpreter::Interpreter;
use crate::heap::Value;

pub fn run(interp: &mut Interpreter) {
    println!("Donky REPL v1.0 — type .help for commands, .quit to exit");
    let stdin = io::stdin();
    let mut buffer = String::new();

    loop {
        let prompt = if buffer.trim().is_empty() { "donky> " } else { "  ...> " };
        print!("{}", prompt);
        let _ = io::stdout().flush();

        let mut line = String::new();
        match stdin.lock().read_line(&mut line) {
            Ok(0) => { println!(); break; }
            Ok(_) => {}
            Err(e) => { eprintln!("read error: {}", e); break; }
        }

        if buffer.trim().is_empty() {
            let t = line.trim();
            if t.is_empty() { continue; }
            if t == ".quit" || t == ".exit" { break; }
            if t == ".clear" {
                print!("\x1b[2J\x1b[1;1H");
                let _ = io::stdout().flush();
                continue;
            }
            if t == ".help" {
                println!("Commands:");
                println!("  .help        show this help");
                println!("  .clear       clear the screen");
                println!("  .quit        exit (also: .exit, Ctrl+D)");
                continue;
            }
            if t.starts_with('.') {
                eprintln!("unknown command: {}", t);
                continue;
            }
        }

        buffer.push_str(&line);

        if !balanced(&buffer) { continue; }

        let src = std::mem::take(&mut buffer);
        eval_input(interp, &src);
    }
}

/// Returns true when all bracket types are balanced (or over-closed, which
/// we let through to the parser for a real error message).
fn balanced(src: &str) -> bool {
    let mut braces = 0i32;
    let mut parens = 0i32;
    let mut brackets = 0i32;
    let mut in_string = false;
    let mut in_raw = false; // `backtick` raw strings
    let mut in_comment = false;
    let mut chars = src.chars().peekable();
    while let Some(c) = chars.next() {
        if in_comment {
            if c == '\n' { in_comment = false; }
            continue;
        }
        if in_raw {
            if c == '`' { in_raw = false; }
            continue;
        }
        if in_string {
            if c == '\\' { chars.next(); continue; }
            if c == '"' { in_string = false; }
            continue;
        }
        if c == '/' && chars.peek() == Some(&'/') { in_comment = true; chars.next(); continue; }
        match c {
            '"' => in_string = true,
            '`' => in_raw = true,
            '{' => braces += 1,
            '}' => braces -= 1,
            '(' => parens += 1,
            ')' => parens -= 1,
            '[' => brackets += 1,
            ']' => brackets -= 1,
            _ => {}
        }
    }
    braces <= 0 && parens <= 0 && brackets <= 0
}

fn eval_input(interp: &mut Interpreter, src: &str) {
    let trimmed = src.trim();
    if trimmed.is_empty() { return; }

    // Parse first. Only a *parse* failure may be retried with a trailing `;` (that is what makes
    // `1 + 2` work). Retrying after a *runtime* error, as before, ran the input twice and
    // duplicated its side effects (prints, file writes, pushes...).
    let stmts = match parse_src(trimmed) {
        Ok(s) => s,
        Err(first_err) => {
            if !trimmed.ends_with(';') && !trimmed.ends_with('}') {
                match parse_src(&format!("{};", trimmed)) {
                    Ok(s) => s,
                    Err(_) => { eprintln!("Error: {}", first_err); return; }
                }
            } else {
                eprintln!("Error: {}", first_err);
                return;
            }
        }
    };

    match run_stmts(interp, stmts) {
        Ok(Some(v)) => print_value(interp, v),
        Ok(None) => {}
        Err(e) => eprintln!("Error: {}", e),
    }
}

fn print_value(interp: &Interpreter, v: Value) {
    if !v.is_null() {
        println!("{}", v.to_string_with_heap(&interp.heap));
    }
}

fn parse_src(src: &str) -> Result<Vec<Stmt>, String> {
    let tokens = Lexer::new(src).tokenize()?;
    Parser::new(tokens).parse()
}

fn run_stmts(interp: &mut Interpreter, stmts: Vec<Stmt>) -> Result<Option<Value>, String> {
    // A single expression statement is evaluated and its value returned so the REPL can print it.
    if stmts.len() == 1 {
        if let Stmt::ExprStmt(expr) = &stmts[0] {
            return Ok(Some(interp.evaluate(expr.clone())?));
        }
    }
    interp.interpret(stmts)?;
    Ok(None)
}
