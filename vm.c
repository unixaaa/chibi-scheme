/*  vm.c -- stack-based virtual machine backend               */
/*  Copyright (c) 2009-2010 Alex Shinn.  All rights reserved. */
/*  BSD-style license: http://synthcode.com/license.txt       */

#if SEXP_USE_DEBUG_VM > 1
static void sexp_print_stack (sexp ctx, sexp *stack, int top, int fp, sexp out) {
  int i;
  if (! sexp_oportp(out)) out = sexp_current_error_port(ctx);
  for (i=0; i<top; i++) {
    sexp_write_char(ctx, ((i==fp) ? '*' : ' '), out);
    if (i < 10) sexp_write_char(ctx, '0', out);
    sexp_write(ctx, sexp_make_fixnum(i), out);
    sexp_write_string(ctx, ": ", out);
    sexp_write(ctx, stack[i], out);
    sexp_newline(ctx, out);
  }
}
#else
#define sexp_print_stack(ctx, stacl, top, fp, out)
#endif

void sexp_stack_trace (sexp ctx, sexp out) {
  int i, fp=sexp_context_last_fp(ctx);
  sexp self, bc, ls, *stack=sexp_stack_data(sexp_context_stack(ctx));
  if (! sexp_oportp(out)) out = sexp_current_error_port(ctx);
  for (i=fp; i>4; i=sexp_unbox_fixnum(stack[i+3])) {
    self = stack[i+2];
    if (sexp_procedurep(self)) {
      sexp_write_string(ctx, "  called from ", out);
      bc = sexp_procedure_code(self);
      if (sexp_truep(sexp_bytecode_name(bc)))
        sexp_write(ctx, sexp_bytecode_name(bc), out);
      else
        sexp_write_string(ctx, "<anonymous>", out);
      if ((ls=sexp_bytecode_source(bc)) && sexp_pairp(ls)) {
        if (sexp_fixnump(sexp_cdr(ls)) && (sexp_cdr(ls) >= SEXP_ZERO)) {
          sexp_write_string(ctx, " on line ", out);
          sexp_write(ctx, sexp_cdr(ls), out);
        }
        if (sexp_stringp(sexp_car(ls))) {
          sexp_write_string(ctx, " of file ", out);
          sexp_write_string(ctx, sexp_string_data(sexp_car(ls)), out);
        }
      }
      sexp_write_char(ctx, '\n', out);
    }
  }
}

/************************* code generation ****************************/

static void emit_word (sexp ctx, sexp_uint_t val)  {
  unsigned char *data;
  expand_bcode(ctx, sizeof(sexp));
  data = sexp_bytecode_data(sexp_context_bc(ctx));
  sexp_context_align_pos(ctx);
  *((sexp_uint_t*)(&(data[sexp_context_pos(ctx)]))) = val;
  sexp_context_pos(ctx) += sizeof(sexp);
}

static void emit_push (sexp ctx, sexp obj) {
  emit(ctx, SEXP_OP_PUSH);
  emit_word(ctx, (sexp_uint_t)obj);
  if (sexp_pointerp(obj) && ! sexp_symbolp(obj))
    sexp_push(ctx, sexp_bytecode_literals(sexp_context_bc(ctx)), obj);
}

static void emit_enter (sexp ctx) {return;}
static void bless_bytecode (sexp ctx, sexp bc) {return;}

static void emit_return (sexp ctx) {
  emit(ctx, SEXP_OP_RET);
}

static sexp_sint_t sexp_context_make_label (sexp ctx) {
  sexp_sint_t label;
  sexp_context_align_pos(ctx);
  label = sexp_context_pos(ctx);
  sexp_context_pos(ctx) += sizeof(sexp_uint_t);
  return label;
}

static void sexp_context_patch_label (sexp ctx, sexp_sint_t label) {
  sexp bc = sexp_context_bc(ctx);
  unsigned char *data = sexp_bytecode_data(bc)+label;
  *((sexp_sint_t*)data) = sexp_context_pos(ctx)-label;
}

static void generate_lit (sexp ctx, sexp value) {
  emit_push(ctx, value);
}

static void generate_seq (sexp ctx, sexp app) {
  sexp head=app, tail=sexp_cdr(app);
  sexp_uint_t tailp = sexp_context_tailp(ctx);
  sexp_context_tailp(ctx) = 0;
  for ( ; sexp_pairp(tail); head=tail, tail=sexp_cdr(tail))
    if (sexp_pointerp(sexp_car(head)) && (! sexp_litp(sexp_car(head)))) {
      generate(ctx, sexp_car(head));
      emit(ctx, SEXP_OP_DROP);
      sexp_context_depth(ctx)--;
    }
  sexp_context_tailp(ctx) = tailp;
  generate(ctx, sexp_car(head));
}

static void generate_cnd (sexp ctx, sexp cnd) {
  sexp_sint_t label1, label2, tailp=sexp_context_tailp(ctx);
  sexp_context_tailp(ctx) = 0;
  generate(ctx, sexp_cnd_test(cnd));
  sexp_context_tailp(ctx) = tailp;
  emit(ctx, SEXP_OP_JUMP_UNLESS);
  sexp_context_depth(ctx)--;
  label1 = sexp_context_make_label(ctx);
  generate(ctx, sexp_cnd_pass(cnd));
  sexp_context_tailp(ctx) = tailp;
  emit(ctx, SEXP_OP_JUMP);
  sexp_context_depth(ctx)--;
  label2 = sexp_context_make_label(ctx);
  sexp_context_patch_label(ctx, label1);
  generate(ctx, sexp_cnd_fail(cnd));
  sexp_context_patch_label(ctx, label2);
}

static void generate_non_global_ref (sexp ctx, sexp name, sexp cell,
                                     sexp lambda, sexp fv, int unboxp) {
  sexp_uint_t i;
  sexp loc = sexp_cdr(cell);
  if (loc == lambda && sexp_lambdap(lambda)) {
    /* local ref */
    emit(ctx, SEXP_OP_LOCAL_REF);
    emit_word(ctx, sexp_param_index(lambda, name));
  } else {
    /* closure ref */
    for (i=0; sexp_pairp(fv); fv=sexp_cdr(fv), i++)
      if ((name == sexp_ref_name(sexp_car(fv)))
          && (loc == sexp_ref_loc(sexp_car(fv))))
        break;
    emit(ctx, SEXP_OP_CLOSURE_REF);
    emit_word(ctx, i);
  }
  if (unboxp && (sexp_memq(ctx, name, sexp_lambda_sv(loc)) != SEXP_FALSE))
    emit(ctx, SEXP_OP_CDR);
  sexp_context_depth(ctx)++;
}

static void generate_ref (sexp ctx, sexp ref, int unboxp) {
  sexp lam;
  if (! sexp_lambdap(sexp_ref_loc(ref))) {
    /* global ref */
    if (unboxp) {
      emit(ctx,
           (sexp_cdr(sexp_ref_cell(ref)) == SEXP_UNDEF)
           ? SEXP_OP_GLOBAL_REF : SEXP_OP_GLOBAL_KNOWN_REF);
      emit_word(ctx, (sexp_uint_t)sexp_ref_cell(ref));
    } else
      emit_push(ctx, sexp_ref_cell(ref));
  } else {
    lam = sexp_context_lambda(ctx);
    generate_non_global_ref(ctx, sexp_ref_name(ref), sexp_ref_cell(ref),
                            lam, sexp_lambda_fv(lam), unboxp);
  }
}

static void generate_set (sexp ctx, sexp set) {
  sexp ref = sexp_set_var(set), lambda;
  /* compile the value */
  sexp_context_tailp(ctx) = 0;
  if (sexp_lambdap(sexp_set_value(set)))
    sexp_lambda_name(sexp_set_value(set)) = sexp_ref_name(ref);
  generate(ctx, sexp_set_value(set));
  if (! sexp_lambdap(sexp_ref_loc(ref))) {
    /* global vars are set directly */
    emit_push(ctx, sexp_ref_cell(ref));
    emit(ctx, SEXP_OP_SET_CDR);
  } else {
    lambda = sexp_ref_loc(ref);
    if (sexp_truep(sexp_memq(ctx, sexp_ref_name(ref), sexp_lambda_sv(lambda)))) {
      /* stack or closure mutable vars are boxed */
      generate_ref(ctx, ref, 0);
      emit(ctx, SEXP_OP_SET_CDR);
    } else {
      /* internally defined variable */
      emit(ctx, SEXP_OP_LOCAL_SET);
      emit_word(ctx, sexp_param_index(lambda, sexp_ref_name(ref)));
    }
  }
  sexp_context_depth(ctx)--;
}

static void generate_opcode_app (sexp ctx, sexp app) {
  sexp op = sexp_car(app);
  sexp_sint_t i, num_args, inv_default=0;
  sexp_gc_var1(ls);
  sexp_gc_preserve1(ctx, ls);

  num_args = sexp_unbox_fixnum(sexp_length(ctx, sexp_cdr(app)));
  sexp_context_tailp(ctx) = 0;

  if (sexp_opcode_class(op) != SEXP_OPC_PARAMETER) {

    /* maybe push the default for an optional argument */
    if ((num_args == sexp_opcode_num_args(op))
        && sexp_opcode_variadic_p(op) && sexp_opcode_data(op)) {
      if (sexp_opcode_inverse(op)) {
        inv_default = 1;
      } else {
        if (sexp_opcode_opt_param_p(op)) {
#if SEXP_USE_GREEN_THREADS
          emit(ctx, SEXP_OP_PARAMETER_REF);
          emit_word(ctx, (sexp_uint_t)sexp_opcode_data(op));
#else
          emit_push(ctx, sexp_opcode_data(op));
#endif
          emit(ctx, SEXP_OP_CDR);
        } else {
          emit_push(ctx, sexp_opcode_data(op));
        }
        sexp_push(ctx, sexp_bytecode_literals(sexp_context_bc(ctx)),
                  sexp_opcode_data(op));
        sexp_context_depth(ctx)++;
        num_args++;
      }
    }

    /* push the arguments onto the stack in reverse order */
    ls = ((sexp_opcode_inverse(op)
           && (sexp_opcode_class(op) != SEXP_OPC_ARITHMETIC))
          ? sexp_cdr(app) : sexp_reverse(ctx, sexp_cdr(app)));
    for ( ; sexp_pairp(ls); ls = sexp_cdr(ls))
      generate(ctx, sexp_car(ls));

  }

  /* push the default for inverse opcodes */
  if (inv_default) {
    emit_push(ctx, sexp_opcode_data(op));
    if (sexp_opcode_opt_param_p(op)) emit(ctx, SEXP_OP_CDR);
    sexp_context_depth(ctx)++;
    num_args++;
  }

  /* emit the actual operator call */
  switch (sexp_opcode_class(op)) {
  case SEXP_OPC_ARITHMETIC:
    /* fold variadic arithmetic operators */
    for (i=num_args-1; i>0; i--)
      emit(ctx, sexp_opcode_code(op));
    break;
  case SEXP_OPC_ARITHMETIC_CMP:
    if (num_args > 2) {
      emit(ctx, SEXP_OP_STACK_REF);
      emit_word(ctx, 2);
      emit(ctx, SEXP_OP_STACK_REF);
      emit_word(ctx, 2);
      emit(ctx, sexp_opcode_code(op));
      emit(ctx, SEXP_OP_AND);
      for (i=num_args-2; i>0; i--) {
        emit(ctx, SEXP_OP_STACK_REF);
        emit_word(ctx, 3);
        emit(ctx, SEXP_OP_STACK_REF);
        emit_word(ctx, 3);
        emit(ctx, sexp_opcode_code(op));
        emit(ctx, SEXP_OP_AND);
        emit(ctx, SEXP_OP_AND);
      }
    } else
      emit(ctx, sexp_opcode_code(op));
    break;
  case SEXP_OPC_FOREIGN:
    emit(ctx, sexp_opcode_code(op));
    emit_word(ctx, (sexp_uint_t)op);
    break;
  case SEXP_OPC_TYPE_PREDICATE:
  case SEXP_OPC_GETTER:
  case SEXP_OPC_SETTER:
  case SEXP_OPC_CONSTRUCTOR:
    emit(ctx, sexp_opcode_code(op));
    if ((sexp_opcode_class(op) != SEXP_OPC_CONSTRUCTOR)
        || sexp_opcode_code(op) == SEXP_OP_MAKE) {
      if (sexp_opcode_data(op))
        emit_word(ctx, sexp_unbox_fixnum(sexp_opcode_data(op)));
      if (sexp_opcode_data2(op))
        emit_word(ctx, sexp_unbox_fixnum(sexp_opcode_data2(op)));
    }
    break;
  case SEXP_OPC_PARAMETER:
#if SEXP_USE_GREEN_THREADS
    if (num_args > 0) {
      if (sexp_opcode_data2(op) && sexp_applicablep(sexp_opcode_data2(op))) {
        ls = sexp_list2(ctx, sexp_opcode_data2(op), sexp_cadr(app));
        generate(ctx, ls);
      } else {
        generate(ctx, sexp_cadr(app));
      }
    }
    emit(ctx, SEXP_OP_PARAMETER_REF);
    emit_word(ctx, (sexp_uint_t)op);
    sexp_push(ctx, sexp_bytecode_literals(sexp_context_bc(ctx)), op);
#else
    if (num_args > 0) generate(ctx, sexp_cadr(app));
    emit_push(ctx, sexp_opcode_data(op));
#endif
    emit(ctx, ((num_args == 0) ? SEXP_OP_CDR : SEXP_OP_SET_CDR));
    break;
  default:
    emit(ctx, sexp_opcode_code(op));
  }

  sexp_context_depth(ctx) -= (num_args-1);
  sexp_gc_release1(ctx);
}

static void generate_general_app (sexp ctx, sexp app) {
  sexp_uint_t len = sexp_unbox_fixnum(sexp_length(ctx, sexp_cdr(app))),
    tailp = sexp_context_tailp(ctx);
  sexp_gc_var1(ls);
  sexp_gc_preserve1(ctx, ls);

  /* push the arguments onto the stack */
  sexp_context_tailp(ctx) = 0;
  for (ls=sexp_reverse(ctx, sexp_cdr(app)); sexp_pairp(ls); ls=sexp_cdr(ls))
    generate(ctx, sexp_car(ls));

  /* push the operator onto the stack */
  generate(ctx, sexp_car(app));

  /* maybe overwrite the current frame */
  emit(ctx, (tailp ? SEXP_OP_TAIL_CALL : SEXP_OP_CALL));
  emit_word(ctx, (sexp_uint_t)sexp_make_fixnum(len));

  sexp_context_tailp(ctx) = tailp;
  sexp_context_depth(ctx) -= len;
  sexp_gc_release1(ctx);
}

static void generate_app (sexp ctx, sexp app) {
  if (sexp_opcodep(sexp_car(app)))
    generate_opcode_app(ctx, app);
  else
    generate_general_app(ctx, app);
}

static void generate_lambda (sexp ctx, sexp lambda) {
  sexp ctx2, fv, ls, flags, len, ref, prev_lambda, prev_fv;
  sexp_uint_t k;
  sexp_gc_var2(tmp, bc);
  sexp_gc_preserve2(ctx, tmp, bc);
  prev_lambda = sexp_context_lambda(ctx);
  prev_fv = sexp_lambdap(prev_lambda) ? sexp_lambda_fv(prev_lambda) : SEXP_NULL;
  fv = sexp_lambda_fv(lambda);
  ctx2 = sexp_make_eval_context(ctx, sexp_context_stack(ctx), sexp_context_env(ctx), 0);
  sexp_context_lambda(ctx2) = lambda;
  /* allocate space for local vars */
  for (ls=sexp_lambda_locals(lambda); sexp_pairp(ls); ls=sexp_cdr(ls))
    emit_push(ctx2, SEXP_VOID);
  /* box mutable vars */
  for (ls=sexp_lambda_sv(lambda); sexp_pairp(ls); ls=sexp_cdr(ls)) {
    k = sexp_param_index(lambda, sexp_car(ls));
    if (k >= 0) {
      emit(ctx2, SEXP_OP_LOCAL_REF);
      emit_word(ctx2, k);
      emit_push(ctx2, sexp_car(ls));
      emit(ctx2, SEXP_OP_CONS);
      emit(ctx2, SEXP_OP_LOCAL_SET);
      emit_word(ctx2, k);
      emit(ctx2, SEXP_OP_DROP);
    }
  }
  sexp_context_tailp(ctx2) = 1;
  generate(ctx2, sexp_lambda_body(lambda));
  flags = sexp_make_fixnum((sexp_listp(ctx2, sexp_lambda_params(lambda))
                             == SEXP_FALSE) ? 1uL : 0uL);
  len = sexp_length(ctx2, sexp_lambda_params(lambda));
  bc = finalize_bytecode(ctx2);
  sexp_bytecode_name(bc) = sexp_lambda_name(lambda);
  sexp_bytecode_source(bc) = sexp_lambda_source(lambda);
  if (sexp_nullp(fv)) {
    /* shortcut, no free vars */
    tmp = sexp_make_vector(ctx2, SEXP_ZERO, SEXP_VOID);
    tmp = sexp_make_procedure(ctx2, flags, len, bc, tmp);
    sexp_push(ctx, sexp_bytecode_literals(sexp_context_bc(ctx)), tmp);
    generate_lit(ctx, tmp);
  } else {
    /* push the closed vars */
    emit_push(ctx, SEXP_VOID);
    emit_push(ctx, sexp_length(ctx, fv));
    emit(ctx, SEXP_OP_MAKE_VECTOR);
    sexp_context_depth(ctx)--;
    for (k=0; sexp_pairp(fv); fv=sexp_cdr(fv), k++) {
      ref = sexp_car(fv);
      generate_non_global_ref(ctx, sexp_ref_name(ref), sexp_ref_cell(ref),
                              prev_lambda, prev_fv, 0);
      emit_push(ctx, sexp_make_fixnum(k));
      emit(ctx, SEXP_OP_STACK_REF);
      emit_word(ctx, 3);
      emit(ctx, SEXP_OP_VECTOR_SET);
      emit(ctx, SEXP_OP_DROP);
      sexp_context_depth(ctx)--;
    }
    /* push the additional procedure info and make the closure */
    emit_push(ctx, bc);
    emit_push(ctx, len);
    emit_push(ctx, flags);
    emit(ctx, SEXP_OP_MAKE_PROCEDURE);
  }
  sexp_gc_release2(ctx);
}

static void generate (sexp ctx, sexp x) {
  if (sexp_pointerp(x)) {
    switch (sexp_pointer_tag(x)) {
    case SEXP_PAIR:   generate_app(ctx, x); break;
    case SEXP_LAMBDA: generate_lambda(ctx, x); break;
    case SEXP_CND:    generate_cnd(ctx, x); break;
    case SEXP_REF:    generate_ref(ctx, x, 1); break;
    case SEXP_SET:    generate_set(ctx, x); break;
    case SEXP_SEQ:    generate_seq(ctx, sexp_seq_ls(x)); break;
    case SEXP_LIT:    generate_lit(ctx, sexp_lit_value(x)); break;
    default:          generate_lit(ctx, x);
    }
  } else {
    generate_lit(ctx, x);
  }
}

static sexp make_param_list (sexp ctx, sexp_uint_t i) {
  sexp_gc_var1(res);
  sexp_gc_preserve1(ctx, res);
  res = SEXP_NULL;
  for ( ; i>0; i--)
    res = sexp_cons(ctx, sexp_make_fixnum(i), res);
  sexp_gc_release1(ctx);
  return res;
}

static sexp make_opcode_procedure (sexp ctx, sexp op, sexp_uint_t i) {
  sexp ls, bc, res, env;
  sexp_gc_var5(params, ref, refs, lambda, ctx2);
  if (i == sexp_opcode_num_args(op)) { /* return before preserving */
    if (sexp_opcode_proc(op)) return sexp_opcode_proc(op);
  } else if (i < sexp_opcode_num_args(op)) {
    return sexp_compile_error(ctx, "not enough args for opcode", op);
  } else if (! sexp_opcode_variadic_p(op)) { /* i > num_args */
    return sexp_compile_error(ctx, "too many args for opcode", op);
  }
  sexp_gc_preserve5(ctx, params, ref, refs, lambda, ctx2);
  params = make_param_list(ctx, i);
  lambda = sexp_make_lambda(ctx, params);
  ctx2 = sexp_make_child_context(ctx, lambda);
  env = sexp_extend_env(ctx2, sexp_context_env(ctx), params, lambda);
  sexp_context_env(ctx2) = env;
  for (ls=params, refs=SEXP_NULL; sexp_pairp(ls); ls=sexp_cdr(ls)) {
    ref = sexp_make_ref(ctx2, sexp_car(ls), sexp_env_cell(env, sexp_car(ls)));
    sexp_push(ctx2, refs, ref);
  }
  refs = sexp_reverse(ctx2, refs);
  refs = sexp_cons(ctx2, op, refs);
  generate_opcode_app(ctx2, refs);
  bc = finalize_bytecode(ctx2);
  sexp_bytecode_name(bc) = sexp_c_string(ctx2, sexp_opcode_name(op), -1);
  res=sexp_make_procedure(ctx2, SEXP_ZERO, sexp_make_fixnum(i), bc, SEXP_VOID);
  if (i == sexp_opcode_num_args(op))
    sexp_opcode_proc(op) = res;
  sexp_gc_release5(ctx);
  return res;
}

/*********************** the virtual machine **************************/

static sexp sexp_save_stack (sexp ctx, sexp *stack, sexp_uint_t to) {
  sexp res, *data;
  sexp_uint_t i;
  res = sexp_make_vector(ctx, sexp_make_fixnum(to), SEXP_VOID);
  data = sexp_vector_data(res);
  for (i=0; i<to; i++)
    data[i] = stack[i];
  return res;
}

static sexp_uint_t sexp_restore_stack (sexp saved, sexp *current) {
  sexp_uint_t len = sexp_vector_length(saved), i;
  sexp *from = sexp_vector_data(saved);
  for (i=0; i<len; i++)
    current[i] = from[i];
  return len;
}

#define _ARG1 stack[top-1]
#define _ARG2 stack[top-2]
#define _ARG3 stack[top-3]
#define _ARG4 stack[top-4]
#define _ARG5 stack[top-5]
#define _ARG6 stack[top-6]
#define _PUSH(x) (stack[top++]=(x))

#if SEXP_USE_ALIGNED_BYTECODE
#define _ALIGN_IP() ip = (unsigned char *)sexp_word_align((sexp_uint_t)ip)
#else
#define _ALIGN_IP()
#endif

#define _WORD0 ((sexp*)ip)[0]
#define _UWORD0 ((sexp_uint_t*)ip)[0]
#define _SWORD0 ((sexp_sint_t*)ip)[0]
#define _WORD1 ((sexp*)ip)[1]
#define _UWORD1 ((sexp_uint_t*)ip)[1]
#define _SWORD1 ((sexp_sint_t*)ip)[1]

#define sexp_raise(msg, args)                                       \
  do {sexp_context_top(ctx) = top+1;                                \
      stack[top] = args;                                            \
      stack[top] = sexp_user_exception(ctx, self, msg, stack[top]); \
      top++;                                                        \
      goto call_error_handler;}                                     \
  while (0)

#define sexp_check_exception()                                 \
  do {if (sexp_exceptionp(_ARG1)) {                            \
      goto call_error_handler;}}                               \
    while (0)

static int sexp_check_type(sexp ctx, sexp a, sexp b) {
  int d;
  sexp t, v;
  if (! sexp_pointerp(a))
    return 0;
  if (sexp_isa(a, b))
    return 1;
  t = sexp_object_type(ctx, a);
  v = sexp_type_cpl(t);
  d = sexp_type_depth(b);
  return sexp_vectorp(v)
    && (d < sexp_vector_length(v))
    && sexp_vector_ref(v, sexp_make_fixnum(d)) == b;
}

#if SEXP_USE_DEBUG_VM
#include "opt/opcode_names.h"
#endif

#if SEXP_USE_EXTENDED_FCALL
#include "opt/fcall.c"
#endif

sexp sexp_vm (sexp ctx, sexp proc) {
  sexp bc = sexp_procedure_code(proc), cp = sexp_procedure_vars(proc);
  sexp *stack = sexp_stack_data(sexp_context_stack(ctx));
  unsigned char *ip = sexp_bytecode_data(bc);
  sexp_sint_t i, j, k, fp, top = sexp_stack_top(sexp_context_stack(ctx));
#if SEXP_USE_GREEN_THREADS
  sexp root_thread = ctx;
  sexp_sint_t fuel = sexp_context_refuel(ctx);
#endif
#if SEXP_USE_BIGNUMS
  sexp_lsint_t prod;
#endif
  sexp_gc_var3(self, tmp1, tmp2);
  sexp_gc_preserve3(ctx, self, tmp1, tmp2);
  fp = top - 4;
  self = proc;

 loop:
#if SEXP_USE_GREEN_THREADS
  if (--fuel <= 0) {
    tmp1 = sexp_global(ctx, SEXP_G_THREADS_SCHEDULER);
    if (sexp_applicablep(tmp1)) {
      /* save thread */
      sexp_context_top(ctx) = top;
      sexp_context_ip(ctx) = ip;
      sexp_context_last_fp(ctx) = fp;
      sexp_context_proc(ctx) = self;
      /* run scheduler */
      ctx = sexp_apply1(ctx, tmp1, root_thread);
      /* restore thread */
      stack = sexp_stack_data(sexp_context_stack(ctx));
      top = sexp_context_top(ctx);
      fp = sexp_context_last_fp(ctx);
      ip = sexp_context_ip(ctx);
      self = sexp_context_proc(ctx);
      bc = sexp_procedure_code(self);
      cp = sexp_procedure_vars(self);
    }
    fuel = sexp_context_refuel(ctx);
    if (fuel <= 0) goto end_loop;
  }
#endif
#if SEXP_USE_DEBUG_VM
  if (sexp_context_tracep(ctx)) {
    sexp_print_stack(ctx, stack, top, fp, SEXP_FALSE);
    fprintf(stderr, "%s %s ip: %p stack: %p top: %ld fp: %ld (%ld)\n",
            (*ip<=SEXP_OP_NUM_OPCODES) ? reverse_opcode_names[*ip] : "UNKNOWN",
            (SEXP_OP_FCALL0 <= *ip && *ip <= SEXP_OP_FCALL4
             ? sexp_opcode_name(((sexp*)(ip+1))[0]) : ""),
            ip, stack, top, fp, (fp<1024 ? sexp_unbox_fixnum(stack[fp+3]) : -1));
  }
#endif
  switch (*ip++) {
  case SEXP_OP_NOOP:
    break;
  call_error_handler:
    if (! sexp_exception_procedure(_ARG1))
      sexp_exception_procedure(_ARG1) = self;
  case SEXP_OP_RAISE:
    tmp1 = sexp_parameter_ref(ctx, sexp_global(ctx, SEXP_G_ERR_HANDLER));
    sexp_context_last_fp(ctx) = fp;
    if (! sexp_procedurep(tmp1))
      goto end_loop;
    stack[top] = SEXP_ONE;
    stack[top+1] = sexp_make_fixnum(ip-sexp_bytecode_data(bc));
    stack[top+2] = self;
    stack[top+3] = sexp_make_fixnum(fp);
    top += 4;
    self = tmp1;
    bc = sexp_procedure_code(self);
    ip = sexp_bytecode_data(bc);
    cp = sexp_procedure_vars(self);
    fp = top-4;
    break;
  case SEXP_OP_RESUMECC:
    tmp1 = stack[fp-1];
    top = sexp_restore_stack(sexp_vector_ref(cp, 0), stack);
    fp = sexp_unbox_fixnum(_ARG1);
    self = _ARG2;
    bc = sexp_procedure_code(self);
    cp = sexp_procedure_vars(self);
    ip = sexp_bytecode_data(bc) + sexp_unbox_fixnum(_ARG3);
    i = sexp_unbox_fixnum(_ARG4);
    top -= 4;
    _ARG1 = tmp1;
    break;
  case SEXP_OP_CALLCC:
    stack[top] = SEXP_ONE;
    stack[top+1] = sexp_make_fixnum(ip-sexp_bytecode_data(bc));
    stack[top+2] = self;
    stack[top+3] = sexp_make_fixnum(fp);
    tmp1 = _ARG1;
    i = 1;
    sexp_context_top(ctx) = top;
    tmp2 = sexp_make_vector(ctx, SEXP_ONE, SEXP_UNDEF);
    sexp_vector_set(tmp2, SEXP_ZERO, sexp_save_stack(ctx, stack, top+4));
    _ARG1 = sexp_make_procedure(ctx,
                                SEXP_ZERO,
                                SEXP_ONE,
                                sexp_global(ctx, SEXP_G_RESUMECC_BYTECODE),
                                tmp2);
    top++;
    ip -= sizeof(sexp);
    goto make_call;
  case SEXP_OP_APPLY1:
    tmp1 = _ARG1;
    tmp2 = _ARG2;
    i = sexp_unbox_fixnum(sexp_length(ctx, tmp2));
    top += (i-2);
    for ( ; sexp_pairp(tmp2); tmp2=sexp_cdr(tmp2), top--)
      _ARG1 = sexp_car(tmp2);
    top += i+1;
    ip -= sizeof(sexp);
    goto make_call;
  case SEXP_OP_TAIL_CALL:
    _ALIGN_IP();
    i = sexp_unbox_fixnum(_WORD0);             /* number of params */
    tmp1 = _ARG1;                              /* procedure to call */
    /* save frame info */
    tmp2 = stack[fp+3];
    j = sexp_unbox_fixnum(stack[fp]);
    self = stack[fp+2];
    bc = sexp_procedure_code(self);
    cp = sexp_procedure_vars(self);
    ip = (sexp_bytecode_data(bc)
          + sexp_unbox_fixnum(stack[fp+1])) - sizeof(sexp);
    /* copy new args into place */
    for (k=0; k<i; k++)
      stack[fp-j+k] = stack[top-1-i+k];
    top = fp+i-j+1;
    fp = sexp_unbox_fixnum(tmp2);
    goto make_call;
  case SEXP_OP_CALL:
#if SEXP_USE_CHECK_STACK
    if (top+16 >= SEXP_INIT_STACK_SIZE) {
      _ARG1 = sexp_global(ctx, SEXP_G_OOS_ERROR);
      goto end_loop;
    }
#endif
    _ALIGN_IP();
    i = sexp_unbox_fixnum(_WORD0);
    tmp1 = _ARG1;
  make_call:
    if (sexp_opcodep(tmp1)) {
      /* compile non-inlined opcode applications on the fly */
      sexp_context_top(ctx) = top;
      tmp1 = make_opcode_procedure(ctx, tmp1, i);
      if (sexp_exceptionp(tmp1)) {
        _ARG1 = tmp1;
        goto call_error_handler;
      }
    }
    if (! sexp_procedurep(tmp1))
      sexp_raise("non procedure application", sexp_list1(ctx, tmp1));
    j = i - sexp_unbox_fixnum(sexp_procedure_num_args(tmp1));
    if (j < 0)
      sexp_raise("not enough args",
                 sexp_list2(ctx, tmp1, sexp_make_fixnum(i)));
    if (j > 0) {
      if (sexp_procedure_variadic_p(tmp1)) {
        stack[top-i-1] = sexp_cons(ctx, stack[top-i-1], SEXP_NULL);
        for (k=top-i; k<top-(i-j)-1; k++)
          stack[top-i-1] = sexp_cons(ctx, stack[k], stack[top-i-1]);
        for ( ; k<top; k++)
          stack[k-j+1] = stack[k];
        top -= (j-1);
        i -= (j-1);
      } else {
        sexp_raise("too many args",
                   sexp_list2(ctx, tmp1, sexp_make_fixnum(i)));
      }
    } else if (sexp_procedure_variadic_p(tmp1)) {
      /* shift stack, set extra arg to null */
      for (k=top; k>=top-i; k--)
        stack[k] = stack[k-1];
      stack[top-i-1] = SEXP_NULL;
      top++;
      i++;
    }
    _ARG1 = sexp_make_fixnum(i);
    stack[top] = sexp_make_fixnum(ip+sizeof(sexp)-sexp_bytecode_data(bc));
    stack[top+1] = self;
    stack[top+2] = sexp_make_fixnum(fp);
    top += 3;
    self = tmp1;
    bc = sexp_procedure_code(self);
    ip = sexp_bytecode_data(bc);
    cp = sexp_procedure_vars(self);
    fp = top-4;
    break;
  case SEXP_OP_FCALL0:
    _ALIGN_IP();
    tmp1 = _WORD0;
    sexp_context_top(ctx) = top;
    sexp_context_last_fp(ctx) = fp;
    _PUSH(((sexp_proc1)sexp_opcode_func(_WORD0))(ctx sexp_api_pass(_WORD0, 0)));
    ip += sizeof(sexp);
    sexp_check_exception();
    break;
  case SEXP_OP_FCALL1:
    _ALIGN_IP();
    sexp_context_top(ctx) = top;
    _ARG1 = ((sexp_proc2)sexp_opcode_func(_WORD0))(ctx sexp_api_pass(_WORD0, 1), _ARG1);
    ip += sizeof(sexp);
    sexp_check_exception();
    break;
  case SEXP_OP_FCALL2:
    _ALIGN_IP();
    sexp_context_top(ctx) = top;
    _ARG2 = ((sexp_proc3)sexp_opcode_func(_WORD0))(ctx sexp_api_pass(_WORD0, 2), _ARG1, _ARG2);
    top--;
    ip += sizeof(sexp);
    sexp_check_exception();
    break;
  case SEXP_OP_FCALL3:
    _ALIGN_IP();
    sexp_context_top(ctx) = top;
    _ARG3 = ((sexp_proc4)sexp_opcode_func(_WORD0))(ctx sexp_api_pass(_WORD0, 3), _ARG1, _ARG2, _ARG3);
    top -= 2;
    ip += sizeof(sexp);
    sexp_check_exception();
    break;
  case SEXP_OP_FCALL4:
    _ALIGN_IP();
    sexp_context_top(ctx) = top;
    _ARG4 = ((sexp_proc5)sexp_opcode_func(_WORD0))(ctx sexp_api_pass(_WORD0, 4), _ARG1, _ARG2, _ARG3, _ARG4);
    top -= 3;
    ip += sizeof(sexp);
    sexp_check_exception();
    break;
#if SEXP_USE_EXTENDED_FCALL
  case SEXP_OP_FCALLN:
    _ALIGN_IP();
    sexp_context_top(ctx) = top;
    i = sexp_opcode_num_args(_WORD0);
    tmp1 = sexp_fcall(ctx, self, i, _WORD0);
    top -= (i-1);
    _ARG1 = tmp1;
    ip += sizeof(sexp);
    sexp_check_exception();
    break;
#endif
  case SEXP_OP_JUMP_UNLESS:
    _ALIGN_IP();
    if (stack[--top] == SEXP_FALSE)
      ip += _SWORD0;
    else
      ip += sizeof(sexp_sint_t);
    break;
  case SEXP_OP_JUMP:
    _ALIGN_IP();
    ip += _SWORD0;
    break;
  case SEXP_OP_PUSH:
    _ALIGN_IP();
    _PUSH(_WORD0);
    ip += sizeof(sexp);
    break;
  case SEXP_OP_DROP:
    top--;
    break;
  case SEXP_OP_GLOBAL_REF:
    _ALIGN_IP();
    if (sexp_cdr(_WORD0) == SEXP_UNDEF)
      sexp_raise("undefined variable", sexp_list1(ctx, sexp_car(_WORD0)));
    /* ... FALLTHROUGH ... */
  case SEXP_OP_GLOBAL_KNOWN_REF:
    _ALIGN_IP();
    _PUSH(sexp_cdr(_WORD0));
    ip += sizeof(sexp);
    break;
#if SEXP_USE_GREEN_THREADS
  case SEXP_OP_PARAMETER_REF:
    _ALIGN_IP();
    tmp2 = _WORD0;
    ip += sizeof(sexp);
    for (tmp1=sexp_context_params(ctx); sexp_pairp(tmp1); tmp1=sexp_cdr(tmp1))
      if (sexp_caar(tmp1) == tmp2) {
        _PUSH(sexp_car(tmp1));
        goto loop;
      }
    _PUSH(sexp_opcode_data(tmp2));
    break;
#endif
  case SEXP_OP_STACK_REF:
    _ALIGN_IP();
    stack[top] = stack[top - _SWORD0];
    ip += sizeof(sexp);
    top++;
    break;
  case SEXP_OP_LOCAL_REF:
    _ALIGN_IP();
    stack[top] = stack[fp - 1 - _SWORD0];
    ip += sizeof(sexp);
    top++;
    break;
  case SEXP_OP_LOCAL_SET:
    _ALIGN_IP();
    stack[fp - 1 - _SWORD0] = _ARG1;
    _ARG1 = SEXP_VOID;
    ip += sizeof(sexp);
    break;
  case SEXP_OP_CLOSURE_REF:
    _ALIGN_IP();
    _PUSH(sexp_vector_ref(cp, sexp_make_fixnum(_WORD0)));
    ip += sizeof(sexp);
    break;
  case SEXP_OP_VECTOR_REF:
    if (! sexp_vectorp(_ARG1))
      sexp_raise("vector-ref: not a vector", sexp_list1(ctx, _ARG1));
    else if (! sexp_fixnump(_ARG2))
      sexp_raise("vector-ref: not an integer", sexp_list1(ctx, _ARG2));
    i = sexp_unbox_fixnum(_ARG2);
    if ((i < 0) || (i >= sexp_vector_length(_ARG1)))
      sexp_raise("vector-ref: index out of range", sexp_list2(ctx, _ARG1, _ARG2));
    _ARG2 = sexp_vector_ref(_ARG1, _ARG2);
    top--;
    break;
  case SEXP_OP_VECTOR_SET:
    if (! sexp_vectorp(_ARG1))
      sexp_raise("vector-set!: not a vector", sexp_list1(ctx, _ARG1));
    else if (sexp_immutablep(_ARG1))
      sexp_raise("vector-set!: immutable vector", sexp_list1(ctx, _ARG1));
    else if (! sexp_fixnump(_ARG2))
      sexp_raise("vector-set!: not an integer", sexp_list1(ctx, _ARG2));
    i = sexp_unbox_fixnum(_ARG2);
    if ((i < 0) || (i >= sexp_vector_length(_ARG1)))
      sexp_raise("vector-set!: index out of range", sexp_list2(ctx, _ARG1, _ARG2));
    sexp_vector_set(_ARG1, _ARG2, _ARG3);
    _ARG3 = SEXP_VOID;
    top-=2;
    break;
  case SEXP_OP_VECTOR_LENGTH:
    if (! sexp_vectorp(_ARG1))
      sexp_raise("vector-length: not a vector", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_make_fixnum(sexp_vector_length(_ARG1));
    break;
  case SEXP_OP_BYTES_REF:
  case SEXP_OP_STRING_REF:
    if (! sexp_stringp(_ARG1))
      sexp_raise("string-ref: not a string", sexp_list1(ctx, _ARG1));
    else if (! sexp_fixnump(_ARG2))
      sexp_raise("string-ref: not an integer", sexp_list1(ctx, _ARG2));
    i = sexp_unbox_fixnum(_ARG2);
    if ((i < 0) || (i >= sexp_string_length(_ARG1)))
      sexp_raise("string-ref: index out of range", sexp_list2(ctx, _ARG1, _ARG2));
    if (ip[-1] == SEXP_OP_BYTES_REF)
      _ARG2 = sexp_bytes_ref(_ARG1, _ARG2);
    else
#if SEXP_USE_UTF8_STRINGS
      _ARG2 = sexp_string_utf8_ref(ctx, _ARG1, _ARG2);
#else
      _ARG2 = sexp_string_ref(_ARG1, _ARG2);
#endif
    top--;
    break;
  case SEXP_OP_BYTES_SET:
  case SEXP_OP_STRING_SET:
    if (! sexp_stringp(_ARG1))
      sexp_raise("string-set!: not a string", sexp_list1(ctx, _ARG1));
    else if (sexp_immutablep(_ARG1))
      sexp_raise("string-set!: immutable string", sexp_list1(ctx, _ARG1));
    else if (! sexp_fixnump(_ARG2))
      sexp_raise("string-set!: not an integer", sexp_list1(ctx, _ARG2));
    else if (! sexp_charp(_ARG3))
      sexp_raise("string-set!: not a char", sexp_list1(ctx, _ARG3));
    i = sexp_unbox_fixnum(_ARG2);
    if ((i < 0) || (i >= sexp_string_length(_ARG1)))
      sexp_raise("string-set!: index out of range", sexp_list2(ctx, _ARG1, _ARG2));
    if (ip[-1] == SEXP_OP_BYTES_SET)
      sexp_bytes_set(_ARG1, _ARG2, _ARG3);
    else
#if SEXP_USE_UTF8_STRINGS
      sexp_string_utf8_set(ctx, _ARG1, _ARG2, _ARG3);
#else
      sexp_string_set(_ARG1, _ARG2, _ARG3);
#endif
    _ARG3 = SEXP_VOID;
    top-=2;
    break;
  case SEXP_OP_BYTES_LENGTH:
    if (! sexp_stringp(_ARG1))
      sexp_raise("bytes-length: not a byte-vector", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_make_fixnum(sexp_bytes_length(_ARG1));
    break;
  case SEXP_OP_STRING_LENGTH:
    if (! sexp_stringp(_ARG1))
      sexp_raise("string-length: not a string", sexp_list1(ctx, _ARG1));
#if SEXP_USE_UTF8_STRINGS
    _ARG1 = sexp_make_fixnum(sexp_string_utf8_length((unsigned char*)sexp_string_data(_ARG1), sexp_string_length(_ARG1)));
#else
    _ARG1 = sexp_make_fixnum(sexp_string_length(_ARG1));
#endif
    break;
  case SEXP_OP_MAKE_PROCEDURE:
    sexp_context_top(ctx) = top;
    _ARG4 = sexp_make_procedure(ctx, _ARG1, _ARG2, _ARG3, _ARG4);
    top-=3;
    break;
  case SEXP_OP_MAKE_VECTOR:
    sexp_context_top(ctx) = top;
    if (! sexp_fixnump(_ARG1))
      sexp_raise("make-vector: not an integer", sexp_list1(ctx, _ARG1));
    _ARG2 = sexp_make_vector(ctx, _ARG1, _ARG2);
    top--;
    break;
  case SEXP_OP_MAKE_EXCEPTION:
    _ARG5 = sexp_make_exception(ctx, _ARG1, _ARG2, _ARG3, _ARG4, _ARG5);
    top -= 4;
    break;
  case SEXP_OP_AND:
    _ARG2 = sexp_make_boolean((_ARG1 != SEXP_FALSE) && (_ARG2 != SEXP_FALSE));
    top--;
    break;
  case SEXP_OP_EOFP:
    _ARG1 = sexp_make_boolean(_ARG1 == SEXP_EOF); break;
  case SEXP_OP_NULLP:
    _ARG1 = sexp_make_boolean(sexp_nullp(_ARG1)); break;
  case SEXP_OP_FIXNUMP:
    _ARG1 = sexp_make_boolean(sexp_fixnump(_ARG1)); break;
  case SEXP_OP_SYMBOLP:
    _ARG1 = sexp_make_boolean(sexp_symbolp(_ARG1)); break;
  case SEXP_OP_CHARP:
    _ARG1 = sexp_make_boolean(sexp_charp(_ARG1)); break;
  case SEXP_OP_ISA:
    tmp1 = _ARG1, tmp2 = _ARG2;
    if (! sexp_typep(tmp2)) sexp_raise("is-a?: not a type", tmp2);
    top--;
    goto do_check_type;
  case SEXP_OP_TYPEP:
    _ALIGN_IP();
    tmp1 = _ARG1, tmp2 = sexp_type_by_index(ctx, _UWORD0);
    ip += sizeof(sexp);
  do_check_type:
    _ARG1 = sexp_make_boolean(sexp_check_type(ctx, tmp1, tmp2));
    break;
  case SEXP_OP_MAKE:
    _ALIGN_IP();
    _PUSH(sexp_alloc_tagged(ctx, _UWORD1, _UWORD0));
    ip += sizeof(sexp)*2;
    break;
  case SEXP_OP_SLOT_REF:
    _ALIGN_IP();
    if (! sexp_check_type(ctx, _ARG1, sexp_type_by_index(ctx, _UWORD0)))
      sexp_raise("slot-ref: bad type", sexp_list2(ctx, sexp_c_string(ctx, sexp_type_name_by_index(ctx, _UWORD0), -1), _ARG1));
    _ARG1 = sexp_slot_ref(_ARG1, _UWORD1);
    ip += sizeof(sexp)*2;
    break;
  case SEXP_OP_SLOT_SET:
    _ALIGN_IP();
    if (! sexp_check_type(ctx, _ARG1, sexp_type_by_index(ctx, _UWORD0)))
      sexp_raise("slot-set!: bad type", sexp_list2(ctx, sexp_c_string(ctx, sexp_type_name_by_index(ctx, _UWORD0), -1), _ARG1));
    else if (sexp_immutablep(_ARG1))
      sexp_raise("slot-set!: immutable object", sexp_list1(ctx, _ARG1));
    sexp_slot_set(_ARG1, _UWORD1, _ARG2);
    _ARG2 = SEXP_VOID;
    ip += sizeof(sexp)*2;
    top--;
    break;
  case SEXP_OP_SLOTN_REF:
    if (! sexp_typep(_ARG1))
      sexp_raise("slot-ref: not a record type", sexp_list1(ctx, _ARG1));
    else if (! sexp_check_type(ctx, _ARG2, _ARG1))
      sexp_raise("slot-ref: bad type", sexp_list1(ctx, _ARG2));
    else if (! sexp_fixnump(_ARG3))
      sexp_raise("slot-ref: not an integer", sexp_list1(ctx, _ARG3));
    _ARG3 = sexp_slot_ref(_ARG2, sexp_unbox_fixnum(_ARG3));
    top-=2;
    break;
  case SEXP_OP_SLOTN_SET:
    if (! sexp_typep(_ARG1))
      sexp_raise("slot-set!: not a record type", sexp_list1(ctx, _ARG1));
    else if (! sexp_check_type(ctx, _ARG2, _ARG1))
      sexp_raise("slot-set!: bad type", sexp_list1(ctx, _ARG2));
    else if (sexp_immutablep(_ARG2))
      sexp_raise("slot-set!: immutable object", sexp_list1(ctx, _ARG2));
    else if (! sexp_fixnump(_ARG3))
      sexp_raise("slot-set!: not an integer", sexp_list1(ctx, _ARG3));
    sexp_slot_set(_ARG2, sexp_unbox_fixnum(_ARG3), _ARG4);
    _ARG4 = SEXP_VOID;
    top-=3;
    break;
  case SEXP_OP_CAR:
    if (! sexp_pairp(_ARG1))
      sexp_raise("car: not a pair", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_car(_ARG1); break;
  case SEXP_OP_CDR:
    if (! sexp_pairp(_ARG1))
      sexp_raise("cdr: not a pair", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_cdr(_ARG1); break;
  case SEXP_OP_SET_CAR:
    if (! sexp_pairp(_ARG1))
      sexp_raise("set-car!: not a pair", sexp_list1(ctx, _ARG1));
    else if (sexp_immutablep(_ARG1))
      sexp_raise("set-car!: immutable pair", sexp_list1(ctx, _ARG1));
    sexp_car(_ARG1) = _ARG2;
    _ARG2 = SEXP_VOID;
    top--;
    break;
  case SEXP_OP_SET_CDR:
    if (! sexp_pairp(_ARG1))
      sexp_raise("set-cdr!: not a pair", sexp_list1(ctx, _ARG1));
    else if (sexp_immutablep(_ARG1))
      sexp_raise("set-cdr!: immutable pair", sexp_list1(ctx, _ARG1));
    sexp_cdr(_ARG1) = _ARG2;
    _ARG2 = SEXP_VOID;
    top--;
    break;
  case SEXP_OP_CONS:
    sexp_context_top(ctx) = top;
    _ARG2 = sexp_cons(ctx, _ARG1, _ARG2);
    top--;
    break;
  case SEXP_OP_ADD:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
#if SEXP_USE_BIGNUMS
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      j = sexp_unbox_fixnum(tmp1) + sexp_unbox_fixnum(tmp2);
      if ((j < SEXP_MIN_FIXNUM) || (j > SEXP_MAX_FIXNUM))
        _ARG1 = sexp_add(ctx, tmp1=sexp_fixnum_to_bignum(ctx, tmp1), tmp2);
      else
        _ARG1 = sexp_make_fixnum(j);
    }
    else {
      _ARG1 = sexp_add(ctx, tmp1, tmp2);
      sexp_check_exception();
    }
#else
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_fx_add(tmp1, tmp2);
#if SEXP_USE_FLONUMS
    else if (sexp_flonump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_fp_add(ctx, tmp1, tmp2);
    else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, sexp_flonum_value(tmp1) + (double)sexp_unbox_fixnum(tmp2));
    else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, (double)sexp_unbox_fixnum(tmp1) + sexp_flonum_value(tmp2));
#endif
    else sexp_raise("+: not a number", sexp_list2(ctx, tmp1, tmp2));
#endif
    break;
  case SEXP_OP_SUB:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
#if SEXP_USE_BIGNUMS
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      j = sexp_unbox_fixnum(tmp1) - sexp_unbox_fixnum(tmp2);
      if ((j < SEXP_MIN_FIXNUM) || (j > SEXP_MAX_FIXNUM))
        _ARG1 = sexp_sub(ctx, tmp1=sexp_fixnum_to_bignum(ctx, tmp1), tmp2);
      else
        _ARG1 = sexp_make_fixnum(j);
    }
    else {
      _ARG1 = sexp_sub(ctx, tmp1, tmp2);
      sexp_check_exception();
    }
#else
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_fx_sub(tmp1, tmp2);
#if SEXP_USE_FLONUMS
    else if (sexp_flonump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_fp_sub(ctx, tmp1, tmp2);
    else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, sexp_flonum_value(tmp1) - (double)sexp_unbox_fixnum(tmp2));
    else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, (double)sexp_unbox_fixnum(tmp1) - sexp_flonum_value(tmp2));
#endif
    else sexp_raise("-: not a number", sexp_list2(ctx, tmp1, tmp2));
#endif
    break;
  case SEXP_OP_MUL:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
#if SEXP_USE_BIGNUMS
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      prod = (sexp_lsint_t)sexp_unbox_fixnum(tmp1) * sexp_unbox_fixnum(tmp2);
      if ((prod < SEXP_MIN_FIXNUM) || (prod > SEXP_MAX_FIXNUM))
        _ARG1 = sexp_mul(ctx, tmp1=sexp_fixnum_to_bignum(ctx, tmp1), tmp2);
      else
        _ARG1 = sexp_make_fixnum(prod);
    }
    else {
      _ARG1 = sexp_mul(ctx, tmp1, tmp2);
      sexp_check_exception();
    }
#else
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_fx_mul(tmp1, tmp2);
#if SEXP_USE_FLONUMS
    else if (sexp_flonump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_fp_mul(ctx, tmp1, tmp2);
    else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, sexp_flonum_value(tmp1) * (double)sexp_unbox_fixnum(tmp2));
    else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, (double)sexp_unbox_fixnum(tmp1) * sexp_flonum_value(tmp2));
#endif
    else sexp_raise("*: not a number", sexp_list2(ctx, tmp1, tmp2));
#endif
    break;
  case SEXP_OP_DIV:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
    if (tmp2 == SEXP_ZERO) {
#if SEXP_USE_FLONUMS
      if (sexp_flonump(tmp1) && sexp_flonum_value(tmp1) == 0.0)
        _ARG1 = sexp_make_flonum(ctx, 0.0);
      else
#endif
        sexp_raise("divide by zero", SEXP_NULL);
    } else if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
#if SEXP_USE_FLONUMS
      tmp1 = sexp_fixnum_to_flonum(ctx, tmp1);
      tmp2 = sexp_fixnum_to_flonum(ctx, tmp2);
      _ARG1 = sexp_fp_div(ctx, tmp1, tmp2);
      if (sexp_flonum_value(_ARG1) == trunc(sexp_flonum_value(_ARG1)))
        _ARG1 = sexp_make_fixnum(sexp_flonum_value(_ARG1));
#else
      _ARG1 = sexp_fx_div(tmp1, tmp2);
#endif
    }
#if SEXP_USE_BIGNUMS
    else {
      _ARG1 = sexp_div(ctx, tmp1, tmp2);
      sexp_check_exception();
    }
#else
#if SEXP_USE_FLONUMS
    else if (sexp_flonump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_fp_div(ctx, tmp1, tmp2);
    else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, sexp_flonum_value(tmp1) / (double)sexp_unbox_fixnum(tmp2));
    else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2))
      _ARG1 = sexp_make_flonum(ctx, (double)sexp_unbox_fixnum(tmp1) / sexp_flonum_value(tmp2));
#endif
    else sexp_raise("/: not a number", sexp_list2(ctx, tmp1, tmp2));
#endif
    break;
  case SEXP_OP_QUOTIENT:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      if (tmp2 == SEXP_ZERO)
        sexp_raise("divide by zero", SEXP_NULL);
      _ARG1 = sexp_fx_div(tmp1, tmp2);
    }
#if SEXP_USE_BIGNUMS
    else {
      _ARG1 = sexp_quotient(ctx, tmp1, tmp2);
      sexp_check_exception();
    }
#else
    else sexp_raise("quotient: not an integer", sexp_list2(ctx, _ARG1, tmp2));
#endif
    break;
  case SEXP_OP_REMAINDER:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      if (tmp2 == SEXP_ZERO)
        sexp_raise("divide by zero", SEXP_NULL);
      _ARG1 = sexp_fx_rem(tmp1, tmp2);
    }
#if SEXP_USE_BIGNUMS
    else {
      _ARG1 = sexp_remainder(ctx, tmp1, tmp2);
      sexp_check_exception();
    }
#else
    else sexp_raise("remainder: not an integer", sexp_list2(ctx, _ARG1, tmp2));
#endif
    break;
  case SEXP_OP_LT:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      i = (sexp_sint_t)tmp1 < (sexp_sint_t)tmp2;
#if SEXP_USE_BIGNUMS
      _ARG1 = sexp_make_boolean(i);
    } else {
      _ARG1 = sexp_compare(ctx, tmp1, tmp2);
      sexp_check_exception();
      _ARG1 = sexp_make_boolean(sexp_unbox_fixnum(_ARG1) < 0);
    }
#else
#if SEXP_USE_FLONUMS
    } else if (sexp_flonump(tmp1) && sexp_flonump(tmp2)) {
      i = sexp_flonum_value(tmp1) < sexp_flonum_value(tmp2);
    } else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2)) {
      i = sexp_flonum_value(tmp1) < (double)sexp_unbox_fixnum(tmp2); 
    } else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2)) {
      i = (double)sexp_unbox_fixnum(tmp1) < sexp_flonum_value(tmp2);
#endif
    } else sexp_raise("<: not a number", sexp_list2(ctx, tmp1, tmp2));
    _ARG1 = sexp_make_boolean(i);
#endif
    break;
  case SEXP_OP_LE:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      i = (sexp_sint_t)tmp1 <= (sexp_sint_t)tmp2;
#if SEXP_USE_BIGNUMS
      _ARG1 = sexp_make_boolean(i);
    } else {
      _ARG1 = sexp_compare(ctx, tmp1, tmp2);
      sexp_check_exception();
      _ARG1 = sexp_make_boolean(sexp_unbox_fixnum(_ARG1) <= 0);
    }
#else
#if SEXP_USE_FLONUMS
    } else if (sexp_flonump(tmp1) && sexp_flonump(tmp2)) {
      i = sexp_flonum_value(tmp1) <= sexp_flonum_value(tmp2);
    } else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2)) {
      i = sexp_flonum_value(tmp1) <= (double)sexp_unbox_fixnum(tmp2);
    } else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2)) {
      i = (double)sexp_unbox_fixnum(tmp1) <= sexp_flonum_value(tmp2);
#endif
    } else sexp_raise("<=: not a number", sexp_list2(ctx, tmp1, tmp2));
    _ARG1 = sexp_make_boolean(i);
#endif
    break;
  case SEXP_OP_EQN:
    tmp1 = _ARG1, tmp2 = _ARG2;
    sexp_context_top(ctx) = --top;
    if (sexp_fixnump(tmp1) && sexp_fixnump(tmp2)) {
      i = tmp1 == tmp2;
#if SEXP_USE_BIGNUMS
      _ARG1 = sexp_make_boolean(i);
    } else {
      _ARG1 = sexp_compare(ctx, tmp1, tmp2);
      sexp_check_exception();
      _ARG1 = sexp_make_boolean(sexp_unbox_fixnum(_ARG1) == 0);
    }
#else
#if SEXP_USE_FLONUMS
    } else if (sexp_flonump(tmp1) && sexp_flonump(tmp2)) {
      i = sexp_flonum_value(tmp1) == sexp_flonum_value(tmp2);
    } else if (sexp_flonump(tmp1) && sexp_fixnump(tmp2)) {
      i = sexp_flonum_value(tmp1) == (double)sexp_unbox_fixnum(tmp2);
    } else if (sexp_fixnump(tmp1) && sexp_flonump(tmp2)) {
      i = (double)sexp_unbox_fixnum(tmp1) == sexp_flonum_value(tmp2);
#endif
    } else sexp_raise("=: not a number", sexp_list2(ctx, tmp1, tmp2));
    _ARG1 = sexp_make_boolean(i);
#endif
    break;
  case SEXP_OP_EQ:
    _ARG2 = sexp_make_boolean(_ARG1 == _ARG2);
    top--;
    break;
  case SEXP_OP_FIX2FLO:
    if (sexp_fixnump(_ARG1))
      _ARG1 = sexp_fixnum_to_flonum(ctx, _ARG1);
#if SEXP_USE_BIGNUMS
    else if (sexp_bignump(_ARG1))
      _ARG1 = sexp_make_flonum(ctx, sexp_bignum_to_double(_ARG1));
#endif
    else if (! sexp_flonump(_ARG1))
      sexp_raise("exact->inexact: not a number", sexp_list1(ctx, _ARG1));
    break;
  case SEXP_OP_FLO2FIX:
    if (sexp_flonump(_ARG1)) {
      if (sexp_flonum_value(_ARG1) != trunc(sexp_flonum_value(_ARG1))) {
        sexp_raise("inexact->exact: not an integer", sexp_list1(ctx, _ARG1));
#if SEXP_USE_BIGNUMS
      } else if ((sexp_flonum_value(_ARG1) > SEXP_MAX_FIXNUM)
                 || sexp_flonum_value(_ARG1) < SEXP_MIN_FIXNUM) {
        _ARG1 = sexp_double_to_bignum(ctx, sexp_flonum_value(_ARG1));
#endif
      } else {
        _ARG1 = sexp_make_fixnum((sexp_sint_t)sexp_flonum_value(_ARG1));
      }
    } else if (! sexp_fixnump(_ARG1) && ! sexp_bignump(_ARG1)) {
      sexp_raise("inexact->exact: not a number", sexp_list1(ctx, _ARG1));
    }
    break;
  case SEXP_OP_CHAR2INT:
    if (! sexp_charp(_ARG1))
      sexp_raise("char->integer: not a character", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_make_fixnum(sexp_unbox_character(_ARG1));
    break;
  case SEXP_OP_INT2CHAR:
    if (! sexp_fixnump(_ARG1))
      sexp_raise("integer->char: not an integer", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_make_character(sexp_unbox_fixnum(_ARG1));
    break;
  case SEXP_OP_CHAR_UPCASE:
    if (! sexp_charp(_ARG1))
      sexp_raise("char-upcase: not a character", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_make_character(toupper(sexp_unbox_character(_ARG1)));
    break;
  case SEXP_OP_CHAR_DOWNCASE:
    if (! sexp_charp(_ARG1))
      sexp_raise("char-downcase: not a character", sexp_list1(ctx, _ARG1));
    _ARG1 = sexp_make_character(tolower(sexp_unbox_character(_ARG1)));
    break;
  case SEXP_OP_WRITE_CHAR:
    if (! sexp_charp(_ARG1))
      sexp_raise("write-char: not a character", sexp_list1(ctx, _ARG1));
    if (! sexp_oportp(_ARG2))
      sexp_raise("write-char: not an output-port", sexp_list1(ctx, _ARG2));
#if SEXP_USE_UTF8_STRINGS
    if (sexp_unbox_character(_ARG1) >= 0x80)
      sexp_write_utf8_char(ctx, sexp_unbox_character(_ARG1), _ARG2);
    else
#endif
    sexp_write_char(ctx, sexp_unbox_character(_ARG1), _ARG2);
    _ARG2 = SEXP_VOID;
    top--;
    break;
  case SEXP_OP_NEWLINE:
    if (! sexp_oportp(_ARG1))
      sexp_raise("newline: not an output-port", sexp_list1(ctx, _ARG1));
    sexp_newline(ctx, _ARG1);
    _ARG1 = SEXP_VOID;
    break;
  case SEXP_OP_READ_CHAR:
    if (! sexp_iportp(_ARG1))
      sexp_raise("read-char: not an input-port", sexp_list1(ctx, _ARG1));
    i = sexp_read_char(ctx, _ARG1);
#if SEXP_USE_UTF8_STRINGS
    if (i >= 0x80)
      _ARG1 = sexp_read_utf8_char(ctx, _ARG1, i);
    else
#endif
    if (i == EOF) {
#if SEXP_USE_GREEN_THREADS
      if ((errno == EAGAIN)
          && sexp_applicablep(sexp_global(ctx, SEXP_G_THREADS_BLOCKER))) {
        sexp_apply1(ctx, sexp_global(ctx, SEXP_G_THREADS_BLOCKER), _ARG1);
        fuel = 0;
        ip--;      /* try again */
      } else
#endif
        _ARG1 = SEXP_EOF;
    } else
      _ARG1 = sexp_make_character(i);
    break;
  case SEXP_OP_PEEK_CHAR:
    if (! sexp_iportp(_ARG1))
      sexp_raise("peek-char: not an input-port", sexp_list1(ctx, _ARG1));
    i = sexp_read_char(ctx, _ARG1);
    sexp_push_char(ctx, i, _ARG1);
    if (i == EOF) {
#if SEXP_USE_GREEN_THREADS
      if ((errno == EAGAIN)
          && sexp_applicablep(sexp_global(ctx, SEXP_G_THREADS_BLOCKER))) {
        sexp_apply1(ctx, sexp_global(ctx, SEXP_G_THREADS_BLOCKER), _ARG1);
        fuel = 0;
        ip--;      /* try again */
      } else
#endif
        _ARG1 = SEXP_EOF;
    } else
      _ARG1 = sexp_make_character(i);
    break;
  case SEXP_OP_YIELD:
    fuel = 0;
    _PUSH(SEXP_VOID);
    break;
  case SEXP_OP_RET:
    i = sexp_unbox_fixnum(stack[fp]);
    stack[fp-i] = _ARG1;
    top = fp-i+1;
    self = stack[fp+2];
    bc = sexp_procedure_code(self);
    ip = sexp_bytecode_data(bc) + sexp_unbox_fixnum(stack[fp+1]);
    cp = sexp_procedure_vars(self);
    fp = sexp_unbox_fixnum(stack[fp+3]);
    break;
  case SEXP_OP_DONE:
    goto end_loop;
  default:
    sexp_raise("unknown opcode", sexp_list1(ctx, sexp_make_fixnum(*(ip-1))));
  }
  goto loop;

 end_loop:
#if SEXP_USE_GREEN_THREADS
  if (ctx != root_thread) {
    if (sexp_context_refuel(root_thread) <= 0) {
      /* the root already terminated */
      _ARG1 = SEXP_VOID;
    } else {
      /* don't return from child threads */
      sexp_context_refuel(ctx) = fuel = 0;
      goto loop;
    }
  }
#endif
  sexp_gc_release3(ctx);
  sexp_context_top(ctx) = top;
  return _ARG1;
}

/******************************* apply ********************************/

sexp sexp_apply1 (sexp ctx, sexp f, sexp x) {
  sexp res;
  sexp_gc_var1(args);
  if (sexp_opcodep(f)) {
    res = ((sexp_proc2)sexp_opcode_func(f))(ctx sexp_api_pass(f, 1), x);
  } else {
    sexp_gc_preserve1(ctx, args);
    res = sexp_apply(ctx, f, args=sexp_list1(ctx, x));
    sexp_gc_release1(ctx);
  }
  return res;
}

sexp sexp_apply (sexp ctx, sexp proc, sexp args) {
  sexp res, ls, *stack = sexp_stack_data(sexp_context_stack(ctx));
  sexp_sint_t top = sexp_context_top(ctx), len, offset;
  len = sexp_unbox_fixnum(sexp_length(ctx, args));
  if (sexp_opcodep(proc))
    proc = make_opcode_procedure(ctx, proc, len);
  if (! sexp_procedurep(proc)) {
    res = sexp_exceptionp(proc) ? proc :
      sexp_type_exception(ctx, NULL, SEXP_PROCEDURE, proc);
  } else {
    offset = top + len;
    for (ls=args; sexp_pairp(ls); ls=sexp_cdr(ls), top++)
      stack[--offset] = sexp_car(ls);
    stack[top++] = sexp_make_fixnum(len);
    stack[top++] = SEXP_ZERO;
    stack[top++] = sexp_global(ctx, SEXP_G_FINAL_RESUMER);
    stack[top++] = SEXP_ZERO;
    sexp_context_top(ctx) = top;
    res = sexp_vm(ctx, proc);
    if (! res) res = SEXP_VOID; /* shouldn't happen */
  }
  return res;
}