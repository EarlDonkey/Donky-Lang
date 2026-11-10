use cranelift::prelude::*;
use cranelift_object::{ObjectBuilder, ObjectModule};
use cranelift_module::{Module, Linkage, FuncId, DataDescription};
use crate::ast::{Expr, Stmt};
use crate::lexer::TokenType;
use crate::ast::Pos;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicUsize, Ordering};


fn fatal(pos: Pos, msg: &str) -> ! {
    if pos.line > 0 { eprintln!("Error at line {}, col {}: {}", pos.line, pos.col, msg); }
    else { eprintln!("Error: {}", msg); }
    std::process::exit(1);
}

static STRING_COUNTER: AtomicUsize = AtomicUsize::new(0);

// NaN-boxing constants. MUST match runtime_template.c (see its BEGIN TAG ASSERTIONS block).
#[allow(dead_code)] const TAG_MASK: u64 = 0xFFF8_0000_0000_0000;
#[allow(dead_code)] const TAG_BASE: u64 = 0x7FF8_0000_0000_0000;
#[allow(dead_code)] const TAG_BOOL_VAL: u64 = 1;
const BOOL_TRUE_BITS:  i64 = (TAG_BASE | (TAG_BOOL_VAL << 48) | 1) as i64;
const BOOL_FALSE_BITS: i64 = (TAG_BASE | (TAG_BOOL_VAL << 48))     as i64;
const NULL_BITS:       i64 = TAG_BASE as i64;
#[allow(dead_code)] const NUM_ZERO: f64 = 0.0;
#[allow(dead_code)] const NUM_ONE: f64 = 1.0;
#[allow(dead_code)] const NUM_ZERO_BITS: i64 = 0;
#[allow(dead_code)] const NUM_FALSE_BITS: i64 = 0;
#[allow(dead_code)] const NUM_TRUE_BITS: i64 = 0x3FF0_0000_0000_0000;
#[allow(dead_code)] const TYPE_KEY: &str = "__type__";

pub const STDLIB_BUILTINS: [(&str, &str, usize); 103] = [    
    ("sys_cwd", "donky_sys_cwd", 0),
    ("sys_chdir", "donky_sys_chdir", 1),
    ("sys_getenv", "donky_sys_getenv", 1),
    ("sys_setenv", "donky_sys_setenv", 2),
    ("sys_remove", "donky_sys_remove", 1),
    ("sys_rename", "donky_sys_rename", 2),
    ("sys_append_file", "donky_sys_append_file", 2),
    ("sys_puts", "donky_sys_puts", 1),
    ("sys_eputs", "donky_sys_eputs", 1),
    ("sys_exit", "donky_sys_exit", 1),
    ("sys_epoch_ms", "donky_sys_epoch_ms", 0),
    ("sys_gmtime", "donky_sys_gmtime", 1),
    ("sys_timegm", "donky_sys_timegm", 6),
    ("sys_strftime", "donky_sys_strftime", 2),
    ("sys_md5", "donky_sys_md5", 1),
    ("sys_sha1", "donky_sys_sha1", 1),
    ("sys_sha256", "donky_sys_sha256", 1),
    ("sys_hmac_sha256", "donky_sys_hmac_sha256", 2),
    ("http_request", "donky_http_request", 4),
    ("http_status", "donky_http_status", 0),
    ("global_get", "donky_global_get", 1),
    ("global_set", "donky_global_set", 2),
    ("ffi_alloc", "donky_ffi_alloc", 1),
    ("ffi_free",  "donky_ffi_free",  1),
    ("ffi_peek",  "donky_ffi_peek",  2),
    ("ffi_poke",  "donky_ffi_poke",  3),
    ("pow",  "donky_pow",  2),
    ("exp",  "donky_exp",  1),
    ("log",  "donky_log",  1),
    ("tanh", "donky_tanh", 1),
    ("max",  "donky_max",  2),
    ("min",  "donky_min",  2),
    ("abs",  "donky_abs",  1),
    ("dll_call_sig", "donky_dll_call_sig", 4),
    ("dll_callback", "donky_dll_callback", 3),
    ("syscall", "donky_syscall", 2),
    ("mem_alloc", "donky_mem_alloc", 1),
    ("mem_write", "donky_mem_write", 2),
    ("mem_protect", "donky_mem_protect", 3),
    ("mem_free", "donky_mem_free", 2),
    ("fork", "donky_fork", 0),
    ("exec", "donky_exec", 2),
    ("wait", "donky_wait", 1),
    ("pipe", "donky_pipe", 0),
    ("dup2", "donky_dup2", 2),
    ("mmap", "donky_mmap", 3),
    ("munmap", "donky_munmap", 2),
    ("setuid", "donky_setuid", 1),
    ("setgid", "donky_setgid", 1),
    ("seteuid", "donky_seteuid", 1),
    ("setegid", "donky_setegid", 1),
    ("chroot", "donky_chroot", 1),
    ("unshare", "donky_unshare", 1),
    ("prctl", "donky_prctl", 5),
    ("seccomp", "donky_seccomp", 3),
    ("getpwnam", "donky_getpwnam", 1),
    ("getpwuid", "donky_getpwuid", 1),
    ("uname", "donky_uname", 0),
    ("getrlimit", "donky_getrlimit", 1),
    ("setrlimit", "donky_setrlimit", 3),
    ("sys_write_binary", "donky_sys_write_binary", 2),
    ("is_valid_handle", "donky_is_valid_handle", 1),
    ("is_null_handle", "donky_is_null_handle", 1),
    ("dll_call_var_sig", "donky_dll_call_var_sig", 6),
    ("tensor_add", "donky_tensor_add", 2),
    ("tensor_mul", "donky_tensor_mul", 2),
    ("tensor_matmul", "donky_tensor_matmul", 5),
    ("tensor_relu", "donky_tensor_relu", 1),
    ("tensor_softmax", "donky_tensor_softmax", 1),
    ("tensor_sgd_step", "donky_tensor_sgd_step", 3),
    ("tensor_matmul_f32", "donky_tensor_matmul_f32", 5),
    ("tensor_relu_f32", "donky_tensor_relu_f32", 1),
    ("tensor_softmax_f32", "donky_tensor_softmax_f32", 1),
    ("tensor_rms_norm",  "donky_tensor_rms_norm",  1),
    ("tensor_rope",      "donky_tensor_rope",      2),
    ("tensor_attention", "donky_tensor_attention", 5),
    ("tensor_silu",      "donky_tensor_silu",      1),
    ("sin", "donky_sin", 1),
    ("cos", "donky_cos", 1),
    ("mmap_model",         "donky_mmap_model",         1),
    ("read_gguf_metadata", "donky_read_gguf_metadata", 1),
    ("tensor_matmul_q4",   "donky_tensor_matmul_q4",   5),
    ("ffi_peek_f32", "donky_ffi_peek_f32", 2),
    ("ffi_peek_str", "donky_ffi_peek_str", 2),
    ("ffi_peek_q4_row", "donky_ffi_peek_q4_row", 3),
    ("tensor_matmul_q6", "donky_tensor_matmul_q6", 5),
    ("secure_random", "donky_secure_random", 1),
    ("exec_wait",     "donky_exec_wait",     3),
    ("ffi_peek_vocab", "donky_ffi_peek_vocab", 2),
    ("tokenize_bpe", "donky_tokenize_bpe", 2),
    ("tensor_rope_base", "donky_tensor_rope_base", 3),
    ("tensor_attention_decode", "donky_tensor_attention_decode", 7),
    ("world_gen",   "donky_world_gen",   6),   // ptr, w, h, seed, chunk_x, chunk_y
    ("world_mesh",  "donky_world_mesh",  8),   // ptr, w, h, cam_x, cam_y, vw, vh, out_ptr
    ("world_set",   "donky_world_set",   6),   // ptr, w, h, x, y, block
    ("world_physics", "donky_world_physics", 4),
    ("tcp_write_bytes", "donky_tcp_write_bytes", 3),
    ("agents_new",         "donky_agents_new",         1),
    ("agents_tick",        "donky_agents_tick",        6),
    ("agents_render",      "donky_agents_render",      9),
    ("agents_count_alive", "donky_agents_count_alive", 2),
    ("agents_kill",        "donky_agents_kill",        3),   // 3, not 2: added count
    ("agents_place_on_surface", "donky_agents_place_on_surface", 5),
];

#[derive(Clone, Copy)]
pub struct RuntimeFuncs {
    pub print_id: FuncId, pub make_number_id: FuncId, pub alloc_array_id: FuncId, pub array_push_id: FuncId,
    pub alloc_map_id: FuncId, pub map_set_id: FuncId, pub index_get_id: FuncId,
    pub index_set_id: FuncId, pub alloc_string_id: FuncId,
    pub len_id: FuncId, pub push_id: FuncId, pub pop_id: FuncId, pub str_id: FuncId,
    pub num_id: FuncId, pub read_file_id: FuncId, pub write_file_id: FuncId,
    pub file_exists_id: FuncId,
    pub add_id: FuncId, pub eq_id: FuncId, pub byte_at_id: FuncId,
    pub substr_id: FuncId, pub byte_substr_id: FuncId, pub truthy_id: FuncId,
    pub upper_id: FuncId, pub lower_id: FuncId, pub split_id: FuncId,
    pub join_id: FuncId, pub now_id: FuncId, pub now_ms_id: FuncId, pub contains_id: FuncId,
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
    pub gui_init_id: FuncId, pub gui_draw_text_id: FuncId, pub gui_update_id: FuncId,
    pub gui_add_button_id: FuncId, pub gui_add_label_id: FuncId, pub gui_update_text_id: FuncId, 
    pub gui_get_clicked_id: FuncId, pub gui_clear_id: FuncId, pub gui_repaint_id: FuncId,
    pub gui_draw_rect_id: FuncId, pub cpu_usage_id: FuncId, pub gui_webview_id: FuncId,
    pub kill_process_id: FuncId, pub uptime_id: FuncId, pub cpu_cores_id: FuncId,
    pub disk_info_id: FuncId, pub sub_id: FuncId, pub mul_id: FuncId, pub div_id: FuncId,
    pub dll_load_id: FuncId, pub dll_sym_id: FuncId, pub dll_call_id: FuncId,
    pub dll_calli_id: FuncId, pub ffi_cstr_id: FuncId,
    pub json_decode_id: FuncId, pub json_encode_id: FuncId,
    pub json_pretty_id: FuncId, pub json_error_id: FuncId,
    pub stdlib_ids: [FuncId; 103], pub fdiv_id: FuncId,
    pub alloc_function_id: FuncId, pub call_function_id: FuncId,
}

pub struct AotCompiler {
    builder_context: FunctionBuilderContext,
    ctx: codegen::Context,
    module: ObjectModule,
    user_functions: HashMap<String, FuncId>,
    runtime: RuntimeFuncs,
    pending_closures: Vec<(String, Vec<String>, Box<Stmt>)>,
    top_lets: HashSet<String>,
}

struct FunctionContext {
    variables: HashMap<String, Variable>,
    num_vars: HashSet<String>,
    next_var: usize,
    exception_target: Option<Block>,
    loop_stack: Vec<(Block, Block)>,
    top_lets: HashSet<String>,
}

fn emit_exception_check(
    builder: &mut FunctionBuilder,
    module: &mut ObjectModule,
    runtime: &RuntimeFuncs,
    fctx: &FunctionContext,
) {
    let has_exc_func = module.declare_func_in_func(runtime.has_exception_id, builder.func);
    let exc_call = builder.ins().call(has_exc_func, &[]);
    let has_exc = builder.inst_results(exc_call)[0];
    let zero_i32 = builder.ins().iconst(types::I32, 0);
    let cmp = builder.ins().icmp(IntCC::NotEqual, has_exc, zero_i32);
    let cont_block = builder.create_block();
    let exc_block = builder.create_block();
    builder.ins().brif(cmp, exc_block, &[], cont_block, &[]);
    builder.seal_block(exc_block);
    builder.seal_block(cont_block);
    builder.switch_to_block(exc_block);
    if let Some(target) = fctx.exception_target {
        builder.ins().jump(target, &[]);
    } else {
        let null_val = builder.ins().iconst(types::I64, NULL_BITS);
        builder.ins().return_(&[null_val]);
    }
    builder.switch_to_block(cont_block);
}

/// Boxes a compile-time string constant as a Donky string value (same pattern as FieldSet/FieldAccess).
fn alloc_str_const(builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs, s: &str) -> Value {
    let idx = STRING_COUNTER.fetch_add(1, Ordering::SeqCst);
    let data_id = module.declare_data(&format!("str_{}", idx), Linkage::Export, true, false).unwrap();
    let mut data_ctx = DataDescription::new();
    let mut bytes = s.as_bytes().to_vec(); bytes.push(0);
    data_ctx.define(bytes.into_boxed_slice()); module.define_data(data_id, &data_ctx).unwrap();
    let local_data = module.declare_data_in_func(data_id, builder.func);
    let ptr = builder.ins().global_value(types::I64, local_data);
    let alloc_func = module.declare_func_in_func(runtime.alloc_string_id, builder.func);
    let call = builder.ins().call(alloc_func, &[ptr]);
    builder.inst_results(call)[0]
}

/// FuncId of a STDLIB_BUILTINS entry by Donky name (same lookup the builtin dispatch uses).
fn stdlib_func_id(runtime: &RuntimeFuncs, name: &str) -> FuncId {
    let i = STDLIB_BUILTINS.iter().position(|(n, _, _)| *n == name)
        .unwrap_or_else(|| panic!("STDLIB_BUILTINS has no `{}`", name));
    runtime.stdlib_ids[i]
}

/// donky_global_set("name", val). Result ignored; global_set never throws.
fn emit_global_set(builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs, name: &str, val: Value) {
    let key = alloc_str_const(builder, module, runtime, name);
    let set_func = module.declare_func_in_func(stdlib_func_id(runtime, "global_set"), builder.func);
    builder.ins().call(set_func, &[key, val]);
}

/// donky_global_get("name"): the value, or null if never set.
fn emit_global_get(builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs, name: &str) -> Value {
    let key = alloc_str_const(builder, module, runtime, name);
    let get_func = module.declare_func_in_func(stdlib_func_id(runtime, "global_get"), builder.func);
    let call = builder.ins().call(get_func, &[key]);
    builder.inst_results(call)[0]
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

        let stdlib_ids: [FuncId; 103] = std::array::from_fn(|i| {
            let (_, cname, n) = STDLIB_BUILTINS[i];
            declare(cname, &vec![types::I64; n], Some(types::I64))
        });

        let runtime = RuntimeFuncs {
            print_id: declare("donky_print", &[types::I64], None),
            make_number_id: declare("donky_make_number", &[types::F64], Some(types::I64)),
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
            now_ms_id: declare("donky_now_ms", &[], Some(types::I64)),
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
            cls_id: declare("donky_cls", &[], Some(types::I64)),
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
            gui_init_id: declare("donky_gui_init", &[types::I64, types::I64, types::I64], Some(types::I64)),
            gui_draw_text_id: declare("donky_gui_draw_text", &[types::I64, types::I64, types::I64], Some(types::I64)),
            gui_update_id: declare("donky_gui_update", &[], Some(types::I64)),
            gui_add_button_id: declare("donky_gui_add_button", &[types::I64, types::I64, types::I64, types::I64, types::I64], Some(types::I64)),
            gui_add_label_id: declare("donky_gui_add_label", &[types::I64, types::I64, types::I64, types::I64, types::I64], Some(types::I64)),
            gui_update_text_id: declare("donky_gui_update_text", &[types::I64, types::I64], Some(types::I64)),
            gui_get_clicked_id: declare("donky_gui_get_clicked", &[], Some(types::I64)),
            gui_clear_id: declare("donky_gui_clear", &[], Some(types::I64)),
            gui_repaint_id: declare("donky_gui_repaint", &[], Some(types::I64)),
            gui_draw_rect_id: declare("donky_gui_draw_rect", &[types::I64, types::I64, types::I64, types::I64, types::I64], Some(types::I64)),
            cpu_usage_id: declare("donky_get_cpu_usage", &[], Some(types::I64)),
            gui_webview_id: declare("donky_gui_webview", &[types::I64], Some(types::I64)),
            kill_process_id: declare("donky_kill_process", &[types::I64], Some(types::I64)),
            uptime_id: declare("donky_get_uptime", &[], Some(types::I64)),
            cpu_cores_id: declare("donky_get_cpu_cores", &[], Some(types::I64)),
            disk_info_id: declare("donky_get_disk_info", &[], Some(types::I64)),
            sub_id: declare("donky_sub", &[types::I64, types::I64], Some(types::I64)),
            mul_id: declare("donky_mul", &[types::I64, types::I64], Some(types::I64)),
            div_id: declare("donky_div", &[types::I64, types::I64], Some(types::I64)),
            dll_load_id:  declare("donky_dll_load",  &[types::I64], Some(types::I64)),
            dll_sym_id:   declare("donky_dll_sym",   &[types::I64, types::I64], Some(types::I64)),
            dll_call_id:  declare("donky_dll_call",  &[types::I64, types::I64], Some(types::I64)),
            dll_calli_id: declare("donky_dll_calli", &[types::I64, types::I64], Some(types::I64)),
            ffi_cstr_id:  declare("donky_ffi_cstr",  &[types::I64], Some(types::I64)),
            json_decode_id: declare("donky_json_decode", &[types::I64], Some(types::I64)),
            json_encode_id: declare("donky_json_encode", &[types::I64], Some(types::I64)),
            json_pretty_id: declare("donky_json_pretty", &[types::I64], Some(types::I64)),
            json_error_id:  declare("donky_json_error",  &[], Some(types::I64)),
            fdiv_id: declare("donky_fdiv", &[types::I64, types::I64], Some(types::I64)),
            alloc_function_id: declare("donky_alloc_function", &[types::I64], Some(types::I64)),
            call_function_id:  declare("donky_call_function",  &[types::I64, types::I64], Some(types::I64)),
            stdlib_ids,
            
        };
        
        AotCompiler { builder_context: FunctionBuilderContext::new(), ctx: codegen::Context::new(), module, user_functions: HashMap::new(), runtime, pending_closures: Vec::new(), top_lets: HashSet::new() }
    }

    pub fn compile_to_object(mut self, stmts: &[Stmt], output_path: &str) -> Result<(), String> {
        let mut latest_funcs: HashMap<String, (Vec<String>, Box<Stmt>)> = HashMap::new();
        let mut visited: HashSet<std::path::PathBuf> = HashSet::new();
        resolve_imports(stmts, ".", &mut latest_funcs, &mut visited)?;
        // ----------------------------------------

        // Top-level `let`s become globals (readable inside functions via donky_global_get).
        // Only DIRECT top-level lets count (not inside blocks/ifs). `args` is the builtin slot: never a global.
        for s in stmts {
            if let Stmt::Let(name, _) = s {
                if name != "args" { self.top_lets.insert(name.clone()); }
            }
        }
        // Function bodies (main + imported), cloned before the compile loop consumes latest_funcs,
        // so DCE sees names that only functions read.
        let fn_bodies: Vec<Stmt> = latest_funcs.values().map(|(_, b)| (**b).clone()).collect();

        for (name, (params, _)) in &latest_funcs {
            let mut sig = self.module.make_signature();
            for _ in 0..params.len() { sig.params.push(AbiParam::new(types::I64)); }
            sig.returns.push(AbiParam::new(types::I64));
            let func_id = self.module.declare_function(&format!("donky_fn_{}", name), Linkage::Export, &sig).unwrap();
            self.user_functions.insert(name.clone(), func_id);
        }

        let mut pc = std::mem::take(&mut self.pending_closures);
        for (name, (params, body)) in latest_funcs {
            let func_id = self.user_functions[&name];
            self.compile_function(func_id, &params, &body, &mut pc);
        }
        self.pending_closures = pc;

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

            let mut fctx = FunctionContext { 
                variables: HashMap::new(), 
                num_vars: HashSet::new(), 
                next_var: 0, 
                exception_target: None, 
                loop_stack: Vec::new(),
                top_lets: self.top_lets.clone()
            };
            
            let get_args_func = self.module.declare_func_in_func(self.runtime.get_args_id, builder.func);
            let args_call = builder.ins().call(get_args_func, &[]);
            let args_val = builder.inst_results(args_call)[0];
            let args_var = Variable::new(fctx.next_var); fctx.next_var += 1;
            builder.declare_var(args_var, types::I64);
            builder.def_var(args_var, args_val);
            fctx.variables.insert("args".to_string(), args_var);
            
            // --- DCE Optimization ---
            let optimized_stmts = Self::optimize_block(stmts.to_vec(), &fn_bodies);

            let root = Stmt::Block(optimized_stmts.clone());
            fctx.num_vars = Self::analyze_numeric(&[], &root);
            //fctx.num_vars = Self::analyze_numeric(&[], &optimized_stmts);
            let mut terminated = false;
            
            for stmt in &optimized_stmts {
                if let Stmt::Let(name, expr) = stmt {
                    // Top-level let, inlined (RHS compiled once): local slot AND donky_global_set.
                    let val = Self::compile_expr(
                        &mut builder, &mut self.module, &self.runtime, &self.user_functions,
                        &mut self.pending_closures, &mut fctx, expr
                    );
                    let var = Variable::new(fctx.next_var); fctx.next_var += 1;
                    builder.declare_var(var, types::I64); builder.def_var(var, val);
                    fctx.variables.insert(name.clone(), var);
                    emit_global_set(&mut builder, &mut self.module, &self.runtime, name, val);
                } else if !matches!(stmt, Stmt::Function(_, _, _)) {
                    terminated = Self::compile_stmt(
                        &mut builder, &mut self.module, &self.runtime, &self.user_functions, 
                        &mut self.pending_closures, &mut fctx, stmt
                    );
                    if terminated { break; }
                }
            }
            
            if !terminated { 
                let null_val = builder.ins().iconst(types::I64, NULL_BITS);
                builder.ins().return_(&[null_val]); 
            }
            builder.seal_all_blocks();
            builder.finalize();
        }
        self.module.define_function(main_id, &mut self.ctx).unwrap();
        self.module.clear_context(&mut self.ctx);

        // --- COMPILE PENDING CLOSURES ---
        // Done AFTER donky_toplevel is defined, so the queue is fully populated.
        let mut i = 0;
        while i < self.pending_closures.len() {
            let (name, params, body) = self.pending_closures[i].clone();
            let mut sig = self.module.make_signature();
            for _ in 0..params.len() { sig.params.push(AbiParam::new(types::I64)); }
            sig.returns.push(AbiParam::new(types::I64));
            let func_id = self.module.declare_function(&format!("donky_fn_{}", name), Linkage::Export, &sig).unwrap();
            self.user_functions.insert(name.clone(), func_id);
            
            // We use a temporary vec here too, so we don't double-borrow
            let mut pc = std::mem::take(&mut self.pending_closures);
            self.compile_function(func_id, &params, &body, &mut pc);
            self.pending_closures = pc;
            
            i += 1;
        }

        let product = self.module.finish();
        let bytes = product.emit().unwrap();
        std::fs::write(output_path, bytes).map_err(|e| e.to_string())?;
        Ok(())
    }

    fn optimize_block(stmts: Vec<Stmt>, fn_bodies: &[Stmt]) -> Vec<Stmt> {
        use std::collections::HashSet;

        fn ex(e: &Expr, u: &mut HashSet<String>) {
            match e {
                Expr::Ident(n) => { u.insert(n.clone()); }
                Expr::Binary(l, _, r) => { ex(l, u); ex(r, u); }
                Expr::Unary(_, x) => ex(x, u),
                Expr::Call(n, args, _) => { u.insert(n.clone()); for a in args { ex(a, u); } }
                Expr::MethodCall(obj, m, args, _) => { u.insert(m.clone()); ex(obj, u); for a in args { ex(a, u); } }
                Expr::Index(o, i) => { ex(o, u); ex(i, u); }
                Expr::FieldAccess(o, _) => ex(o, u),
                Expr::Array(v) => { for a in v { ex(a, u); } }
                Expr::Map(p) => { for (k, v) in p { ex(k, u); ex(v, u); } }
                Expr::InterpolatedString(p) => { for a in p { ex(a, u); } }
                Expr::StructInit(_, f) => { for (_, v) in f { ex(v, u); } }
                Expr::Ternary(c, t, f) => { ex(c, u); ex(t, u); ex(f, u); }
                Expr::Function(_, body) => st(body, u),
                _ => {}
            }
        }

        fn st(s: &Stmt, u: &mut HashSet<String>) {
            match s {
                Stmt::Let(_, e) => ex(e, u),
                Stmt::Assign(n, e) => { u.insert(n.clone()); ex(e, u); }
                Stmt::IndexSet(t, i, v) => { ex(t, u); ex(i, u); ex(v, u); }
                Stmt::FieldSet(o, _, v) => { ex(o, u); ex(v, u); }
                Stmt::Print(e) | Stmt::ExprStmt(e) | Stmt::Throw(e) => ex(e, u),
                Stmt::Return(Some(e)) => ex(e, u),
                Stmt::If(c, t, e) => { ex(c, u); st(t, u); if let Some(e) = e { st(e, u); } }
                Stmt::While(c, b) => { ex(c, u); st(b, u); }
                Stmt::For(_, it, b) => { ex(it, u); st(b, u); }
                Stmt::ForRange(_, a, b, body) => { ex(a, u); ex(b, u); st(body, u); }
                Stmt::Try(t, _, c) => { st(t, u); st(c, u); }
                Stmt::Match(t, cases, eb) => {
                    ex(t, u);
                    for (c, b) in cases { ex(c, u); st(b, u); }
                    if let Some(b) = eb { st(b, u); }
                }
                Stmt::Block(v) => { for x in v { st(x, u); } }
                _ => {}
            }
        }

        // Only literals/identifiers are safe to drop; calls may throw or have side effects.
        fn pure(e: &Expr) -> bool {
            matches!(e, Expr::Number(_) | Expr::Bool(_) | Expr::String(_) | Expr::Ident(_))
        }

        let mut used: HashSet<String> = HashSet::new();
        for s in &stmts { st(s, &mut used); }
        // A top-level let is now a global: any function body that mentions its name keeps it alive.
        for b in fn_bodies { st(b, &mut used); }

        stmts.into_iter().filter(|s| match s {
            Stmt::Let(name, e) => used.contains(name) || !pure(e),
            _ => true,
        }).collect()
    }

    fn compile_function(&mut self, func_id: FuncId, params: &[String], body: &Stmt, pending_closures: &mut Vec<(String, Vec<String>, Box<Stmt>)>) {
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
            let mut fctx = FunctionContext { 
                variables: HashMap::new(), 
                num_vars: HashSet::new(), 
                next_var: 0, 
                exception_target: None, 
                loop_stack: Vec::new(),
                top_lets: self.top_lets.clone()
            };
            
            // Analyze numeric types for this function
            fctx.num_vars = Self::analyze_numeric(params, body);
            
            let block_params: Vec<Value> = builder.block_params(entry_block).to_vec();

            // Implicit program `args` goes in FIRST so a parameter named `args` shadows it.
            let get_args_func = self.module.declare_func_in_func(self.runtime.get_args_id, builder.func);
            let args_call = builder.ins().call(get_args_func, &[]);
            let args_val = builder.inst_results(args_call)[0];
            let args_var = Variable::new(fctx.next_var); fctx.next_var += 1;
            builder.declare_var(args_var, types::I64);
            builder.def_var(args_var, args_val);
            fctx.variables.insert("args".to_string(), args_var);

            for (i, param_name) in params.iter().enumerate() {
                let (pname, _) = Self::split_param(param_name);
                let var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(var, types::I64);
                builder.def_var(var, block_params[i]);
                fctx.variables.insert(pname.to_string(), var);
            }
            let mut pc = std::mem::take(pending_closures);
            let terminated = Self::compile_stmt(
                &mut builder, 
                &mut self.module, 
                &self.runtime, 
                &self.user_functions, 
                &mut pc, 
                &mut fctx, 
                body
            );
            *pending_closures = pc;
            if !terminated { 
                let null_val = builder.ins().iconst(types::I64, NULL_BITS);
                builder.ins().return_(&[null_val]); 
            }
            builder.seal_all_blocks();
            builder.finalize();
        }
        self.module.define_function(func_id, &mut self.ctx).unwrap();
        self.module.clear_context(&mut self.ctx);
    }

    fn compile_stmt(
        builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs,
        user_funcs: &HashMap<String, FuncId>, 
        pending_closures: &mut Vec<(String, Vec<String>, Box<Stmt>)>,
        fctx: &mut FunctionContext, stmt: &Stmt,
    ) -> bool {
        match stmt {
            Stmt::Let(name, expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, expr);
                let var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(var, types::I64); builder.def_var(var, val);
                fctx.variables.insert(name.clone(), var); false
            }
            Stmt::Assign(name, expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, expr);
                let var = match fctx.variables.get(name) {
                    Some(v) => v,
                    None => fatal(Pos::default(), &format!("assignment to undeclared variable `{}`", name)),
                };
                builder.def_var(*var, val); false
            }
            Stmt::IndexSet(target, index, val) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, target);
                let i = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, index);
                let v = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, val);
                let local_callee = module.declare_func_in_func(runtime.index_set_id, builder.func);
                builder.ins().call(local_callee, &[t, i, v]);
                emit_exception_check(builder, module, runtime, fctx);
                false
            }
            Stmt::FieldSet(obj, field, val) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, obj);
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
                
                let v = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, val);
                let set_func = module.declare_func_in_func(runtime.index_set_id, builder.func);
                builder.ins().call(set_func, &[t, key, v]);
                emit_exception_check(builder, module, runtime, fctx);
                false
            }
            Stmt::Print(expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, expr);
                let local_callee = module.declare_func_in_func(runtime.print_id, builder.func);
                builder.ins().call(local_callee, &[val]);
                emit_exception_check(builder, module, runtime, fctx);
                false
            }
            Stmt::ExprStmt(expr) => { Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, expr); false }
            Stmt::Return(expr) => {
                let val = if let Some(e) = expr { 
                    Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, e) 
                } else { 
                    builder.ins().iconst(types::I64, NULL_BITS) 
                };
                builder.ins().return_(&[val]); true
            }
            Stmt::Break(pos) => {
                match fctx.loop_stack.last() {
                    Some((break_target, _)) => { builder.ins().jump(*break_target, &[]); }
                    None => fatal(*pos, "`break` used outside of a loop"),
                }
                true
            }
            Stmt::Continue(pos) => {
                match fctx.loop_stack.last() {
                    Some((_, continue_target)) => { builder.ins().jump(*continue_target, &[]); }
                    None => fatal(*pos, "`continue` used outside of a loop"),
                }
                true
            }
            Stmt::Throw(expr) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, expr);
                let throw_func = module.declare_func_in_func(runtime.throw_id, builder.func);
                builder.ins().call(throw_func, &[val]);
                if let Some(target) = &fctx.exception_target {
                    builder.ins().jump(*target, &[]);
                } else {
                    let null_val = builder.ins().iconst(types::I64, NULL_BITS);
                    builder.ins().return_(&[null_val]);
                }
                true
            }
            Stmt::Try(try_b, err_name, catch_b) => {
                let catch_block = builder.create_block();
                let merge_block = builder.create_block();
                let prev_target = fctx.exception_target;
                fctx.exception_target = Some(catch_block);
                let try_term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, try_b);
                fctx.exception_target = prev_target;
                if !try_term {
                    builder.ins().jump(merge_block, &[]);
                }
                // All predecessors of catch_block (throws / exception checks in the try body) now exist.
                builder.seal_block(catch_block);
                builder.switch_to_block(catch_block);
                let get_exc_func = module.declare_func_in_func(runtime.get_exception_id, builder.func);
                let exc_call = builder.ins().call(get_exc_func, &[]);
                let exc_val = builder.inst_results(exc_call)[0];
                let clear_exc_func = module.declare_func_in_func(runtime.clear_exception_id, builder.func);
                builder.ins().call(clear_exc_func, &[]);
                let saved_vars = fctx.variables.clone();
                let err_var = Variable::new(fctx.next_var);
                fctx.next_var += 1;
                builder.declare_var(err_var, types::I64);
                builder.def_var(err_var, exc_val);
                fctx.variables.insert(err_name.clone(), err_var);
                let catch_term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, catch_b);
                fctx.variables = saved_vars;
                if !catch_term {
                    builder.ins().jump(merge_block, &[]);
                }
                if !try_term || !catch_term {
                    builder.seal_block(merge_block);
                    builder.switch_to_block(merge_block);
                    false
                } else {
                    true
                }
            }
            Stmt::If(cond, then_b, else_b) => {
                let then_block = builder.create_block();
                let else_block = builder.create_block();
                let merge_block = builder.create_block();
                let cond_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, cond);
                let known = Self::is_num(cond, &fctx.num_vars);
                let is_true = Self::cond_to_bool(builder, module, runtime, cond_val, known);
                builder.ins().brif(is_true, then_block, &[], else_block, &[]);
                builder.seal_block(then_block);
                builder.seal_block(else_block);
                builder.switch_to_block(then_block);
                let then_term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, then_b);
                if !then_term {
                    builder.ins().jump(merge_block, &[]);
                }
                builder.switch_to_block(else_block);
                let else_term = if let Some(eb) = else_b {
                    Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, eb)
                } else {
                    false
                };
                if !else_term {
                    builder.ins().jump(merge_block, &[]);
                }
                if !then_term || !else_term {
                    builder.seal_block(merge_block);
                    builder.switch_to_block(merge_block);
                    false
                } else {
                    true
                }
            }
            Stmt::While(cond, body) => {
                let header = builder.create_block();
                let body_block = builder.create_block();
                let exit_block = builder.create_block();
                builder.ins().jump(header, &[]);
                builder.switch_to_block(header);
                let cond_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, cond);
                let known = Self::is_num(cond, &fctx.num_vars);
                let is_true = Self::cond_to_bool(builder, module, runtime, cond_val, known);
                builder.ins().brif(is_true, body_block, &[], exit_block, &[]);
                builder.seal_block(body_block);
                fctx.loop_stack.push((exit_block, header));
                builder.switch_to_block(body_block);
                let body_term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, body);
                fctx.loop_stack.pop();
                if !body_term {
                    builder.ins().jump(header, &[]);
                }
                builder.switch_to_block(exit_block);
                // Back-edge (and any `continue`) now emitted: header has all predecessors.
                builder.seal_block(header);
                // Breaks (if any) now emitted: exit has all predecessors.
                builder.seal_block(exit_block);
                false
            }
            Stmt::For(var_name, iter_expr, body) => {
                let saved_vars = fctx.variables.clone();
                let iter_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                let idx_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                let iter_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, iter_expr);
                builder.declare_var(iter_var, types::I64);
                builder.def_var(iter_var, iter_val);
                let zero = builder.ins().iconst(types::I64, 0);
                builder.declare_var(idx_var, types::I64);
                builder.def_var(idx_var, zero);
                let header = builder.create_block();
                let body_block = builder.create_block();
                let incr_block = builder.create_block();
                let exit_block = builder.create_block();
                builder.ins().jump(header, &[]);
                builder.switch_to_block(header);
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
                fctx.loop_stack.push((exit_block, incr_block));
                builder.switch_to_block(body_block);
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let item_call = builder.ins().call(get_func, &[iter_h, idx_h]);
                let item_val = builder.inst_results(item_call)[0];
                let loop_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(loop_var, types::I64);
                builder.def_var(loop_var, item_val);
                fctx.variables.insert(var_name.clone(), loop_var);
                let term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, body);
                fctx.loop_stack.pop();
                if !term {
                    builder.ins().jump(incr_block, &[]);
                }
                // Body end + all `continue`s are emitted: incr_block has all predecessors.
                builder.seal_block(incr_block);
                builder.switch_to_block(incr_block);
                // Read idx fresh from incr_block (so body mutations to idx are honoured),
                // and use the NaN-boxed representation of the number 1.0.
                let cur_idx = builder.use_var(idx_var);
                let cur_f = builder.ins().bitcast(types::F64, MemFlags::new(), cur_idx);
                let one_f = builder.ins().f64const(1.0);
                let sum_f = builder.ins().fadd(cur_f, one_f);
                let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                let call = builder.ins().call(mk, &[sum_f]);
                let next_idx = builder.inst_results(call)[0];
                builder.def_var(idx_var, next_idx);
                builder.ins().jump(header, &[]);
                builder.switch_to_block(exit_block);
                builder.seal_block(header);
                builder.seal_block(exit_block);
                fctx.variables = saved_vars;
                false
            }
            Stmt::ForRange(var_name, start_expr, end_expr, body) => {
                let saved_vars = fctx.variables.clone();
                let idx_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                let end_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                let start_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, start_expr);
                let end_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, end_expr);
                builder.declare_var(idx_var, types::I64); builder.def_var(idx_var, start_val);
                builder.declare_var(end_var, types::I64); builder.def_var(end_var, end_val);
                let header = builder.create_block();
                let body_block = builder.create_block();
                let incr_block = builder.create_block();
                let exit_block = builder.create_block();
                builder.ins().jump(header, &[]);
                builder.switch_to_block(header);
                let cur = builder.use_var(idx_var);
                let endv = builder.use_var(end_var);
                let cur_f = builder.ins().bitcast(types::F64, MemFlags::new(), cur);
                let end_f = builder.ins().bitcast(types::F64, MemFlags::new(), endv);
                let cond = builder.ins().fcmp(FloatCC::LessThan, cur_f, end_f);
                builder.ins().brif(cond, body_block, &[], exit_block, &[]);
                builder.seal_block(body_block);
                fctx.loop_stack.push((exit_block, incr_block));
                builder.switch_to_block(body_block);
                let loop_var = Variable::new(fctx.next_var); fctx.next_var += 1;
                builder.declare_var(loop_var, types::I64);
                let cur_b = builder.use_var(idx_var);
                builder.def_var(loop_var, cur_b);
                fctx.variables.insert(var_name.clone(), loop_var);
                let term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, body);
                fctx.loop_stack.pop();
                if !term { builder.ins().jump(incr_block, &[]); }
                builder.seal_block(incr_block);
                builder.switch_to_block(incr_block);
                let c2 = builder.use_var(idx_var);
                let c2f = builder.ins().bitcast(types::F64, MemFlags::new(), c2);
                let one = builder.ins().f64const(1.0);
                let sum = builder.ins().fadd(c2f, one);
                let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                let call = builder.ins().call(mk, &[sum]);
                let nxt = builder.inst_results(call)[0];
                builder.def_var(idx_var, nxt);
                builder.ins().jump(header, &[]);
                builder.switch_to_block(exit_block);
                builder.seal_block(header);
                builder.seal_block(exit_block);
                fctx.variables = saved_vars;
                false
            }
            Stmt::Match(target, cases, else_block) => {
                if cases.is_empty() {
                    return if let Some(eb) = else_block {
                        Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, eb)
                    } else { false };
                }
                let merge_block = builder.create_block();
                let mut check_blocks = Vec::new();
                for _ in 0..cases.len() { check_blocks.push(builder.create_block()); }
                let else_check_block = if else_block.is_some() { Some(builder.create_block()) } else { None };
                let target_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, target);
                builder.ins().jump(check_blocks[0], &[]);
                builder.seal_block(check_blocks[0]);
                for (i, (case_expr, body)) in cases.iter().enumerate() {
                    builder.switch_to_block(check_blocks[i]);
                    let case_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, case_expr);
                    let eq_func = module.declare_func_in_func(runtime.eq_id, builder.func);
                    let eq_call = builder.ins().call(eq_func, &[target_val, case_val]);
                    let is_eq = builder.inst_results(eq_call)[0];
                    let is_eq_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), is_eq);
                    let zero = builder.ins().f64const(0.0);
                    let cond = builder.ins().fcmp(FloatCC::NotEqual, is_eq_f64, zero);
                    let body_block = builder.create_block();
                    let next_check = if i + 1 < cases.len() { check_blocks[i+1] } else { else_check_block.unwrap_or(merge_block) };
                    builder.ins().brif(cond, body_block, &[], next_check, &[]);
                    builder.seal_block(body_block);
                    if i + 1 < cases.len() { builder.seal_block(check_blocks[i+1]); }
                    builder.switch_to_block(body_block);
                    let term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, body);
                    if !term {
                        builder.ins().jump(merge_block, &[]);
                    }
                }
                if let Some(eb_block) = else_check_block { builder.seal_block(eb_block); }
                if let Some(eb) = else_block {
                    if let Some(eb_block) = else_check_block {
                        builder.switch_to_block(eb_block);
                        let term = Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, eb);
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
                // Names declared inside the block are dropped when it ends (matches the tree-walker).
                let saved_vars = fctx.variables.clone();
                let mut terminated = false;
                for s in stmts {
                    if Self::compile_stmt(builder, module, runtime, user_funcs, pending_closures, fctx, s) {
                        terminated = true;
                        break;
                    }
                }
                fctx.variables = saved_vars;
                terminated
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
        user_funcs: &HashMap<String, FuncId>, 
        pending_closures: &mut Vec<(String, Vec<String>, Box<Stmt>)>,
        fctx: &mut FunctionContext, expr: &Expr,
    ) -> Value {
        match expr {
            Expr::Function(params, body) => {
                let anon_name = format!("anon_fn_{}", STRING_COUNTER.fetch_add(1, Ordering::SeqCst));
                pending_closures.push((anon_name.clone(), params.clone(), body.clone()));
                
                let mut sig = module.make_signature();
                for _ in 0..params.len() { sig.params.push(AbiParam::new(types::I64)); }
                sig.returns.push(AbiParam::new(types::I64));
                
                let func_id = module.declare_function(&format!("donky_fn_{}", anon_name), Linkage::Export, &sig).unwrap();
                let func_ref = module.declare_func_in_func(func_id, builder.func);
                let addr = builder.ins().func_addr(types::I64, func_ref);
                let alloc_fn = module.declare_func_in_func(runtime.alloc_function_id, builder.func);
                let call = builder.ins().call(alloc_fn, &[addr]);
                builder.inst_results(call)[0]
            }
            Expr::Number(n) => builder.ins().iconst(types::I64, n.to_bits() as i64),
            Expr::Bool(b) => {
                let val = TAG_BASE | (TAG_BOOL_VAL << 48) | (if *b { 1 } else { 0 });
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
                    return builder.ins().iconst(types::I64, NULL_BITS);
                }
                // Don't panic on unknown identifiers. Return null so the program
                // can continue and throw a proper runtime error if needed.
                if let Some(var) = fctx.variables.get(name) {
                    builder.use_var(*var)
                } else if let Some(func_id) = user_funcs.get(name) {
                    // Function name used as a value (e.g. passing to map or spawn).
                    // Wrap it in a TAG_FUNCTION object so donky_call_function accepts it.
                    let func_ref = module.declare_func_in_func(*func_id, builder.func);
                    let addr = builder.ins().func_addr(types::I64, func_ref);
                    let alloc_fn = module.declare_func_in_func(runtime.alloc_function_id, builder.func);
                    let call = builder.ins().call(alloc_fn, &[addr]);
                    builder.inst_results(call)[0] 
                } else if fctx.top_lets.contains(name) {
                    // Top-level `let` read from inside a function: go through the global map.
                    emit_global_get(builder, module, runtime, name)
                } else {
                    eprintln!("warning: undefined variable `{}` evaluates to null", name);
                    builder.ins().iconst(types::I64, NULL_BITS) // Return null
                }
            }
            Expr::Unary(op, right) => {
                let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, right);
                match op {
                    TokenType::Minus => {
                        let v_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), val);
                        let neg = builder.ins().fneg(v_f64);
                        let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                        let call = builder.ins().call(mk, &[neg]);
                        builder.inst_results(call)[0]
                    }
                    TokenType::Bang => {
                        let truthy_func = module.declare_func_in_func(runtime.truthy_id, builder.func);
                        let truthy_call = builder.ins().call(truthy_func, &[val]);
                        let truthy_val = builder.inst_results(truthy_call)[0];
                        let truthy_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), truthy_val);
                        let zero_f64 = builder.ins().f64const(0.0);
                        let is_zero = builder.ins().fcmp(FloatCC::Equal, truthy_f64, zero_f64);
                        let t = builder.ins().iconst(types::I64, BOOL_TRUE_BITS);
                        let f = builder.ins().iconst(types::I64, BOOL_FALSE_BITS);
                        builder.ins().select(is_zero, t, f)
                    }
                    _ => val,
                }
            }
            Expr::Call(name, args, _) if name == "format" && !user_funcs.contains_key("format")
                && matches!(args.first(), Some(Expr::String(_))) => {
                let fmt = match &args[0] { Expr::String(s) => s.clone(), _ => unreachable!() };
                let vals: Vec<Expr> = match &args[1..] { [Expr::Array(e)] => e.clone(), rest => rest.to_vec() };
                Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, &Self::desugar_format(&fmt, &vals))
            }
            Expr::MethodCall(obj, method, args, pos) => {
                if method == "format" {
                    if let Expr::String(fmt) = obj.as_ref() {
                        let vals: Vec<Expr> = match args.as_slice() { [Expr::Array(e)] => e.clone(), _ => args.clone() };
                        return Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, &Self::desugar_format(fmt, &vals));
                    }
                }
                let mut full = Vec::with_capacity(args.len() + 1);
                full.push((**obj).clone());
                full.extend(args.iter().cloned());
                Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, &Expr::Call(method.clone(), full, *pos))
            }
            Expr::Ternary(c, t, f) => {
                let cv = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, c);
                let known = Self::is_num(c, &fctx.num_vars);
                let cb = Self::cond_to_bool(builder, module, runtime, cv, known);
                let then_b = builder.create_block();
                let else_b = builder.create_block();
                let merge = builder.create_block();
                builder.append_block_param(merge, types::I64);
                builder.ins().brif(cb, then_b, &[], else_b, &[]);
                builder.seal_block(then_b);
                builder.seal_block(else_b);
                builder.switch_to_block(then_b);
                let tv = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, t);
                builder.ins().jump(merge, &[tv]);
                builder.switch_to_block(else_b);
                let fv = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, f);
                builder.ins().jump(merge, &[fv]);
                builder.seal_block(merge);
                builder.switch_to_block(merge);
                builder.block_params(merge)[0]
            }
            Expr::Call(name, args, pos) => {
                if let Some(func_id) = user_funcs.get(name) {
                    let local_callee = module.declare_func_in_func(*func_id, builder.func);
                    let mut arg_vals = Vec::new();
                    for arg in args { arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, arg)); }
                    Self::pad_args(builder, local_callee, &mut arg_vals);
                    let call = builder.ins().call(local_callee, &arg_vals);
                    let ret_val = builder.inst_results(call)[0];
                    emit_exception_check(builder, module, runtime, fctx);
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
                        "gui_init" => runtime.gui_init_id,
                        "gui_draw_text" => runtime.gui_draw_text_id,
                        "gui_update" => runtime.gui_update_id,
                        "gui_add_button" => runtime.gui_add_button_id,
                        "gui_add_label" => runtime.gui_add_label_id,
                        "gui_update_text" => runtime.gui_update_text_id,
                        "gui_get_clicked" => runtime.gui_get_clicked_id,
                        "gui_clear" => runtime.gui_clear_id,
                        "gui_repaint" => runtime.gui_repaint_id,
                        "gui_draw_rect" => runtime.gui_draw_rect_id,
                        "cpu_usage" => runtime.cpu_usage_id,
                        "gui_webview" => runtime.gui_webview_id,
                        "kill_process" => runtime.kill_process_id,
                        "uptime" => runtime.uptime_id,
                        "cpu_cores" => runtime.cpu_cores_id,
                        "disk_info" => runtime.disk_info_id,
                        "sub" => runtime.sub_id,
                        "mul" => runtime.mul_id,
                        "div" => runtime.div_id,
                        "dll_load" => runtime.dll_load_id,
                        "dll_sym" => runtime.dll_sym_id,
                        "dll_call" => runtime.dll_call_id,
                        "dll_calli" => runtime.dll_calli_id,
                        "ffi_cstr" => runtime.ffi_cstr_id,
                        "json_decode" => runtime.json_decode_id,
                        "json_encode" => runtime.json_encode_id,
                        "json_pretty" => runtime.json_pretty_id,
                        "json_error" => runtime.json_error_id,
                        "now_ms" => runtime.now_ms_id,
                        _ => match STDLIB_BUILTINS.iter().position(|(n, _, _)| *n == name.as_str()) {
                            Some(i) => runtime.stdlib_ids[i],
                            None => {
                                if let Some(var) = fctx.variables.get(name).copied() {
                                    return Self::compile_indirect_call(builder, module, runtime, user_funcs, pending_closures, fctx, var, args);
                                }
                                fatal(*pos, &format!("unknown function `{}`", name))
                            }
                        },
                    };
                    let local_callee = module.declare_func_in_func(builtin_id, builder.func);
                    let mut arg_vals = Vec::new();
                    for arg in args { arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, arg)); }
                    Self::pad_args(builder, local_callee, &mut arg_vals);
                    let call = builder.ins().call(local_callee, &arg_vals);
                    let results = builder.inst_results(call);
                    let ret_val = if results.is_empty() { builder.ins().iconst(types::I64, NULL_BITS) } else { results[0] };

                    // --- Check for exceptions thrown by builtins ---
                    emit_exception_check(builder, module, runtime, fctx);
                    ret_val
                }
            }
            Expr::Array(elements) => {
                let arr_func = module.declare_func_in_func(runtime.alloc_array_id, builder.func);
                let call = builder.ins().call(arr_func, &[]);
                let arr_handle = builder.inst_results(call)[0];
                for el in elements {
                    let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, el);
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
                    let key = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, k);
                    let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, v);
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

                    let val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, field_val);
                    builder.ins().call(set_func, &[map_handle, field_key, val]);
                }
                map_handle
            }
            Expr::Index(target, index) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, target);
                let i = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, index);
                let get_func = module.declare_func_in_func(runtime.index_get_id, builder.func);
                let call = builder.ins().call(get_func, &[t, i]);
                let r = builder.inst_results(call)[0];
                emit_exception_check(builder, module, runtime, fctx);
                r
            }
            Expr::FieldAccess(obj, field) => {
                let t = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, obj);
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
                let r = builder.inst_results(call)[0];
                emit_exception_check(builder, module, runtime, fctx);
                r
            }
            Expr::Binary(left, op, right) if matches!(op, TokenType::And | TokenType::Or) => {
                // Short-circuit: the right operand is only evaluated when it can change the result.
                // (It used to be evaluated unconditionally, so `i < n && a[i] == x` indexed out of range
                //  and `f() || g()` always ran g().)
                let is_and = *op == TokenType::And;
                let lhs = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, left);
                let lknown = Self::is_num(left, &fctx.num_vars);
                let lb = Self::cond_to_bool(builder, module, runtime, lhs, lknown);
                let rhs_block = builder.create_block();
                let short_block = builder.create_block();
                let merge = builder.create_block();
                builder.append_block_param(merge, types::I64);
                if is_and { builder.ins().brif(lb, rhs_block, &[], short_block, &[]); }
                else { builder.ins().brif(lb, short_block, &[], rhs_block, &[]); }
                builder.seal_block(rhs_block);
                builder.seal_block(short_block);

                builder.switch_to_block(short_block);
                let short_val = builder.ins().iconst(types::I64, if is_and { BOOL_FALSE_BITS } else { BOOL_TRUE_BITS });
                builder.ins().jump(merge, &[short_val]);

                builder.switch_to_block(rhs_block);
                let rhs = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, right);
                let rknown = Self::is_num(right, &fctx.num_vars);
                let rb = Self::cond_to_bool(builder, module, runtime, rhs, rknown);
                let rt = builder.ins().iconst(types::I64, BOOL_TRUE_BITS);
                let rf = builder.ins().iconst(types::I64, BOOL_FALSE_BITS);
                let r_val = builder.ins().select(rb, rt, rf);
                builder.ins().jump(merge, &[r_val]);

                builder.seal_block(merge);
                builder.switch_to_block(merge);
                builder.block_params(merge)[0]
            }
            Expr::Binary(left, op, right) => {
                // --- Constant Folding ---
                if let (Expr::Number(l), Expr::Number(r)) = (left.as_ref(), right.as_ref()) {
                    let cmp_fold = match op {
                        TokenType::EqEq => Some(l == r), TokenType::NotEq => Some(l != r),
                        TokenType::Lt => Some(l < r), TokenType::LtEq => Some(l <= r),
                        TokenType::Gt => Some(l > r), TokenType::GtEq => Some(l >= r),
                        _ => None,
                    };
                    if let Some(b) = cmp_fold {
                        return builder.ins().iconst(types::I64, if b { BOOL_TRUE_BITS } else { BOOL_FALSE_BITS });
                    }
                    let result = match op {
                        TokenType::Plus => Some(l + r),
                        TokenType::Minus => Some(l - r),
                        TokenType::Star => Some(l * r),
                        TokenType::Slash => if *r != 0.0 { Some(l / r) } else { None },
                        TokenType::Percent => Some(l % r),
                        _ => None,
                    };
                    if let Some(val) = result.filter(|v| v.is_finite()) {
                        return builder.ins().iconst(types::I64, val.to_bits() as i64);
                    }
                }
                // --- END Constant Folding ---
                
                let lhs = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, left);
                let rhs = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, right);

                let both_num = Self::is_num(left, &fctx.num_vars) && Self::is_num(right, &fctx.num_vars);


                let cmp = match op {
                    TokenType::EqEq => Some(FloatCC::Equal), TokenType::NotEq => Some(FloatCC::NotEqual),
                    TokenType::Lt => Some(FloatCC::LessThan), TokenType::LtEq => Some(FloatCC::LessThanOrEqual),
                    TokenType::Gt => Some(FloatCC::GreaterThan), TokenType::GtEq => Some(FloatCC::GreaterThanOrEqual),
                    _ => None,
                };
                if (op == &TokenType::EqEq || op == &TokenType::NotEq) && !both_num {
                    let eq_func = module.declare_func_in_func(runtime.eq_id, builder.func);
                    let call = builder.ins().call(eq_func, &[lhs, rhs]);
                    let is_eq = builder.inst_results(call)[0];
                    let eq_f = builder.ins().bitcast(types::F64, MemFlags::new(), is_eq);
                    let zero = builder.ins().f64const(0.0);
                    let c = if op == &TokenType::NotEq {
                        builder.ins().fcmp(FloatCC::Equal, eq_f, zero)
                    } else {
                        builder.ins().fcmp(FloatCC::NotEqual, eq_f, zero)
                    };
                    let t = builder.ins().iconst(types::I64, BOOL_TRUE_BITS);
                    let f = builder.ins().iconst(types::I64, BOOL_FALSE_BITS);
                    builder.ins().select(c, t, f)
                } else if let Some(cc) = cmp {
                    let a_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), lhs);
                    let b_f64 = builder.ins().bitcast(types::F64, MemFlags::new(), rhs);
                    let c = builder.ins().fcmp(cc, a_f64, b_f64);
                    let t = builder.ins().iconst(types::I64, BOOL_TRUE_BITS);
                    let f = builder.ins().iconst(types::I64, BOOL_FALSE_BITS);
                    builder.ins().select(c, t, f)
                } else {
                    match op {
                        TokenType::Plus => {
                            if both_num {
                                let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs);
                                let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs);
                                let res = builder.ins().fadd(a, b);
                                let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                                let call = builder.ins().call(mk, &[res]);
                                builder.inst_results(call)[0]
                            } else {
                                let add_func = module.declare_func_in_func(runtime.add_id, builder.func);
                                let call = builder.ins().call(add_func, &[lhs, rhs]);
                                let r = builder.inst_results(call)[0];
                                emit_exception_check(builder, module, runtime, fctx);
                                r
                            }
                        }
                                                TokenType::Minus => {
                            if both_num {
                                let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs);
                                let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs);
                                let res = builder.ins().fsub(a, b);
                                let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                                let call = builder.ins().call(mk, &[res]);
                                builder.inst_results(call)[0]
                            } else {
                                // checked: throws "cannot subtract X and Y" on non-numbers
                                let f = module.declare_func_in_func(runtime.sub_id, builder.func);
                                let call = builder.ins().call(f, &[lhs, rhs]);
                                let r = builder.inst_results(call)[0];
                                emit_exception_check(builder, module, runtime, fctx);
                                r
                            }
                        }
                        TokenType::Star => {
                            if both_num {
                                let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs);
                                let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs);
                                let res = builder.ins().fmul(a, b);
                                let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                                let call = builder.ins().call(mk, &[res]);
                                builder.inst_results(call)[0]
                            } else {
                                // checked: throws "cannot multiply X and Y" on non-numbers
                                let f = module.declare_func_in_func(runtime.mul_id, builder.func);
                                let call = builder.ins().call(f, &[lhs, rhs]);
                                let r = builder.inst_results(call)[0];
                                emit_exception_check(builder, module, runtime, fctx);
                                r
                            }
                        }
                        TokenType::Slash => {
                            if !both_num {
                                // checked: throws "cannot divide X and Y" on non-numbers
                                let chk = module.declare_func_in_func(runtime.fdiv_id, builder.func);
                                builder.ins().call(chk, &[lhs, rhs]);
                                emit_exception_check(builder, module, runtime, fctx);
                            }
                            let a = builder.ins().bitcast(types::F64, MemFlags::new(), lhs);
                            let b = builder.ins().bitcast(types::F64, MemFlags::new(), rhs);
                            let res = builder.ins().fdiv(a, b);
                            let mk = module.declare_func_in_func(runtime.make_number_id, builder.func);
                            let call = builder.ins().call(mk, &[res]);
                            builder.inst_results(call)[0]
                        }
                        TokenType::Div => {
                            // div = checked integer division: calls donky_div, throws on zero and on type mismatch. / = raw IEEE.
                            let div_func = module.declare_func_in_func(runtime.div_id, builder.func);
                            let call = builder.ins().call(div_func, &[lhs, rhs]);
                            let r = builder.inst_results(call)[0];
                            emit_exception_check(builder, module, runtime, fctx);
                            r
                        }
                        TokenType::Percent => {
                            let mod_func = module.declare_func_in_func(runtime.mod_id, builder.func);
                            let call = builder.ins().call(mod_func, &[lhs, rhs]);
                            let r = builder.inst_results(call)[0];
                            emit_exception_check(builder, module, runtime, fctx);
                            r
                        }
                        // dead: short-circuit arm matches first
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
                    return Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, &Expr::String("".into()));
                }
                let mut acc = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, &parts[0]);
                let str_func = module.declare_func_in_func(runtime.str_id, builder.func);
                let add_func = module.declare_func_in_func(runtime.add_id, builder.func);
                
                // Convert initial part to string
                let acc_str_call = builder.ins().call(str_func, &[acc]);
                acc = builder.inst_results(acc_str_call)[0];

                for part in &parts[1..] {
                    let p_val = Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, part);
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
    /// `f(a, b)` where `f` is a local/parameter holding a function value (a raw code address).
    /// Builtins and top-level functions take priority, so a variable named `len` still calls `len`.
    fn compile_indirect_call(
        builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs,
        user_funcs: &HashMap<String, FuncId>,
        pending_closures: &mut Vec<(String, Vec<String>, Box<Stmt>)>,
        fctx: &mut FunctionContext, var: Variable, args: &[Expr],
    ) -> Value {
        let mut arg_vals = Vec::new();
        for arg in args { arg_vals.push(Self::compile_expr(builder, module, runtime, user_funcs, pending_closures, fctx, arg)); }
        let fobj = builder.use_var(var);
        // Pack the arguments into a Donky array: donky_call_function(fn_obj, args_array)
        let arr_fn = module.declare_func_in_func(runtime.alloc_array_id, builder.func);
        let arr_call = builder.ins().call(arr_fn, &[]);
        let args_arr = builder.inst_results(arr_call)[0];
        let push_fn = module.declare_func_in_func(runtime.array_push_id, builder.func);
        for v in &arg_vals { builder.ins().call(push_fn, &[args_arr, *v]); }
        let call_fn = module.declare_func_in_func(runtime.call_function_id, builder.func);
        let call = builder.ins().call(call_fn, &[fobj, args_arr]);
        let ret_val = builder.inst_results(call)[0];

        emit_exception_check(builder, module, runtime, fctx);
        ret_val
    }

    fn split_param(p: &str) -> (&str, Option<&str>) {
        match p.split_once(':') { Some((n, t)) => (n, Some(t)), None => (p, None) }
    }
    fn desugar_format(fmt: &str, vals: &[Expr]) -> Expr {
        let segs: Vec<&str> = fmt.split("{}").collect();
        let mut parts = Vec::new();
        for (k, seg) in segs.iter().enumerate() {
            if !seg.is_empty() { parts.push(Expr::String(seg.to_string())); }
            if k + 1 < segs.len() {
                parts.push(vals.get(k).cloned().unwrap_or_else(|| Expr::String("{}".into())));
            }
        }
        Expr::InterpolatedString(parts)
    }
    fn pad_args(builder: &mut FunctionBuilder, callee: codegen::ir::FuncRef, args: &mut Vec<Value>) {
        let sig = builder.func.dfg.ext_funcs[callee].signature;
        let n = builder.func.dfg.signatures[sig].params.len();
        while args.len() < n {
            let z = builder.ins().iconst(types::I64, 0);
            args.push(z);
        }
        args.truncate(n);
    }

    fn is_num(e: &Expr, num: &HashSet<String>) -> bool {
        match e {
            Expr::Number(_) => true,
            Expr::Ident(n) => num.contains(n),
            Expr::Unary(TokenType::Minus, x) => Self::is_num(x, num),
            // `!x`, comparisons, && and || produce TAG_BOOL after Fix 2: not numeric.
            Expr::Binary(l, op, r) => match op {
                TokenType::Plus => Self::is_num(l, num) && Self::is_num(r, num),
                TokenType::Minus | TokenType::Star | TokenType::Slash | TokenType::Percent
                | TokenType::Div => true,
                _ => false,
            },
            Expr::Ternary(_, t, f) => Self::is_num(t, num) && Self::is_num(f, num),
            _ => false,
        }
    }

    fn collect_defs<'a>(s: &'a Stmt, defs: &mut Vec<(String, Option<&'a Expr>)>) {
        match s {
            Stmt::Let(n, e) | Stmt::Assign(n, e) => defs.push((n.clone(), Some(e))),
            Stmt::If(_, t, e) => { Self::collect_defs(t, defs); if let Some(e) = e { Self::collect_defs(e, defs); } }
            Stmt::While(_, b) => Self::collect_defs(b, defs),
            Stmt::For(v, _, b) => { defs.push((v.clone(), None)); Self::collect_defs(b, defs); }
            Stmt::ForRange(v, s, _, b) => { defs.push((v.clone(), Some(s))); Self::collect_defs(b, defs); }
            Stmt::Try(t, n, c) => { Self::collect_defs(t, defs); defs.push((n.clone(), None)); Self::collect_defs(c, defs); }
            Stmt::Match(_, cases, eb) => {
                for (_, b) in cases { Self::collect_defs(b, defs); }
                if let Some(b) = eb { Self::collect_defs(b, defs); }
            }
            Stmt::Block(v) => { for x in v { Self::collect_defs(x, defs); } }
            _ => {}
        }
    }

    /// A variable is numeric only if EVERY definition of it in the function is numeric.
    fn analyze_numeric(params: &[String], body: &Stmt) -> HashSet<String> {
        let mut defs = Vec::new();
        Self::collect_defs(body, &mut defs);
        let mut num: HashSet<String> = HashSet::new();
        let mut untyped: HashSet<String> = HashSet::new();
        for p in params {
            match Self::split_param(p) {
                (n, Some("int")) | (n, Some("float")) | (n, Some("num")) => { num.insert(n.to_string()); }
                (n, _) => { untyped.insert(n.to_string()); }
            }
        }
        for (n, _) in &defs { if !untyped.contains(n) { num.insert(n.clone()); } }
        loop {
            let mut changed = false;
            for (n, e) in &defs {
                if !num.contains(n) { continue; }
                let ok = match e { Some(e) => Self::is_num(e, &num), None => false };
                if !ok { num.remove(n); changed = true; }
            }
            if !changed { break; }
        }
        num
    }

    fn cond_to_bool(builder: &mut FunctionBuilder, module: &mut ObjectModule, runtime: &RuntimeFuncs, val: Value, known_num: bool) -> Value {
        let v = if known_num { val } else {
            let f = module.declare_func_in_func(runtime.truthy_id, builder.func);
            let c = builder.ins().call(f, &[val]);
            builder.inst_results(c)[0]
        };
        let vf = builder.ins().bitcast(types::F64, MemFlags::new(), v);
        let zero = builder.ins().f64const(0.0);
        builder.ins().fcmp(FloatCC::NotEqual, vf, zero)
    }
}

fn find_module(base_dir: &str, import: &str) -> Option<std::path::PathBuf> {
    use std::path::{Path, PathBuf};
    let mut names = vec![import.to_string()];
    if !import.ends_with(".donk") { names.push(format!("{}.donk", import)); }

    let mut roots: Vec<PathBuf> = vec![
        Path::new(base_dir).to_path_buf(),
        PathBuf::from("donky_modules"),
        PathBuf::from("lib"),
    ];
    if let Ok(exe) = std::env::current_exe() {
        if let Some(root) = exe.parent().and_then(|p| p.parent()) { roots.push(root.join("lib")); }
    }
    for root in &roots {
        for n in &names {
            let p = root.join(n);
            if p.is_file() { return Some(p); }
        }
    }
    let stem = import.trim_end_matches(".donk");
    let pkg = Path::new("donky_modules").join(stem);
    for f in ["main.donk", "mod.donk", &format!("{}.donk", stem)] {
        let p = pkg.join(f);
        if p.is_file() { return Some(p); }
    }
    None
}

fn resolve_imports(
    stmts: &[Stmt], base_dir: &str,
    all_funcs: &mut HashMap<String, (Vec<String>, Box<Stmt>)>,
    visited: &mut HashSet<std::path::PathBuf>,
) -> Result<(), String> {
    for stmt in stmts {
        match stmt {
            Stmt::Import(path) => {
                let full = find_module(base_dir, path).ok_or_else(|| format!(
                    "Import error: cannot find module \"{}\" (searched {}, donky_modules/, lib/)", path, base_dir))?;
                let canon = std::fs::canonicalize(&full).unwrap_or_else(|_| full.clone());
                if !visited.insert(canon) { continue; }
                let source = std::fs::read_to_string(&full)
                    .map_err(|e| format!("Import error: {}: {}", full.display(), e))?;
                let toks = crate::lexer::Lexer::new(&source).tokenize()
                    .map_err(|e| format!("Lex error in {}: {}", full.display(), e))?;
                let imported = crate::parser::Parser::new(toks).parse()
                    .map_err(|e| format!("Parse error in {}: {}", full.display(), e))?;
                let new_base = full.parent().map(|p| p.to_string_lossy().to_string()).unwrap_or_default();
                resolve_imports(&imported, &new_base, all_funcs, visited)?;
            }
            Stmt::Function(name, params, body) => {
                all_funcs.insert(name.clone(), (params.clone(), body.clone()));
            }
            _ => {}
        }
    }
    Ok(())
}