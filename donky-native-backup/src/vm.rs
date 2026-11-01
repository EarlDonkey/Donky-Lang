use crate::ast::{Expr, Stmt};
use crate::lexer::{TokenType, Lexer};
use crate::parser::Parser;
use crate::heap::{Value, ValueKind, MapKey, Heap, HeapObject};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::Path;
use std::rc::Rc;
use std::cell::RefCell;
use std::sync::{Mutex, OnceLock};
use std::net::{TcpListener, TcpStream};
use std::io::{Read, Write};

static CLIENTS: OnceLock<Mutex<HashMap<usize, TcpStream>>> = OnceLock::new();
static SERVERS: OnceLock<Mutex<HashMap<usize, TcpListener>>> = OnceLock::new();
static COUNTER: OnceLock<Mutex<usize>> = OnceLock::new();

fn get_clients() -> &'static Mutex<HashMap<usize, TcpStream>> { CLIENTS.get_or_init(|| Mutex::new(HashMap::new())) }
fn get_servers() -> &'static Mutex<HashMap<usize, TcpListener>> { SERVERS.get_or_init(|| Mutex::new(HashMap::new())) }
fn get_counter() -> &'static Mutex<usize> { COUNTER.get_or_init(|| Mutex::new(1)) }

#[repr(C, u8)]
#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub enum OpCode {
    ConstantNumber(f64),
    ConstantBool(bool),
    ConstantNull,
    StringConst(u32),
    Add, Sub, Mul, Div, Mod, Eq, NotEq, Lt, Gt, LtEq, GtEq,
    Negate, Not,
    Print, Jump(usize), JumpIfFalse(usize), JumpIfTrue(usize), GetLocal(usize), SetLocal(usize),
    DefineFunction(String, usize, usize),
    DefineFunctionGlobal(String, usize, usize, u32),
    GetGlobal(u32),
    SetGlobal(u32),
    Call(usize), CallGlobal(u32, usize),
    Builtin(String, usize), MethodCall(String, usize),
    PushLocalAddConst(u32, f64),
    PushLocalSubConst(u32, f64),
    Return, Array(usize), Map(usize), Struct(String, Vec<String>),
    FieldAccess(String), FieldSet(String),
    Index, IndexSet,
    Interpolate(usize),
    TryStart(usize), TryEnd, Throw,
    Dup, Pop, Halt,
    BinLocals(TokenType, u32, u32),
    IncLocal(u32, f64),
    AddLocalConst(u32, f64, u32),
    JumpIfLocalCmpFalse(TokenType, u32, u32, usize),
    AddLocalMulLocal(u32, u32, u32),
    IncLocalJump(u32, f64, usize),
    JumpIfLocalConstCmpFalse(TokenType, u32, f64, usize),
    MakeClosure(usize, Vec<usize>, usize, usize),
}

// ===================== PEEPHOLE =====================

fn bin_op_of(op: &OpCode) -> Option<TokenType> {
    match op {
        OpCode::Add => Some(TokenType::Plus), OpCode::Sub => Some(TokenType::Minus),
        OpCode::Mul => Some(TokenType::Star), OpCode::Div => Some(TokenType::Slash),
        OpCode::Mod => Some(TokenType::Percent),
        OpCode::Eq => Some(TokenType::EqEq), OpCode::NotEq => Some(TokenType::NotEq),
        OpCode::Lt => Some(TokenType::Lt), OpCode::Gt => Some(TokenType::Gt),
        OpCode::LtEq => Some(TokenType::LtEq), OpCode::GtEq => Some(TokenType::GtEq),
        _ => None,
    }
}

fn cmp_op_of(op: &OpCode) -> Option<TokenType> {
    match op {
        OpCode::Lt => Some(TokenType::Lt), OpCode::Gt => Some(TokenType::Gt),
        OpCode::LtEq => Some(TokenType::LtEq), OpCode::GtEq => Some(TokenType::GtEq),
        OpCode::Eq => Some(TokenType::EqEq), OpCode::NotEq => Some(TokenType::NotEq),
        _ => None,
    }
}

fn is_comparison(op: &TokenType) -> bool {
    matches!(op, TokenType::EqEq | TokenType::NotEq | TokenType::Lt | TokenType::Gt | TokenType::LtEq | TokenType::GtEq)
}

fn peephole(code: Vec<OpCode>) -> Vec<OpCode> {
    let mut targets: HashSet<usize> = HashSet::new();
    for op in &code {
        match op {
            OpCode::Jump(t) | OpCode::JumpIfFalse(t) | OpCode::JumpIfTrue(t) | OpCode::TryStart(t)
            | OpCode::JumpIfLocalCmpFalse(_, _, _, t) | OpCode::IncLocalJump(_, _, t)
            | OpCode::JumpIfLocalConstCmpFalse(_, _, _, t) => { targets.insert(*t); }
            _ => {}
        }
    }
    let mut new_code: Vec<OpCode> = Vec::with_capacity(code.len());
    let mut old_to_new: Vec<usize> = vec![0; code.len() + 1];
    let mut i = 0;
    while i < code.len() {
        old_to_new[i] = new_code.len();
        let clean = |i: usize, n: usize| -> bool { !(1..n).any(|j| i + j < code.len() && targets.contains(&(i + j))) };
        if i + 2 < code.len() && clean(i, 3) {
            if let (OpCode::GetLocal(a), OpCode::GetLocal(b)) = (&code[i], &code[i + 1]) {
                if let Some(tok) = bin_op_of(&code[i + 2]) {
                    new_code.push(OpCode::BinLocals(tok, *a as u32, *b as u32)); i += 3; continue;
                }
            }
        }
        if i + 1 < code.len() && clean(i, 2) {
            if let OpCode::BinLocals(op, a, b) = &code[i] {
                if is_comparison(op) {
                    if let OpCode::JumpIfFalse(t) = &code[i + 1] {
                        new_code.push(OpCode::JumpIfLocalCmpFalse(op.clone(), *a, *b, *t)); i += 2; continue;
                    }
                }
            }
        }
        if i + 3 < code.len() && clean(i, 4) {
            if let (OpCode::GetLocal(dst), OpCode::BinLocals(TokenType::Star, a, b), OpCode::Add, OpCode::SetLocal(dst2)) =
                (&code[i], &code[i + 1], &code[i + 2], &code[i + 3]) {
                if dst == dst2 { new_code.push(OpCode::AddLocalMulLocal(*dst as u32, *a as u32, *b as u32)); i += 4; continue; }
            }
        }
        if i + 1 < code.len() && clean(i, 2) {
            if let (OpCode::IncLocal(slot, delta), OpCode::Jump(t)) = (&code[i], &code[i + 1]) {
                new_code.push(OpCode::IncLocalJump(*slot, *delta, *t)); i += 2; continue;
            }
        }
        if i + 3 < code.len() && clean(i, 4) {
            if let (OpCode::GetLocal(a), OpCode::ConstantNumber(k), OpCode::SetLocal(b)) = (&code[i], &code[i + 1], &code[i + 3]) {
                if a == b {
                    match &code[i + 2] {
                        OpCode::Add => { new_code.push(OpCode::IncLocal(*a as u32, *k)); i += 4; continue; }
                        OpCode::Sub => { new_code.push(OpCode::IncLocal(*a as u32, -*k)); i += 4; continue; }
                        _ => {}
                    }
                }
            }
        }
        if i + 3 < code.len() && clean(i, 4) {
            if let (OpCode::GetLocal(a), OpCode::ConstantNumber(k), OpCode::SetLocal(b)) = (&code[i], &code[i + 1], &code[i + 3]) {
                if a != b {
                    match &code[i + 2] {
                        OpCode::Add => { new_code.push(OpCode::AddLocalConst(*a as u32, *k, *b as u32)); i += 4; continue; }
                        OpCode::Sub => { new_code.push(OpCode::AddLocalConst(*a as u32, -*k, *b as u32)); i += 4; continue; }
                        _ => {}
                    }
                }
            }
        }
        if i + 3 < code.len() && clean(i, 4) {
            if let (OpCode::GetLocal(a), OpCode::ConstantNumber(k)) = (&code[i], &code[i + 1]) {
                if let Some(cmp) = cmp_op_of(&code[i + 2]) {
                    if let OpCode::JumpIfFalse(t) = &code[i + 3] {
                        new_code.push(OpCode::JumpIfLocalConstCmpFalse(cmp, *a as u32, *k, *t)); i += 4; continue;
                    }
                }
            }
        }
        // NEW RULE: GetLocal(a), ConstantNumber(k), Add/Sub  →  PushLocalAddConst / PushLocalSubConst
        if i + 2 < code.len() && clean(i, 3) {
            if let (OpCode::GetLocal(a), OpCode::ConstantNumber(k)) = (&code[i], &code[i + 1]) {
                match &code[i + 2] {
                    OpCode::Add => { new_code.push(OpCode::PushLocalAddConst(*a as u32, *k)); i += 3; continue; }
                    OpCode::Sub => { new_code.push(OpCode::PushLocalSubConst(*a as u32, *k)); i += 3; continue; }
                    _ => {}
                }
            }
        }
        new_code.push(code[i].clone()); i += 1;
    }
    old_to_new[code.len()] = new_code.len();
    for op in new_code.iter_mut() {
        match op {
            OpCode::Jump(t) | OpCode::JumpIfFalse(t) | OpCode::JumpIfTrue(t) | OpCode::TryStart(t) => { if *t <= code.len() { *t = old_to_new[*t]; } }
            OpCode::JumpIfLocalCmpFalse(_, _, _, t) => { if *t <= code.len() { *t = old_to_new[*t]; } }
            OpCode::IncLocalJump(_, _, t) => { if *t <= code.len() { *t = old_to_new[*t]; } }
            OpCode::JumpIfLocalConstCmpFalse(_, _, _, t) => { if *t <= code.len() { *t = old_to_new[*t]; } }
            _ => {}
        }
    }
    new_code
}
// ===================== COMPILER =====================

struct LoopContext { continue_jumps: Vec<usize>, break_jumps: Vec<usize> }

#[derive(Debug, Default)]
struct UpvalueState { map: HashMap<String, usize>, list: Vec<String> }

struct EnclosingSnapshot {
    locals: HashMap<String, usize>,
    upvalues: Rc<RefCell<UpvalueState>>,
    captured_names: Rc<RefCell<Vec<String>>>,
    enclosing: Option<Rc<RefCell<EnclosingSnapshot>>>,
}

#[derive(Debug, Clone)]
enum CapturedSlot { Immediate(usize), ParentUpvalue(usize) }

#[derive(Debug, Clone)]
enum PendingOp {
    Ready(OpCode),
    GetUpvalue(usize),
    SetUpvalue(usize),
    MakeClosureRaw(usize, Vec<CapturedSlot>, usize, usize),
}

fn resolve_pending(ops: Vec<PendingOp>, final_next_local: usize) -> Vec<OpCode> {
    ops.into_iter().map(|op| match op {
        PendingOp::Ready(o) => o,
        PendingOp::GetUpvalue(idx) => OpCode::GetLocal(final_next_local + idx),
        PendingOp::SetUpvalue(idx) => OpCode::SetLocal(final_next_local + idx),
        PendingOp::MakeClosureRaw(fi, slots, np, nl) => {
            let resolved: Vec<usize> = slots.into_iter().map(|s| match s {
                CapturedSlot::Immediate(s) => s,
                CapturedSlot::ParentUpvalue(idx) => final_next_local + idx,
            }).collect();
            OpCode::MakeClosure(fi, resolved, np, nl)
        }
    }).collect()
}

pub struct Compiler {
    code: Vec<PendingOp>,
    locals: HashMap<String, usize>,
    next_local: usize,
    num_params: usize,
    upvalues: Rc<RefCell<UpvalueState>>,
    captured_names: Rc<RefCell<Vec<String>>>,
    enclosing: Option<Rc<RefCell<EnclosingSnapshot>>>,
    loop_stack: Vec<LoopContext>,
    functions: Rc<RefCell<Vec<Vec<OpCode>>>>,
    string_pool: Rc<RefCell<Vec<String>>>,
    string_map: Rc<RefCell<HashMap<String, u32>>>,
    globals: Rc<RefCell<HashMap<String, u32>>>,
    is_main: bool,
}

impl Compiler {
    pub fn new() -> Self {
        let c = Compiler {
            code: Vec::new(), locals: HashMap::new(), next_local: 0, num_params: 0,
            upvalues: Rc::new(RefCell::new(UpvalueState::default())),
            captured_names: Rc::new(RefCell::new(Vec::new())),
            enclosing: None, loop_stack: Vec::new(),
            functions: Rc::new(RefCell::new(Vec::new())),
            string_pool: Rc::new(RefCell::new(Vec::new())),
            string_map: Rc::new(RefCell::new(HashMap::new())),
            globals: Rc::new(RefCell::new(HashMap::new())),
            is_main: true,
        };
        c.globals.borrow_mut().insert("args".to_string(), 0);
        c
    }

    fn intern(&self, s: String) -> u32 {
        if let Some(&idx) = self.string_map.borrow().get(&s) { return idx; }
        let idx = self.string_pool.borrow().len() as u32;
        self.string_pool.borrow_mut().push(s.clone());
        self.string_map.borrow_mut().insert(s, idx);
        idx
    }

    fn alloc_global(&mut self, name: &str) -> u32 {
        let mut g = self.globals.borrow_mut();
        if let Some(&s) = g.get(name) { return s; }
        let s = g.len() as u32;
        g.insert(name.to_string(), s);
        s
    }

    fn lookup_global(&self, name: &str) -> Option<u32> {
        self.globals.borrow().get(name).copied()
    }

    fn prepass_globals(&mut self, stmts: &[Stmt]) {
        for stmt in stmts {
            match stmt {
                Stmt::Function(name, _, _) => { self.alloc_global(name); }
                Stmt::Import(path) => {
                    match fs::read_to_string(path) {
                        Ok(content) => match Lexer::new(&content).tokenize() {
                            Ok(tokens) => match Parser::new(tokens).parse() {
                                Ok(ast) => self.prepass_globals(&ast),
                                Err(e) => eprintln!("[import] parse error in {}: {}", path, e),
                            },
                            Err(e) => eprintln!("[import] lex error in {}: {}", path, e),
                        },
                        Err(e) => eprintln!("[import] read failed for {}: {}", path, e),
                    }
                }
                _ => {}
            }
        }
    }

    pub fn compile(mut self, stmts: Vec<Stmt>) -> (Vec<OpCode>, Vec<Vec<OpCode>>, Vec<String>) {
        self.prepass_globals(&stmts);
        for stmt in stmts { self.compile_stmt(stmt); }
        self.code.push(PendingOp::Ready(OpCode::Halt));
        let main_raw = resolve_pending(std::mem::take(&mut self.code), self.next_local);
        let before_main = main_raw.len();
        let main = peephole(main_raw);
        if std::env::var("DONKY_DEBUG").is_ok() {
            eprintln!("[PEEPHOLE] main {} -> {}", before_main, main.len());
        }
        let funcs_snapshot: Vec<Vec<OpCode>> = self.functions.borrow().iter().cloned().collect();
        let funcs: Vec<Vec<OpCode>> = funcs_snapshot.into_iter().map(|f| {
            let before = f.len();
            let after = peephole(f);
            if std::env::var("DONKY_DEBUG").is_ok() {
                eprintln!("[PEEPHOLE] func {} -> {}", before, after.len());
            }
            after
        }).collect();
        let pool: Vec<String> = self.string_pool.borrow().clone();
        (main, funcs, pool)
    }

    fn compile_function(&mut self, params: Vec<String>, body: Box<Stmt>, capture: bool)
        -> (usize, Vec<String>, usize, usize)
    {
        let enclosing = if capture {
            Some(Rc::new(RefCell::new(EnclosingSnapshot {
                locals: self.locals.clone(),
                upvalues: self.upvalues.clone(),
                captured_names: self.captured_names.clone(),
                enclosing: self.enclosing.clone(),
            })))
        } else { None };
        let mut sub = Compiler {
            code: Vec::new(), locals: HashMap::new(), next_local: 0,
            num_params: params.len(),
            upvalues: Rc::new(RefCell::new(UpvalueState::default())),
            captured_names: Rc::new(RefCell::new(Vec::new())),
            enclosing, loop_stack: Vec::new(),
            functions: self.functions.clone(),
            string_pool: self.string_pool.clone(),
            string_map: self.string_map.clone(),
            globals: self.globals.clone(),
            is_main: false,
        };
        for (i, p) in params.iter().enumerate() { sub.locals.insert(p.clone(), i); }
        sub.next_local = params.len();
        sub.compile_stmt(*body);
        sub.code.push(PendingOp::Ready(OpCode::Return));
        let final_next_local = sub.next_local;
        let func_code = resolve_pending(sub.code, final_next_local);
        let func_idx = self.functions.borrow().len();
        self.functions.borrow_mut().push(func_code);
        let captured: Vec<String> = sub.captured_names.borrow().clone();
        (func_idx, captured, sub.num_params, final_next_local - sub.num_params)
    }

    fn resolve_parent_slot(&mut self, name: &str) -> Option<CapturedSlot> {
        if let Some(&slot) = self.locals.get(name) { return Some(CapturedSlot::Immediate(slot)); }
        let upval_idx: Option<usize> = self.upvalues.borrow().map.get(name).copied();
        if let Some(idx) = upval_idx { return Some(CapturedSlot::ParentUpvalue(idx)); }
        let enc = self.enclosing.clone()?;
        Self::ensure_enclosing_captures(&enc, name)?;
        let idx = {
            let mut u = self.upvalues.borrow_mut();
            if let Some(&i) = u.map.get(name) { i } else {
                let i = u.map.len(); u.map.insert(name.to_string(), i); u.list.push(name.to_string()); i
            }
        };
        self.captured_names.borrow_mut().push(name.to_string());
        Some(CapturedSlot::ParentUpvalue(idx))
    }

    fn ensure_enclosing_captures(enclosing: &Rc<RefCell<EnclosingSnapshot>>, name: &str) -> Option<()> {
        let (has_local, has_upval, grand, enc_upv, enc_caps) = {
            let e = enclosing.borrow();
            let has_local = e.locals.contains_key(name);
            let has_upval = e.upvalues.borrow().map.contains_key(name);
            let grand = e.enclosing.clone();
            let enc_upv = e.upvalues.clone();
            let enc_caps = e.captured_names.clone();
            (has_local, has_upval, grand, enc_upv, enc_caps)
        };
        if has_local || has_upval { return Some(()); }
        let grand = grand?;
        Self::ensure_enclosing_captures(&grand, name)?;
        let mut u = enc_upv.borrow_mut();
        if !u.map.contains_key(name) {
            let idx = u.map.len(); u.map.insert(name.to_string(), idx); u.list.push(name.to_string());
            drop(u);
            enc_caps.borrow_mut().push(name.to_string());
        }
        Some(())
    }

    fn compile_stmt(&mut self, stmt: Stmt) {
        match stmt {
            Stmt::Asm(_) => {}
            Stmt::Import(path) => {
                let content = match fs::read_to_string(&path) {
                    Ok(c) => c,
                    Err(e) => { eprintln!("[import] read failed for {}: {}", path, e); return; }
                };
                let mut lex = Lexer::new(&content);
                let tokens = match lex.tokenize() {
                    Ok(t) => t,
                    Err(e) => { eprintln!("[import] lex error in {}: {}", path, e); return; }
                };
                let mut parser = Parser::new(tokens);
                let imported = match parser.parse() {
                    Ok(a) => a,
                    Err(e) => { eprintln!("[import] parse error in {}: {}", path, e); return; }
                };
                self.prepass_globals(&imported);
                for s in imported { self.compile_stmt(s); }
            }
            Stmt::Block(stmts) => {
                let saved = self.locals.clone();
                let saved_is_main = self.is_main;
                self.is_main = false;
                for s in stmts { self.compile_stmt(s); }
                self.is_main = saved_is_main;
                self.locals = saved;
            }
            Stmt::Let(name, expr) => {
                self.compile_expr(expr);
                let slot = self.next_local; self.locals.insert(name.clone(), slot); self.next_local += 1;
                if self.is_main {
                    let gslot = self.alloc_global(&name);
                    self.code.push(PendingOp::Ready(OpCode::Dup));
                    self.code.push(PendingOp::Ready(OpCode::SetGlobal(gslot)));
                }
                self.code.push(PendingOp::Ready(OpCode::SetLocal(slot)));
            }
            Stmt::Print(expr) => { self.compile_expr(expr); self.code.push(PendingOp::Ready(OpCode::Print)); }
            Stmt::Assign(name, expr) => {
                self.compile_expr(expr);
                if let Some(&slot) = self.locals.get(&name) {
                    self.code.push(PendingOp::Ready(OpCode::SetLocal(slot)));
                } else {
                    let uv: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = uv {
                        self.code.push(PendingOp::SetUpvalue(idx));
                    } else if let Some(gslot) = self.lookup_global(&name) {
                        self.code.push(PendingOp::Ready(OpCode::SetGlobal(gslot)));
                    }
                }
            }
            Stmt::FieldSet(obj, field, val) => {
                self.compile_expr(obj); self.compile_expr(val);
                self.code.push(PendingOp::Ready(OpCode::FieldSet(field)));
            }
            Stmt::IndexSet(target, idx_expr, val_expr) => {
                self.compile_expr(*target);
                self.compile_expr(*idx_expr);
                self.compile_expr(*val_expr);
                self.code.push(PendingOp::Ready(OpCode::IndexSet));
            }
            Stmt::ExprStmt(expr) => { self.compile_expr(expr); self.code.push(PendingOp::Ready(OpCode::Pop)); }
            Stmt::If(cond, then_b, else_b) => {
                self.compile_expr(cond);
                let jif = self.code.len(); self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                self.compile_stmt(*then_b);
                if let Some(eb) = else_b {
                    let jo = self.code.len(); self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                    let et = self.code.len(); self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(et));
                    self.compile_stmt(*eb);
                    let end = self.code.len(); self.code[jo] = PendingOp::Ready(OpCode::Jump(end));
                } else { let end = self.code.len(); self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(end)); }
            }
            Stmt::While(cond, body) => {
                let ls = self.code.len();
                self.compile_expr(cond);
                let jif = self.code.len(); self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                self.loop_stack.push(LoopContext { continue_jumps: Vec::new(), break_jumps: Vec::new() });
                self.compile_stmt(*body);
                let ctx = self.loop_stack.pop().unwrap();
                self.code.push(PendingOp::Ready(OpCode::Jump(ls)));
                let le = self.code.len();
                self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(le));
                for j in ctx.continue_jumps { self.code[j] = PendingOp::Ready(OpCode::Jump(ls)); }
                for j in ctx.break_jumps { self.code[j] = PendingOp::Ready(OpCode::Jump(le)); }
            }
            Stmt::For(name, iter_expr, body) => {
                let saved = self.locals.clone();
                let iter_slot = self.next_local; self.next_local += 1;
                let idx_slot = self.next_local; self.next_local += 1;
                let x_slot = self.next_local; self.next_local += 1;
                self.compile_expr(iter_expr);
                self.code.push(PendingOp::Ready(OpCode::SetLocal(iter_slot)));
                self.code.push(PendingOp::Ready(OpCode::ConstantNumber(0.0)));
                self.code.push(PendingOp::Ready(OpCode::SetLocal(idx_slot)));
                let ls = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::GetLocal(idx_slot)));
                self.code.push(PendingOp::Ready(OpCode::GetLocal(iter_slot)));
                self.code.push(PendingOp::Ready(OpCode::Builtin("len".to_string(), 1)));
                self.code.push(PendingOp::Ready(OpCode::Lt));
                let jif = self.code.len(); self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                self.code.push(PendingOp::Ready(OpCode::GetLocal(iter_slot)));
                self.code.push(PendingOp::Ready(OpCode::GetLocal(idx_slot)));
                self.code.push(PendingOp::Ready(OpCode::Index));
                self.code.push(PendingOp::Ready(OpCode::SetLocal(x_slot)));
                self.locals.insert(name.clone(), x_slot);
                self.loop_stack.push(LoopContext { continue_jumps: Vec::new(), break_jumps: Vec::new() });
                self.compile_stmt(*body);
                let ctx = self.loop_stack.pop().unwrap();
                let incr = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::IncLocal(idx_slot as u32, 1.0)));
                self.code.push(PendingOp::Ready(OpCode::Jump(ls)));
                let end = self.code.len();
                self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(end));
                for j in ctx.continue_jumps { self.code[j] = PendingOp::Ready(OpCode::Jump(incr)); }
                for j in ctx.break_jumps { self.code[j] = PendingOp::Ready(OpCode::Jump(end)); }
                self.locals = saved;
            }
            Stmt::Break => { if let Some(ctx) = self.loop_stack.last_mut() { let j = self.code.len(); self.code.push(PendingOp::Ready(OpCode::Jump(0))); ctx.break_jumps.push(j); } }
            Stmt::Continue => { if let Some(ctx) = self.loop_stack.last_mut() { let j = self.code.len(); self.code.push(PendingOp::Ready(OpCode::Jump(0))); ctx.continue_jumps.push(j); } }
            Stmt::Match(target, cases, else_block) => {
                self.compile_expr(target);
                let mut end_jumps = Vec::new();
                for (cv, block) in cases {
                    self.code.push(PendingOp::Ready(OpCode::Dup));
                    self.compile_expr(cv);
                    self.code.push(PendingOp::Ready(OpCode::Eq));
                    let jif = self.code.len(); self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                    self.code.push(PendingOp::Ready(OpCode::Pop));
                    self.compile_stmt(*block);
                    let ej = self.code.len(); self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                    end_jumps.push(ej);
                    let nc = self.code.len(); self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(nc));
                }
                self.code.push(PendingOp::Ready(OpCode::Pop));
                if let Some(eb) = else_block { self.compile_stmt(*eb); }
                let end = self.code.len();
                for j in end_jumps { self.code[j] = PendingOp::Ready(OpCode::Jump(end)); }
            }
            Stmt::Function(name, params, body) => {
                let mut final_params = params;
                if name.contains('.') {
                    if final_params.first().map_or(true, |p| p != "self") { final_params.insert(0, "self".to_string()); }
                }
                let (idx, _caps, _np, _nl) = self.compile_function(final_params, body, false);
                let slot = self.next_local; self.locals.insert(name.clone(), slot); self.next_local += 1;
                if self.is_main {
                    let gslot = self.lookup_global(&name).unwrap_or_else(|| self.alloc_global(&name));
                    self.code.push(PendingOp::Ready(OpCode::DefineFunctionGlobal(name, idx, slot, gslot)));
                } else {
                    self.code.push(PendingOp::Ready(OpCode::DefineFunction(name, idx, slot)));
                }
            }
            Stmt::Return(expr) => {
                if let Some(e) = expr { self.compile_expr(e); } else { self.code.push(PendingOp::Ready(OpCode::ConstantNull)); }
                self.code.push(PendingOp::Ready(OpCode::Return));
            }
            Stmt::Throw(expr) => { self.compile_expr(expr); self.code.push(PendingOp::Ready(OpCode::Throw)); }
            Stmt::Try(try_block, err_name, catch_block) => {
                let ts = self.code.len(); self.code.push(PendingOp::Ready(OpCode::TryStart(0)));
                self.compile_stmt(*try_block);
                self.code.push(PendingOp::Ready(OpCode::TryEnd));
                let jo = self.code.len(); self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                let hs = self.code.len(); self.code[ts] = PendingOp::Ready(OpCode::TryStart(hs));
                let es = self.next_local; self.next_local += 1;
                let saved = self.locals.insert(err_name.clone(), es);
                self.code.push(PendingOp::Ready(OpCode::SetLocal(es)));
                self.compile_stmt(*catch_block);
                let end = self.code.len(); self.code[jo] = PendingOp::Ready(OpCode::Jump(end));
                match saved { Some(old) => { self.locals.insert(err_name, old); } None => { self.locals.remove(&err_name); } }
            }
            Stmt::StructDef(_, _) => {}
        }
    }

    fn compile_expr(&mut self, expr: Expr) {
        match expr {
            Expr::Number(n) => self.code.push(PendingOp::Ready(OpCode::ConstantNumber(n))),
            Expr::String(s) => { let idx = self.intern(s); self.code.push(PendingOp::Ready(OpCode::StringConst(idx))); }
            Expr::Bool(b) => self.code.push(PendingOp::Ready(OpCode::ConstantBool(b))),
            Expr::Ident(name) => {
                if let Some(&slot) = self.locals.get(&name) {
                    self.code.push(PendingOp::Ready(OpCode::GetLocal(slot)));
                } else {
                    let uv: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = uv {
                        self.code.push(PendingOp::GetUpvalue(idx));
                    } else if let Some(enc) = self.enclosing.clone() {
                        if Self::ensure_enclosing_captures(&enc, &name).is_some() {
                            let idx = { let mut u = self.upvalues.borrow_mut(); if let Some(&i) = u.map.get(&name) { i } else { let i = u.map.len(); u.map.insert(name.clone(), i); u.list.push(name.clone()); i } };
                            self.captured_names.borrow_mut().push(name.clone());
                            self.code.push(PendingOp::GetUpvalue(idx));
                        } else if let Some(gslot) = self.lookup_global(&name) {
                            self.code.push(PendingOp::Ready(OpCode::GetGlobal(gslot)));
                        } else {
                            self.code.push(PendingOp::Ready(OpCode::ConstantNull));
                        }
                    } else if let Some(gslot) = self.lookup_global(&name) {
                        self.code.push(PendingOp::Ready(OpCode::GetGlobal(gslot)));
                    } else {
                        self.code.push(PendingOp::Ready(OpCode::ConstantNull));
                    }
                }
            }
            Expr::InterpolatedString(parts) => {
                let n = parts.len();
                for p in parts { self.compile_expr(p); }
                self.code.push(PendingOp::Ready(OpCode::Interpolate(n)));
            }
            Expr::Function(params, body) => {
                let (idx, names, np, nl) = self.compile_function(params, body, true);
                let mut slots = Vec::new();
                for name in names {
                    match self.resolve_parent_slot(&name) {
                        Some(s) => slots.push(s),
                        None => slots.push(CapturedSlot::Immediate(usize::MAX)),
                    }
                }
                self.code.push(PendingOp::MakeClosureRaw(idx, slots, np, nl));
            }
            Expr::Array(elements) => {
                let len = elements.len();
                for el in elements { self.compile_expr(el); }
                self.code.push(PendingOp::Ready(OpCode::Array(len)));
            }
            Expr::Map(pairs) => {
                let len = pairs.len();
                for (k, v) in pairs { self.compile_expr(k); self.compile_expr(v); }
                self.code.push(PendingOp::Ready(OpCode::Map(len)));
            }
            Expr::StructInit(name, fields) => {
                let mut fns = Vec::new();
                for (f, v) in fields { self.compile_expr(v); fns.push(f); }
                self.code.push(PendingOp::Ready(OpCode::Struct(name, fns)));
            }
            Expr::FieldAccess(obj, field) => { self.compile_expr(*obj); self.code.push(PendingOp::Ready(OpCode::FieldAccess(field))); }
            Expr::Index(t, i) => { self.compile_expr(*t); self.compile_expr(*i); self.code.push(PendingOp::Ready(OpCode::Index)); }
            Expr::MethodCall(t, m, args) => {
                self.compile_expr(*t);
                for a in args.iter().rev() { self.compile_expr(a.clone()); }
                self.code.push(PendingOp::Ready(OpCode::MethodCall(m, args.len())));
            }
            Expr::Call(name, args) => {
                for a in args.iter().rev() { self.compile_expr(a.clone()); }
                if is_builtin(&name) {
                    self.code.push(PendingOp::Ready(OpCode::Builtin(name, args.len())));
                } else if let Some(&slot) = self.locals.get(&name) {
                    self.code.push(PendingOp::Ready(OpCode::GetLocal(slot)));
                    self.code.push(PendingOp::Ready(OpCode::Call(args.len())));
                } else {
                    let uv: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = uv {
                        self.code.push(PendingOp::GetUpvalue(idx));
                        self.code.push(PendingOp::Ready(OpCode::Call(args.len())));
                    } else if let Some(gslot) = self.lookup_global(&name) {
                        self.code.push(PendingOp::Ready(OpCode::CallGlobal(gslot, args.len())));
                    } else {
                        for _ in 0..args.len() { self.code.push(PendingOp::Ready(OpCode::Pop)); }
                        self.code.push(PendingOp::Ready(OpCode::ConstantNull));
                    }
                }
            }
            Expr::Unary(op, right) => {
                // Constant folding
                if let (TokenType::Minus, Expr::Number(n)) = (&op, &*right) {
                    self.code.push(PendingOp::Ready(OpCode::ConstantNumber(-*n)));
                    return;
                }
                if let (TokenType::Bang, Expr::Bool(b)) = (&op, &*right) {
                    self.code.push(PendingOp::Ready(OpCode::ConstantBool(!*b)));
                    return;
                }
                self.compile_expr(*right);
                match op { TokenType::Minus => self.code.push(PendingOp::Ready(OpCode::Negate)), TokenType::Bang => self.code.push(PendingOp::Ready(OpCode::Not)), _ => {} }
            }
            Expr::Binary(l, op, r) => {
                // Constant folding: number op number
                if let (Expr::Number(a), Expr::Number(b)) = (&*l, &*r) {
                    let (a, b) = (*a, *b);
                    match op {
                        TokenType::Plus    => { self.code.push(PendingOp::Ready(OpCode::ConstantNumber(a + b))); return; }
                        TokenType::Minus   => { self.code.push(PendingOp::Ready(OpCode::ConstantNumber(a - b))); return; }
                        TokenType::Star    => { self.code.push(PendingOp::Ready(OpCode::ConstantNumber(a * b))); return; }
                        TokenType::Slash   if b != 0.0 => { self.code.push(PendingOp::Ready(OpCode::ConstantNumber(a / b))); return; }
                        TokenType::Percent if b != 0.0 => { self.code.push(PendingOp::Ready(OpCode::ConstantNumber(a % b))); return; }
                        TokenType::EqEq    => { self.code.push(PendingOp::Ready(OpCode::ConstantBool(a == b))); return; }
                        TokenType::NotEq   => { self.code.push(PendingOp::Ready(OpCode::ConstantBool(a != b))); return; }
                        TokenType::Lt      => { self.code.push(PendingOp::Ready(OpCode::ConstantBool(a < b))); return; }
                        TokenType::Gt      => { self.code.push(PendingOp::Ready(OpCode::ConstantBool(a > b))); return; }
                        TokenType::LtEq    => { self.code.push(PendingOp::Ready(OpCode::ConstantBool(a <= b))); return; }
                        TokenType::GtEq    => { self.code.push(PendingOp::Ready(OpCode::ConstantBool(a >= b))); return; }
                        _ => {}
                    }
                }
                // Constant folding: string + string
                if let (TokenType::Plus, Expr::String(a), Expr::String(b)) = (&op, &*l, &*r) {
                    let idx = self.intern(format!("{}{}", a, b));
                    self.code.push(PendingOp::Ready(OpCode::StringConst(idx)));
                    return;
                }
                if op == TokenType::And {
                    self.compile_expr(*l);
                    let jif_idx = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::Dup));
                    self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                    self.code.push(PendingOp::Ready(OpCode::Pop));
                    self.compile_expr(*r);
                    let end = self.code.len();
                    if let PendingOp::Ready(OpCode::JumpIfFalse(t)) = &mut self.code[jif_idx + 1] { *t = end; }
                    return;
                }
                if op == TokenType::Or {
                    self.compile_expr(*l);
                    let jit_idx = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::Dup));
                    self.code.push(PendingOp::Ready(OpCode::JumpIfTrue(0)));
                    self.code.push(PendingOp::Ready(OpCode::Pop));
                    self.compile_expr(*r);
                    let end = self.code.len();
                    if let PendingOp::Ready(OpCode::JumpIfTrue(t)) = &mut self.code[jit_idx + 1] { *t = end; }
                    return;
                }
                self.compile_expr(*l); self.compile_expr(*r);
                let oc = match op {
                    TokenType::Plus => OpCode::Add, TokenType::Minus => OpCode::Sub,
                    TokenType::Star => OpCode::Mul, TokenType::Slash => OpCode::Div,
                    TokenType::Percent => OpCode::Mod,
                    TokenType::EqEq => OpCode::Eq, TokenType::NotEq => OpCode::NotEq,
                    TokenType::Lt => OpCode::Lt, TokenType::Gt => OpCode::Gt,
                    TokenType::LtEq => OpCode::LtEq, TokenType::GtEq => OpCode::GtEq,
                    _ => return,
                };
                self.code.push(PendingOp::Ready(oc));
            }
            Expr::Ternary(c, t, e) => {
                self.compile_expr(*c);
                let jif = self.code.len(); self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                self.compile_expr(*t);
                let jo = self.code.len(); self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                let et = self.code.len(); self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(et));
                self.compile_expr(*e);
                let end = self.code.len(); self.code[jo] = PendingOp::Ready(OpCode::Jump(end));
            }
        }
    }
}

fn is_builtin(name: &str) -> bool {
    matches!(name,
        "str" | "num" | "type" | "len" | "push" | "pop" | "keys"
        | "split" | "join" | "upper" | "lower" | "trim" | "replace" | "contains"
        | "str_eq" | "char_code" | "byte_at" | "byte_substr" | "bytes_to_str"
        | "read_file" | "write_file" | "file_exists" | "is_dir" | "list_dir" | "mkdir" | "now"
        | "random" | "clear_screen" | "print_str" | "print_char" | "read_line" | "input"
        | "tcp_listen" | "tcp_accept" | "tcp_read" | "tcp_write" | "tcp_close" | "http_get" | "system"
    )
}

// ===================== BUILTINS (free fn) =====================

fn builtin_impl(heap: &mut Heap, name: &str, args: Vec<Value>) -> Result<Value, String> {
    match name {
        "str" => { let s = args.get(0).map(|v| v.to_string_with_heap(heap)).unwrap_or_default(); let i = heap.alloc(HeapObject::String(s)); Ok(Value::string(i)) }
        "num" => match args.get(0) {
            Some(v) if v.is_number() => Ok(Value::number(v.as_f64())),
            Some(v) if v.is_string() => { let s = heap.get_string(v.payload()).clone(); s.parse::<f64>().map(Value::number).map_err(|_| format!("num() cannot parse {:?}", s)) }
            _ => Err("num() expects string or number".to_string()),
        },
        "type" => {
            let t = args.get(0).map(|v| v.kind()).map(|k| match k {
                ValueKind::Number(_) => "number", ValueKind::String(_) => "string", ValueKind::Bool(_) => "bool",
                ValueKind::Array(_) => "array", ValueKind::Map(_) => "map", ValueKind::Struct(_) => "struct",
                ValueKind::Function(_) | ValueKind::Closure(_) => "function", ValueKind::Null => "null",
            }).unwrap_or("null");
            let i = heap.alloc(HeapObject::String(t.to_string())); Ok(Value::string(i))
        }
        "len" => match args.get(0) {
            Some(v) if v.is_string() => Ok(Value::number(heap.get_string(v.payload()).len() as f64)),
            Some(v) if v.is_array() => Ok(Value::number(heap.get_array(v.payload()).len() as f64)),
            Some(v) if v.is_map() => Ok(Value::number(heap.get_map(v.payload()).len() as f64)),
            _ => Err("len() expects collection".to_string()),
        },
        "push" => { if args.len() >= 2 && args[0].is_array() { let idx = args[0].payload(); let v = args[1]; heap.get_array_mut(idx).push(v); Ok(Value::null()) } else { Err("push() expects array".to_string()) } }
        "pop" => { if let Some(v) = args.get(0) { if v.is_array() { return Ok(heap.get_array_mut(v.payload()).pop().unwrap_or(Value::null())); } } Err("pop() expects array".to_string()) }
        "keys" => {
            if let Some(v) = args.get(0) { if v.is_map() {
                let key_list: Vec<MapKey> = heap.get_map(v.payload()).keys().cloned().collect();
                let mut keys = Vec::with_capacity(key_list.len());
                for k in key_list {
                    match k {
                        MapKey::String(s) => { let i = heap.alloc(HeapObject::String(s)); keys.push(Value::string(i)); }
                        MapKey::Number(n) => keys.push(Value::number(f64::from_bits(n as u64))),
                        MapKey::Bool(b) => keys.push(Value::bool(b)),
                    }
                }
                let idx = heap.alloc(HeapObject::Array(keys));
                return Ok(Value::array(idx));
            } }
            Err("keys() expects map".to_string())
        }
        "split" => {
            if args.len() >= 2 && args[0].is_string() && args[1].is_string() {
                let s = heap.get_string(args[0].payload()).clone();
                let sep = heap.get_string(args[1].payload()).clone();
                if sep.is_empty() { return Err("split() sep empty".to_string()); }
                let parts: Vec<Value> = s.split(&sep).map(|p| { let i = heap.alloc(HeapObject::String(p.to_string())); Value::string(i) }).collect();
                let idx = heap.alloc(HeapObject::Array(parts));
                Ok(Value::array(idx))
            } else { Err("split() expects two strings".to_string()) }
        }
        "join" => {
            if args.len() >= 2 && args[0].is_array() && args[1].is_string() {
                let sep = heap.get_string(args[1].payload()).clone();
                let arr = heap.get_array(args[0].payload()).clone();
                let mut parts = Vec::new();
                for item in arr.iter() { if item.is_string() { parts.push(heap.get_string(item.payload()).clone()); } else { return Err("join() expects strings".to_string()); } }
                let i = heap.alloc(HeapObject::String(parts.join(&sep))); Ok(Value::string(i))
            } else { Err("join() expects array and string".to_string()) }
        }
        "upper" | "lower" | "trim" => {
            if let Some(v) = args.get(0) { if v.is_string() {
                let s = heap.get_string(v.payload());
                let out = match name { "upper" => s.to_uppercase(), "lower" => s.to_lowercase(), _ => s.trim().to_string() };
                let i = heap.alloc(HeapObject::String(out)); return Ok(Value::string(i));
            } }
            Err(format!("{}()", name))
        }
        "replace" => {
            if args.len() >= 3 && args[0].is_string() && args[1].is_string() && args[2].is_string() {
                let s = heap.get_string(args[0].payload()).clone();
                let o = heap.get_string(args[1].payload()).clone();
                let n = heap.get_string(args[2].payload()).clone();
                let i = heap.alloc(HeapObject::String(s.replace(&o, &n))); Ok(Value::string(i))
            } else { Err("replace()".to_string()) }
        }
        "contains" => match (args.get(0), args.get(1)) {
            (Some(t), Some(i)) if t.is_string() && i.is_string() => Ok(Value::bool(heap.get_string(t.payload()).contains(heap.get_string(i.payload()).as_str()))),
            (Some(t), Some(i)) if t.is_array() => {
                let arr = heap.get_array(t.payload());
                let needle = i.to_string_with_heap(heap);
                Ok(Value::bool(arr.iter().any(|x| x.to_string_with_heap(heap) == needle)))
            }
            (Some(t), Some(i)) if t.is_map() => {
                if let Ok(k) = MapKey::from_value(*i, heap) { return Ok(Value::bool(heap.get_map(t.payload()).contains_key(&k))); }
                Ok(Value::bool(false))
            }
            _ => Err("contains()".to_string()),
        },
        "str_eq" => {
            if args.len() >= 2 {
                let a = args[0].to_string_with_heap(heap); let b = args[1].to_string_with_heap(heap);
                Ok(Value::number(if a == b { 1.0 } else { 0.0 }))
            } else { Err("str_eq()".to_string()) }
        }
        "char_code" => {
            if let Some(v) = args.get(0) { if v.is_string() {
                let s = heap.get_string(v.payload());
                if s.as_bytes().len() == 1 { return Ok(Value::number(s.as_bytes()[0] as f64)); }
            } }
            Err("char_code()".to_string())
        }
        "byte_at" => {
            if args.len() >= 2 && args[0].is_string() {
                if let Some(n) = args[1].as_number() {
                    let s = heap.get_string(args[0].payload());
                    let bytes = s.as_bytes();
                    let idx = n as isize;
                    if idx < 0 || idx as usize >= bytes.len() { return Ok(Value::number(-1.0)); }
                    return Ok(Value::number(bytes[idx as usize] as f64));
                }
            }
            Err("byte_at()".to_string())
        }
        "byte_substr" => {
            if args.len() >= 3 && args[0].is_string() {
                if let (Some(a), Some(b)) = (args[1].as_number(), args[2].as_number()) {
                    let s = heap.get_string(args[0].payload()).clone();
                    let bytes = s.as_bytes();
                    let mut start = if a < 0.0 { 0 } else { a as usize };
                    let mut end = if b < 0.0 { 0 } else { b as usize };
                    if start > bytes.len() { start = bytes.len(); }
                    if end > bytes.len() { end = bytes.len(); }
                    if start > end { start = end; }
                    let i = heap.alloc(HeapObject::String(String::from_utf8_lossy(&bytes[start..end]).to_string()));
                    return Ok(Value::string(i));
                }
            }
            Err("byte_substr()".to_string())
        }
        "bytes_to_str" => {
            if let Some(v) = args.get(0) { if v.is_array() {
                let arr = heap.get_array(v.payload()).clone();
                let mut buf = Vec::with_capacity(arr.len());
                for item in arr.iter() {
                    if let Some(n) = item.as_number() {
                        let b = n as i64;
                        if !(0..=255).contains(&b) { return Err(format!("byte out of range: {}", b)); }
                        buf.push(b as u8);
                    } else { return Err("bytes_to_str()".to_string()); }
                }
                let i = heap.alloc(HeapObject::String(String::from_utf8_lossy(&buf).to_string()));
                return Ok(Value::string(i));
            } }
            Err("bytes_to_str()".to_string())
        }
        "read_file" => match args.get(0) {
            Some(v) if v.is_string() => { let p = heap.get_string(v.payload()).clone(); let s = fs::read_to_string(&p).map_err(|e| e.to_string())?; let i = heap.alloc(HeapObject::String(s)); Ok(Value::string(i)) }
            _ => Err("read_file()".to_string()),
        },
        "write_file" => {
            if args.len() >= 2 && args[0].is_string() && args[1].is_string() {
                let p = heap.get_string(args[0].payload()).clone();
                let c = heap.get_string(args[1].payload()).clone();
                fs::write(&p, c).map_err(|e| e.to_string())?;
                Ok(Value::null())
            } else { Err("write_file()".to_string()) }
        }
        "file_exists" => match args.get(0) {
            Some(v) if v.is_string() => { let p = heap.get_string(v.payload()).clone(); Ok(Value::bool(Path::new(&p).exists())) }
            _ => Err("file_exists()".to_string()),
        },
        "is_dir" => match args.get(0) {
            Some(v) if v.is_string() => {
                let p = heap.get_string(v.payload()).clone();
                Ok(Value::bool(Path::new(&p).is_dir()))
            }
            _ => Err("is_dir() expects string".to_string()),
        },
        "now" => {
            use std::time::{SystemTime, UNIX_EPOCH};
            let secs = SystemTime::now().duration_since(UNIX_EPOCH)
                .map(|d| d.as_secs() as f64).unwrap_or(0.0);
            Ok(Value::number(secs))
        }
        "list_dir" => {
            if let Some(v) = args.get(0) { if v.is_string() {
                let p = heap.get_string(v.payload()).clone();
                let mut files = Vec::new();
                if let Ok(entries) = fs::read_dir(&p) { for e in entries.flatten() { if let Ok(name) = e.file_name().into_string() { let i = heap.alloc(HeapObject::String(name)); files.push(Value::string(i)); } } }
                let idx = heap.alloc(HeapObject::Array(files));
                return Ok(Value::array(idx));
            } }
            Err("list_dir()".to_string())
        }
        "mkdir" => match args.get(0) {
            Some(v) if v.is_string() => { let p = heap.get_string(v.payload()).clone(); let _ = fs::create_dir_all(&p); Ok(Value::null()) }
            _ => Err("mkdir()".to_string()),
        },
        "random" => {
            let max = args.get(0).and_then(|v| v.as_number()).map(|n| n as u64).unwrap_or(100);
            use std::time::{SystemTime, UNIX_EPOCH};
            let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().subsec_nanos() as u64;
            Ok(Value::number(((nanos * 1103515245 + 12345) % max) as f64))
        }
        "clear_screen" => { print!("\x1b[2J\x1b[1;1H"); use std::io::Write; std::io::stdout().flush().unwrap(); Ok(Value::null()) }
        "print_str" => { if let Some(v) = args.get(0) { print!("{}", v.to_string_with_heap(heap)); use std::io::Write; std::io::stdout().flush().unwrap(); } Ok(Value::null()) }
        "print_char" => { if let Some(n) = args.get(0).and_then(|v| v.as_number()) { print!("{}", n as u8 as char); use std::io::Write; std::io::stdout().flush().unwrap(); } Ok(Value::null()) }
        "read_line" => { let mut input = String::new(); std::io::stdin().read_line(&mut input).unwrap(); let i = heap.alloc(HeapObject::String(input.trim().to_string())); Ok(Value::string(i)) }
        "input" => {
            let prompt = args.get(0).filter(|v| v.is_string()).map(|v| heap.get_string(v.payload()).clone()).unwrap_or_default();
            print!("{}", prompt); use std::io::Write; std::io::stdout().flush().unwrap();
            let mut input = String::new(); std::io::stdin().read_line(&mut input).unwrap();
            let i = heap.alloc(HeapObject::String(input.trim().to_string())); Ok(Value::string(i))
        }
        "tcp_listen" => {
            if let Some(p) = args.get(0).and_then(|v| v.as_number()) {
                let l = TcpListener::bind(format!("127.0.0.1:{}", p)).map_err(|e| e.to_string())?;
                let mut c = get_counter().lock().unwrap(); let id = *c; *c += 1;
                get_servers().lock().unwrap().insert(id, l);
                return Ok(Value::number(id as f64));
            }
            Err("tcp_listen()".to_string())
        }
        "tcp_accept" => {
            if let Some(id) = args.get(0).and_then(|v| v.as_number()) {
                let servers = get_servers().lock().unwrap();
                if let Some(l) = servers.get(&(id as usize)) {
                    match l.accept() {
                        Ok((s, _)) => { let mut c = get_counter().lock().unwrap(); let cid = *c; *c += 1; get_clients().lock().unwrap().insert(cid, s); return Ok(Value::number(cid as f64)); }
                        Err(e) => return Err(format!("accept: {}", e)),
                    }
                }
            }
            Err("tcp_accept()".to_string())
        }
        "tcp_read" => {
            if let Some(id) = args.get(0).and_then(|v| v.as_number()) {
                let max = args.get(1).and_then(|v| v.as_number()).map(|n| n as usize).unwrap_or(4096);
                let mut clients = get_clients().lock().unwrap();
                if let Some(s) = clients.get_mut(&(id as usize)) {
                    let mut buf = vec![0; max];
                    match s.read(&mut buf) { Ok(n) => { buf.truncate(n); let i = heap.alloc(HeapObject::String(String::from_utf8_lossy(&buf).to_string())); return Ok(Value::string(i)); } Err(e) => return Err(format!("read: {}", e)) }
                }
            }
            Err("tcp_read()".to_string())
        }
        "tcp_write" => {
            if args.len() >= 2 {
                if let (Some(id), true) = (args[0].as_number(), args[1].is_string()) {
                    let msg = heap.get_string(args[1].payload()).clone();
                    let mut clients = get_clients().lock().unwrap();
                    if let Some(s) = clients.get_mut(&(id as usize)) { return s.write_all(msg.as_bytes()).map(|_| Value::null()).map_err(|e| e.to_string()); }
                }
            }
            Err("tcp_write()".to_string())
        }
        "tcp_close" => {
            if let Some(id) = args.get(0).and_then(|v| v.as_number()) {
                get_clients().lock().unwrap().remove(&(id as usize));
                get_servers().lock().unwrap().remove(&(id as usize));
                return Ok(Value::null());
            }
            Err("tcp_close()".to_string())
        }
        "http_get" => {
            if let Some(v) = args.get(0) { if v.is_string() {
                let url = heap.get_string(v.payload()).clone();
                match ureq::get(&url).call() {
                    Ok(resp) => { let s = resp.into_string().map_err(|e| e.to_string())?; let i = heap.alloc(HeapObject::String(s)); return Ok(Value::string(i)); }
                    Err(e) => return Err(format!("http_get: {}", e)),
                }
            } }
            Err("http_get()".to_string())
        }
        "system" => {
            if let Some(v) = args.get(0) { if v.is_string() {
                let cmd = heap.get_string(v.payload()).clone();
                #[cfg(windows)]
                let output = std::process::Command::new("cmd").args(&["/C", &cmd]).output();
                #[cfg(not(windows))]
                let output = std::process::Command::new("sh").args(&["-c", &cmd]).output();
                match output {
                    Ok(out) => {
                        let mut s = String::from_utf8_lossy(&out.stdout).to_string();
                        s.push_str(&String::from_utf8_lossy(&out.stderr));
                        let i = heap.alloc(HeapObject::String(s));
                        return Ok(Value::string(i));
                    }
                    Err(e) => return Err(format!("system() failed: {}", e)),
                }
            } }
            Err("system() expects string".to_string())
        }
        _ => Err(format!("Unknown builtin: {}", name)),
    }
}

// ===================== VM =====================

struct Handler {
    ip: usize,
    current_code: Rc<Vec<OpCode>>,
    locals: Vec<Value>,
    frames: Vec<Vec<Value>>,
    call_stack: Vec<(usize, usize, Rc<Vec<OpCode>>)>,
    closure_stack: Vec<Option<(Rc<RefCell<Vec<Value>>>, usize, usize)>>,
    stack: Vec<Value>,
}

pub struct VM {
    stack: Vec<Value>,
    locals: Vec<Value>,
    globals: Vec<Value>,
    frames: Vec<Vec<Value>>,
    functions: Vec<Vec<OpCode>>,
    functions_by_name: HashMap<String, u32>,
    call_stack: Vec<(usize, usize, Rc<Vec<OpCode>>)>,
    closure_stack: Vec<Option<(Rc<RefCell<Vec<Value>>>, usize, usize)>>,
    handlers: Vec<Handler>,
    string_pool_heap: Vec<u32>,
    locals_pool: Vec<Vec<Value>>, // Recycles Vecs to eliminate heap allocation on calls
    pub heap: Heap,
    pub silent: bool,
}

impl VM {
    pub fn new() -> Self {
        VM {
            stack: Vec::new(), locals: Vec::new(), globals: Vec::new(), frames: Vec::new(),
            functions: Vec::new(), functions_by_name: HashMap::new(),
            call_stack: Vec::new(), closure_stack: Vec::new(),
            handlers: Vec::new(), string_pool_heap: Vec::new(), locals_pool: Vec::new(),
            heap: Heap::new(),
            silent: false,
        }
    }

    pub fn run(&mut self, code: Vec<OpCode>, funcs: Vec<Vec<OpCode>>, string_pool: Vec<String>, script_args: Vec<String>) -> Result<(), String> {
        self.run_impl(code, funcs, string_pool, script_args, None)
    }

    pub fn run_profiled(
        &mut self,
        code: Vec<OpCode>,
        funcs: Vec<Vec<OpCode>>,
        string_pool: Vec<String>,
        script_args: Vec<String>,
    ) -> Result<crate::profiler::ProfileReport, String> {
        let mut report = crate::profiler::ProfileReport::new();
        self.run_impl(code, funcs, string_pool, script_args, Some(&mut report))?;
        Ok(report)
    }

    fn run_impl(
        &mut self,
        code: Vec<OpCode>,
        funcs: Vec<Vec<OpCode>>,
        string_pool: Vec<String>,
        script_args: Vec<String>,
        mut report: Option<&mut crate::profiler::ProfileReport>,
    ) -> Result<(), String> {
        self.functions = funcs;
        self.string_pool_heap = string_pool.into_iter()
            .map(|s| self.heap.alloc(HeapObject::String(s)))
            .collect();

        let args_array: Vec<Value> = script_args.into_iter().map(|s| {
            let idx = self.heap.alloc(HeapObject::String(s));
            Value::string(idx)
        }).collect();
        let args_idx = self.heap.alloc(HeapObject::Array(args_array));
        self.globals = vec![Value::array(args_idx)];

        let silent = self.silent;
        let Self {
            stack, locals, globals, frames, functions, functions_by_name,
            call_stack, closure_stack, handlers, string_pool_heap, heap,
            locals_pool, silent: _,
        } = self;

        // Precompute Rc-wrapped code arrays once. Rc::clone is O(1) instead
        // of O(bytecode_len) per Call/TryStart/Return — eliminates the
        // dominant allocation source during deep call stacks.
        let functions_rc: Vec<Rc<Vec<OpCode>>> = functions.drain(..).map(Rc::new).collect();
        let main_code_rc: Rc<Vec<OpCode>> = Rc::new(code);

        let mut ip: usize = 0;
        let mut current_code: Rc<Vec<OpCode>> = Rc::clone(&main_code_rc);

        // FIX: Include frames, handlers, closure_stack, and string_pool_heap in roots
        macro_rules! roots { () => {{
            let mut r = Vec::new();
            r.extend_from_slice(stack);
            r.extend_from_slice(locals);
            r.extend_from_slice(globals);
            for frame in frames.iter() {
                r.extend_from_slice(frame);
            }
            for h in handlers.iter() {
                r.extend_from_slice(&h.stack);
                r.extend_from_slice(&h.locals);
                for hf in h.frames.iter() { r.extend_from_slice(hf); }
            }
            for c in closure_stack.iter() {
                if let Some((cap, _, _)) = c {
                    let vals: Vec<Value> = cap.borrow().clone();
                    for v in vals { r.push(v); }
                }
            }
            for &idx in string_pool_heap.iter() {
                r.push(Value::string(idx));
            }
            r
        }}}

        while ip < current_code.len() {
            let op = unsafe { current_code.get_unchecked(ip) };
            if let Some(r) = report.as_deref_mut() {
                r.on_instruction(op);
            }
            match op {
                OpCode::ConstantNumber(n) => stack.push(Value::number(*n)),
                OpCode::ConstantBool(b) => stack.push(Value::bool(*b)),
                OpCode::ConstantNull => stack.push(Value::null()),
                OpCode::StringConst(idx) => {
                    let heap_idx = string_pool_heap[*idx as usize];
                    stack.push(Value::string(heap_idx));
                }

                OpCode::Add => {
                    let b = stack.pop().unwrap_or(Value::null());
                    let a = stack.pop().unwrap_or(Value::null());
                    if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { stack.push(Value::number(x + y)); }
                    else if a.is_string() && b.is_string() {
                        let mut s = heap.get_string(a.payload()).clone();
                        s.push_str(heap.get_string(b.payload()));
                        let i = heap.alloc(HeapObject::String(s));
                        stack.push(Value::string(i));
                    } else { stack.push(Value::null()); }
                }
                OpCode::Sub => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { stack.push(Value::number(x - y)); } else { stack.push(Value::null()); } }
                OpCode::Mul => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { stack.push(Value::number(x * y)); } else { stack.push(Value::null()); } }
                OpCode::Div => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { stack.push(Value::number(x / y)); } else { stack.push(Value::null()); } }
                OpCode::Mod => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); match (a.as_number(), b.as_number()) { (Some(x), Some(y)) if y != 0.0 => stack.push(Value::number(x % y)), _ => stack.push(Value::null()) } }
                OpCode::Eq => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); let eq = if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { x == y } else if a.is_string() && b.is_string() { heap.get_string(a.payload()) == heap.get_string(b.payload()) } else if a.is_bool() && b.is_bool() { a.as_bool() == b.as_bool() } else { false }; stack.push(Value::bool(eq)); }
                OpCode::NotEq => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); let ne = if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { x != y } else if a.is_string() && b.is_string() { heap.get_string(a.payload()) != heap.get_string(b.payload()) } else if a.is_bool() && b.is_bool() { a.as_bool() != b.as_bool() } else { true }; stack.push(Value::bool(ne)); }
                OpCode::Lt => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); let r = if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { x < y } else if a.is_string() && b.is_string() { heap.get_string(a.payload()) < heap.get_string(b.payload()) } else { false }; stack.push(Value::bool(r)); }
                OpCode::Gt => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); let r = if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { x > y } else if a.is_string() && b.is_string() { heap.get_string(a.payload()) > heap.get_string(b.payload()) } else { false }; stack.push(Value::bool(r)); }
                OpCode::LtEq => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); let r = if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { x <= y } else if a.is_string() && b.is_string() { heap.get_string(a.payload()) <= heap.get_string(b.payload()) } else { false }; stack.push(Value::bool(r)); }
                OpCode::GtEq => { let b = stack.pop().unwrap_or(Value::null()); let a = stack.pop().unwrap_or(Value::null()); let r = if let (Some(x), Some(y)) = (a.as_number(), b.as_number()) { x >= y } else if a.is_string() && b.is_string() { heap.get_string(a.payload()) >= heap.get_string(b.payload()) } else { false }; stack.push(Value::bool(r)); }
                OpCode::Negate => { let v = stack.pop().unwrap_or(Value::null()); if let Some(n) = v.as_number() { stack.push(Value::number(-n)); } else { stack.push(Value::null()); } }
                OpCode::Not => { let v = stack.pop().unwrap_or(Value::null()); if v.is_bool() { stack.push(Value::bool(!v.as_bool())); } else { stack.push(Value::null()); } }
                OpCode::Print => {
                    let v = stack.pop().unwrap_or(Value::null());
                    if !silent { println!("{}", v.to_string_with_heap(heap)); }
                }

                OpCode::BinLocals(op, a, b) => {
                    let lv = unsafe { *locals.get_unchecked(*a as usize) };
                    let rv = unsafe { *locals.get_unchecked(*b as usize) };
                    let res = if let (Some(x), Some(y)) = (lv.as_number(), rv.as_number()) {
                        match op {
                            TokenType::Plus => Value::number(x + y), TokenType::Minus => Value::number(x - y),
                            TokenType::Star => Value::number(x * y), TokenType::Slash => Value::number(x / y),
                            TokenType::Percent => Value::number(x % y),
                            TokenType::EqEq => Value::bool(x == y), TokenType::NotEq => Value::bool(x != y),
                            TokenType::Lt => Value::bool(x < y), TokenType::Gt => Value::bool(x > y),
                            TokenType::LtEq => Value::bool(x <= y), TokenType::GtEq => Value::bool(x >= y),
                            _ => Value::null(),
                        }
                    } else if lv.is_string() && rv.is_string() {
                        match op {
                            TokenType::Plus => { let mut s = heap.get_string(lv.payload()).clone(); s.push_str(heap.get_string(rv.payload())); let i = heap.alloc(HeapObject::String(s)); Value::string(i) }
                            TokenType::EqEq => Value::bool(heap.get_string(lv.payload()) == heap.get_string(rv.payload())),
                            TokenType::NotEq => Value::bool(heap.get_string(lv.payload()) != heap.get_string(rv.payload())),
                            TokenType::Lt => Value::bool(heap.get_string(lv.payload()) < heap.get_string(rv.payload())),
                            TokenType::Gt => Value::bool(heap.get_string(lv.payload()) > heap.get_string(rv.payload())),
                            TokenType::LtEq => Value::bool(heap.get_string(lv.payload()) <= heap.get_string(rv.payload())),
                            TokenType::GtEq => Value::bool(heap.get_string(lv.payload()) >= heap.get_string(rv.payload())),
                            _ => Value::null(),
                        }
                    } else if lv.is_bool() && rv.is_bool() {
                        match op {
                            TokenType::EqEq => Value::bool(lv.as_bool() == rv.as_bool()),
                            TokenType::NotEq => Value::bool(lv.as_bool() != rv.as_bool()),
                            _ => Value::null(),
                        }
                    } else { Value::null() };
                    stack.push(res);
                }
                OpCode::IncLocal(slot, delta) => {
                    let v = unsafe { locals.get_unchecked_mut(*slot as usize) };
                    if let Some(n) = v.as_number() { *v = Value::number(n + delta); }
                }
                OpCode::AddLocalConst(a, k, b) => {
                    let v = unsafe { *locals.get_unchecked(*a as usize) };
                    let res = if let Some(n) = v.as_number() { Value::number(n + k) } else { Value::null() };
                    let bi = *b as usize;
                    if bi >= locals.len() { locals.resize(bi + 1, Value::null()); }
                    locals[bi] = res;
                }
                OpCode::JumpIfLocalCmpFalse(op, a, b, target) => {
                    let lv = unsafe { *locals.get_unchecked(*a as usize) };
                    let rv = unsafe { *locals.get_unchecked(*b as usize) };
                    let cond = if let (Some(x), Some(y)) = (lv.as_number(), rv.as_number()) {
                        match op { TokenType::Lt => x < y, TokenType::LtEq => x <= y, TokenType::Gt => x > y, TokenType::GtEq => x >= y, TokenType::EqEq => x == y, TokenType::NotEq => x != y, _ => false }
                    } else if lv.is_string() && rv.is_string() {
                        match op { TokenType::EqEq => heap.get_string(lv.payload()) == heap.get_string(rv.payload()), TokenType::NotEq => heap.get_string(lv.payload()) != heap.get_string(rv.payload()), TokenType::Lt => heap.get_string(lv.payload()) < heap.get_string(rv.payload()), TokenType::Gt => heap.get_string(lv.payload()) > heap.get_string(rv.payload()), TokenType::LtEq => heap.get_string(lv.payload()) <= heap.get_string(rv.payload()), TokenType::GtEq => heap.get_string(lv.payload()) >= heap.get_string(rv.payload()), _ => false }
                    } else if lv.is_bool() && rv.is_bool() {
                        match op { TokenType::EqEq => lv.as_bool() == rv.as_bool(), TokenType::NotEq => lv.as_bool() != rv.as_bool(), _ => false }
                    } else { false };
                    if !cond { ip = *target; continue; }
                }
                OpCode::AddLocalMulLocal(dst, a, b) => {
                    let ai = *a as usize; let bi = *b as usize; let di = *dst as usize;
                    let la = unsafe { *locals.get_unchecked(ai) };
                    let lb = unsafe { *locals.get_unchecked(bi) };
                    let ld = unsafe { *locals.get_unchecked(di) };
                    let prod = match (la.as_number(), lb.as_number()) { (Some(x), Some(y)) => Some(x * y), _ => None };
                    let sum = match (ld.as_number(), prod) {
                        (Some(x), Some(y)) => Value::number(x + y),
                        _ => Value::null(),
                    };
                    locals[di] = sum;
                }
                OpCode::IncLocalJump(slot, delta, target) => {
                    let v = unsafe { locals.get_unchecked_mut(*slot as usize) };
                    if let Some(n) = v.as_number() { *v = Value::number(n + delta); }
                    ip = *target; continue;
                }
                OpCode::JumpIfLocalConstCmpFalse(op, slot, k, target) => {
                    let v = unsafe { *locals.get_unchecked(*slot as usize) };
                    let cond = if let Some(x) = v.as_number() {
                        match op { TokenType::Lt => x < *k, TokenType::LtEq => x <= *k, TokenType::Gt => x > *k, TokenType::GtEq => x >= *k, TokenType::EqEq => x == *k, TokenType::NotEq => x != *k, _ => false }
                    } else { false };
                    if !cond { ip = *target; continue; }
                }

                OpCode::Interpolate(n) => {
                    let mut parts: Vec<Value> = Vec::with_capacity(*n);
                    for _ in 0..*n { parts.push(stack.pop().unwrap_or(Value::null())); }
                    parts.reverse();
                    let mut s = String::new();
                    for v in parts { s.push_str(&v.to_string_with_heap(heap)); }
                    let i = heap.alloc(HeapObject::String(s));
                    stack.push(Value::string(i));
                }

                OpCode::MakeClosure(func_idx, captured_slots, np, nl) => {
                    let captured: Vec<Value> = captured_slots.iter().map(|&s| {
                        if s == usize::MAX { Value::null() } else { locals.get(s).copied().unwrap_or(Value::null()) }
                    }).collect();
                    let idx = heap.alloc(HeapObject::Closure {
                        func_idx: *func_idx as u32, num_params: *np as u32, num_locals: *nl as u32,
                        captured: Rc::new(RefCell::new(captured)),
                    });
                    stack.push(Value::closure(idx));
                }

                OpCode::DefineFunction(name, idx, slot) => {
                    let heap_idx = heap.alloc(HeapObject::CodeFunction(*idx as u32));
                    if locals.len() <= *slot { locals.resize(*slot + 1, Value::null()); }
                    locals[*slot] = Value::function(heap_idx);
                    functions_by_name.insert(name.clone(), *idx as u32);
                }
                OpCode::DefineFunctionGlobal(name, idx, local_slot, global_slot) => {
                    let heap_idx = heap.alloc(HeapObject::CodeFunction(*idx as u32));
                    if locals.len() <= *local_slot { locals.resize(*local_slot + 1, Value::null()); }
                    locals[*local_slot] = Value::function(heap_idx);
                    if globals.len() <= *global_slot as usize { globals.resize(*global_slot as usize + 1, Value::null()); }
                    globals[*global_slot as usize] = Value::function(heap_idx);
                    functions_by_name.insert(name.clone(), *idx as u32);
                }
                OpCode::GetGlobal(slot) => {
                    let v = globals.get(*slot as usize).copied().unwrap_or(Value::null());
                    stack.push(v);
                }
                OpCode::SetGlobal(slot) => {
                    let val = stack.pop().unwrap_or(Value::null());
                    if globals.len() <= *slot as usize { globals.resize(*slot as usize + 1, Value::null()); }
                    globals[*slot as usize] = val;
                }

                OpCode::Builtin(name, arg_count) => {
                    if heap.needs_gc() { let r = roots!(); heap.collect_garbage(&r); }
                    let mut args = Vec::with_capacity(*arg_count);
                    for _ in 0..*arg_count { args.push(stack.pop().unwrap_or(Value::null())); }
                    let res = builtin_impl(heap, name, args)?;
                    stack.push(res);
                }

                OpCode::Dup => { let top = stack.last().copied().unwrap_or(Value::null()); stack.push(top); }
                OpCode::Pop => { stack.pop(); }
                OpCode::SetLocal(slot) => {
                    let val = stack.pop().unwrap_or(Value::null());
                    if locals.len() <= *slot { locals.resize(*slot + 1, Value::null()); }
                    locals[*slot] = val;
                }
                OpCode::GetLocal(slot) => { let v = locals.get(*slot).copied().unwrap_or(Value::null()); stack.push(v); }
                OpCode::Jump(target) => { ip = *target; continue; }
                OpCode::JumpIfFalse(target) => {
                    let val = stack.pop().unwrap_or(Value::null());
                    if !val.is_truthy() { ip = *target; continue; }
                }
                OpCode::JumpIfTrue(target) => {
                    let val = stack.pop().unwrap_or(Value::null());
                    if val.is_truthy() { ip = *target; continue; }
                }

                OpCode::MethodCall(method, arg_count) => {
                    let mut args = Vec::new();
                    for _ in 0..*arg_count { args.push(stack.pop().unwrap_or(Value::null())); }
                    let target = stack.pop().unwrap_or(Value::null());
                    if target.is_struct() {
                        let (name, _) = heap.get_struct(target.payload());
                        let full_name = format!("{}.{}", name, method);
                        if let Some(&func_idx) = functions_by_name.get(&full_name) {
                            call_stack.push((ip, handlers.len(), Rc::clone(&current_code)));
                            closure_stack.push(None);
                            let mut new_frame = locals_pool.pop().unwrap_or_default();
                            new_frame.clear();
                            new_frame.push(target);
                            new_frame.extend(args);
                            frames.push(std::mem::replace(locals, new_frame));
                            current_code = Rc::clone(&functions_rc[func_idx as usize]);
                            ip = 0; continue;
                        }
                    }
                }

                OpCode::Call(arg_count) => {
                    let func_val = stack.pop().unwrap_or(Value::null());
                    if func_val.is_function() {
                        let heap_obj = heap.get(func_val.payload()).clone();
                        if let HeapObject::CodeFunction(code_idx) = heap_obj {
                            call_stack.push((ip, handlers.len(), Rc::clone(&current_code)));
                            closure_stack.push(None);
                            let mut new_frame = locals_pool.pop().unwrap_or_default();
                            new_frame.clear();
                            for _ in 0..*arg_count { new_frame.push(stack.pop().unwrap_or(Value::null())); }
                            frames.push(std::mem::replace(locals, new_frame));
                            if let Some(r) = report.as_deref_mut() {
                                let mut name = format!("fn#{}", code_idx);
                                for (n, idx) in functions_by_name.iter() {
                                    if *idx == code_idx { name = n.clone(); break; }
                                }
                                r.push_frame(name);
                            }
                            current_code = Rc::clone(&functions_rc[code_idx as usize]);
                            ip = 0; continue;
                        }
                    } else if func_val.is_closure() {
                        let heap_obj = heap.get(func_val.payload()).clone();
                        if let HeapObject::Closure { func_idx, num_params, num_locals, captured } = heap_obj {
                            let cap_snapshot: Vec<Value> = { let b = captured.borrow(); b.iter().copied().collect() };
                            call_stack.push((ip, handlers.len(), Rc::clone(&current_code)));
                            closure_stack.push(Some((captured.clone(), num_params as usize, num_locals as usize)));
                            frames.push(std::mem::replace(locals, Vec::new()));
                            let mut new_locals: Vec<Value> = Vec::with_capacity(num_params as usize + num_locals as usize + cap_snapshot.len());
                            for _ in 0..*arg_count { new_locals.push(stack.pop().unwrap_or(Value::null())); }
                            new_locals.resize(num_params as usize + num_locals as usize, Value::null());
                            new_locals.extend(cap_snapshot);
                            *locals = new_locals;
                            if let Some(r) = report.as_deref_mut() {
                                r.push_frame(format!("closure#{}", func_idx));
                            }
                            current_code = Rc::clone(&functions_rc[func_idx as usize]);
                            ip = 0; continue;
                        }
                    }
                }
                OpCode::CallGlobal(gslot, arg_count) => {
                    let func_val = globals.get(*gslot as usize).copied().unwrap_or(Value::null());
                    if func_val.is_function() {
                        let heap_obj = heap.get(func_val.payload()).clone();
                        if let HeapObject::CodeFunction(code_idx) = heap_obj {
                            call_stack.push((ip, handlers.len(), Rc::clone(&current_code)));
                            closure_stack.push(None);
                            let mut new_frame = locals_pool.pop().unwrap_or_default();
                            new_frame.clear();
                            for _ in 0..*arg_count { new_frame.push(stack.pop().unwrap_or(Value::null())); }
                            frames.push(std::mem::replace(locals, new_frame));
                            if let Some(r) = report.as_deref_mut() {
                                let mut name = format!("fn#{}", code_idx);
                                for (n, idx) in functions_by_name.iter() {
                                    if *idx == code_idx { name = n.clone(); break; }
                                }
                                r.push_frame(name);
                            }
                            current_code = Rc::clone(&functions_rc[code_idx as usize]);
                            ip = 0; continue;
                        }
                    } else if func_val.is_closure() {
                        let heap_obj = heap.get(func_val.payload()).clone();
                        if let HeapObject::Closure { func_idx, num_params, num_locals, captured } = heap_obj {
                            let cap_snapshot: Vec<Value> = { let b = captured.borrow(); b.iter().copied().collect() };
                            call_stack.push((ip, handlers.len(), Rc::clone(&current_code)));
                            closure_stack.push(Some((captured.clone(), num_params as usize, num_locals as usize)));
                            let mut new_frame = locals_pool.pop().unwrap_or_default();
                            new_frame.clear();
                            for _ in 0..*arg_count { new_frame.push(stack.pop().unwrap_or(Value::null())); }
                            new_frame.resize(num_params as usize + num_locals as usize, Value::null());
                            new_frame.extend(cap_snapshot);
                            frames.push(std::mem::replace(locals, new_frame));
                            if let Some(r) = report.as_deref_mut() {
                                r.push_frame(format!("closure#{}", func_idx));
                            }
                            current_code = Rc::clone(&functions_rc[func_idx as usize]);
                            ip = 0; continue;
                        }
                    }
                }
                OpCode::PushLocalAddConst(slot, k) => {
                    let v = locals.get(*slot as usize).copied().unwrap_or(Value::null());
                    if let Some(n) = v.as_number() { stack.push(Value::number(n + *k)); }
                    else { stack.push(Value::null()); }
                }
                OpCode::PushLocalSubConst(slot, k) => {
                    let v = locals.get(*slot as usize).copied().unwrap_or(Value::null());
                    if let Some(n) = v.as_number() { stack.push(Value::number(n - *k)); }
                    else { stack.push(Value::null()); }
                }

                OpCode::Return => {
                    let ret_val = stack.pop().unwrap_or(Value::null());
                    if let Some((ret_ip, handlers_len, caller_code)) = call_stack.pop() {
                        if let Some(r) = report.as_deref_mut() {
                            r.pop_frame();
                        }
                        handlers.truncate(handlers_len);
                        if let Some(Some((cap, np, nl))) = closure_stack.pop() {
                            let base = np + nl;
                            let mut b = cap.borrow_mut();
                            for i in 0..b.len() {
                                if base + i < locals.len() { b[i] = locals[base + i]; }
                            }
                        }
                        let old_locals = std::mem::replace(locals, frames.pop().unwrap_or_default());
                        locals_pool.push(old_locals); // Return the used frame to the pool
                        stack.push(ret_val);
                        current_code = caller_code;
                        ip = ret_ip;
                    } else { break; }
                }

                OpCode::TryStart(handler_ip) => {
                    handlers.push(Handler {
                        ip: *handler_ip,
                        current_code: current_code.clone(),
                        locals: locals.clone(), frames: frames.clone(),
                        call_stack: call_stack.clone(), closure_stack: closure_stack.clone(), stack: stack.clone(),
                    });
                }
                OpCode::TryEnd => { handlers.pop(); }
                OpCode::Throw => {
                    let exc = stack.pop().unwrap_or(Value::null());
                    if let Some(h) = handlers.pop() {
                        *locals = h.locals; *frames = h.frames; *call_stack = h.call_stack; *closure_stack = h.closure_stack;
                        *stack = h.stack; stack.push(exc);
                        current_code = h.current_code; ip = h.ip; continue;
                    } else {
                        return Err(format!("Uncaught exception: {}", exc.to_string_with_heap(heap)));
                    }
                }

                OpCode::Array(size) => {
                    if heap.needs_gc() { let r = roots!(); heap.collect_garbage(&r); }
                    let mut elements = Vec::with_capacity(*size);
                    for _ in 0..*size { elements.push(stack.pop().unwrap_or(Value::null())); }
                    elements.reverse();
                    let idx = heap.alloc(HeapObject::Array(elements));
                    stack.push(Value::array(idx));
                }
                OpCode::Map(size) => {
                    if heap.needs_gc() { let r = roots!(); heap.collect_garbage(&r); }
                    let mut map = HashMap::new();
                    for _ in 0..*size {
                        let v = stack.pop().unwrap_or(Value::null());
                        let k = stack.pop().unwrap_or(Value::null());
                        if let Ok(key) = MapKey::from_value(k, heap) { map.insert(key, v); }
                    }
                    let idx = heap.alloc(HeapObject::Map(map));
                    stack.push(Value::map(idx));
                }
                OpCode::Struct(name, fields) => {
                    if heap.needs_gc() { let r = roots!(); heap.collect_garbage(&r); }
                    let mut map = HashMap::new();
                    for f in fields.iter().rev() {
                        let v = stack.pop().unwrap_or(Value::null());
                        map.insert(f.clone(), v);
                    }
                    let idx = heap.alloc(HeapObject::Struct(name.clone(), map));
                    stack.push(Value::struct_val(idx));
                }
                OpCode::FieldAccess(field) => {
                    let target = stack.pop().unwrap_or(Value::null());
                    if target.is_struct() {
                        let (_, map) = heap.get_struct(target.payload());
                        stack.push(map.get(field).copied().unwrap_or(Value::null()));
                    } else { stack.push(Value::null()); }
                }
                OpCode::FieldSet(field) => {
                    let value = stack.pop().unwrap_or(Value::null());
                    let target = stack.pop().unwrap_or(Value::null());
                    if target.is_struct() {
                        heap.get_struct_mut(target.payload()).insert(field.clone(), value);
                    }
                }
                OpCode::Index => {
                    let iv = stack.pop().unwrap_or(Value::null());
                    let t = stack.pop().unwrap_or(Value::null());
                    match t.kind() {
                        ValueKind::Array(idx) => {
                            if let Some(n) = iv.as_number() {
                                let arr = heap.get_array(idx);
                                let i = n as usize;
                                if i < arr.len() { stack.push(arr[i]); } else { stack.push(Value::null()); }
                            } else { stack.push(Value::null()); }
                        }
                        ValueKind::Map(idx) => {
                            if let Ok(key) = MapKey::from_value(iv, heap) {
                                if let Some(v) = heap.get_map(idx).get(&key).copied() { stack.push(v); } else { stack.push(Value::null()); }
                            } else { stack.push(Value::null()); }
                        }
                        ValueKind::Struct(idx) => {
                            if iv.is_string() {
                                let k = heap.get_string(iv.payload()).clone();
                                let (_, map) = heap.get_struct(idx);
                                if let Some(v) = map.get(&k).copied() { stack.push(v); } else { stack.push(Value::null()); }
                            } else { stack.push(Value::null()); }
                        }
                        _ => stack.push(Value::null()),
                    }
                }
                OpCode::IndexSet => {
                    let value = stack.pop().unwrap_or(Value::null());
                    let iv = stack.pop().unwrap_or(Value::null());
                    let t = stack.pop().unwrap_or(Value::null());
                    match t.kind() {
                        ValueKind::Array(idx) => {
                            if let Some(n) = iv.as_number() {
                                let arr = heap.get_array_mut(idx);
                                let i = n as usize;
                                if i < arr.len() { arr[i] = value; }
                            }
                        }
                        ValueKind::Map(idx) => {
                            if let Ok(key) = MapKey::from_value(iv, heap) { heap.get_map_mut(idx).insert(key, value); }
                        }
                        ValueKind::Struct(idx) => {
                            if iv.is_string() {
                                let k = heap.get_string(iv.payload()).clone();
                                heap.get_struct_mut(idx).insert(k, value);
                            }
                        }
                        _ => {}
                    }
                }
                OpCode::Halt => break,
            }
            ip += 1;
        }
        Ok(())
    }
}

// ===================== BYTECODE SERIALIZATION =====================

#[derive(serde::Serialize, serde::Deserialize)]
pub struct CompiledBundle {
    pub version: u32,
    pub code: Vec<OpCode>,
    pub funcs: Vec<Vec<OpCode>>,
    pub string_pool: Vec<String>,
}

pub fn save_bytecode(
    path: &str,
    code: Vec<OpCode>,
    funcs: Vec<Vec<OpCode>>,
    string_pool: Vec<String>,
) -> Result<(), String> {
    let bundle = CompiledBundle { version: 1, code, funcs, string_pool };
    let json = serde_json::to_string_pretty(&bundle).map_err(|e| e.to_string())?;
    std::fs::write(path, json).map_err(|e| e.to_string())
}

pub fn load_bytecode(path: &str) -> Result<CompiledBundle, String> {
    let data = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let bundle: CompiledBundle = serde_json::from_str(&data).map_err(|e| e.to_string())?;
    if bundle.version != 1 {
        return Err(format!("unsupported bytecode version: {} (expected 1)", bundle.version));
    }
    Ok(bundle)
}

// ===================== BINARY BYTECODE =====================

const BIN_MAGIC: u8 = 0xBC;

pub fn save_bytecode_bin(
    path: &str,
    code: Vec<OpCode>,
    funcs: Vec<Vec<OpCode>>,
    string_pool: Vec<String>,
) -> Result<(), String> {
    let bundle = CompiledBundle { version: 1, code, funcs, string_pool };
    let mut bytes = Vec::with_capacity(256);
    bytes.push(BIN_MAGIC);
    let payload = bincode::serialize(&bundle).map_err(|e| e.to_string())?;
    bytes.extend(payload);
    std::fs::write(path, bytes).map_err(|e| e.to_string())
}

/// Loads either binary (`0xBC` magic) or JSON (backward compat) bytecode.
pub fn load_bytecode_auto(path: &str) -> Result<CompiledBundle, String> {
    let bytes = std::fs::read(path).map_err(|e| e.to_string())?;
    if bytes.is_empty() {
        return Err("empty bytecode file".to_string());
    }
    if bytes[0] == BIN_MAGIC {
        let bundle: CompiledBundle = bincode::deserialize(&bytes[1..])
            .map_err(|e| format!("corrupt binary bytecode: {}", e))?;
        if bundle.version != 1 {
            return Err(format!("unsupported bytecode version: {}", bundle.version));
        }
        Ok(bundle)
    } else {
        // Legacy JSON format
        let bundle: CompiledBundle = serde_json::from_slice(&bytes)
            .map_err(|e| format!("parse: {}", e))?;
        if bundle.version != 1 {
            return Err(format!("unsupported bytecode version: {}", bundle.version));
        }
        Ok(bundle)
    }
}