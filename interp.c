#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <ffi.h>

#include "config.h"
#include "word.h"
#include "interp.h"
#include "ansi.h"

cell_t *stack_get_nth(cell_list_t *stack, unsigned n) {
  if (TAILQ_EMPTY(stack))
    return NULL;

  cell_t *c;
  TAILQ_FOREACH_REVERSE(c, stack, cell_list, list) {
    if (n == 0)
      return c;
    n--;
  }

  return NULL;
}

static tpmi_status_t eval_word(tpmi_t *interp, entry_t *entry);

static tpmi_status_t eval_cell(tpmi_t *interp, cell_t *c) {
  if (c->type == CT_ATOM) {
    entry_t *entry = dict_find(interp->words, (char *)c->ptr);

    if (entry->word == NULL) {
      ERROR(interp, "unknown identifier: '%s'", c->atom);
      return TPMI_ERROR;
    }

    if (entry->word->type != WT_VAR)
      return eval_word(interp, entry);
  }

  STACK_PUSH(&interp->stack, cell_copy(c));
  return TPMI_OK;
}

typedef struct arg_info {
  unsigned args;
  cell_t *first;
} arg_info_t;

static bool check_func_args(tpmi_t *interp, entry_t *entry, arg_info_t *ai) {
  word_t *word = entry->word;
  unsigned args = fn_arg_count(word->func, ARG_INPUT);

  ai->args = args;

  if (args == 0)
    return true;

  cell_t *stk_arg = stack_get_nth(&interp->stack, args - 1);

  if (stk_arg == NULL) {
    ERROR(interp, "'%s' expected %u args, but stack has %u elements",
          entry->key, args, clist_length(&interp->stack));
    return false;
  }

  ai->first = stk_arg;

  /* check arguments */
  unsigned i = 0;

  for (const fn_arg_t *arg = word->func->args; arg->flags; arg++) {
    if (arg->flags & ARG_INPUT) {
      if ((arg->type != NULL) && (arg->type != stk_arg->type)) {
        ERROR(interp, "'%s' argument %u type mismatch - expected "
              BOLD "%s" RESET ", got " BOLD "%s" RESET,
              entry->key, i, arg->type->name, stk_arg->type->name);
        return false;
      }
      stk_arg = CELL_NEXT(stk_arg);
      i++;
    }
  }

  return true;
}

static tpmi_status_t eval_word(tpmi_t *interp, entry_t *entry) {
  word_t *word = entry->word;

  if (word->type == WT_BUILTIN) {
    arg_info_t ai;
    if (!check_func_args(interp, entry, &ai))
      return TPMI_ERROR;
    return ((tpmi_fn_t)word->func->fn)(interp);
  }

  if (word->type == WT_DEF) {
    cell_t *c;
    tpmi_status_t status = TPMI_OK;
    TAILQ_FOREACH(c, &word->def, list)
      if (!(status = eval_cell(interp, c)))
        break;
    return status;
  }
  
  if (word->type == WT_CFUNC) {
    arg_info_t ai;

    if (!check_func_args(interp, entry, &ai))
      return TPMI_ERROR;

    unsigned n = fn_arg_count(word->func, ARG_INPUT|ARG_OUTPUT);

    /* construct a call */
    ffi_type *arg_ctype[n];
    void *arg_value[n];

    cell_t *arg = ai.first;

    for (int i = 0; i < n; i++) {
      const fn_arg_t *fn_arg = &word->func->args[i];

      if (fn_arg->flags & ARG_INPUT) {
        if (fn_arg->type == CT_INT) {
          arg_ctype[i] = &ffi_type_sint;
          arg_value[i] = &arg->i;
        } else if (fn_arg->type == CT_FLOAT) {
          arg_ctype[i] = &ffi_type_float;
          arg_value[i] = &arg->f;
        } else {
          arg_ctype[i] = &ffi_type_pointer;
          arg_value[i] = &arg->ptr;
        }

        arg = CELL_NEXT(arg);
      } else {
        /* pass output arguments, but firstly push them on top of stack */
        cell_t *c;

        arg_ctype[i] = &ffi_type_pointer;

        if (fn_arg->type == CT_INT) {
          c = cell_int(0);
          arg_value[i] = &c->i;
        } else if (fn_arg->type == CT_FLOAT) {
          c = cell_float(0.0);
          arg_value[i] = &c->f;
        } else if (fn_arg->type == CT_MONO) {
          c = cell_mono();
          arg_value[i] = &c->ptr;
        } else if (fn_arg->type == CT_COLOR) {
          c = cell_color();
          arg_value[i] = &c->ptr;
        } else {
          abort();
        }

        STACK_PUSH(&interp->stack, c);
      }
    }

    ffi_cif cif;
    ffi_arg result;

    assert(ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
                        n, &ffi_type_void, arg_ctype) == FFI_OK);

    ffi_call(&cif, FFI_FN(word->func->fn), &result, arg_value);

    /* remove input arguments from stack */
    arg = ai.first;

    for (unsigned i = 0; i < ai.args; i++) {
      cell_t *c = arg;
      arg = CELL_NEXT(arg);
      CELL_REMOVE(&interp->stack, c);
      cell_delete(c);
    }

    return TPMI_OK;
  }

  abort();
}

extern void tpmi_init(tpmi_t *interp);

tpmi_t *tpmi_new() {
  tpmi_t *interp = calloc(1, sizeof(tpmi_t));
  TAILQ_INIT(&interp->stack);
  interp->words = dict_new();

  tpmi_init(interp);

  return interp;
}

void tpmi_delete(tpmi_t *interp) {
  clist_reset(&interp->stack);
  dict_delete(interp->words);
  free(interp);
}

static bool read_float(const char *str, unsigned span, float *f) {
  char *end;
  *f = strtof(str, &end);
  return end == str + span;
}

static bool read_int(const char *str, unsigned span, int *i) {
  char *end;
  bool hex = (str[0] == '0' && tolower(str[1]) == 'x');
  *i = strtol(hex ? str + 2 : str, &end, hex ? 16 : 10);
  return end == str + span;
}

static cell_t make_cell(const char *line, unsigned span) {
  int i; float f;

  if (read_int(line, span, &i))
    return (cell_t){CT_INT, {.i = i}};
  if (read_float(line, span, &f))
    return (cell_t){CT_FLOAT, {.f = f}};
  if (line[0] == '"' && line[span - 1] == '"')
    return (cell_t){CT_STRING, {.ptr = strndup(line + 1, span - 2)}};
  return (cell_t){CT_ATOM, {.ptr = strndup(line, span)}};
}

tpmi_status_t tpmi_compile(tpmi_t *interp, const char *line) {
  tpmi_status_t status = TPMI_OK;
  unsigned n = 0;

  while (true) {
    /* skip spaces */
    line += strspn(line, " \t\n");

    /* find token */
    unsigned len;

    if (*line == '"') {
      char *closing = strchr(line + 1, '"');

      if (closing == NULL) {
        ERROR(interp, "missing closing quote character");
        status = TPMI_ERROR;
        goto error;
      }

      len = closing + 1 - line;
    } else {
      len = strcspn(line, " \t\n");
    }

    if (len == 0)
      break;

    /* parse token */
    cell_t c = make_cell(line, len);

    switch (*interp->mode) {
      case TPMI_EVAL: 
        /* evaluation mode */
        status = TPMI_NEED_MORE;

        if (strncmp(line, ":", len) == 0) {
          *interp->mode = TPMI_COMPILE;
          interp->curr_word = NULL;
        } else if (strncmp(line, "'", len) == 0)
          *interp->mode = TPMI_FUNCREF;
        else if (strncasecmp(line, "variable", len) == 0)
          *interp->mode = TPMI_DEFVAR;
        else if (strncasecmp(line, "immediate", len) == 0) {
          if (interp->curr_word != NULL)
            interp->curr_word->immediate = true;
          status = TPMI_OK;
        } else
          status = eval_cell(interp, &c);
        break;

      case TPMI_COMPILE:
        /* compilation mode */

        if (strncmp(line, ";", len) == 0) {
          *interp->mode = TPMI_EVAL;
          status = TPMI_OK;
        } else if (interp->curr_word == NULL) {
          if (c.type == CT_ATOM) {
            entry_t *entry = dict_add(interp->words, c.atom); 

            if (entry->word == NULL) {
              word_t *word = calloc(1, sizeof(word_t));
              word->type = WT_DEF;
              word->immediate = false;
              TAILQ_INIT(&word->def);
              interp->curr_word = entry->word = word;
              status = TPMI_NEED_MORE;
            } else {
              ERROR(interp, "word '%s' has been already defined", c.atom);
              status = TPMI_ERROR;
            }
          } else {
            ERROR(interp, "expected word name");
            status = TPMI_ERROR;
          }
        } else {
          if (c.type == CT_ATOM && 
              dict_add(interp->words, c.atom)->word->immediate) {
            status = eval_cell(interp, &c);
          } else {
            STACK_PUSH(&interp->curr_word->def, cell_copy(&c));
            status = TPMI_NEED_MORE;
          }
        }
        break;

      case TPMI_DEFVAR:
        *interp->mode = TPMI_EVAL;

        if (c.type == CT_ATOM) {
          dict_add(interp->words, c.atom)->word->type = WT_VAR;
          status = TPMI_OK;
        } else {
          ERROR(interp, "'variable' expects name");
          status = TPMI_ERROR;
        }
        break;

      case TPMI_FUNCREF:
        *interp->mode = TPMI_EVAL;
        status = TPMI_ERROR;

        if (c.type == CT_ATOM)
          if (dict_add(interp->words, c.atom)->word->type == WT_CFUNC) {
            STACK_PUSH(&interp->stack, cell_copy(&c));
            status = TPMI_OK;
          }

        if (status == TPMI_ERROR)
          ERROR(interp, "'tick' expects C function name");
        break;
    }

    line += len;

    if ((c.type != NULL) && (c.type->delete != NULL))
      c.type->delete(&c);

error:

    if (status == TPMI_ERROR) {
      fprintf(stderr, RED "failure at token %u\n" RESET, n + 1);
      fprintf(stderr, RED "error: " RESET "%s\n", interp->errmsg);
      break;
    }
  }

  return status;
}
