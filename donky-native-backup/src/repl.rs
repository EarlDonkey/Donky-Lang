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
    let mut in_comment = false;
    let mut chars = src.chars().peekable();
    while let Some(c) = chars.next() {
        if in_comment {
            if c == '\n' { in_comment = false; }
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

    match try_eval(interp, trimmed) {
        Ok(Some(v)) => print_value(interp, v),
        Ok(None) => {}
        Err(first_err) => {
            // If the source doesn't end with a terminator, retry with `;`.
            // This is what makes `1 + 2` work without a trailing semicolon.
            if !trimmed.ends_with(';') && !trimmed.ends_with('}') {
                let with_semi = format!("{};", trimmed);
                match try_eval(interp, &with_semi) {
                    Ok(Some(v)) => { print_value(interp, v); return; }
                    Ok(None) => return,
                    Err(_) => {}
                }
            }
            eprintln!("Error: {}", first_err);
        }
    }
}

fn print_value(interp: &Interpreter, v: Value) {
    if !v.is_null() {
        println!("{}", v.to_string_with_heap(&interp.heap));
    }
}

fn try_eval(interp: &mut Interpreter, src: &str) -> Result<Option<Value>, String> {
    let mut lex = Lexer::new(src);
    let tokens = lex.tokenize()?;
    let mut parser = Parser::new(tokens);
    let stmts = parser.parse()?;

    // If the whole input is a single expression statement, evaluate and
    // return the value so the REPL can print it.
    if stmts.len() == 1 {
        if let Stmt::ExprStmt(expr) = &stmts[0] {
            return Ok(Some(interp.evaluate(expr.clone())?));
        }
    }

    interp.interpret(stmts)?;
    Ok(None)
}