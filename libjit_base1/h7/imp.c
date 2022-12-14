/*-*- Mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*-*/
#include <assert.h>
#include <ctype.h>
#include <jit/jit.h>
#include <jit/jit-dump.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "object.h"


static int debug = 0;

static const imp_object END_OF_FRAME = NULL;

// forward declaration
jit_value_t compile(imp_object env, jit_function_t function, imp_object form, imp_object *enclosed);


static jit_type_t fn_signature(int nparams) {
    jit_type_t *params = malloc(sizeof(jit_type_t) * nparams);
    for (int i = 0; i < nparams; i++) {
        params[i] = jit_type_void_ptr;
    }
    return jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, nparams, 1);
}

static jit_value_t emit_malloc(jit_function_t function, int size) {
    jit_type_t *params = malloc(sizeof(jit_type_t));
    params[0] = jit_type_ulong;
    jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, params, 1, 1);
    jit_value_t *args = malloc(sizeof(jit_value_t));
    args[0] = jit_value_create_nint_constant(function, jit_type_nint, size);
    return jit_insn_call_native (function, "malloc", (void *)malloc, signature, args, 1, 0);
}

/**
 * Extends the lexical environment for a new fn by adding the end of
 * frame marker and a list of parameters bound to jit param values.
 *
 * Leaves jit param 0 untouched for the closure.
 */
static imp_object extend_env_with_params(jit_function_t fn, imp_object env,
                                         imp_object params) {
    imp_object newenv = imp_cons(END_OF_FRAME, env);
    int i = 1;
    for (imp_object it = params; it != NULL; it = imp_rest(it)) {
        jit_value_t jit_param = jit_value_get_param (fn, i++);
        newenv = imp_assoc(newenv, imp_first(it), imp_pointer(jit_param));
    }
    return newenv;
}

static void die(char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}

/**
 * JIT compiles a function.
 */
static jit_function_t compile_fn(jit_context_t jitctx, imp_object params,
                                 imp_object body, imp_object env,
                                 imp_object *enclosed) {
    int nparams = imp_count(params) + 1;
    jit_function_t jitfn = jit_function_create(jitctx, fn_signature(nparams));
    imp_object newenv = extend_env_with_params(jitfn, env, params);
    jit_value_t result = compile(newenv, jitfn, body, enclosed);
    jit_insn_return(jitfn, result);
    if (!jit_function_compile(jitfn))
        die("JIT compilation failed");
    if (debug)
        jit_dump_function(stdout, jitfn, NULL);
    return jitfn;
}


/**
 * Emits code that constructs the Fn closure object for a function.
 *
 * An Fn object has this structure:
 *
 *     +---------------------+
 *     | object type tag FN  | int
 *     +---------------------+ 
 *     | code entrypoint ptr | pointer
 *     +---------------------+
 *     | arity               | int
 *     +---------------------+
 *     | closed over value 1 | pointer
 *     +---------------------+
 *                :
 *     +---------------------+
 *     | closed over value N | pointer
 *     +---------------------+
 *
 */
static jit_value_t emit_closure(jit_function_t fn, imp_object env,
                                jit_function_t newfn, int arity,
                                imp_object newenclosed, imp_object *enclosed) {
    // allocate space for the object
    int enclosed_count = imp_count(newenclosed);
    int size = offsetof(imp_object_struct, fields.fn.closure) + 
        sizeof(void*) * enclosed_count;
    jit_value_t obj = emit_malloc(fn, size);
    
    // fill in object type
    jit_value_t tag = jit_value_create_nint_constant(fn, jit_type_int, FN);
    jit_insn_store_relative(fn, obj, offsetof(imp_object_struct, type), tag);

    // fill in function entrypoint pointer
    jit_nint entrypoint = (jit_nint)jit_function_to_closure(newfn);
    jit_value_t ptr = jit_value_create_nint_constant(fn, jit_type_nint, entrypoint);
    jit_insn_store_relative(fn, obj, offsetof(imp_object_struct,
                                                fields.fn.entrypoint), ptr);
    // fill in arity
    jit_value_t arityc = jit_value_create_nint_constant(fn, jit_type_int, arity);
    jit_insn_store_relative(fn, obj, offsetof(imp_object_struct,
                                              fields.fn.arity), arityc);
    
    // fill in closed over values
    // XXX - could represent enclosed as a vector so that we don't have to do
    //       this in reverse
    int offset = size - sizeof(void*);
    for (imp_object entry = newenclosed; entry != NULL; entry = imp_rest(entry)) {
        imp_object symbol = imp_first(imp_first(entry));
        jit_value_t value = compile(env, fn, symbol, enclosed);
        jit_insn_store_relative (fn, obj, offset, value);
        offset -= sizeof(void*);
    }

    return obj;
}

static jit_value_t emit_fixnum2int(jit_function_t fn, jit_value_t fixnum) {
   jit_value_t one = jit_value_create_nint_constant(fn, jit_type_nint, 1);
   return jit_insn_shr(fn, fixnum, one);
}

static jit_value_t emit_int2fixnum(jit_function_t fn, jit_value_t fixnum) {
   jit_value_t one = jit_value_create_nint_constant(fn, jit_type_nint, 1);
   jit_value_t shifted = jit_insn_shl(fn, fixnum, one);
   return jit_insn_or(fn, shifted, one);
}

static jit_value_t emit_binop(jit_function_t fn, imp_object env,
                              imp_object form, imp_object *enclosed) {
    char *opname = imp_symbol_cstr(imp_first(form));
    int op = opname[0];
    // XXX type checking
    jit_value_t x = compile(env, fn, imp_second(form), enclosed);
    jit_value_t y = compile(env, fn, imp_third(form), enclosed);
    jit_value_t one = jit_value_create_nint_constant(fn, jit_type_nint, 1);
    // addition can be done without conversion
    if (op == '+')
        return jit_insn_sub(fn, jit_insn_add(fn, x, y), one);
    if (op == '-')
        return jit_insn_add(fn, jit_insn_sub(fn, x, y), one);

    // everything else needs conversion first
    x = emit_fixnum2int(fn, x);
    y = emit_fixnum2int(fn, y);
    jit_value_t result;
    switch (op) {
    case '/': result = jit_insn_div(fn, x, y); break;
    case '*': result = jit_insn_mul(fn, x, y); break;
    default: die("unhandled binop"); break;
    }
    return emit_int2fixnum(fn, result);
}

static jit_value_t emit_if(jit_function_t fn, imp_object env,
                           imp_object form, imp_object *enclosed) {
    jit_label_t falselabel = jit_label_undefined;
    jit_label_t endiflabel = jit_label_undefined;
    jit_value_t false = jit_value_create_nint_constant(fn, 
                                                       jit_type_nint, (jit_nint) FALSE);
    jit_value_t condition = compile(env, fn, imp_nth(form, 1), enclosed);
    jit_value_t eq = jit_insn_eq(fn, condition, false);
    jit_insn_branch_if(fn, eq, &falselabel);

    // true clause
    jit_value_t trueclause = compile(env, fn, imp_nth(form, 2), enclosed);
    jit_value_t result = jit_value_create(fn, jit_type_void_ptr);
    jit_insn_store(fn, result, trueclause);
    jit_insn_branch(fn, &endiflabel);

    // false clause
    jit_insn_label(fn, &falselabel);
    jit_value_t falseclause = compile(env, fn, imp_nth(form, 3), enclosed);
    jit_insn_store(fn, result, falseclause);
    jit_insn_label(fn, &endiflabel);
    return result;
}

static jit_value_t emit_let(jit_function_t fn, imp_object env,
                            imp_object form, imp_object *enclosed) {
    imp_object bindings = imp_second(form);
    imp_object body = imp_third(form);
    imp_object bindname = imp_first(bindings);
    imp_object bindvalue = imp_second(bindings);
    jit_value_t jitvalue = compile(env, fn, bindvalue, enclosed);
    env = imp_assoc(env, bindname, imp_pointer(jitvalue));
    return compile(env, fn, body, enclosed);
}

static jit_value_t emit_fn(jit_function_t fn, imp_object env,
                           imp_object form, imp_object *enclosed) {
    imp_object params = imp_second(form);
    imp_object body = imp_third(form);
    imp_object newenclosed = EMPTY_LIST;
    jit_function_t newfn = compile_fn(jit_function_get_context(fn),
                                      params, body, env, &newenclosed);
    return emit_closure(fn, env, newfn, imp_count(params),
                        newenclosed, enclosed);
}

static jit_value_t emit_application(jit_function_t fn, imp_object env,
                           imp_object form, imp_object *enclosed) {
    jit_value_t fcompiled = compile(env, fn, imp_first(form), enclosed);
    int nargs = imp_count(imp_rest(form));
    jit_value_t *args = malloc(sizeof(jit_value_t) * (nargs + 1));
    args[0] = fcompiled;
    int i = 1;
    for (imp_object it = imp_rest(form); it != NULL; it = imp_rest(it)) {
        imp_object arg = imp_first(it);
        args[i++] = compile(env, fn, arg, enclosed);
    }
    jit_value_t ptr = jit_insn_load_relative (fn, fcompiled,
                                              offsetof(imp_object_struct,
                                                       fields.fn.entrypoint), jit_type_void_ptr);
    return jit_insn_call_indirect(fn, ptr, fn_signature(nargs + 1), args, nargs + 1, 0);
}

static jit_value_t emit_resolve(jit_function_t fn, imp_object env,
                           imp_object form, imp_object *enclosed) {
        // lookup in local environment
        imp_object entry = env;
        for (; entry != NULL; entry = imp_rest(entry)) {
            imp_object pair = imp_first(entry);
            if (pair == NULL) { /* end of frame */
                break;
            }
            imp_object key = imp_first(pair);
            imp_object value = imp_second(pair);
            if (imp_equals(key, form) && imp_type_of(value) == POINTER) {
                return value->fields.pointer;
            }
        }

        // lookup in parent environments
        for (; entry != NULL; entry = imp_rest(entry)) {
            imp_object pair = imp_first(entry);
            if (pair == NULL) continue;
            imp_object key = imp_first(pair);
            if (imp_equals(key, form)) {
                // add it to teh closure
                int idx = *enclosed == NULL ? -1 : (int)imp_second(imp_first(*enclosed));
                idx++;
                *enclosed = imp_assoc(*enclosed, key, (void*) (long) idx);
                jit_value_t closure_arg = jit_value_get_param (fn, 0);
                int offset = offsetof(imp_object_struct, fields.fn.closure[idx]);
                return jit_insn_load_relative (fn, closure_arg, offset, jit_type_void_ptr);
            }
        }
        fprintf(stderr, "unbound: %s\n", form->fields.symbol.name);
        exit(1);
}

static jit_value_t emit_literal(jit_function_t fn, imp_object env,
                           imp_object form, imp_object *enclosed) {
    return jit_value_create_nint_constant (fn, jit_type_nint, (jit_nint)form);
}

jit_value_t compile(imp_object env, jit_function_t fn, imp_object form, imp_object *enclosed) {
    if (imp_type_of(form) == CONS) {
        imp_object f = imp_first(form);
        if (imp_type_of(f) == SYMBOL) {
            char *fname = f->fields.symbol.name;
            if (!strcmp(fname, "+")) {
                return emit_binop(fn, env, form, enclosed);
            } else if (!strcmp(fname, "-")) {
                return emit_binop(fn, env, form, enclosed);
            } else if (!strcmp(fname, "*")) {
                return emit_binop(fn, env, form, enclosed);
            } else if (!strcmp(fname, "/")) {
                return emit_binop(fn, env, form, enclosed);
            } else if (!strcmp(fname, "if")) { // (if cond true false)
                return emit_if(fn, env, form, enclosed);
            } else if (!strcmp(fname, "let")) { // (let (x 2) ...)
                return emit_let(fn, env, form, enclosed);
            } else if (!strcmp(fname, "fn")) { // (fn (x 2) ...)
                return emit_fn(fn, env, form, enclosed);
            }
        }
        return emit_application(fn, env, form, enclosed);
    } else if (imp_type_of(form) == SYMBOL) {
        return emit_resolve(fn, env, form, enclosed);
    } else {
        return emit_literal(fn, env, form, enclosed);
    }
}

imp_object eval(jit_context_t context, imp_object form) {
    jit_context_build_start(context);
    jit_type_t signature = jit_type_create_signature(jit_abi_cdecl, jit_type_void_ptr, NULL, 0, 1);
    jit_function_t function = jit_function_create(context, signature);
    imp_object enclosed = NULL;
    jit_value_t result = compile(NULL, function, form, &enclosed);  
    jit_insn_return(function, result);
    jit_context_build_end(context);
    if (!jit_function_compile(function)) {
        fprintf(stderr, "JIT compilation error\n");
        return NULL;
    }
    if (debug)
        jit_dump_function(stdout, function, NULL);
    imp_object result2;
    jit_function_apply(function, NULL, &result2);
    return result2;
}

int main0 (int argc, char *argv[]) {

    if (argc > 1) {
        debug = 1;
    }

    jit_context_t context = jit_context_create();
    //imp_print(imp_read());
    imp_print(eval(context, imp_read()));
    //imp_print(imp_cons(imp_symbol("a"), imp_cons(imp_symbol("b"), NULL)));
    printf("\n");

    jit_context_destroy(context);
    return 0;
}
