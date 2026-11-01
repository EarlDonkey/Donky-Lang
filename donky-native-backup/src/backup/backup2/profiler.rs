// profiler.rs — instruction-level profiler for the Donky VM
use std::collections::HashMap;
use crate::vm::OpCode;

pub struct ProfileReport {
    pub opcode_counts: HashMap<String, u64>,
    pub opcode_by_frame: HashMap<String, HashMap<String, u64>>,
    pub call_counts: HashMap<String, u64>,
    pub call_stack: Vec<String>,
    pub total_instructions: u64,
}

impl ProfileReport {
    pub fn new() -> Self {
        ProfileReport {
            opcode_counts: HashMap::new(),
            opcode_by_frame: HashMap::new(),
            call_counts: HashMap::new(),
            call_stack: Vec::new(),
            total_instructions: 0,
        }
    }

    pub fn on_instruction(&mut self, op: &OpCode) {
        self.total_instructions += 1;
        let name = opcode_name(op);
        *self.opcode_counts.entry(name.clone()).or_insert(0) += 1;

        let frame = if self.call_stack.is_empty() {
            "main".to_string()
        } else {
            self.call_stack.join(";")
        };
        *self.opcode_by_frame
            .entry(frame)
            .or_default()
            .entry(name)
            .or_insert(0) += 1;
    }

    pub fn push_frame(&mut self, name: String) {
        *self.call_counts.entry(name.clone()).or_insert(0) += 1;
        self.call_stack.push(name);
    }

    pub fn pop_frame(&mut self) {
        self.call_stack.pop();
    }

    pub fn ascii_report(&self) -> String {
        let mut out = String::new();
        out.push_str("=== Opcode heatmap ===\n");
        if self.total_instructions == 0 {
            out.push_str("  (no instructions executed)\n");
            return out;
        }
        let mut entries: Vec<(&String, &u64)> = self.opcode_counts.iter().collect();
        entries.sort_by(|a, b| b.1.cmp(a.1));
        let total = self.total_instructions as f64;
        for (name, count) in entries.iter().take(20) {
            let pct = (**count as f64) / total * 100.0;
            let bar_len = ((pct * 0.5) as usize).max(1);
            let bar: String = "█".repeat(bar_len);
            out.push_str(&format!("  {:<26} {:>5.1}%  {}\n", name, pct, bar));
        }

        out.push_str("\n=== Function call counts ===\n");
        if self.call_counts.is_empty() {
            out.push_str("  (no user function calls)\n");
        } else {
            let mut calls: Vec<(&String, &u64)> = self.call_counts.iter().collect();
            calls.sort_by(|a, b| b.1.cmp(a.1));
            for (name, count) in calls.iter().take(10) {
                out.push_str(&format!("  {:<26} {} calls\n", name, count));
            }
        }

        out.push_str("\n=== Hottest frames ===\n");
        let mut frames: Vec<(&String, u64)> = self
            .opcode_by_frame
            .iter()
            .map(|(k, v)| (k, v.values().sum()))
            .collect();
        frames.sort_by(|a, b| b.1.cmp(&a.1));
        for (frame, count) in frames.iter().take(10) {
            let pct = (*count as f64) / total * 100.0;
            out.push_str(&format!("  {:<40} {:>5.1}%  ({} ops)\n", frame, pct, count));
        }

        out.push_str(&format!("\nTotal instructions executed: {}\n", self.total_instructions));
        out
    }

    pub fn folded_stacks(&self) -> String {
        // Brendan Gregg folded format: "a;b;c 123" (one per line)
        let mut out = String::new();
        for (frame, opcodes) in &self.opcode_by_frame {
            for (op, count) in opcodes {
                out.push_str(&format!("{};{} {}\n", frame, op, count));
            }
        }
        out
    }
}

fn opcode_name(op: &OpCode) -> String {
    let dbg = format!("{:?}", op);
    match dbg.find('(') {
        Some(i) => dbg[..i].to_string(),
        None => dbg,
    }
}