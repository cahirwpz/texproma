#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#include "ansi.h"
#include "cell.h"

typedef struct {
  char t;
  const cell_type_t *ct;
} fn_sig_t;

static const fn_sig_t fn_sig_map[] = {
  {'i', CT_INT},
  {'f', CT_FLOAT},
  {'a', CT_ATOM},
  {'s', CT_STRING},
  {'m', CT_MONO},
  {'c', CT_COLOR},
  {'?', NULL},
  {0}
};

fn_t *new_fn(fn_ctor_t *ctor) {
  unsigned n = strlen(ctor->sig);
  fn_t *fn = calloc(1, sizeof(fn_t) + n * sizeof(fn_arg_t));

  fn->ptr = ctor->ptr;
  fn->count = n;

  for (unsigned i = 0; i < n; i++) {
    const fn_sig_t *fn_sig;
    char t = tolower(ctor->sig[i]);

    for (fn_sig = fn_sig_map; fn_sig->t; fn_sig++) {
      if (t == fn_sig->t) {
        fn->args[i].type = fn_sig->ct;
        break;
      }
    }

    assert(fn_sig->t != 0);

    fn->args[i].flags = islower(ctor->sig[i]) ? ARG_INPUT : ARG_OUTPUT;
  }

  return fn;
}

unsigned fn_arg_count(const fn_t *fn, fn_arg_flags_t flags) {
  unsigned count = 0;

  for (unsigned i = 0; i < fn->count; i++)
    if (fn->args[i].flags & flags)
      count++;

  return count;
}

static void print_args(const fn_t *fn, fn_arg_flags_t flags) {
  bool first = true;

  fputs(BOLD "(" RESET, stdout);
  for (unsigned i = 0; i < fn->count; i++) {
    const fn_arg_t *arg = &fn->args[i];
    if (arg->flags & flags) {
      if (first)
        first = false;
      else
        fputs(BOLD "," RESET " ", stdout);
      fputs(arg->type ? arg->type->name : "?", stdout);
    }
  }
  fputs(BOLD ")" RESET, stdout);
}

void fn_sig_print(const fn_t *fn) {
  print_args(fn, ARG_INPUT);
  fputs(BOLD " ↦ " RESET, stdout);
  print_args(fn, ARG_OUTPUT);
}
