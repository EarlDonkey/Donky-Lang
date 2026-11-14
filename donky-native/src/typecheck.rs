//! typecheck.rs -- compile-time `raw` pointer checking for Donky.
//!
//! `raw` is a static type only. The C runtime never sees it: every raw value is still a plain
//! uint64_t word holding a number. The checker exists to stop "a number that looks like a pointer"
//! from reaching mem_free / munmap / ffi_poke / world_* and friends.
//!
//! Types: Raw, Number, Str, Bool, Array, Map, Func, Null, Unknown.
//!   * `Raw` is produced only by the RAW_CONSTRUCTORS builtins (and by parameters annotated `p: raw`).
//!   * `Unknown` is anything the checker cannot see through: function parameters (unless annotated),
//!     map/array element reads (`world["ptr"]`), user-function results, most builtin results.
//!     Unknown is ACCEPTED everywhere, so the checker never rejects code it cannot prove wrong.
//!   * An error is reported only when a value of a KNOWN non-raw type reaches a raw slot, or a
//!     known raw value is used in arithmetic / ordering / compared with a known non-raw value.
//!
//! Best-effort by design: types do not flow through maps, arrays, struct fields or function
//! boundaries (except `: raw` / `: number` parameter annotations), so those uses are not checked.

use std::collections::{HashMap, HashSet};

use crate::ast::{Expr, Pos, Stmt};
use crate::lexer::{Lexer, Token, TokenType};

/// The only builtins whose result is `raw`.
pub const RAW_CONSTRUCTORS: &[&str] = &["mem_alloc", "mmap", "agents_new", "trainer_init"];

/// `ptr_add(p: raw, offset: number) -> raw` is the only way to do pointer arithmetic.
/// NOT yet in aot.rs's STDLIB_BUILTINS: waiting on Agent 4's `donky_ptr_add`. Remove from
/// PENDING_BUILTINS (see the test below) once `("ptr_add", "donky_ptr_add", 2)` is registered.
pub const PTR_ADD: &str = "ptr_add";
#[allow(dead_code)]
const PENDING_BUILTINS: &[&str] = &["ptr_add"];

/// KNOWN GAP: `mmap_model(path)` returns a *map* `{addr, size}` (type `Map`, not `raw`). The address
/// inside it is read back with `mm["addr"]` / `mm.get("addr")`, which is `Unknown` to this checker, so
/// `mem_free(mm["addr"], n)` or `ffi_peek(mm["addr"] + 8, ..)` is NOT caught at type-check time.
/// Types do not flow through maps/arrays/struct fields; closing this needs runtime tagging or a
/// typed-field system. Passing the map itself to a raw slot (`mem_free(mm, n)`) IS caught.

/// Builtins that take `raw` pointers, with the 0-based argument positions that must be `raw`.
/// Positions beyond 0 (marked below) are not in the written spec: they were read off the call sites
/// in lib/world.donk and the table comments in aot.rs. `tcp_write_bytes` is an ASSUMED (conn, ptr, len).
pub const RAW_PARAMS: &[(&str, &[usize])] = &[
    // free functions
    ("mem_free", &[0]), ("munmap", &[0]), ("trainer_free", &[0]),
    // world / agents
    ("world_gen", &[0]), ("world_mesh", &[0, 7]), ("world_set", &[0]), ("world_physics", &[0]),
    ("agents_tick", &[0, 2]), ("agents_render", &[0, 2]), ("agents_count_alive", &[0]),
    ("agents_kill", &[0]), ("agents_place_on_surface", &[0, 2]),
    // memory / io
    ("tcp_write_bytes", &[1]),
    ("ffi_peek", &[0]), ("ffi_peek_f32", &[0]), ("ffi_peek_q4_row", &[0]),
    ("ffi_peek_vocab", &[0]), ("ffi_peek_str", &[0]), ("ffi_poke", &[0]),
    ("mem_write", &[0]), ("mem_protect", &[0]),
    // trainer
    ("trainer_load", &[0]), ("trainer_data_ptr", &[0]),
    // pointer arithmetic: arg 0 must be raw; arg 1 is checked to be a number separately in call_result
    ("ptr_add", &[0]),
];

/// Builtins whose result type is known and useful for tracking (everything else is Unknown).
const STR_RETURNS: &[&str] = &["str", "type", "upper", "lower", "substr", "join", "json_encode", "json_pretty"];
const NUM_RETURNS: &[&str] = &["len", "num", "floor", "now", "now_ms", "random", "sin", "cos"];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Ty { Raw, Number, Str, Bool, Array, Map, Func, Null, Unknown }

impl Ty {
    fn name(self) -> &'static str {
        match self {
            Ty::Raw => "raw", Ty::Number => "number", Ty::Str => "string", Ty::Bool => "bool",
            Ty::Array => "array", Ty::Map => "map", Ty::Func => "function", Ty::Null => "null",
            Ty::Unknown => "unknown",
        }
    }
    /// A type we KNOW is not raw (so it must not reach a raw slot).
    fn known_non_raw(self) -> bool { !matches!(self, Ty::Raw | Ty::Unknown) }
}

fn join(a: Ty, b: Ty) -> Ty { if a == b { a } else { Ty::Unknown } }

type Env = HashMap<String, Ty>;

fn join_envs(base: &Env, branches: &[&Env]) -> Env {
    let mut out = Env::new();
    for (k, &v0) in base {
        let mut t = v0;
        for b in branches { t = join(t, b.get(k).copied().unwrap_or(Ty::Unknown)); }
        out.insert(k.clone(), t);
    }
    out
}

struct Diag { line: u32, col: u32, width: usize, headline: String, label: String, notes: Vec<String> }

struct Checker {
    file: String,
    lines: Vec<String>,
    toks: Vec<Token>,
    cursor: usize,
    diags: Vec<Diag>,
    user_fns: HashSet<String>,
}

fn is_constructor_note() -> String {
    "`raw` values are returned by `mem_alloc`, `mmap`, `agents_new`, and `trainer_init`.".to_string()
}

fn param_ty(p: &str) -> (String, Ty) {
    match p.split_once(':') {
        Some((n, t)) => (n.trim().to_string(), match t.trim() { "raw" => Ty::Raw, "number" => Ty::Number, _ => Ty::Unknown }),
        None => (p.trim().to_string(), Ty::Unknown),
    }
}

fn collect_fns(s: &Stmt, out: &mut HashSet<String>) {
    match s {
        Stmt::Function(n, _, b) => { out.insert(n.clone()); collect_fns(b, out); }
        Stmt::Block(ss) => for x in ss { collect_fns(x, out); },
        Stmt::If(_, a, b) => { collect_fns(a, out); if let Some(b) = b { collect_fns(b, out); } }
        Stmt::While(_, b) | Stmt::For(_, _, b) | Stmt::ForRange(_, _, _, b) => collect_fns(b, out),
        Stmt::Try(a, _, b) => { collect_fns(a, out); collect_fns(b, out); }
        Stmt::Match(_, cs, e) => { for (_, b) in cs { collect_fns(b, out); } if let Some(e) = e { collect_fns(e, out); } }
        _ => {}
    }
}

fn op_tokens(op: &TokenType) -> Vec<TokenType> {
    use TokenType::*;
    match op {
        Plus => vec![Plus, PlusEq, PlusPlus], Minus => vec![Minus, MinusEq, MinusMinus],
        Star => vec![Star, StarEq], Slash => vec![Slash, SlashEq], Percent => vec![Percent, PercentEq],
        other => vec![other.clone()],
    }
}

fn op_text(op: &TokenType) -> &'static str {
    use TokenType::*;
    match op {
        Plus => "+", Minus => "-", Star => "*", Slash => "/", Percent => "%", Div => "div",
        Lt => "<", LtEq => "<=", Gt => ">", GtEq => ">=", EqEq => "==", NotEq => "!=", _ => "?",
    }
}

fn operand_name(e: &Expr) -> Option<String> {
    match e { Expr::Ident(n) => Some(n.clone()), Expr::Call(n, _, _) => Some(format!("{}(...)", n)), _ => None }
}

impl Checker {
    // ----------------------------------------------------------------- locating source positions
    fn tok_index_at(&self, p: Pos) -> Option<usize> {
        self.toks.iter().position(|t| t.line as u32 == p.line && t.col as u32 == p.col)
    }

    /// Next operator token (one of `ops`) at or after `from` whose left or right neighbour is the
    /// identifier `name` (when given). Falls back to a scan from the start of the file.
    fn locate_op(&mut self, from: usize, ops: &[TokenType], name: Option<&str>) -> Option<(u32, u32, usize)> {
        let n = self.toks.len();
        let ok = |this: &Checker, i: usize| -> bool {
            if !ops.contains(&this.toks[i].ttype) { return false; }
            match name {
                None => true,
                Some(nm) => {
                    let base = nm.trim_end_matches("(...)");
                    (i > 0 && this.toks[i - 1].lexeme == base) || (i + 1 < n && this.toks[i + 1].lexeme == base)
                }
            }
        };
        let found = (from..n).find(|&i| ok(self, i)).or_else(|| (0..from.min(n)).find(|&i| ok(self, i)));
        found.map(|i| {
            self.cursor = i + 1;
            let t = &self.toks[i];
            (t.line as u32, t.col as u32, t.lexeme.chars().count().max(1))
        })
    }

    /// `name = ...` (plain assignment) at or after `from`.
    fn locate_assign(&mut self, from: usize, name: &str) -> Option<(u32, u32, usize)> {
        let n = self.toks.len();
        let ok = |this: &Checker, i: usize| this.toks[i].ttype == TokenType::Ident && this.toks[i].lexeme == name
            && i + 1 < n && this.toks[i + 1].ttype == TokenType::Eq;
        let found = (from..n).find(|&i| ok(self, i)).or_else(|| (0..from.min(n)).find(|&i| ok(self, i)));
        found.map(|i| { self.cursor = i + 1; let t = &self.toks[i]; (t.line as u32, t.col as u32, name.chars().count().max(1)) })
    }

    /// Span of argument `idx` of the call whose callee token is at `pos`.
    fn arg_span(&mut self, pos: Pos, callee: &str, idx: usize) -> Option<(u32, u32, usize, String)> {
        let i = self.tok_index_at(pos)?;
        if self.toks[i].lexeme != callee || self.toks.get(i + 1)?.ttype != TokenType::LParen { return None; }
        self.cursor = i + 1;
        let (mut depth, mut arg) = (0usize, 0usize);
        let mut first: Option<usize> = None;
        let mut last: Option<usize> = None;
        let mut j = i + 2;
        while j < self.toks.len() {
            let t = &self.toks[j];
            match t.ttype {
                TokenType::LParen | TokenType::LBracket | TokenType::LBrace => depth += 1,
                TokenType::RParen | TokenType::RBracket | TokenType::RBrace => {
                    if depth == 0 { break; }
                    depth -= 1;
                }
                TokenType::Comma if depth == 0 => {
                    if arg == idx { break; }
                    arg += 1; j += 1; continue;
                }
                TokenType::Eof => return None,
                _ => {}
            }
            if arg == idx { if first.is_none() { first = Some(j); } last = Some(j); }
            j += 1;
        }
        let (f, l) = (first?, last?);
        let (ft, lt) = (&self.toks[f], &self.toks[l]);
        let line_txt: Vec<char> = self.lines.get(ft.line - 1)?.chars().collect();
        let start = ft.col - 1;
        let end = if lt.line == ft.line { (lt.col - 1 + lt.lexeme.chars().count()).min(line_txt.len()) } else { line_txt.len() };
        if end <= start { return None; }
        let text: String = line_txt[start..end].iter().collect();
        Some((ft.line as u32, ft.col as u32, end - start, text))
    }

    fn push(&mut self, loc: Option<(u32, u32, usize)>, headline: String, label: String, notes: Vec<String>) {
        let (line, col, width) = loc.unwrap_or((0, 0, 1));
        if self.diags.iter().any(|d| d.line == line && d.col == col && d.headline == headline) { return; }
        self.diags.push(Diag { line, col, width, headline, label, notes });
    }

    // --------------------------------------------------------------------------- expression walk
    fn expr(&mut self, e: &Expr, env: &mut Env) -> Ty {
        match e {
            Expr::Number(_) => Ty::Number,
            Expr::String(_) => Ty::Str,
            Expr::Bool(_) => Ty::Bool,
            Expr::InterpolatedString(parts) => { for p in parts { self.expr(p, env); } Ty::Str }
            Expr::Ident(n) => match n.as_str() {
                "null" => Ty::Null,
                "args" => Ty::Array,
                _ => env.get(n).copied().unwrap_or(Ty::Unknown),
            },
            Expr::Array(es) => { for x in es { self.expr(x, env); } Ty::Array }
            Expr::Map(ps) => { for (k, v) in ps { self.expr(k, env); self.expr(v, env); } Ty::Map }
            Expr::StructInit(_, fs) => { for (_, v) in fs { self.expr(v, env); } Ty::Map }
            // Indexing / field reads: types do not flow through containers. Never an error, even on a raw
            // (see the mmap_model KNOWN GAP above).
            Expr::Index(o, i) => { self.expr(o, env); self.expr(i, env); Ty::Unknown }
            Expr::FieldAccess(o, _) => { self.expr(o, env); Ty::Unknown }
            Expr::Function(params, body) => {
                let mut fenv = Env::new();
                for p in params { let (n, t) = param_ty(p); fenv.insert(n, t); }
                self.stmt(body, &mut fenv);
                Ty::Func
            }
            Expr::Ternary(c, a, b) => {
                self.expr(c, env);
                let (ta, tb) = (self.expr(a, env), self.expr(b, env));
                join(ta, tb)
            }
            Expr::Unary(op, x) => {
                let c0 = self.cursor;
                let t = self.expr(x, env);
                match op {
                    TokenType::Minus => {
                        if t == Ty::Raw {
                            let loc = self.locate_op(c0, &[TokenType::Minus], operand_name(x).as_deref());
                            self.push(loc, "cannot negate a `raw` pointer".into(),
                                format!("`{}` is a `raw` pointer; unary `-` is not defined on it", operand_name(x).unwrap_or_else(|| "this value".into())),
                                vec![RAW_USES.to_string()]);
                        }
                        Ty::Number
                    }
                    _ => Ty::Bool,
                }
            }
            Expr::Binary(l, op, r) => {
                let c0 = self.cursor;
                let (lt, rt) = (self.expr(l, env), self.expr(r, env));
                use TokenType::*;
                match op {
                    Plus | Minus | Star | Slash | Percent | Div | Lt | LtEq | Gt | GtEq => {
                        if lt == Ty::Raw || rt == Ty::Raw {
                            let (who, nm) = if lt == Ty::Raw { (l, operand_name(l)) } else { (r, operand_name(r)) };
                            let loc = self.locate_op(c0, &op_tokens(op), nm.as_deref());
                            let _ = who;
                            let arith = matches!(op, Plus | Minus | Star | Slash | Percent | Div);
                            self.push(loc,
                                format!("cannot apply `{}` to a `raw` pointer", op_text(op)),
                                format!("`{}` is a `raw` pointer; {} is not defined on pointers",
                                    nm.unwrap_or_else(|| "this value".into()),
                                    if arith { "arithmetic" } else { "ordering" }),
                                vec![RAW_USES.to_string()]);
                        }
                        match op {
                            Lt | LtEq | Gt | GtEq => Ty::Bool,
                            Plus => if lt == Ty::Number && rt == Ty::Number { Ty::Number }
                                    else if lt == Ty::Str && rt == Ty::Str { Ty::Str } else { Ty::Unknown },
                            _ => if lt == Ty::Raw || rt == Ty::Raw { Ty::Unknown } else { Ty::Number },
                        }
                    }
                    EqEq | NotEq => {
                        let bad = (lt == Ty::Raw && rt.known_non_raw()) || (rt == Ty::Raw && lt.known_non_raw());
                        if bad {
                            let (rawside, other) = if lt == Ty::Raw { (l, rt) } else { (r, lt) };
                            let nm = operand_name(rawside);
                            let loc = self.locate_op(c0, &[op.clone()], nm.as_deref());
                            let mut notes = vec![
                                "a `raw` pointer can only be compared with another `raw` value.".to_string(),
                                "to test for a failed allocation use `is_null_handle(p)`.".to_string(),
                            ];
                            if other == Ty::Null {
                                notes.insert(0, "a failed `mem_alloc` returns the number 0, never `null`, so this comparison could never be true.".to_string());
                            }
                            self.push(loc, format!("cannot compare `raw` with `{}`", other.name()),
                                format!("`{}` is a `raw` pointer; the other operand is a `{}`",
                                    nm.unwrap_or_else(|| "this value".into()), other.name()), notes);
                        }
                        Ty::Bool
                    }
                    _ => Ty::Bool, // && ||
                }
            }
            Expr::MethodCall(obj, m, args, pos) => {
                let mut tys = vec![self.expr(obj, env)];
                for a in args { tys.push(self.expr(a, env)); }
                self.call_result(m, &tys, Some(*pos), false)
            }
            Expr::Call(name, args, pos) => {
                let tys: Vec<Ty> = args.iter().map(|a| self.expr(a, env)).collect();
                self.call_result(name, &tys, Some(*pos), true)
            }
        }
    }

    fn call_result(&mut self, name: &str, tys: &[Ty], pos: Option<Pos>, plain_call: bool) -> Ty {
        if self.user_fns.contains(name) { return Ty::Unknown; } // a Donky fn shadows the builtin
        if let Some((_, positions)) = RAW_PARAMS.iter().find(|(n, _)| *n == name) {
            for &p in positions.iter() {
                let Some(&t) = tys.get(p) else { continue };
                if !t.known_non_raw() { continue; }
                let span = if plain_call { pos.and_then(|ps| self.arg_span(ps, name, p)) } else { None };
                let (loc, text) = match span {
                    Some((l, c, w, txt)) => (Some((l, c, w)), txt),
                    None => (pos.map(|ps| (ps.line, ps.col, name.chars().count().max(1))), "this value".to_string()),
                };
                let text = if text.chars().count() > 40 { format!("{}...", text.chars().take(37).collect::<String>()) } else { text };
                let nth = if p > 0 { format!(" (argument {})", p + 1) } else { String::new() };
                let from = if t == Ty::Number { "an arbitrary `number`".to_string() } else { format!("a `{}` value", t.name()) };
                self.push(loc,
                    format!("expected `raw`, got `{}`", t.name()),
                    format!("`{}` is a `{}`; `{}` requires a `raw` pointer{}", text, t.name(), name, nth),
                    vec![format!("{} You cannot construct a `raw` from {}.", is_constructor_note(), from)]);
            }
        }
        if name == PTR_ADD {
            if let Some(&t) = tys.get(1) {
                if t.known_non_raw() && t != Ty::Number || t == Ty::Raw {
                    let span = if plain_call { pos.and_then(|ps| self.arg_span(ps, name, 1)) } else { None };
                    let (loc, text) = match span {
                        Some((l, c, w, txt)) => (Some((l, c, w)), txt),
                        None => (pos.map(|ps| (ps.line, ps.col, name.chars().count().max(1))), "this value".to_string()),
                    };
                    self.push(loc, format!("expected `number`, got `{}`", t.name()),
                        format!("`{}` is a `{}`; the offset of `ptr_add` (argument 2) must be a `number`", text, t.name()),
                        vec!["`ptr_add(p, offset)` takes a `raw` pointer and a byte offset, and returns a `raw`.".to_string()]);
                }
            }
            return Ty::Raw;
        }
        if name == "mmap_model" { return Ty::Map; }
        if RAW_CONSTRUCTORS.contains(&name) { return Ty::Raw; }
        if STR_RETURNS.contains(&name) { return Ty::Str; }
        if NUM_RETURNS.contains(&name) { return Ty::Number; }
        Ty::Unknown
    }

    // ---------------------------------------------------------------------------- statement walk
    fn stmts(&mut self, ss: &[Stmt], env: &mut Env) { for s in ss { self.stmt(s, env); } }

    fn stmt(&mut self, s: &Stmt, env: &mut Env) {
        match s {
            Stmt::Let(n, e) => { let t = self.expr(e, env); env.insert(n.clone(), t); }
            Stmt::Assign(n, e) => {
                let c0 = self.cursor;
                let t = self.expr(e, env);
                let old = env.get(n).copied();
                let conflict = match old {
                    Some(Ty::Raw) => t.known_non_raw() && t != Ty::Null,
                    Some(Ty::Number) => t == Ty::Raw,
                    _ => false,
                };
                if conflict {
                    let o = old.unwrap();
                    let loc = self.locate_assign(c0, n);
                    let mut notes = vec![];
                    if o == Ty::Raw || t == Ty::Raw { notes.push(format!("{} You cannot construct a `raw` from an arbitrary `number`.", is_constructor_note())); }
                    self.push(loc, format!("expected `{}`, got `{}`", o.name(), t.name()),
                        format!("`{}` is a `{}`; a `{}` cannot be assigned to it", n, o.name(), t.name()), notes);
                    env.insert(n.clone(), Ty::Unknown);
                } else {
                    let new = match old { Some(o) => join(o, t), None => Ty::Unknown };
                    env.insert(n.clone(), new);
                }
            }
            Stmt::IndexSet(a, b, c) => { self.expr(a, env); self.expr(b, env); self.expr(c, env); }
            Stmt::FieldSet(o, _, v) => { self.expr(o, env); self.expr(v, env); }
            Stmt::Print(e) | Stmt::ExprStmt(e) | Stmt::Throw(e) => { self.expr(e, env); }
            Stmt::Return(Some(e)) => { self.expr(e, env); }
            Stmt::Block(ss) => self.stmts(ss, env),
            Stmt::If(c, t, e) => {
                self.expr(c, env);
                let mut e1 = env.clone(); self.stmt(t, &mut e1);
                let mut e2 = env.clone(); if let Some(e) = e { self.stmt(e, &mut e2); }
                *env = join_envs(env, &[&e1, &e2]);
            }
            Stmt::While(c, b) => {
                self.expr(c, env);
                let mut e1 = env.clone(); self.stmt(b, &mut e1);
                *env = join_envs(env, &[&e1]);
            }
            Stmt::For(n, it, b) => {
                self.expr(it, env);
                let mut e1 = env.clone(); e1.insert(n.clone(), Ty::Unknown); self.stmt(b, &mut e1);
                *env = join_envs(env, &[&e1]);
            }
            Stmt::ForRange(n, a, z, b) => {
                self.expr(a, env); self.expr(z, env);
                let mut e1 = env.clone(); e1.insert(n.clone(), Ty::Number); self.stmt(b, &mut e1);
                *env = join_envs(env, &[&e1]);
            }
            Stmt::Match(t, cases, els) => {
                self.expr(t, env);
                let mut branches = Vec::new();
                for (v, b) in cases { self.expr(v, env); let mut e1 = env.clone(); self.stmt(b, &mut e1); branches.push(e1); }
                if let Some(b) = els { let mut e1 = env.clone(); self.stmt(b, &mut e1); branches.push(e1); }
                let refs: Vec<&Env> = branches.iter().collect();
                *env = join_envs(env, &refs);
            }
            Stmt::Try(b, n, c) => {
                let mut e1 = env.clone(); self.stmt(b, &mut e1);
                let mut e2 = env.clone(); e2.insert(n.clone(), Ty::Unknown); self.stmt(c, &mut e2);
                *env = join_envs(env, &[&e1, &e2]);
            }
            Stmt::Function(_, params, body) => {
                let mut fenv = Env::new(); // functions cannot see top-level lets
                for p in params { let (n, t) = param_ty(p); fenv.insert(n, t); }
                self.stmt(body, &mut fenv);
            }
            Stmt::Return(None) | Stmt::Break(_) | Stmt::Continue(_) | Stmt::StructDef(..) | Stmt::Import(_) | Stmt::Asm(_) => {}
        }
    }

    // ------------------------------------------------------------------------------- rendering
    fn render(&self) -> String {
        let mut out = String::new();
        for d in &self.diags {
            out.push_str(&format!("error: {}\n", d.headline));
            if d.line == 0 {
                out.push_str(&format!("  --> {}\n   = {}\n", self.file, d.label));
            } else {
                let w = d.line.to_string().len();
                let pad = " ".repeat(w);
                let src = self.lines.get(d.line as usize - 1).map(|s| s.trim_end_matches('\r')).unwrap_or("");
                out.push_str(&format!("{}--> {}:{}:{}\n", pad, self.file, d.line, d.col));
                out.push_str(&format!("{} |\n", pad));
                out.push_str(&format!("{} | {}\n", d.line, src));
                out.push_str(&format!("{} | {}{} {}\n", pad, " ".repeat(d.col as usize - 1), "^".repeat(d.width.max(1)), d.label));
                out.push_str(&format!("{} |\n", pad));
                for n in &d.notes { out.push_str(&format!("{} = note: {}\n", pad, n)); }
            }
            out.push('\n');
        }
        out
    }
}

const RAW_USES: &str = "a `raw` value can only be passed to a raw-typed builtin, freed, compared with another `raw`, or passed to `str()`.";

/// Check one parsed file. `file` is only used in messages; `source` must be the exact text `stmts`
/// were parsed from (positions are matched against a fresh lex of it).
pub fn check_program(stmts: &[Stmt], file: &str, source: &str) -> Result<(), String> {
    let toks = Lexer::new(source).tokenize().map_err(|e| format!("typecheck: {}", e))?;
    let mut user_fns = HashSet::new();
    for s in stmts { collect_fns(s, &mut user_fns); }
    let lines = source.trim_start_matches('\u{feff}').split('\n').map(|s| s.to_string()).collect();
    let mut c = Checker { file: file.to_string(), lines, toks, cursor: 0, diags: Vec::new(), user_fns };
    let mut env = Env::new();
    c.stmts(stmts, &mut env);
    if c.diags.is_empty() { return Ok(()); }
    let n = c.diags.len();
    Err(format!("{}error: aborting due to {} previous type error{}\n", c.render(), n, if n == 1 { "" } else { "s" }))
}

#[cfg(test)]
mod tests {
    use super::*;
    /// Every name in the tables must be a real builtin, or the checker silently does nothing for it.
    #[test]
    fn tables_name_real_builtins() {
        let have: HashSet<&str> = crate::aot::STDLIB_BUILTINS.iter().map(|(n, _, _)| *n)
            .chain(PENDING_BUILTINS.iter().copied()).collect();
        for n in RAW_CONSTRUCTORS { assert!(have.contains(n), "unknown builtin in RAW_CONSTRUCTORS: {}", n); }
        for (n, ps) in RAW_PARAMS {
            let arity = crate::aot::STDLIB_BUILTINS.iter().find(|(m, _, _)| m == n).map(|(_, _, a)| *a)
                .or(if PENDING_BUILTINS.contains(n) { Some(2) } else { None });
            assert!(arity.is_some(), "unknown builtin in RAW_PARAMS: {}", n);
            for p in ps.iter() { assert!(*p < arity.unwrap(), "{}: raw position {} >= arity", n, p); }
        }
    }
}
