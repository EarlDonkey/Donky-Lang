use cranelift::prelude::*;
use cranelift_object::{ObjectBuilder, ObjectModule};
use cranelift_module::{Module, Linkage, FuncId, DataDescription};
use crate::ast::{Expr, Stmt};
use crate::lexer::TokenType;
use std::collections::HashMap;
use std::sync::atomic::{AtomicUsize, Ordering};

static STRING_COUNTER: AtomicUsize = AtomicUsize::new(0);

#[derive(Clone, Copy)]
pub struct RuntimeFuncs {
    pub print_id: FuncId, pub alloc_array_id: FuncId, pub array_push_id: FuncId,
    pub alloc_map_id: FuncId, pub map_set_id: FuncId, pub index_get_id: FuncId,
    pub index_set_id: FuncId, pub alloc_string_id: FuncId,
    pub len_id: FuncId, pub push_id: FuncId, pub pop_id: FuncId, pub str_id: FuncId,
    pub num_id: FuncId, pub read_file_id: FuncId, pub write_file_id: FuncId,
    pub file_exists_id: FuncId,
    pub add_id: FuncId, pub eq_id: FuncId, pub byte_at_id: FuncId,
    pub substr_id: FuncId, pub byte_substr_id: FuncId, pub truthy_id: FuncId,
    pub upper_id: FuncId, pub lower_id: FuncId, pub split_id: FuncId,
    pub join_id: FuncId, pub now_id: FuncId, pub contains_id: FuncId,
    pub type_id: FuncId, pub random_id: FuncId, pub keys_id: FuncId,
    pub floor_id: FuncId, pub bytes_to_str_id: FuncId, pub char_code_id: FuncId,
    pub throw_id: FuncId, pub has_exception_id: FuncId,
    pub get_exception_id: FuncId, pub clear_exception_id: FuncId,
    pub get_args_id: FuncId,
    pub cls_id: FuncId, pub getch_id: FuncId, pub kbhit_id: FuncId,
    pub input_id: FuncId, pub sleep_id: FuncId,
    pub total_mem_id: FuncId, pub used_mem_id: FuncId, pub get_processes_id: FuncId,
    pub tcp_listen_id: FuncId, pub tcp_accept_id: FuncId,
    pub tcp_read_id: FuncId, pub tcp_write_id: FuncId, pub tcp_close_id: FuncId,
    pub http_get_id: FuncId, pub mod_id: FuncId, pub spawn_id: FuncId, pub gc_id: FuncId,
}

pub struct AotCompiler {
    builder_context: FunctionBuilderContext,
    ctx: codegen::Context,
    module: ObjectModule,
    user_functions: HashMap<String, FuncId>,
    runtime: RuntimeFuncs,
}

struct FunctionContext {
    variables: HashMap<String, Variable>,
    next_var: usize,
    exception_target: Option<(Block, Vec<Variable>)>,
    loop_stack: Vec<(Block, Block, Vec<Variable>)>,
}

impl AotCompiler {
    pub fn new() -> Self {
        let mut flag_builder = settings::builder();
        flag_builder.set("opt_level", "speed").unwrap();
        let isa_builder = cranelift_native::builder().unwrap();
        let isa = isa_builder.finish(settings::Flags::new(flag_builder)).unwrap();
        let object_builder = ObjectBuilder::new(isa, "donky_app", cranelift_module::default_libcall_names()).unwrap();
        let mut module = ObjectModule::new(object_builder);

        let mut declare = |name: &str, params: &[Type], ret: Option<Type>| {
            let mut sig = module.make_signature();
            for p in params { sig.params.push(AbiParam::new(*p)); }
            if let Some(r) = ret { sig.returns.push(AbiParam::new(r)); }
            module.declare_function(name, Linkage::Import, &sig).unwrap()
        };

        let runtime = RuntimeFuncs {
            print_id: declare("donky_print", &[types::I64], None),
            alloc_array_id: declare("donky_alloc_array", &[], Some(types::I64)),
            array_push_id: declare("donky_array_push", &[types::I64, types::I64], Some(types::I64)),
            alloc_map_id: declare("donky_alloc_map", &[], Some(types::I64)),
            map_set_id: declare("donky_map_set", &[types::I64, types::I64, types::I64], Some(types::I64)),
            index_get_id: declare("donky_index_get", &[types::I64, types::I64], Some(types::I64)),
            index_set_id: declare("donky_index_set", &[types::I64, types::I64, types::I64], Some(types::I64)),
            alloc_string_id: declare("donky_alloc_string", &[types::I64], Some(types::I64)),
            len_id: declare("donky_len", &[types::I64], Some(types::I64)),
            push_id: declare("donky_push", &[types::I64, types::I64], Some(types::I64)),
            pop_id: declare("donky_pop", &[types::I64], Some(types::I64)),
            str_id: declare("donky_str", &[types::I64], Some(types::I64)),
            num_id: declare("donky_num", &[types::I64], Some(types::I64)),
            read_file_id: declare("donky_read_file", &[types::I64], Some(types::I64)),
            write_file_id: declare("donky_write_file", &[types::I64, types::I64], Some(types::I64)),
            file_exists_id: declare("donky_file_exists", &[types::I64], Some(types::I64)),
            add_id: declare("donky_add", &[types::I64, types::I64], Some(types::I64)),
            eq_id: declare("donky_eq", &[types::I64, types::I64], Some(types::I64)),
            byte_at_id: declare("donky_byte_at", &[types::I64, types::I64], Some(types::I64)),
            substr_id: declare("donky_substr", &[types::I64, types::I64, types::I64], Some(types::I64)),
            byte_substr_id: declare("donky_byte_substr", &[types::I64, types::I64, types::I64], Some(types::I64)),
            truthy_id: declare("donky_truthy", &[types::I64], Some(types::I64)),
            upper_id: declare("donky_upper", &[types::I64], Some(types::I64)),
            lower_id: declare("donky_lower", &[types::I64], Some(types::I64)),
            split_id: declare("donky_split", &[types::I64, types::I64], Some(types::I64)),
            join_id: declare("donky_join", &[types::I64, types::I64], Some(types::I64)),
            now_id: declare("donky_now", &[], Some(types::I64)),
            contains_id: declare("donky_contains", &[types::I64, types::I64], Some(types::I64)),
            type_id: declare("donky_type", &[types::I64], Some(types::I64)),
            random_id: declare("donky_random", &[types::I64], Some(types::I64)),
            keys_id: declare("donky_keys", &[types::I64], Some(types::I64)),
            floor_id: declare("donky_floor", &[types::I64], Some(types::I64)),
            bytes_to_str_id: declare("donky_bytes_to_str", &[types::I64], Some(types::I64)),
            char_code_id: declare("donky_char_code", &[types::I64], Some(types::I64)),
            throw_id: declare("donky_throw", &[types::I64], None),
            has_exception_id: declare("donky_has_exception", &[], Some(types::I32)),
            get_exception_id: declare("donky_get_exception", &[], Some(types::I64)),
            clear_exception_id: declare("donky_clear_exception", &[], None),
            get_args_id: declare("donky_get_args", &[], Some(types::I64)),
            cls_id: declare("donky_cls", &[], None),
            getch_id: declare("donky_getch", &[], Some(types::I64)),
            kbhit_id: declare("donky_kbhit", &[], Some(types::I64)),
            input_id: declare("donky_input", &[types::I64], Some(types::I64)),
            sleep_id: declare("donky_sleep", &[types::I64], Some(types::I64)),
            total_mem_id: declare("donky_get_total_mem", &[], Some(types::I64)),
            used_mem_id: declare("donky_get_used_mem", &[], Some(types::I64)),
            get_processes_id: declare("donky_get_processes", &[], Some(types::I64)),
            tcp_listen_id: declare("donky_tcp_listen", &[types::I64], Some(types::I64)),
            tcp_accept_id: declare("donky_tcp_accept", &[types::I64], Some(types::I64)),
            tcp_read_id:   declare("donky_tcp_read",   &[types::I64, types::I64], Some(types::I64)),
            tcp_write_id:  declare("donky_tcp_write",  &[types::I64, types::I64], Some(types::I64)),
            tcp_close_id:  declare("donky_tcp_close",  &[types::I64], Some(types::I64)),
            http_get_id: declare("donky_http_get", &[types::I64], Some(types::I64)),
            mod_id: declare("donky_mod", &[types::I64, types::I64], Some(types::I64)),
            spawn_id: declare("donky_spawn", &[types::I64, types::I64], Some(types::I64)),
            gc_id: declare("donky_gc", &[], Some(types::I64)),
            
        };
        
        AotCompiler { builder_context: FunctionBuilderContext::new(), ctx: codegen::Context::new(), module, user_functions: HashMap::new(), runtime }
    }

    pub fn compile_to_object(mut self, stmts: &[Stmt], output_path: &str) -> Result<(), String> {
        // --- NEW: Recursive Import Resolution ---
        use std::path::Path;
        use std::fs;
        
        fn resolve_imports(stmts: &[Stmt], base_dir: &str, all_funcs: &mut HashMap<String, (Vec<String>, Box<Stmt>)>) {
            use std::path::Path;
            use std::fs;
            for stmt in stmts {
                match stmt {
                    Stmt::Import(path) => {
                        let full_path = Path::new(base_dir).join(path).to_string_lossy().to_string();
                        
                        let source = match fs::read_to_string(&full_path) {
                            Ok(s) => s,
                            Err(e) => { println!("DEBUG: Failed to read file: {}", e); continue; }
                        };
                        
                        let toks = match crate::lexer::Lexer::new(&source).tokenize() {
                            Ok(t) => t,
                            Err(e) => { println!("DEBUG: Lex error in {}: {}", full_path, e); continue; }
                        };
                        
                        let imported_stmts = match crate::parser::Parser::new(toks).parse() {
                            Ok(a) => a,
                            Err(e) => { println!("DEBUG: Parse error in {}: {}", full_path, e); continue; }
                        };

                        let new_base = Path::new(&full_path).parent().unwrap_or(Path::new("")).to_string_lossy().to_string();
                        resolve_imports(&imported_stmts, &new_base, all_funcs);
                    }
                    Stmt::Function(name, params, body) => {
                        all_funcs.insert(name.clone(), (params.clone(), body.clone()));
                    }
                    _ => {}
                }
            }
        }

        let mut latest_funcs: HashMap<String, (Vec<String>, Box<Stmt>)> = HashMap::new();
        // Start resolution from the current directory
        resolve_imports(stmts, ".", &mut latest_funcs);
        // ----------------------------------------

        for (name, (params, _)) in &latest_funcs {
            let mut sig = self.module.make_signature();
            for _ in 0..params.len() { sig.params.push(AbiParam::new(types::I64)); }
            sig.returns.push(AbiParam::new(types::I64));
            let func_id = self.module.declare_function(&format!("donky_fn_{}", name), Linkage::Export, &sig).unwrap();
            self.user_functions.insert(name.clone(), func_id);
        }

        for (name, (params, body)) in latest_funcs {
            let func_id = self.user_functions[&name];
            self.compile_function(func_id, &params, &body);
        }

        let mut sig = self.module.make_signature();
        sig.returns.push(AbiParam::new(types::I64));
        let main_id = self.module.declare_function("donky_toplevel", Linkage::Export, &sig).unwrap();
        self.ctx.func.signature = sig;

        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.builder_context);
            let entry_block = builder.create_block();
            builder.append_block_params_for_function_params(entry_block);
            builder.switch_to_block(entry_block);
            builder.seal_block(entry_block);

            let mut fctx = FunctionContext { variables: HashMap::new(), next_var: 0, exception_target: None, loop_stack: Vec::new() };
            
            let get_args_func = self.module.declare_func_in_func(self.runtime.get_args_id, builder.func);
            let args_call = builder.ins().call(get_args_func, &[]);
            let args_val = builder.inst_results(args_call)[0];
            let args_var = Variable::new(fctx.next_var); fctx.next_var += 1;
            builder.declare_var(args_var, types::I64);
            builder.def_var(args_var, args_val);
            fctx.variables.insert("args".to_string(), args_var);
            
            let mut terminated = false;
            for stmt in stmts {
                if !matches!(stmt, Stmt::Function(_, _, _)) {
                    terminated = Self::compile_stmt(&mut builder, &mut self.module, &self.runtime, &self.user_functions, &mut fctx, stmt);
                    if terminated { break; }
                }
            }
            if !terminated { 
                let zero = builder.ins().iconst(types::I64, 0);
                builder.ins().return_(&[zero]); 
            }
            builder.finalize();
        }
        self.module.define_function(main_id, &mut self.ctx).unwrap();
        self.module.clear_context(&mut self.ctx);
        let product = self.module.finish();
        let bytes = product.emit().unwrap();
        std::fs::write(output_path, bytes).map_err(|e| e.to_string())?;
        Ok(())
    }

    fn compile_function(&mut self, func_id: FuncId, params: &[String], body: &Stmt) {
        let mut sig = self.module.make_signature();
        for _ in 0..params.len() { sig.params.push(AbiParam::new(types::I64)); }
        sig.returns.push(AbiParam::new(types::I64));
        self.ctx.func.signature = sig;
        {
            let mut builder = FunctionBuilder::new(&mut self.ctx.func, &mut self.builder_context);
            let entry_block = builder.create_block();
            builder.append_block_params_for_function_params(entry_block);
            builder.switch_to_block(entry_block);
            builder.seal_block(entry_block);
            let mut fctx = FunctionContext { variables: HashMap::new(), next_var: 0, exception_target: None, loop_stack: Vec::new() };
            let block_params: Vec<Value> = builder.block_params(entry_block).to_vec();
            for (i, param_name) in params.iter().enumerate() {
                let var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(var, types::I64);
                builder.def_var(var, block_params[i]);
                fctx.variables.insert(param_name.clone(), var);
            }
            let get_args_func = self.module.declare_func_in_func(self.runtime.get_args_id, builder.func);
            let args_call = builder.ins().call(get_args_func, &[]);
            let args_val = builder.inst_results(args_call)[0];
            let args_var = Variable::new(fctx.next_var); fctx.next_var += 1;
            builder.declare_var(args_var, types::I64);
            builder.def_var(args_var, args_val);
            fctx.variables.insert("args".to_string(), args_var);
            let terminated = Self::compile_stmt(&mut builder, &mut self.module, &self.runtime, &self.user_functions, &mut fctx, body);
            if !terminated { 
                let zero = builder.ins().iconst(types::I64, 0);
                builder.ins().return_(&[zero]); 
            }
            builder.finalize();
        }
        self.module.define_function(func_id, &mut self.ctx).unwrap();
        self.module.clear_context(&mut self.ctx);
    }

    fn get_sorted_vars(fctx: &FunctionContext) -> Vec<Variable> {
        let mut vars: Vec<Variable> = fctx.variables.values().copied().collect();
        vars.sort_by_key(|v| v.index());
        vars
    }

    fn compile_stmt(
        builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs,
        user_funcs: &HashMap<String, FuncId>, fctx: &mut FunctionContext, stmt: &Stmt,
    ) -> bool {
        match stmt {
            Stmt::Let(name, expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(var, types::I64); builder.def_var(var, val);
                fctx.variables.insert(name.clone(), var); false
            }
            Stmt::Assign(name, expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let var = fctx.variables.get(name).unwrap(); builder.def_var(*var, val); false
            }
            Stmt::IndexSet(target, index, val) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, fctx, target);
                let i = Self::compile_expr(builder, module, runtime, user_funcs, fctx, index);
                let v = Self::compile_expr(builder, module, runtime, user_funcs, fctx, val);
                let local_callee = module.declare_func_in_func(runtime.index_set_id, builder.func);
                builder.ins().call(local_callee, &[t, i, v]); false
            }
            Stmt::FieldSet(obj, field, val) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, fctx, obj);
                let key_idx = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
                let data_id = module.declare_data(&format!("str_{}", key_idx), Linkage::Export, true, false).unwrap();
                let mut data_ctx = DataDescription::new();
                let mut bytes = field.as_bytes().to_vec(); bytes.push(0);
                data_ctx.define(bytes.into_boxed_slice()); module.define_data(data_id, &data_ctx).unwrap();
                let local_data = module.declare_data_in_func(data_id, builder.func);
                let key_ptr = builder.ins().global_value(types::I64, local_data);
                let alloc_str_func = module.declare_func_in_func(runtime.alloc_string_id, builder.func);
                let call = builder.ins().call(alloc_str_func, &[key_ptr]);
                let key = builder.inst_results(call)[0];
                
                let v = Self::compile_expr(builder, module, runtime, user_funcs, fctx, val);
                let set_func = module.declare_func_in_func(runtime.index_set_id, builder.func);
                builder.ins().call(set_func, &[t, key, v]);
                false
            }
            Stmt::Print(expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let local_callee = module.declare_func_in_func(runtime.print_id, builder.func);
                builder.ins().call(local_callee, &[val]); false
            }
            Stmt::ExprStmt(expr) => { Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr); false }
            Stmt::Return(expr) => {
                let val = if let Some(e) = expr { 
                    Self::compile_expr(builder, module, runtime, user_funcs, fctx, e) 
                } else { 
                    builder.ins().iconst(types::I64, 0) 
                };
                builder.ins().return_(&[val]); true
            }
            Stmt::Break => {
                if let Some((break_target, _, vars)) = fctx.loop_stack.last() {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(*break_target, &args);
                }
                true
            }
            Stmt::Continue => {
                if let Some((_, continue_target, vars)) = fctx.loop_stack.last() {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(*continue_target, &args);
                }
                true
            }
            Stmt::Throw(expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let throw_func = module.declare_func_in_func(runtime.throw_id, builder.func);
                builder.ins().call(throw_func, &[val]);
                if let Some((target, vars)) = &fctx.exception_target {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(*target, &args);
                } else {
                    let zero = builder.ins().iconst(types::I64, 0);
                    builder.ins().return_(&[zero]);
                }
                true
            }
            Stmt::Try(try_b, err_name, catch_b) => {
                let vars = Self::get_sorted_vars(fctx);
                let catch_block = builder.create_block();
                let merge_block = builder.create_block();
                for _ in 0..vars.len() {
                    builder.append_block_param(catch_block, types::I64);
                    builder.append_block_param(merge_block, types::I64);
                }
                let prev_target = fctx.exception_target.clone();
                fctx.exception_target = Some((catch_block, vars.clone()));
                let try_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, try_b);
                fctx.exception_target = prev_target;
                if !try_term {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(merge_block, &args);
                }
                builder.seal_block(catch_block);
                builder.switch_to_block(catch_block);
                let catch_params = builder.block_params(catch_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, catch_params[i]);
                }
                let get_exc_func = module.declare_func_in_func(runtime.get_exception_id, builder.func);
                let exc_call = builder.ins().call(get_exc_func, &[]);
                let exc_val = builder.inst_results(exc_call)[0];
                let clear_exc_func = module.declare_func_in_func(runtime.clear_exception_id, builder.func);
                builder.ins().call(clear_exc_func, &[]);
                let err_var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(err_var, types::I64);
                builder.def_var(err_var, exc_val);
                fctx.variables.insert(err_name.clone(), err_var);
                let catch_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, catch_b);
                if !catch_term {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(merge_block, &args);
                }
                if !try_term || !catch_term {
                    builder.seal_block(merge_block);
                    builder.switch_to_block(merge_block);
                    let merge_params = builder.block_params(merge_block).to_vec();
                    for (i, var) in vars.iter().enumerate() {
                        builder.def_var(*var, merge_params[i]);
                    }
                    false
                } else {
                    true
                }
            }
            Stmt::If(cond, then_b, else_b) => {
                let vars = Self::get_sorted_vars(fctx);
                let then_block = builder.create_block();
                let else_block = builder.create_block();
                let merge_block = builder.create_block();
                for _ in 0..vars.len() {
                    builder.append_block_param(then_block, types::I64);
                    builder.append_block_param(else_block, types::I64);
                    builder.append_block_param(merge_block, types::I64);
                }
                let cond_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, cond);
                let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                let truthy_call = builder.ins().call(truthy_func, &[cond_val]);
                let truthy_val = builder.inst_results(truthy_call)[0];
                let truthy_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), truthy_val);
                let zero = builder.ins().f64const(0.0);
                let is_true = builder.ins().fcmp(FloatCC::NotEqual, truthy_f64, zero);
                let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().brif(is_true, then_block, &args, else_block, &args);
                builder.seal_block(then_block);
                builder.seal_block(else_block);
                builder.switch_to_block(then_block);
                let then_params = builder.block_params(then_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, then_params[i]);
                }
                let then_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, then_b);
                if !then_term {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(merge_block, &args);
                }
                builder.switch_to_block(else_block);
                let else_params = builder.block_params(else_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, else_params[i]);
                }
                let else_term = if let Some(eb) = else_b {
                    Self::compile_stmt(builder, module, runtime, user_funcs, fctx, eb)
                } else {
                    false
                };
                if !else_term {
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(merge_block, &args);
                }
                if !then_term || !else_term {
                    builder.seal_block(merge_block);
                    builder.switch_to_block(merge_block);
                    let merge_params = builder.block_params(merge_block).to_vec();
                    for (i, var) in vars.iter().enumerate() {
                        builder.def_var(*var, merge_params[i]);
                    }
                    false
                } else {
                    true
                }
            }
            Stmt::While(cond, body) => {
                let vars = Self::get_sorted_vars(fctx);
                let header = builder.create_block();
                let body_block = builder.create_block();
                let exit_block = builder.create_block();
                for _ in 0..vars.len() {
                    builder.append_block_param(header, types::I64);
                    builder.append_block_param(body_block, types::I64);
                    builder.append_block_param(exit_block, types::I64);
                }
                let header_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().jump(header, &header_args);
                builder.switch_to_block(header);
                let header_params = builder.block_params(header).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, header_params[i]);
                }
                let cond_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, cond);
                let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                let truthy_call = builder.ins().call(truthy_func, &[cond_val]);
                let truthy_val = builder.inst_results(truthy_call)[0];
                let truthy_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), truthy_val);
                let zero = builder.ins().f64const(0.0);
                let is_true = builder.ins().fcmp(FloatCC::NotEqual, truthy_f64, zero);
                let body_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                let exit_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().brif(is_true, body_block, &body_args, exit_block, &exit_args);
                builder.seal_block(body_block);
                //builder.seal_block(exit_block);
                fctx.loop_stack.push((exit_block, header, vars.clone()));
                builder.switch_to_block(body_block);
                let body_params = builder.block_params(body_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, body_params[i]);
                }
                let body_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, body);
                fctx.loop_stack.pop();
                if !body_term {
                    let loop_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(header, &loop_args);
                }
                builder.switch_to_block(exit_block);
                let exit_params = builder.block_params(exit_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, exit_params[i]);
                }
                builder.seal_block(header);
                builder.seal_block(exit_block);
                false
            }
            Stmt::For(var_name, iter_expr, body) => {
                let iter_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                let idx_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                let iter_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, iter_expr);
                builder.declare_var(iter_var, types::I64);
                builder.def_var(iter_var, iter_val);
                let zero = builder.ins().iconst(types::I64, 0);
                builder.declare_var(idx_var, types::I64);
                builder.def_var(idx_var, zero);
                fctx.variables.insert(format!("__iter_{}", iter_var.index()), iter_var);
                fctx.variables.insert(format!("__idx_{}", idx_var.index()), idx_var);
                let vars = Self::get_sorted_vars(fctx);
                let header = builder.create_block();
                let body_block = builder.create_block();
                let incr_block = builder.create_block();
                let exit_block = builder.create_block();
                for _ in 0..vars.len() {
                    builder.append_block_param(header, types::I64);
                    builder.append_block_param(body_block, types::I64);
                    builder.append_block_param(incr_block, types::I64);
                    builder.append_block_param(exit_block, types::I64);
                }
                let header_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().jump(header, &header_args);
                builder.switch_to_block(header);
                let header_params = builder.block_params(header).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, header_params[i]);
                }
                let len_func = module.declare_func_in_func(runtime.len_id, builder.func);
                let iter_h = builder.use_var(iter_var);
                let len_call = builder.ins().call(len_func, &[iter_h]);
                let len_val = builder.inst_results(len_call)[0];
                let idx_h = builder.use_var(idx_var);
                let idx_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), idx_h);
                let len_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), len_val);
                let cond = builder.ins().fcmp(FloatCC::LessThan, idx_f64, len_f64);
                let body_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                let exit_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().brif(cond, body_block, &body_args, exit_block, &exit_args);
                builder.seal_block(body_block);
                builder.seal_block(incr_block);
                builder.seal_block(exit_block);
                fctx.loop_stack.push((exit_block, incr_block, vars.clone()));
                builder.switch_to_block(body_block);
                let body_params = builder.block_params(body_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, body_params[i]);
                }
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let item_call = builder.ins().call(get_func, &[iter_h, idx_h]);
                let item_val = builder.inst_results(item_call)[0];
                let loop_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(loop_var, types::I64);
                builder.def_var(loop_var, item_val);
                fctx.variables.insert(var_name.clone(), loop_var);
                let term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, body);
                if !term {
                    let incr_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().jump(incr_block, &incr_args);
                }
                fctx.loop_stack.pop();
                builder.switch_to_block(incr_block);
                let incr_params = builder.block_params(incr_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, incr_params[i]);
                }
                // FIXED: read idx fresh from incr_block (so body mutations to idx are honoured),
                //        and use the NaN-boxed representation of the number 1.0, not raw 1_i64.
                let cur_idx = builder.use_var(idx_var);
                let one = builder.ins().iconst(types::I64, 1.0_f64.to_bits() as i64);
                let add_func = module.declare_func_in_func(runtime.add_id, builder.func);
                let add_call = builder.ins().call(add_func, &[cur_idx, one]);
                let next_idx = builder.inst_results(add_call)[0];
                builder.def_var(idx_var, next_idx);
                let loop_args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().jump(header, &loop_args);
                builder.switch_to_block(exit_block);
                let exit_params = builder.block_params(exit_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, exit_params[i]);
                }
                builder.seal_block(header);
                false
            }
            Stmt::Match(target, cases, else_block) => {
                let vars = Self::get_sorted_vars(fctx);
                let merge_block = builder.create_block();
                let mut check_blocks = Vec::new();
                for _ in 0..cases.len() { check_blocks.push(builder.create_block()); }
                let else_check_block = if else_block.is_some() { Some(builder.create_block()) } else { None };
                for _ in 0..vars.len() {
                    builder.append_block_param(merge_block, types::I64);
                    for b in &check_blocks { builder.append_block_param(*b, types::I64); }
                    if let Some(b) = else_check_block { builder.append_block_param(b, types::I64); }
                }
                let target_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, target);
                let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                builder.ins().jump(check_blocks[0], &args);
                builder.seal_block(check_blocks[0]);
                for (i, (case_expr, body)) in cases.iter().enumerate() {
                    builder.switch_to_block(check_blocks[i]);
                    let block_params = builder.block_params(check_blocks[i]).to_vec();
                    for (j, var) in vars.iter().enumerate() {
                        builder.def_var(*var, block_params[j]);
                    }
                    let case_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, case_expr);
                    let eq_func = module.declare_func_in_func(runtime.eq_id, builder.func);
                    let eq_call = builder.ins().call(eq_func, &[target_val, case_val]);
                    let is_eq = builder.inst_results(eq_call)[0];
                    let is_eq_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), is_eq);
                    let zero = builder.ins().f64const(0.0);
                    let cond = builder.ins().fcmp(FloatCC::NotEqual, is_eq_f64, zero);
                    let body_block = builder.create_block();
                    for _ in 0..vars.len() { builder.append_block_param(body_block, types::I64); }
                    builder.seal_block(body_block);
                    let next_check = if i + 1 < cases.len() { check_blocks[i+1] } else { else_check_block.unwrap_or(merge_block) };
                    if i + 1 < cases.len() { builder.seal_block(check_blocks[i+1]); }
                    //if let Some(eb) = else_check_block { builder.seal_block(eb); }
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().brif(cond, body_block, &args, next_check, &args);
                    builder.switch_to_block(body_block);
                    let body_params = builder.block_params(body_block).to_vec();
                    for (j, var) in vars.iter().enumerate() {
                        builder.def_var(*var, body_params[j]);
                    }
                    let term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, body);
                    if !term {
                        let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                        builder.ins().jump(merge_block, &args);
                    }
                }
                if let Some(eb_block) = else_check_block { builder.seal_block(eb_block); }
                if let Some(eb) = else_block {
                    if let Some(eb_block) = else_check_block {
                        builder.switch_to_block(eb_block);
                        let block_params = builder.block_params(eb_block).to_vec();
                        for (j, var) in vars.iter().enumerate() {
                            builder.def_var(*var, block_params[j]);
                        }
                        let term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, eb);
                        if !term {
                            let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                            builder.ins().jump(merge_block, &args);
                        }
                    }
                }
                builder.seal_block(merge_block);
                builder.switch_to_block(merge_block);
                let merge_params = builder.block_params(merge_block).to_vec();
                for (i, var) in vars.iter().enumerate() {
                    builder.def_var(*var, merge_params[i]);
                }
                false
            }
            Stmt::Block(stmts) => {
                for s in stmts {
                    if Self::compile_stmt(builder, module, runtime, user_funcs, fctx, s) {
                        return true;
                    }
                }
                false
            }
            Stmt::Function(_, _, _) => false,
            Stmt::StructDef(_, _) => false,
            Stmt::Import(_) => false,
            Stmt::Asm(_) => false,
            _ => panic!("Unsupported statement in native codegen"),
        }
    }

    fn compile_expr(
        builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs,
        user_funcs: &HashMap<String, FuncId>, fctx: &mut FunctionContext, expr: &Expr,
    ) -> Value {
        match expr {
            Expr::Number(n) => builder.ins().iconst(types::I64, n.to_bits() as i64),
            Expr::Bool(b) => {
                let tag_base: u64 = 0x7FF8_0000_0000_0000;
                let val = tag_base | (1 << 48) | (if *b { 1 } else { 0 });
                builder.ins().iconst(types::I64, val as i64)
            }
            Expr::String(s) => {
                let id = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
                let data_id = module.declare_data(&format!("str_{}", id), Linkage::Export, true, false).unwrap();
                let mut data_ctx = DataDescription::new();
                let mut bytes = s.as_bytes().to_vec(); bytes.push(0);
                data_ctx.define(bytes.into_boxed_slice()); module.define_data(data_id, &data_ctx).unwrap();
                let local_data = module.declare_data_in_func(data_id, builder.func);
                let ptr = builder.ins().global_value(types::I64, local_data);
                let local_callee = module.declare_func_in_func(runtime.alloc_string_id, builder.func);
                let call = builder.ins().call(local_callee, &[ptr]);
                builder.inst_results(call)[0]
            }
            Expr::Ident(name) => { 
                if name == "null" {
                    return builder.ins().iconst(types::I64, 0);
                }
                // FIXED: Don't panic on unknown identifiers. Return null so the program
                // can continue and throw a proper runtime error if needed.
                if let Some(var) = fctx.variables.get(name) {
                    builder.use_var(*var)
                } else if let Some(func_id) = user_funcs.get(name) {
                    // NEW: If it's a function name, return its memory address
                    let func_ref = module.declare_func_in_func(*func_id, builder.func);
                    let addr = builder.ins().func_addr(types::I64, func_ref);
                    addr 
                } else {
                    builder.ins().iconst(types::I64, 0) // Return null
                }
            }
            Expr::Unary(op, right) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, right);
                match op {
                    TokenType::Minus => {
                        let v_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), val);
                        let neg = builder.ins().fneg(v_f64);
                        builder.ins().bitcast(types::I64, MemFlags::new(), neg)
                    }
                    TokenType::Bang => {
                        let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                        let truthy_call = builder.ins().call(truthy_func, &[val]);
                        let truthy_val = builder.inst_results(truthy_call)[0];
                        let truthy_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), truthy_val);
                        let zero_f64 = builder.ins().f64const(0.0);
                        let is_zero = builder.ins().fcmp(FloatCC::Equal, truthy_f64, zero_f64);
                        let int_val = builder.ins().uextend(types::I64, is_zero);
                        let f64_result = builder.ins().fcvt_from_uint(types::F64, int_val);
                        builder.ins().bitcast(types::I64, MemFlags::new(), f64_result)
                    }
                    _ => val,
                }
            }
            Expr::Call(name, args) => {
                if let Some(func_id) = user_funcs.get(name) {
                    let local_callee = module.declare_func_in_func(*func_id, builder.func);
                    let mut arg_vals = Vec::new();
                    for arg in args { arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, fctx, arg)); }
                    let call = builder.ins().call(local_callee, &arg_vals);
                    let ret_val = builder.inst_results(call)[0];
                    let has_exc_func = module.declare_func_in_func(runtime.has_exception_id, builder.func);
                    let exc_call = builder.ins().call(has_exc_func, &[]);
                    let has_exc = builder.inst_results(exc_call)[0];
                    let zero = builder.ins().iconst(types::I32, 0);
                    let cmp = builder.ins().icmp(IntCC::NotEqual, has_exc, zero);
                    let cont_block = builder.create_block(); let exc_block = builder.create_block();
                    let vars = Self::get_sorted_vars(fctx);
                    for _ in 0..vars.len() {
                        builder.append_block_param(exc_block, types::I64);
                        builder.append_block_param(cont_block, types::I64);
                    }
                    
                    let args: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().brif(cmp, exc_block, &args, cont_block, &args);
                    builder.seal_block(exc_block); builder.seal_block(cont_block);
                    
                    builder.switch_to_block(exc_block);
                    let exc_params = builder.block_params(exc_block).to_vec();
                    for (i, var) in vars.iter().enumerate() {
                        builder.def_var(*var, exc_params[i]);
                    }
                    if let Some((target, tvars)) = &fctx.exception_target {
                        let args: Vec<Value> = tvars.iter().map(|v| builder.use_var(*v)).collect();
                        builder.ins().jump(*target, &args);
                    } else {
                        let zero_val = builder.ins().iconst(types::I64, 0);
                        builder.ins().return_(&[zero_val]);
                    }
                    
                    builder.switch_to_block(cont_block);
                    let cont_params = builder.block_params(cont_block).to_vec();
                    for (i, var) in vars.iter().enumerate() {
                        builder.def_var(*var, cont_params[i]);
                    }
                    ret_val
                } else {
                    let builtin_id = match name.as_str() {
                        "len" => runtime.len_id, "push" => runtime.push_id, "pop" => runtime.pop_id,
                        "str" => runtime.str_id, "num" => runtime.num_id, "read_file" => runtime.read_file_id,
                        "write_file" => runtime.write_file_id, "byte_at" => runtime.byte_at_id,
                        "substr" => runtime.substr_id, "byte_substr" => runtime.byte_substr_id,
                        "upper" => runtime.upper_id, "lower" => runtime.lower_id,
                        "split" => runtime.split_id, "join" => runtime.join_id, "now" => runtime.now_id,
                        "contains" => runtime.contains_id, "file_exists" => runtime.file_exists_id,
                        "type" => runtime.type_id, "random" => runtime.random_id, "keys" => runtime.keys_id,
                        "floor" => runtime.floor_id, "bytes_to_str" => runtime.bytes_to_str_id,
                        "char_code" => runtime.char_code_id,
                        "cls" => runtime.cls_id, "getch" => runtime.getch_id, "kbhit" => runtime.kbhit_id,
                        "input" => runtime.input_id, "sleep" => runtime.sleep_id,
                        "total_mem" => runtime.total_mem_id, "used_mem" => runtime.used_mem_id,
                        "get_processes" => runtime.get_processes_id,
                        "tcp_listen" => runtime.tcp_listen_id,
                        "tcp_accept" => runtime.tcp_accept_id,
                        "tcp_read"   => runtime.tcp_read_id,
                        "tcp_write"  => runtime.tcp_write_id,
                        "tcp_close"  => runtime.tcp_close_id,
                        "http_get" => runtime.http_get_id,
                        "spawn" => runtime.spawn_id,
                        "gc" => runtime.gc_id,
                        _ => panic!("Unknown builtin function: {}", name),
                    };
                    let local_callee = module.declare_func_in_func(builtin_id, builder.func);
                    let mut arg_vals = Vec::new();
                    for arg in args { arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, fctx, arg)); }
                    
                    let call = builder.ins().call(local_callee, &arg_vals);
                    let results = builder.inst_results(call);
                    let ret_val = if results.is_empty() { builder.ins().iconst(types::I64, 0) } else { results[0] };

                    // --- NEW: Check for exceptions thrown by builtins ---
                    let has_exc_func = module.declare_func_in_func(runtime.has_exception_id, builder.func);
                    let exc_call = builder.ins().call(has_exc_func, &[]);
                    let has_exc = builder.inst_results(exc_call)[0];
                    let zero_i32 = builder.ins().iconst(types::I32, 0);
                    let cmp = builder.ins().icmp(IntCC::NotEqual, has_exc, zero_i32);
                    
                    let cont_block = builder.create_block();
                    let exc_block = builder.create_block();
                    let vars = Self::get_sorted_vars(fctx);
                    for _ in 0..vars.len() {
                        builder.append_block_param(exc_block, types::I64);
                        builder.append_block_param(cont_block, types::I64);
                    }
                    
                    let args_vals: Vec<Value> = vars.iter().map(|v| builder.use_var(*v)).collect();
                    builder.ins().brif(cmp, exc_block, &args_vals, cont_block, &args_vals);
                    builder.seal_block(exc_block);
                    builder.seal_block(cont_block);
                    
                    builder.switch_to_block(exc_block);
                    let exc_params = builder.block_params(exc_block).to_vec();
                    for (i, var) in vars.iter().enumerate() {
                        builder.def_var(*var, exc_params[i]);
                    }
                    if let Some((target, tvars)) = &fctx.exception_target {
                        let targs: Vec<Value> = tvars.iter().map(|v| builder.use_var(*v)).collect();
                        builder.ins().jump(*target, &targs);
                    } else {
                        let zero_val = builder.ins().iconst(types::I64, 0);
                        builder.ins().return_(&[zero_val]);
                    }
                    
                    builder.switch_to_block(cont_block);
                    let cont_params = builder.block_params(cont_block).to_vec();
                    for (i, var) in vars.iter().enumerate() {
                        builder.def_var(*var, cont_params[i]);
                    }
                    ret_val
                }
            }
            Expr::Array(elements) => {
                let arr_func = module.declare_func_in_func(runtime.alloc_array_id, builder.func);
                let call = builder.ins().call(arr_func, &[]);
                let arr_handle = builder.inst_results(call)[0];
                for el in elements {
                    let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, el);
                    let push_func = module.declare_func_in_func(runtime.array_push_id, builder.func);
                    builder.ins().call(push_func, &[arr_handle, val]);
                }
                arr_handle
            }
            Expr::Map(pairs) => {
                let map_func = module.declare_func_in_func(runtime.alloc_map_id, builder.func);
                let call = builder.ins().call(map_func, &[]);
                let map_handle = builder.inst_results(call)[0];
                for (k, v) in pairs {
                    let key = Self::compile_expr(builder, module, runtime, user_funcs, fctx, k);
                    let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, v);
                    let set_func = module.declare_func_in_func(runtime.map_set_id, builder.func);
                    builder.ins().call(set_func, &[map_handle, key, val]);
                }
                map_handle
            }
            Expr::StructInit(name, fields) => {
                let map_func = module.declare_func_in_func(runtime.alloc_map_id, builder.func);
                let call = builder.ins().call(map_func, &[]);
                let map_handle = builder.inst_results(call)[0];
                
                // 1. Add a "__type__" field so we know what this object is
                let type_key_idx = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
                let type_data_id = module.declare_data(&format!("str_{}", type_key_idx), Linkage::Export, true, false).unwrap();
                let mut type_data_ctx = DataDescription::new();
                let mut type_bytes = b"__type__".to_vec(); type_bytes.push(0);
                type_data_ctx.define(type_bytes.into_boxed_slice()); module.define_data(type_data_id, &type_data_ctx).unwrap();
                let type_local_data = module.declare_data_in_func(type_data_id, builder.func);
                let type_ptr = builder.ins().global_value(types::I64, type_local_data);
                let alloc_str_func = module.declare_func_in_func(runtime.alloc_string_id, builder.func);
                let type_key_call = builder.ins().call(alloc_str_func, &[type_ptr]);
                let type_key = builder.inst_results(type_key_call)[0];
                
                // Box the struct name as a string
                let name_idx = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
                let name_data_id = module.declare_data(&format!("str_{}", name_idx), Linkage::Export, true, false).unwrap();
                let mut name_data_ctx = DataDescription::new();
                let mut name_bytes = name.as_bytes().to_vec(); name_bytes.push(0);
                name_data_ctx.define(name_bytes.into_boxed_slice()); module.define_data(name_data_id, &name_data_ctx).unwrap();
                let name_local_data = module.declare_data_in_func(name_data_id, builder.func);
                let name_ptr = builder.ins().global_value(types::I64, name_local_data);
                let name_val_call = builder.ins().call(alloc_str_func, &[name_ptr]);
                let name_val = builder.inst_results(name_val_call)[0];
                
                // Set the type field
                let set_func = module.declare_func_in_func(runtime.map_set_id, builder.func);
                builder.ins().call(set_func, &[map_handle, type_key, name_val]);

                // 2. Add all the user fields
                for (field_name, field_val) in fields {
                    let field_idx = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
                    let field_data_id = module.declare_data(&format!("str_{}", field_idx), Linkage::Export, true, false).unwrap();
                    let mut field_data_ctx = DataDescription::new();
                    let mut field_bytes = field_name.as_bytes().to_vec(); field_bytes.push(0);
                    field_data_ctx.define(field_bytes.into_boxed_slice()); module.define_data(field_data_id, &field_data_ctx).unwrap();
                    let field_local_data = module.declare_data_in_func(field_data_id, builder.func);
                    let field_ptr = builder.ins().global_value(types::I64, field_local_data);
                    let field_key_call = builder.ins().call(alloc_str_func, &[field_ptr]);
                    let field_key = builder.inst_results(field_key_call)[0];

                    let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, field_val);
                    builder.ins().call(set_func, &[map_handle, field_key, val]);
                }
                map_handle
            }
            Expr::Index(target, index) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, fctx, target);
                let i = Self::compile_expr(builder, module, runtime, user_funcs, fctx, index);
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let call = builder.ins().call(get_func, &[t, i]);
                builder.inst_results(call)[0]
            }
            Expr::FieldAccess(obj, field) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, fctx, obj);
                let key_idx = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
                let data_id = module.declare_data(&format!("str_{}", key_idx), Linkage::Export, true, false).unwrap();
                let mut data_ctx = DataDescription::new();
                let mut bytes = field.as_bytes().to_vec(); bytes.push(0);
                data_ctx.define(bytes.into_boxed_slice()); module.define_data(data_id, &data_ctx).unwrap();
                let local_data = module.declare_data_in_func(data_id, builder.func);
                let key_ptr = builder.ins().global_value(types::I64, local_data);
                let alloc_str_func = module.declare_func_in_func(runtime.alloc_string_id, builder.func);
                let call = builder.ins().call(alloc_str_func, &[key_ptr]);
                let key = builder.inst_results(call)[0];
                
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let call = builder.ins().call(get_func, &[t, key]);
                builder.inst_results(call)[0]
            }
            Expr::Binary(left, op, right) => {
                let lhs = Self::compile_expr(builder, module, runtime, user_funcs, fctx, left);
                let rhs = Self::compile_expr(builder, module, runtime, user_funcs, fctx, right);
                let cmp = match op {
                    TokenType::EqEq => Some(FloatCC::Equal), TokenType::NotEq => Some(FloatCC::NotEqual),
                    TokenType::Lt => Some(FloatCC::LessThan), TokenType::LtEq => Some(FloatCC::LessThanOrEqual),
                    TokenType::Gt => Some(FloatCC::GreaterThan), TokenType::GtEq => Some(FloatCC::GreaterThanOrEqual),
                    _ => None,
                };
                if op == &TokenType::EqEq || op == &TokenType::NotEq {
                    let eq_func = module.declare_func_in_func(runtime.eq_id, builder.func);
                    let call = builder.ins().call(eq_func, &[lhs, rhs]);
                    let is_eq = builder.inst_results(call)[0];
                    if op == &TokenType::NotEq {
                        let f = builder.ins().bitcast(types::F64, MemFlags::new(), is_eq);
                        let one = builder.ins().f64const(1.0);
                        let inv = builder.ins().fsub(one, f);
                        builder.ins().bitcast(types::I64, MemFlags::new(), inv)
                    } else { is_eq }
                } else if let Some(cc) = cmp {
                    let a_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), lhs);
                    let b_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), rhs);
                    let bool_val = builder.ins().fcmp(cc, a_f64, b_f64);
                    let int_val = builder.ins().uextend(types::I64, bool_val);
                    let f64_val = builder.ins().fcvt_from_uint(types::F64, int_val);
                    builder.ins().bitcast(types::I64, MemFlags::new(), f64_val)
                } else {
                    match op {
                        TokenType::Plus => {
                            let add_func = module.declare_func_in_func(runtime.add_id, builder.func);
                            let call = builder.ins().call(add_func, &[lhs, rhs]);
                            builder.inst_results(call)[0]
                        }
                        TokenType::Minus => { 
                            let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs); 
                            let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs); 
                            let res = builder.ins().fsub(a, b);
                            builder.ins().bitcast(types::I64, MemFlags::new(), res) 
                        }
                        TokenType::Star => { 
                            let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs); 
                            let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs); 
                            let res = builder.ins().fmul(a, b);
                            builder.ins().bitcast(types::I64, MemFlags::new(), res) 
                        }
                        TokenType::Slash => { 
                            let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs); 
                            let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs); 
                            let res = builder.ins().fdiv(a, b);
                            builder.ins().bitcast(types::I64, MemFlags::new(), res) 
                        }
                        TokenType::Percent => {
                            let mod_func = module.declare_func_in_func(runtime.mod_id, builder.func);
                            let call = builder.ins().call(mod_func, &[lhs, rhs]);
                            builder.inst_results(call)[0]
                        }
                        TokenType::And => {
                            let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                            let lhs_t_call = builder.ins().call(truthy_func, &[lhs]);
                            let lhs_t = builder.inst_results(lhs_t_call)[0];
                            let rhs_t_call = builder.ins().call(truthy_func, &[rhs]);
                            let rhs_t = builder.inst_results(rhs_t_call)[0];
                            let lhs_f = builder.ins().bitcast(types::F64, MemFlags::new(), lhs_t);
                            let rhs_f = builder.ins().bitcast(types::F64, MemFlags::new(), rhs_t);
                            let zero = builder.ins().f64const(0.0);
                            let lhs_true = builder.ins().fcmp(FloatCC::NotEqual, lhs_f, zero);
                            let rhs_true = builder.ins().fcmp(FloatCC::NotEqual, rhs_f, zero);
                            let and_val = builder.ins().band(lhs_true, rhs_true);
                            let int_val = builder.ins().uextend(types::I64, and_val);
                            let f64_val = builder.ins().fcvt_from_uint(types::F64, int_val);
                            builder.ins().bitcast(types::I64, MemFlags::new(), f64_val)
                        }
                        TokenType::Or => {
                            let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                            let lhs_t_call = builder.ins().call(truthy_func, &[lhs]);
                            let lhs_t = builder.inst_results(lhs_t_call)[0];
                            let rhs_t_call = builder.ins().call(truthy_func, &[rhs]);
                            let rhs_t = builder.inst_results(rhs_t_call)[0];
                            let lhs_f = builder.ins().bitcast(types::F64, MemFlags::new(), lhs_t);
                            let rhs_f = builder.ins().bitcast(types::F64, MemFlags::new(), rhs_t);
                            let zero = builder.ins().f64const(0.0);
                            let lhs_true = builder.ins().fcmp(FloatCC::NotEqual, lhs_f, zero);
                            let rhs_true = builder.ins().fcmp(FloatCC::NotEqual, rhs_f, zero);
                            let or_val = builder.ins().bor(lhs_true, rhs_true);
                            let int_val = builder.ins().uextend(types::I64, or_val);
                            let f64_val = builder.ins().fcvt_from_uint(types::F64, int_val);
                            builder.ins().bitcast(types::I64, MemFlags::new(), f64_val)
                        }
                        _ => panic!("Unsupported binary op"),
                    }
                }
            }
            Expr::InterpolatedString(parts) => {
                if parts.is_empty() {
                    return Self::compile_expr(builder, module, runtime, user_funcs, fctx, &Expr::String("".into()));
                }
                let mut acc = Self::compile_expr(builder, module, runtime, user_funcs, fctx, &parts[0]);
                let str_func = module.declare_func_in_func(runtime.str_id, builder.func);
                let add_func = module.declare_func_in_func(runtime.add_id, builder.func);
                
                // Convert initial part to string
                let acc_str_call = builder.ins().call(str_func, &[acc]);
                acc = builder.inst_results(acc_str_call)[0];

                for part in &parts[1..] {
                    let p_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, part);
                    let p_str_call = builder.ins().call(str_func, &[p_val]);
                    let p_str = builder.inst_results(p_str_call)[0];
                    let add_call = builder.ins().call(add_func, &[acc, p_str]);
                    acc = builder.inst_results(add_call)[0];
                }
                acc
            }
            _ => panic!("Unsupported expression in native codegen"),
        }
    }
}