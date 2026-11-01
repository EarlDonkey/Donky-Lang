use cranelift::prelude::*;
use cranelift_object::{ObjectBuilder, ObjectModule};
use cranelift_module::{Module, Linkage, FuncId, DataDescription};
use crate::ast::{Expr, Stmt};
use crate::lexer::TokenType;
use std::collections::HashMap;

#[derive(Clone, Copy)]
pub struct RuntimeFuncs {
    pub print_id: FuncId,
    pub alloc_array_id: FuncId,
    pub array_push_id: FuncId,
    pub alloc_map_id: FuncId,
    pub map_set_id: FuncId,
    pub index_get_id: FuncId,
    pub index_set_id: FuncId,
    pub alloc_string_id: FuncId,
    
    pub len_id: FuncId,
    pub push_id: FuncId,
    pub pop_id: FuncId,
    pub str_id: FuncId,
    pub num_id: FuncId,
    pub read_file_id: FuncId,
    pub add_id: FuncId,
    pub eq_id: FuncId,
    pub byte_at_id: FuncId,
    pub substr_id: FuncId,
    pub truthy_id: FuncId,

    pub throw_id: FuncId,
    pub has_exception_id: FuncId,
    pub get_exception_id: FuncId,
    pub clear_exception_id: FuncId,

    pub get_args_id: FuncId,
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
    exception_target: Option<Block>,
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
            add_id: declare("donky_add", &[types::I64, types::I64], Some(types::I64)),
            eq_id: declare("donky_eq", &[types::I64, types::I64], Some(types::I64)),
            byte_at_id: declare("donky_byte_at", &[types::I64, types::I64], Some(types::I64)),
            substr_id: declare("donky_substr", &[types::I64, types::I64, types::I64], Some(types::I64)),
            truthy_id: declare("donky_truthy", &[types::I64], Some(types::I64)),
            
            throw_id: declare("donky_throw", &[types::I64], None),
            has_exception_id: declare("donky_has_exception", &[], Some(types::I32)),
            get_exception_id: declare("donky_get_exception", &[], Some(types::I64)),
            clear_exception_id: declare("donky_clear_exception", &[], None),
            
            get_args_id: declare("donky_get_args", &[], Some(types::I64)),
        };
        
        AotCompiler {
            builder_context: FunctionBuilderContext::new(),
            ctx: codegen::Context::new(),
            module,
            user_functions: HashMap::new(),
            runtime,
        }
    }

    pub fn compile_to_object(mut self, stmts: &[Stmt], output_path: &str) -> Result<(), String> {
        for stmt in stmts {
            if let Stmt::Function(name, params, _) = stmt {
                let mut sig = self.module.make_signature();
                for _ in 0..params.len() { sig.params.push(AbiParam::new(types::I64)); }
                sig.returns.push(AbiParam::new(types::I64));
                let export_name = format!("donky_fn_{}", name);
                let func_id = self.module.declare_function(&export_name, Linkage::Export, &sig).unwrap();
                self.user_functions.insert(name.clone(), func_id);
            }
        }

        for stmt in stmts {
            if let Stmt::Function(name, params, body) = stmt {
                let func_id = self.user_functions[name];
                self.compile_function(func_id, params, body);
            }
        }

        // donky_toplevel returns I64 to match all other functions
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

            let mut fctx = FunctionContext { variables: HashMap::new(), next_var: 0, exception_target: None };
            
            // Inject args
            let get_args_func = self.module.declare_func_in_func(self.runtime.get_args_id, builder.func);
            let args_call = builder.ins().call(get_args_func, &[]);
            let args_val = builder.inst_results(args_call)[0];
            let args_var = Variable::new(fctx.next_var);
            fctx.next_var += 1;
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

            let mut fctx = FunctionContext { variables: HashMap::new(), next_var: 0, exception_target: None };
            let block_params: Vec<Value> = builder.block_params(entry_block).to_vec();

            for (i, param_name) in params.iter().enumerate() {
                let var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(var, types::I64);
                builder.def_var(var, block_params[i]);
                fctx.variables.insert(param_name.clone(), var);
            }

            // Inject args
            let get_args_func = self.module.declare_func_in_func(self.runtime.get_args_id, builder.func);
            let args_call = builder.ins().call(get_args_func, &[]);
            let args_val = builder.inst_results(args_call)[0];
            let args_var = Variable::new(fctx.next_var);
            fctx.next_var += 1;
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

    fn compile_stmt(
        builder: &mut FunctionBuilder, 
        module: &mut ObjectModule, 
        runtime: &RuntimeFuncs,
        user_funcs: &HashMap<String, FuncId>,
        fctx: &mut FunctionContext, 
        stmt: &Stmt,
    ) -> bool {
        match stmt {
            Stmt::Let(name, expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(var, types::I64);
                builder.def_var(var, val);
                fctx.variables.insert(name.clone(), var);
                false
            }
            Stmt::Assign(name, expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let var = fctx.variables.get(name).expect(&format!("Undefined variable: {}", name));
                builder.def_var(*var, val);
                false
            }
            Stmt::IndexSet(target, index, val) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, fctx, target);
                let i = Self::compile_expr(builder, module, runtime, user_funcs, fctx, index);
                let v = Self::compile_expr(builder, module, runtime, user_funcs, fctx, val);
                let local_callee = module.declare_func_in_func(runtime.index_set_id, builder.func);
                builder.ins().call(local_callee, &[t, i, v]);
                false
            }
            Stmt::Print(expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let local_callee = module.declare_func_in_func(runtime.print_id, builder.func);
                builder.ins().call(local_callee, &[val]);
                false
            }
            Stmt::ExprStmt(expr) => {
                Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                false
            }
            Stmt::Return(expr) => {
                let val = if let Some(e) = expr {
                    Self::compile_expr(builder, module, runtime, user_funcs, fctx, e)
                } else {
                    builder.ins().iconst(types::I64, 0)
                };
                builder.ins().return_(&[val]);
                true
            }
            Stmt::Throw(expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, expr);
                let throw_func = module.declare_func_in_func(runtime.throw_id, builder.func);
                builder.ins().call(throw_func, &[val]);
                
                if let Some(target) = fctx.exception_target {
                    builder.ins().jump(target, &[]);
                } else {
                    let zero = builder.ins().iconst(types::I64, 0);
                    builder.ins().return_(&[zero]);
                }
                true
            }
            Stmt::Try(try_b, err_name, catch_b) => {
                let catch_block = builder.create_block();
                let merge_block = builder.create_block();
                
                let prev_target = fctx.exception_target;
                fctx.exception_target = Some(catch_block);
                
                let try_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, try_b);
                
                fctx.exception_target = prev_target;
                
                if !try_term {
                    builder.ins().jump(merge_block, &[]);
                }
                
                builder.seal_block(catch_block);
                builder.switch_to_block(catch_block);
                
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
                    builder.ins().jump(merge_block, &[]);
                }
                
                builder.switch_to_block(merge_block);
                builder.seal_block(merge_block);
                false
            }
            Stmt::If(cond, then_b, else_b) => {
                let cond_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, cond);
                
                let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                let call = builder.ins().call(truthy_func, &[cond_val]);
                let truthy_val = builder.inst_results(call)[0];
                
                let truthy_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), truthy_val);
                let zero = builder.ins().f64const(0.0);
                let is_true = builder.ins().fcmp(FloatCC::NotEqual, truthy_f64, zero);

                let then_block = builder.create_block();
                let else_block = builder.create_block();
                let merge_block = builder.create_block();

                builder.ins().brif(is_true, then_block, &[], else_block, &[]);
                builder.seal_block(then_block);
                builder.seal_block(else_block);

                builder.switch_to_block(then_block);
                let then_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, then_b);
                if !then_term {
                    builder.ins().jump(merge_block, &[]);
                }

                builder.switch_to_block(else_block);
                let else_term = if let Some(eb) = else_b {
                    Self::compile_stmt(builder, module, runtime, user_funcs, fctx, eb)
                } else { false };
                if !else_term {
                    builder.ins().jump(merge_block, &[]);
                }

                builder.seal_block(merge_block);
                builder.switch_to_block(merge_block);
                false
            }
            Stmt::While(cond, body) => {
                let header_block = builder.create_block();
                let body_block = builder.create_block();
                let exit_block = builder.create_block();
                
                builder.ins().jump(header_block, &[]);

                builder.switch_to_block(header_block);
                builder.seal_block(header_block);
                let cond_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, cond);
                
                let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                let call = builder.ins().call(truthy_func, &[cond_val]);
                let truthy_val = builder.inst_results(call)[0];
                
                let truthy_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), truthy_val);
                let zero = builder.ins().f64const(0.0);
                let is_true = builder.ins().fcmp(FloatCC::NotEqual, truthy_f64, zero);
                
                builder.ins().brif(is_true, body_block, &[], exit_block, &[]);
                builder.seal_block(body_block);
                builder.seal_block(exit_block);

                builder.switch_to_block(body_block);
                let body_term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, body);
                if !body_term {
                    builder.ins().jump(header_block, &[]);
                }

                builder.switch_to_block(exit_block);
                false
            }
            Stmt::For(var_name, iter_expr, body) => {
                let iter_var_name = format!("__iter_{}", fctx.next_var);
                let idx_var_name = format!("__idx_{}", fctx.next_var);
                
                let iter_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, iter_expr);
                let iter_var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(iter_var, types::I64);
                builder.def_var(iter_var, iter_val);
                fctx.variables.insert(iter_var_name.clone(), iter_var);

                let zero = builder.ins().iconst(types::I64, 0);
                let idx_var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(idx_var, types::I64);
                builder.def_var(idx_var, zero);
                fctx.variables.insert(idx_var_name.clone(), idx_var);

                let header_block = builder.create_block();
                let body_block = builder.create_block();
                let exit_block = builder.create_block();
                
                builder.ins().jump(header_block, &[]);

                builder.switch_to_block(header_block);
                builder.seal_block(header_block);
                
                let len_func = module.declare_func_in_func(runtime.len_id, builder.func);
                let iter_h = builder.use_var(iter_var);
                let len_call = builder.ins().call(len_func, &[iter_h]);
                let len_val = builder.inst_results(len_call)[0];
                
                let idx_h = builder.use_var(idx_var);
                let idx_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), idx_h);
                let len_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), len_val);
                let cond = builder.ins().fcmp(FloatCC::LessThan, idx_f64, len_f64);
                builder.ins().brif(cond, body_block, &[], exit_block, &[]);
                builder.seal_block(body_block);
                builder.seal_block(exit_block);

                builder.switch_to_block(body_block);
                
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let item_call = builder.ins().call(get_func, &[iter_h, idx_h]);
                let item_val = builder.inst_results(item_call)[0];
                
                let loop_var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(loop_var, types::I64);
                builder.def_var(loop_var, item_val);
                fctx.variables.insert(var_name.clone(), loop_var);

                let term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, body);
                if !term {
                    let one = builder.ins().iconst(types::I64, 1);
                    let add_func = module.declare_func_in_func(runtime.add_id, builder.func);
                    let add_call = builder.ins().call(add_func, &[idx_h, one]);
                    let next_idx = builder.inst_results(add_call)[0];
                    builder.def_var(idx_var, next_idx);
                    
                    builder.ins().jump(header_block, &[]);
                }

                builder.switch_to_block(exit_block);
                false
            }
            Stmt::Match(target, cases, else_block) => {
                let target_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, target);
                let merge_block = builder.create_block();
                
                let mut check_blocks = Vec::new();
                for _ in 0..cases.len() { check_blocks.push(builder.create_block()); }
                let else_check_block = if else_block.is_some() { Some(builder.create_block()) } else { None };

                builder.ins().jump(check_blocks[0], &[]);
                builder.seal_block(check_blocks[0]);

                for (i, (case_expr, body)) in cases.iter().enumerate() {
                    builder.switch_to_block(check_blocks[i]);
                    
                    let case_val = Self::compile_expr(builder, module, runtime, user_funcs, fctx, case_expr);
                    let eq_func = module.declare_func_in_func(runtime.eq_id, builder.func);
                    let eq_call = builder.ins().call(eq_func, &[target_val, case_val]);
                    let is_eq = builder.inst_results(eq_call)[0];
                    let is_eq_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), is_eq);
                    let zero = builder.ins().f64const(0.0);
                    let cond = builder.ins().fcmp(FloatCC::NotEqual, is_eq_f64, zero);

                    let body_block = builder.create_block();
                    builder.seal_block(body_block);
                    
                    let next_check = if i + 1 < cases.len() { check_blocks[i+1] } else { else_check_block.unwrap_or(merge_block) };
                    
                    builder.ins().brif(cond, body_block, &[], next_check, &[]);
                    if i + 1 < cases.len() { builder.seal_block(check_blocks[i+1]); }
                    if let Some(eb) = else_check_block { builder.seal_block(eb); }

                    builder.switch_to_block(body_block);
                    let term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, body);
                    if !term {
                        builder.ins().jump(merge_block, &[]);
                    }
                }

                if let Some(eb) = else_block {
                    if let Some(eb_block) = else_check_block {
                        builder.switch_to_block(eb_block);
                        let term = Self::compile_stmt(builder, module, runtime, user_funcs, fctx, eb);
                        if !term {
                            builder.ins().jump(merge_block, &[]);
                        }
                    }
                }

                builder.seal_block(merge_block);
                builder.switch_to_block(merge_block);
                false
            }
            Stmt::Block(stmts) => {
                for s in stmts {
                    if Self::compile_stmt(builder, module, runtime, user_funcs, fctx, s) { return true; }
                }
                false
            }
            Stmt::Function(_, _, _) => false,
            _ => panic!("Unsupported statement in native codegen"),
        }
    }

    fn compile_expr(
        builder: &mut FunctionBuilder, 
        module: &mut ObjectModule, 
        runtime: &RuntimeFuncs,
        user_funcs: &HashMap<String, FuncId>,
        fctx: &mut FunctionContext, 
        expr: &Expr,
    ) -> Value {
        match expr {
            Expr::Number(n) => builder.ins().iconst(types::I64, n.to_bits() as i64),
            Expr::Bool(b) => {
                let tag_base: u64 = 0x7FF8_0000_0000_0000;
                let tag_bool: u64 = 1;
                let val = tag_base | (tag_bool << 48) | (if *b { 1 } else { 0 });
                builder.ins().iconst(types::I64, val as i64)
            }
            Expr::String(s) => {
                let data_id = module.declare_data(&format!("str_{}_{}", s.len(), s.as_ptr() as usize), Linkage::Export, true, false).unwrap();
                let mut data_ctx = DataDescription::new();
                let mut bytes = s.as_bytes().to_vec();
                bytes.push(0);
                data_ctx.define(bytes.into_boxed_slice());
                module.define_data(data_id, &data_ctx).unwrap();
                
                let local_data = module.declare_data_in_func(data_id, builder.func);
                let ptr = builder.ins().global_value(types::I64, local_data);
                
                let local_callee = module.declare_func_in_func(runtime.alloc_string_id, builder.func);
                let call = builder.ins().call(local_callee, &[ptr]);
                builder.inst_results(call)[0]
            }
            Expr::Ident(name) => {
                let var = fctx.variables.get(name).expect(&format!("Undefined variable: {}", name));
                builder.use_var(*var)
            }
            Expr::Call(name, args) => {
                if let Some(func_id) = user_funcs.get(name) {
                    let local_callee = module.declare_func_in_func(*func_id, builder.func);
                    let mut arg_vals = Vec::new();
                    for arg in args {
                        arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, fctx, arg));
                    }
                    let call = builder.ins().call(local_callee, &arg_vals);
                    let ret_val = builder.inst_results(call)[0];
                    
                    // Check for exception
                    let has_exc_func = module.declare_func_in_func(runtime.has_exception_id, builder.func);
                    let exc_call = builder.ins().call(has_exc_func, &[]);
                    let has_exc = builder.inst_results(exc_call)[0];
                    let zero = builder.ins().iconst(types::I32, 0);
                    let cmp = builder.ins().icmp(IntCC::NotEqual, has_exc, zero);
                    
                    let cont_block = builder.create_block();
                    let exc_block = builder.create_block();
                    builder.ins().brif(cmp, exc_block, &[], cont_block, &[]);
                    builder.seal_block(exc_block);
                    builder.seal_block(cont_block);
                    
                    builder.switch_to_block(exc_block);
                    if let Some(target) = fctx.exception_target {
                        builder.ins().jump(target, &[]);
                    } else {
                        let zero_val = builder.ins().iconst(types::I64, 0);
                        builder.ins().return_(&[zero_val]);
                    }
                    
                    builder.switch_to_block(cont_block);
                    ret_val
                } else {
                    let builtin_id = match name.as_str() {
                        "len" => runtime.len_id,
                        "push" => runtime.push_id,
                        "pop" => runtime.pop_id,
                        "str" => runtime.str_id,
                        "num" => runtime.num_id,
                        "read_file" => runtime.read_file_id,
                        "byte_at" => runtime.byte_at_id,
                        "substr" => runtime.substr_id,
                        _ => panic!("Unknown builtin function: {}", name),
                    };
                    
                    let local_callee = module.declare_func_in_func(builtin_id, builder.func);
                    let mut arg_vals = Vec::new();
                    for arg in args {
                        arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, fctx, arg));
                    }
                    let call = builder.ins().call(local_callee, &arg_vals);
                    let results = builder.inst_results(call);
                    if results.is_empty() {
                        builder.ins().iconst(types::I64, 0)
                    } else {
                        results[0]
                    }
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
            Expr::Index(target, index) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, fctx, target);
                let i = Self::compile_expr(builder, module, runtime, user_funcs, fctx, index);
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let call = builder.ins().call(get_func, &[t, i]);
                builder.inst_results(call)[0]
            }
            Expr::Binary(left, op, right) => {
                let lhs = Self::compile_expr(builder, module, runtime, user_funcs, fctx, left);
                let rhs = Self::compile_expr(builder, module, runtime, user_funcs, fctx, right);
                
                let cmp = match op {
                    TokenType::EqEq => Some(FloatCC::Equal),
                    TokenType::NotEq => Some(FloatCC::NotEqual),
                    TokenType::Lt => Some(FloatCC::LessThan),
                    TokenType::LtEq => Some(FloatCC::LessThanOrEqual),
                    TokenType::Gt => Some(FloatCC::GreaterThan),
                    TokenType::GtEq => Some(FloatCC::GreaterThanOrEqual),
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
                    } else {
                        is_eq
                    }
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
                        _ => panic!("Unsupported binary op"),
                    }
                }
            }
            _ => panic!("Unsupported expression in native codegen"),
        }
    }
}