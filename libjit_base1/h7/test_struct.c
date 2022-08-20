
#include <stdio.h>
#include <stdlib.h>
#include <jit/jit.h>
#include <stddef.h>

typedef struct Test{
    int age;
    const char* name;
}Test;

void prints(Test* _t){
    printf("age = %d, name = %s\n", _t->age, _t->name);
}

void prints0(){
    static int id = 0;
    printf("prints0 >> %d\n", id ++);
}

static jit_value_t emit_malloc(jit_function_t function, int size) {
    jit_type_t params[1];
    params[0] = jit_type_ulong;
    jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 1, 1);
    jit_value_t args[1];
    args[0] = jit_value_create_nint_constant(function, jit_type_nint, size);
    return jit_insn_call_native (function, "malloc", (void *)malloc, signature, args, 1, 0);
}

//https://blog.csdn.net/wxdao/article/details/17641189
int main(int argc, char** argv) {
  jit_init();

  jit_context_t context = jit_context_create();
 // jit_debugger_t dbg = jit_debugger_create(context);
  //jit_debugger_t dbg = jit_debugger_from_context(context);

  // void foo()
  jit_function_t F = jit_function_create(context,
      jit_type_create_signature(jit_abi_cdecl, jit_type_void, NULL, 0, 1));

  jit_type_t struct_types[2];
  struct_types[0] = jit_type_int;
  struct_types[1] = jit_type_create_pointer(jit_type_sys_char, 1);
  jit_type_t ty_struct = jit_type_create_struct(struct_types, 2, 1);
  char *ts_names[] = { "age", "name"};
  jit_type_set_names(ty_struct, ts_names, 2);

  //jit_value_t val_struct = emit_malloc(F, sizeof (struct Test));
  jit_value_t val_struct = jit_value_create(F, ty_struct); //not work?
  jit_value_set_addressable(val_struct);
  //set age and name
  //TODO can't create const int ?
  jit_value_t age = jit_value_create_nint_constant(F, jit_type_nint, 888);
  jit_insn_store_relative(F,
                          jit_insn_address_of(F, val_struct),
                          jit_type_get_offset(ty_struct, 0),
                          age);

//  jit_insn_store_relative(F,
//                          val_struct,
//                          offsetof(Test, age),
//                          age);
  jit_type_t type_cstring = jit_type_create_pointer(jit_type_sys_char, 1);
  jit_value_t name = jit_value_create_long_constant(
      F, type_cstring, (long)"heaven7");
//  jit_insn_store_relative(F,
//                          val_struct,
//                          offsetof(Test, name),
//                          name);
  jit_insn_store_relative(F,
                          jit_insn_address_of(F, val_struct),
                          jit_type_get_offset(ty_struct, 1),
                          name);

  // jit_insn_store_elem 应该是为数组用的
  //jit_insn_store_elem(F, val_struct, jit_value_create_nint_constant(F, jit_type_nint, 1), name);
  //start call
  //jit_type_t _m_type = jit_type_create_pointer(jit_type_void, 1);
  jit_type_t ptr_st = jit_type_create_pointer(ty_struct, 1);
  jit_type_t sig_prints = jit_type_create_signature(
      jit_abi_cdecl, jit_type_void, &ptr_st, 1, 1);

  jit_type_t sig_prints0 = jit_type_create_signature(
      jit_abi_cdecl, jit_type_void, NULL, 0, 1);
  //JIT_CALL_NORETURN 代表的是状态栈的返回，如果用了这个flag, 则不能连续调用 jit_insn_call_xxx
  jit_insn_call_native(
      F, "prints0", (void*)prints0, sig_prints0, NULL, 0, JIT_CALL_NOTHROW);

  jit_value_t ptr_struct = jit_insn_address_of(F, val_struct);
  jit_insn_call_native(
      F, "prints", (void*)prints, sig_prints, &ptr_struct, 1, JIT_CALL_NOTHROW);

  //dump
  jit_dump_function(stdout, F, "F [uncompiled]");
  jit_function_compile(F);
  jit_dump_function(stdout, F, "F [compiled]");

  jit_function_apply(F, NULL, NULL);
  jit_context_destroy(context);
}
