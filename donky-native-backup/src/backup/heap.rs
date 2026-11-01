use crate::ast::Stmt;
use std::collections::HashMap;
use std::rc::Rc;
use std::cell::RefCell;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum MapKey { String(String), Number(usize), Bool(bool) }

impl MapKey {
    pub fn from_value(v: &Value) -> Result<MapKey, String> {
        match v {
            Value::String(s) => Ok(MapKey::String(s.clone())),
            Value::Number(n) => Ok(MapKey::Number(n.to_bits() as usize)),
            Value::Bool(b) => Ok(MapKey::Bool(*b)),
            _ => Err("Map keys must be strings, numbers, or bools".to_string()),
        }
    }
}

/// `repr(C, u8)`:
///   - C: field layout matches C structs (stable, predictable field offsets).
///   - u8: discriminant is exactly 1 byte at offset 0.
/// Gives LLVM a stable "tag at offset 0, u8-sized" contract so pattern matches
/// compile down to a single-byte load + jump table instead of guessing.
#[repr(C, u8)]
#[derive(Debug, Clone)]
pub enum Value {
    Number(f64),
    String(String),
    Bool(bool),
    Function {
        params: Vec<String>,
        body: Box<Stmt>,
        captured: Option<Rc<RefCell<Vec<HashMap<String, Value>>>>>,
    },
    Closure(usize, usize, usize, Rc<RefCell<Vec<Value>>>),
    Array(usize),
    Map(usize),
    Struct(usize),
    Null,
}

impl Value {
    pub fn is_truthy(&self) -> bool { matches!(self, Value::Bool(true)) }
    pub fn to_string(&self) -> String {
        match self {
            Value::Number(n) => if n.fract() == 0.0 { format!("{:.0}", n) } else { n.to_string() },
            Value::String(s) => s.clone(),
            Value::Bool(b) => b.to_string(),
            Value::Function { .. } => "<fn>".to_string(),
            Value::Closure(..) => "<fn>".to_string(),
            Value::Array(idx) => format!("<array:{}>", idx),
            Value::Map(idx) => format!("<map:{}>", idx),
            Value::Struct(idx) => format!("<struct:{}>", idx),
            Value::Null => "null".to_string(),
        }
    }
}

#[derive(Debug, Clone)]
pub enum HeapObject {
    Array(Vec<Value>),
    Map(HashMap<MapKey, Value>),
    Struct(String, HashMap<String, Value>),
}

pub struct Heap {
    pub objects: Vec<Option<HeapObject>>,
    marks: Vec<bool>,
    next_gc: usize,
}

impl Heap {
    pub fn new() -> Self { Heap { objects: Vec::new(), marks: Vec::new(), next_gc: 1024 } }
    pub fn needs_gc(&self) -> bool { self.objects.len() >= self.next_gc }
    pub fn alloc(&mut self, obj: HeapObject) -> usize {
        let idx = self.objects.len();
        self.objects.push(Some(obj));
        self.marks.push(false);
        idx
    }
    pub fn get(&self, idx: usize) -> &HeapObject { self.objects[idx].as_ref().unwrap() }
    pub fn get_mut(&mut self, idx: usize) -> &mut HeapObject { self.objects[idx].as_mut().unwrap() }

    pub fn collect_garbage(&mut self, roots: &[Value]) {
        for mark in self.marks.iter_mut() { *mark = false; }
        for root in roots { self.mark_value(root); }
        let mut freed = 0;
        for i in 0..self.objects.len() {
            if !self.marks[i] && self.objects[i].is_some() {
                self.objects[i] = None;
                freed += 1;
            }
        }
        self.next_gc = std::cmp::max(1024, self.objects.len() * 2);
        println!("[GC] Swept {} objects. Heap size: {}", freed, self.objects.len());
    }

    fn mark_value(&mut self, val: &Value) {
        match val {
            Value::Array(idx) | Value::Map(idx) | Value::Struct(idx) => {
                self.mark_object(*idx);
            }
            Value::Function { captured: Some(env), .. } => {
                let vals: Vec<Value> = env.borrow().iter().flat_map(|s| s.values().cloned()).collect();
                for v in vals { self.mark_value(&v); }
            }
            Value::Closure(_, _, _, captured) => {
                let vals: Vec<Value> = captured.borrow().clone();
                for v in vals { self.mark_value(&v); }
            }
            _ => {}
        }
    }

    fn mark_object(&mut self, idx: usize) {
        if idx >= self.objects.len() || self.marks[idx] || self.objects[idx].is_none() { return; }
        self.marks[idx] = true;
        let obj = self.objects[idx].as_ref().unwrap().clone();
        match obj {
            HeapObject::Array(arr) => { for v in arr { self.mark_value(&v); } }
            HeapObject::Map(map) => { for v in map.values() { self.mark_value(v); } }
            HeapObject::Struct(_, fields) => { for v in fields.values() { self.mark_value(v); } }
        }
    }
}
