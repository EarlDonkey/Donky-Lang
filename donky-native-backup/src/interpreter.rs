use std::collections::HashMap;
use std::rc::Rc;
use std::cell::RefCell;
use std::fs;
use std::path::Path;
use std::sync::{Mutex, OnceLock};
use std::net::{TcpListener, TcpStream};
use std::io::{Read, Write};
use crate::ast::{Expr, Stmt};
use crate::lexer::TokenType;
use crate::heap::{Value, ValueKind, MapKey, Heap, HeapObject};

static CLIENTS: OnceLock<Mutex<HashMap<usize, TcpStream>>> = OnceLock::new();
static SERVERS: OnceLock<Mutex<HashMap<usize, TcpListener>>> = OnceLock::new();
static COUNTER: OnceLock<Mutex<usize>> = OnceLock::new();

fn get_clients() -> &'static Mutex<HashMap<usize, TcpStream>> { CLIENTS.get_or_init(|| Mutex::new(HashMap::new())) }
fn get_servers() -> &'static Mutex<HashMap<usize, TcpListener>> { SERVERS.get_or_init(|| Mutex::new(HashMap::new())) }
fn get_counter() -> &'static Mutex<usize> { COUNTER.get_or_init(|| Mutex::new(1)) }

pub enum ControlFlow { Normal, Return(Value), Break, Continue, Throw(Value) }

pub struct Environment {
    scopes: Vec<HashMap<String, Value>>,
    structs: HashMap<String, HashMap<String, String>>,
}

impl Environment {
    pub fn new() -> Self { Environment { scopes: vec![HashMap::new()], structs: HashMap::new() } }
    pub fn define(&mut self, name: String, value: Value) { self.scopes.last_mut().unwrap().insert(name, value); }
    pub fn get(&self, name: &str) -> Result<Value, String> {
        for scope in self.scopes.iter().rev() {
            if let Some(v) = scope.get(name) { return Ok(*v); }
        }
        Err(format!("Undefined variable '{}'", name))
    }
    pub fn assign(&mut self, name: &str, value: Value) -> Result<(), String> {
        for scope in self.scopes.iter_mut().rev() {
            if scope.contains_key(name) { scope.insert(name.to_string(), value); return Ok(()); }
        }
        Err(format!("Assignment to undefined variable '{}'", name))
    }
    pub fn push_scope(&mut self) { self.scopes.push(HashMap::new()); }
    pub fn pop_scope(&mut self) { self.scopes.pop(); }
}

pub struct Interpreter { env: Environment, pub heap: Heap }

impl Interpreter {
    pub fn new(script_args: Vec<String>) -> Self {
        let mut env = Environment::new();
        let mut heap = Heap::new();
        let arr: Vec<Value> = script_args.into_iter()
            .map(|s| { let i = heap.alloc(HeapObject::String(s)); Value::string(i) })
            .collect();
        let args_idx = heap.alloc(HeapObject::Array(arr));
        env.define("args".to_string(), Value::array(args_idx));
        Interpreter { env, heap }
    }

    pub fn interpret(&mut self, statements: Vec<Stmt>) -> Result<(), String> {
        for stmt in statements {
            match self.execute(stmt)? {
                ControlFlow::Normal => {}
                ControlFlow::Throw(v) => return Err(format!("Uncaught exception: {}", v.to_string_with_heap(&self.heap))),
                _ => return Err("Cannot return/break outside of function/loop".to_string()),
            }
        }
        Ok(())
    }

    fn execute(&mut self, stmt: Stmt) -> Result<ControlFlow, String> {
        match stmt {
            Stmt::Asm(_) => Ok(ControlFlow::Normal),
            Stmt::Import(path) => {
                let file_content = fs::read_to_string(&path).map_err(|e| e.to_string())?;
                let mut lex = crate::lexer::Lexer::new(&file_content);
                let tokens = lex.tokenize()?;
                let mut parser = crate::parser::Parser::new(tokens);
                let ast = parser.parse()?;
                for s in ast { self.execute(s)?; }
                Ok(ControlFlow::Normal)
            }
            Stmt::Match(target_expr, cases, else_block) => {
                let target = self.evaluate(target_expr)?;
                for (case_expr, block) in cases {
                    let case_val = self.evaluate(case_expr)?;
                    if target.to_string_with_heap(&self.heap) == case_val.to_string_with_heap(&self.heap) { return self.execute(*block); }
                }
                if let Some(eb) = else_block { self.execute(*eb) } else { Ok(ControlFlow::Normal) }
            }
            Stmt::Try(try_block, err_name, catch_block) => {
                match self.execute(*try_block) {
                    Ok(ControlFlow::Throw(val)) => {
                        self.env.push_scope(); self.env.define(err_name, val);
                        let res = self.execute(*catch_block); self.env.pop_scope(); res
                    }
                    Ok(other) => Ok(other),
                    Err(e) => {
                        let sidx = self.heap.alloc(HeapObject::String(e));
                        self.env.push_scope(); self.env.define(err_name, Value::string(sidx));
                        let res = self.execute(*catch_block); self.env.pop_scope(); res
                    }
                }
            }
            Stmt::Throw(expr) => { let v = self.evaluate(expr)?; Ok(ControlFlow::Throw(v)) }
            Stmt::StructDef(name, fields) => {
                let mut ft = HashMap::new();
                for (f, t) in fields { ft.insert(f, t); }
                self.env.structs.insert(name, ft);
                Ok(ControlFlow::Normal)
            }
            Stmt::Let(name, expr) => { let v = self.evaluate(expr)?; self.env.define(name, v); Ok(ControlFlow::Normal) }
            Stmt::Assign(name, expr) => { let v = self.evaluate(expr)?; self.env.assign(&name, v)?; Ok(ControlFlow::Normal) }
            Stmt::IndexSet(target_expr, idx_expr, val_expr) => {
                let index = self.evaluate(*idx_expr)?;
                let value = self.evaluate(*val_expr)?;
                let target = self.evaluate(*target_expr)?;
                match target.kind() {
                    ValueKind::Array(idx) => {
                        if let Some(n) = index.as_number() {
                            let i = n as usize;
                            let arr = self.heap.get_array_mut(idx);
                            if i < arr.len() { arr[i] = value; } else { return Err(format!("Index {} out of range", i)); }
                        } else { return Err("Array index must be a number".to_string()); }
                    }
                    ValueKind::Map(idx) => {
                        if let Ok(key) = MapKey::from_value(index, &self.heap) {
                            self.heap.get_map_mut(idx).insert(key, value);
                        }
                    }
                    ValueKind::Struct(idx) => {
                        if index.is_string() {
                            let k = self.heap.get_string(index.payload()).clone();
                            self.heap.get_struct_mut(idx).insert(k, value);
                        } else { return Err("Struct index must be a string".to_string()); }
                    }
                    _ => return Err("Can only index-assign arrays, maps, or structs".to_string()),
                }
                Ok(ControlFlow::Normal)
            }
            Stmt::FieldSet(obj_expr, field, val_expr) => {
                let obj = self.evaluate(obj_expr)?;
                let value = self.evaluate(val_expr)?;
                if obj.is_struct() {
                    self.heap.get_struct_mut(obj.payload()).insert(field, value);
                    Ok(ControlFlow::Normal)
                } else { Err("Can only set fields on structs".to_string()) }
            }
            Stmt::Print(expr) => {
                let v = self.evaluate(expr)?;
                println!("{}", v.to_string_with_heap(&self.heap));
                Ok(ControlFlow::Normal)
            }
            Stmt::ExprStmt(expr) => { self.evaluate(expr)?; Ok(ControlFlow::Normal) }
            Stmt::Block(stmts) => {
                self.env.push_scope();
                let mut res = ControlFlow::Normal;
                for s in stmts { res = self.execute(s)?; if !matches!(res, ControlFlow::Normal) { break; } }
                self.env.pop_scope();
                Ok(res)
            }
            Stmt::If(cond, then_b, else_b) => {
                if self.evaluate(cond)?.is_truthy() { self.execute(*then_b) }
                else if let Some(eb) = else_b { self.execute(*eb) }
                else { Ok(ControlFlow::Normal) }
            }
            Stmt::While(cond, body) => {
                while self.evaluate(cond.clone())?.is_truthy() {
                    match self.execute((*body).clone())? {
                        ControlFlow::Break => break,
                        ControlFlow::Return(v) => return Ok(ControlFlow::Return(v)),
                        ControlFlow::Throw(v) => return Ok(ControlFlow::Throw(v)),
                        _ => {}
                    }
                }
                Ok(ControlFlow::Normal)
            }
            Stmt::For(name, iter_expr, body) => {
                let it = self.evaluate(iter_expr)?;
                if it.is_array() {
                    let items = self.heap.get_array(it.payload()).clone();
                    for item in items {
                        self.env.push_scope(); self.env.define(name.clone(), item);
                        let res = self.execute((*body).clone())?; self.env.pop_scope();
                        match res {
                            ControlFlow::Break => break,
                            ControlFlow::Return(v) => return Ok(ControlFlow::Return(v)),
                            ControlFlow::Throw(v) => return Ok(ControlFlow::Throw(v)),
                            _ => {}
                        }
                    }
                    Ok(ControlFlow::Normal)
                } else { Err("Can only iterate over arrays right now".to_string()) }
            }
            Stmt::Function(name, params, body) => {
                let mut final_params = params;
                if name.contains('.') {
                    if final_params.first().map_or(true, |p| p != "self") { final_params.insert(0, "self".to_string()); }
                }
                let idx = self.heap.alloc(HeapObject::Function { params: final_params, body, captured: None });
                self.env.define(name, Value::function(idx));
                Ok(ControlFlow::Normal)
            }
            Stmt::Return(expr) => { let v = if let Some(e) = expr { self.evaluate(e)? } else { Value::null() }; Ok(ControlFlow::Return(v)) }
            Stmt::Break => Ok(ControlFlow::Break),
            Stmt::Continue => Ok(ControlFlow::Continue),
        }
    }

    pub fn evaluate(&mut self, expr: Expr) -> Result<Value, String> {
        match expr {
            Expr::Number(n) => Ok(Value::number(n)),
            Expr::String(s) => { let i = self.heap.alloc(HeapObject::String(s)); Ok(Value::string(i)) }
            Expr::Bool(b) => Ok(Value::bool(b)),
            Expr::Function(params, body) => {
                let snapshot: Vec<HashMap<String, Value>> = self.env.scopes.clone();
                let idx = self.heap.alloc(HeapObject::Function {
                    params, body, captured: Some(Rc::new(RefCell::new(snapshot))),
                });
                Ok(Value::function(idx))
            }
            Expr::Ident(name) => self.env.get(&name),
            Expr::InterpolatedString(parts) => {
                let mut s = String::new();
                for p in parts { s.push_str(&self.evaluate(p)?.to_string_with_heap(&self.heap)); }
                let idx = self.heap.alloc(HeapObject::String(s));
                Ok(Value::string(idx))
            }
            Expr::StructInit(name, fields) => {
                if !self.env.structs.contains_key(&name) { return Err(format!("Undefined struct '{}'", name)); }
                let mut map = HashMap::new();
                for (f, e) in fields { let v = self.evaluate(e)?; map.insert(f, v); }
                let idx = self.heap.alloc(HeapObject::Struct(name, map));
                Ok(Value::struct_val(idx))
            }
            Expr::FieldAccess(obj, field) => {
                let o = self.evaluate(*obj)?;
                if o.is_struct() {
                    let (_, fields) = self.heap.get_struct(o.payload());
                    Ok(fields.get(&field).copied().unwrap_or(Value::null()))
                } else { Ok(Value::null()) }
            }
            Expr::Array(elements) => {
                let mut arr = Vec::with_capacity(elements.len());
                for el in elements { arr.push(self.evaluate(el)?); }
                let idx = self.heap.alloc(HeapObject::Array(arr));
                Ok(Value::array(idx))
            }
            Expr::Map(pairs) => {
                let mut map = HashMap::new();
                for (k, v) in pairs {
                    let kv = self.evaluate(k)?;
                    let key = MapKey::from_value(kv, &self.heap)?;
                    let vv = self.evaluate(v)?;
                    map.insert(key, vv);
                }
                let idx = self.heap.alloc(HeapObject::Map(map));
                Ok(Value::map(idx))
            }
            Expr::Index(target, index) => {
                let t = self.evaluate(*target)?;
                let iv = self.evaluate(*index)?;
                match t.kind() {
                    ValueKind::Array(idx) => {
                        if let Some(n) = iv.as_number() {
                            let i = n as usize;
                            let arr = self.heap.get_array(idx);
                            if i < arr.len() { Ok(arr[i]) } else { Err(format!("Index {} out of range", i)) }
                        } else { Err("Array index must be a number".to_string()) }
                    }
                    ValueKind::Map(idx) => {
                        let key = MapKey::from_value(iv, &self.heap)?;
                        self.heap.get_map(idx).get(&key).copied().ok_or_else(|| "Key not found in map".to_string())
                    }
                    ValueKind::Struct(idx) => {
                        if iv.is_string() {
                            let k = self.heap.get_string(iv.payload()).clone();
                            let (_, fields) = self.heap.get_struct(idx);
                            fields.get(&k).copied().ok_or_else(|| format!("Key '{}' not found", k))
                        } else { Err("Struct index must be string".to_string()) }
                    }
                    ValueKind::String(idx) => {
                        if let Some(n) = iv.as_number() {
                            let s = self.heap.get_string(idx);
                            let i = n as usize;
                            if i < s.len() {
                                let ch = s.chars().nth(i).unwrap().to_string();
                                let sidx = self.heap.alloc(HeapObject::String(ch));
                                Ok(Value::string(sidx))
                            } else { Err(format!("String index {} out of range", i)) }
                        } else { Err("String index must be number".to_string()) }
                    }
                    _ => Err(format!("Cannot index '{}'", t.to_string_with_heap(&self.heap))),
                }
            }
            Expr::MethodCall(target, method, args) => {
                let obj = self.evaluate(*target)?;
                if obj.is_struct() {
                    let (name, _) = self.heap.get_struct(obj.payload());
                    let struct_name = name.clone();
                    let full_name = format!("{}.{}", struct_name, method);
                    let callee = self.env.get(&full_name)?;
                    if callee.is_function() {
                        let heap_obj = self.heap.get(callee.payload()).clone();
                        if let HeapObject::Function { params, body, captured } = heap_obj {
                            let mut arg_vals = vec![obj];
                            for a in args { arg_vals.push(self.evaluate(a)?); }
                            if arg_vals.len() != params.len() { return Err(format!("Method {} expects {} args", full_name, params.len())); }
                            self.invoke_function(params, body, captured, arg_vals)
                        } else { Err(format!("Method '{}' not found", full_name)) }
                    } else { Err(format!("Method '{}' not found", full_name)) }
                } else { Err("Can only call methods on structs".to_string()) }
            }
            Expr::Call(name, args) => self.call_builtin_or_user(name, args),
            Expr::Unary(op, right) => {
                let rv = self.evaluate(*right)?;
                match op {
                    TokenType::Minus => if let Some(n) = rv.as_number() { Ok(Value::number(-n)) } else { Err("Operand must be number".to_string()) },
                    TokenType::Bang => if rv.is_bool() { Ok(Value::bool(!rv.as_bool())) } else { Err("Operand must be bool".to_string()) },
                    _ => Err("Unknown unary".to_string()),
                }
            }
            Expr::Binary(left, op, right) => self.eval_binary(left, op, right),
            Expr::Ternary(c, t, e) => {
                if self.evaluate(*c)?.is_truthy() { self.evaluate(*t) } else { self.evaluate(*e) }
            }
        }
    }

    fn eval_binary(&mut self, left: Box<Expr>, op: TokenType, right: Box<Expr>) -> Result<Value, String> {
        if op == TokenType::And {
            let lv = self.evaluate(*left)?;
            if lv.is_bool() { if !lv.as_bool() { return Ok(Value::bool(false)); } let rv = self.evaluate(*right)?; if rv.is_bool() { return Ok(Value::bool(rv.as_bool())); } return Err("Operands must be bools".to_string()); }
            return Err("Operands must be bools".to_string());
        }
        if op == TokenType::Or {
            let lv = self.evaluate(*left)?;
            if lv.is_bool() { if lv.as_bool() { return Ok(Value::bool(true)); } let rv = self.evaluate(*right)?; if rv.is_bool() { return Ok(Value::bool(rv.as_bool())); } return Err("Operands must be bools".to_string()); }
            return Err("Operands must be bools".to_string());
        }
        let lv = self.evaluate(*left)?;
        let rv = self.evaluate(*right)?;
        match op {
            TokenType::Plus => {
                if let (Some(a), Some(b)) = (lv.as_number(), rv.as_number()) { return Ok(Value::number(a + b)); }
                if lv.is_string() && rv.is_string() {
                    let mut s = self.heap.get_string(lv.payload()).clone();
                    s.push_str(self.heap.get_string(rv.payload()));
                    let idx = self.heap.alloc(HeapObject::String(s));
                    return Ok(Value::string(idx));
                }
                Err("Operands must be two numbers or two strings".to_string())
            }
            TokenType::Minus | TokenType::Star | TokenType::Slash | TokenType::Div => {
                if let (Some(a), Some(b)) = (lv.as_number(), rv.as_number()) {
                    let res = match op {
                        TokenType::Minus => a - b,
                        TokenType::Star => a * b,
                        TokenType::Slash => { if b == 0.0 { return Err("Div by zero".to_string()); } a / b }
                        TokenType::Div => { if b == 0.0 { return Err("Div by zero".to_string()); } (a / b).trunc() }
                        _ => unreachable!(),
                    };
                    Ok(Value::number(res))
                } else { Err("Operands must be numbers".to_string()) }
            }
            TokenType::Gt | TokenType::GtEq | TokenType::Lt | TokenType::LtEq => {
                if let (Some(a), Some(b)) = (lv.as_number(), rv.as_number()) {
                    return Ok(Value::bool(match op { TokenType::Gt => a > b, TokenType::GtEq => a >= b, TokenType::Lt => a < b, TokenType::LtEq => a <= b, _ => unreachable!() }));
                }
                if lv.is_string() && rv.is_string() {
                    let a = self.heap.get_string(lv.payload());
                    let b = self.heap.get_string(rv.payload());
                    return Ok(Value::bool(match op { TokenType::Gt => a > b, TokenType::GtEq => a >= b, TokenType::Lt => a < b, TokenType::LtEq => a <= b, _ => unreachable!() }));
                }
                Err("Operands must be two numbers or two strings".to_string())
            }
            TokenType::EqEq => {
                if let (Some(a), Some(b)) = (lv.as_number(), rv.as_number()) { return Ok(Value::bool(a == b)); }
                if lv.is_string() && rv.is_string() { return Ok(Value::bool(self.heap.get_string(lv.payload()) == self.heap.get_string(rv.payload()))); }
                if lv.is_bool() && rv.is_bool() { return Ok(Value::bool(lv.as_bool() == rv.as_bool())); }
                Ok(Value::bool(false))
            }
            TokenType::NotEq => {
                if let (Some(a), Some(b)) = (lv.as_number(), rv.as_number()) { return Ok(Value::bool(a != b)); }
                if lv.is_string() && rv.is_string() { return Ok(Value::bool(self.heap.get_string(lv.payload()) != self.heap.get_string(rv.payload()))); }
                if lv.is_bool() && rv.is_bool() { return Ok(Value::bool(lv.as_bool() != rv.as_bool())); }
                Ok(Value::bool(true))
            }
            _ => Err("Unknown binary operator".to_string()),
        }
    }

    fn call_builtin_or_user(&mut self, name: String, args: Vec<Expr>) -> Result<Value, String> {
        let mut arg_vals: Vec<Value> = Vec::with_capacity(args.len());
        for a in args.iter() { arg_vals.push(self.evaluate(a.clone())?); }

        match name.as_str() {
            "tcp_listen" => {
                if let Some(p) = arg_vals.get(0).and_then(|v| v.as_number()) {
                    let l = TcpListener::bind(format!("127.0.0.1:{}", p)).map_err(|e| e.to_string())?;
                    let mut c = get_counter().lock().unwrap(); let id = *c; *c += 1;
                    get_servers().lock().unwrap().insert(id, l);
                    return Ok(Value::number(id as f64));
                }
                return Err("tcp_listen expects port".to_string());
            }
            "tcp_accept" => {
                if let Some(id_n) = arg_vals.get(0).and_then(|v| v.as_number()) {
                    let servers = get_servers().lock().unwrap();
                    if let Some(l) = servers.get(&(id_n as usize)) {
                        match l.accept() {
                            Ok((s, _)) => {
                                let mut c = get_counter().lock().unwrap(); let cid = *c; *c += 1;
                                get_clients().lock().unwrap().insert(cid, s);
                                return Ok(Value::number(cid as f64));
                            }
                            Err(e) => return Err(format!("accept: {}", e)),
                        }
                    }
                }
                return Err("Invalid server socket".to_string());
            }
            "tcp_read" => {
                if let Some(id_n) = arg_vals.get(0).and_then(|v| v.as_number()) {
                    let max = arg_vals.get(1).and_then(|v| v.as_number()).map(|n| n as usize).unwrap_or(4096);
                    let mut clients = get_clients().lock().unwrap();
                    if let Some(s) = clients.get_mut(&(id_n as usize)) {
                        let mut buf = vec![0; max];
                        match s.read(&mut buf) {
                            Ok(n) => { buf.truncate(n); let sidx = self.heap.alloc(HeapObject::String(String::from_utf8_lossy(&buf).to_string())); return Ok(Value::string(sidx)); }
                            Err(e) => return Err(format!("read: {}", e)),
                        }
                    }
                }
                return Err("Invalid client socket".to_string());
            }
            "tcp_write" => {
                if arg_vals.len() >= 2 {
                    if let (Some(id_n), true) = (arg_vals[0].as_number(), arg_vals[1].is_string()) {
                        let msg = self.heap.get_string(arg_vals[1].payload()).clone();
                        let mut clients = get_clients().lock().unwrap();
                        if let Some(s) = clients.get_mut(&(id_n as usize)) {
                            return s.write_all(msg.as_bytes()).map(|_| Value::null()).map_err(|e| e.to_string());
                        }
                    }
                }
                return Err("Invalid tcp_write".to_string());
            }
            "tcp_close" => {
                if let Some(id_n) = arg_vals.get(0).and_then(|v| v.as_number()) {
                    get_clients().lock().unwrap().remove(&(id_n as usize));
                    get_servers().lock().unwrap().remove(&(id_n as usize));
                    return Ok(Value::null());
                }
                return Err("Invalid socket id".to_string());
            }
            "http_get" => {
                if let Some(url_idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let url = self.heap.get_string(url_idx).clone();
                    match ureq::get(&url).call() {
                        Ok(resp) => { let s = resp.into_string().map_err(|e| e.to_string())?; let sidx = self.heap.alloc(HeapObject::String(s)); return Ok(Value::string(sidx)); }
                        Err(e) => return Err(format!("http_get: {}", e)),
                    }
                }
                return Err("http_get expects URL".to_string());
            }
            "char_code" => {
                if let Some(idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let s = self.heap.get_string(idx);
                    if s.as_bytes().len() == 1 { return Ok(Value::number(s.as_bytes()[0] as f64)); }
                }
                return Err("char_code() expects single char".to_string());
            }
            "clear_screen" => {
                print!("\x1b[2J\x1b[1;1H");
                use std::io::Write; std::io::stdout().flush().unwrap();
                return Ok(Value::null());
            }
            "str_eq" => {
                if arg_vals.len() >= 2 {
                    return Ok(Value::number(if arg_vals[0].to_string_with_heap(&self.heap) == arg_vals[1].to_string_with_heap(&self.heap) { 1.0 } else { 0.0 }));
                }
                return Err("str_eq()".to_string());
            }
            "print_char" => {
                if let Some(n) = arg_vals.get(0).and_then(|v| v.as_number()) { print!("{}", n as u8 as char); use std::io::Write; std::io::stdout().flush().unwrap(); }
                return Ok(Value::null());
            }
            "print_str" => {
                if let Some(v) = arg_vals.get(0) { print!("{}", v.to_string_with_heap(&self.heap)); use std::io::Write; std::io::stdout().flush().unwrap(); }
                return Ok(Value::null());
            }
            "read_line" => {
                let mut input = String::new();
                std::io::stdin().read_line(&mut input).unwrap();
                let sidx = self.heap.alloc(HeapObject::String(input.trim().to_string()));
                return Ok(Value::string(sidx));
            }
            "random" => {
                let max = arg_vals.get(0).and_then(|v| v.as_number()).map(|n| n as u64).unwrap_or(100);
                use std::time::{SystemTime, UNIX_EPOCH};
                let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().subsec_nanos() as u64;
                return Ok(Value::number(((nanos * 1103515245 + 12345) % max) as f64));
            }
            "list_dir" => {
                if let Some(p_idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let p = self.heap.get_string(p_idx).clone();
                    let mut files = Vec::new();
                    if let Ok(entries) = fs::read_dir(&p) {
                        for entry in entries.flatten() {
                            if let Ok(name) = entry.file_name().into_string() {
                                let i = self.heap.alloc(HeapObject::String(name));
                                files.push(Value::string(i));
                            }
                        }
                    }
                    let idx = self.heap.alloc(HeapObject::Array(files));
                    return Ok(Value::array(idx));
                }
                return Err("list_dir() expects string".to_string());
            }
            "mkdir" => {
                if let Some(p_idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let p = self.heap.get_string(p_idx).clone();
                    let _ = fs::create_dir_all(&p);
                    return Ok(Value::null());
                }
                return Err("mkdir() expects string".to_string());
            }
            "len" => {
                if let Some(v) = arg_vals.get(0) {
                    if v.is_string() { return Ok(Value::number(self.heap.get_string(v.payload()).len() as f64)); }
                    if v.is_array() { return Ok(Value::number(self.heap.get_array(v.payload()).len() as f64)); }
                    if v.is_map() { return Ok(Value::number(self.heap.get_map(v.payload()).len() as f64)); }
                }
                return Err("len() expects collection".to_string());
            }
            "push" => {
                if arg_vals.len() >= 2 {
                    if arg_vals[0].is_array() {
                        let idx = arg_vals[0].payload();
                        let v = arg_vals[1];
                        self.heap.get_array_mut(idx).push(v);
                        return Ok(Value::null());
                    }
                }
                return Err("push() expects array".to_string());
            }
            "pop" => {
                if let Some(v) = arg_vals.get(0) {
                    if v.is_array() {
                        let idx = v.payload();
                        return Ok(self.heap.get_array_mut(idx).pop().unwrap_or(Value::null()));
                    }
                }
                return Err("pop() expects array".to_string());
            }
            "input" => {
                let prompt = arg_vals.get(0).filter(|v| v.is_string()).map(|v| self.heap.get_string(v.payload()).clone()).unwrap_or_default();
                print!("{}", prompt);
                use std::io::Write; std::io::stdout().flush().unwrap();
                let mut input = String::new();
                std::io::stdin().read_line(&mut input).unwrap();
                let sidx = self.heap.alloc(HeapObject::String(input.trim().to_string()));
                return Ok(Value::string(sidx));
            }
            "str" => {
                if let Some(v) = arg_vals.get(0) {
                    let s = v.to_string_with_heap(&self.heap);
                    let sidx = self.heap.alloc(HeapObject::String(s));
                    return Ok(Value::string(sidx));
                }
                return Err("str()".to_string());
            }
            "num" => {
                if let Some(v) = arg_vals.get(0) {
                    if let Some(n) = v.as_number() { return Ok(Value::number(n)); }
                    if v.is_string() {
                        let s = self.heap.get_string(v.payload());
                        if let Ok(n) = s.parse::<f64>() { return Ok(Value::number(n)); }
                        return Err(format!("num() cannot parse {:?}", s));
                    }
                }
                return Err("num() expects string or number".to_string());
            }
            "type" => {
                let t = arg_vals.get(0).map(|v| v.kind()).map(|k| match k {
                    ValueKind::Number(_) => "number", ValueKind::String(_) => "string", ValueKind::Bool(_) => "bool",
                    ValueKind::Array(_) => "array", ValueKind::Map(_) => "map", ValueKind::Struct(_) => "struct",
                    ValueKind::Function(_) | ValueKind::Closure(_) => "function", ValueKind::Null => "null",
                }).unwrap_or("null");
                let sidx = self.heap.alloc(HeapObject::String(t.to_string()));
                return Ok(Value::string(sidx));
            }
            "upper" | "lower" | "trim" => {
                if let Some(v) = arg_vals.get(0) {
                    if v.is_string() {
                        let s = self.heap.get_string(v.payload());
                        let out = match name.as_str() { "upper" => s.to_uppercase(), "lower" => s.to_lowercase(), _ => s.trim().to_string() };
                        let sidx = self.heap.alloc(HeapObject::String(out));
                        return Ok(Value::string(sidx));
                    }
                }
                return Err(format!("{}()", name));
            }
            "replace" => {
                if arg_vals.len() >= 3 && arg_vals[0].is_string() && arg_vals[1].is_string() && arg_vals[2].is_string() {
                    let s = self.heap.get_string(arg_vals[0].payload()).clone();
                    let o = self.heap.get_string(arg_vals[1].payload()).clone();
                    let n = self.heap.get_string(arg_vals[2].payload()).clone();
                    let out = s.replace(&o, &n);
                    let sidx = self.heap.alloc(HeapObject::String(out));
                    return Ok(Value::string(sidx));
                }
                return Err("replace()".to_string());
            }
            "contains" => {
                if arg_vals.len() >= 2 {
                    let t = arg_vals[0]; let i = arg_vals[1];
                    if t.is_string() && i.is_string() {
                        return Ok(Value::bool(self.heap.get_string(t.payload()).contains(self.heap.get_string(i.payload()).as_str())));
                    }
                    if t.is_array() {
                        let arr = self.heap.get_array(t.payload());
                        let needle = i.to_string_with_heap(&self.heap);
                        return Ok(Value::bool(arr.iter().any(|x| x.to_string_with_heap(&self.heap) == needle)));
                    }
                    if t.is_map() {
                        if let Ok(k) = MapKey::from_value(i, &self.heap) {
                            return Ok(Value::bool(self.heap.get_map(t.payload()).contains_key(&k)));
                        }
                    }
                }
                return Err("contains()".to_string());
            }
            "split" => {
                if arg_vals.len() >= 2 && arg_vals[0].is_string() && arg_vals[1].is_string() {
                    let s = self.heap.get_string(arg_vals[0].payload()).clone();
                    let sep = self.heap.get_string(arg_vals[1].payload()).clone();
                    if sep.is_empty() { return Err("split() sep empty".to_string()); }
                    let parts: Vec<Value> = s.split(&sep).map(|p| { let i = self.heap.alloc(HeapObject::String(p.to_string())); Value::string(i) }).collect();
                    let idx = self.heap.alloc(HeapObject::Array(parts));
                    return Ok(Value::array(idx));
                }
                return Err("split()".to_string());
            }
            "join" => {
                if arg_vals.len() >= 2 && arg_vals[0].is_array() && arg_vals[1].is_string() {
                    let sep = self.heap.get_string(arg_vals[1].payload()).clone();
                    let arr = self.heap.get_array(arg_vals[0].payload()).clone();
                    let mut parts = Vec::new();
                    for item in arr.iter() {
                        if item.is_string() { parts.push(self.heap.get_string(item.payload()).clone()); }
                        else { return Err("join() expects strings".to_string()); }
                    }
                    let sidx = self.heap.alloc(HeapObject::String(parts.join(&sep)));
                    return Ok(Value::string(sidx));
                }
                return Err("join()".to_string());
            }
            "keys" => {
                if let Some(v) = arg_vals.get(0) {
                    if v.is_map() {
                        // FIX: snapshot the keys into an owned Vec<MapKey> first,
                        // releasing the immutable heap borrow before we start
                        // allocating Strings in the loop body.
                        let key_list: Vec<MapKey> = self.heap.get_map(v.payload()).keys().cloned().collect();
                        let mut keys: Vec<Value> = Vec::with_capacity(key_list.len());
                        for k in key_list {
                            match k {
                                MapKey::String(s) => { let i = self.heap.alloc(HeapObject::String(s)); keys.push(Value::string(i)); }
                                MapKey::Number(n) => keys.push(Value::number(f64::from_bits(n as u64))),
                                MapKey::Bool(b) => keys.push(Value::bool(b)),
                            }
                        }
                        let idx = self.heap.alloc(HeapObject::Array(keys));
                        return Ok(Value::array(idx));
                    }
                }
                return Err("keys() expects map".to_string());
            }
            "read_file" => {
                if let Some(p_idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let p = self.heap.get_string(p_idx).clone();
                    let s = fs::read_to_string(&p).map_err(|e| format!("read_file: {}", e))?;
                    let sidx = self.heap.alloc(HeapObject::String(s));
                    return Ok(Value::string(sidx));
                }
                return Err("read_file()".to_string());
            }
            "write_file" => {
                if arg_vals.len() >= 2 && arg_vals[0].is_string() && arg_vals[1].is_string() {
                    let p = self.heap.get_string(arg_vals[0].payload()).clone();
                    let c = self.heap.get_string(arg_vals[1].payload()).clone();
                    fs::write(&p, c).map_err(|e| format!("write_file: {}", e))?;
                    return Ok(Value::null());
                }
                return Err("write_file()".to_string());
            }
            "file_exists" => {
                if let Some(p_idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let p = self.heap.get_string(p_idx).clone();
                    return Ok(Value::bool(Path::new(&p).exists()));
                }
                return Err("file_exists()".to_string());
            }
            "is_dir" => {
                if let Some(p_idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let p = self.heap.get_string(p_idx).clone();
                    return Ok(Value::bool(Path::new(&p).is_dir()));
                }
                return Err("is_dir() expects string".to_string());
            }
            "now" => {
                use std::time::{SystemTime, UNIX_EPOCH};
                let secs = SystemTime::now().duration_since(UNIX_EPOCH)
                    .map(|d| d.as_secs() as f64).unwrap_or(0.0);
                return Ok(Value::number(secs));
            }
            "byte_at" => {
                if arg_vals.len() >= 2 && arg_vals[0].is_string() {
                    if let Some(n) = arg_vals[1].as_number() {
                        let s = self.heap.get_string(arg_vals[0].payload());
                        let bytes = s.as_bytes();
                        let idx = n as isize;
                        if idx < 0 || idx as usize >= bytes.len() { return Ok(Value::number(-1.0)); }
                        return Ok(Value::number(bytes[idx as usize] as f64));
                    }
                }
                return Err("byte_at()".to_string());
            }
            "byte_substr" => {
                if arg_vals.len() >= 3 && arg_vals[0].is_string() {
                    if let (Some(a_n), Some(b_n)) = (arg_vals[1].as_number(), arg_vals[2].as_number()) {
                        let s = self.heap.get_string(arg_vals[0].payload()).clone();
                        let bytes = s.as_bytes();
                        let mut start = if a_n < 0.0 { 0 } else { a_n as usize };
                        let mut end = if b_n < 0.0 { 0 } else { b_n as usize };
                        if start > bytes.len() { start = bytes.len(); }
                        if end > bytes.len() { end = bytes.len(); }
                        if start > end { start = end; }
                        let out = String::from_utf8_lossy(&bytes[start..end]).to_string();
                        let sidx = self.heap.alloc(HeapObject::String(out));
                        return Ok(Value::string(sidx));
                    }
                }
                return Err("byte_substr()".to_string());
            }
            "bytes_to_str" => {
                if let Some(v) = arg_vals.get(0) {
                    if v.is_array() {
                        let arr = self.heap.get_array(v.payload()).clone();
                        let mut buf = Vec::with_capacity(arr.len());
                        for item in arr.iter() {
                            if let Some(n) = item.as_number() {
                                let b = n as i64;
                                if !(0..=255).contains(&b) { return Err(format!("byte out of range: {}", b)); }
                                buf.push(b as u8);
                            } else { return Err("bytes_to_str()".to_string()); }
                        }
                        let sidx = self.heap.alloc(HeapObject::String(String::from_utf8_lossy(&buf).to_string()));
                        return Ok(Value::string(sidx));
                    }
                }
                return Err("bytes_to_str()".to_string());
            }
            "system" => {
                if let Some(idx) = arg_vals.get(0).filter(|v| v.is_string()).map(|v| v.payload()) {
                    let cmd = self.heap.get_string(idx).clone();
                    #[cfg(windows)]
                    let output = std::process::Command::new("cmd").args(&["/C", &cmd]).output();
                    #[cfg(not(windows))]
                    let output = std::process::Command::new("sh").args(&["-c", &cmd]).output();
                    match output {
                        Ok(out) => {
                            let mut s = String::from_utf8_lossy(&out.stdout).to_string();
                            s.push_str(&String::from_utf8_lossy(&out.stderr));
                            let sidx = self.heap.alloc(HeapObject::String(s));
                            return Ok(Value::string(sidx));
                        }
                        Err(e) => return Err(format!("system() failed: {}", e)),
                    }
                }
                return Err("system() expects string".to_string());
            }
            _ => {}
        }

        // User function call
        let callee = self.env.get(&name)?;
        if callee.is_function() {
            let heap_obj = self.heap.get(callee.payload()).clone();
            if let HeapObject::Function { params, body, captured } = heap_obj {
                if arg_vals.len() != params.len() { return Err(format!("{}() expects {} args, got {}", name, params.len(), arg_vals.len())); }
                return self.invoke_function(params, body, captured, arg_vals);
            }
        }
        Err(format!("'{}' is not a function", name))
    }

    fn invoke_function(
        &mut self,
        params: Vec<String>,
        body: Box<Stmt>,
        captured: Option<Rc<RefCell<Vec<HashMap<String, Value>>>>>,
        arg_vals: Vec<Value>,
    ) -> Result<Value, String> {
        let caller_scopes = std::mem::take(&mut self.env.scopes);
        match &captured {
            Some(env) => { self.env.scopes = env.borrow().clone(); }
            None => {
                let mut fresh = vec![HashMap::new()];
                if let Some(g) = caller_scopes.get(0) { fresh[0] = g.clone(); }
                self.env.scopes = fresh;
            }
        }
        self.env.push_scope();
        for (p, v) in params.iter().zip(arg_vals) { self.env.define(p.clone(), v); }
        let result = self.execute(*body);
        self.env.scopes.pop();
        let body_scopes = std::mem::replace(&mut self.env.scopes, caller_scopes);
        if let Some(env) = &captured { *env.borrow_mut() = body_scopes; }
        match result? {
            ControlFlow::Return(val) => Ok(val),
            ControlFlow::Throw(val) => Err(val.to_string_with_heap(&self.heap)),
            _ => Ok(Value::null()),
        }
    }
}