// profiler.rs — instruction-level profiler for the Donky VM
//
// The hot path (`on_instruction`) used to format!("{:?}") the opcode, join the whole call stack
// into a String and hash both — several allocations per executed instruction, so profiling ran
// 50-100x slower than normal execution and skewed what it measured. Now:
//   * opcode names are resolved once per opcode *kind* (cached by discriminant),
//   * the current frame key is rebuilt only on call/return, not per instruction,
//   * per-instruction work is two integer-keyed hash updates and no allocation.
use std::collections::HashMap;
use std::mem::{discriminant, Discriminant};
use crate::vm::OpCode;

pub struct ProfileReport {
    pub call_counts: HashMap<String, u64>,
    pub call_stack: Vec<String>,
    pub total_instructions: u64,

    names: HashMap<Discriminant<OpCode>, String>,
    op_counts: HashMap<Discriminant<OpCode>, u64>,
    frame_ids: HashMap<String, usize>,          // frame key -> index into frames
    frames: Vec<(String, HashMap<Discriminant<OpCode>, u64>)>,
    current_frame: usize,
}

impl ProfileReport {
    pub fn new() -> Self {
        let mut r = ProfileReport {
            call_counts: HashMap::new(),
            call_stack: Vec::new(),
            total_instructions: 0,
            names: HashMap::new(),
            op_counts: HashMap::new(),
            frame_ids: HashMap::new(),
            frames: Vec::new(),
            current_frame: 0,
        };
        r.current_frame = r.intern_frame("main".to_string());
        r
    }

    fn intern_frame(&mut self, key: String) -> usize {
        if let Some(&i) = self.frame_ids.get(&key) { return i; }
        let i = self.frames.len();
        self.frames.push((key.clone(), HashMap::new()));
        self.frame_ids.insert(key, i);
        i
    }

    fn refresh_frame(&mut self) {
        let key = if self.call_stack.is_empty() { "main".to_string() } else { self.call_stack.join(";") };
        self.current_frame = self.intern_frame(key);
    }

    pub fn on_instruction(&mut self, op: &OpCode) {
        self.total_instructions += 1;
        let d = discriminant(op);
        if !self.names.contains_key(&d) {
            self.names.insert(d, opcode_name(op)); // allocates once per opcode kind
        }
        *self.op_counts.entry(d).or_insert(0) += 1;
        *self.frames[self.current_frame].1.entry(d).or_insert(0) += 1;
    }

    pub fn push_frame(&mut self, name: String) {
        *self.call_counts.entry(name.clone()).or_insert(0) += 1;
        self.call_stack.push(name);
        self.refresh_frame();
    }

    pub fn pop_frame(&mut self) {
        self.call_stack.pop();
        self.refresh_frame();
    }

    fn name_of(&self, d: &Discriminant<OpCode>) -> String {
        self.names.get(d).cloned().unwrap_or_else(|| "?".to_string())
    }

    pub fn ascii_report(&self) -> String {
        let mut out = String::new();
        out.push_str("=== Opcode heatmap ===\n");
        if self.total_instructions == 0 {
            out.push_str("  (no instructions executed)\n");
            return out;
        }
        let mut entries: Vec<(String, u64)> = self.op_counts.iter().map(|(d, c)| (self.name_of(d), *c)).collect();
        entries.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
        let total = self.total_instructions as f64;
        for (name, count) in entries.iter().take(20) {
            let pct = (*count as f64) / total * 100.0;
            let bar_len = ((pct * 0.5) as usize).max(1);
            let bar: String = "█".repeat(bar_len);
            out.push_str(&format!("  {:<26} {:>5.1}%  {}\n", name, pct, bar));
        }

        out.push_str("\n=== Function call counts ===\n");
        if self.call_counts.is_empty() {
            out.push_str("  (no user function calls)\n");
        } else {
            let mut calls: Vec<(&String, &u64)> = self.call_counts.iter().collect();
            calls.sort_by(|a, b| b.1.cmp(a.1).then_with(|| a.0.cmp(b.0)));
            for (name, count) in calls.iter().take(10) {
                out.push_str(&format!("  {:<26} {} calls\n", name, count));
            }
        }

        out.push_str("\n=== Hottest frames ===\n");
        let mut frames: Vec<(&String, u64)> = self.frames.iter()
            .map(|(k, ops)| (k, ops.values().sum::<u64>()))
            .filter(|(_, n)| *n > 0)
            .collect();
        frames.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(b.0)));
        for (frame, count) in frames.iter().take(10) {
            let pct = (*count as f64) / total * 100.0;
            out.push_str(&format!("  {:<40} {:>5.1}%  ({} ops)\n", frame, pct, count));
        }

        out.push_str(&format!("\nTotal instructions executed: {}\n", self.total_instructions));
        out
    }

    pub fn folded_stacks(&self) -> String {
        // Brendan Gregg folded format: "a;b;c 123" (one per line), sorted so runs diff cleanly.
        let mut lines: Vec<String> = Vec::new();
        for (frame, ops) in &self.frames {
            for (d, count) in ops {
                lines.push(format!("{};{} {}", frame, self.name_of(d), count));
            }
        }
        lines.sort();
        let mut out = lines.join("\n");
        if !out.is_empty() { out.push('\n'); }
        out
    }
}

fn opcode_name(op: &OpCode) -> String {
    let dbg = format!("{:?}", op);
    match dbg.find(|c: char| c == '(' || c == ' ' || c == '{') {
        Some(i) => dbg[..i].to_string(),
        None => dbg,
    }
}
