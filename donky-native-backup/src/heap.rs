use crate::ast::Stmt;
use std::collections::HashMap;
use std::rc::Rc;
use std::cell::RefCell;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum MapKey { String(String), Number(usize), Bool(bool) }

impl MapKey {
    pub fn from_value(v: Value, heap: &Heap) -> Result<MapKey, String> {
        if v.is_number() { Ok(MapKey::Number(v.as_f64().to_bits() as usize)) }
        else if v.is_bool() { Ok(MapKey::Bool(v.as_bool())) }
        else if v.is_string() { Ok(MapKey::String(heap.get_string(v.payload()).clone())) }
        else { Err("Map keys must be strings, numbers, or bools".to_string()) }
    }
}

// ===================== NaN-boxed Value =====================

#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct Value(u64);

#[derive(Debug, Clone, Copy)]
pub enum ValueKind {
    Number(f64),
    Bool(bool),
    Null,
    String(u32),
    Array(u32),
    Map(u32),
    Struct(u32),
    Function(u32),
    Closure(u32),
}

impl Value {
    const TAG_MASK: u64 = 0xFFF8_0000_0000_0000;
    const TAG_BASE: u64 = 0x7FF8_0000_0000_0000;
    const PAYLOAD_MASK: u64 = 0x0000_FFFF_FFFF_FFFF;
    const CANONICAL_NAN: u64 = 0x7FF7_FFFF_FFFF_FFFF;

    const TAG_NULL: u64 = 0;
    const TAG_BOOL: u64 = 1;
    const TAG_STRING: u64 = 2;
    const TAG_ARRAY: u64 = 3;
    const TAG_MAP: u64 = 4;
    const TAG_STRUCT: u64 = 5;
    const TAG_FUNCTION: u64 = 6;
    const TAG_CLOSURE: u64 = 7;

    #[inline] pub fn null() -> Self { Value(Self::TAG_BASE) }
    #[inline] pub fn bool(b: bool) -> Self { Value(Self::TAG_BASE | (Self::TAG_BOOL << 48) | (b as u64)) }
    #[inline] pub fn number(n: f64) -> Self {
        let bits = n.to_bits();
        if (bits & Self::TAG_MASK) == Self::TAG_BASE { Value(Self::CANONICAL_NAN) } else { Value(bits) }
    }
    #[inline] pub fn string(idx: u32) -> Self { Value(Self::TAG_BASE | (Self::TAG_STRING << 48) | (idx as u64)) }
    #[inline] pub fn array(idx: u32) -> Self { Value(Self::TAG_BASE | (Self::TAG_ARRAY << 48) | (idx as u64)) }
    #[inline] pub fn map(idx: u32) -> Self { Value(Self::TAG_BASE | (Self::TAG_MAP << 48) | (idx as u64)) }
    #[inline] pub fn struct_val(idx: u32) -> Self { Value(Self::TAG_BASE | (Self::TAG_STRUCT << 48) | (idx as u64)) }
    #[inline] pub fn function(idx: u32) -> Self { Value(Self::TAG_BASE | (Self::TAG_FUNCTION << 48) | (idx as u64)) }
    #[inline] pub fn closure(idx: u32) -> Self { Value(Self::TAG_BASE | (Self::TAG_CLOSURE << 48) | (idx as u64)) }

    #[inline] fn is_tagged(self) -> bool { (self.0 & Self::TAG_MASK) == Self::TAG_BASE }
    #[inline] fn tag(self) -> u64 { (self.0 >> 48) & 0x7 }
    #[inline] pub fn payload(self) -> u32 { (self.0 & Self::PAYLOAD_MASK) as u32 }

    #[inline] pub fn is_null(self) -> bool { self.0 == Self::TAG_BASE }
    #[inline] pub fn is_number(self) -> bool { !self.is_tagged() }
    #[inline] pub fn is_bool(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_BOOL }
    #[inline] pub fn is_string(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_STRING }
    #[inline] pub fn is_array(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_ARRAY }
    #[inline] pub fn is_map(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_MAP }
    #[inline] pub fn is_struct(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_STRUCT }
    #[inline] pub fn is_function(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_FUNCTION }
    #[inline] pub fn is_closure(self) -> bool { self.is_tagged() && self.tag() == Self::TAG_CLOSURE }

    #[inline] pub fn as_f64(self) -> f64 { f64::from_bits(self.0) }

    #[inline] pub fn as_number(self) -> Option<f64> {
        if self.is_number() { Some(f64::from_bits(self.0)) } else { None }
    }

    #[inline] pub fn as_bool(self) -> bool { (self.0 & 1) == 1 }
    #[inline] pub fn as_index(self) -> u32 { self.payload() }

    pub fn is_truthy(self) -> bool { self.is_bool() && self.as_bool() }

    pub fn kind(self) -> ValueKind {
        if self.is_number() { return ValueKind::Number(self.as_f64()); }
        if self.is_null() { return ValueKind::Null; }
        if self.is_bool() { return ValueKind::Bool(self.as_bool()); }
        match self.tag() {
            Self::TAG_STRING => ValueKind::String(self.payload()),
            Self::TAG_ARRAY => ValueKind::Array(self.payload()),
            Self::TAG_MAP => ValueKind::Map(self.payload()),
            Self::TAG_STRUCT => ValueKind::Struct(self.payload()),
            Self::TAG_FUNCTION => ValueKind::Function(self.payload()),
            Self::TAG_CLOSURE => ValueKind::Closure(self.payload()),
            _ => unreachable!(),
        }
    }

    pub fn to_string_with_heap(self, heap: &Heap) -> String {
        self.render(heap, 0)
    }

    fn render(self, heap: &Heap, depth: usize) -> String {
        const MAX_DEPTH: usize = 8;
        match self.kind() {
            ValueKind::Number(n) => if n.fract() == 0.0 { format!("{:.0}", n) } else { n.to_string() },
            ValueKind::Bool(b) => b.to_string(),
            ValueKind::Null => "null".to_string(),
            ValueKind::String(idx) => heap.get_string(idx).clone(),
            ValueKind::Array(idx) => {
                if depth >= MAX_DEPTH { return "[...]".to_string(); }
                let items: Vec<String> = heap.get_array(idx).iter()
                    .map(|v| v.render(heap, depth + 1))
                    .collect();
                format!("[{}]", items.join(", "))
            }
            ValueKind::Map(idx) => {
                if depth >= MAX_DEPTH { return "{...}".to_string(); }
                let mut entries: Vec<(String, String)> = heap.get_map(idx).iter()
                    .map(|(k, v)| {
                        let key_str = match k {
                            MapKey::String(s) => format!("\"{}\"", s),
                            MapKey::Number(n) => format!("{}", f64::from_bits(*n as u64)),
                            MapKey::Bool(b) => b.to_string(),
                        };
                        (key_str, v.render(heap, depth + 1))
                    })
                    .collect();
                entries.sort_by(|a, b| a.0.cmp(&b.0));
                let parts: Vec<String> = entries.into_iter()
                    .map(|(k, v)| format!("{}: {}", k, v))
                    .collect();
                format!("{{{}}}", parts.join(", "))
            }
            ValueKind::Struct(idx) => {
                if depth >= MAX_DEPTH { return "<struct:{...}>".to_string(); }
                let (name, fields) = heap.get_struct(idx);
                let mut entries: Vec<(String, String)> = fields.iter()
                    .map(|(k, v)| (k.clone(), v.render(heap, depth + 1)))
                    .collect();
                entries.sort_by(|a, b| a.0.cmp(&b.0));
                let parts: Vec<String> = entries.into_iter()
                    .map(|(k, v)| format!("{}: {}", k, v))
                    .collect();
                format!("{}{{{}}}", name, parts.join(", "))
            }
            ValueKind::Function(_) | ValueKind::Closure(_) => "<fn>".to_string(),
        }
    }
}

impl std::fmt::Debug for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { write!(f, "{:?}", self.kind()) }
}

impl PartialEq for Value {
    fn eq(&self, other: &Self) -> bool {
        if self.is_number() && other.is_number() { self.as_f64() == other.as_f64() }
        else { self.0 == other.0 }
    }
}

// ===================== Heap =====================

pub enum HeapObject {
    String(String),
    Array(Vec<Value>),
    Map(HashMap<MapKey, Value>),
    Struct(String, HashMap<String, Value>),
    CodeFunction(u32),
    Function {
        params: Vec<String>,
        body: Box<Stmt>,
        captured: Option<Rc<RefCell<Vec<HashMap<String, Value>>>>>,
    },
    Closure {
        func_idx: u32,
        num_params: u32,
        num_locals: u32,
        captured: Rc<RefCell<Vec<Value>>>,
    },
}

impl Clone for HeapObject {
    fn clone(&self) -> Self {
        match self {
            HeapObject::String(s) => HeapObject::String(s.clone()),
            HeapObject::Array(v) => HeapObject::Array(v.clone()),
            HeapObject::Map(m) => HeapObject::Map(m.clone()),
            HeapObject::Struct(n, f) => HeapObject::Struct(n.clone(), f.clone()),
            HeapObject::CodeFunction(i) => HeapObject::CodeFunction(*i),
            HeapObject::Function { params, body, captured } =>
                HeapObject::Function { params: params.clone(), body: body.clone(), captured: captured.clone() },
            HeapObject::Closure { func_idx, num_params, num_locals, captured } =>
                HeapObject::Closure { func_idx: *func_idx, num_params: *num_params, num_locals: *num_locals, captured: captured.clone() },
        }
    }
}

pub struct Heap {
    pub objects: Vec<Option<HeapObject>>,
    marks: Vec<bool>,
    next_gc: usize,
}

impl Heap {
    pub fn new() -> Self { Heap { objects: Vec::new(), marks: Vec::new(), next_gc: 1024 } }
    pub fn needs_gc(&self) -> bool { self.objects.len() >= self.next_gc }

    pub fn alloc(&mut self, obj: HeapObject) -> u32 {
        let idx = self.objects.len() as u32;
        self.objects.push(Some(obj));
        self.marks.push(false);
        idx
    }

    pub fn get(&self, idx: u32) -> &HeapObject { self.objects[idx as usize].as_ref().unwrap() }
    pub fn get_mut(&mut self, idx: u32) -> &mut HeapObject { self.objects[idx as usize].as_mut().unwrap() }

    pub fn get_string(&self, idx: u32) -> &String {
        if let HeapObject::String(s) = self.get(idx) { s } else { panic!("heap[{}] is not a string", idx) }
    }
    pub fn get_array(&self, idx: u32) -> &Vec<Value> {
        if let HeapObject::Array(a) = self.get(idx) { a } else { panic!("heap[{}] is not an array", idx) }
    }
    pub fn get_array_mut(&mut self, idx: u32) -> &mut Vec<Value> {
        if let HeapObject::Array(a) = self.get_mut(idx) { a } else { panic!("heap[{}] is not an array", idx) }
    }
    pub fn get_map(&self, idx: u32) -> &HashMap<MapKey, Value> {
        if let HeapObject::Map(m) = self.get(idx) { m } else { panic!("heap[{}] is not a map", idx) }
    }
    pub fn get_map_mut(&mut self, idx: u32) -> &mut HashMap<MapKey, Value> {
        if let HeapObject::Map(m) = self.get_mut(idx) { m } else { panic!("heap[{}] is not a map", idx) }
    }
    pub fn get_struct(&self, idx: u32) -> (&String, &HashMap<String, Value>) {
        if let HeapObject::Struct(n, f) = self.get(idx) { (n, f) } else { panic!("heap[{}] is not a struct", idx) }
    }
    pub fn get_struct_mut(&mut self, idx: u32) -> &mut HashMap<String, Value> {
        if let HeapObject::Struct(_, f) = self.get_mut(idx) { f } else { panic!("heap[{}] is not a struct", idx) }
    }

    pub fn collect_garbage(&mut self, roots: &[Value]) {
        for m in self.marks.iter_mut() { *m = false; }
        for r in roots { self.mark_value(*r); }
        let mut freed = 0;
        for i in 0..self.objects.len() {
            if !self.marks[i] && self.objects[i].is_some() {
                self.objects[i] = None; freed += 1;
            }
        }
        self.next_gc = std::cmp::max(1024, self.objects.len() * 2);
        if std::env::var("DONKY_GC_LOG").is_ok() {
            println!("[GC] Swept {} objects. Heap size: {}", freed, self.objects.len());
        }
    }

    fn mark_value(&mut self, v: Value) {
        if v.is_number() || v.is_null() || v.is_bool() { return; }
        self.mark_object(v.payload());
    }

    fn mark_object(&mut self, idx: u32) {
        let i = idx as usize;
        if i >= self.objects.len() || self.marks[i] || self.objects[i].is_none() { return; }
        self.marks[i] = true;
        let obj = self.objects[i].as_ref().unwrap().clone();
        match obj {
            HeapObject::String(_) | HeapObject::CodeFunction(_) => {}
            HeapObject::Array(arr) => { for v in arr { self.mark_value(v); } }
            HeapObject::Map(map) => { for v in map.values() { self.mark_value(*v); } }
            HeapObject::Struct(_, fields) => { for v in fields.values() { self.mark_value(*v); } }
            HeapObject::Function { captured, .. } => {
                if let Some(env) = captured {
                    let vals: Vec<Value> = env.borrow().iter().flat_map(|s| s.values().copied()).collect();
                    for v in vals { self.mark_value(v); }
                }
            }
            HeapObject::Closure { captured, .. } => {
                let vals: Vec<Value> = captured.borrow().clone();
                for v in vals { self.mark_value(v); }
            }
        }
    }
}