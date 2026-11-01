use crate::ast::{Expr, Stmt};
use crate::lexer::{TokenType, Lexer};
use crate::parser::Parser;
use crate::heap::{Value, MapKey, Heap, HeapObject};
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

/// `repr(C, u8)`: 1-byte discriminant at offset 0, C-compatible field layout.
/// Makes `match op { ... }` compile to a stable byte load + jump table.
#[repr(C, u8)]
#[derive(Debug, Clone)]
pub enum OpCode {
    Constant(Value), Add, Sub, Mul, Div, Eq, NotEq, Lt, Gt, LtEq, GtEq,
    Negate, Not,
    Print, Jump(usize), JumpIfFalse(usize), GetLocal(usize), SetLocal(usize),
    DefineFunction(String, usize, usize),
    Call(usize), Builtin(String, usize), MethodCall(String, usize),
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
            OpCode::Jump(t) | OpCode::JumpIfFalse(t) | OpCode::TryStart(t)
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
        let clean = |i: usize, n: usize| -> bool {
            !(1..n).any(|j| i + j < code.len() && targets.contains(&(i + j)))
        };
        if i + 2 < code.len() && clean(i, 3) {
            if let (OpCode::GetLocal(a), OpCode::GetLocal(b)) = (&code[i], &code[i + 1]) {
                if let Some(tok) = bin_op_of(&code[i + 2]) {
                    new_code.push(OpCode::BinLocals(tok, *a as u32, *b as u32));
                    i += 3; continue;
                }
            }
        }
        if i + 1 < code.len() && clean(i, 2) {
            if let OpCode::BinLocals(op, a, b) = &code[i] {
                if is_comparison(op) {
                    if let OpCode::JumpIfFalse(target) = &code[i + 1] {
                        new_code.push(OpCode::JumpIfLocalCmpFalse(op.clone(), *a, *b, *target));
                        i += 2; continue;
                    }
                }
            }
        }
        if i + 3 < code.len() && clean(i, 4) {
            if let (OpCode::GetLocal(dst), OpCode::BinLocals(TokenType::Star, a, b), OpCode::Add, OpCode::SetLocal(dst2)) =
                (&code[i], &code[i + 1], &code[i + 2], &code[i + 3])
            {
                if dst == dst2 {
                    new_code.push(OpCode::AddLocalMulLocal(*dst as u32, *a as u32, *b as u32));
                    i += 4; continue;
                }
            }
        }
        if i + 1 < code.len() && clean(i, 2) {
            if let (OpCode::IncLocal(slot, delta), OpCode::Jump(target)) = (&code[i], &code[i + 1]) {
                new_code.push(OpCode::IncLocalJump(*slot, *delta, *target));
                i += 2; continue;
            }
        }
        if i + 3 < code.len() && clean(i, 4) {
            if let (OpCode::GetLocal(a), OpCode::Constant(Value::Number(k)), OpCode::SetLocal(b)) =
                (&code[i], &code[i + 1], &code[i + 3])
            {
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
            if let (OpCode::GetLocal(a), OpCode::Constant(Value::Number(k)), OpCode::SetLocal(b)) =
                (&code[i], &code[i + 1], &code[i + 3])
            {
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
            if let (OpCode::GetLocal(a), OpCode::Constant(Value::Number(k))) = (&code[i], &code[i + 1]) {
                if let Some(cmp) = cmp_op_of(&code[i + 2]) {
                    if let OpCode::JumpIfFalse(target) = &code[i + 3] {
                        new_code.push(OpCode::JumpIfLocalConstCmpFalse(cmp, *a as u32, *k, *target));
                        i += 4; continue;
                    }
                }
            }
        }
        new_code.push(code[i].clone());
        i += 1;
    }
    old_to_new[code.len()] = new_code.len();
    for op in new_code.iter_mut() {
        match op {
            OpCode::Jump(t) | OpCode::JumpIfFalse(t) | OpCode::TryStart(t) => { if *t <= code.len() { *t = old_to_new[*t]; } }
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
}

impl Compiler {
    pub fn new() -> Self {
        Compiler {
            code: Vec::new(), locals: HashMap::new(), next_local: 0, num_params: 0,
            upvalues: Rc::new(RefCell::new(UpvalueState::default())),
            captured_names: Rc::new(RefCell::new(Vec::new())),
            enclosing: None, loop_stack: Vec::new(),
            functions: Rc::new(RefCell::new(Vec::new())),
        }
    }

    pub fn compile(mut self, stmts: Vec<Stmt>) -> (Vec<OpCode>, Vec<Vec<OpCode>>) {
        for stmt in stmts { self.compile_stmt(stmt); }
        self.code.push(PendingOp::Ready(OpCode::Halt));
        let main_raw = resolve_pending(std::mem::take(&mut self.code), self.next_local);
        let before_main = main_raw.len();
        let main = peephole(main_raw);
        if std::env::var("DONKY_DEBUG").is_ok() {
            eprintln!("[PEEPHOLE] main {} -> {}", before_main, main.len());
        }
        let funcs_snapshot: Vec<Vec<OpCode>> = {
            let borrow = self.functions.borrow();
            borrow.iter().map(|f| f.clone()).collect()
        };
        let funcs: Vec<Vec<OpCode>> = funcs_snapshot.into_iter().map(|f| {
            let before = f.len();
            let after = peephole(f);
            if std::env::var("DONKY_DEBUG").is_ok() {
                eprintln!("[PEEPHOLE] func {} -> {}", before, after.len());
            }
            after
        }).collect();
        (main, funcs)
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
        let np = sub.num_params;
        let nl = final_next_local - np;
        (func_idx, captured, np, nl)
    }

    fn resolve_parent_slot(&mut self, name: &str) -> Option<CapturedSlot> {
        if let Some(&slot) = self.locals.get(name) {
            return Some(CapturedSlot::Immediate(slot));
        }
        let upval_idx: Option<usize> = self.upvalues.borrow().map.get(name).copied();
        if let Some(idx) = upval_idx {
            return Some(CapturedSlot::ParentUpvalue(idx));
        }
        let enc = self.enclosing.clone()?;
        Self::ensure_enclosing_captures(&enc, name)?;
        let idx = {
            let mut u = self.upvalues.borrow_mut();
            if let Some(&i) = u.map.get(name) { i } else {
                let i = u.map.len();
                u.map.insert(name.to_string(), i);
                u.list.push(name.to_string());
                i
            }
        };
        self.captured_names.borrow_mut().push(name.to_string());
        Some(CapturedSlot::ParentUpvalue(idx))
    }

    fn ensure_enclosing_captures(enclosing: &Rc<RefCell<EnclosingSnapshot>>, name: &str) -> Option<()> {
        let (has_local, has_upval, grand, enc_upv, enc_caps) = {
            let e = enclosing.borrow();
            let has_local = e.locals.contains_key(name);
            let has_upval = { e.upvalues.borrow().map.contains_key(name) };
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
            let idx = u.map.len();
            u.map.insert(name.to_string(), idx);
            u.list.push(name.to_string());
            drop(u);
            enc_caps.borrow_mut().push(name.to_string());
        }
        Some(())
    }

    fn compile_stmt(&mut self, stmt: Stmt) {
        match stmt {
            Stmt::Asm(_) => {}
            Stmt::Import(path) => {
                let content = match fs::read_to_string(&path) { Ok(c) => c, Err(_) => return };
                let mut lex = Lexer::new(&content);
                let tokens = match lex.tokenize() { Ok(t) => t, Err(_) => return };
                let mut parser = Parser::new(tokens);
                let imported = match parser.parse() { Ok(a) => a, Err(_) => return };
                for s in imported { self.compile_stmt(s); }
            }
            Stmt::Block(stmts) => {
                let saved = self.locals.clone();
                for s in stmts { self.compile_stmt(s); }
                self.locals = saved;
            }
            Stmt::Let(name, expr) => {
                self.compile_expr(expr);
                let slot = self.next_local;
                self.locals.insert(name, slot);
                self.next_local += 1;
                self.code.push(PendingOp::Ready(OpCode::SetLocal(slot)));
            }
            Stmt::Print(expr) => { self.compile_expr(expr); self.code.push(PendingOp::Ready(OpCode::Print)); }
            Stmt::Assign(name, expr) => {
                self.compile_expr(expr);
                if let Some(&slot) = self.locals.get(&name) {
                    self.code.push(PendingOp::Ready(OpCode::SetLocal(slot)));
                } else {
                    let upval_idx: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = upval_idx {
                        self.code.push(PendingOp::SetUpvalue(idx));
                    }
                }
            }
            Stmt::FieldSet(obj_expr, field, val_expr) => {
                self.compile_expr(obj_expr); self.compile_expr(val_expr);
                self.code.push(PendingOp::Ready(OpCode::FieldSet(field)));
            }
            Stmt::IndexSet(name, idx_expr, val_expr) => {
                if let Some(&slot) = self.locals.get(&name) {
                    self.code.push(PendingOp::Ready(OpCode::GetLocal(slot)));
                } else {
                    let upval_idx: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = upval_idx {
                        self.code.push(PendingOp::GetUpvalue(idx));
                    } else {
                        self.code.push(PendingOp::Ready(OpCode::Constant(Value::Null)));
                    }
                }
                self.compile_expr(idx_expr); self.compile_expr(val_expr);
                self.code.push(PendingOp::Ready(OpCode::IndexSet));
            }
            Stmt::ExprStmt(expr) => { self.compile_expr(expr); self.code.push(PendingOp::Ready(OpCode::Pop)); }
            Stmt::If(cond, then_b, else_b) => {
                self.compile_expr(cond);
                let jif = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                self.compile_stmt(*then_b);
                if let Some(eb) = else_b {
                    let jo = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                    let et = self.code.len();
                    self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(et));
                    self.compile_stmt(*eb);
                    let end = self.code.len();
                    self.code[jo] = PendingOp::Ready(OpCode::Jump(end));
                } else {
                    let end = self.code.len();
                    self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(end));
                }
            }
            Stmt::While(cond, body) => {
                let ls = self.code.len();
                self.compile_expr(cond);
                let jif = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
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
                self.code.push(PendingOp::Ready(OpCode::Constant(Value::Number(0.0))));
                self.code.push(PendingOp::Ready(OpCode::SetLocal(idx_slot)));
                let ls = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::GetLocal(idx_slot)));
                self.code.push(PendingOp::Ready(OpCode::GetLocal(iter_slot)));
                self.code.push(PendingOp::Ready(OpCode::Builtin("len".to_string(), 1)));
                self.code.push(PendingOp::Ready(OpCode::Lt));
                let jif = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
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
            Stmt::Break => {
                if let Some(ctx) = self.loop_stack.last_mut() {
                    let j = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                    ctx.break_jumps.push(j);
                }
            }
            Stmt::Continue => {
                if let Some(ctx) = self.loop_stack.last_mut() {
                    let j = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                    ctx.continue_jumps.push(j);
                }
            }
            Stmt::Match(target, cases, else_block) => {
                self.compile_expr(target);
                let mut end_jumps = Vec::new();
                for (cv, block) in cases {
                    self.code.push(PendingOp::Ready(OpCode::Dup));
                    self.compile_expr(cv);
                    self.code.push(PendingOp::Ready(OpCode::Eq));
                    let jif = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                    self.code.push(PendingOp::Ready(OpCode::Pop));
                    self.compile_stmt(*block);
                    let ej = self.code.len();
                    self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                    end_jumps.push(ej);
                    let nc = self.code.len();
                    self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(nc));
                }
                self.code.push(PendingOp::Ready(OpCode::Pop));
                if let Some(eb) = else_block { self.compile_stmt(*eb); }
                let end = self.code.len();
                for j in end_jumps { self.code[j] = PendingOp::Ready(OpCode::Jump(end)); }
            }
            Stmt::Function(name, params, body) => {
                let mut final_params = params;
                if name.contains('.') {
                    if final_params.first().map_or(true, |p| p != "self") {
                        final_params.insert(0, "self".to_string());
                    }
                }
                let (idx, _caps, _np, _nl) = self.compile_function(final_params, body, false);
                let slot = self.next_local;
                self.locals.insert(name.clone(), slot);
                self.next_local += 1;
                self.code.push(PendingOp::Ready(OpCode::DefineFunction(name, idx, slot)));
            }
            Stmt::Return(expr) => {
                if let Some(e) = expr { self.compile_expr(e); }
                else { self.code.push(PendingOp::Ready(OpCode::Constant(Value::Null))); }
                self.code.push(PendingOp::Ready(OpCode::Return));
            }
            Stmt::Throw(expr) => { self.compile_expr(expr); self.code.push(PendingOp::Ready(OpCode::Throw)); }
            Stmt::Try(try_block, err_name, catch_block) => {
                let ts = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::TryStart(0)));
                self.compile_stmt(*try_block);
                self.code.push(PendingOp::Ready(OpCode::TryEnd));
                let jo = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                let hs = self.code.len();
                self.code[ts] = PendingOp::Ready(OpCode::TryStart(hs));
                let es = self.next_local; self.next_local += 1;
                let saved = self.locals.insert(err_name.clone(), es);
                self.code.push(PendingOp::Ready(OpCode::SetLocal(es)));
                self.compile_stmt(*catch_block);
                let end = self.code.len();
                self.code[jo] = PendingOp::Ready(OpCode::Jump(end));
                match saved {
                    Some(old) => { self.locals.insert(err_name, old); }
                    None => { self.locals.remove(&err_name); }
                }
            }
            Stmt::StructDef(_, _) => {}
        }
    }

    fn compile_expr(&mut self, expr: Expr) {
        match expr {
            Expr::Number(n) => self.code.push(PendingOp::Ready(OpCode::Constant(Value::Number(n)))),
            Expr::String(s) => self.code.push(PendingOp::Ready(OpCode::Constant(Value::String(s)))),
            Expr::Bool(b) => self.code.push(PendingOp::Ready(OpCode::Constant(Value::Bool(b)))),
            Expr::Ident(name) => {
                if let Some(&slot) = self.locals.get(&name) {
                    self.code.push(PendingOp::Ready(OpCode::GetLocal(slot)));
                } else {
                    let upval_idx: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = upval_idx {
                        self.code.push(PendingOp::GetUpvalue(idx));
                    } else if let Some(enc) = self.enclosing.clone() {
                        if Self::ensure_enclosing_captures(&enc, &name).is_some() {
                            let idx = {
                                let mut u = self.upvalues.borrow_mut();
                                if let Some(&i) = u.map.get(&name) { i } else {
                                    let i = u.map.len();
                                    u.map.insert(name.clone(), i);
                                    u.list.push(name.clone());
                                    i
                                }
                            };
                            self.captured_names.borrow_mut().push(name.clone());
                            self.code.push(PendingOp::GetUpvalue(idx));
                        } else {
                            self.code.push(PendingOp::Ready(OpCode::Constant(Value::Null)));
                        }
                    } else {
                        self.code.push(PendingOp::Ready(OpCode::Constant(Value::Null)));
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
            Expr::FieldAccess(obj, field) => {
                self.compile_expr(*obj);
                self.code.push(PendingOp::Ready(OpCode::FieldAccess(field)));
            }
            Expr::Index(t, i) => {
                self.compile_expr(*t); self.compile_expr(*i);
                self.code.push(PendingOp::Ready(OpCode::Index));
            }
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
                    let upval_idx: Option<usize> = self.upvalues.borrow().map.get(&name).copied();
                    if let Some(idx) = upval_idx {
                        self.code.push(PendingOp::GetUpvalue(idx));
                        self.code.push(PendingOp::Ready(OpCode::Call(args.len())));
                    } else {
                        for _ in 0..args.len() { self.code.push(PendingOp::Ready(OpCode::Pop)); }
                        self.code.push(PendingOp::Ready(OpCode::Constant(Value::Null)));
                    }
                }
            }
            Expr::Unary(op, right) => {
                self.compile_expr(*right);
                match op {
                    TokenType::Minus => self.code.push(PendingOp::Ready(OpCode::Negate)),
                    TokenType::Bang => self.code.push(PendingOp::Ready(OpCode::Not)),
                    _ => {}
                }
            }
            Expr::Binary(l, op, r) => {
                self.compile_expr(*l); self.compile_expr(*r);
                let oc = match op {
                    TokenType::Plus => OpCode::Add, TokenType::Minus => OpCode::Sub,
                    TokenType::Star => OpCode::Mul, TokenType::Slash => OpCode::Div,
                    TokenType::EqEq => OpCode::Eq, TokenType::NotEq => OpCode::NotEq,
                    TokenType::Lt => OpCode::Lt, TokenType::Gt => OpCode::Gt,
                    TokenType::LtEq => OpCode::LtEq, TokenType::GtEq => OpCode::GtEq,
                    _ => return,
                };
                self.code.push(PendingOp::Ready(oc));
            }
            Expr::Ternary(c, t, e) => {
                self.compile_expr(*c);
                let jif = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::JumpIfFalse(0)));
                self.compile_expr(*t);
                let jo = self.code.len();
                self.code.push(PendingOp::Ready(OpCode::Jump(0)));
                let et = self.code.len();
                self.code[jif] = PendingOp::Ready(OpCode::JumpIfFalse(et));
                self.compile_expr(*e);
                let end = self.code.len();
                self.code[jo] = PendingOp::Ready(OpCode::Jump(end));
            }
        }
    }
}

fn is_builtin(name: &str) -> bool {
    matches!(name,
        "str" | "num" | "type" | "len" | "push" | "pop" | "keys"
        | "split" | "join" | "upper" | "lower" | "trim" | "replace" | "contains"
        | "str_eq" | "char_code" | "byte_at" | "byte_substr" | "bytes_to_str"
        | "read_file" | "write_file" | "file_exists" | "list_dir" | "mkdir"
        | "random" | "clear_screen" | "print_str" | "print_char" | "read_line" | "input"
        | "tcp_listen" | "tcp_accept" | "tcp_read" | "tcp_write" | "tcp_close" | "http_get"
    )
}

// ===================== FREE FUNCTIONS (extracted from &mut self) =====================

fn compute_roots(stack: &[Value], locals: &[Value]) -> Vec<Value> {
    let mut r = Vec::with_capacity(stack.len() + locals.len());
    r.extend_from_slice(stack);
    r.extend_from_slice(locals);
    r
}

fn builtin_impl(heap: &mut Heap, name: &str, args: Vec<Value>) -> Result<Value, String> {
    match name {
        "str" => Ok(Value::String(args.get(0).unwrap_or(&Value::Null).to_string())),
        "num" => match args.get(0).unwrap_or(&Value::Null) {
            Value::Number(n) => Ok(Value::Number(*n)),
            Value::String(s) => s.parse::<f64>().map(Value::Number).map_err(|_| format!("num() cannot parse {:?}", s)),
            _ => Err("num() expects string or number".to_string()),
        },
        "type" => {
            let t = match args.get(0).unwrap_or(&Value::Null) {
                Value::Number(_) => "number", Value::String(_) => "string", Value::Bool(_) => "bool",
                Value::Array(_) => "array", Value::Map(_) => "map", Value::Struct(..) => "struct",
                Value::Function { .. } => "function", Value::Closure(..) => "function", Value::Null => "null",
            };
            Ok(Value::String(t.to_string()))
        }
        "len" => match args.get(0).unwrap_or(&Value::Null) {
            Value::String(s) => Ok(Value::Number(s.len() as f64)),
            Value::Array(idx) => if let HeapObject::Array(arr) = heap.get(*idx) { Ok(Value::Number(arr.len() as f64)) } else { Err("Not array".to_string()) },
            Value::Map(idx) => if let HeapObject::Map(map) = heap.get(*idx) { Ok(Value::Number(map.len() as f64)) } else { Err("Not map".to_string()) },
            _ => Err("len() expects collection".to_string()),
        },
        "push" => {
            let v = args.get(1).cloned().unwrap_or(Value::Null);
            if let Some(Value::Array(idx)) = args.get(0) {
                if let HeapObject::Array(arr) = heap.get_mut(*idx) { arr.push(v); }
            }
            Ok(Value::Null)
        }
        "pop" => {
            if let Some(Value::Array(idx)) = args.get(0) {
                if let HeapObject::Array(arr) = heap.get_mut(*idx) { return Ok(arr.pop().unwrap_or(Value::Null)); }
            }
            Err("pop() expects array".to_string())
        }
        "keys" => {
            if let Some(Value::Map(idx)) = args.get(0) {
                let mut keys = Vec::new();
                if let HeapObject::Map(map) = heap.get(*idx) {
                    for k in map.keys() {
                        let kv = match k {
                            MapKey::String(s) => Value::String(s.clone()),
                            MapKey::Number(n) => Value::Number(f64::from_bits(*n as u64)),
                            MapKey::Bool(b) => Value::Bool(*b),
                        };
                        keys.push(kv);
                    }
                }
                let out = heap.alloc(HeapObject::Array(keys));
                return Ok(Value::Array(out));
            }
            Err("keys() expects map".to_string())
        }
        "split" => {
            if let (Some(Value::String(sv)), Some(Value::String(sep))) = (args.get(0), args.get(1)) {
                if sep.is_empty() { return Err("split() sep empty".to_string()); }
                let parts: Vec<Value> = sv.split(sep.as_str()).map(|p| Value::String(p.to_string())).collect();
                let idx = heap.alloc(HeapObject::Array(parts));
                Ok(Value::Array(idx))
            } else { Err("split() expects two strings".to_string()) }
        }
        "join" => {
            if let (Some(Value::Array(idx)), Some(Value::String(sep))) = (args.get(0), args.get(1)) {
                if let HeapObject::Array(arr) = heap.get(*idx) {
                    let mut parts = Vec::new();
                    for item in arr.iter() {
                        if let Value::String(s) = item { parts.push(s.clone()); }
                        else { return Err("join() expects strings".to_string()); }
                    }
                    return Ok(Value::String(parts.join(sep)));
                }
            }
            Err("join() expects array and string".to_string())
        }
        "upper" => match args.get(0) { Some(Value::String(s)) => Ok(Value::String(s.to_uppercase())), _ => Err("upper()".to_string()) },
        "lower" => match args.get(0) { Some(Value::String(s)) => Ok(Value::String(s.to_lowercase())), _ => Err("lower()".to_string()) },
        "trim" => match args.get(0) { Some(Value::String(s)) => Ok(Value::String(s.trim().to_string())), _ => Err("trim()".to_string()) },
        "replace" => {
            if let (Some(Value::String(sv)), Some(Value::String(ov)), Some(Value::String(nv))) = (args.get(0), args.get(1), args.get(2)) {
                Ok(Value::String(sv.replace(ov, nv)))
            } else { Err("replace()".to_string()) }
        }
        "contains" => match (args.get(0), args.get(1)) {
            (Some(Value::String(s)), Some(Value::String(sub))) => Ok(Value::Bool(s.contains(sub))),
            (Some(Value::Array(idx)), Some(v)) => {
                if let HeapObject::Array(arr) = heap.get(*idx) {
                    Ok(Value::Bool(arr.iter().any(|x| x.to_string() == v.to_string())))
                } else { Err("Not array".to_string()) }
            }
            (Some(Value::Map(idx)), Some(v)) => {
                if let Ok(k) = MapKey::from_value(v) {
                    if let HeapObject::Map(map) = heap.get(*idx) { return Ok(Value::Bool(map.contains_key(&k))); }
                }
                Ok(Value::Bool(false))
            }
            _ => Err("contains()".to_string()),
        },
        "str_eq" => {
            let a = args.get(0).cloned().unwrap_or(Value::Null);
            let b = args.get(1).cloned().unwrap_or(Value::Null);
            Ok(Value::Number(if a.to_string() == b.to_string() { 1.0 } else { 0.0 }))
        }
        "char_code" => match args.get(0) {
            Some(Value::String(s)) if s.as_bytes().len() == 1 => Ok(Value::Number(s.as_bytes()[0] as f64)),
            _ => Err("char_code()".to_string()),
        },
        "byte_at" => {
            if let (Some(Value::String(sv)), Some(Value::Number(n))) = (args.get(0), args.get(1)) {
                let idx = *n as isize; let bytes = sv.as_bytes();
                if idx < 0 || idx as usize >= bytes.len() { return Ok(Value::Number(-1.0)); }
                return Ok(Value::Number(bytes[idx as usize] as f64));
            }
            Err("byte_at()".to_string())
        }
        "byte_substr" => {
            if let (Some(Value::String(sv)), Some(Value::Number(a)), Some(Value::Number(b))) = (args.get(0), args.get(1), args.get(2)) {
                let bytes = sv.as_bytes();
                let mut start = if *a < 0.0 { 0 } else { *a as usize };
                let mut end = if *b < 0.0 { 0 } else { *b as usize };
                if start > bytes.len() { start = bytes.len(); }
                if end > bytes.len() { end = bytes.len(); }
                if start > end { start = end; }
                return Ok(Value::String(String::from_utf8_lossy(&bytes[start..end]).to_string()));
            }
            Err("byte_substr()".to_string())
        }
        "bytes_to_str" => {
            if let Some(Value::Array(idx)) = args.get(0) {
                if let HeapObject::Array(arr) = heap.get(*idx) {
                    let mut buf = Vec::with_capacity(arr.len());
                    for v in arr.iter() {
                        if let Value::Number(n) = v {
                            let b = *n as i64;
                            if !(0..=255).contains(&b) { return Err(format!("byte out of range: {}", b)); }
                            buf.push(b as u8);
                        } else { return Err("bytes_to_str()".to_string()); }
                    }
                    return Ok(Value::String(String::from_utf8_lossy(&buf).to_string()));
                }
            }
            Err("bytes_to_str()".to_string())
        }
        "read_file" => match args.get(0) {
            Some(Value::String(p)) => Ok(Value::String(fs::read_to_string(p).map_err(|e| e.to_string())?)),
            _ => Err("read_file()".to_string()),
        },
        "write_file" => {
            if let (Some(Value::String(p)), Some(Value::String(c))) = (args.get(0), args.get(1)) {
                fs::write(p, c).map_err(|e| e.to_string())?;
                Ok(Value::Null)
            } else { Err("write_file()".to_string()) }
        }
        "file_exists" => match args.get(0) {
            Some(Value::String(p)) => Ok(Value::Bool(Path::new(p).exists())),
            _ => Err("file_exists()".to_string()),
        },
        "list_dir" => {
            if let Some(Value::String(p)) = args.get(0) {
                let mut files = Vec::new();
                if let Ok(entries) = fs::read_dir(p) {
                    for entry in entries.flatten() {
                        if let Ok(name) = entry.file_name().into_string() { files.push(Value::String(name)); }
                    }
                }
                let idx = heap.alloc(HeapObject::Array(files));
                return Ok(Value::Array(idx));
            }
            Err("list_dir()".to_string())
        }
        "mkdir" => match args.get(0) {
            Some(Value::String(p)) => { let _ = fs::create_dir_all(p); Ok(Value::Null) }
            _ => Err("mkdir()".to_string()),
        },
        "random" => {
            let max = match args.get(0) { Some(Value::Number(n)) => *n as u64, _ => 100 };
            use std::time::{SystemTime, UNIX_EPOCH};
            let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().subsec_nanos() as u64;
            Ok(Value::Number(((nanos * 1103515245 + 12345) % max) as f64))
        }
        "clear_screen" => {
            print!("\x1b[2J\x1b[1;1H");
            use std::io::Write; std::io::stdout().flush().unwrap();
            Ok(Value::Null)
        }
        "print_str" => {
            if let Some(v) = args.get(0) { print!("{}", v.to_string()); use std::io::Write; std::io::stdout().flush().unwrap(); }
            Ok(Value::Null)
        }
        "print_char" => {
            if let Some(Value::Number(n)) = args.get(0) { print!("{}", *n as u8 as char); use std::io::Write; std::io::stdout().flush().unwrap(); }
            Ok(Value::Null)
        }
        "read_line" => {
            let mut input = String::new();
            std::io::stdin().read_line(&mut input).unwrap();
            Ok(Value::String(input.trim().to_string()))
        }
        "input" => {
            let prompt = match args.get(0) { Some(Value::String(s)) => s.clone(), _ => String::new() };
            print!("{}", prompt);
            use std::io::Write; std::io::stdout().flush().unwrap();
            let mut input = String::new();
            std::io::stdin().read_line(&mut input).unwrap();
            Ok(Value::String(input.trim().to_string()))
        }
        "tcp_listen" => {
            if let Some(Value::Number(port)) = args.get(0) {
                let l = TcpListener::bind(format!("127.0.0.1:{}", port)).map_err(|e| e.to_string())?;
                let mut c = get_counter().lock().unwrap();
                let id = *c; *c += 1;
                get_servers().lock().unwrap().insert(id, l);
                return Ok(Value::Number(id as f64));
            }
            Err("tcp_listen()".to_string())
        }
        "tcp_accept" => {
            if let Some(Value::Number(id)) = args.get(0) {
                let servers = get_servers().lock().unwrap();
                if let Some(l) = servers.get(&(*id as usize)) {
                    match l.accept() {
                        Ok((s, _)) => {
                            let mut c = get_counter().lock().unwrap();
                            let cid = *c; *c += 1;
                            get_clients().lock().unwrap().insert(cid, s);
                            return Ok(Value::Number(cid as f64));
                        }
                        Err(e) => return Err(format!("accept: {}", e)),
                    }
                }
            }
            Err("tcp_accept()".to_string())
        }
        "tcp_read" => {
            if let Some(Value::Number(id)) = args.get(0) {
                let max = match args.get(1) { Some(Value::Number(n)) => *n as usize, _ => 4096 };
                let mut clients = get_clients().lock().unwrap();
                if let Some(s) = clients.get_mut(&(*id as usize)) {
                    let mut buf = vec![0; max];
                    match s.read(&mut buf) {
                        Ok(n) => { buf.truncate(n); return Ok(Value::String(String::from_utf8_lossy(&buf).to_string())); }
                        Err(e) => return Err(format!("read: {}", e)),
                    }
                }
            }
            Err("tcp_read()".to_string())
        }
        "tcp_write" => {
            if let (Some(Value::Number(id)), Some(Value::String(msg))) = (args.get(0), args.get(1)) {
                let mut clients = get_clients().lock().unwrap();
                if let Some(s) = clients.get_mut(&(*id as usize)) {
                    return s.write_all(msg.as_bytes()).map(|_| Value::Null).map_err(|e| e.to_string());
                }
            }
            Err("tcp_write()".to_string())
        }
        "tcp_close" => {
            if let Some(Value::Number(id)) = args.get(0) {
                get_clients().lock().unwrap().remove(&(*id as usize));
                get_servers().lock().unwrap().remove(&(*id as usize));
                return Ok(Value::Null);
            }
            Err("tcp_close()".to_string())
        }
        "http_get" => {
            if let Some(Value::String(url)) = args.get(0) {
                match ureq::get(url).call() {
                    Ok(resp) => return resp.into_string().map(Value::String).map_err(|e| e.to_string()),
                    Err(e) => return Err(format!("http_get: {}", e)),
                }
            }
            Err("http_get()".to_string())
        }
        _ => Err(format!("Unknown builtin: {}", name)),
    }
}

// ===================== VM =====================

struct Handler {
    ip: usize,
    locals: Vec<Value>,
    frames: Vec<Vec<Value>>,
    call_stack: Vec<(usize, usize, Vec<OpCode>)>,
    closure_stack: Vec<Option<(Rc<RefCell<Vec<Value>>>, usize, usize)>>,
    stack: Vec<Value>,
}

pub struct VM {
    stack: Vec<Value>,
    locals: Vec<Value>,
    frames: Vec<Vec<Value>>,
    functions: Vec<Vec<OpCode>>,
    functions_by_name: HashMap<String, usize>,
    call_stack: Vec<(usize, usize, Vec<OpCode>)>,
    closure_stack: Vec<Option<(Rc<RefCell<Vec<Value>>>, usize, usize)>>,
    handlers: Vec<Handler>,
    pub heap: Heap,
}

impl VM {
    pub fn new() -> Self {
        VM {
            stack: Vec::new(), locals: Vec::new(), frames: Vec::new(),
            functions: Vec::new(), functions_by_name: HashMap::new(),
            call_stack: Vec::new(), closure_stack: Vec::new(),
            handlers: Vec::new(), heap: Heap::new(),
        }
    }

    pub fn run(&mut self, code: Vec<OpCode>, funcs: Vec<Vec<OpCode>>) -> Result<(), String> {
        self.functions = funcs;

        // STEP 3: destructure `self` into disjoint &mut fields.
        // LLVM's noalias model now knows stack / locals / heap etc. don't alias.
        let Self {
            stack,
            locals,
            frames,
            functions,
            functions_by_name,
            call_stack,
            closure_stack,
            handlers,
            heap,
        } = self;

        let mut ip: usize = 0;
        let mut current_code: Vec<OpCode> = code.clone();
        // NOTE: `code` intentionally stays owned & alive — `Throw` needs it for
        // `current_code = code.clone()`. (Earlier draft had a bug moving it into
        // an unused `_original_code`.)

        while ip < current_code.len() {
            // STEP 2: skip the bounds check. Guarded by the `while` condition above.
            let op = unsafe { current_code.get_unchecked(ip) };
            match op {
                OpCode::Constant(v) => stack.push(v.clone()),
                OpCode::Add => {
                    let b = stack.pop().unwrap_or(Value::Null);
                    let a = stack.pop().unwrap_or(Value::Null);
                    match (a, b) {
                        (Value::Number(x), Value::Number(y)) => stack.push(Value::Number(x + y)),
                        (Value::String(x), Value::String(y)) => stack.push(Value::String(format!("{}{}", x, y))),
                        _ => stack.push(Value::Null),
                    }
                }
                OpCode::Sub => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); if let (Value::Number(x), Value::Number(y)) = (a, b) { stack.push(Value::Number(x - y)); } }
                OpCode::Mul => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); if let (Value::Number(x), Value::Number(y)) = (a, b) { stack.push(Value::Number(x * y)); } }
                OpCode::Div => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); if let (Value::Number(x), Value::Number(y)) = (a, b) { stack.push(Value::Number(x / y)); } }
                OpCode::Eq => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); stack.push(Value::Bool(a.to_string() == b.to_string())); }
                OpCode::NotEq => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); stack.push(Value::Bool(a.to_string() != b.to_string())); }
                OpCode::Lt => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); match (&a, &b) { (Value::Number(x), Value::Number(y)) => stack.push(Value::Bool(x < y)), _ => stack.push(Value::Bool(a.to_string() < b.to_string())) } }
                OpCode::Gt => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); match (&a, &b) { (Value::Number(x), Value::Number(y)) => stack.push(Value::Bool(x > y)), _ => stack.push(Value::Bool(a.to_string() > b.to_string())) } }
                OpCode::LtEq => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); match (&a, &b) { (Value::Number(x), Value::Number(y)) => stack.push(Value::Bool(x <= y)), _ => stack.push(Value::Bool(a.to_string() <= b.to_string())) } }
                OpCode::GtEq => { let b = stack.pop().unwrap_or(Value::Null); let a = stack.pop().unwrap_or(Value::Null); match (&a, &b) { (Value::Number(x), Value::Number(y)) => stack.push(Value::Bool(x >= y)), _ => stack.push(Value::Bool(a.to_string() >= b.to_string())) } }
                OpCode::Negate => { let v = stack.pop().unwrap_or(Value::Null); if let Value::Number(n) = v { stack.push(Value::Number(-n)); } else { stack.push(Value::Null); } }
                OpCode::Not => { let v = stack.pop().unwrap_or(Value::Null); if let Value::Bool(b) = v { stack.push(Value::Bool(!b)); } else { stack.push(Value::Null); } }
                OpCode::Print => { let val = stack.pop().unwrap_or(Value::Null); println!("{}", val.to_string()); }

                OpCode::BinLocals(op, a, b) => {
                    let ai = *a as usize;
                    let bi = *b as usize;
                    debug_assert!(ai < locals.len());
                    debug_assert!(bi < locals.len());
                    let lv = unsafe { locals.get_unchecked(ai) };
                    let rv = unsafe { locals.get_unchecked(bi) };
                    let res = match (lv, rv) {
                        (Value::Number(x), Value::Number(y)) => match op {
                            TokenType::Plus => Value::Number(x + y),
                            TokenType::Minus => Value::Number(x - y),
                            TokenType::Star => Value::Number(x * y),
                            TokenType::Slash => Value::Number(x / y),
                            TokenType::EqEq => Value::Bool(x == y),
                            TokenType::NotEq => Value::Bool(x != y),
                            TokenType::Lt => Value::Bool(x < y),
                            TokenType::Gt => Value::Bool(x > y),
                            TokenType::LtEq => Value::Bool(x <= y),
                            TokenType::GtEq => Value::Bool(x >= y),
                            _ => Value::Null,
                        },
                        (Value::String(x), Value::String(y)) => match op {
                            TokenType::Plus => Value::String(format!("{}{}", x, y)),
                            TokenType::EqEq => Value::Bool(x == y),
                            TokenType::NotEq => Value::Bool(x != y),
                            TokenType::Lt => Value::Bool(x < y),
                            TokenType::Gt => Value::Bool(x > y),
                            TokenType::LtEq => Value::Bool(x <= y),
                            TokenType::GtEq => Value::Bool(x >= y),
                            _ => Value::Null,
                        },
                        (Value::Bool(x), Value::Bool(y)) => match op {
                            TokenType::EqEq => Value::Bool(x == y),
                            TokenType::NotEq => Value::Bool(x != y),
                            _ => Value::Null,
                        },
                        _ => Value::Null,
                    };
                    stack.push(res);
                }
                OpCode::IncLocal(slot, delta) => {
                    let s = *slot as usize;
                    debug_assert!(s < locals.len());
                    unsafe {
                        if let Value::Number(n) = locals.get_unchecked_mut(s) { *n += delta; }
                    }
                }
                OpCode::AddLocalConst(a, k, b) => {
                    let ai = *a as usize;
                    let bi = *b as usize;
                    debug_assert!(ai < locals.len());
                    let res = unsafe {
                        match locals.get_unchecked(ai) {
                            Value::Number(n) => Value::Number(n + k),
                            _ => Value::Null,
                        }
                    };
                    if bi >= locals.len() { locals.resize(bi + 1, Value::Null); }
                    locals[bi] = res;
                }

                OpCode::JumpIfLocalCmpFalse(op, a, b, target) => {
                    let ai = *a as usize;
                    let bi = *b as usize;
                    debug_assert!(ai < locals.len());
                    debug_assert!(bi < locals.len());
                    let cond = unsafe {
                        match (locals.get_unchecked(ai), locals.get_unchecked(bi)) {
                            (Value::Number(x), Value::Number(y)) => match op {
                                TokenType::Lt => x < y, TokenType::LtEq => x <= y,
                                TokenType::Gt => x > y, TokenType::GtEq => x >= y,
                                TokenType::EqEq => x == y, TokenType::NotEq => x != y,
                                _ => false,
                            },
                            (Value::String(x), Value::String(y)) => match op {
                                TokenType::EqEq => x == y, TokenType::NotEq => x != y,
                                TokenType::Lt => x < y, TokenType::Gt => x > y,
                                TokenType::LtEq => x <= y, TokenType::GtEq => x >= y,
                                _ => false,
                            },
                            (Value::Bool(x), Value::Bool(y)) => match op {
                                TokenType::EqEq => x == y, TokenType::NotEq => x != y,
                                _ => false,
                            },
                            _ => false,
                        }
                    };
                    if !cond { ip = *target; continue; }
                }
                OpCode::AddLocalMulLocal(dst, a, b) => {
                    let ai = *a as usize;
                    let bi = *b as usize;
                    let di = *dst as usize;
                    debug_assert!(ai < locals.len());
                    debug_assert!(bi < locals.len());
                    debug_assert!(di < locals.len());
                    let prod = unsafe {
                        match (locals.get_unchecked(ai), locals.get_unchecked(bi)) {
                            (Value::Number(x), Value::Number(y)) => Value::Number(x * y),
                            _ => Value::Null,
                        }
                    };
                    let sum = {
                        let cur = unsafe { locals.get_unchecked(di) };
                        match (cur, &prod) {
                            (Value::Number(x), Value::Number(y)) => Value::Number(x + y),
                            (Value::String(x), Value::String(y)) => Value::String(format!("{}{}", x, y)),
                            _ => Value::Null,
                        }
                    };
                    locals[di] = sum;
                }
                OpCode::IncLocalJump(slot, delta, target) => {
                    let s = *slot as usize;
                    debug_assert!(s < locals.len());
                    unsafe {
                        if let Value::Number(n) = locals.get_unchecked_mut(s) { *n += delta; }
                    }
                    ip = *target;
                    continue;
                }
                OpCode::JumpIfLocalConstCmpFalse(op, slot, k, target) => {
                    let s = *slot as usize;
                    debug_assert!(s < locals.len());
                    let cond = unsafe {
                        match locals.get_unchecked(s) {
                            Value::Number(x) => match op {
                                TokenType::Lt => *x < *k, TokenType::LtEq => *x <= *k,
                                TokenType::Gt => *x > *k, TokenType::GtEq => *x >= *k,
                                TokenType::EqEq => *x == *k, TokenType::NotEq => *x != *k,
                                _ => false,
                            },
                            Value::String(x) => {
                                let rhs = format!("{}", k);
                                match op {
                                    TokenType::EqEq => *x == rhs, TokenType::NotEq => *x != rhs,
                                    TokenType::Lt => *x < rhs, TokenType::Gt => *x > rhs,
                                    TokenType::LtEq => *x <= rhs, TokenType::GtEq => *x >= rhs,
                                    _ => false,
                                }
                            }
                            _ => false,
                        }
                    };
                    if !cond { ip = *target; continue; }
                }

                OpCode::Interpolate(n) => {
                    if *n == 0 { stack.push(Value::String(String::new())); }
                    else {
                        let mut parts = Vec::new();
                        for _ in 0..*n { parts.push(stack.pop().unwrap_or(Value::Null)); }
                        parts.reverse();
                        let s: String = parts.iter().map(|v| v.to_string()).collect();
                        stack.push(Value::String(s));
                    }
                }

                OpCode::MakeClosure(func_idx, captured_slots, np, nl) => {
                    let captured: Vec<Value> = captured_slots.iter().map(|&s| {
                        if s == usize::MAX { Value::Null } else { locals.get(s).cloned().unwrap_or(Value::Null) }
                    }).collect();
                    stack.push(Value::Closure(*func_idx, *np, *nl, Rc::new(RefCell::new(captured))));
                }

                OpCode::DefineFunction(name, idx, slot) => {
                    if locals.len() <= *slot { locals.resize(*slot + 1, Value::Null); }
                    locals[*slot] = Value::Number(*idx as f64);
                    functions_by_name.insert(name.clone(), *idx);
                }

                OpCode::Builtin(name, arg_count) => {
                    if heap.needs_gc() {
                        let roots = compute_roots(stack, locals);
                        heap.collect_garbage(&roots);
                    }
                    let mut args = Vec::with_capacity(*arg_count);
                    for _ in 0..*arg_count { args.push(stack.pop().unwrap_or(Value::Null)); }
                    let res = builtin_impl(heap, name, args)?;
                    stack.push(res);
                }

                OpCode::Dup => { let top = stack.last().cloned().unwrap_or(Value::Null); stack.push(top); }
                OpCode::Pop => { stack.pop(); }
                OpCode::SetLocal(slot) => {
                    let val = stack.pop().unwrap_or(Value::Null);
                    if locals.len() <= *slot { locals.resize(*slot + 1, Value::Null); }
                    locals[*slot] = val;
                }
                OpCode::GetLocal(slot) => {
                    let v = locals.get(*slot).cloned().unwrap_or(Value::Null);
                    stack.push(v);
                }
                OpCode::Jump(target) => { ip = *target; continue; }
                OpCode::JumpIfFalse(target) => {
                    let val = stack.pop().unwrap_or(Value::Null);
                    if !val.is_truthy() { ip = *target; continue; }
                }

                OpCode::MethodCall(method, arg_count) => {
                    let mut args = Vec::new();
                    for _ in 0..*arg_count { args.push(stack.pop().unwrap_or(Value::Null)); }
                    let target = stack.pop().unwrap_or(Value::Null);
                    if let Value::Struct(idx) = target {
                        let struct_name = if let HeapObject::Struct(n, _) = heap.get(idx) { n.clone() } else { "".to_string() };
                        let full_name = format!("{}.{}", struct_name, method);
                        if let Some(&func_idx) = functions_by_name.get(&full_name) {
                            call_stack.push((ip, handlers.len(), current_code.clone()));
                            closure_stack.push(None);
                            frames.push(std::mem::replace(locals, Vec::new()));
                            let mut final_args = vec![Value::Struct(idx)];
                            final_args.extend(args);
                            *locals = final_args;
                            current_code = functions[func_idx].clone();
                            ip = 0; continue;
                        }
                    }
                }

                OpCode::Call(arg_count) => {
                    let func_idx_val = stack.pop().unwrap_or(Value::Null);
                    match func_idx_val {
                        Value::Number(idx) => {
                            call_stack.push((ip, handlers.len(), current_code.clone()));
                            closure_stack.push(None);
                            frames.push(std::mem::replace(locals, Vec::new()));
                            let new_locals: Vec<Value> = (0..*arg_count).map(|_| stack.pop().unwrap_or(Value::Null)).collect();
                            *locals = new_locals;
                            current_code = functions[idx as usize].clone();
                            ip = 0; continue;
                        }
                        Value::Closure(idx, np, nl, captured) => {
                            let cap_snapshot: Vec<Value> = {
                                let b = captured.borrow();
                                b.iter().cloned().collect()
                            };
                            call_stack.push((ip, handlers.len(), current_code.clone()));
                            closure_stack.push(Some((captured.clone(), np, nl)));
                            frames.push(std::mem::replace(locals, Vec::new()));
                            let mut new_locals: Vec<Value> = Vec::with_capacity(np + nl + cap_snapshot.len());
                            for _ in 0..*arg_count { new_locals.push(stack.pop().unwrap_or(Value::Null)); }
                            new_locals.resize(np + nl, Value::Null);
                            new_locals.extend(cap_snapshot);
                            *locals = new_locals;
                            current_code = functions[idx].clone();
                            ip = 0; continue;
                        }
                        _ => {}
                    }
                }
                OpCode::Return => {
                    let ret_val = stack.pop().unwrap_or(Value::Null);
                    if let Some((ret_ip, handlers_len, caller_code)) = call_stack.pop() {
                        handlers.truncate(handlers_len);
                        if let Some(Some((cap, np, nl))) = closure_stack.pop() {
                            let base = np + nl;
                            let mut b = cap.borrow_mut();
                            for i in 0..b.len() {
                                if base + i < locals.len() {
                                    b[i] = locals[base + i].clone();
                                }
                            }
                        }
                        *locals = frames.pop().unwrap_or_default();
                        stack.push(ret_val);
                        current_code = caller_code;
                        ip = ret_ip;
                    } else { break; }
                }

                OpCode::TryStart(handler_ip) => {
                    handlers.push(Handler {
                        ip: *handler_ip,
                        locals: locals.clone(),
                        frames: frames.clone(),
                        call_stack: call_stack.clone(),
                        closure_stack: closure_stack.clone(),
                        stack: stack.clone(),
                    });
                }
                OpCode::TryEnd => { handlers.pop(); }
                OpCode::Throw => {
                    let exc = stack.pop().unwrap_or(Value::Null);
                    if let Some(h) = handlers.pop() {
                        *locals = h.locals;
                        *frames = h.frames;
                        *call_stack = h.call_stack;
                        *closure_stack = h.closure_stack;
                        *stack = h.stack;
                        stack.push(exc);
                        current_code = code.clone();
                        ip = h.ip; continue;
                    } else {
                        return Err(format!("Uncaught exception: {}", exc.to_string()));
                    }
                }

                OpCode::Array(size) => {
                    if heap.needs_gc() {
                        let roots = compute_roots(stack, locals);
                        heap.collect_garbage(&roots);
                    }
                    let mut elements = Vec::with_capacity(*size);
                    for _ in 0..*size { elements.push(stack.pop().unwrap_or(Value::Null)); }
                    elements.reverse();
                    let idx = heap.alloc(HeapObject::Array(elements));
                    stack.push(Value::Array(idx));
                }
                OpCode::Map(size) => {
                    if heap.needs_gc() {
                        let roots = compute_roots(stack, locals);
                        heap.collect_garbage(&roots);
                    }
                    let mut map = HashMap::new();
                    for _ in 0..*size {
                        let v = stack.pop().unwrap_or(Value::Null);
                        let k = stack.pop().unwrap_or(Value::Null);
                        if let Ok(key) = MapKey::from_value(&k) { map.insert(key, v); }
                    }
                    let idx = heap.alloc(HeapObject::Map(map));
                    stack.push(Value::Map(idx));
                }
                OpCode::Struct(name, fields) => {
                    if heap.needs_gc() {
                        let roots = compute_roots(stack, locals);
                        heap.collect_garbage(&roots);
                    }
                    let mut map = HashMap::new();
                    for f in fields.iter().rev() {
                        let v = stack.pop().unwrap_or(Value::Null);
                        map.insert(f.clone(), v);
                    }
                    let idx = heap.alloc(HeapObject::Struct(name.clone(), map));
                    stack.push(Value::Struct(idx));
                }
                OpCode::FieldAccess(field) => {
                    let target = stack.pop().unwrap_or(Value::Null);
                    if let Value::Struct(idx) = target {
                        if let HeapObject::Struct(_, map) = heap.get(idx) {
                            if let Some(v) = map.get(field) { stack.push(v.clone()); } else { stack.push(Value::Null); }
                        }
                    } else { stack.push(Value::Null); }
                }
                OpCode::FieldSet(field) => {
                    let value = stack.pop().unwrap_or(Value::Null);
                    let target = stack.pop().unwrap_or(Value::Null);
                    if let Value::Struct(idx) = target {
                        if let HeapObject::Struct(_, map) = heap.get_mut(idx) {
                            map.insert(field.clone(), value);
                        }
                    }
                }
                OpCode::Index => {
                    let idx_val = stack.pop().unwrap_or(Value::Null);
                    let target = stack.pop().unwrap_or(Value::Null);
                    match (target, idx_val) {
                        (Value::Array(arr_idx), Value::Number(n)) => {
                            if let HeapObject::Array(arr) = heap.get(arr_idx) {
                                let i = n as usize;
                                if i < arr.len() { stack.push(arr[i].clone()); } else { stack.push(Value::Null); }
                            } else { stack.push(Value::Null); }
                        }
                        (Value::Map(map_idx), k) => {
                            if let Ok(key) = MapKey::from_value(&k) {
                                if let HeapObject::Map(map) = heap.get(map_idx) {
                                    if let Some(v) = map.get(&key) { stack.push(v.clone()); } else { stack.push(Value::Null); }
                                }
                            } else { stack.push(Value::Null); }
                        }
                        (Value::Struct(idx), k) => {
                            if let Value::String(key) = k {
                                if let HeapObject::Struct(_, map) = heap.get(idx) {
                                    if let Some(v) = map.get(&key) { stack.push(v.clone()); } else { stack.push(Value::Null); }
                                }
                            } else { stack.push(Value::Null); }
                        }
                        _ => stack.push(Value::Null),
                    }
                }
                OpCode::IndexSet => {
                    let value = stack.pop().unwrap_or(Value::Null);
                    let idx_val = stack.pop().unwrap_or(Value::Null);
                    let target = stack.pop().unwrap_or(Value::Null);
                    match (target, idx_val) {
                        (Value::Array(arr_idx), Value::Number(n)) => {
                            if let HeapObject::Array(arr) = heap.get_mut(arr_idx) {
                                let i = n as usize;
                                if i < arr.len() { arr[i] = value; }
                            }
                        }
                        (Value::Map(map_idx), k) => {
                            if let Ok(key) = MapKey::from_value(&k) {
                                if let HeapObject::Map(map) = heap.get_mut(map_idx) {
                                    map.insert(key, value);
                                }
                            }
                        }
                        (Value::Struct(idx), k) => {
                            if let Value::String(key) = k {
                                if let HeapObject::Struct(_, map) = heap.get_mut(idx) {
                                    map.insert(key, value);
                                }
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