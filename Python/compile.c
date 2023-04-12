/*
 * This file compiles an abstract syntax tree (AST) into Python bytecode.
 *
 * The primary entry point is _PyAST_Compile(), which returns a
 * PyCodeObject.  The compiler makes several passes to build the code
 * object:
 *   1. Checks for future statements.  See future.c
 *   2. Builds a symbol table.  See symtable.c.
 *   3. Generate code for basic blocks.  See compiler_mod() in this file.
 *   4. Assemble the basic blocks into final code.  See assemble() in
 *      this file.
 *   5. Optimize the byte code (peephole optimizations).
 *
 * Note that compiler_mod() suggests module, but the module ast type
 * (mod_ty) has cases for expressions and interactive statements.
 *
 * CAUTION: The VISIT_* macros abort the current function when they
 * encounter a problem. So don't invoke them when there is memory
 * which needs to be released. Code blocks are OK, as the compiler
 * structure takes care of releasing those.  Use the arena to manage
 * objects.
 */

#include <stdbool.h>

// Need _PyOpcode_RelativeJump of pycore_opcode.h
#define NEED_OPCODE_TABLES

#include "Python.h"
#include "pycore_ast.h"           // _PyAST_GetDocString()
#include "pycore_code.h"          // _PyCode_New()
#include "pycore_compile.h"       // _PyFuture_FromAST()
#include "pycore_intrinsics.h"
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_opcode.h"        // _PyOpcode_Caches
#include "pycore_pymem.h"         // _PyMem_IsPtrFreed()
#include "pycore_symtable.h"      // PySTEntryObject

#include "opcode_metadata.h"      // _PyOpcode_opcode_metadata


#define DEFAULT_BLOCK_SIZE 16
#define DEFAULT_CODE_SIZE 128
#define DEFAULT_LNOTAB_SIZE 16
#define DEFAULT_CNOTAB_SIZE 32
#define DEFAULT_PROFILE_TABLE_SIZE 32

#define COMP_GENEXP   0
#define COMP_LISTCOMP 1
#define COMP_SETCOMP  2
#define COMP_DICTCOMP 3

/* A soft limit for stack use, to avoid excessive
 * memory use for large constants, etc.
 *
 * The value 30 is plucked out of thin air.
 * Code that could use more stack than this is
 * rare, so the exact value is unimportant.
 */
#define STACK_USE_GUIDELINE 30

#undef SUCCESS
#undef ERROR
#define SUCCESS 0
#define ERROR -1

#define RETURN_IF_ERROR(X)  \
    if ((X) == -1) {        \
        return ERROR;       \
    }

/* If we exceed this limit, it should
 * be considered a compiler bug.
 * Currently it should be impossible
 * to exceed STACK_USE_GUIDELINE * 100,
 * as 100 is the maximum parse depth.
 * For performance reasons we will
 * want to reduce this to a
 * few hundred in the future.
 *
 * NOTE: Whatever MAX_ALLOWED_STACK_USE is
 * set to, it should never restrict what Python
 * we can write, just how we compile it.
 */
#define MAX_ALLOWED_STACK_USE (STACK_USE_GUIDELINE * 100)


#define MAX_REAL_OPCODE 254

#define IS_WITHIN_OPCODE_RANGE(opcode) \
        (((opcode) >= 0 && (opcode) <= MAX_REAL_OPCODE) || \
         IS_PSEUDO_OPCODE(opcode))

#define IS_JUMP_OPCODE(opcode) \
         is_bit_set_in_table(_PyOpcode_Jump, opcode)

#define IS_BLOCK_PUSH_OPCODE(opcode) \
        ((opcode) == SETUP_FINALLY || \
         (opcode) == SETUP_WITH || \
         (opcode) == SETUP_CLEANUP)

#define HAS_TARGET(opcode) \
        (IS_JUMP_OPCODE(opcode) || IS_BLOCK_PUSH_OPCODE(opcode))

/* opcodes that must be last in the basicblock */
#define IS_TERMINATOR_OPCODE(opcode) \
        (IS_JUMP_OPCODE(opcode) || IS_SCOPE_EXIT_OPCODE(opcode))

/* opcodes which are not emitted in codegen stage, only by the assembler */
#define IS_ASSEMBLER_OPCODE(opcode) \
        ((opcode) == JUMP_FORWARD || \
         (opcode) == JUMP_BACKWARD || \
         (opcode) == JUMP_BACKWARD_NO_INTERRUPT)

#define IS_BACKWARDS_JUMP_OPCODE(opcode) \
        ((opcode) == JUMP_BACKWARD || \
         (opcode) == JUMP_BACKWARD_NO_INTERRUPT)

#define IS_UNCONDITIONAL_JUMP_OPCODE(opcode) \
        ((opcode) == JUMP || \
         (opcode) == JUMP_NO_INTERRUPT || \
         (opcode) == JUMP_FORWARD || \
         (opcode) == JUMP_BACKWARD || \
         (opcode) == JUMP_BACKWARD_NO_INTERRUPT)

#define IS_SCOPE_EXIT_OPCODE(opcode) \
        ((opcode) == RETURN_VALUE || \
         (opcode) == RAISE_VARARGS || \
         (opcode) == RERAISE)

#define IS_SUPERINSTRUCTION_OPCODE(opcode) \
        ((opcode) == LOAD_FAST__LOAD_FAST || \
         (opcode) == LOAD_FAST__LOAD_CONST || \
         (opcode) == LOAD_CONST__LOAD_FAST || \
         (opcode) == STORE_FAST__LOAD_FAST || \
         (opcode) == STORE_FAST__STORE_FAST)

#define IS_TOP_LEVEL_AWAIT(C) ( \
        ((C)->c_flags.cf_flags & PyCF_ALLOW_TOP_LEVEL_AWAIT) \
        && ((C)->u->u_ste->ste_type == ModuleBlock))

typedef _PyCompilerSrcLocation location;

#define LOCATION(LNO, END_LNO, COL, END_COL) \
    ((const location){(LNO), (END_LNO), (COL), (END_COL)})

static location NO_LOCATION = {-1, -1, -1, -1};

/* Return true if loc1 starts after loc2 ends. */
static inline bool
location_is_after(location loc1, location loc2) {
    return (loc1.lineno > loc2.end_lineno) ||
            ((loc1.lineno == loc2.end_lineno) &&
             (loc1.col_offset > loc2.end_col_offset));
}

static inline bool
same_location(location a, location b)
{
    return a.lineno == b.lineno &&
           a.end_lineno == b.end_lineno &&
           a.col_offset == b.col_offset &&
           a.end_col_offset == b.end_col_offset;
}

#define LOC(x) SRC_LOCATION_FROM_AST(x)

typedef struct jump_target_label_ {
    int id;
} jump_target_label;

static struct jump_target_label_ NO_LABEL = {-1};

#define SAME_LABEL(L1, L2) ((L1).id == (L2).id)
#define IS_LABEL(L) (!SAME_LABEL((L), (NO_LABEL)))

#define NEW_JUMP_TARGET_LABEL(C, NAME) \
    jump_target_label NAME = cfg_new_label(CFG_BUILDER(C)); \
    if (!IS_LABEL(NAME)) { \
        return 0; \
    }

#define USE_LABEL(C, LBL) \
    if (cfg_builder_use_label(CFG_BUILDER(C), LBL) < 0) { \
        return 0; \
    }

struct instr {
    int i_opcode;
    int i_oparg;
    location i_loc;
    /* The following fields should not be set by the front-end: */
    struct basicblock_ *i_target; /* target block (if jump instruction) */
    struct basicblock_ *i_except; /* target block when exception is raised */
};

/* One arg*/
#define INSTR_SET_OP1(I, OP, ARG) \
    do { \
        assert(HAS_ARG(OP)); \
        struct instr *_instr__ptr_ = (I); \
        _instr__ptr_->i_opcode = (OP); \
        _instr__ptr_->i_oparg = (ARG); \
    } while (0);

/* No args*/
#define INSTR_SET_OP0(I, OP) \
    do { \
        assert(!HAS_ARG(OP)); \
        struct instr *_instr__ptr_ = (I); \
        _instr__ptr_->i_opcode = (OP); \
        _instr__ptr_->i_oparg = 0; \
    } while (0);

typedef struct exceptstack {
    struct basicblock_ *handlers[CO_MAXBLOCKS+1];
    int depth;
} ExceptStack;

#define LOG_BITS_PER_INT 5
#define MASK_LOW_LOG_BITS 31

static inline int
is_bit_set_in_table(const uint32_t *table, int bitindex) {
    /* Is the relevant bit set in the relevant word? */
    /* 512 bits fit into 9 32-bits words.
     * Word is indexed by (bitindex>>ln(size of int in bits)).
     * Bit within word is the low bits of bitindex.
     */
    if (bitindex >= 0 && bitindex < 512) {
        uint32_t word = table[bitindex >> LOG_BITS_PER_INT];
        return (word >> (bitindex & MASK_LOW_LOG_BITS)) & 1;
    }
    else {
        return 0;
    }
}

static inline int
is_relative_jump(struct instr *i)
{
    return is_bit_set_in_table(_PyOpcode_RelativeJump, i->i_opcode);
}

static inline int
is_block_push(struct instr *i)
{
    return IS_BLOCK_PUSH_OPCODE(i->i_opcode);
}

static inline int
is_jump(struct instr *i)
{
    return IS_JUMP_OPCODE(i->i_opcode);
}

static int
instr_size(struct instr *instruction)
{
    int opcode = instruction->i_opcode;
    assert(!IS_PSEUDO_OPCODE(opcode));
    int oparg = instruction->i_oparg;
    assert(HAS_ARG(opcode) || oparg == 0);
    int extended_args = (0xFFFFFF < oparg) + (0xFFFF < oparg) + (0xFF < oparg);
    int caches = _PyOpcode_Caches[opcode];
    return extended_args + 1 + caches;
}

static void
write_instr(_Py_CODEUNIT *codestr, struct instr *instruction, int ilen)
{
    int opcode = instruction->i_opcode;
    assert(!IS_PSEUDO_OPCODE(opcode));
    int oparg = instruction->i_oparg;
    assert(HAS_ARG(opcode) || oparg == 0);
    int caches = _PyOpcode_Caches[opcode];
    switch (ilen - caches) {
        case 4:
            codestr->opcode = EXTENDED_ARG;
            codestr->oparg = (oparg >> 24) & 0xFF;
            codestr++;
            /* fall through */
        case 3:
            codestr->opcode = EXTENDED_ARG;
            codestr->oparg = (oparg >> 16) & 0xFF;
            codestr++;
            /* fall through */
        case 2:
            codestr->opcode = EXTENDED_ARG;
            codestr->oparg = (oparg >> 8) & 0xFF;
            codestr++;
            /* fall through */
        case 1:
            codestr->opcode = opcode;
            codestr->oparg = oparg & 0xFF;
            codestr++;
            break;
        default:
            Py_UNREACHABLE();
    }
    while (caches--) {
        codestr->opcode = CACHE;
        codestr->oparg = 0;
        codestr++;
    }
}

typedef struct basicblock_ {
    /* Each basicblock in a compilation unit is linked via b_list in the
       reverse order that the block are allocated.  b_list points to the next
       block, not to be confused with b_next, which is next by control flow. */
    struct basicblock_ *b_list;
    /* The label of this block if it is a jump target, -1 otherwise */
    int b_label;
    /* Exception stack at start of block, used by assembler to create the exception handling table */
    ExceptStack *b_exceptstack;
    /* pointer to an array of instructions, initially NULL */
    struct instr *b_instr;
    /* If b_next is non-NULL, it is a pointer to the next
       block reached by normal control flow. */
    struct basicblock_ *b_next;
    /* number of instructions used */
    int b_iused;
    /* length of instruction array (b_instr) */
    int b_ialloc;
    /* Used by add_checks_for_loads_of_unknown_variables */
    uint64_t b_unsafe_locals_mask;
    /* Number of predecessors that a block has. */
    int b_predecessors;
    /* depth of stack upon entry of block, computed by stackdepth() */
    int b_startdepth;
    /* instruction offset for block, computed by assemble_jump_offsets() */
    int b_offset;
    /* Basic block is an exception handler that preserves lasti */
    unsigned b_preserve_lasti : 1;
    /* Used by compiler passes to mark whether they have visited a basic block. */
    unsigned b_visited : 1;
    /* b_except_handler is used by the cold-detection algorithm to mark exception targets */
    unsigned b_except_handler : 1;
    /* b_cold is true if this block is not perf critical (like an exception handler) */
    unsigned b_cold : 1;
    /* b_warm is used by the cold-detection algorithm to mark blocks which are definitely not cold */
    unsigned b_warm : 1;
} basicblock;


static struct instr *
basicblock_last_instr(const basicblock *b) {
    assert(b->b_iused >= 0);
    if (b->b_iused > 0) {
        assert(b->b_instr != NULL);
        return &b->b_instr[b->b_iused - 1];
    }
    return NULL;
}

static inline int
basicblock_returns(const basicblock *b) {
    struct instr *last = basicblock_last_instr(b);
    return last && last->i_opcode == RETURN_VALUE;
}

static inline int
basicblock_exits_scope(const basicblock *b) {
    struct instr *last = basicblock_last_instr(b);
    return last && IS_SCOPE_EXIT_OPCODE(last->i_opcode);
}

static inline int
basicblock_nofallthrough(const basicblock *b) {
    struct instr *last = basicblock_last_instr(b);
    return (last &&
            (IS_SCOPE_EXIT_OPCODE(last->i_opcode) ||
             IS_UNCONDITIONAL_JUMP_OPCODE(last->i_opcode)));
}

#define BB_NO_FALLTHROUGH(B) (basicblock_nofallthrough(B))
#define BB_HAS_FALLTHROUGH(B) (!basicblock_nofallthrough(B))

/* fblockinfo tracks the current frame block.

A frame block is used to handle loops, try/except, and try/finally.
It's called a frame block to distinguish it from a basic block in the
compiler IR.
*/

enum fblocktype { WHILE_LOOP, FOR_LOOP, TRY_EXCEPT, FINALLY_TRY, FINALLY_END,
                  WITH, ASYNC_WITH, HANDLER_CLEANUP, POP_VALUE, EXCEPTION_HANDLER,
                  EXCEPTION_GROUP_HANDLER, ASYNC_COMPREHENSION_GENERATOR };

struct fblockinfo {
    enum fblocktype fb_type;
    jump_target_label fb_block;
    /* (optional) type-specific exit or cleanup block */
    jump_target_label fb_exit;
    /* (optional) additional information required for unwinding */
    void *fb_datum;
};

enum {
    COMPILER_SCOPE_MODULE,
    COMPILER_SCOPE_CLASS,
    COMPILER_SCOPE_FUNCTION,
    COMPILER_SCOPE_ASYNC_FUNCTION,
    COMPILER_SCOPE_LAMBDA,
    COMPILER_SCOPE_COMPREHENSION,
};

typedef struct cfg_builder_ {
    /* The entryblock, at which control flow begins. All blocks of the
       CFG are reachable through the b_next links */
    basicblock *g_entryblock;
    /* Pointer to the most recently allocated block.  By following
       b_list links, you can reach all allocated blocks. */
    basicblock *g_block_list;
    /* pointer to the block currently being constructed */
    basicblock *g_curblock;
    /* label for the next instruction to be placed */
    jump_target_label g_current_label;
    /* next free label id */
    int g_next_free_label;
} cfg_builder;

/* The following items change on entry and exit of code blocks.
   They must be saved and restored when returning to a block.
*/
struct compiler_unit {
    PySTEntryObject *u_ste;

    PyObject *u_name;
    PyObject *u_qualname;  /* dot-separated qualified name (lazy) */
    int u_scope_type;

    /* The following fields are dicts that map objects to
       the index of them in co_XXX.      The index is used as
       the argument for opcodes that refer to those collections.
    */
    PyObject *u_consts;    /* all constants */
    PyObject *u_names;     /* all names */
    PyObject *u_varnames;  /* local variables */
    PyObject *u_cellvars;  /* cell variables */
    PyObject *u_freevars;  /* free variables */

    PyObject *u_private;        /* for private name mangling */

    Py_ssize_t u_argcount;        /* number of arguments for block */
    Py_ssize_t u_posonlyargcount;        /* number of positional only arguments for block */
    Py_ssize_t u_kwonlyargcount; /* number of keyword only arguments for block */

    cfg_builder u_cfg_builder;  /* The control flow graph */

    int u_nfblocks;
    struct fblockinfo u_fblock[CO_MAXBLOCKS];

    int u_firstlineno; /* the first lineno of the block */
};

/* This struct captures the global state of a compilation.

The u pointer points to the current compilation unit, while units
for enclosing blocks are stored in c_stack.     The u and c_stack are
managed by compiler_enter_scope() and compiler_exit_scope().

Note that we don't track recursion levels during compilation - the
task of detecting and rejecting excessive levels of nesting is
handled by the symbol analysis pass.

*/

struct compiler {
    PyObject *c_filename;
    struct symtable *c_st;
    PyFutureFeatures c_future;   /* module's __future__ */
    PyCompilerFlags c_flags;

    int c_optimize;              /* optimization level */
    int c_interactive;           /* true if in interactive mode */
    int c_nestlevel;
    PyObject *c_const_cache;     /* Python dict holding all constants,
                                    including names tuple */
    struct compiler_unit *u; /* compiler state for current block */
    PyObject *c_stack;           /* Python list holding compiler_unit ptrs */
    PyArena *c_arena;            /* pointer to memory allocation arena */
};

#define CFG_BUILDER(C) (&((C)->u->u_cfg_builder))


typedef struct {
    // A list of strings corresponding to name captures. It is used to track:
    // - Repeated name assignments in the same pattern.
    // - Different name assignments in alternatives.
    // - The order of name assignments in alternatives.
    PyObject *stores;
    // If 0, any name captures against our subject will raise.
    int allow_irrefutable;
    // An array of blocks to jump to on failure. Jumping to fail_pop[i] will pop
    // i items off of the stack. The end result looks like this (with each block
    // falling through to the next):
    // fail_pop[4]: POP_TOP
    // fail_pop[3]: POP_TOP
    // fail_pop[2]: POP_TOP
    // fail_pop[1]: POP_TOP
    // fail_pop[0]: NOP
    jump_target_label *fail_pop;
    // The current length of fail_pop.
    Py_ssize_t fail_pop_size;
    // The number of items on top of the stack that need to *stay* on top of the
    // stack. Variable captures go beneath these. All of them will be popped on
    // failure.
    Py_ssize_t on_top;
} pattern_context;

static int basicblock_next_instr(basicblock *);

static basicblock *cfg_builder_new_block(cfg_builder *g);
static int cfg_builder_maybe_start_new_block(cfg_builder *g);
static int cfg_builder_addop_i(cfg_builder *g, int opcode, Py_ssize_t oparg, location loc);

static void compiler_free(struct compiler *);
static int compiler_error(struct compiler *, location loc, const char *, ...);
static int compiler_warn(struct compiler *, location loc, const char *, ...);
static int compiler_nameop(struct compiler *, location, identifier, expr_context_ty);

static PyCodeObject *compiler_mod(struct compiler *, mod_ty);
static int compiler_visit_stmt(struct compiler *, stmt_ty);
static int compiler_visit_keyword(struct compiler *, keyword_ty);
static int compiler_visit_expr(struct compiler *, expr_ty);
static int compiler_augassign(struct compiler *, stmt_ty);
static int compiler_annassign(struct compiler *, stmt_ty);
static int compiler_subscript(struct compiler *, expr_ty);
static int compiler_slice(struct compiler *, expr_ty);

static bool are_all_items_const(asdl_expr_seq *, Py_ssize_t, Py_ssize_t);


static int compiler_with(struct compiler *, stmt_ty, int);
static int compiler_async_with(struct compiler *, stmt_ty, int);
static int compiler_async_for(struct compiler *, stmt_ty);
static int compiler_call_simple_kw_helper(struct compiler *c,
                                          location loc,
                                          asdl_keyword_seq *keywords,
                                          Py_ssize_t nkwelts);
static int compiler_call_helper(struct compiler *c, location loc,
                                int n, asdl_expr_seq *args,
                                asdl_keyword_seq *keywords);
static int compiler_try_except(struct compiler *, stmt_ty);
static int compiler_try_star_except(struct compiler *, stmt_ty);
static int compiler_set_qualname(struct compiler *);

static int compiler_sync_comprehension_generator(
                                      struct compiler *c, location loc,
                                      asdl_comprehension_seq *generators, int gen_index,
                                      int depth,
                                      expr_ty elt, expr_ty val, int type);

static int compiler_async_comprehension_generator(
                                      struct compiler *c, location loc,
                                      asdl_comprehension_seq *generators, int gen_index,
                                      int depth,
                                      expr_ty elt, expr_ty val, int type);

static int compiler_pattern(struct compiler *, pattern_ty, pattern_context *);
static int compiler_match(struct compiler *, stmt_ty);
static int compiler_pattern_subpattern(struct compiler *,
                                       pattern_ty, pattern_context *);

static int remove_redundant_nops(basicblock *bb);

static PyCodeObject *assemble(struct compiler *, int addNone);

#define CAPSULE_NAME "compile.c compiler unit"

PyObject *
_Py_Mangle(PyObject *privateobj, PyObject *ident)
{
    /* Name mangling: __private becomes _classname__private.
       This is independent from how the name is used. */
    PyObject *result;
    size_t nlen, plen, ipriv;
    Py_UCS4 maxchar;
    if (privateobj == NULL || !PyUnicode_Check(privateobj) ||
        PyUnicode_READ_CHAR(ident, 0) != '_' ||
        PyUnicode_READ_CHAR(ident, 1) != '_') {
        return Py_NewRef(ident);
    }
    nlen = PyUnicode_GET_LENGTH(ident);
    plen = PyUnicode_GET_LENGTH(privateobj);
    /* Don't mangle __id__ or names with dots.

       The only time a name with a dot can occur is when
       we are compiling an import statement that has a
       package name.

       TODO(jhylton): Decide whether we want to support
       mangling of the module name, e.g. __M.X.
    */
    if ((PyUnicode_READ_CHAR(ident, nlen-1) == '_' &&
         PyUnicode_READ_CHAR(ident, nlen-2) == '_') ||
        PyUnicode_FindChar(ident, '.', 0, nlen, 1) != -1) {
        return Py_NewRef(ident); /* Don't mangle __whatever__ */
    }
    /* Strip leading underscores from class name */
    ipriv = 0;
    while (PyUnicode_READ_CHAR(privateobj, ipriv) == '_')
        ipriv++;
    if (ipriv == plen) {
        return Py_NewRef(ident); /* Don't mangle if class is just underscores */
    }
    plen -= ipriv;

    if (plen + nlen >= PY_SSIZE_T_MAX - 1) {
        PyErr_SetString(PyExc_OverflowError,
                        "private identifier too large to be mangled");
        return NULL;
    }

    maxchar = PyUnicode_MAX_CHAR_VALUE(ident);
    if (PyUnicode_MAX_CHAR_VALUE(privateobj) > maxchar)
        maxchar = PyUnicode_MAX_CHAR_VALUE(privateobj);

    result = PyUnicode_New(1 + nlen + plen, maxchar);
    if (!result)
        return 0;
    /* ident = "_" + priv[ipriv:] + ident # i.e. 1+plen+nlen bytes */
    PyUnicode_WRITE(PyUnicode_KIND(result), PyUnicode_DATA(result), 0, '_');
    if (PyUnicode_CopyCharacters(result, 1, privateobj, ipriv, plen) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    if (PyUnicode_CopyCharacters(result, plen+1, ident, 0, nlen) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    assert(_PyUnicode_CheckConsistency(result, 1));
    return result;
}


static int
compiler_setup(struct compiler *c, mod_ty mod, PyObject *filename,
               PyCompilerFlags flags, int optimize, PyArena *arena)
{
    c->c_const_cache = PyDict_New();
    if (!c->c_const_cache) {
        return ERROR;
    }

    c->c_stack = PyList_New(0);
    if (!c->c_stack) {
        return ERROR;
    }

    c->c_filename = Py_NewRef(filename);
    c->c_arena = arena;
    if (!_PyFuture_FromAST(mod, filename, &c->c_future)) {
        return ERROR;
    }
    int merged = c->c_future.ff_features | flags.cf_flags;
    c->c_future.ff_features = merged;
    flags.cf_flags = merged;
    c->c_flags = flags;
    c->c_optimize = (optimize == -1) ? _Py_GetConfig()->optimization_level : optimize;
    c->c_nestlevel = 0;

    _PyASTOptimizeState state;
    state.optimize = c->c_optimize;
    state.ff_features = merged;

    if (!_PyAST_Optimize(mod, arena, &state)) {
        return ERROR;
    }
    c->c_st = _PySymtable_Build(mod, filename, &c->c_future);
    if (c->c_st == NULL) {
        if (!PyErr_Occurred()) {
            PyErr_SetString(PyExc_SystemError, "no symtable");
        }
        return ERROR;
    }
    return SUCCESS;
}

static struct compiler*
new_compiler(mod_ty mod, PyObject *filename, PyCompilerFlags *pflags,
             int optimize, PyArena *arena)
{
    PyCompilerFlags flags = pflags ? *pflags : _PyCompilerFlags_INIT;
    struct compiler *c = PyMem_Calloc(1, sizeof(struct compiler));
    if (c == NULL) {
        return NULL;
    }
    if (compiler_setup(c, mod, filename, flags, optimize, arena) < 0) {
        compiler_free(c);
        return NULL;
    }
    return c;
}

PyCodeObject *
_PyAST_Compile(mod_ty mod, PyObject *filename, PyCompilerFlags *pflags,
               int optimize, PyArena *arena)
{
    struct compiler *c = new_compiler(mod, filename, pflags, optimize, arena);
    if (c == NULL) {
        return NULL;
    }

    PyCodeObject *co = compiler_mod(c, mod);
    compiler_free(c);
    assert(co || PyErr_Occurred());
    return co;
}

static void
compiler_free(struct compiler *c)
{
    if (c->c_st)
        _PySymtable_Free(c->c_st);
    Py_XDECREF(c->c_filename);
    Py_XDECREF(c->c_const_cache);
    Py_XDECREF(c->c_stack);
    PyMem_Free(c);
}

static PyObject *
list2dict(PyObject *list)
{
    Py_ssize_t i, n;
    PyObject *v, *k;
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    n = PyList_Size(list);
    for (i = 0; i < n; i++) {
        v = PyLong_FromSsize_t(i);
        if (!v) {
            Py_DECREF(dict);
            return NULL;
        }
        k = PyList_GET_ITEM(list, i);
        if (PyDict_SetItem(dict, k, v) < 0) {
            Py_DECREF(v);
            Py_DECREF(dict);
            return NULL;
        }
        Py_DECREF(v);
    }
    return dict;
}

/* Return new dict containing names from src that match scope(s).

src is a symbol table dictionary.  If the scope of a name matches
either scope_type or flag is set, insert it into the new dict.  The
values are integers, starting at offset and increasing by one for
each key.
*/

static PyObject *
dictbytype(PyObject *src, int scope_type, int flag, Py_ssize_t offset)
{
    Py_ssize_t i = offset, scope, num_keys, key_i;
    PyObject *k, *v, *dest = PyDict_New();
    PyObject *sorted_keys;

    assert(offset >= 0);
    if (dest == NULL)
        return NULL;

    /* Sort the keys so that we have a deterministic order on the indexes
       saved in the returned dictionary.  These indexes are used as indexes
       into the free and cell var storage.  Therefore if they aren't
       deterministic, then the generated bytecode is not deterministic.
    */
    sorted_keys = PyDict_Keys(src);
    if (sorted_keys == NULL)
        return NULL;
    if (PyList_Sort(sorted_keys) != 0) {
        Py_DECREF(sorted_keys);
        return NULL;
    }
    num_keys = PyList_GET_SIZE(sorted_keys);

    for (key_i = 0; key_i < num_keys; key_i++) {
        /* XXX this should probably be a macro in symtable.h */
        long vi;
        k = PyList_GET_ITEM(sorted_keys, key_i);
        v = PyDict_GetItemWithError(src, k);
        assert(v && PyLong_Check(v));
        vi = PyLong_AS_LONG(v);
        scope = (vi >> SCOPE_OFFSET) & SCOPE_MASK;

        if (scope == scope_type || vi & flag) {
            PyObject *item = PyLong_FromSsize_t(i);
            if (item == NULL) {
                Py_DECREF(sorted_keys);
                Py_DECREF(dest);
                return NULL;
            }
            i++;
            if (PyDict_SetItem(dest, k, item) < 0) {
                Py_DECREF(sorted_keys);
                Py_DECREF(item);
                Py_DECREF(dest);
                return NULL;
            }
            Py_DECREF(item);
        }
    }
    Py_DECREF(sorted_keys);
    return dest;
}

static void
cfg_builder_check(cfg_builder *g)
{
    for (basicblock *block = g->g_block_list; block != NULL; block = block->b_list) {
        assert(!_PyMem_IsPtrFreed(block));
        if (block->b_instr != NULL) {
            assert(block->b_ialloc > 0);
            assert(block->b_iused >= 0);
            assert(block->b_ialloc >= block->b_iused);
        }
        else {
            assert (block->b_iused == 0);
            assert (block->b_ialloc == 0);
        }
    }
}

static int
cfg_builder_init(cfg_builder *g)
{
    g->g_block_list = NULL;
    basicblock *block = cfg_builder_new_block(g);
    if (block == NULL) {
        return ERROR;
    }
    g->g_curblock = g->g_entryblock = block;
    g->g_current_label = NO_LABEL;
    return SUCCESS;
}

static void
cfg_builder_fini(cfg_builder* g)
{
    cfg_builder_check(g);
    basicblock *b = g->g_block_list;
    while (b != NULL) {
        if (b->b_instr) {
            PyObject_Free((void *)b->b_instr);
        }
        basicblock *next = b->b_list;
        PyObject_Free((void *)b);
        b = next;
    }
}

static void
compiler_unit_free(struct compiler_unit *u)
{
    cfg_builder_fini(&u->u_cfg_builder);
    Py_CLEAR(u->u_ste);
    Py_CLEAR(u->u_name);
    Py_CLEAR(u->u_qualname);
    Py_CLEAR(u->u_consts);
    Py_CLEAR(u->u_names);
    Py_CLEAR(u->u_varnames);
    Py_CLEAR(u->u_freevars);
    Py_CLEAR(u->u_cellvars);
    Py_CLEAR(u->u_private);
    PyObject_Free(u);
}

static int
compiler_set_qualname(struct compiler *c)
{
    Py_ssize_t stack_size;
    struct compiler_unit *u = c->u;
    PyObject *name, *base;

    base = NULL;
    stack_size = PyList_GET_SIZE(c->c_stack);
    assert(stack_size >= 1);
    if (stack_size > 1) {
        int scope, force_global = 0;
        struct compiler_unit *parent;
        PyObject *mangled, *capsule;

        capsule = PyList_GET_ITEM(c->c_stack, stack_size - 1);
        parent = (struct compiler_unit *)PyCapsule_GetPointer(capsule, CAPSULE_NAME);
        assert(parent);

        if (u->u_scope_type == COMPILER_SCOPE_FUNCTION
            || u->u_scope_type == COMPILER_SCOPE_ASYNC_FUNCTION
            || u->u_scope_type == COMPILER_SCOPE_CLASS) {
            assert(u->u_name);
            mangled = _Py_Mangle(parent->u_private, u->u_name);
            if (!mangled) {
                return ERROR;
            }

            scope = _PyST_GetScope(parent->u_ste, mangled);
            Py_DECREF(mangled);
            assert(scope != GLOBAL_IMPLICIT);
            if (scope == GLOBAL_EXPLICIT)
                force_global = 1;
        }

        if (!force_global) {
            if (parent->u_scope_type == COMPILER_SCOPE_FUNCTION
                || parent->u_scope_type == COMPILER_SCOPE_ASYNC_FUNCTION
                || parent->u_scope_type == COMPILER_SCOPE_LAMBDA)
            {
                _Py_DECLARE_STR(dot_locals, ".<locals>");
                base = PyUnicode_Concat(parent->u_qualname,
                                        &_Py_STR(dot_locals));
                if (base == NULL) {
                    return ERROR;
                }
            }
            else {
                base = Py_NewRef(parent->u_qualname);
            }
        }
    }

    if (base != NULL) {
        _Py_DECLARE_STR(dot, ".");
        name = PyUnicode_Concat(base, &_Py_STR(dot));
        Py_DECREF(base);
        if (name == NULL) {
            return ERROR;
        }
        PyUnicode_Append(&name, u->u_name);
        if (name == NULL) {
            return ERROR;
        }
    }
    else {
        name = Py_NewRef(u->u_name);
    }
    u->u_qualname = name;

    return SUCCESS;
}

static jump_target_label
cfg_new_label(cfg_builder *g)
{
    jump_target_label lbl = {g->g_next_free_label++};
    return lbl;
}

/* Allocate a new block and return a pointer to it.
   Returns NULL on error.
*/
static basicblock *
cfg_builder_new_block(cfg_builder *g)
{
    basicblock *b = (basicblock *)PyObject_Calloc(1, sizeof(basicblock));
    if (b == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    /* Extend the singly linked list of blocks with new block. */
    b->b_list = g->g_block_list;
    g->g_block_list = b;
    b->b_label = -1;
    return b;
}

static basicblock *
cfg_builder_use_next_block(cfg_builder *g, basicblock *block)
{
    assert(block != NULL);
    g->g_curblock->b_next = block;
    g->g_curblock = block;
    return block;
}

static int
cfg_builder_use_label(cfg_builder *g, jump_target_label lbl)
{
    g->g_current_label = lbl;
    return cfg_builder_maybe_start_new_block(g);
}

static inline int
basicblock_append_instructions(basicblock *target, basicblock *source)
{
    for (int i = 0; i < source->b_iused; i++) {
        int n = basicblock_next_instr(target);
        if (n < 0) {
            return -1;
        }
        target->b_instr[n] = source->b_instr[i];
    }
    return 0;
}

static basicblock *
copy_basicblock(cfg_builder *g, basicblock *block)
{
    /* Cannot copy a block if it has a fallthrough, since
     * a block can only have one fallthrough predecessor.
     */
    assert(BB_NO_FALLTHROUGH(block));
    basicblock *result = cfg_builder_new_block(g);
    if (result == NULL) {
        return NULL;
    }
    if (basicblock_append_instructions(result, block) < 0) {
        return NULL;
    }
    return result;
}

/* Returns the offset of the next instruction in the current block's
   b_instr array.  Resizes the b_instr as necessary.
   Returns -1 on failure.
*/

static int
basicblock_next_instr(basicblock *b)
{
    assert(b != NULL);
    if (b->b_instr == NULL) {
        b->b_instr = (struct instr *)PyObject_Calloc(
                         DEFAULT_BLOCK_SIZE, sizeof(struct instr));
        if (b->b_instr == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        b->b_ialloc = DEFAULT_BLOCK_SIZE;
    }
    else if (b->b_iused == b->b_ialloc) {
        struct instr *tmp;
        size_t oldsize, newsize;
        oldsize = b->b_ialloc * sizeof(struct instr);
        newsize = oldsize << 1;

        if (oldsize > (SIZE_MAX >> 1)) {
            PyErr_NoMemory();
            return -1;
        }

        if (newsize == 0) {
            PyErr_NoMemory();
            return -1;
        }
        b->b_ialloc <<= 1;
        tmp = (struct instr *)PyObject_Realloc(
                                        (void *)b->b_instr, newsize);
        if (tmp == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        b->b_instr = tmp;
        memset((char *)b->b_instr + oldsize, 0, newsize - oldsize);
    }
    return b->b_iused++;
}


/* Return the stack effect of opcode with argument oparg.

   Some opcodes have different stack effect when jump to the target and
   when not jump. The 'jump' parameter specifies the case:

   * 0 -- when not jump
   * 1 -- when jump
   * -1 -- maximal
 */
static int
stack_effect(int opcode, int oparg, int jump)
{
    switch (opcode) {
        case NOP:
        case EXTENDED_ARG:
        case RESUME:
        case CACHE:
            return 0;

        /* Stack manipulation */
        case POP_TOP:
            return -1;
        case SWAP:
            return 0;
        case END_FOR:
            return -2;

        /* Unary operators */
        case UNARY_NEGATIVE:
        case UNARY_NOT:
        case UNARY_INVERT:
            return 0;

        case SET_ADD:
        case LIST_APPEND:
            return -1;
        case MAP_ADD:
            return -2;

        case BINARY_SUBSCR:
            return -1;
        case BINARY_SLICE:
            return -2;
        case STORE_SUBSCR:
            return -3;
        case STORE_SLICE:
            return -4;
        case DELETE_SUBSCR:
            return -2;

        case GET_ITER:
            return 0;

        case LOAD_BUILD_CLASS:
            return 1;

        case RETURN_VALUE:
            return -1;
        case SETUP_ANNOTATIONS:
            return 0;
        case YIELD_VALUE:
            return 0;
        case POP_BLOCK:
            return 0;
        case POP_EXCEPT:
            return -1;

        case STORE_NAME:
            return -1;
        case DELETE_NAME:
            return 0;
        case UNPACK_SEQUENCE:
            return oparg-1;
        case UNPACK_EX:
            return (oparg&0xFF) + (oparg>>8);
        case FOR_ITER:
            return 1;
        case SEND:
            return jump > 0 ? -1 : 0;
        case STORE_ATTR:
            return -2;
        case DELETE_ATTR:
            return -1;
        case STORE_GLOBAL:
            return -1;
        case DELETE_GLOBAL:
            return 0;
        case LOAD_CONST:
            return 1;
        case LOAD_NAME:
            return 1;
        case BUILD_TUPLE:
        case BUILD_LIST:
        case BUILD_SET:
        case BUILD_STRING:
            return 1-oparg;
        case BUILD_MAP:
            return 1 - 2*oparg;
        case BUILD_CONST_KEY_MAP:
            return -oparg;
        case LOAD_ATTR:
            return (oparg & 1);
        case COMPARE_OP:
        case IS_OP:
        case CONTAINS_OP:
            return -1;
        case CHECK_EXC_MATCH:
            return 0;
        case CHECK_EG_MATCH:
            return 0;
        case IMPORT_NAME:
            return -1;
        case IMPORT_FROM:
            return 1;

        /* Jumps */
        case JUMP_FORWARD:
        case JUMP_BACKWARD:
        case JUMP:
        case JUMP_BACKWARD_NO_INTERRUPT:
        case JUMP_NO_INTERRUPT:
            return 0;

        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            return jump ? 0 : -1;

        case POP_JUMP_IF_NONE:
        case POP_JUMP_IF_NOT_NONE:
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
            return -1;

        case LOAD_GLOBAL:
            return (oparg & 1) + 1;

        /* Exception handling pseudo-instructions */
        case SETUP_FINALLY:
            /* 0 in the normal flow.
             * Restore the stack position and push 1 value before jumping to
             * the handler if an exception be raised. */
            return jump ? 1 : 0;
        case SETUP_CLEANUP:
            /* As SETUP_FINALLY, but pushes lasti as well */
            return jump ? 2 : 0;
        case SETUP_WITH:
            /* 0 in the normal flow.
             * Restore the stack position to the position before the result
             * of __(a)enter__ and push 2 values before jumping to the handler
             * if an exception be raised. */
            return jump ? 1 : 0;
        case PREP_RERAISE_STAR:
             return -1;
        case RERAISE:
            return -1;
        case PUSH_EXC_INFO:
            return 1;

        case WITH_EXCEPT_START:
            return 1;

        case LOAD_FAST:
        case LOAD_FAST_CHECK:
            return 1;
        case STORE_FAST:
            return -1;
        case DELETE_FAST:
            return 0;

        case RETURN_GENERATOR:
            return 0;

        case RAISE_VARARGS:
            return -oparg;

        /* Functions and calls */
        case KW_NAMES:
            return 0;
        case CALL:
            return -1-oparg;
        case CALL_INTRINSIC_1:
            return 0;
        case CALL_FUNCTION_EX:
            return -2 - ((oparg & 0x01) != 0);
        case MAKE_FUNCTION:
            return 0 - ((oparg & 0x01) != 0) - ((oparg & 0x02) != 0) -
                ((oparg & 0x04) != 0) - ((oparg & 0x08) != 0);
        case BUILD_SLICE:
            if (oparg == 3)
                return -2;
            else
                return -1;

        /* Closures */
        case MAKE_CELL:
        case COPY_FREE_VARS:
            return 0;
        case LOAD_CLOSURE:
            return 1;
        case LOAD_DEREF:
        case LOAD_CLASSDEREF:
            return 1;
        case STORE_DEREF:
            return -1;
        case DELETE_DEREF:
            return 0;

        /* Iterators and generators */
        case GET_AWAITABLE:
            return 0;

        case BEFORE_ASYNC_WITH:
        case BEFORE_WITH:
            return 1;
        case GET_AITER:
            return 0;
        case GET_ANEXT:
            return 1;
        case GET_YIELD_FROM_ITER:
            return 0;
        case END_ASYNC_FOR:
            return -2;
        case CLEANUP_THROW:
            return -2;
        case FORMAT_VALUE:
            /* If there's a fmt_spec on the stack, we go from 2->1,
               else 1->1. */
            return (oparg & FVS_MASK) == FVS_HAVE_SPEC ? -1 : 0;
        case LOAD_METHOD:
            return 1;
        case LOAD_ASSERTION_ERROR:
            return 1;
        case LIST_EXTEND:
        case SET_UPDATE:
        case DICT_MERGE:
        case DICT_UPDATE:
            return -1;
        case MATCH_CLASS:
            return -2;
        case GET_LEN:
        case MATCH_MAPPING:
        case MATCH_SEQUENCE:
        case MATCH_KEYS:
            return 1;
        case COPY:
        case PUSH_NULL:
            return 1;
        case BINARY_OP:
            return -1;
        case INTERPRETER_EXIT:
            return -1;
        default:
            return PY_INVALID_STACK_EFFECT;
    }
    return PY_INVALID_STACK_EFFECT; /* not reachable */
}

int
PyCompile_OpcodeStackEffectWithJump(int opcode, int oparg, int jump)
{
    return stack_effect(opcode, oparg, jump);
}

int
PyCompile_OpcodeStackEffect(int opcode, int oparg)
{
    return stack_effect(opcode, oparg, -1);
}

static int
basicblock_addop(basicblock *b, int opcode, int oparg, location loc)
{
    assert(IS_WITHIN_OPCODE_RANGE(opcode));
    assert(!IS_ASSEMBLER_OPCODE(opcode));
    assert(HAS_ARG(opcode) || HAS_TARGET(opcode) || oparg == 0);
    assert(0 <= oparg && oparg < (1 << 30));

    int off = basicblock_next_instr(b);
    if (off < 0) {
        return ERROR;
    }
    struct instr *i = &b->b_instr[off];
    i->i_opcode = opcode;
    i->i_oparg = oparg;
    i->i_target = NULL;
    i->i_loc = loc;

    return SUCCESS;
}

static bool
cfg_builder_current_block_is_terminated(cfg_builder *g)
{
    if (IS_LABEL(g->g_current_label)) {
        return true;
    }
    struct instr *last = basicblock_last_instr(g->g_curblock);
    return last && IS_TERMINATOR_OPCODE(last->i_opcode);
}

static int
cfg_builder_maybe_start_new_block(cfg_builder *g)
{
    if (cfg_builder_current_block_is_terminated(g)) {
        basicblock *b = cfg_builder_new_block(g);
        if (b == NULL) {
            return -1;
        }
        b->b_label = g->g_current_label.id;
        g->g_current_label = NO_LABEL;
        cfg_builder_use_next_block(g, b);
    }
    return 0;
}

static int
cfg_builder_addop(cfg_builder *g, int opcode, int oparg, location loc)
{
    if (cfg_builder_maybe_start_new_block(g) != 0) {
        return -1;
    }
    return basicblock_addop(g->g_curblock, opcode, oparg, loc);
}

static int
cfg_builder_addop_noarg(cfg_builder *g, int opcode, location loc)
{
    assert(!HAS_ARG(opcode));
    return cfg_builder_addop(g, opcode, 0, loc);
}

static Py_ssize_t
dict_add_o(PyObject *dict, PyObject *o)
{
    PyObject *v;
    Py_ssize_t arg;

    v = PyDict_GetItemWithError(dict, o);
    if (!v) {
        if (PyErr_Occurred()) {
            return -1;
        }
        arg = PyDict_GET_SIZE(dict);
        v = PyLong_FromSsize_t(arg);
        if (!v) {
            return -1;
        }
        if (PyDict_SetItem(dict, o, v) < 0) {
            Py_DECREF(v);
            return -1;
        }
        Py_DECREF(v);
    }
    else
        arg = PyLong_AsLong(v);
    return arg;
}

// Merge const *o* recursively and return constant key object.
static PyObject*
merge_consts_recursive(PyObject *const_cache, PyObject *o)
{
    assert(PyDict_CheckExact(const_cache));
    // None and Ellipsis are singleton, and key is the singleton.
    // No need to merge object and key.
    if (o == Py_None || o == Py_Ellipsis) {
        return Py_NewRef(o);
    }

    PyObject *key = _PyCode_ConstantKey(o);
    if (key == NULL) {
        return NULL;
    }

    // t is borrowed reference
    PyObject *t = PyDict_SetDefault(const_cache, key, key);
    if (t != key) {
        // o is registered in const_cache.  Just use it.
        Py_XINCREF(t);
        Py_DECREF(key);
        return t;
    }

    // We registered o in const_cache.
    // When o is a tuple or frozenset, we want to merge its
    // items too.
    if (PyTuple_CheckExact(o)) {
        Py_ssize_t len = PyTuple_GET_SIZE(o);
        for (Py_ssize_t i = 0; i < len; i++) {
            PyObject *item = PyTuple_GET_ITEM(o, i);
            PyObject *u = merge_consts_recursive(const_cache, item);
            if (u == NULL) {
                Py_DECREF(key);
                return NULL;
            }

            // See _PyCode_ConstantKey()
            PyObject *v;  // borrowed
            if (PyTuple_CheckExact(u)) {
                v = PyTuple_GET_ITEM(u, 1);
            }
            else {
                v = u;
            }
            if (v != item) {
                PyTuple_SET_ITEM(o, i, Py_NewRef(v));
                Py_DECREF(item);
            }

            Py_DECREF(u);
        }
    }
    else if (PyFrozenSet_CheckExact(o)) {
        // *key* is tuple. And its first item is frozenset of
        // constant keys.
        // See _PyCode_ConstantKey() for detail.
        assert(PyTuple_CheckExact(key));
        assert(PyTuple_GET_SIZE(key) == 2);

        Py_ssize_t len = PySet_GET_SIZE(o);
        if (len == 0) {  // empty frozenset should not be re-created.
            return key;
        }
        PyObject *tuple = PyTuple_New(len);
        if (tuple == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        Py_ssize_t i = 0, pos = 0;
        PyObject *item;
        Py_hash_t hash;
        while (_PySet_NextEntry(o, &pos, &item, &hash)) {
            PyObject *k = merge_consts_recursive(const_cache, item);
            if (k == NULL) {
                Py_DECREF(tuple);
                Py_DECREF(key);
                return NULL;
            }
            PyObject *u;
            if (PyTuple_CheckExact(k)) {
                u = Py_NewRef(PyTuple_GET_ITEM(k, 1));
                Py_DECREF(k);
            }
            else {
                u = k;
            }
            PyTuple_SET_ITEM(tuple, i, u);  // Steals reference of u.
            i++;
        }

        // Instead of rewriting o, we create new frozenset and embed in the
        // key tuple.  Caller should get merged frozenset from the key tuple.
        PyObject *new = PyFrozenSet_New(tuple);
        Py_DECREF(tuple);
        if (new == NULL) {
            Py_DECREF(key);
            return NULL;
        }
        assert(PyTuple_GET_ITEM(key, 1) == o);
        Py_DECREF(o);
        PyTuple_SET_ITEM(key, 1, new);
    }

    return key;
}

static Py_ssize_t
compiler_add_const(struct compiler *c, PyObject *o)
{
    PyObject *key = merge_consts_recursive(c->c_const_cache, o);
    if (key == NULL) {
        return -1;
    }

    Py_ssize_t arg = dict_add_o(c->u->u_consts, key);
    Py_DECREF(key);
    return arg;
}

static int
compiler_addop_load_const(struct compiler *c, location loc, PyObject *o)
{
    Py_ssize_t arg = compiler_add_const(c, o);
    if (arg < 0) {
        return ERROR;
    }
    return cfg_builder_addop_i(CFG_BUILDER(c), LOAD_CONST, arg, loc);
}

static int
compiler_addop_o(struct compiler *c, location loc,
                 int opcode, PyObject *dict, PyObject *o)
{
    Py_ssize_t arg = dict_add_o(dict, o);
    if (arg < 0) {
        return ERROR;
    }
    return cfg_builder_addop_i(CFG_BUILDER(c), opcode, arg, loc);
}

static int
compiler_addop_name(struct compiler *c, location loc,
                    int opcode, PyObject *dict, PyObject *o)
{
    Py_ssize_t arg;

    PyObject *mangled = _Py_Mangle(c->u->u_private, o);
    if (!mangled) {
        return ERROR;
    }
    arg = dict_add_o(dict, mangled);
    Py_DECREF(mangled);
    if (arg < 0) {
        return ERROR;
    }
    if (opcode == LOAD_ATTR) {
        arg <<= 1;
    }
    if (opcode == LOAD_METHOD) {
        opcode = LOAD_ATTR;
        arg <<= 1;
        arg |= 1;
    }
    return cfg_builder_addop_i(CFG_BUILDER(c), opcode, arg, loc);
}

/* Add an opcode with an integer argument */
static int
cfg_builder_addop_i(cfg_builder *g, int opcode, Py_ssize_t oparg, location loc)
{
    /* oparg value is unsigned, but a signed C int is usually used to store
       it in the C code (like Python/ceval.c).

       Limit to 32-bit signed C int (rather than INT_MAX) for portability.

       The argument of a concrete bytecode instruction is limited to 8-bit.
       EXTENDED_ARG is used for 16, 24, and 32-bit arguments. */

    int oparg_ = Py_SAFE_DOWNCAST(oparg, Py_ssize_t, int);
    return cfg_builder_addop(g, opcode, oparg_, loc);
}

static int
cfg_builder_addop_j(cfg_builder *g, location loc,
                    int opcode, jump_target_label target)
{
    assert(IS_LABEL(target));
    assert(IS_JUMP_OPCODE(opcode) || IS_BLOCK_PUSH_OPCODE(opcode));
    return cfg_builder_addop(g, opcode, target.id, loc);
}

#define ADDOP(C, LOC, OP) \
    RETURN_IF_ERROR(cfg_builder_addop_noarg(CFG_BUILDER(C), (OP), (LOC)))

#define ADDOP_IN_SCOPE(C, LOC, OP) { \
    if (cfg_builder_addop_noarg(CFG_BUILDER(C), (OP), (LOC)) < 0) { \
        compiler_exit_scope(C); \
        return -1; \
    } \
}

#define ADDOP_LOAD_CONST(C, LOC, O) \
    RETURN_IF_ERROR(compiler_addop_load_const((C), (LOC), (O)))

/* Same as ADDOP_LOAD_CONST, but steals a reference. */
#define ADDOP_LOAD_CONST_NEW(C, LOC, O) { \
    PyObject *__new_const = (O); \
    if (__new_const == NULL) { \
        return ERROR; \
    } \
    if (compiler_addop_load_const((C), (LOC), __new_const) < 0) { \
        Py_DECREF(__new_const); \
        return ERROR; \
    } \
    Py_DECREF(__new_const); \
}

#define ADDOP_N(C, LOC, OP, O, TYPE) { \
    assert(!HAS_CONST(OP)); /* use ADDOP_LOAD_CONST_NEW */ \
    if (compiler_addop_o((C), (LOC), (OP), (C)->u->u_ ## TYPE, (O)) < 0) { \
        Py_DECREF((O)); \
        return ERROR; \
    } \
    Py_DECREF((O)); \
}

#define ADDOP_NAME(C, LOC, OP, O, TYPE) \
    RETURN_IF_ERROR(compiler_addop_name((C), (LOC), (OP), (C)->u->u_ ## TYPE, (O)))

#define ADDOP_I(C, LOC, OP, O) \
    RETURN_IF_ERROR(cfg_builder_addop_i(CFG_BUILDER(C), (OP), (O), (LOC)))

#define ADDOP_JUMP(C, LOC, OP, O) \
    RETURN_IF_ERROR(cfg_builder_addop_j(CFG_BUILDER(C), (LOC), (OP), (O)))

#define ADDOP_COMPARE(C, LOC, CMP) \
    RETURN_IF_ERROR(compiler_addcompare((C), (LOC), (cmpop_ty)(CMP)))

#define ADDOP_BINARY(C, LOC, BINOP) \
    RETURN_IF_ERROR(addop_binary((C), (LOC), (BINOP), false))

#define ADDOP_INPLACE(C, LOC, BINOP) \
    RETURN_IF_ERROR(addop_binary((C), (LOC), (BINOP), true))

#define ADD_YIELD_FROM(C, LOC, await) \
    RETURN_IF_ERROR(compiler_add_yield_from((C), (LOC), (await)))

#define POP_EXCEPT_AND_RERAISE(C, LOC) \
    RETURN_IF_ERROR(compiler_pop_except_and_reraise((C), (LOC)))

#define ADDOP_YIELD(C, LOC) \
    RETURN_IF_ERROR(addop_yield((C), (LOC)))

/* VISIT and VISIT_SEQ takes an ASDL type as their second argument.  They use
   the ASDL name to synthesize the name of the C type and the visit function.
*/

#define VISIT(C, TYPE, V) \
    RETURN_IF_ERROR(compiler_visit_ ## TYPE((C), (V)));

#define VISIT_IN_SCOPE(C, TYPE, V) {\
    if (compiler_visit_ ## TYPE((C), (V)) < 0) { \
        compiler_exit_scope(C); \
        return ERROR; \
    } \
}

#define VISIT_SEQ(C, TYPE, SEQ) { \
    int _i; \
    asdl_ ## TYPE ## _seq *seq = (SEQ); /* avoid variable capture */ \
    for (_i = 0; _i < asdl_seq_LEN(seq); _i++) { \
        TYPE ## _ty elt = (TYPE ## _ty)asdl_seq_GET(seq, _i); \
        if (compiler_visit_ ## TYPE((C), elt) < 0) \
            return ERROR; \
    } \
}

#define VISIT_SEQ_IN_SCOPE(C, TYPE, SEQ) { \
    int _i; \
    asdl_ ## TYPE ## _seq *seq = (SEQ); /* avoid variable capture */ \
    for (_i = 0; _i < asdl_seq_LEN(seq); _i++) { \
        TYPE ## _ty elt = (TYPE ## _ty)asdl_seq_GET(seq, _i); \
        if (compiler_visit_ ## TYPE((C), elt) < 0) { \
            compiler_exit_scope(C); \
            return ERROR; \
        } \
    } \
}


static int
compiler_enter_scope(struct compiler *c, identifier name,
                     int scope_type, void *key, int lineno)
{
    location loc = LOCATION(lineno, lineno, 0, 0);

    struct compiler_unit *u;

    u = (struct compiler_unit *)PyObject_Calloc(1, sizeof(
                                            struct compiler_unit));
    if (!u) {
        PyErr_NoMemory();
        return ERROR;
    }
    u->u_scope_type = scope_type;
    u->u_argcount = 0;
    u->u_posonlyargcount = 0;
    u->u_kwonlyargcount = 0;
    u->u_ste = PySymtable_Lookup(c->c_st, key);
    if (!u->u_ste) {
        compiler_unit_free(u);
        return ERROR;
    }
    u->u_name = Py_NewRef(name);
    u->u_varnames = list2dict(u->u_ste->ste_varnames);
    u->u_cellvars = dictbytype(u->u_ste->ste_symbols, CELL, 0, 0);
    if (!u->u_varnames || !u->u_cellvars) {
        compiler_unit_free(u);
        return ERROR;
    }
    if (u->u_ste->ste_needs_class_closure) {
        /* Cook up an implicit __class__ cell. */
        int res;
        assert(u->u_scope_type == COMPILER_SCOPE_CLASS);
        assert(PyDict_GET_SIZE(u->u_cellvars) == 0);
        res = PyDict_SetItem(u->u_cellvars, &_Py_ID(__class__),
                             _PyLong_GetZero());
        if (res < 0) {
            compiler_unit_free(u);
            return ERROR;
        }
    }

    u->u_freevars = dictbytype(u->u_ste->ste_symbols, FREE, DEF_FREE_CLASS,
                               PyDict_GET_SIZE(u->u_cellvars));
    if (!u->u_freevars) {
        compiler_unit_free(u);
        return ERROR;
    }

    u->u_nfblocks = 0;
    u->u_firstlineno = lineno;
    u->u_consts = PyDict_New();
    if (!u->u_consts) {
        compiler_unit_free(u);
        return ERROR;
    }
    u->u_names = PyDict_New();
    if (!u->u_names) {
        compiler_unit_free(u);
        return ERROR;
    }

    u->u_private = NULL;

    /* Push the old compiler_unit on the stack. */
    if (c->u) {
        PyObject *capsule = PyCapsule_New(c->u, CAPSULE_NAME, NULL);
        if (!capsule || PyList_Append(c->c_stack, capsule) < 0) {
            Py_XDECREF(capsule);
            compiler_unit_free(u);
            return ERROR;
        }
        Py_DECREF(capsule);
        u->u_private = Py_XNewRef(c->u->u_private);
    }
    c->u = u;

    c->c_nestlevel++;

    cfg_builder *g = CFG_BUILDER(c);
    RETURN_IF_ERROR(cfg_builder_init(g));

    if (u->u_scope_type == COMPILER_SCOPE_MODULE) {
        loc.lineno = 0;
    }
    else {
        RETURN_IF_ERROR(compiler_set_qualname(c));
    }
    ADDOP_I(c, loc, RESUME, 0);

    if (u->u_scope_type == COMPILER_SCOPE_MODULE) {
        loc.lineno = -1;
    }
    return SUCCESS;
}

static void
compiler_exit_scope(struct compiler *c)
{
    // Don't call PySequence_DelItem() with an exception raised
    PyObject *exc_type, *exc_val, *exc_tb;
    PyErr_Fetch(&exc_type, &exc_val, &exc_tb);

    c->c_nestlevel--;
    compiler_unit_free(c->u);
    /* Restore c->u to the parent unit. */
    Py_ssize_t n = PyList_GET_SIZE(c->c_stack) - 1;
    if (n >= 0) {
        PyObject *capsule = PyList_GET_ITEM(c->c_stack, n);
        c->u = (struct compiler_unit *)PyCapsule_GetPointer(capsule, CAPSULE_NAME);
        assert(c->u);
        /* we are deleting from a list so this really shouldn't fail */
        if (PySequence_DelItem(c->c_stack, n) < 0) {
            _PyErr_WriteUnraisableMsg("on removing the last compiler "
                                      "stack item", NULL);
        }
        cfg_builder_check(CFG_BUILDER(c));
    }
    else {
        c->u = NULL;
    }

    PyErr_Restore(exc_type, exc_val, exc_tb);
}

/* Search if variable annotations are present statically in a block. */

static bool
find_ann(asdl_stmt_seq *stmts)
{
    int i, j, res = 0;
    stmt_ty st;

    for (i = 0; i < asdl_seq_LEN(stmts); i++) {
        st = (stmt_ty)asdl_seq_GET(stmts, i);
        switch (st->kind) {
        case AnnAssign_kind:
            return true;
        case For_kind:
            res = find_ann(st->v.For.body) ||
                  find_ann(st->v.For.orelse);
            break;
        case AsyncFor_kind:
            res = find_ann(st->v.AsyncFor.body) ||
                  find_ann(st->v.AsyncFor.orelse);
            break;
        case While_kind:
            res = find_ann(st->v.While.body) ||
                  find_ann(st->v.While.orelse);
            break;
        case If_kind:
            res = find_ann(st->v.If.body) ||
                  find_ann(st->v.If.orelse);
            break;
        case With_kind:
            res = find_ann(st->v.With.body);
            break;
        case AsyncWith_kind:
            res = find_ann(st->v.AsyncWith.body);
            break;
        case Try_kind:
            for (j = 0; j < asdl_seq_LEN(st->v.Try.handlers); j++) {
                excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
                    st->v.Try.handlers, j);
                if (find_ann(handler->v.ExceptHandler.body)) {
                    return true;
                }
            }
            res = find_ann(st->v.Try.body) ||
                  find_ann(st->v.Try.finalbody) ||
                  find_ann(st->v.Try.orelse);
            break;
        case TryStar_kind:
            for (j = 0; j < asdl_seq_LEN(st->v.TryStar.handlers); j++) {
                excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
                    st->v.TryStar.handlers, j);
                if (find_ann(handler->v.ExceptHandler.body)) {
                    return true;
                }
            }
            res = find_ann(st->v.TryStar.body) ||
                  find_ann(st->v.TryStar.finalbody) ||
                  find_ann(st->v.TryStar.orelse);
            break;
        default:
            res = false;
        }
        if (res) {
            break;
        }
    }
    return res;
}

/*
 * Frame block handling functions
 */

static int
compiler_push_fblock(struct compiler *c, location loc,
                     enum fblocktype t, jump_target_label block_label,
                     jump_target_label exit, void *datum)
{
    struct fblockinfo *f;
    if (c->u->u_nfblocks >= CO_MAXBLOCKS) {
        return compiler_error(c, loc, "too many statically nested blocks");
    }
    f = &c->u->u_fblock[c->u->u_nfblocks++];
    f->fb_type = t;
    f->fb_block = block_label;
    f->fb_exit = exit;
    f->fb_datum = datum;
    return SUCCESS;
}

static void
compiler_pop_fblock(struct compiler *c, enum fblocktype t, jump_target_label block_label)
{
    struct compiler_unit *u = c->u;
    assert(u->u_nfblocks > 0);
    u->u_nfblocks--;
    assert(u->u_fblock[u->u_nfblocks].fb_type == t);
    assert(SAME_LABEL(u->u_fblock[u->u_nfblocks].fb_block, block_label));
}

static int
compiler_call_exit_with_nones(struct compiler *c, location loc)
{
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADDOP_I(c, loc, CALL, 2);
    return SUCCESS;
}

static int
compiler_add_yield_from(struct compiler *c, location loc, int await)
{
    NEW_JUMP_TARGET_LABEL(c, send);
    NEW_JUMP_TARGET_LABEL(c, fail);
    NEW_JUMP_TARGET_LABEL(c, exit);

    USE_LABEL(c, send);
    ADDOP_JUMP(c, loc, SEND, exit);
    // Set up a virtual try/except to handle when StopIteration is raised during
    // a close or throw call. The only way YIELD_VALUE raises if they do!
    ADDOP_JUMP(c, loc, SETUP_FINALLY, fail);
    ADDOP_I(c, loc, YIELD_VALUE, 0);
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    ADDOP_I(c, loc, RESUME, await ? 3 : 2);
    ADDOP_JUMP(c, loc, JUMP_NO_INTERRUPT, send);

    USE_LABEL(c, fail);
    ADDOP(c, loc, CLEANUP_THROW);

    USE_LABEL(c, exit);
    return SUCCESS;
}

static int
compiler_pop_except_and_reraise(struct compiler *c, location loc)
{
    /* Stack contents
     * [exc_info, lasti, exc]            COPY        3
     * [exc_info, lasti, exc, exc_info]  POP_EXCEPT
     * [exc_info, lasti, exc]            RERAISE      1
     * (exception_unwind clears the stack)
     */

    ADDOP_I(c, loc, COPY, 3);
    ADDOP(c, loc, POP_EXCEPT);
    ADDOP_I(c, loc, RERAISE, 1);
    return SUCCESS;
}

/* Unwind a frame block.  If preserve_tos is true, the TOS before
 * popping the blocks will be restored afterwards, unless another
 * return, break or continue is found. In which case, the TOS will
 * be popped.
 */
static int
compiler_unwind_fblock(struct compiler *c, location *ploc,
                       struct fblockinfo *info, int preserve_tos)
{
    switch (info->fb_type) {
        case WHILE_LOOP:
        case EXCEPTION_HANDLER:
        case EXCEPTION_GROUP_HANDLER:
        case ASYNC_COMPREHENSION_GENERATOR:
            return SUCCESS;

        case FOR_LOOP:
            /* Pop the iterator */
            if (preserve_tos) {
                ADDOP_I(c, *ploc, SWAP, 2);
            }
            ADDOP(c, *ploc, POP_TOP);
            return SUCCESS;

        case TRY_EXCEPT:
            ADDOP(c, *ploc, POP_BLOCK);
            return SUCCESS;

        case FINALLY_TRY:
            /* This POP_BLOCK gets the line number of the unwinding statement */
            ADDOP(c, *ploc, POP_BLOCK);
            if (preserve_tos) {
                RETURN_IF_ERROR(
                    compiler_push_fblock(c, *ploc, POP_VALUE, NO_LABEL, NO_LABEL, NULL));
            }
            /* Emit the finally block */
            VISIT_SEQ(c, stmt, info->fb_datum);
            if (preserve_tos) {
                compiler_pop_fblock(c, POP_VALUE, NO_LABEL);
            }
            /* The finally block should appear to execute after the
             * statement causing the unwinding, so make the unwinding
             * instruction artificial */
            *ploc = NO_LOCATION;
            return SUCCESS;

        case FINALLY_END:
            if (preserve_tos) {
                ADDOP_I(c, *ploc, SWAP, 2);
            }
            ADDOP(c, *ploc, POP_TOP); /* exc_value */
            if (preserve_tos) {
                ADDOP_I(c, *ploc, SWAP, 2);
            }
            ADDOP(c, *ploc, POP_BLOCK);
            ADDOP(c, *ploc, POP_EXCEPT);
            return SUCCESS;

        case WITH:
        case ASYNC_WITH:
            *ploc = LOC((stmt_ty)info->fb_datum);
            ADDOP(c, *ploc, POP_BLOCK);
            if (preserve_tos) {
                ADDOP_I(c, *ploc, SWAP, 2);
            }
            RETURN_IF_ERROR(compiler_call_exit_with_nones(c, *ploc));
            if (info->fb_type == ASYNC_WITH) {
                ADDOP_I(c, *ploc, GET_AWAITABLE, 2);
                ADDOP_LOAD_CONST(c, *ploc, Py_None);
                ADD_YIELD_FROM(c, *ploc, 1);
            }
            ADDOP(c, *ploc, POP_TOP);
            /* The exit block should appear to execute after the
             * statement causing the unwinding, so make the unwinding
             * instruction artificial */
            *ploc = NO_LOCATION;
            return SUCCESS;

        case HANDLER_CLEANUP: {
            if (info->fb_datum) {
                ADDOP(c, *ploc, POP_BLOCK);
            }
            if (preserve_tos) {
                ADDOP_I(c, *ploc, SWAP, 2);
            }
            ADDOP(c, *ploc, POP_BLOCK);
            ADDOP(c, *ploc, POP_EXCEPT);
            if (info->fb_datum) {
                ADDOP_LOAD_CONST(c, *ploc, Py_None);
                RETURN_IF_ERROR(compiler_nameop(c, *ploc, info->fb_datum, Store));
                RETURN_IF_ERROR(compiler_nameop(c, *ploc, info->fb_datum, Del));
            }
            return SUCCESS;
        }
        case POP_VALUE: {
            if (preserve_tos) {
                ADDOP_I(c, *ploc, SWAP, 2);
            }
            ADDOP(c, *ploc, POP_TOP);
            return SUCCESS;
        }
    }
    Py_UNREACHABLE();
}

/** Unwind block stack. If loop is not NULL, then stop when the first loop is encountered. */
static int
compiler_unwind_fblock_stack(struct compiler *c, location *ploc,
                             int preserve_tos, struct fblockinfo **loop)
{
    if (c->u->u_nfblocks == 0) {
        return SUCCESS;
    }
    struct fblockinfo *top = &c->u->u_fblock[c->u->u_nfblocks-1];
    if (top->fb_type == EXCEPTION_GROUP_HANDLER) {
        return compiler_error(
            c, *ploc, "'break', 'continue' and 'return' cannot appear in an except* block");
    }
    if (loop != NULL && (top->fb_type == WHILE_LOOP || top->fb_type == FOR_LOOP)) {
        *loop = top;
        return SUCCESS;
    }
    struct fblockinfo copy = *top;
    c->u->u_nfblocks--;
    RETURN_IF_ERROR(compiler_unwind_fblock(c, ploc, &copy, preserve_tos));
    RETURN_IF_ERROR(compiler_unwind_fblock_stack(c, ploc, preserve_tos, loop));
    c->u->u_fblock[c->u->u_nfblocks] = copy;
    c->u->u_nfblocks++;
    return SUCCESS;
}

/* Compile a sequence of statements, checking for a docstring
   and for annotations. */

static int
compiler_body(struct compiler *c, location loc, asdl_stmt_seq *stmts)
{
    int i = 0;
    stmt_ty st;
    PyObject *docstring;

    /* Set current line number to the line number of first statement.
       This way line number for SETUP_ANNOTATIONS will always
       coincide with the line number of first "real" statement in module.
       If body is empty, then lineno will be set later in assemble. */
    if (c->u->u_scope_type == COMPILER_SCOPE_MODULE && asdl_seq_LEN(stmts)) {
        st = (stmt_ty)asdl_seq_GET(stmts, 0);
        loc = LOC(st);
    }
    /* Every annotated class and module should have __annotations__. */
    if (find_ann(stmts)) {
        ADDOP(c, loc, SETUP_ANNOTATIONS);
    }
    if (!asdl_seq_LEN(stmts)) {
        return SUCCESS;
    }
    /* if not -OO mode, set docstring */
    if (c->c_optimize < 2) {
        docstring = _PyAST_GetDocString(stmts);
        if (docstring) {
            i = 1;
            st = (stmt_ty)asdl_seq_GET(stmts, 0);
            assert(st->kind == Expr_kind);
            VISIT(c, expr, st->v.Expr.value);
            RETURN_IF_ERROR(compiler_nameop(c, NO_LOCATION, &_Py_ID(__doc__), Store));
        }
    }
    for (; i < asdl_seq_LEN(stmts); i++) {
        VISIT(c, stmt, (stmt_ty)asdl_seq_GET(stmts, i));
    }
    return SUCCESS;
}

static int
compiler_codegen(struct compiler *c, mod_ty mod)
{
    _Py_DECLARE_STR(anon_module, "<module>");
    RETURN_IF_ERROR(
        compiler_enter_scope(c, &_Py_STR(anon_module), COMPILER_SCOPE_MODULE,
                             mod, 1));

    location loc = LOCATION(1, 1, 0, 0);
    switch (mod->kind) {
    case Module_kind:
        if (compiler_body(c, loc, mod->v.Module.body) < 0) {
            compiler_exit_scope(c);
            return ERROR;
        }
        break;
    case Interactive_kind:
        if (find_ann(mod->v.Interactive.body)) {
            ADDOP(c, loc, SETUP_ANNOTATIONS);
        }
        c->c_interactive = 1;
        VISIT_SEQ_IN_SCOPE(c, stmt, mod->v.Interactive.body);
        break;
    case Expression_kind:
        VISIT_IN_SCOPE(c, expr, mod->v.Expression.body);
        break;
    default:
        PyErr_Format(PyExc_SystemError,
                     "module kind %d should not be possible",
                     mod->kind);
        return ERROR;
    }
    return SUCCESS;
}

static PyCodeObject *
compiler_mod(struct compiler *c, mod_ty mod)
{
    int addNone = mod->kind != Expression_kind;
    if (compiler_codegen(c, mod) < 0) {
        return NULL;
    }
    PyCodeObject *co = assemble(c, addNone);
    compiler_exit_scope(c);
    return co;
}

/* The test for LOCAL must come before the test for FREE in order to
   handle classes where name is both local and free.  The local var is
   a method and the free var is a free var referenced within a method.
*/

static int
get_ref_type(struct compiler *c, PyObject *name)
{
    int scope;
    if (c->u->u_scope_type == COMPILER_SCOPE_CLASS &&
        _PyUnicode_EqualToASCIIString(name, "__class__"))
        return CELL;
    scope = _PyST_GetScope(c->u->u_ste, name);
    if (scope == 0) {
        PyErr_Format(PyExc_SystemError,
                     "_PyST_GetScope(name=%R) failed: "
                     "unknown scope in unit %S (%R); "
                     "symbols: %R; locals: %R; globals: %R",
                     name,
                     c->u->u_name, c->u->u_ste->ste_id,
                     c->u->u_ste->ste_symbols, c->u->u_varnames, c->u->u_names);
        return -1;
    }
    return scope;
}

static int
compiler_lookup_arg(PyObject *dict, PyObject *name)
{
    PyObject *v;
    v = PyDict_GetItemWithError(dict, name);
    if (v == NULL)
        return -1;
    return PyLong_AS_LONG(v);
}

static int
compiler_make_closure(struct compiler *c, location loc,
                      PyCodeObject *co, Py_ssize_t flags)
{
    if (co->co_nfreevars) {
        int i = PyCode_GetFirstFree(co);
        for (; i < co->co_nlocalsplus; ++i) {
            /* Bypass com_addop_varname because it will generate
               LOAD_DEREF but LOAD_CLOSURE is needed.
            */
            PyObject *name = PyTuple_GET_ITEM(co->co_localsplusnames, i);

            /* Special case: If a class contains a method with a
               free variable that has the same name as a method,
               the name will be considered free *and* local in the
               class.  It should be handled by the closure, as
               well as by the normal name lookup logic.
            */
            int reftype = get_ref_type(c, name);
            if (reftype == -1) {
                return ERROR;
            }
            int arg;
            if (reftype == CELL) {
                arg = compiler_lookup_arg(c->u->u_cellvars, name);
            }
            else {
                arg = compiler_lookup_arg(c->u->u_freevars, name);
            }
            if (arg == -1) {
                PyObject *freevars = _PyCode_GetFreevars(co);
                if (freevars == NULL) {
                    PyErr_Clear();
                }
                PyErr_Format(PyExc_SystemError,
                    "compiler_lookup_arg(name=%R) with reftype=%d failed in %S; "
                    "freevars of code %S: %R",
                    name,
                    reftype,
                    c->u->u_name,
                    co->co_name,
                    freevars);
                Py_DECREF(freevars);
                return ERROR;
            }
            ADDOP_I(c, loc, LOAD_CLOSURE, arg);
        }
        flags |= 0x08;
        ADDOP_I(c, loc, BUILD_TUPLE, co->co_nfreevars);
    }
    ADDOP_LOAD_CONST(c, loc, (PyObject*)co);
    ADDOP_I(c, loc, MAKE_FUNCTION, flags);
    return SUCCESS;
}

static int
compiler_decorators(struct compiler *c, asdl_expr_seq* decos)
{
    if (!decos) {
        return SUCCESS;
    }

    for (Py_ssize_t i = 0; i < asdl_seq_LEN(decos); i++) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(decos, i));
    }
    return SUCCESS;
}

static int
compiler_apply_decorators(struct compiler *c, asdl_expr_seq* decos)
{
    if (!decos) {
        return SUCCESS;
    }

    for (Py_ssize_t i = asdl_seq_LEN(decos) - 1; i > -1; i--) {
        location loc = LOC((expr_ty)asdl_seq_GET(decos, i));
        ADDOP_I(c, loc, CALL, 0);
    }
    return SUCCESS;
}

static int
compiler_visit_kwonlydefaults(struct compiler *c, location loc,
                              asdl_arg_seq *kwonlyargs, asdl_expr_seq *kw_defaults)
{
    /* Push a dict of keyword-only default values.

       Return -1 on error, 0 if no dict pushed, 1 if a dict is pushed.
       */
    int i;
    PyObject *keys = NULL;

    for (i = 0; i < asdl_seq_LEN(kwonlyargs); i++) {
        arg_ty arg = asdl_seq_GET(kwonlyargs, i);
        expr_ty default_ = asdl_seq_GET(kw_defaults, i);
        if (default_) {
            PyObject *mangled = _Py_Mangle(c->u->u_private, arg->arg);
            if (!mangled) {
                goto error;
            }
            if (keys == NULL) {
                keys = PyList_New(1);
                if (keys == NULL) {
                    Py_DECREF(mangled);
                    return ERROR;
                }
                PyList_SET_ITEM(keys, 0, mangled);
            }
            else {
                int res = PyList_Append(keys, mangled);
                Py_DECREF(mangled);
                if (res == -1) {
                    goto error;
                }
            }
            if (compiler_visit_expr(c, default_) < 0) {
                goto error;
            }
        }
    }
    if (keys != NULL) {
        Py_ssize_t default_count = PyList_GET_SIZE(keys);
        PyObject *keys_tuple = PyList_AsTuple(keys);
        Py_DECREF(keys);
        ADDOP_LOAD_CONST_NEW(c, loc, keys_tuple);
        ADDOP_I(c, loc, BUILD_CONST_KEY_MAP, default_count);
        assert(default_count > 0);
        return 1;
    }
    else {
        return 0;
    }

error:
    Py_XDECREF(keys);
    return ERROR;
}

static int
compiler_visit_annexpr(struct compiler *c, expr_ty annotation)
{
    location loc = LOC(annotation);
    ADDOP_LOAD_CONST_NEW(c, loc, _PyAST_ExprAsUnicode(annotation));
    return SUCCESS;
}

static int
compiler_visit_argannotation(struct compiler *c, identifier id,
    expr_ty annotation, Py_ssize_t *annotations_len, location loc)
{
    if (!annotation) {
        return SUCCESS;
    }
    PyObject *mangled = _Py_Mangle(c->u->u_private, id);
    if (!mangled) {
        return ERROR;
    }
    ADDOP_LOAD_CONST(c, loc, mangled);
    Py_DECREF(mangled);

    if (c->c_future.ff_features & CO_FUTURE_ANNOTATIONS) {
        VISIT(c, annexpr, annotation);
    }
    else {
        if (annotation->kind == Starred_kind) {
            // *args: *Ts (where Ts is a TypeVarTuple).
            // Do [annotation_value] = [*Ts].
            // (Note that in theory we could end up here even for an argument
            // other than *args, but in practice the grammar doesn't allow it.)
            VISIT(c, expr, annotation->v.Starred.value);
            ADDOP_I(c, loc, UNPACK_SEQUENCE, (Py_ssize_t) 1);
        }
        else {
            VISIT(c, expr, annotation);
        }
    }
    *annotations_len += 2;
    return SUCCESS;
}

static int
compiler_visit_argannotations(struct compiler *c, asdl_arg_seq* args,
                              Py_ssize_t *annotations_len, location loc)
{
    int i;
    for (i = 0; i < asdl_seq_LEN(args); i++) {
        arg_ty arg = (arg_ty)asdl_seq_GET(args, i);
        RETURN_IF_ERROR(
            compiler_visit_argannotation(
                        c,
                        arg->arg,
                        arg->annotation,
                        annotations_len,
                        loc));
    }
    return SUCCESS;
}

static int
compiler_visit_annotations(struct compiler *c, location loc,
                           arguments_ty args, expr_ty returns)
{
    /* Push arg annotation names and values.
       The expressions are evaluated out-of-order wrt the source code.

       Return -1 on error, 0 if no annotations pushed, 1 if a annotations is pushed.
       */
    Py_ssize_t annotations_len = 0;

    RETURN_IF_ERROR(
        compiler_visit_argannotations(c, args->args, &annotations_len, loc));

    RETURN_IF_ERROR(
        compiler_visit_argannotations(c, args->posonlyargs, &annotations_len, loc));

    if (args->vararg && args->vararg->annotation) {
        RETURN_IF_ERROR(
            compiler_visit_argannotation(c, args->vararg->arg,
                                         args->vararg->annotation, &annotations_len, loc));
    }

    RETURN_IF_ERROR(
        compiler_visit_argannotations(c, args->kwonlyargs, &annotations_len, loc));

    if (args->kwarg && args->kwarg->annotation) {
        RETURN_IF_ERROR(
            compiler_visit_argannotation(c, args->kwarg->arg,
                                         args->kwarg->annotation, &annotations_len, loc));
    }

    RETURN_IF_ERROR(
        compiler_visit_argannotation(c, &_Py_ID(return), returns, &annotations_len, loc));

    if (annotations_len) {
        ADDOP_I(c, loc, BUILD_TUPLE, annotations_len);
        return 1;
    }

    return 0;
}

static int
compiler_visit_defaults(struct compiler *c, arguments_ty args,
                        location loc)
{
    VISIT_SEQ(c, expr, args->defaults);
    ADDOP_I(c, loc, BUILD_TUPLE, asdl_seq_LEN(args->defaults));
    return SUCCESS;
}

static Py_ssize_t
compiler_default_arguments(struct compiler *c, location loc,
                           arguments_ty args)
{
    Py_ssize_t funcflags = 0;
    if (args->defaults && asdl_seq_LEN(args->defaults) > 0) {
        RETURN_IF_ERROR(compiler_visit_defaults(c, args, loc));
        funcflags |= 0x01;
    }
    if (args->kwonlyargs) {
        int res = compiler_visit_kwonlydefaults(c, loc,
                                                args->kwonlyargs,
                                                args->kw_defaults);
        RETURN_IF_ERROR(res);
        if (res > 0) {
            funcflags |= 0x02;
        }
    }
    return funcflags;
}

static bool
forbidden_name(struct compiler *c, location loc, identifier name,
               expr_context_ty ctx)
{
    if (ctx == Store && _PyUnicode_EqualToASCIIString(name, "__debug__")) {
        compiler_error(c, loc, "cannot assign to __debug__");
        return true;
    }
    if (ctx == Del && _PyUnicode_EqualToASCIIString(name, "__debug__")) {
        compiler_error(c, loc, "cannot delete __debug__");
        return true;
    }
    return false;
}

static int
compiler_check_debug_one_arg(struct compiler *c, arg_ty arg)
{
    if (arg != NULL) {
        if (forbidden_name(c, LOC(arg), arg->arg, Store)) {
            return ERROR;
        }
    }
    return SUCCESS;
}

static int
compiler_check_debug_args_seq(struct compiler *c, asdl_arg_seq *args)
{
    if (args != NULL) {
        for (Py_ssize_t i = 0, n = asdl_seq_LEN(args); i < n; i++) {
            RETURN_IF_ERROR(
                compiler_check_debug_one_arg(c, asdl_seq_GET(args, i)));
        }
    }
    return SUCCESS;
}

static int
compiler_check_debug_args(struct compiler *c, arguments_ty args)
{
    RETURN_IF_ERROR(compiler_check_debug_args_seq(c, args->posonlyargs));
    RETURN_IF_ERROR(compiler_check_debug_args_seq(c, args->args));
    RETURN_IF_ERROR(compiler_check_debug_one_arg(c, args->vararg));
    RETURN_IF_ERROR(compiler_check_debug_args_seq(c, args->kwonlyargs));
    RETURN_IF_ERROR(compiler_check_debug_one_arg(c, args->kwarg));
    return SUCCESS;
}

static inline int
insert_instruction(basicblock *block, int pos, struct instr *instr) {
    RETURN_IF_ERROR(basicblock_next_instr(block));
    for (int i = block->b_iused - 1; i > pos; i--) {
        block->b_instr[i] = block->b_instr[i-1];
    }
    block->b_instr[pos] = *instr;
    return SUCCESS;
}

static int
wrap_in_stopiteration_handler(struct compiler *c)
{
    NEW_JUMP_TARGET_LABEL(c, handler);

    /* Insert SETUP_CLEANUP at start */
    struct instr setup = {
        .i_opcode = SETUP_CLEANUP,
        .i_oparg = handler.id,
        .i_loc = NO_LOCATION,
        .i_target = NULL,
    };
    RETURN_IF_ERROR(
        insert_instruction(c->u->u_cfg_builder.g_entryblock, 0, &setup));

    ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
    ADDOP(c, NO_LOCATION, RETURN_VALUE);
    USE_LABEL(c, handler);
    ADDOP_I(c, NO_LOCATION, CALL_INTRINSIC_1, INTRINSIC_STOPITERATION_ERROR);
    ADDOP_I(c, NO_LOCATION, RERAISE, 1);
    return SUCCESS;
}

static int
compiler_function(struct compiler *c, stmt_ty s, int is_async)
{
    PyCodeObject *co;
    PyObject *docstring = NULL;
    arguments_ty args;
    expr_ty returns;
    identifier name;
    asdl_expr_seq* decos;
    asdl_stmt_seq *body;
    Py_ssize_t i, funcflags;
    int annotations;
    int scope_type;
    int firstlineno;

    if (is_async) {
        assert(s->kind == AsyncFunctionDef_kind);

        args = s->v.AsyncFunctionDef.args;
        returns = s->v.AsyncFunctionDef.returns;
        decos = s->v.AsyncFunctionDef.decorator_list;
        name = s->v.AsyncFunctionDef.name;
        body = s->v.AsyncFunctionDef.body;

        scope_type = COMPILER_SCOPE_ASYNC_FUNCTION;
    } else {
        assert(s->kind == FunctionDef_kind);

        args = s->v.FunctionDef.args;
        returns = s->v.FunctionDef.returns;
        decos = s->v.FunctionDef.decorator_list;
        name = s->v.FunctionDef.name;
        body = s->v.FunctionDef.body;

        scope_type = COMPILER_SCOPE_FUNCTION;
    }

    RETURN_IF_ERROR(compiler_check_debug_args(c, args));
    RETURN_IF_ERROR(compiler_decorators(c, decos));

    firstlineno = s->lineno;
    if (asdl_seq_LEN(decos)) {
        firstlineno = ((expr_ty)asdl_seq_GET(decos, 0))->lineno;
    }

    location loc = LOC(s);
    funcflags = compiler_default_arguments(c, loc, args);
    if (funcflags == -1) {
        return ERROR;
    }
    annotations = compiler_visit_annotations(c, loc, args, returns);
    RETURN_IF_ERROR(annotations);
    if (annotations > 0) {
        funcflags |= 0x04;
    }

    RETURN_IF_ERROR(
        compiler_enter_scope(c, name, scope_type, (void *)s, firstlineno));

    /* if not -OO mode, add docstring */
    if (c->c_optimize < 2) {
        docstring = _PyAST_GetDocString(body);
    }
    if (compiler_add_const(c, docstring ? docstring : Py_None) < 0) {
        compiler_exit_scope(c);
        return ERROR;
    }

    c->u->u_argcount = asdl_seq_LEN(args->args);
    c->u->u_posonlyargcount = asdl_seq_LEN(args->posonlyargs);
    c->u->u_kwonlyargcount = asdl_seq_LEN(args->kwonlyargs);
    for (i = docstring ? 1 : 0; i < asdl_seq_LEN(body); i++) {
        VISIT_IN_SCOPE(c, stmt, (stmt_ty)asdl_seq_GET(body, i));
    }
    if (c->u->u_ste->ste_coroutine || c->u->u_ste->ste_generator) {
        if (wrap_in_stopiteration_handler(c) < 0) {
            compiler_exit_scope(c);
            return ERROR;
        }
    }
    co = assemble(c, 1);
    compiler_exit_scope(c);
    if (co == NULL) {
        Py_XDECREF(co);
        return ERROR;
    }
    if (compiler_make_closure(c, loc, co, funcflags) < 0) {
        Py_DECREF(co);
        return ERROR;
    }
    Py_DECREF(co);

    RETURN_IF_ERROR(compiler_apply_decorators(c, decos));
    return compiler_nameop(c, loc, name, Store);
}

static int
compiler_class(struct compiler *c, stmt_ty s)
{
    PyCodeObject *co;
    int i, firstlineno;
    asdl_expr_seq *decos = s->v.ClassDef.decorator_list;

    RETURN_IF_ERROR(compiler_decorators(c, decos));

    firstlineno = s->lineno;
    if (asdl_seq_LEN(decos)) {
        firstlineno = ((expr_ty)asdl_seq_GET(decos, 0))->lineno;
    }

    /* ultimately generate code for:
         <name> = __build_class__(<func>, <name>, *<bases>, **<keywords>)
       where:
         <func> is a zero arg function/closure created from the class body.
            It mutates its locals to build the class namespace.
         <name> is the class name
         <bases> is the positional arguments and *varargs argument
         <keywords> is the keyword arguments and **kwds argument
       This borrows from compiler_call.
    */
    /* 1. compile the class body into a code object */
    RETURN_IF_ERROR(
        compiler_enter_scope(c, s->v.ClassDef.name,
                             COMPILER_SCOPE_CLASS, (void *)s, firstlineno));

    /* this block represents what we do in the new scope */
    {
        location loc = LOCATION(firstlineno, firstlineno, 0, 0);
        /* use the class name for name mangling */
        Py_XSETREF(c->u->u_private, Py_NewRef(s->v.ClassDef.name));
        /* load (global) __name__ ... */
        if (compiler_nameop(c, loc, &_Py_ID(__name__), Load) < 0) {
            compiler_exit_scope(c);
            return ERROR;
        }
        /* ... and store it as __module__ */
        if (compiler_nameop(c, loc, &_Py_ID(__module__), Store) < 0) {
            compiler_exit_scope(c);
            return ERROR;
        }
        assert(c->u->u_qualname);
        ADDOP_LOAD_CONST(c, loc, c->u->u_qualname);
        if (compiler_nameop(c, loc, &_Py_ID(__qualname__), Store) < 0) {
            compiler_exit_scope(c);
            return ERROR;
        }
        /* compile the body proper */
        if (compiler_body(c, loc, s->v.ClassDef.body) < 0) {
            compiler_exit_scope(c);
            return ERROR;
        }
        /* The following code is artificial */
        /* Return __classcell__ if it is referenced, otherwise return None */
        if (c->u->u_ste->ste_needs_class_closure) {
            /* Store __classcell__ into class namespace & return it */
            i = compiler_lookup_arg(c->u->u_cellvars, &_Py_ID(__class__));
            if (i < 0) {
                compiler_exit_scope(c);
                return ERROR;
            }
            assert(i == 0);
            ADDOP_I(c, NO_LOCATION, LOAD_CLOSURE, i);
            ADDOP_I(c, NO_LOCATION, COPY, 1);
            if (compiler_nameop(c, NO_LOCATION, &_Py_ID(__classcell__), Store) < 0) {
                compiler_exit_scope(c);
                return ERROR;
            }
        }
        else {
            /* No methods referenced __class__, so just return None */
            assert(PyDict_GET_SIZE(c->u->u_cellvars) == 0);
            ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
        }
        ADDOP_IN_SCOPE(c, NO_LOCATION, RETURN_VALUE);
        /* create the code object */
        co = assemble(c, 1);
    }
    /* leave the new scope */
    compiler_exit_scope(c);
    if (co == NULL) {
        return ERROR;
    }

    location loc = LOC(s);
    /* 2. load the 'build_class' function */
    ADDOP(c, loc, PUSH_NULL);
    ADDOP(c, loc, LOAD_BUILD_CLASS);

    /* 3. load a function (or closure) made from the code object */
    if (compiler_make_closure(c, loc, co, 0) < 0) {
        Py_DECREF(co);
        return ERROR;
    }
    Py_DECREF(co);

    /* 4. load class name */
    ADDOP_LOAD_CONST(c, loc, s->v.ClassDef.name);

    /* 5. generate the rest of the code for the call */
    RETURN_IF_ERROR(compiler_call_helper(c, loc, 2,
                                         s->v.ClassDef.bases,
                                         s->v.ClassDef.keywords));

    /* 6. apply decorators */
    RETURN_IF_ERROR(compiler_apply_decorators(c, decos));

    /* 7. store into <name> */
    RETURN_IF_ERROR(compiler_nameop(c, loc, s->v.ClassDef.name, Store));
    return SUCCESS;
}

/* Return false if the expression is a constant value except named singletons.
   Return true otherwise. */
static bool
check_is_arg(expr_ty e)
{
    if (e->kind != Constant_kind) {
        return true;
    }
    PyObject *value = e->v.Constant.value;
    return (value == Py_None
         || value == Py_False
         || value == Py_True
         || value == Py_Ellipsis);
}

/* Check operands of identity checks ("is" and "is not").
   Emit a warning if any operand is a constant except named singletons.
 */
static int
check_compare(struct compiler *c, expr_ty e)
{
    Py_ssize_t i, n;
    bool left = check_is_arg(e->v.Compare.left);
    n = asdl_seq_LEN(e->v.Compare.ops);
    for (i = 0; i < n; i++) {
        cmpop_ty op = (cmpop_ty)asdl_seq_GET(e->v.Compare.ops, i);
        bool right = check_is_arg((expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
        if (op == Is || op == IsNot) {
            if (!right || !left) {
                const char *msg = (op == Is)
                        ? "\"is\" with a literal. Did you mean \"==\"?"
                        : "\"is not\" with a literal. Did you mean \"!=\"?";
                return compiler_warn(c, LOC(e), msg);
            }
        }
        left = right;
    }
    return SUCCESS;
}

static int compiler_addcompare(struct compiler *c, location loc,
                               cmpop_ty op)
{
    int cmp;
    switch (op) {
    case Eq:
        cmp = Py_EQ;
        break;
    case NotEq:
        cmp = Py_NE;
        break;
    case Lt:
        cmp = Py_LT;
        break;
    case LtE:
        cmp = Py_LE;
        break;
    case Gt:
        cmp = Py_GT;
        break;
    case GtE:
        cmp = Py_GE;
        break;
    case Is:
        ADDOP_I(c, loc, IS_OP, 0);
        return SUCCESS;
    case IsNot:
        ADDOP_I(c, loc, IS_OP, 1);
        return SUCCESS;
    case In:
        ADDOP_I(c, loc, CONTAINS_OP, 0);
        return SUCCESS;
    case NotIn:
        ADDOP_I(c, loc, CONTAINS_OP, 1);
        return SUCCESS;
    default:
        Py_UNREACHABLE();
    }
    ADDOP_I(c, loc, COMPARE_OP, cmp);
    return SUCCESS;
}



static int
compiler_jump_if(struct compiler *c, location loc,
                 expr_ty e, jump_target_label next, int cond)
{
    switch (e->kind) {
    case UnaryOp_kind:
        if (e->v.UnaryOp.op == Not) {
            return compiler_jump_if(c, loc, e->v.UnaryOp.operand, next, !cond);
        }
        /* fallback to general implementation */
        break;
    case BoolOp_kind: {
        asdl_expr_seq *s = e->v.BoolOp.values;
        Py_ssize_t i, n = asdl_seq_LEN(s) - 1;
        assert(n >= 0);
        int cond2 = e->v.BoolOp.op == Or;
        jump_target_label next2 = next;
        if (!cond2 != !cond) {
            NEW_JUMP_TARGET_LABEL(c, new_next2);
            next2 = new_next2;
        }
        for (i = 0; i < n; ++i) {
            RETURN_IF_ERROR(
                compiler_jump_if(c, loc, (expr_ty)asdl_seq_GET(s, i), next2, cond2));
        }
        RETURN_IF_ERROR(
            compiler_jump_if(c, loc, (expr_ty)asdl_seq_GET(s, n), next, cond));
        if (!SAME_LABEL(next2, next)) {
            USE_LABEL(c, next2);
        }
        return SUCCESS;
    }
    case IfExp_kind: {
        NEW_JUMP_TARGET_LABEL(c, end);
        NEW_JUMP_TARGET_LABEL(c, next2);
        RETURN_IF_ERROR(
            compiler_jump_if(c, loc, e->v.IfExp.test, next2, 0));
        RETURN_IF_ERROR(
            compiler_jump_if(c, loc, e->v.IfExp.body, next, cond));
        ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

        USE_LABEL(c, next2);
        RETURN_IF_ERROR(
            compiler_jump_if(c, loc, e->v.IfExp.orelse, next, cond));

        USE_LABEL(c, end);
        return SUCCESS;
    }
    case Compare_kind: {
        Py_ssize_t n = asdl_seq_LEN(e->v.Compare.ops) - 1;
        if (n > 0) {
            RETURN_IF_ERROR(check_compare(c, e));
            NEW_JUMP_TARGET_LABEL(c, cleanup);
            VISIT(c, expr, e->v.Compare.left);
            for (Py_ssize_t i = 0; i < n; i++) {
                VISIT(c, expr,
                    (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
                ADDOP_I(c, LOC(e), SWAP, 2);
                ADDOP_I(c, LOC(e), COPY, 2);
                ADDOP_COMPARE(c, LOC(e), asdl_seq_GET(e->v.Compare.ops, i));
                ADDOP_JUMP(c, LOC(e), POP_JUMP_IF_FALSE, cleanup);
            }
            VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, n));
            ADDOP_COMPARE(c, LOC(e), asdl_seq_GET(e->v.Compare.ops, n));
            ADDOP_JUMP(c, LOC(e), cond ? POP_JUMP_IF_TRUE : POP_JUMP_IF_FALSE, next);
            NEW_JUMP_TARGET_LABEL(c, end);
            ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

            USE_LABEL(c, cleanup);
            ADDOP(c, LOC(e), POP_TOP);
            if (!cond) {
                ADDOP_JUMP(c, NO_LOCATION, JUMP, next);
            }

            USE_LABEL(c, end);
            return SUCCESS;
        }
        /* fallback to general implementation */
        break;
    }
    default:
        /* fallback to general implementation */
        break;
    }

    /* general implementation */
    VISIT(c, expr, e);
    ADDOP_JUMP(c, LOC(e), cond ? POP_JUMP_IF_TRUE : POP_JUMP_IF_FALSE, next);
    return SUCCESS;
}

static int
compiler_ifexp(struct compiler *c, expr_ty e)
{
    assert(e->kind == IfExp_kind);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, next);

    RETURN_IF_ERROR(
        compiler_jump_if(c, LOC(e), e->v.IfExp.test, next, 0));

    VISIT(c, expr, e->v.IfExp.body);
    ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

    USE_LABEL(c, next);
    VISIT(c, expr, e->v.IfExp.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_lambda(struct compiler *c, expr_ty e)
{
    PyCodeObject *co;
    Py_ssize_t funcflags;
    arguments_ty args = e->v.Lambda.args;
    assert(e->kind == Lambda_kind);

    RETURN_IF_ERROR(compiler_check_debug_args(c, args));

    location loc = LOC(e);
    funcflags = compiler_default_arguments(c, loc, args);
    if (funcflags == -1) {
        return 0;
    }

    _Py_DECLARE_STR(anon_lambda, "<lambda>");
    RETURN_IF_ERROR(
        compiler_enter_scope(c, &_Py_STR(anon_lambda), COMPILER_SCOPE_LAMBDA,
                             (void *)e, e->lineno));

    /* Make None the first constant, so the lambda can't have a
       docstring. */
    RETURN_IF_ERROR(compiler_add_const(c, Py_None));

    c->u->u_argcount = asdl_seq_LEN(args->args);
    c->u->u_posonlyargcount = asdl_seq_LEN(args->posonlyargs);
    c->u->u_kwonlyargcount = asdl_seq_LEN(args->kwonlyargs);
    VISIT_IN_SCOPE(c, expr, e->v.Lambda.body);
    if (c->u->u_ste->ste_generator) {
        co = assemble(c, 0);
    }
    else {
        location loc = LOCATION(e->lineno, e->lineno, 0, 0);
        ADDOP_IN_SCOPE(c, loc, RETURN_VALUE);
        co = assemble(c, 1);
    }
    compiler_exit_scope(c);
    if (co == NULL) {
        return ERROR;
    }

    if (compiler_make_closure(c, loc, co, funcflags) < 0) {
        Py_DECREF(co);
        return ERROR;
    }
    Py_DECREF(co);

    return SUCCESS;
}

static int
compiler_if(struct compiler *c, stmt_ty s)
{
    jump_target_label next;
    assert(s->kind == If_kind);
    NEW_JUMP_TARGET_LABEL(c, end);
    if (asdl_seq_LEN(s->v.If.orelse)) {
        NEW_JUMP_TARGET_LABEL(c, orelse);
        next = orelse;
    }
    else {
        next = end;
    }
    RETURN_IF_ERROR(
        compiler_jump_if(c, LOC(s), s->v.If.test, next, 0));

    VISIT_SEQ(c, stmt, s->v.If.body);
    if (asdl_seq_LEN(s->v.If.orelse)) {
        ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

        USE_LABEL(c, next);
        VISIT_SEQ(c, stmt, s->v.If.orelse);
    }

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_for(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, cleanup);
    NEW_JUMP_TARGET_LABEL(c, end);

    RETURN_IF_ERROR(compiler_push_fblock(c, loc, FOR_LOOP, start, end, NULL));

    VISIT(c, expr, s->v.For.iter);
    ADDOP(c, loc, GET_ITER);

    USE_LABEL(c, start);
    ADDOP_JUMP(c, loc, FOR_ITER, cleanup);

    USE_LABEL(c, body);
    VISIT(c, expr, s->v.For.target);
    VISIT_SEQ(c, stmt, s->v.For.body);
    /* Mark jump as artificial */
    ADDOP_JUMP(c, NO_LOCATION, JUMP, start);

    USE_LABEL(c, cleanup);
    ADDOP(c, NO_LOCATION, END_FOR);

    compiler_pop_fblock(c, FOR_LOOP, start);

    VISIT_SEQ(c, stmt, s->v.For.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}


static int
compiler_async_for(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    if (IS_TOP_LEVEL_AWAIT(c)){
        c->u->u_ste->ste_coroutine = 1;
    } else if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION) {
        return compiler_error(c, loc, "'async for' outside async function");
    }

    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, end);

    VISIT(c, expr, s->v.AsyncFor.iter);
    ADDOP(c, loc, GET_AITER);

    USE_LABEL(c, start);
    RETURN_IF_ERROR(compiler_push_fblock(c, loc, FOR_LOOP, start, end, NULL));

    /* SETUP_FINALLY to guard the __anext__ call */
    ADDOP_JUMP(c, loc, SETUP_FINALLY, except);
    ADDOP(c, loc, GET_ANEXT);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADD_YIELD_FROM(c, loc, 1);
    ADDOP(c, loc, POP_BLOCK);  /* for SETUP_FINALLY */

    /* Success block for __anext__ */
    VISIT(c, expr, s->v.AsyncFor.target);
    VISIT_SEQ(c, stmt, s->v.AsyncFor.body);
    /* Mark jump as artificial */
    ADDOP_JUMP(c, NO_LOCATION, JUMP, start);

    compiler_pop_fblock(c, FOR_LOOP, start);

    /* Except block for __anext__ */
    USE_LABEL(c, except);

    /* Use same line number as the iterator,
     * as the END_ASYNC_FOR succeeds the `for`, not the body. */
    loc = LOC(s->v.AsyncFor.iter);
    ADDOP(c, loc, END_ASYNC_FOR);

    /* `else` block */
    VISIT_SEQ(c, stmt, s->v.For.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_while(struct compiler *c, stmt_ty s)
{
    NEW_JUMP_TARGET_LABEL(c, loop);
    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, anchor);

    USE_LABEL(c, loop);

    RETURN_IF_ERROR(compiler_push_fblock(c, LOC(s), WHILE_LOOP, loop, end, NULL));
    RETURN_IF_ERROR(compiler_jump_if(c, LOC(s), s->v.While.test, anchor, 0));

    USE_LABEL(c, body);
    VISIT_SEQ(c, stmt, s->v.While.body);
    RETURN_IF_ERROR(compiler_jump_if(c, LOC(s), s->v.While.test, body, 1));

    compiler_pop_fblock(c, WHILE_LOOP, loop);

    USE_LABEL(c, anchor);
    if (s->v.While.orelse) {
        VISIT_SEQ(c, stmt, s->v.While.orelse);
    }

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_return(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    int preserve_tos = ((s->v.Return.value != NULL) &&
                        (s->v.Return.value->kind != Constant_kind));
    if (c->u->u_ste->ste_type != FunctionBlock) {
        return compiler_error(c, loc, "'return' outside function");
    }
    if (s->v.Return.value != NULL &&
        c->u->u_ste->ste_coroutine && c->u->u_ste->ste_generator)
    {
        return compiler_error(c, loc, "'return' with value in async generator");
    }

    if (preserve_tos) {
        VISIT(c, expr, s->v.Return.value);
    } else {
        /* Emit instruction with line number for return value */
        if (s->v.Return.value != NULL) {
            loc = LOC(s->v.Return.value);
            ADDOP(c, loc, NOP);
        }
    }
    if (s->v.Return.value == NULL || s->v.Return.value->lineno != s->lineno) {
        loc = LOC(s);
        ADDOP(c, loc, NOP);
    }

    RETURN_IF_ERROR(compiler_unwind_fblock_stack(c, &loc, preserve_tos, NULL));
    if (s->v.Return.value == NULL) {
        ADDOP_LOAD_CONST(c, loc, Py_None);
    }
    else if (!preserve_tos) {
        ADDOP_LOAD_CONST(c, loc, s->v.Return.value->v.Constant.value);
    }
    ADDOP(c, loc, RETURN_VALUE);

    return SUCCESS;
}

static int
compiler_break(struct compiler *c, location loc)
{
    struct fblockinfo *loop = NULL;
    /* Emit instruction with line number */
    ADDOP(c, loc, NOP);
    RETURN_IF_ERROR(compiler_unwind_fblock_stack(c, &loc, 0, &loop));
    if (loop == NULL) {
        return compiler_error(c, loc, "'break' outside loop");
    }
    RETURN_IF_ERROR(compiler_unwind_fblock(c, &loc, loop, 0));
    ADDOP_JUMP(c, loc, JUMP, loop->fb_exit);
    return SUCCESS;
}

static int
compiler_continue(struct compiler *c, location loc)
{
    struct fblockinfo *loop = NULL;
    /* Emit instruction with line number */
    ADDOP(c, loc, NOP);
    RETURN_IF_ERROR(compiler_unwind_fblock_stack(c, &loc, 0, &loop));
    if (loop == NULL) {
        return compiler_error(c, loc, "'continue' not properly in loop");
    }
    ADDOP_JUMP(c, loc, JUMP, loop->fb_block);
    return SUCCESS;
}


static location
location_of_last_executing_statement(asdl_stmt_seq *stmts)
{
    for (Py_ssize_t i = asdl_seq_LEN(stmts) - 1; i >= 0; i++) {
        location loc = LOC((stmt_ty)asdl_seq_GET(stmts, i));
        if (loc.lineno > 0) {
            return loc;
        }
    }
    return NO_LOCATION;
}

/* Code generated for "try: <body> finally: <finalbody>" is as follows:

        SETUP_FINALLY           L
        <code for body>
        POP_BLOCK
        <code for finalbody>
        JUMP E
    L:
        <code for finalbody>
    E:

   The special instructions use the block stack.  Each block
   stack entry contains the instruction that created it (here
   SETUP_FINALLY), the level of the value stack at the time the
   block stack entry was created, and a label (here L).

   SETUP_FINALLY:
    Pushes the current value stack level and the label
    onto the block stack.
   POP_BLOCK:
    Pops en entry from the block stack.

   The block stack is unwound when an exception is raised:
   when a SETUP_FINALLY entry is found, the raised and the caught
   exceptions are pushed onto the value stack (and the exception
   condition is cleared), and the interpreter jumps to the label
   gotten from the block stack.
*/

static int
compiler_try_finally(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    /* `try` block */
    ADDOP_JUMP(c, loc, SETUP_FINALLY, end);

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, FINALLY_TRY, body, end,
                             s->v.Try.finalbody));

    if (s->v.Try.handlers && asdl_seq_LEN(s->v.Try.handlers)) {
        RETURN_IF_ERROR(compiler_try_except(c, s));
    }
    else {
        VISIT_SEQ(c, stmt, s->v.Try.body);
    }
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    compiler_pop_fblock(c, FINALLY_TRY, body);
    VISIT_SEQ(c, stmt, s->v.Try.finalbody);

    ADDOP_JUMP(c, NO_LOCATION, JUMP, exit);
    /* `finally` block */

    USE_LABEL(c, end);

    loc = NO_LOCATION;
    ADDOP_JUMP(c, loc, SETUP_CLEANUP, cleanup);
    ADDOP(c, loc, PUSH_EXC_INFO);
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, FINALLY_END, end, NO_LABEL, NULL));
    VISIT_SEQ(c, stmt, s->v.Try.finalbody);
    loc = location_of_last_executing_statement(s->v.Try.finalbody);
    compiler_pop_fblock(c, FINALLY_END, end);

    ADDOP_I(c, loc, RERAISE, 0);

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, loc);

    USE_LABEL(c, exit);
    return SUCCESS;
}

static int
compiler_try_star_finally(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);
    /* `try` block */
    ADDOP_JUMP(c, loc, SETUP_FINALLY, end);

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, FINALLY_TRY, body, end,
                             s->v.TryStar.finalbody));

    if (s->v.TryStar.handlers && asdl_seq_LEN(s->v.TryStar.handlers)) {
        RETURN_IF_ERROR(compiler_try_star_except(c, s));
    }
    else {
        VISIT_SEQ(c, stmt, s->v.TryStar.body);
    }
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    compiler_pop_fblock(c, FINALLY_TRY, body);
    VISIT_SEQ(c, stmt, s->v.TryStar.finalbody);

    ADDOP_JUMP(c, NO_LOCATION, JUMP, exit);

    /* `finally` block */
    USE_LABEL(c, end);

    loc = NO_LOCATION;
    ADDOP_JUMP(c, loc, SETUP_CLEANUP, cleanup);
    ADDOP(c, loc, PUSH_EXC_INFO);
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, FINALLY_END, end, NO_LABEL, NULL));

    VISIT_SEQ(c, stmt, s->v.TryStar.finalbody);
    loc = location_of_last_executing_statement(s->v.Try.finalbody);

    compiler_pop_fblock(c, FINALLY_END, end);
    ADDOP_I(c, loc, RERAISE, 0);

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, loc);

    USE_LABEL(c, exit);
    return SUCCESS;
}


/*
   Code generated for "try: S except E1 as V1: S1 except E2 as V2: S2 ...":
   (The contents of the value stack is shown in [], with the top
   at the right; 'tb' is trace-back info, 'val' the exception's
   associated value, and 'exc' the exception.)

   Value stack          Label   Instruction     Argument
   []                           SETUP_FINALLY   L1
   []                           <code for S>
   []                           POP_BLOCK
   []                           JUMP            L0

   [exc]                L1:     <evaluate E1>           )
   [exc, E1]                    CHECK_EXC_MATCH         )
   [exc, bool]                  POP_JUMP_IF_FALSE L2    ) only if E1
   [exc]                        <assign to V1>  (or POP if no V1)
   []                           <code for S1>
                                JUMP            L0

   [exc]                L2:     <evaluate E2>
   .............................etc.......................

   [exc]                Ln+1:   RERAISE     # re-raise exception

   []                   L0:     <next statement>

   Of course, parts are not generated if Vi or Ei is not present.
*/
static int
compiler_try_except(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    Py_ssize_t i, n;

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    ADDOP_JUMP(c, loc, SETUP_FINALLY, except);

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, TRY_EXCEPT, body, NO_LABEL, NULL));
    VISIT_SEQ(c, stmt, s->v.Try.body);
    compiler_pop_fblock(c, TRY_EXCEPT, body);
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    if (s->v.Try.orelse && asdl_seq_LEN(s->v.Try.orelse)) {
        VISIT_SEQ(c, stmt, s->v.Try.orelse);
    }
    ADDOP_JUMP(c, NO_LOCATION, JUMP, end);
    n = asdl_seq_LEN(s->v.Try.handlers);

    USE_LABEL(c, except);

    ADDOP_JUMP(c, NO_LOCATION, SETUP_CLEANUP, cleanup);
    ADDOP(c, NO_LOCATION, PUSH_EXC_INFO);

    /* Runtime will push a block here, so we need to account for that */
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, EXCEPTION_HANDLER, NO_LABEL, NO_LABEL, NULL));

    for (i = 0; i < n; i++) {
        excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
            s->v.Try.handlers, i);
        location loc = LOC(handler);
        if (!handler->v.ExceptHandler.type && i < n-1) {
            return compiler_error(c, loc, "default 'except:' must be last");
        }
        NEW_JUMP_TARGET_LABEL(c, next_except);
        except = next_except;
        if (handler->v.ExceptHandler.type) {
            VISIT(c, expr, handler->v.ExceptHandler.type);
            ADDOP(c, loc, CHECK_EXC_MATCH);
            ADDOP_JUMP(c, loc, POP_JUMP_IF_FALSE, except);
        }
        if (handler->v.ExceptHandler.name) {
            NEW_JUMP_TARGET_LABEL(c, cleanup_end);
            NEW_JUMP_TARGET_LABEL(c, cleanup_body);

            RETURN_IF_ERROR(
                compiler_nameop(c, loc, handler->v.ExceptHandler.name, Store));

            /*
              try:
                  # body
              except type as name:
                  try:
                      # body
                  finally:
                      name = None # in case body contains "del name"
                      del name
            */

            /* second try: */
            ADDOP_JUMP(c, loc, SETUP_CLEANUP, cleanup_end);

            USE_LABEL(c, cleanup_body);
            RETURN_IF_ERROR(
                compiler_push_fblock(c, loc, HANDLER_CLEANUP, cleanup_body,
                                     NO_LABEL, handler->v.ExceptHandler.name));

            /* second # body */
            VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
            compiler_pop_fblock(c, HANDLER_CLEANUP, cleanup_body);
            /* name = None; del name; # Mark as artificial */
            ADDOP(c, NO_LOCATION, POP_BLOCK);
            ADDOP(c, NO_LOCATION, POP_BLOCK);
            ADDOP(c, NO_LOCATION, POP_EXCEPT);
            ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));
            ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

            /* except: */
            USE_LABEL(c, cleanup_end);

            /* name = None; del name; # artificial */
            ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));

            ADDOP_I(c, NO_LOCATION, RERAISE, 1);
        }
        else {
            NEW_JUMP_TARGET_LABEL(c, cleanup_body);

            ADDOP(c, loc, POP_TOP); /* exc_value */

            USE_LABEL(c, cleanup_body);
            RETURN_IF_ERROR(
                compiler_push_fblock(c, loc, HANDLER_CLEANUP, cleanup_body,
                                     NO_LABEL, NULL));

            VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
            compiler_pop_fblock(c, HANDLER_CLEANUP, cleanup_body);
            ADDOP(c, NO_LOCATION, POP_BLOCK);
            ADDOP(c, NO_LOCATION, POP_EXCEPT);
            ADDOP_JUMP(c, NO_LOCATION, JUMP, end);
        }

        USE_LABEL(c, except);
    }
    /* artificial */
    compiler_pop_fblock(c, EXCEPTION_HANDLER, NO_LABEL);
    ADDOP_I(c, NO_LOCATION, RERAISE, 0);

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, NO_LOCATION);

    USE_LABEL(c, end);
    return SUCCESS;
}

/*
   Code generated for "try: S except* E1 as V1: S1 except* E2 as V2: S2 ...":
   (The contents of the value stack is shown in [], with the top
   at the right; 'tb' is trace-back info, 'val' the exception instance,
   and 'typ' the exception's type.)

   Value stack                   Label         Instruction     Argument
   []                                         SETUP_FINALLY         L1
   []                                         <code for S>
   []                                         POP_BLOCK
   []                                         JUMP                  L0

   [exc]                            L1:       COPY 1       )  save copy of the original exception
   [orig, exc]                                BUILD_LIST   )  list for raised/reraised excs ("result")
   [orig, exc, res]                           SWAP 2

   [orig, res, exc]                           <evaluate E1>
   [orig, res, exc, E1]                       CHECK_EG_MATCH
   [orig, red, rest/exc, match?]              COPY 1
   [orig, red, rest/exc, match?, match?]      POP_JUMP_IF_NOT_NONE  H1
   [orig, red, exc, None]                     POP_TOP
   [orig, red, exc]                           JUMP L2

   [orig, res, rest, match]         H1:       <assign to V1>  (or POP if no V1)

   [orig, res, rest]                          SETUP_FINALLY         R1
   [orig, res, rest]                          <code for S1>
   [orig, res, rest]                          JUMP                  L2

   [orig, res, rest, i, v]          R1:       LIST_APPEND   3 ) exc raised in except* body - add to res
   [orig, res, rest, i]                       POP

   [orig, res, rest]                L2:       <evaluate E2>
   .............................etc.......................

   [orig, res, rest]                Ln+1:     LIST_APPEND 1  ) add unhandled exc to res (could be None)

   [orig, res]                                PREP_RERAISE_STAR
   [exc]                                      COPY 1
   [exc, exc]                                 POP_JUMP_IF_NOT_NONE  RER
   [exc]                                      POP_TOP
   []                                         JUMP                  L0

   [exc]                            RER:      SWAP 2
   [exc, prev_exc_info]                       POP_EXCEPT
   [exc]                                      RERAISE               0

   []                               L0:       <next statement>
*/
static int
compiler_try_star_except(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);

    NEW_JUMP_TARGET_LABEL(c, body);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, orelse);
    NEW_JUMP_TARGET_LABEL(c, end);
    NEW_JUMP_TARGET_LABEL(c, cleanup);
    NEW_JUMP_TARGET_LABEL(c, reraise_star);

    ADDOP_JUMP(c, loc, SETUP_FINALLY, except);

    USE_LABEL(c, body);
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, TRY_EXCEPT, body, NO_LABEL, NULL));
    VISIT_SEQ(c, stmt, s->v.TryStar.body);
    compiler_pop_fblock(c, TRY_EXCEPT, body);
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    ADDOP_JUMP(c, NO_LOCATION, JUMP, orelse);
    Py_ssize_t n = asdl_seq_LEN(s->v.TryStar.handlers);

    USE_LABEL(c, except);

    ADDOP_JUMP(c, NO_LOCATION, SETUP_CLEANUP, cleanup);
    ADDOP(c, NO_LOCATION, PUSH_EXC_INFO);

    /* Runtime will push a block here, so we need to account for that */
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, EXCEPTION_GROUP_HANDLER,
                             NO_LABEL, NO_LABEL, "except handler"));

    for (Py_ssize_t i = 0; i < n; i++) {
        excepthandler_ty handler = (excepthandler_ty)asdl_seq_GET(
            s->v.TryStar.handlers, i);
        location loc = LOC(handler);
        NEW_JUMP_TARGET_LABEL(c, next_except);
        except = next_except;
        NEW_JUMP_TARGET_LABEL(c, handle_match);
        if (i == 0) {
            /* Push the original EG into the stack */
            /*
               [exc]            COPY 1
               [orig, exc]
            */
            ADDOP_I(c, loc, COPY, 1);

            /* create empty list for exceptions raised/reraise in the except* blocks */
            /*
               [orig, exc]       BUILD_LIST
               [orig, exc, []]   SWAP 2
               [orig, [], exc]
            */
            ADDOP_I(c, loc, BUILD_LIST, 0);
            ADDOP_I(c, loc, SWAP, 2);
        }
        if (handler->v.ExceptHandler.type) {
            VISIT(c, expr, handler->v.ExceptHandler.type);
            ADDOP(c, loc, CHECK_EG_MATCH);
            ADDOP_I(c, loc, COPY, 1);
            ADDOP_JUMP(c, loc, POP_JUMP_IF_NOT_NONE, handle_match);
            ADDOP(c, loc, POP_TOP);  // match
            ADDOP_JUMP(c, loc, JUMP, except);
        }

        USE_LABEL(c, handle_match);

        NEW_JUMP_TARGET_LABEL(c, cleanup_end);
        NEW_JUMP_TARGET_LABEL(c, cleanup_body);

        if (handler->v.ExceptHandler.name) {
            RETURN_IF_ERROR(
                compiler_nameop(c, loc, handler->v.ExceptHandler.name, Store));
        }
        else {
            ADDOP(c, loc, POP_TOP);  // match
        }

        /*
          try:
              # body
          except type as name:
              try:
                  # body
              finally:
                  name = None # in case body contains "del name"
                  del name
        */
        /* second try: */
        ADDOP_JUMP(c, loc, SETUP_CLEANUP, cleanup_end);

        USE_LABEL(c, cleanup_body);
        RETURN_IF_ERROR(
            compiler_push_fblock(c, loc, HANDLER_CLEANUP, cleanup_body,
                                 NO_LABEL, handler->v.ExceptHandler.name));

        /* second # body */
        VISIT_SEQ(c, stmt, handler->v.ExceptHandler.body);
        compiler_pop_fblock(c, HANDLER_CLEANUP, cleanup_body);
        /* name = None; del name; # artificial */
        ADDOP(c, NO_LOCATION, POP_BLOCK);
        if (handler->v.ExceptHandler.name) {
            ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));
        }
        ADDOP_JUMP(c, NO_LOCATION, JUMP, except);

        /* except: */
        USE_LABEL(c, cleanup_end);

        /* name = None; del name; # artificial */
        if (handler->v.ExceptHandler.name) {
            ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Store));
            RETURN_IF_ERROR(
                compiler_nameop(c, NO_LOCATION, handler->v.ExceptHandler.name, Del));
        }

        /* add exception raised to the res list */
        ADDOP_I(c, NO_LOCATION, LIST_APPEND, 3); // exc
        ADDOP(c, NO_LOCATION, POP_TOP); // lasti
        ADDOP_JUMP(c, NO_LOCATION, JUMP, except);

        USE_LABEL(c, except);

        if (i == n - 1) {
            /* Add exc to the list (if not None it's the unhandled part of the EG) */
            ADDOP_I(c, NO_LOCATION, LIST_APPEND, 1);
            ADDOP_JUMP(c, NO_LOCATION, JUMP, reraise_star);
        }
    }
    /* artificial */
    compiler_pop_fblock(c, EXCEPTION_GROUP_HANDLER, NO_LABEL);
    NEW_JUMP_TARGET_LABEL(c, reraise);

    USE_LABEL(c, reraise_star);
    ADDOP(c, NO_LOCATION, PREP_RERAISE_STAR);
    ADDOP_I(c, NO_LOCATION, COPY, 1);
    ADDOP_JUMP(c, NO_LOCATION, POP_JUMP_IF_NOT_NONE, reraise);

    /* Nothing to reraise */
    ADDOP(c, NO_LOCATION, POP_TOP);
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    ADDOP(c, NO_LOCATION, POP_EXCEPT);
    ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

    USE_LABEL(c, reraise);
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    ADDOP_I(c, NO_LOCATION, SWAP, 2);
    ADDOP(c, NO_LOCATION, POP_EXCEPT);
    ADDOP_I(c, NO_LOCATION, RERAISE, 0);

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, NO_LOCATION);

    USE_LABEL(c, orelse);
    VISIT_SEQ(c, stmt, s->v.TryStar.orelse);

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_try(struct compiler *c, stmt_ty s) {
    if (s->v.Try.finalbody && asdl_seq_LEN(s->v.Try.finalbody))
        return compiler_try_finally(c, s);
    else
        return compiler_try_except(c, s);
}

static int
compiler_try_star(struct compiler *c, stmt_ty s)
{
    if (s->v.TryStar.finalbody && asdl_seq_LEN(s->v.TryStar.finalbody)) {
        return compiler_try_star_finally(c, s);
    }
    else {
        return compiler_try_star_except(c, s);
    }
}

static int
compiler_import_as(struct compiler *c, location loc,
                   identifier name, identifier asname)
{
    /* The IMPORT_NAME opcode was already generated.  This function
       merely needs to bind the result to a name.

       If there is a dot in name, we need to split it and emit a
       IMPORT_FROM for each name.
    */
    Py_ssize_t len = PyUnicode_GET_LENGTH(name);
    Py_ssize_t dot = PyUnicode_FindChar(name, '.', 0, len, 1);
    if (dot == -2) {
        return ERROR;
    }
    if (dot != -1) {
        /* Consume the base module name to get the first attribute */
        while (1) {
            Py_ssize_t pos = dot + 1;
            PyObject *attr;
            dot = PyUnicode_FindChar(name, '.', pos, len, 1);
            if (dot == -2) {
                return ERROR;
            }
            attr = PyUnicode_Substring(name, pos, (dot != -1) ? dot : len);
            if (!attr) {
                return ERROR;
            }
            ADDOP_N(c, loc, IMPORT_FROM, attr, names);
            if (dot == -1) {
                break;
            }
            ADDOP_I(c, loc, SWAP, 2);
            ADDOP(c, loc, POP_TOP);
        }
        RETURN_IF_ERROR(compiler_nameop(c, loc, asname, Store));
        ADDOP(c, loc, POP_TOP);
        return SUCCESS;
    }
    return compiler_nameop(c, loc, asname, Store);
}

static int
compiler_import(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    /* The Import node stores a module name like a.b.c as a single
       string.  This is convenient for all cases except
         import a.b.c as d
       where we need to parse that string to extract the individual
       module names.
       XXX Perhaps change the representation to make this case simpler?
     */
    Py_ssize_t i, n = asdl_seq_LEN(s->v.Import.names);

    PyObject *zero = _PyLong_GetZero();  // borrowed reference
    for (i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.Import.names, i);
        int r;

        ADDOP_LOAD_CONST(c, loc, zero);
        ADDOP_LOAD_CONST(c, loc, Py_None);
        ADDOP_NAME(c, loc, IMPORT_NAME, alias->name, names);

        if (alias->asname) {
            r = compiler_import_as(c, loc, alias->name, alias->asname);
            RETURN_IF_ERROR(r);
        }
        else {
            identifier tmp = alias->name;
            Py_ssize_t dot = PyUnicode_FindChar(
                alias->name, '.', 0, PyUnicode_GET_LENGTH(alias->name), 1);
            if (dot != -1) {
                tmp = PyUnicode_Substring(alias->name, 0, dot);
                if (tmp == NULL) {
                    return ERROR;
                }
            }
            r = compiler_nameop(c, loc, tmp, Store);
            if (dot != -1) {
                Py_DECREF(tmp);
            }
            RETURN_IF_ERROR(r);
        }
    }
    return SUCCESS;
}

static int
compiler_from_import(struct compiler *c, stmt_ty s)
{
    Py_ssize_t n = asdl_seq_LEN(s->v.ImportFrom.names);

    ADDOP_LOAD_CONST_NEW(c, LOC(s), PyLong_FromLong(s->v.ImportFrom.level));

    PyObject *names = PyTuple_New(n);
    if (!names) {
        return ERROR;
    }

    /* build up the names */
    for (Py_ssize_t i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, i);
        PyTuple_SET_ITEM(names, i, Py_NewRef(alias->name));
    }

    if (location_is_after(LOC(s), c->c_future.ff_location) &&
        s->v.ImportFrom.module &&
        _PyUnicode_EqualToASCIIString(s->v.ImportFrom.module, "__future__"))
    {
        Py_DECREF(names);
        return compiler_error(c, LOC(s), "from __future__ imports must occur "
                              "at the beginning of the file");
    }
    ADDOP_LOAD_CONST_NEW(c, LOC(s), names);

    if (s->v.ImportFrom.module) {
        ADDOP_NAME(c, LOC(s), IMPORT_NAME, s->v.ImportFrom.module, names);
    }
    else {
        _Py_DECLARE_STR(empty, "");
        ADDOP_NAME(c, LOC(s), IMPORT_NAME, &_Py_STR(empty), names);
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        alias_ty alias = (alias_ty)asdl_seq_GET(s->v.ImportFrom.names, i);
        identifier store_name;

        if (i == 0 && PyUnicode_READ_CHAR(alias->name, 0) == '*') {
            assert(n == 1);
            ADDOP_I(c, LOC(s), CALL_INTRINSIC_1, INTRINSIC_IMPORT_STAR);
            ADDOP(c, NO_LOCATION, POP_TOP);
            return SUCCESS;
        }

        ADDOP_NAME(c, LOC(s), IMPORT_FROM, alias->name, names);
        store_name = alias->name;
        if (alias->asname) {
            store_name = alias->asname;
        }

        RETURN_IF_ERROR(compiler_nameop(c, LOC(s), store_name, Store));
    }
    /* remove imported module */
    ADDOP(c, LOC(s), POP_TOP);
    return SUCCESS;
}

static int
compiler_assert(struct compiler *c, stmt_ty s)
{
    /* Always emit a warning if the test is a non-zero length tuple */
    if ((s->v.Assert.test->kind == Tuple_kind &&
        asdl_seq_LEN(s->v.Assert.test->v.Tuple.elts) > 0) ||
        (s->v.Assert.test->kind == Constant_kind &&
         PyTuple_Check(s->v.Assert.test->v.Constant.value) &&
         PyTuple_Size(s->v.Assert.test->v.Constant.value) > 0))
    {
        RETURN_IF_ERROR(
            compiler_warn(c, LOC(s), "assertion is always true, "
                                     "perhaps remove parentheses?"));
    }
    if (c->c_optimize) {
        return SUCCESS;
    }
    NEW_JUMP_TARGET_LABEL(c, end);
    RETURN_IF_ERROR(compiler_jump_if(c, LOC(s), s->v.Assert.test, end, 1));
    ADDOP(c, LOC(s), LOAD_ASSERTION_ERROR);
    if (s->v.Assert.msg) {
        VISIT(c, expr, s->v.Assert.msg);
        ADDOP_I(c, LOC(s), CALL, 0);
    }
    ADDOP_I(c, LOC(s), RAISE_VARARGS, 1);

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_stmt_expr(struct compiler *c, location loc, expr_ty value)
{
    if (c->c_interactive && c->c_nestlevel <= 1) {
        VISIT(c, expr, value);
        ADDOP_I(c, loc, CALL_INTRINSIC_1, INTRINSIC_PRINT);
        ADDOP(c, NO_LOCATION, POP_TOP);
        return SUCCESS;
    }

    if (value->kind == Constant_kind) {
        /* ignore constant statement */
        ADDOP(c, loc, NOP);
        return SUCCESS;
    }

    VISIT(c, expr, value);
    ADDOP(c, NO_LOCATION, POP_TOP); /* artificial */
    return SUCCESS;
}

static int
compiler_visit_stmt(struct compiler *c, stmt_ty s)
{

    switch (s->kind) {
    case FunctionDef_kind:
        return compiler_function(c, s, 0);
    case ClassDef_kind:
        return compiler_class(c, s);
    case Return_kind:
        return compiler_return(c, s);
    case Delete_kind:
        VISIT_SEQ(c, expr, s->v.Delete.targets)
        break;
    case Assign_kind:
    {
        Py_ssize_t n = asdl_seq_LEN(s->v.Assign.targets);
        VISIT(c, expr, s->v.Assign.value);
        for (Py_ssize_t i = 0; i < n; i++) {
            if (i < n - 1) {
                ADDOP_I(c, LOC(s), COPY, 1);
            }
            VISIT(c, expr,
                  (expr_ty)asdl_seq_GET(s->v.Assign.targets, i));
        }
        break;
    }
    case AugAssign_kind:
        return compiler_augassign(c, s);
    case AnnAssign_kind:
        return compiler_annassign(c, s);
    case For_kind:
        return compiler_for(c, s);
    case While_kind:
        return compiler_while(c, s);
    case If_kind:
        return compiler_if(c, s);
    case Match_kind:
        return compiler_match(c, s);
    case Raise_kind:
    {
        Py_ssize_t n = 0;
        if (s->v.Raise.exc) {
            VISIT(c, expr, s->v.Raise.exc);
            n++;
            if (s->v.Raise.cause) {
                VISIT(c, expr, s->v.Raise.cause);
                n++;
            }
        }
        ADDOP_I(c, LOC(s), RAISE_VARARGS, (int)n);
        break;
    }
    case Try_kind:
        return compiler_try(c, s);
    case TryStar_kind:
        return compiler_try_star(c, s);
    case Assert_kind:
        return compiler_assert(c, s);
    case Import_kind:
        return compiler_import(c, s);
    case ImportFrom_kind:
        return compiler_from_import(c, s);
    case Global_kind:
    case Nonlocal_kind:
        break;
    case Expr_kind:
    {
        return compiler_stmt_expr(c, LOC(s), s->v.Expr.value);
    }
    case Pass_kind:
    {
        ADDOP(c, LOC(s), NOP);
        break;
    }
    case Break_kind:
    {
        return compiler_break(c, LOC(s));
    }
    case Continue_kind:
    {
        return compiler_continue(c, LOC(s));
    }
    case With_kind:
        return compiler_with(c, s, 0);
    case AsyncFunctionDef_kind:
        return compiler_function(c, s, 1);
    case AsyncWith_kind:
        return compiler_async_with(c, s, 0);
    case AsyncFor_kind:
        return compiler_async_for(c, s);
    }

    return SUCCESS;
}

static int
unaryop(unaryop_ty op)
{
    switch (op) {
    case Invert:
        return UNARY_INVERT;
    case Not:
        return UNARY_NOT;
    case USub:
        return UNARY_NEGATIVE;
    default:
        PyErr_Format(PyExc_SystemError,
            "unary op %d should not be possible", op);
        return 0;
    }
}

static int
addop_binary(struct compiler *c, location loc, operator_ty binop,
             bool inplace)
{
    int oparg;
    switch (binop) {
        case Add:
            oparg = inplace ? NB_INPLACE_ADD : NB_ADD;
            break;
        case Sub:
            oparg = inplace ? NB_INPLACE_SUBTRACT : NB_SUBTRACT;
            break;
        case Mult:
            oparg = inplace ? NB_INPLACE_MULTIPLY : NB_MULTIPLY;
            break;
        case MatMult:
            oparg = inplace ? NB_INPLACE_MATRIX_MULTIPLY : NB_MATRIX_MULTIPLY;
            break;
        case Div:
            oparg = inplace ? NB_INPLACE_TRUE_DIVIDE : NB_TRUE_DIVIDE;
            break;
        case Mod:
            oparg = inplace ? NB_INPLACE_REMAINDER : NB_REMAINDER;
            break;
        case Pow:
            oparg = inplace ? NB_INPLACE_POWER : NB_POWER;
            break;
        case LShift:
            oparg = inplace ? NB_INPLACE_LSHIFT : NB_LSHIFT;
            break;
        case RShift:
            oparg = inplace ? NB_INPLACE_RSHIFT : NB_RSHIFT;
            break;
        case BitOr:
            oparg = inplace ? NB_INPLACE_OR : NB_OR;
            break;
        case BitXor:
            oparg = inplace ? NB_INPLACE_XOR : NB_XOR;
            break;
        case BitAnd:
            oparg = inplace ? NB_INPLACE_AND : NB_AND;
            break;
        case FloorDiv:
            oparg = inplace ? NB_INPLACE_FLOOR_DIVIDE : NB_FLOOR_DIVIDE;
            break;
        default:
            PyErr_Format(PyExc_SystemError, "%s op %d should not be possible",
                         inplace ? "inplace" : "binary", binop);
            return ERROR;
    }
    ADDOP_I(c, loc, BINARY_OP, oparg);
    return SUCCESS;
}


static int
addop_yield(struct compiler *c, location loc) {
    if (c->u->u_ste->ste_generator && c->u->u_ste->ste_coroutine) {
        ADDOP_I(c, loc, CALL_INTRINSIC_1, INTRINSIC_ASYNC_GEN_WRAP);
    }
    ADDOP_I(c, loc, YIELD_VALUE, 0);
    ADDOP_I(c, loc, RESUME, 1);
    return SUCCESS;
}

static int
compiler_nameop(struct compiler *c, location loc,
                identifier name, expr_context_ty ctx)
{
    int op, scope;
    Py_ssize_t arg;
    enum { OP_FAST, OP_GLOBAL, OP_DEREF, OP_NAME } optype;

    PyObject *dict = c->u->u_names;
    PyObject *mangled;

    assert(!_PyUnicode_EqualToASCIIString(name, "None") &&
           !_PyUnicode_EqualToASCIIString(name, "True") &&
           !_PyUnicode_EqualToASCIIString(name, "False"));

    if (forbidden_name(c, loc, name, ctx)) {
        return ERROR;
    }

    mangled = _Py_Mangle(c->u->u_private, name);
    if (!mangled) {
        return ERROR;
    }

    op = 0;
    optype = OP_NAME;
    scope = _PyST_GetScope(c->u->u_ste, mangled);
    switch (scope) {
    case FREE:
        dict = c->u->u_freevars;
        optype = OP_DEREF;
        break;
    case CELL:
        dict = c->u->u_cellvars;
        optype = OP_DEREF;
        break;
    case LOCAL:
        if (c->u->u_ste->ste_type == FunctionBlock)
            optype = OP_FAST;
        break;
    case GLOBAL_IMPLICIT:
        if (c->u->u_ste->ste_type == FunctionBlock)
            optype = OP_GLOBAL;
        break;
    case GLOBAL_EXPLICIT:
        optype = OP_GLOBAL;
        break;
    default:
        /* scope can be 0 */
        break;
    }

    /* XXX Leave assert here, but handle __doc__ and the like better */
    assert(scope || PyUnicode_READ_CHAR(name, 0) == '_');

    switch (optype) {
    case OP_DEREF:
        switch (ctx) {
        case Load:
            op = (c->u->u_ste->ste_type == ClassBlock) ? LOAD_CLASSDEREF : LOAD_DEREF;
            break;
        case Store: op = STORE_DEREF; break;
        case Del: op = DELETE_DEREF; break;
        }
        break;
    case OP_FAST:
        switch (ctx) {
        case Load: op = LOAD_FAST; break;
        case Store: op = STORE_FAST; break;
        case Del: op = DELETE_FAST; break;
        }
        ADDOP_N(c, loc, op, mangled, varnames);
        return SUCCESS;
    case OP_GLOBAL:
        switch (ctx) {
        case Load: op = LOAD_GLOBAL; break;
        case Store: op = STORE_GLOBAL; break;
        case Del: op = DELETE_GLOBAL; break;
        }
        break;
    case OP_NAME:
        switch (ctx) {
        case Load: op = LOAD_NAME; break;
        case Store: op = STORE_NAME; break;
        case Del: op = DELETE_NAME; break;
        }
        break;
    }

    assert(op);
    arg = dict_add_o(dict, mangled);
    Py_DECREF(mangled);
    if (arg < 0) {
        return ERROR;
    }
    if (op == LOAD_GLOBAL) {
        arg <<= 1;
    }
    return cfg_builder_addop_i(CFG_BUILDER(c), op, arg, loc);
}

static int
compiler_boolop(struct compiler *c, expr_ty e)
{
    int jumpi;
    Py_ssize_t i, n;
    asdl_expr_seq *s;

    location loc = LOC(e);
    assert(e->kind == BoolOp_kind);
    if (e->v.BoolOp.op == And)
        jumpi = JUMP_IF_FALSE_OR_POP;
    else
        jumpi = JUMP_IF_TRUE_OR_POP;
    NEW_JUMP_TARGET_LABEL(c, end);
    s = e->v.BoolOp.values;
    n = asdl_seq_LEN(s) - 1;
    assert(n >= 0);
    for (i = 0; i < n; ++i) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(s, i));
        ADDOP_JUMP(c, loc, jumpi, end);
        NEW_JUMP_TARGET_LABEL(c, next);

        USE_LABEL(c, next);
    }
    VISIT(c, expr, (expr_ty)asdl_seq_GET(s, n));

    USE_LABEL(c, end);
    return SUCCESS;
}

static int
starunpack_helper(struct compiler *c, location loc,
                  asdl_expr_seq *elts, int pushed,
                  int build, int add, int extend, int tuple)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    if (n > 2 && are_all_items_const(elts, 0, n)) {
        PyObject *folded = PyTuple_New(n);
        if (folded == NULL) {
            return ERROR;
        }
        PyObject *val;
        for (Py_ssize_t i = 0; i < n; i++) {
            val = ((expr_ty)asdl_seq_GET(elts, i))->v.Constant.value;
            PyTuple_SET_ITEM(folded, i, Py_NewRef(val));
        }
        if (tuple && !pushed) {
            ADDOP_LOAD_CONST_NEW(c, loc, folded);
        } else {
            if (add == SET_ADD) {
                Py_SETREF(folded, PyFrozenSet_New(folded));
                if (folded == NULL) {
                    return ERROR;
                }
            }
            ADDOP_I(c, loc, build, pushed);
            ADDOP_LOAD_CONST_NEW(c, loc, folded);
            ADDOP_I(c, loc, extend, 1);
            if (tuple) {
                ADDOP_I(c, loc, CALL_INTRINSIC_1, INTRINSIC_LIST_TO_TUPLE);
            }
        }
        return SUCCESS;
    }

    int big = n+pushed > STACK_USE_GUIDELINE;
    int seen_star = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind) {
            seen_star = 1;
            break;
        }
    }
    if (!seen_star && !big) {
        for (Py_ssize_t i = 0; i < n; i++) {
            expr_ty elt = asdl_seq_GET(elts, i);
            VISIT(c, expr, elt);
        }
        if (tuple) {
            ADDOP_I(c, loc, BUILD_TUPLE, n+pushed);
        } else {
            ADDOP_I(c, loc, build, n+pushed);
        }
        return SUCCESS;
    }
    int sequence_built = 0;
    if (big) {
        ADDOP_I(c, loc, build, pushed);
        sequence_built = 1;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind) {
            if (sequence_built == 0) {
                ADDOP_I(c, loc, build, i+pushed);
                sequence_built = 1;
            }
            VISIT(c, expr, elt->v.Starred.value);
            ADDOP_I(c, loc, extend, 1);
        }
        else {
            VISIT(c, expr, elt);
            if (sequence_built) {
                ADDOP_I(c, loc, add, 1);
            }
        }
    }
    assert(sequence_built);
    if (tuple) {
        ADDOP_I(c, loc, CALL_INTRINSIC_1, INTRINSIC_LIST_TO_TUPLE);
    }
    return SUCCESS;
}

static int
unpack_helper(struct compiler *c, location loc, asdl_expr_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    int seen_star = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == Starred_kind && !seen_star) {
            if ((i >= (1 << 8)) ||
                (n-i-1 >= (INT_MAX >> 8))) {
                return compiler_error(c, loc,
                    "too many expressions in "
                    "star-unpacking assignment");
            }
            ADDOP_I(c, loc, UNPACK_EX, (i + ((n-i-1) << 8)));
            seen_star = 1;
        }
        else if (elt->kind == Starred_kind) {
            return compiler_error(c, loc,
                "multiple starred expressions in assignment");
        }
    }
    if (!seen_star) {
        ADDOP_I(c, loc, UNPACK_SEQUENCE, n);
    }
    return SUCCESS;
}

static int
assignment_helper(struct compiler *c, location loc, asdl_expr_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    RETURN_IF_ERROR(unpack_helper(c, loc, elts));
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty elt = asdl_seq_GET(elts, i);
        VISIT(c, expr, elt->kind != Starred_kind ? elt : elt->v.Starred.value);
    }
    return SUCCESS;
}

static int
compiler_list(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    asdl_expr_seq *elts = e->v.List.elts;
    if (e->v.List.ctx == Store) {
        return assignment_helper(c, loc, elts);
    }
    else if (e->v.List.ctx == Load) {
        return starunpack_helper(c, loc, elts, 0,
                                 BUILD_LIST, LIST_APPEND, LIST_EXTEND, 0);
    }
    else {
        VISIT_SEQ(c, expr, elts);
    }
    return SUCCESS;
}

static int
compiler_tuple(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    asdl_expr_seq *elts = e->v.Tuple.elts;
    if (e->v.Tuple.ctx == Store) {
        return assignment_helper(c, loc, elts);
    }
    else if (e->v.Tuple.ctx == Load) {
        return starunpack_helper(c, loc, elts, 0,
                                 BUILD_LIST, LIST_APPEND, LIST_EXTEND, 1);
    }
    else {
        VISIT_SEQ(c, expr, elts);
    }
    return SUCCESS;
}

static int
compiler_set(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    return starunpack_helper(c, loc, e->v.Set.elts, 0,
                             BUILD_SET, SET_ADD, SET_UPDATE, 0);
}

static bool
are_all_items_const(asdl_expr_seq *seq, Py_ssize_t begin, Py_ssize_t end)
{
    for (Py_ssize_t i = begin; i < end; i++) {
        expr_ty key = (expr_ty)asdl_seq_GET(seq, i);
        if (key == NULL || key->kind != Constant_kind) {
            return false;
        }
    }
    return true;
}

static int
compiler_subdict(struct compiler *c, expr_ty e, Py_ssize_t begin, Py_ssize_t end)
{
    Py_ssize_t i, n = end - begin;
    PyObject *keys, *key;
    int big = n*2 > STACK_USE_GUIDELINE;
    location loc = LOC(e);
    if (n > 1 && !big && are_all_items_const(e->v.Dict.keys, begin, end)) {
        for (i = begin; i < end; i++) {
            VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
        }
        keys = PyTuple_New(n);
        if (keys == NULL) {
            return SUCCESS;
        }
        for (i = begin; i < end; i++) {
            key = ((expr_ty)asdl_seq_GET(e->v.Dict.keys, i))->v.Constant.value;
            PyTuple_SET_ITEM(keys, i - begin, Py_NewRef(key));
        }
        ADDOP_LOAD_CONST_NEW(c, loc, keys);
        ADDOP_I(c, loc, BUILD_CONST_KEY_MAP, n);
        return SUCCESS;
    }
    if (big) {
        ADDOP_I(c, loc, BUILD_MAP, 0);
    }
    for (i = begin; i < end; i++) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.keys, i));
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
        if (big) {
            ADDOP_I(c, loc, MAP_ADD, 1);
        }
    }
    if (!big) {
        ADDOP_I(c, loc, BUILD_MAP, n);
    }
    return SUCCESS;
}

static int
compiler_dict(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    Py_ssize_t i, n, elements;
    int have_dict;
    int is_unpacking = 0;
    n = asdl_seq_LEN(e->v.Dict.values);
    have_dict = 0;
    elements = 0;
    for (i = 0; i < n; i++) {
        is_unpacking = (expr_ty)asdl_seq_GET(e->v.Dict.keys, i) == NULL;
        if (is_unpacking) {
            if (elements) {
                RETURN_IF_ERROR(compiler_subdict(c, e, i - elements, i));
                if (have_dict) {
                    ADDOP_I(c, loc, DICT_UPDATE, 1);
                }
                have_dict = 1;
                elements = 0;
            }
            if (have_dict == 0) {
                ADDOP_I(c, loc, BUILD_MAP, 0);
                have_dict = 1;
            }
            VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Dict.values, i));
            ADDOP_I(c, loc, DICT_UPDATE, 1);
        }
        else {
            if (elements*2 > STACK_USE_GUIDELINE) {
                RETURN_IF_ERROR(compiler_subdict(c, e, i - elements, i + 1));
                if (have_dict) {
                    ADDOP_I(c, loc, DICT_UPDATE, 1);
                }
                have_dict = 1;
                elements = 0;
            }
            else {
                elements++;
            }
        }
    }
    if (elements) {
        RETURN_IF_ERROR(compiler_subdict(c, e, n - elements, n));
        if (have_dict) {
            ADDOP_I(c, loc, DICT_UPDATE, 1);
        }
        have_dict = 1;
    }
    if (!have_dict) {
        ADDOP_I(c, loc, BUILD_MAP, 0);
    }
    return SUCCESS;
}

static int
compiler_compare(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    Py_ssize_t i, n;

    RETURN_IF_ERROR(check_compare(c, e));
    VISIT(c, expr, e->v.Compare.left);
    assert(asdl_seq_LEN(e->v.Compare.ops) > 0);
    n = asdl_seq_LEN(e->v.Compare.ops) - 1;
    if (n == 0) {
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, 0));
        ADDOP_COMPARE(c, loc, asdl_seq_GET(e->v.Compare.ops, 0));
    }
    else {
        NEW_JUMP_TARGET_LABEL(c, cleanup);
        for (i = 0; i < n; i++) {
            VISIT(c, expr,
                (expr_ty)asdl_seq_GET(e->v.Compare.comparators, i));
            ADDOP_I(c, loc, SWAP, 2);
            ADDOP_I(c, loc, COPY, 2);
            ADDOP_COMPARE(c, loc, asdl_seq_GET(e->v.Compare.ops, i));
            ADDOP_JUMP(c, loc, JUMP_IF_FALSE_OR_POP, cleanup);
        }
        VISIT(c, expr, (expr_ty)asdl_seq_GET(e->v.Compare.comparators, n));
        ADDOP_COMPARE(c, loc, asdl_seq_GET(e->v.Compare.ops, n));
        NEW_JUMP_TARGET_LABEL(c, end);
        ADDOP_JUMP(c, NO_LOCATION, JUMP, end);

        USE_LABEL(c, cleanup);
        ADDOP_I(c, loc, SWAP, 2);
        ADDOP(c, loc, POP_TOP);

        USE_LABEL(c, end);
    }
    return SUCCESS;
}

static PyTypeObject *
infer_type(expr_ty e)
{
    switch (e->kind) {
    case Tuple_kind:
        return &PyTuple_Type;
    case List_kind:
    case ListComp_kind:
        return &PyList_Type;
    case Dict_kind:
    case DictComp_kind:
        return &PyDict_Type;
    case Set_kind:
    case SetComp_kind:
        return &PySet_Type;
    case GeneratorExp_kind:
        return &PyGen_Type;
    case Lambda_kind:
        return &PyFunction_Type;
    case JoinedStr_kind:
    case FormattedValue_kind:
        return &PyUnicode_Type;
    case Constant_kind:
        return Py_TYPE(e->v.Constant.value);
    default:
        return NULL;
    }
}

static int
check_caller(struct compiler *c, expr_ty e)
{
    switch (e->kind) {
    case Constant_kind:
    case Tuple_kind:
    case List_kind:
    case ListComp_kind:
    case Dict_kind:
    case DictComp_kind:
    case Set_kind:
    case SetComp_kind:
    case GeneratorExp_kind:
    case JoinedStr_kind:
    case FormattedValue_kind: {
        location loc = LOC(e);
        return compiler_warn(c, loc, "'%.200s' object is not callable; "
                                     "perhaps you missed a comma?",
                                     infer_type(e)->tp_name);
    }
    default:
        return SUCCESS;
    }
}

static int
check_subscripter(struct compiler *c, expr_ty e)
{
    PyObject *v;

    switch (e->kind) {
    case Constant_kind:
        v = e->v.Constant.value;
        if (!(v == Py_None || v == Py_Ellipsis ||
              PyLong_Check(v) || PyFloat_Check(v) || PyComplex_Check(v) ||
              PyAnySet_Check(v)))
        {
            return SUCCESS;
        }
        /* fall through */
    case Set_kind:
    case SetComp_kind:
    case GeneratorExp_kind:
    case Lambda_kind: {
        location loc = LOC(e);
        return compiler_warn(c, loc, "'%.200s' object is not subscriptable; "
                                     "perhaps you missed a comma?",
                                     infer_type(e)->tp_name);
    }
    default:
        return SUCCESS;
    }
}

static int
check_index(struct compiler *c, expr_ty e, expr_ty s)
{
    PyObject *v;

    PyTypeObject *index_type = infer_type(s);
    if (index_type == NULL
        || PyType_FastSubclass(index_type, Py_TPFLAGS_LONG_SUBCLASS)
        || index_type == &PySlice_Type) {
        return SUCCESS;
    }

    switch (e->kind) {
    case Constant_kind:
        v = e->v.Constant.value;
        if (!(PyUnicode_Check(v) || PyBytes_Check(v) || PyTuple_Check(v))) {
            return SUCCESS;
        }
        /* fall through */
    case Tuple_kind:
    case List_kind:
    case ListComp_kind:
    case JoinedStr_kind:
    case FormattedValue_kind: {
        location loc = LOC(e);
        return compiler_warn(c, loc, "%.200s indices must be integers "
                                     "or slices, not %.200s; "
                                     "perhaps you missed a comma?",
                                     infer_type(e)->tp_name,
                                     index_type->tp_name);
    }
    default:
        return SUCCESS;
    }
}

static int
is_import_originated(struct compiler *c, expr_ty e)
{
    /* Check whether the global scope has an import named
     e, if it is a Name object. For not traversing all the
     scope stack every time this function is called, it will
     only check the global scope to determine whether something
     is imported or not. */

    if (e->kind != Name_kind) {
        return 0;
    }

    long flags = _PyST_GetSymbol(c->c_st->st_top, e->v.Name.id);
    return flags & DEF_IMPORT;
}

// If an attribute access spans multiple lines, update the current start
// location to point to the attribute name.
static location
update_start_location_to_match_attr(struct compiler *c, location loc,
                                    expr_ty attr)
{
    assert(attr->kind == Attribute_kind);
    if (loc.lineno != attr->end_lineno) {
        loc.lineno = attr->end_lineno;
        int len = (int)PyUnicode_GET_LENGTH(attr->v.Attribute.attr);
        if (len <= attr->end_col_offset) {
            loc.col_offset = attr->end_col_offset - len;
        }
        else {
            // GH-94694: Somebody's compiling weird ASTs. Just drop the columns:
            loc.col_offset = -1;
            loc.end_col_offset = -1;
        }
        // Make sure the end position still follows the start position, even for
        // weird ASTs:
        loc.end_lineno = Py_MAX(loc.lineno, loc.end_lineno);
        if (loc.lineno == loc.end_lineno) {
            loc.end_col_offset = Py_MAX(loc.col_offset, loc.end_col_offset);
        }
    }
    return loc;
}

// Return 1 if the method call was optimized, 0 if not, and -1 on error.
static int
maybe_optimize_method_call(struct compiler *c, expr_ty e)
{
    Py_ssize_t argsl, i, kwdsl;
    expr_ty meth = e->v.Call.func;
    asdl_expr_seq *args = e->v.Call.args;
    asdl_keyword_seq *kwds = e->v.Call.keywords;

    /* Check that the call node is an attribute access */
    if (meth->kind != Attribute_kind || meth->v.Attribute.ctx != Load) {
        return 0;
    }

    /* Check that the base object is not something that is imported */
    if (is_import_originated(c, meth->v.Attribute.value)) {
        return 0;
    }

    /* Check that there aren't too many arguments */
    argsl = asdl_seq_LEN(args);
    kwdsl = asdl_seq_LEN(kwds);
    if (argsl + kwdsl + (kwdsl != 0) >= STACK_USE_GUIDELINE) {
        return 0;
    }
    /* Check that there are no *varargs types of arguments. */
    for (i = 0; i < argsl; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        if (elt->kind == Starred_kind) {
            return 0;
        }
    }

    for (i = 0; i < kwdsl; i++) {
        keyword_ty kw = asdl_seq_GET(kwds, i);
        if (kw->arg == NULL) {
            return 0;
        }
    }
    /* Alright, we can optimize the code. */
    VISIT(c, expr, meth->v.Attribute.value);
    location loc = LOC(meth);
    loc = update_start_location_to_match_attr(c, loc, meth);
    ADDOP_NAME(c, loc, LOAD_METHOD, meth->v.Attribute.attr, names);
    VISIT_SEQ(c, expr, e->v.Call.args);

    if (kwdsl) {
        VISIT_SEQ(c, keyword, kwds);
        RETURN_IF_ERROR(
            compiler_call_simple_kw_helper(c, loc, kwds, kwdsl));
    }
    loc = update_start_location_to_match_attr(c, LOC(e), meth);
    ADDOP_I(c, loc, CALL, argsl + kwdsl);
    return 1;
}

static int
validate_keywords(struct compiler *c, asdl_keyword_seq *keywords)
{
    Py_ssize_t nkeywords = asdl_seq_LEN(keywords);
    for (Py_ssize_t i = 0; i < nkeywords; i++) {
        keyword_ty key = ((keyword_ty)asdl_seq_GET(keywords, i));
        if (key->arg == NULL) {
            continue;
        }
        location loc = LOC(key);
        if (forbidden_name(c, loc, key->arg, Store)) {
            return ERROR;
        }
        for (Py_ssize_t j = i + 1; j < nkeywords; j++) {
            keyword_ty other = ((keyword_ty)asdl_seq_GET(keywords, j));
            if (other->arg && !PyUnicode_Compare(key->arg, other->arg)) {
                compiler_error(c, LOC(other), "keyword argument repeated: %U", key->arg);
                return ERROR;
            }
        }
    }
    return SUCCESS;
}

static int
compiler_call(struct compiler *c, expr_ty e)
{
    RETURN_IF_ERROR(validate_keywords(c, e->v.Call.keywords));
    int ret = maybe_optimize_method_call(c, e);
    if (ret < 0) {
        return ERROR;
    }
    if (ret == 1) {
        return SUCCESS;
    }
    RETURN_IF_ERROR(check_caller(c, e->v.Call.func));
    location loc = LOC(e->v.Call.func);
    ADDOP(c, loc, PUSH_NULL);
    VISIT(c, expr, e->v.Call.func);
    loc = LOC(e);
    return compiler_call_helper(c, loc, 0,
                                e->v.Call.args,
                                e->v.Call.keywords);
}

static int
compiler_joined_str(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    Py_ssize_t value_count = asdl_seq_LEN(e->v.JoinedStr.values);
    if (value_count > STACK_USE_GUIDELINE) {
        _Py_DECLARE_STR(empty, "");
        ADDOP_LOAD_CONST_NEW(c, loc, Py_NewRef(&_Py_STR(empty)));
        ADDOP_NAME(c, loc, LOAD_METHOD, &_Py_ID(join), names);
        ADDOP_I(c, loc, BUILD_LIST, 0);
        for (Py_ssize_t i = 0; i < asdl_seq_LEN(e->v.JoinedStr.values); i++) {
            VISIT(c, expr, asdl_seq_GET(e->v.JoinedStr.values, i));
            ADDOP_I(c, loc, LIST_APPEND, 1);
        }
        ADDOP_I(c, loc, CALL, 1);
    }
    else {
        VISIT_SEQ(c, expr, e->v.JoinedStr.values);
        if (asdl_seq_LEN(e->v.JoinedStr.values) != 1) {
            ADDOP_I(c, loc, BUILD_STRING, asdl_seq_LEN(e->v.JoinedStr.values));
        }
    }
    return SUCCESS;
}

/* Used to implement f-strings. Format a single value. */
static int
compiler_formatted_value(struct compiler *c, expr_ty e)
{
    /* Our oparg encodes 2 pieces of information: the conversion
       character, and whether or not a format_spec was provided.

       Convert the conversion char to 3 bits:
           : 000  0x0  FVC_NONE   The default if nothing specified.
       !s  : 001  0x1  FVC_STR
       !r  : 010  0x2  FVC_REPR
       !a  : 011  0x3  FVC_ASCII

       next bit is whether or not we have a format spec:
       yes : 100  0x4
       no  : 000  0x0
    */

    int conversion = e->v.FormattedValue.conversion;
    int oparg;

    /* The expression to be formatted. */
    VISIT(c, expr, e->v.FormattedValue.value);

    switch (conversion) {
    case 's': oparg = FVC_STR;   break;
    case 'r': oparg = FVC_REPR;  break;
    case 'a': oparg = FVC_ASCII; break;
    case -1:  oparg = FVC_NONE;  break;
    default:
        PyErr_Format(PyExc_SystemError,
                     "Unrecognized conversion character %d", conversion);
        return ERROR;
    }
    if (e->v.FormattedValue.format_spec) {
        /* Evaluate the format spec, and update our opcode arg. */
        VISIT(c, expr, e->v.FormattedValue.format_spec);
        oparg |= FVS_HAVE_SPEC;
    }

    /* And push our opcode and oparg */
    location loc = LOC(e);
    ADDOP_I(c, loc, FORMAT_VALUE, oparg);

    return SUCCESS;
}

static int
compiler_subkwargs(struct compiler *c, location loc,
                   asdl_keyword_seq *keywords,
                   Py_ssize_t begin, Py_ssize_t end)
{
    Py_ssize_t i, n = end - begin;
    keyword_ty kw;
    PyObject *keys, *key;
    assert(n > 0);
    int big = n*2 > STACK_USE_GUIDELINE;
    if (n > 1 && !big) {
        for (i = begin; i < end; i++) {
            kw = asdl_seq_GET(keywords, i);
            VISIT(c, expr, kw->value);
        }
        keys = PyTuple_New(n);
        if (keys == NULL) {
            return ERROR;
        }
        for (i = begin; i < end; i++) {
            key = ((keyword_ty) asdl_seq_GET(keywords, i))->arg;
            PyTuple_SET_ITEM(keys, i - begin, Py_NewRef(key));
        }
        ADDOP_LOAD_CONST_NEW(c, loc, keys);
        ADDOP_I(c, loc, BUILD_CONST_KEY_MAP, n);
        return SUCCESS;
    }
    if (big) {
        ADDOP_I(c, NO_LOCATION, BUILD_MAP, 0);
    }
    for (i = begin; i < end; i++) {
        kw = asdl_seq_GET(keywords, i);
        ADDOP_LOAD_CONST(c, loc, kw->arg);
        VISIT(c, expr, kw->value);
        if (big) {
            ADDOP_I(c, NO_LOCATION, MAP_ADD, 1);
        }
    }
    if (!big) {
        ADDOP_I(c, loc, BUILD_MAP, n);
    }
    return SUCCESS;
}

/* Used by compiler_call_helper and maybe_optimize_method_call to emit
 * KW_NAMES before CALL.
 */
static int
compiler_call_simple_kw_helper(struct compiler *c, location loc,
                               asdl_keyword_seq *keywords, Py_ssize_t nkwelts)
{
    PyObject *names;
    names = PyTuple_New(nkwelts);
    if (names == NULL) {
        return ERROR;
    }
    for (int i = 0; i < nkwelts; i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        PyTuple_SET_ITEM(names, i, Py_NewRef(kw->arg));
    }
    Py_ssize_t arg = compiler_add_const(c, names);
    if (arg < 0) {
        return ERROR;
    }
    Py_DECREF(names);
    ADDOP_I(c, loc, KW_NAMES, arg);
    return SUCCESS;
}


/* shared code between compiler_call and compiler_class */
static int
compiler_call_helper(struct compiler *c, location loc,
                     int n, /* Args already pushed */
                     asdl_expr_seq *args,
                     asdl_keyword_seq *keywords)
{
    Py_ssize_t i, nseen, nelts, nkwelts;

    RETURN_IF_ERROR(validate_keywords(c, keywords));

    nelts = asdl_seq_LEN(args);
    nkwelts = asdl_seq_LEN(keywords);

    if (nelts + nkwelts*2 > STACK_USE_GUIDELINE) {
         goto ex_call;
    }
    for (i = 0; i < nelts; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        if (elt->kind == Starred_kind) {
            goto ex_call;
        }
    }
    for (i = 0; i < nkwelts; i++) {
        keyword_ty kw = asdl_seq_GET(keywords, i);
        if (kw->arg == NULL) {
            goto ex_call;
        }
    }

    /* No * or ** args, so can use faster calling sequence */
    for (i = 0; i < nelts; i++) {
        expr_ty elt = asdl_seq_GET(args, i);
        assert(elt->kind != Starred_kind);
        VISIT(c, expr, elt);
    }
    if (nkwelts) {
        VISIT_SEQ(c, keyword, keywords);
        RETURN_IF_ERROR(
            compiler_call_simple_kw_helper(c, loc, keywords, nkwelts));
    }
    ADDOP_I(c, loc, CALL, n + nelts + nkwelts);
    return SUCCESS;

ex_call:

    /* Do positional arguments. */
    if (n ==0 && nelts == 1 && ((expr_ty)asdl_seq_GET(args, 0))->kind == Starred_kind) {
        VISIT(c, expr, ((expr_ty)asdl_seq_GET(args, 0))->v.Starred.value);
    }
    else if (starunpack_helper(c, loc, args, n, BUILD_LIST,
                                 LIST_APPEND, LIST_EXTEND, 1) < 0) {
        return ERROR;
    }
    /* Then keyword arguments */
    if (nkwelts) {
        /* Has a new dict been pushed */
        int have_dict = 0;

        nseen = 0;  /* the number of keyword arguments on the stack following */
        for (i = 0; i < nkwelts; i++) {
            keyword_ty kw = asdl_seq_GET(keywords, i);
            if (kw->arg == NULL) {
                /* A keyword argument unpacking. */
                if (nseen) {
                    RETURN_IF_ERROR(compiler_subkwargs(c, loc, keywords, i - nseen, i));
                    if (have_dict) {
                        ADDOP_I(c, loc, DICT_MERGE, 1);
                    }
                    have_dict = 1;
                    nseen = 0;
                }
                if (!have_dict) {
                    ADDOP_I(c, loc, BUILD_MAP, 0);
                    have_dict = 1;
                }
                VISIT(c, expr, kw->value);
                ADDOP_I(c, loc, DICT_MERGE, 1);
            }
            else {
                nseen++;
            }
        }
        if (nseen) {
            /* Pack up any trailing keyword arguments. */
            RETURN_IF_ERROR(compiler_subkwargs(c, loc, keywords, nkwelts - nseen, nkwelts));
            if (have_dict) {
                ADDOP_I(c, loc, DICT_MERGE, 1);
            }
            have_dict = 1;
        }
        assert(have_dict);
    }
    ADDOP_I(c, loc, CALL_FUNCTION_EX, nkwelts > 0);
    return SUCCESS;
}


/* List and set comprehensions and generator expressions work by creating a
  nested function to perform the actual iteration. This means that the
  iteration variables don't leak into the current scope.
  The defined function is called immediately following its definition, with the
  result of that call being the result of the expression.
  The LC/SC version returns the populated container, while the GE version is
  flagged in symtable.c as a generator, so it returns the generator object
  when the function is called.

  Possible cleanups:
    - iterate over the generator sequence instead of using recursion
*/


static int
compiler_comprehension_generator(struct compiler *c, location loc,
                                 asdl_comprehension_seq *generators, int gen_index,
                                 int depth,
                                 expr_ty elt, expr_ty val, int type)
{
    comprehension_ty gen;
    gen = (comprehension_ty)asdl_seq_GET(generators, gen_index);
    if (gen->is_async) {
        return compiler_async_comprehension_generator(
            c, loc, generators, gen_index, depth, elt, val, type);
    } else {
        return compiler_sync_comprehension_generator(
            c, loc, generators, gen_index, depth, elt, val, type);
    }
}

static int
compiler_sync_comprehension_generator(struct compiler *c, location loc,
                                      asdl_comprehension_seq *generators,
                                      int gen_index, int depth,
                                      expr_ty elt, expr_ty val, int type)
{
    /* generate code for the iterator, then each of the ifs,
       and then write to the element */

    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, if_cleanup);
    NEW_JUMP_TARGET_LABEL(c, anchor);

    comprehension_ty gen = (comprehension_ty)asdl_seq_GET(generators,
                                                          gen_index);

    if (gen_index == 0) {
        /* Receive outermost iter as an implicit argument */
        c->u->u_argcount = 1;
        ADDOP_I(c, loc, LOAD_FAST, 0);
    }
    else {
        /* Sub-iter - calculate on the fly */
        /* Fast path for the temporary variable assignment idiom:
             for y in [f(x)]
         */
        asdl_expr_seq *elts;
        switch (gen->iter->kind) {
            case List_kind:
                elts = gen->iter->v.List.elts;
                break;
            case Tuple_kind:
                elts = gen->iter->v.Tuple.elts;
                break;
            default:
                elts = NULL;
        }
        if (asdl_seq_LEN(elts) == 1) {
            expr_ty elt = asdl_seq_GET(elts, 0);
            if (elt->kind != Starred_kind) {
                VISIT(c, expr, elt);
                start = NO_LABEL;
            }
        }
        if (IS_LABEL(start)) {
            VISIT(c, expr, gen->iter);
            ADDOP(c, loc, GET_ITER);
        }
    }
    if (IS_LABEL(start)) {
        depth++;
        USE_LABEL(c, start);
        ADDOP_JUMP(c, loc, FOR_ITER, anchor);
    }
    VISIT(c, expr, gen->target);

    /* XXX this needs to be cleaned up...a lot! */
    Py_ssize_t n = asdl_seq_LEN(gen->ifs);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty e = (expr_ty)asdl_seq_GET(gen->ifs, i);
        RETURN_IF_ERROR(compiler_jump_if(c, loc, e, if_cleanup, 0));
    }

    if (++gen_index < asdl_seq_LEN(generators)) {
        RETURN_IF_ERROR(
            compiler_comprehension_generator(c, loc,
                                             generators, gen_index, depth,
                                             elt, val, type));
    }

    location elt_loc = LOC(elt);

    /* only append after the last for generator */
    if (gen_index >= asdl_seq_LEN(generators)) {
        /* comprehension specific code */
        switch (type) {
        case COMP_GENEXP:
            VISIT(c, expr, elt);
            ADDOP_YIELD(c, elt_loc);
            ADDOP(c, elt_loc, POP_TOP);
            break;
        case COMP_LISTCOMP:
            VISIT(c, expr, elt);
            ADDOP_I(c, elt_loc, LIST_APPEND, depth + 1);
            break;
        case COMP_SETCOMP:
            VISIT(c, expr, elt);
            ADDOP_I(c, elt_loc, SET_ADD, depth + 1);
            break;
        case COMP_DICTCOMP:
            /* With '{k: v}', k is evaluated before v, so we do
               the same. */
            VISIT(c, expr, elt);
            VISIT(c, expr, val);
            elt_loc = LOCATION(elt->lineno,
                               val->end_lineno,
                               elt->col_offset,
                               val->end_col_offset);
            ADDOP_I(c, elt_loc, MAP_ADD, depth + 1);
            break;
        default:
            return ERROR;
        }
    }

    USE_LABEL(c, if_cleanup);
    if (IS_LABEL(start)) {
        ADDOP_JUMP(c, elt_loc, JUMP, start);

        USE_LABEL(c, anchor);
        ADDOP(c, NO_LOCATION, END_FOR);
    }

    return SUCCESS;
}

static int
compiler_async_comprehension_generator(struct compiler *c, location loc,
                                      asdl_comprehension_seq *generators,
                                      int gen_index, int depth,
                                      expr_ty elt, expr_ty val, int type)
{
    NEW_JUMP_TARGET_LABEL(c, start);
    NEW_JUMP_TARGET_LABEL(c, except);
    NEW_JUMP_TARGET_LABEL(c, if_cleanup);

    comprehension_ty gen = (comprehension_ty)asdl_seq_GET(generators,
                                                          gen_index);

    if (gen_index == 0) {
        /* Receive outermost iter as an implicit argument */
        c->u->u_argcount = 1;
        ADDOP_I(c, loc, LOAD_FAST, 0);
    }
    else {
        /* Sub-iter - calculate on the fly */
        VISIT(c, expr, gen->iter);
        ADDOP(c, loc, GET_AITER);
    }

    USE_LABEL(c, start);
    /* Runtime will push a block here, so we need to account for that */
    RETURN_IF_ERROR(
        compiler_push_fblock(c, loc, ASYNC_COMPREHENSION_GENERATOR,
                             start, NO_LABEL, NULL));

    ADDOP_JUMP(c, loc, SETUP_FINALLY, except);
    ADDOP(c, loc, GET_ANEXT);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADD_YIELD_FROM(c, loc, 1);
    ADDOP(c, loc, POP_BLOCK);
    VISIT(c, expr, gen->target);

    Py_ssize_t n = asdl_seq_LEN(gen->ifs);
    for (Py_ssize_t i = 0; i < n; i++) {
        expr_ty e = (expr_ty)asdl_seq_GET(gen->ifs, i);
        RETURN_IF_ERROR(compiler_jump_if(c, loc, e, if_cleanup, 0));
    }

    depth++;
    if (++gen_index < asdl_seq_LEN(generators)) {
        RETURN_IF_ERROR(
            compiler_comprehension_generator(c, loc,
                                             generators, gen_index, depth,
                                             elt, val, type));
    }

    location elt_loc = LOC(elt);
    /* only append after the last for generator */
    if (gen_index >= asdl_seq_LEN(generators)) {
        /* comprehension specific code */
        switch (type) {
        case COMP_GENEXP:
            VISIT(c, expr, elt);
            ADDOP_YIELD(c, elt_loc);
            ADDOP(c, elt_loc, POP_TOP);
            break;
        case COMP_LISTCOMP:
            VISIT(c, expr, elt);
            ADDOP_I(c, elt_loc, LIST_APPEND, depth + 1);
            break;
        case COMP_SETCOMP:
            VISIT(c, expr, elt);
            ADDOP_I(c, elt_loc, SET_ADD, depth + 1);
            break;
        case COMP_DICTCOMP:
            /* With '{k: v}', k is evaluated before v, so we do
               the same. */
            VISIT(c, expr, elt);
            VISIT(c, expr, val);
            elt_loc = LOCATION(elt->lineno,
                               val->end_lineno,
                               elt->col_offset,
                               val->end_col_offset);
            ADDOP_I(c, elt_loc, MAP_ADD, depth + 1);
            break;
        default:
            return ERROR;
        }
    }

    USE_LABEL(c, if_cleanup);
    ADDOP_JUMP(c, elt_loc, JUMP, start);

    compiler_pop_fblock(c, ASYNC_COMPREHENSION_GENERATOR, start);

    USE_LABEL(c, except);

    ADDOP(c, loc, END_ASYNC_FOR);

    return SUCCESS;
}

static int
compiler_comprehension(struct compiler *c, expr_ty e, int type,
                       identifier name, asdl_comprehension_seq *generators, expr_ty elt,
                       expr_ty val)
{
    PyCodeObject *co = NULL;
    comprehension_ty outermost;
    int scope_type = c->u->u_scope_type;
    int is_async_generator = 0;
    int is_top_level_await = IS_TOP_LEVEL_AWAIT(c);

    outermost = (comprehension_ty) asdl_seq_GET(generators, 0);
    if (compiler_enter_scope(c, name, COMPILER_SCOPE_COMPREHENSION,
                             (void *)e, e->lineno) < 0)
    {
        goto error;
    }
    location loc = LOC(e);

    is_async_generator = c->u->u_ste->ste_coroutine;

    if (is_async_generator && type != COMP_GENEXP &&
        scope_type != COMPILER_SCOPE_ASYNC_FUNCTION &&
        scope_type != COMPILER_SCOPE_COMPREHENSION &&
        !is_top_level_await)
    {
        compiler_error(c, loc, "asynchronous comprehension outside of "
                               "an asynchronous function");
        goto error_in_scope;
    }

    if (type != COMP_GENEXP) {
        int op;
        switch (type) {
        case COMP_LISTCOMP:
            op = BUILD_LIST;
            break;
        case COMP_SETCOMP:
            op = BUILD_SET;
            break;
        case COMP_DICTCOMP:
            op = BUILD_MAP;
            break;
        default:
            PyErr_Format(PyExc_SystemError,
                         "unknown comprehension type %d", type);
            goto error_in_scope;
        }

        ADDOP_I(c, loc, op, 0);
    }

    if (compiler_comprehension_generator(c, loc, generators, 0, 0,
                                         elt, val, type) < 0) {
        goto error_in_scope;
    }

    if (type != COMP_GENEXP) {
        ADDOP(c, LOC(e), RETURN_VALUE);
    }
    if (type == COMP_GENEXP) {
        if (wrap_in_stopiteration_handler(c) < 0) {
            goto error_in_scope;
        }
    }

    co = assemble(c, 1);
    compiler_exit_scope(c);
    if (is_top_level_await && is_async_generator){
        c->u->u_ste->ste_coroutine = 1;
    }
    if (co == NULL) {
        goto error;
    }

    loc = LOC(e);
    if (compiler_make_closure(c, loc, co, 0) < 0) {
        goto error;
    }
    Py_DECREF(co);

    VISIT(c, expr, outermost->iter);

    loc = LOC(e);
    if (outermost->is_async) {
        ADDOP(c, loc, GET_AITER);
    } else {
        ADDOP(c, loc, GET_ITER);
    }

    ADDOP_I(c, loc, CALL, 0);

    if (is_async_generator && type != COMP_GENEXP) {
        ADDOP_I(c, loc, GET_AWAITABLE, 0);
        ADDOP_LOAD_CONST(c, loc, Py_None);
        ADD_YIELD_FROM(c, loc, 1);
    }

    return SUCCESS;
error_in_scope:
    compiler_exit_scope(c);
error:
    Py_XDECREF(co);
    return ERROR;
}

static int
compiler_genexp(struct compiler *c, expr_ty e)
{
    assert(e->kind == GeneratorExp_kind);
    _Py_DECLARE_STR(anon_genexpr, "<genexpr>");
    return compiler_comprehension(c, e, COMP_GENEXP, &_Py_STR(anon_genexpr),
                                  e->v.GeneratorExp.generators,
                                  e->v.GeneratorExp.elt, NULL);
}

static int
compiler_listcomp(struct compiler *c, expr_ty e)
{
    assert(e->kind == ListComp_kind);
    _Py_DECLARE_STR(anon_listcomp, "<listcomp>");
    return compiler_comprehension(c, e, COMP_LISTCOMP, &_Py_STR(anon_listcomp),
                                  e->v.ListComp.generators,
                                  e->v.ListComp.elt, NULL);
}

static int
compiler_setcomp(struct compiler *c, expr_ty e)
{
    assert(e->kind == SetComp_kind);
    _Py_DECLARE_STR(anon_setcomp, "<setcomp>");
    return compiler_comprehension(c, e, COMP_SETCOMP, &_Py_STR(anon_setcomp),
                                  e->v.SetComp.generators,
                                  e->v.SetComp.elt, NULL);
}


static int
compiler_dictcomp(struct compiler *c, expr_ty e)
{
    assert(e->kind == DictComp_kind);
    _Py_DECLARE_STR(anon_dictcomp, "<dictcomp>");
    return compiler_comprehension(c, e, COMP_DICTCOMP, &_Py_STR(anon_dictcomp),
                                  e->v.DictComp.generators,
                                  e->v.DictComp.key, e->v.DictComp.value);
}


static int
compiler_visit_keyword(struct compiler *c, keyword_ty k)
{
    VISIT(c, expr, k->value);
    return SUCCESS;
}


static int
compiler_with_except_finish(struct compiler *c, jump_target_label cleanup) {
    NEW_JUMP_TARGET_LABEL(c, suppress);
    ADDOP_JUMP(c, NO_LOCATION, POP_JUMP_IF_TRUE, suppress);
    ADDOP_I(c, NO_LOCATION, RERAISE, 2);

    USE_LABEL(c, suppress);
    ADDOP(c, NO_LOCATION, POP_TOP); /* exc_value */
    ADDOP(c, NO_LOCATION, POP_BLOCK);
    ADDOP(c, NO_LOCATION, POP_EXCEPT);
    ADDOP(c, NO_LOCATION, POP_TOP);
    ADDOP(c, NO_LOCATION, POP_TOP);
    NEW_JUMP_TARGET_LABEL(c, exit);
    ADDOP_JUMP(c, NO_LOCATION, JUMP, exit);

    USE_LABEL(c, cleanup);
    POP_EXCEPT_AND_RERAISE(c, NO_LOCATION);

    USE_LABEL(c, exit);
    return SUCCESS;
}

/*
   Implements the async with statement.

   The semantics outlined in that PEP are as follows:

   async with EXPR as VAR:
       BLOCK

   It is implemented roughly as:

   context = EXPR
   exit = context.__aexit__  # not calling it
   value = await context.__aenter__()
   try:
       VAR = value  # if VAR present in the syntax
       BLOCK
   finally:
       if an exception was raised:
           exc = copy of (exception, instance, traceback)
       else:
           exc = (None, None, None)
       if not (await exit(*exc)):
           raise
 */
static int
compiler_async_with(struct compiler *c, stmt_ty s, int pos)
{
    location loc = LOC(s);
    withitem_ty item = asdl_seq_GET(s->v.AsyncWith.items, pos);

    assert(s->kind == AsyncWith_kind);
    if (IS_TOP_LEVEL_AWAIT(c)){
        c->u->u_ste->ste_coroutine = 1;
    } else if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION){
        return compiler_error(c, loc, "'async with' outside async function");
    }

    NEW_JUMP_TARGET_LABEL(c, block);
    NEW_JUMP_TARGET_LABEL(c, final);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    /* Evaluate EXPR */
    VISIT(c, expr, item->context_expr);

    ADDOP(c, loc, BEFORE_ASYNC_WITH);
    ADDOP_I(c, loc, GET_AWAITABLE, 1);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADD_YIELD_FROM(c, loc, 1);

    ADDOP_JUMP(c, loc, SETUP_WITH, final);

    /* SETUP_WITH pushes a finally block. */
    USE_LABEL(c, block);
    RETURN_IF_ERROR(compiler_push_fblock(c, loc, ASYNC_WITH, block, final, s));

    if (item->optional_vars) {
        VISIT(c, expr, item->optional_vars);
    }
    else {
        /* Discard result from context.__aenter__() */
        ADDOP(c, loc, POP_TOP);
    }

    pos++;
    if (pos == asdl_seq_LEN(s->v.AsyncWith.items)) {
        /* BLOCK code */
        VISIT_SEQ(c, stmt, s->v.AsyncWith.body)
    }
    else {
        RETURN_IF_ERROR(compiler_async_with(c, s, pos));
    }

    compiler_pop_fblock(c, ASYNC_WITH, block);

    ADDOP(c, loc, POP_BLOCK);
    /* End of body; start the cleanup */

    /* For successful outcome:
     * call __exit__(None, None, None)
     */
    RETURN_IF_ERROR(compiler_call_exit_with_nones(c, loc));
    ADDOP_I(c, loc, GET_AWAITABLE, 2);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADD_YIELD_FROM(c, loc, 1);

    ADDOP(c, loc, POP_TOP);

    ADDOP_JUMP(c, loc, JUMP, exit);

    /* For exceptional outcome: */
    USE_LABEL(c, final);

    ADDOP_JUMP(c, loc, SETUP_CLEANUP, cleanup);
    ADDOP(c, loc, PUSH_EXC_INFO);
    ADDOP(c, loc, WITH_EXCEPT_START);
    ADDOP_I(c, loc, GET_AWAITABLE, 2);
    ADDOP_LOAD_CONST(c, loc, Py_None);
    ADD_YIELD_FROM(c, loc, 1);
    RETURN_IF_ERROR(compiler_with_except_finish(c, cleanup));

    USE_LABEL(c, exit);
    return SUCCESS;
}


/*
   Implements the with statement from PEP 343.
   with EXPR as VAR:
       BLOCK
   is implemented as:
        <code for EXPR>
        SETUP_WITH  E
        <code to store to VAR> or POP_TOP
        <code for BLOCK>
        LOAD_CONST (None, None, None)
        CALL_FUNCTION_EX 0
        JUMP  EXIT
    E:  WITH_EXCEPT_START (calls EXPR.__exit__)
        POP_JUMP_IF_TRUE T:
        RERAISE
    T:  POP_TOP (remove exception from stack)
        POP_EXCEPT
        POP_TOP
    EXIT:
 */

static int
compiler_with(struct compiler *c, stmt_ty s, int pos)
{
    withitem_ty item = asdl_seq_GET(s->v.With.items, pos);

    assert(s->kind == With_kind);

    NEW_JUMP_TARGET_LABEL(c, block);
    NEW_JUMP_TARGET_LABEL(c, final);
    NEW_JUMP_TARGET_LABEL(c, exit);
    NEW_JUMP_TARGET_LABEL(c, cleanup);

    /* Evaluate EXPR */
    VISIT(c, expr, item->context_expr);
    /* Will push bound __exit__ */
    location loc = LOC(s);
    ADDOP(c, loc, BEFORE_WITH);
    ADDOP_JUMP(c, loc, SETUP_WITH, final);

    /* SETUP_WITH pushes a finally block. */
    USE_LABEL(c, block);
    RETURN_IF_ERROR(compiler_push_fblock(c, loc, WITH, block, final, s));

    if (item->optional_vars) {
        VISIT(c, expr, item->optional_vars);
    }
    else {
    /* Discard result from context.__enter__() */
        ADDOP(c, loc, POP_TOP);
    }

    pos++;
    if (pos == asdl_seq_LEN(s->v.With.items)) {
        /* BLOCK code */
        VISIT_SEQ(c, stmt, s->v.With.body)
    }
    else {
        RETURN_IF_ERROR(compiler_with(c, s, pos));
    }

    ADDOP(c, NO_LOCATION, POP_BLOCK);
    compiler_pop_fblock(c, WITH, block);

    /* End of body; start the cleanup. */

    /* For successful outcome:
     * call __exit__(None, None, None)
     */
    loc = LOC(s);
    RETURN_IF_ERROR(compiler_call_exit_with_nones(c, loc));
    ADDOP(c, loc, POP_TOP);
    ADDOP_JUMP(c, loc, JUMP, exit);

    /* For exceptional outcome: */
    USE_LABEL(c, final);

    ADDOP_JUMP(c, loc, SETUP_CLEANUP, cleanup);
    ADDOP(c, loc, PUSH_EXC_INFO);
    ADDOP(c, loc, WITH_EXCEPT_START);
    RETURN_IF_ERROR(compiler_with_except_finish(c, cleanup));

    USE_LABEL(c, exit);
    return SUCCESS;
}

static int
compiler_visit_expr1(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    switch (e->kind) {
    case NamedExpr_kind:
        VISIT(c, expr, e->v.NamedExpr.value);
        ADDOP_I(c, loc, COPY, 1);
        VISIT(c, expr, e->v.NamedExpr.target);
        break;
    case BoolOp_kind:
        return compiler_boolop(c, e);
    case BinOp_kind:
        VISIT(c, expr, e->v.BinOp.left);
        VISIT(c, expr, e->v.BinOp.right);
        ADDOP_BINARY(c, loc, e->v.BinOp.op);
        break;
    case UnaryOp_kind:
        VISIT(c, expr, e->v.UnaryOp.operand);
        if (e->v.UnaryOp.op == UAdd) {
            ADDOP_I(c, loc, CALL_INTRINSIC_1, INTRINSIC_UNARY_POSITIVE);
        }
        else {
            ADDOP(c, loc, unaryop(e->v.UnaryOp.op));
        }
        break;
    case Lambda_kind:
        return compiler_lambda(c, e);
    case IfExp_kind:
        return compiler_ifexp(c, e);
    case Dict_kind:
        return compiler_dict(c, e);
    case Set_kind:
        return compiler_set(c, e);
    case GeneratorExp_kind:
        return compiler_genexp(c, e);
    case ListComp_kind:
        return compiler_listcomp(c, e);
    case SetComp_kind:
        return compiler_setcomp(c, e);
    case DictComp_kind:
        return compiler_dictcomp(c, e);
    case Yield_kind:
        if (c->u->u_ste->ste_type != FunctionBlock) {
            return compiler_error(c, loc, "'yield' outside function");
        }
        if (e->v.Yield.value) {
            VISIT(c, expr, e->v.Yield.value);
        }
        else {
            ADDOP_LOAD_CONST(c, loc, Py_None);
        }
        ADDOP_YIELD(c, loc);
        break;
    case YieldFrom_kind:
        if (c->u->u_ste->ste_type != FunctionBlock) {
            return compiler_error(c, loc, "'yield' outside function");
        }
        if (c->u->u_scope_type == COMPILER_SCOPE_ASYNC_FUNCTION) {
            return compiler_error(c, loc, "'yield from' inside async function");
        }
        VISIT(c, expr, e->v.YieldFrom.value);
        ADDOP(c, loc, GET_YIELD_FROM_ITER);
        ADDOP_LOAD_CONST(c, loc, Py_None);
        ADD_YIELD_FROM(c, loc, 0);
        break;
    case Await_kind:
        if (!IS_TOP_LEVEL_AWAIT(c)){
            if (c->u->u_ste->ste_type != FunctionBlock){
                return compiler_error(c, loc, "'await' outside function");
            }

            if (c->u->u_scope_type != COMPILER_SCOPE_ASYNC_FUNCTION &&
                    c->u->u_scope_type != COMPILER_SCOPE_COMPREHENSION) {
                return compiler_error(c, loc, "'await' outside async function");
            }
        }

        VISIT(c, expr, e->v.Await.value);
        ADDOP_I(c, loc, GET_AWAITABLE, 0);
        ADDOP_LOAD_CONST(c, loc, Py_None);
        ADD_YIELD_FROM(c, loc, 1);
        break;
    case Compare_kind:
        return compiler_compare(c, e);
    case Call_kind:
        return compiler_call(c, e);
    case Constant_kind:
        ADDOP_LOAD_CONST(c, loc, e->v.Constant.value);
        break;
    case JoinedStr_kind:
        return compiler_joined_str(c, e);
    case FormattedValue_kind:
        return compiler_formatted_value(c, e);
    /* The following exprs can be assignment targets. */
    case Attribute_kind:
        VISIT(c, expr, e->v.Attribute.value);
        loc = LOC(e);
        loc = update_start_location_to_match_attr(c, loc, e);
        switch (e->v.Attribute.ctx) {
        case Load:
            ADDOP_NAME(c, loc, LOAD_ATTR, e->v.Attribute.attr, names);
            break;
        case Store:
            if (forbidden_name(c, loc, e->v.Attribute.attr, e->v.Attribute.ctx)) {
                return ERROR;
            }
            ADDOP_NAME(c, loc, STORE_ATTR, e->v.Attribute.attr, names);
            break;
        case Del:
            ADDOP_NAME(c, loc, DELETE_ATTR, e->v.Attribute.attr, names);
            break;
        }
        break;
    case Subscript_kind:
        return compiler_subscript(c, e);
    case Starred_kind:
        switch (e->v.Starred.ctx) {
        case Store:
            /* In all legitimate cases, the Starred node was already replaced
             * by compiler_list/compiler_tuple. XXX: is that okay? */
            return compiler_error(c, loc,
                "starred assignment target must be in a list or tuple");
        default:
            return compiler_error(c, loc,
                "can't use starred expression here");
        }
        break;
    case Slice_kind:
    {
        int n = compiler_slice(c, e);
        RETURN_IF_ERROR(n);
        ADDOP_I(c, loc, BUILD_SLICE, n);
        break;
    }
    case Name_kind:
        return compiler_nameop(c, loc, e->v.Name.id, e->v.Name.ctx);
    /* child nodes of List and Tuple will have expr_context set */
    case List_kind:
        return compiler_list(c, e);
    case Tuple_kind:
        return compiler_tuple(c, e);
    }
    return SUCCESS;
}

static int
compiler_visit_expr(struct compiler *c, expr_ty e)
{
    int res = compiler_visit_expr1(c, e);
    return res;
}

static bool
is_two_element_slice(expr_ty s)
{
    return s->kind == Slice_kind &&
           s->v.Slice.step == NULL;
}

static int
compiler_augassign(struct compiler *c, stmt_ty s)
{
    assert(s->kind == AugAssign_kind);
    expr_ty e = s->v.AugAssign.target;

    location loc = LOC(e);

    switch (e->kind) {
    case Attribute_kind:
        VISIT(c, expr, e->v.Attribute.value);
        ADDOP_I(c, loc, COPY, 1);
        loc = update_start_location_to_match_attr(c, loc, e);
        ADDOP_NAME(c, loc, LOAD_ATTR, e->v.Attribute.attr, names);
        break;
    case Subscript_kind:
        VISIT(c, expr, e->v.Subscript.value);
        if (is_two_element_slice(e->v.Subscript.slice)) {
            RETURN_IF_ERROR(compiler_slice(c, e->v.Subscript.slice));
            ADDOP_I(c, loc, COPY, 3);
            ADDOP_I(c, loc, COPY, 3);
            ADDOP_I(c, loc, COPY, 3);
            ADDOP(c, loc, BINARY_SLICE);
        }
        else {
            VISIT(c, expr, e->v.Subscript.slice);
            ADDOP_I(c, loc, COPY, 2);
            ADDOP_I(c, loc, COPY, 2);
            ADDOP(c, loc, BINARY_SUBSCR);
        }
        break;
    case Name_kind:
        RETURN_IF_ERROR(compiler_nameop(c, loc, e->v.Name.id, Load));
        break;
    default:
        PyErr_Format(PyExc_SystemError,
            "invalid node type (%d) for augmented assignment",
            e->kind);
        return ERROR;
    }

    loc = LOC(s);

    VISIT(c, expr, s->v.AugAssign.value);
    ADDOP_INPLACE(c, loc, s->v.AugAssign.op);

    loc = LOC(e);

    switch (e->kind) {
    case Attribute_kind:
        loc = update_start_location_to_match_attr(c, loc, e);
        ADDOP_I(c, loc, SWAP, 2);
        ADDOP_NAME(c, loc, STORE_ATTR, e->v.Attribute.attr, names);
        break;
    case Subscript_kind:
        if (is_two_element_slice(e->v.Subscript.slice)) {
            ADDOP_I(c, loc, SWAP, 4);
            ADDOP_I(c, loc, SWAP, 3);
            ADDOP_I(c, loc, SWAP, 2);
            ADDOP(c, loc, STORE_SLICE);
        }
        else {
            ADDOP_I(c, loc, SWAP, 3);
            ADDOP_I(c, loc, SWAP, 2);
            ADDOP(c, loc, STORE_SUBSCR);
        }
        break;
    case Name_kind:
        return compiler_nameop(c, loc, e->v.Name.id, Store);
    default:
        Py_UNREACHABLE();
    }
    return SUCCESS;
}

static int
check_ann_expr(struct compiler *c, expr_ty e)
{
    VISIT(c, expr, e);
    ADDOP(c, LOC(e), POP_TOP);
    return SUCCESS;
}

static int
check_annotation(struct compiler *c, stmt_ty s)
{
    /* Annotations of complex targets does not produce anything
       under annotations future */
    if (c->c_future.ff_features & CO_FUTURE_ANNOTATIONS) {
        return SUCCESS;
    }

    /* Annotations are only evaluated in a module or class. */
    if (c->u->u_scope_type == COMPILER_SCOPE_MODULE ||
        c->u->u_scope_type == COMPILER_SCOPE_CLASS) {
        return check_ann_expr(c, s->v.AnnAssign.annotation);
    }
    return SUCCESS;
}

static int
check_ann_subscr(struct compiler *c, expr_ty e)
{
    /* We check that everything in a subscript is defined at runtime. */
    switch (e->kind) {
    case Slice_kind:
        if (e->v.Slice.lower && check_ann_expr(c, e->v.Slice.lower) < 0) {
            return ERROR;
        }
        if (e->v.Slice.upper && check_ann_expr(c, e->v.Slice.upper) < 0) {
            return ERROR;
        }
        if (e->v.Slice.step && check_ann_expr(c, e->v.Slice.step) < 0) {
            return ERROR;
        }
        return SUCCESS;
    case Tuple_kind: {
        /* extended slice */
        asdl_expr_seq *elts = e->v.Tuple.elts;
        Py_ssize_t i, n = asdl_seq_LEN(elts);
        for (i = 0; i < n; i++) {
            RETURN_IF_ERROR(check_ann_subscr(c, asdl_seq_GET(elts, i)));
        }
        return SUCCESS;
    }
    default:
        return check_ann_expr(c, e);
    }
}

static int
compiler_annassign(struct compiler *c, stmt_ty s)
{
    location loc = LOC(s);
    expr_ty targ = s->v.AnnAssign.target;
    PyObject* mangled;

    assert(s->kind == AnnAssign_kind);

    /* We perform the actual assignment first. */
    if (s->v.AnnAssign.value) {
        VISIT(c, expr, s->v.AnnAssign.value);
        VISIT(c, expr, targ);
    }
    switch (targ->kind) {
    case Name_kind:
        if (forbidden_name(c, loc, targ->v.Name.id, Store)) {
            return ERROR;
        }
        /* If we have a simple name in a module or class, store annotation. */
        if (s->v.AnnAssign.simple &&
            (c->u->u_scope_type == COMPILER_SCOPE_MODULE ||
             c->u->u_scope_type == COMPILER_SCOPE_CLASS)) {
            if (c->c_future.ff_features & CO_FUTURE_ANNOTATIONS) {
                VISIT(c, annexpr, s->v.AnnAssign.annotation)
            }
            else {
                VISIT(c, expr, s->v.AnnAssign.annotation);
            }
            ADDOP_NAME(c, loc, LOAD_NAME, &_Py_ID(__annotations__), names);
            mangled = _Py_Mangle(c->u->u_private, targ->v.Name.id);
            ADDOP_LOAD_CONST_NEW(c, loc, mangled);
            ADDOP(c, loc, STORE_SUBSCR);
        }
        break;
    case Attribute_kind:
        if (forbidden_name(c, loc, targ->v.Attribute.attr, Store)) {
            return ERROR;
        }
        if (!s->v.AnnAssign.value &&
            check_ann_expr(c, targ->v.Attribute.value) < 0) {
            return ERROR;
        }
        break;
    case Subscript_kind:
        if (!s->v.AnnAssign.value &&
            (check_ann_expr(c, targ->v.Subscript.value) < 0 ||
             check_ann_subscr(c, targ->v.Subscript.slice) < 0)) {
                return ERROR;
        }
        break;
    default:
        PyErr_Format(PyExc_SystemError,
                     "invalid node type (%d) for annotated assignment",
                     targ->kind);
        return ERROR;
    }
    /* Annotation is evaluated last. */
    if (!s->v.AnnAssign.simple && check_annotation(c, s) < 0) {
        return ERROR;
    }
    return SUCCESS;
}

/* Raises a SyntaxError and returns 0.
   If something goes wrong, a different exception may be raised.
*/

static int
compiler_error(struct compiler *c, location loc,
               const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    PyObject *msg = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    if (msg == NULL) {
        return ERROR;
    }
    PyObject *loc_obj = PyErr_ProgramTextObject(c->c_filename, loc.lineno);
    if (loc_obj == NULL) {
        loc_obj = Py_NewRef(Py_None);
    }
    PyObject *args = Py_BuildValue("O(OiiOii)", msg, c->c_filename,
                                   loc.lineno, loc.col_offset + 1, loc_obj,
                                   loc.end_lineno, loc.end_col_offset + 1);
    Py_DECREF(msg);
    if (args == NULL) {
        goto exit;
    }
    PyErr_SetObject(PyExc_SyntaxError, args);
 exit:
    Py_DECREF(loc_obj);
    Py_XDECREF(args);
    return ERROR;
}

/* Emits a SyntaxWarning and returns 1 on success.
   If a SyntaxWarning raised as error, replaces it with a SyntaxError
   and returns 0.
*/
static int
compiler_warn(struct compiler *c, location loc,
              const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    PyObject *msg = PyUnicode_FromFormatV(format, vargs);
    va_end(vargs);
    if (msg == NULL) {
        return ERROR;
    }
    if (PyErr_WarnExplicitObject(PyExc_SyntaxWarning, msg, c->c_filename,
                                 loc.lineno, NULL, NULL) < 0)
    {
        if (PyErr_ExceptionMatches(PyExc_SyntaxWarning)) {
            /* Replace the SyntaxWarning exception with a SyntaxError
               to get a more accurate error report */
            PyErr_Clear();
            assert(PyUnicode_AsUTF8(msg) != NULL);
            compiler_error(c, loc, PyUnicode_AsUTF8(msg));
        }
        Py_DECREF(msg);
        return ERROR;
    }
    Py_DECREF(msg);
    return SUCCESS;
}

static int
compiler_subscript(struct compiler *c, expr_ty e)
{
    location loc = LOC(e);
    expr_context_ty ctx = e->v.Subscript.ctx;
    int op = 0;

    if (ctx == Load) {
        RETURN_IF_ERROR(check_subscripter(c, e->v.Subscript.value));
        RETURN_IF_ERROR(check_index(c, e->v.Subscript.value, e->v.Subscript.slice));
    }

    VISIT(c, expr, e->v.Subscript.value);
    if (is_two_element_slice(e->v.Subscript.slice) && ctx != Del) {
        RETURN_IF_ERROR(compiler_slice(c, e->v.Subscript.slice));
        if (ctx == Load) {
            ADDOP(c, loc, BINARY_SLICE);
        }
        else {
            assert(ctx == Store);
            ADDOP(c, loc, STORE_SLICE);
        }
    }
    else {
        VISIT(c, expr, e->v.Subscript.slice);
        switch (ctx) {
            case Load:    op = BINARY_SUBSCR; break;
            case Store:   op = STORE_SUBSCR; break;
            case Del:     op = DELETE_SUBSCR; break;
        }
        assert(op);
        ADDOP(c, loc, op);
    }
    return SUCCESS;
}

/* Returns the number of the values emitted,
 * thus are needed to build the slice, or -1 if there is an error. */
static int
compiler_slice(struct compiler *c, expr_ty s)
{
    int n = 2;
    assert(s->kind == Slice_kind);

    /* only handles the cases where BUILD_SLICE is emitted */
    if (s->v.Slice.lower) {
        VISIT(c, expr, s->v.Slice.lower);
    }
    else {
        ADDOP_LOAD_CONST(c, LOC(s), Py_None);
    }

    if (s->v.Slice.upper) {
        VISIT(c, expr, s->v.Slice.upper);
    }
    else {
        ADDOP_LOAD_CONST(c, LOC(s), Py_None);
    }

    if (s->v.Slice.step) {
        n++;
        VISIT(c, expr, s->v.Slice.step);
    }
    return n;
}


// PEP 634: Structural Pattern Matching

// To keep things simple, all compiler_pattern_* and pattern_helper_* routines
// follow the convention of consuming TOS (the subject for the given pattern)
// and calling jump_to_fail_pop on failure (no match).

// When calling into these routines, it's important that pc->on_top be kept
// updated to reflect the current number of items that we are using on the top
// of the stack: they will be popped on failure, and any name captures will be
// stored *underneath* them on success. This lets us defer all names stores
// until the *entire* pattern matches.

#define WILDCARD_CHECK(N) \
    ((N)->kind == MatchAs_kind && !(N)->v.MatchAs.name)

#define WILDCARD_STAR_CHECK(N) \
    ((N)->kind == MatchStar_kind && !(N)->v.MatchStar.name)

// Limit permitted subexpressions, even if the parser & AST validator let them through
#define MATCH_VALUE_EXPR(N) \
    ((N)->kind == Constant_kind || (N)->kind == Attribute_kind)

// Allocate or resize pc->fail_pop to allow for n items to be popped on failure.
static int
ensure_fail_pop(struct compiler *c, pattern_context *pc, Py_ssize_t n)
{
    Py_ssize_t size = n + 1;
    if (size <= pc->fail_pop_size) {
        return SUCCESS;
    }
    Py_ssize_t needed = sizeof(jump_target_label) * size;
    jump_target_label *resized = PyObject_Realloc(pc->fail_pop, needed);
    if (resized == NULL) {
        PyErr_NoMemory();
        return ERROR;
    }
    pc->fail_pop = resized;
    while (pc->fail_pop_size < size) {
        NEW_JUMP_TARGET_LABEL(c, new_block);
        pc->fail_pop[pc->fail_pop_size++] = new_block;
    }
    return SUCCESS;
}

// Use op to jump to the correct fail_pop block.
static int
jump_to_fail_pop(struct compiler *c, location loc,
                 pattern_context *pc, int op)
{
    // Pop any items on the top of the stack, plus any objects we were going to
    // capture on success:
    Py_ssize_t pops = pc->on_top + PyList_GET_SIZE(pc->stores);
    RETURN_IF_ERROR(ensure_fail_pop(c, pc, pops));
    ADDOP_JUMP(c, loc, op, pc->fail_pop[pops]);
    return SUCCESS;
}

// Build all of the fail_pop blocks and reset fail_pop.
static int
emit_and_reset_fail_pop(struct compiler *c, location loc,
                        pattern_context *pc)
{
    if (!pc->fail_pop_size) {
        assert(pc->fail_pop == NULL);
        return SUCCESS;
    }
    while (--pc->fail_pop_size) {
        USE_LABEL(c, pc->fail_pop[pc->fail_pop_size]);
        if (cfg_builder_addop_noarg(CFG_BUILDER(c), POP_TOP, loc) < 0) {
            pc->fail_pop_size = 0;
            PyObject_Free(pc->fail_pop);
            pc->fail_pop = NULL;
            return ERROR;
        }
    }
    USE_LABEL(c, pc->fail_pop[0]);
    PyObject_Free(pc->fail_pop);
    pc->fail_pop = NULL;
    return SUCCESS;
}

static int
compiler_error_duplicate_store(struct compiler *c, location loc, identifier n)
{
    return compiler_error(c, loc,
        "multiple assignments to name %R in pattern", n);
}

// Duplicate the effect of 3.10's ROT_* instructions using SWAPs.
static int
pattern_helper_rotate(struct compiler *c, location loc, Py_ssize_t count)
{
    while (1 < count) {
        ADDOP_I(c, loc, SWAP, count--);
    }
    return SUCCESS;
}

static int
pattern_helper_store_name(struct compiler *c, location loc,
                          identifier n, pattern_context *pc)
{
    if (n == NULL) {
        ADDOP(c, loc, POP_TOP);
        return SUCCESS;
    }
    if (forbidden_name(c, loc, n, Store)) {
        return ERROR;
    }
    // Can't assign to the same name twice:
    int duplicate = PySequence_Contains(pc->stores, n);
    RETURN_IF_ERROR(duplicate);
    if (duplicate) {
        return compiler_error_duplicate_store(c, loc, n);
    }
    // Rotate this object underneath any items we need to preserve:
    Py_ssize_t rotations = pc->on_top + PyList_GET_SIZE(pc->stores) + 1;
    RETURN_IF_ERROR(pattern_helper_rotate(c, loc, rotations));
    RETURN_IF_ERROR(PyList_Append(pc->stores, n));
    return SUCCESS;
}


static int
pattern_unpack_helper(struct compiler *c, location loc,
                      asdl_pattern_seq *elts)
{
    Py_ssize_t n = asdl_seq_LEN(elts);
    int seen_star = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        pattern_ty elt = asdl_seq_GET(elts, i);
        if (elt->kind == MatchStar_kind && !seen_star) {
            if ((i >= (1 << 8)) ||
                (n-i-1 >= (INT_MAX >> 8))) {
                return compiler_error(c, loc,
                    "too many expressions in "
                    "star-unpacking sequence pattern");
            }
            ADDOP_I(c, loc, UNPACK_EX, (i + ((n-i-1) << 8)));
            seen_star = 1;
        }
        else if (elt->kind == MatchStar_kind) {
            return compiler_error(c, loc,
                "multiple starred expressions in sequence pattern");
        }
    }
    if (!seen_star) {
        ADDOP_I(c, loc, UNPACK_SEQUENCE, n);
    }
    return SUCCESS;
}

static int
pattern_helper_sequence_unpack(struct compiler *c, location loc,
                               asdl_pattern_seq *patterns, Py_ssize_t star,
                               pattern_context *pc)
{
    RETURN_IF_ERROR(pattern_unpack_helper(c, loc, patterns));
    Py_ssize_t size = asdl_seq_LEN(patterns);
    // We've now got a bunch of new subjects on the stack. They need to remain
    // there after each subpattern match:
    pc->on_top += size;
    for (Py_ssize_t i = 0; i < size; i++) {
        // One less item to keep track of each time we loop through:
        pc->on_top--;
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        RETURN_IF_ERROR(compiler_pattern_subpattern(c, pattern, pc));
    }
    return SUCCESS;
}

// Like pattern_helper_sequence_unpack, but uses BINARY_SUBSCR instead of
// UNPACK_SEQUENCE / UNPACK_EX. This is more efficient for patterns with a
// starred wildcard like [first, *_] / [first, *_, last] / [*_, last] / etc.
static int
pattern_helper_sequence_subscr(struct compiler *c, location loc,
                               asdl_pattern_seq *patterns, Py_ssize_t star,
                               pattern_context *pc)
{
    // We need to keep the subject around for extracting elements:
    pc->on_top++;
    Py_ssize_t size = asdl_seq_LEN(patterns);
    for (Py_ssize_t i = 0; i < size; i++) {
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        if (WILDCARD_CHECK(pattern)) {
            continue;
        }
        if (i == star) {
            assert(WILDCARD_STAR_CHECK(pattern));
            continue;
        }
        ADDOP_I(c, loc, COPY, 1);
        if (i < star) {
            ADDOP_LOAD_CONST_NEW(c, loc, PyLong_FromSsize_t(i));
        }
        else {
            // The subject may not support negative indexing! Compute a
            // nonnegative index:
            ADDOP(c, loc, GET_LEN);
            ADDOP_LOAD_CONST_NEW(c, loc, PyLong_FromSsize_t(size - i));
            ADDOP_BINARY(c, loc, Sub);
        }
        ADDOP(c, loc, BINARY_SUBSCR);
        RETURN_IF_ERROR(compiler_pattern_subpattern(c, pattern, pc));
    }
    // Pop the subject, we're done with it:
    pc->on_top--;
    ADDOP(c, loc, POP_TOP);
    return SUCCESS;
}

// Like compiler_pattern, but turn off checks for irrefutability.
static int
compiler_pattern_subpattern(struct compiler *c,
                            pattern_ty p, pattern_context *pc)
{
    int allow_irrefutable = pc->allow_irrefutable;
    pc->allow_irrefutable = 1;
    RETURN_IF_ERROR(compiler_pattern(c, p, pc));
    pc->allow_irrefutable = allow_irrefutable;
    return SUCCESS;
}

static int
compiler_pattern_as(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchAs_kind);
    if (p->v.MatchAs.pattern == NULL) {
        // An irrefutable match:
        if (!pc->allow_irrefutable) {
            if (p->v.MatchAs.name) {
                const char *e = "name capture %R makes remaining patterns unreachable";
                return compiler_error(c, LOC(p), e, p->v.MatchAs.name);
            }
            const char *e = "wildcard makes remaining patterns unreachable";
            return compiler_error(c, LOC(p), e);
        }
        return pattern_helper_store_name(c, LOC(p), p->v.MatchAs.name, pc);
    }
    // Need to make a copy for (possibly) storing later:
    pc->on_top++;
    ADDOP_I(c, LOC(p), COPY, 1);
    RETURN_IF_ERROR(compiler_pattern(c, p->v.MatchAs.pattern, pc));
    // Success! Store it:
    pc->on_top--;
    RETURN_IF_ERROR(pattern_helper_store_name(c, LOC(p), p->v.MatchAs.name, pc));
    return SUCCESS;
}

static int
compiler_pattern_star(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchStar_kind);
    RETURN_IF_ERROR(
        pattern_helper_store_name(c, LOC(p), p->v.MatchStar.name, pc));
    return SUCCESS;
}

static int
validate_kwd_attrs(struct compiler *c, asdl_identifier_seq *attrs, asdl_pattern_seq* patterns)
{
    // Any errors will point to the pattern rather than the arg name as the
    // parser is only supplying identifiers rather than Name or keyword nodes
    Py_ssize_t nattrs = asdl_seq_LEN(attrs);
    for (Py_ssize_t i = 0; i < nattrs; i++) {
        identifier attr = ((identifier)asdl_seq_GET(attrs, i));
        location loc = LOC((pattern_ty) asdl_seq_GET(patterns, i));
        if (forbidden_name(c, loc, attr, Store)) {
            return ERROR;
        }
        for (Py_ssize_t j = i + 1; j < nattrs; j++) {
            identifier other = ((identifier)asdl_seq_GET(attrs, j));
            if (!PyUnicode_Compare(attr, other)) {
                location loc = LOC((pattern_ty) asdl_seq_GET(patterns, j));
                compiler_error(c, loc, "attribute name repeated in class pattern: %U", attr);
                return ERROR;
            }
        }
    }
    return SUCCESS;
}

static int
compiler_pattern_class(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchClass_kind);
    asdl_pattern_seq *patterns = p->v.MatchClass.patterns;
    asdl_identifier_seq *kwd_attrs = p->v.MatchClass.kwd_attrs;
    asdl_pattern_seq *kwd_patterns = p->v.MatchClass.kwd_patterns;
    Py_ssize_t nargs = asdl_seq_LEN(patterns);
    Py_ssize_t nattrs = asdl_seq_LEN(kwd_attrs);
    Py_ssize_t nkwd_patterns = asdl_seq_LEN(kwd_patterns);
    if (nattrs != nkwd_patterns) {
        // AST validator shouldn't let this happen, but if it does,
        // just fail, don't crash out of the interpreter
        const char * e = "kwd_attrs (%d) / kwd_patterns (%d) length mismatch in class pattern";
        return compiler_error(c, LOC(p), e, nattrs, nkwd_patterns);
    }
    if (INT_MAX < nargs || INT_MAX < nargs + nattrs - 1) {
        const char *e = "too many sub-patterns in class pattern %R";
        return compiler_error(c, LOC(p), e, p->v.MatchClass.cls);
    }
    if (nattrs) {
        RETURN_IF_ERROR(validate_kwd_attrs(c, kwd_attrs, kwd_patterns));
    }
    VISIT(c, expr, p->v.MatchClass.cls);
    PyObject *attr_names = PyTuple_New(nattrs);
    if (attr_names == NULL) {
        return ERROR;
    }
    Py_ssize_t i;
    for (i = 0; i < nattrs; i++) {
        PyObject *name = asdl_seq_GET(kwd_attrs, i);
        PyTuple_SET_ITEM(attr_names, i, Py_NewRef(name));
    }
    ADDOP_LOAD_CONST_NEW(c, LOC(p), attr_names);
    ADDOP_I(c, LOC(p), MATCH_CLASS, nargs);
    ADDOP_I(c, LOC(p), COPY, 1);
    ADDOP_LOAD_CONST(c, LOC(p), Py_None);
    ADDOP_I(c, LOC(p), IS_OP, 1);
    // TOS is now a tuple of (nargs + nattrs) attributes (or None):
    pc->on_top++;
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    ADDOP_I(c, LOC(p), UNPACK_SEQUENCE, nargs + nattrs);
    pc->on_top += nargs + nattrs - 1;
    for (i = 0; i < nargs + nattrs; i++) {
        pc->on_top--;
        pattern_ty pattern;
        if (i < nargs) {
            // Positional:
            pattern = asdl_seq_GET(patterns, i);
        }
        else {
            // Keyword:
            pattern = asdl_seq_GET(kwd_patterns, i - nargs);
        }
        if (WILDCARD_CHECK(pattern)) {
            ADDOP(c, LOC(p), POP_TOP);
            continue;
        }
        RETURN_IF_ERROR(compiler_pattern_subpattern(c, pattern, pc));
    }
    // Success! Pop the tuple of attributes:
    return SUCCESS;
}

static int
compiler_pattern_mapping(struct compiler *c, pattern_ty p,
                         pattern_context *pc)
{
    assert(p->kind == MatchMapping_kind);
    asdl_expr_seq *keys = p->v.MatchMapping.keys;
    asdl_pattern_seq *patterns = p->v.MatchMapping.patterns;
    Py_ssize_t size = asdl_seq_LEN(keys);
    Py_ssize_t npatterns = asdl_seq_LEN(patterns);
    if (size != npatterns) {
        // AST validator shouldn't let this happen, but if it does,
        // just fail, don't crash out of the interpreter
        const char * e = "keys (%d) / patterns (%d) length mismatch in mapping pattern";
        return compiler_error(c, LOC(p), e, size, npatterns);
    }
    // We have a double-star target if "rest" is set
    PyObject *star_target = p->v.MatchMapping.rest;
    // We need to keep the subject on top during the mapping and length checks:
    pc->on_top++;
    ADDOP(c, LOC(p), MATCH_MAPPING);
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    if (!size && !star_target) {
        // If the pattern is just "{}", we're done! Pop the subject:
        pc->on_top--;
        ADDOP(c, LOC(p), POP_TOP);
        return SUCCESS;
    }
    if (size) {
        // If the pattern has any keys in it, perform a length check:
        ADDOP(c, LOC(p), GET_LEN);
        ADDOP_LOAD_CONST_NEW(c, LOC(p), PyLong_FromSsize_t(size));
        ADDOP_COMPARE(c, LOC(p), GtE);
        RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    }
    if (INT_MAX < size - 1) {
        return compiler_error(c, LOC(p), "too many sub-patterns in mapping pattern");
    }
    // Collect all of the keys into a tuple for MATCH_KEYS and
    // **rest. They can either be dotted names or literals:

    // Maintaining a set of Constant_kind kind keys allows us to raise a
    // SyntaxError in the case of duplicates.
    PyObject *seen = PySet_New(NULL);
    if (seen == NULL) {
        return ERROR;
    }

    // NOTE: goto error on failure in the loop below to avoid leaking `seen`
    for (Py_ssize_t i = 0; i < size; i++) {
        expr_ty key = asdl_seq_GET(keys, i);
        if (key == NULL) {
            const char *e = "can't use NULL keys in MatchMapping "
                            "(set 'rest' parameter instead)";
            location loc = LOC((pattern_ty) asdl_seq_GET(patterns, i));
            compiler_error(c, loc, e);
            goto error;
        }

        if (key->kind == Constant_kind) {
            int in_seen = PySet_Contains(seen, key->v.Constant.value);
            if (in_seen < 0) {
                goto error;
            }
            if (in_seen) {
                const char *e = "mapping pattern checks duplicate key (%R)";
                compiler_error(c, LOC(p), e, key->v.Constant.value);
                goto error;
            }
            if (PySet_Add(seen, key->v.Constant.value)) {
                goto error;
            }
        }

        else if (key->kind != Attribute_kind) {
            const char *e = "mapping pattern keys may only match literals and attribute lookups";
            compiler_error(c, LOC(p), e);
            goto error;
        }
        if (compiler_visit_expr(c, key) < 0) {
            goto error;
        }
    }

    // all keys have been checked; there are no duplicates
    Py_DECREF(seen);

    ADDOP_I(c, LOC(p), BUILD_TUPLE, size);
    ADDOP(c, LOC(p), MATCH_KEYS);
    // There's now a tuple of keys and a tuple of values on top of the subject:
    pc->on_top += 2;
    ADDOP_I(c, LOC(p), COPY, 1);
    ADDOP_LOAD_CONST(c, LOC(p), Py_None);
    ADDOP_I(c, LOC(p), IS_OP, 1);
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    // So far so good. Use that tuple of values on the stack to match
    // sub-patterns against:
    ADDOP_I(c, LOC(p), UNPACK_SEQUENCE, size);
    pc->on_top += size - 1;
    for (Py_ssize_t i = 0; i < size; i++) {
        pc->on_top--;
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        RETURN_IF_ERROR(compiler_pattern_subpattern(c, pattern, pc));
    }
    // If we get this far, it's a match! Whatever happens next should consume
    // the tuple of keys and the subject:
    pc->on_top -= 2;
    if (star_target) {
        // If we have a starred name, bind a dict of remaining items to it (this may
        // seem a bit inefficient, but keys is rarely big enough to actually impact
        // runtime):
        // rest = dict(TOS1)
        // for key in TOS:
        //     del rest[key]
        ADDOP_I(c, LOC(p), BUILD_MAP, 0);           // [subject, keys, empty]
        ADDOP_I(c, LOC(p), SWAP, 3);                // [empty, keys, subject]
        ADDOP_I(c, LOC(p), DICT_UPDATE, 2);         // [copy, keys]
        ADDOP_I(c, LOC(p), UNPACK_SEQUENCE, size);  // [copy, keys...]
        while (size) {
            ADDOP_I(c, LOC(p), COPY, 1 + size--);   // [copy, keys..., copy]
            ADDOP_I(c, LOC(p), SWAP, 2);            // [copy, keys..., copy, key]
            ADDOP(c, LOC(p), DELETE_SUBSCR);        // [copy, keys...]
        }
        RETURN_IF_ERROR(pattern_helper_store_name(c, LOC(p), star_target, pc));
    }
    else {
        ADDOP(c, LOC(p), POP_TOP);  // Tuple of keys.
        ADDOP(c, LOC(p), POP_TOP);  // Subject.
    }
    return SUCCESS;

error:
    Py_DECREF(seen);
    return ERROR;
}

static int
compiler_pattern_or(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchOr_kind);
    NEW_JUMP_TARGET_LABEL(c, end);
    Py_ssize_t size = asdl_seq_LEN(p->v.MatchOr.patterns);
    assert(size > 1);
    // We're going to be messing with pc. Keep the original info handy:
    pattern_context old_pc = *pc;
    Py_INCREF(pc->stores);
    // control is the list of names bound by the first alternative. It is used
    // for checking different name bindings in alternatives, and for correcting
    // the order in which extracted elements are placed on the stack.
    PyObject *control = NULL;
    // NOTE: We can't use returning macros anymore! goto error on error.
    for (Py_ssize_t i = 0; i < size; i++) {
        pattern_ty alt = asdl_seq_GET(p->v.MatchOr.patterns, i);
        PyObject *pc_stores = PyList_New(0);
        if (pc_stores == NULL) {
            goto error;
        }
        Py_SETREF(pc->stores, pc_stores);
        // An irrefutable sub-pattern must be last, if it is allowed at all:
        pc->allow_irrefutable = (i == size - 1) && old_pc.allow_irrefutable;
        pc->fail_pop = NULL;
        pc->fail_pop_size = 0;
        pc->on_top = 0;
        if (cfg_builder_addop_i(CFG_BUILDER(c), COPY, 1, LOC(alt)) < 0 ||
            compiler_pattern(c, alt, pc) < 0) {
            goto error;
        }
        // Success!
        Py_ssize_t nstores = PyList_GET_SIZE(pc->stores);
        if (!i) {
            // This is the first alternative, so save its stores as a "control"
            // for the others (they can't bind a different set of names, and
            // might need to be reordered):
            assert(control == NULL);
            control = Py_NewRef(pc->stores);
        }
        else if (nstores != PyList_GET_SIZE(control)) {
            goto diff;
        }
        else if (nstores) {
            // There were captures. Check to see if we differ from control:
            Py_ssize_t icontrol = nstores;
            while (icontrol--) {
                PyObject *name = PyList_GET_ITEM(control, icontrol);
                Py_ssize_t istores = PySequence_Index(pc->stores, name);
                if (istores < 0) {
                    PyErr_Clear();
                    goto diff;
                }
                if (icontrol != istores) {
                    // Reorder the names on the stack to match the order of the
                    // names in control. There's probably a better way of doing
                    // this; the current solution is potentially very
                    // inefficient when each alternative subpattern binds lots
                    // of names in different orders. It's fine for reasonable
                    // cases, though, and the peephole optimizer will ensure
                    // that the final code is as efficient as possible.
                    assert(istores < icontrol);
                    Py_ssize_t rotations = istores + 1;
                    // Perform the same rotation on pc->stores:
                    PyObject *rotated = PyList_GetSlice(pc->stores, 0,
                                                        rotations);
                    if (rotated == NULL ||
                        PyList_SetSlice(pc->stores, 0, rotations, NULL) ||
                        PyList_SetSlice(pc->stores, icontrol - istores,
                                        icontrol - istores, rotated))
                    {
                        Py_XDECREF(rotated);
                        goto error;
                    }
                    Py_DECREF(rotated);
                    // That just did:
                    // rotated = pc_stores[:rotations]
                    // del pc_stores[:rotations]
                    // pc_stores[icontrol-istores:icontrol-istores] = rotated
                    // Do the same thing to the stack, using several
                    // rotations:
                    while (rotations--) {
                        if (pattern_helper_rotate(c, LOC(alt), icontrol + 1) < 0) {
                            goto error;
                        }
                    }
                }
            }
        }
        assert(control);
        if (cfg_builder_addop_j(CFG_BUILDER(c), LOC(alt), JUMP, end) < 0 ||
            emit_and_reset_fail_pop(c, LOC(alt), pc) < 0)
        {
            goto error;
        }
    }
    Py_DECREF(pc->stores);
    *pc = old_pc;
    Py_INCREF(pc->stores);
    // Need to NULL this for the PyObject_Free call in the error block.
    old_pc.fail_pop = NULL;
    // No match. Pop the remaining copy of the subject and fail:
    if (cfg_builder_addop_noarg(CFG_BUILDER(c), POP_TOP, LOC(p)) < 0 ||
        jump_to_fail_pop(c, LOC(p), pc, JUMP) < 0) {
        goto error;
    }

    USE_LABEL(c, end);
    Py_ssize_t nstores = PyList_GET_SIZE(control);
    // There's a bunch of stuff on the stack between where the new stores
    // are and where they need to be:
    // - The other stores.
    // - A copy of the subject.
    // - Anything else that may be on top of the stack.
    // - Any previous stores we've already stashed away on the stack.
    Py_ssize_t nrots = nstores + 1 + pc->on_top + PyList_GET_SIZE(pc->stores);
    for (Py_ssize_t i = 0; i < nstores; i++) {
        // Rotate this capture to its proper place on the stack:
        if (pattern_helper_rotate(c, LOC(p), nrots) < 0) {
            goto error;
        }
        // Update the list of previous stores with this new name, checking for
        // duplicates:
        PyObject *name = PyList_GET_ITEM(control, i);
        int dupe = PySequence_Contains(pc->stores, name);
        if (dupe < 0) {
            goto error;
        }
        if (dupe) {
            compiler_error_duplicate_store(c, LOC(p), name);
            goto error;
        }
        if (PyList_Append(pc->stores, name)) {
            goto error;
        }
    }
    Py_DECREF(old_pc.stores);
    Py_DECREF(control);
    // NOTE: Returning macros are safe again.
    // Pop the copy of the subject:
    ADDOP(c, LOC(p), POP_TOP);
    return SUCCESS;
diff:
    compiler_error(c, LOC(p), "alternative patterns bind different names");
error:
    PyObject_Free(old_pc.fail_pop);
    Py_DECREF(old_pc.stores);
    Py_XDECREF(control);
    return ERROR;
}


static int
compiler_pattern_sequence(struct compiler *c, pattern_ty p,
                          pattern_context *pc)
{
    assert(p->kind == MatchSequence_kind);
    asdl_pattern_seq *patterns = p->v.MatchSequence.patterns;
    Py_ssize_t size = asdl_seq_LEN(patterns);
    Py_ssize_t star = -1;
    int only_wildcard = 1;
    int star_wildcard = 0;
    // Find a starred name, if it exists. There may be at most one:
    for (Py_ssize_t i = 0; i < size; i++) {
        pattern_ty pattern = asdl_seq_GET(patterns, i);
        if (pattern->kind == MatchStar_kind) {
            if (star >= 0) {
                const char *e = "multiple starred names in sequence pattern";
                return compiler_error(c, LOC(p), e);
            }
            star_wildcard = WILDCARD_STAR_CHECK(pattern);
            only_wildcard &= star_wildcard;
            star = i;
            continue;
        }
        only_wildcard &= WILDCARD_CHECK(pattern);
    }
    // We need to keep the subject on top during the sequence and length checks:
    pc->on_top++;
    ADDOP(c, LOC(p), MATCH_SEQUENCE);
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    if (star < 0) {
        // No star: len(subject) == size
        ADDOP(c, LOC(p), GET_LEN);
        ADDOP_LOAD_CONST_NEW(c, LOC(p), PyLong_FromSsize_t(size));
        ADDOP_COMPARE(c, LOC(p), Eq);
        RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    }
    else if (size > 1) {
        // Star: len(subject) >= size - 1
        ADDOP(c, LOC(p), GET_LEN);
        ADDOP_LOAD_CONST_NEW(c, LOC(p), PyLong_FromSsize_t(size - 1));
        ADDOP_COMPARE(c, LOC(p), GtE);
        RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    }
    // Whatever comes next should consume the subject:
    pc->on_top--;
    if (only_wildcard) {
        // Patterns like: [] / [_] / [_, _] / [*_] / [_, *_] / [_, _, *_] / etc.
        ADDOP(c, LOC(p), POP_TOP);
    }
    else if (star_wildcard) {
        RETURN_IF_ERROR(pattern_helper_sequence_subscr(c, LOC(p), patterns, star, pc));
    }
    else {
        RETURN_IF_ERROR(pattern_helper_sequence_unpack(c, LOC(p), patterns, star, pc));
    }
    return SUCCESS;
}

static int
compiler_pattern_value(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchValue_kind);
    expr_ty value = p->v.MatchValue.value;
    if (!MATCH_VALUE_EXPR(value)) {
        const char *e = "patterns may only match literals and attribute lookups";
        return compiler_error(c, LOC(p), e);
    }
    VISIT(c, expr, value);
    ADDOP_COMPARE(c, LOC(p), Eq);
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    return SUCCESS;
}

static int
compiler_pattern_singleton(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    assert(p->kind == MatchSingleton_kind);
    ADDOP_LOAD_CONST(c, LOC(p), p->v.MatchSingleton.value);
    ADDOP_COMPARE(c, LOC(p), Is);
    RETURN_IF_ERROR(jump_to_fail_pop(c, LOC(p), pc, POP_JUMP_IF_FALSE));
    return SUCCESS;
}

static int
compiler_pattern(struct compiler *c, pattern_ty p, pattern_context *pc)
{
    switch (p->kind) {
        case MatchValue_kind:
            return compiler_pattern_value(c, p, pc);
        case MatchSingleton_kind:
            return compiler_pattern_singleton(c, p, pc);
        case MatchSequence_kind:
            return compiler_pattern_sequence(c, p, pc);
        case MatchMapping_kind:
            return compiler_pattern_mapping(c, p, pc);
        case MatchClass_kind:
            return compiler_pattern_class(c, p, pc);
        case MatchStar_kind:
            return compiler_pattern_star(c, p, pc);
        case MatchAs_kind:
            return compiler_pattern_as(c, p, pc);
        case MatchOr_kind:
            return compiler_pattern_or(c, p, pc);
    }
    // AST validator shouldn't let this happen, but if it does,
    // just fail, don't crash out of the interpreter
    const char *e = "invalid match pattern node in AST (kind=%d)";
    return compiler_error(c, LOC(p), e, p->kind);
}

static int
compiler_match_inner(struct compiler *c, stmt_ty s, pattern_context *pc)
{
    VISIT(c, expr, s->v.Match.subject);
    NEW_JUMP_TARGET_LABEL(c, end);
    Py_ssize_t cases = asdl_seq_LEN(s->v.Match.cases);
    assert(cases > 0);
    match_case_ty m = asdl_seq_GET(s->v.Match.cases, cases - 1);
    int has_default = WILDCARD_CHECK(m->pattern) && 1 < cases;
    for (Py_ssize_t i = 0; i < cases - has_default; i++) {
        m = asdl_seq_GET(s->v.Match.cases, i);
        // Only copy the subject if we're *not* on the last case:
        if (i != cases - has_default - 1) {
            ADDOP_I(c, LOC(m->pattern), COPY, 1);
        }
        pc->stores = PyList_New(0);
        if (pc->stores == NULL) {
            return ERROR;
        }
        // Irrefutable cases must be either guarded, last, or both:
        pc->allow_irrefutable = m->guard != NULL || i == cases - 1;
        pc->fail_pop = NULL;
        pc->fail_pop_size = 0;
        pc->on_top = 0;
        // NOTE: Can't use returning macros here (they'll leak pc->stores)!
        if (compiler_pattern(c, m->pattern, pc) < 0) {
            Py_DECREF(pc->stores);
            return ERROR;
        }
        assert(!pc->on_top);
        // It's a match! Store all of the captured names (they're on the stack).
        Py_ssize_t nstores = PyList_GET_SIZE(pc->stores);
        for (Py_ssize_t n = 0; n < nstores; n++) {
            PyObject *name = PyList_GET_ITEM(pc->stores, n);
            if (compiler_nameop(c, LOC(m->pattern), name, Store) < 0) {
                Py_DECREF(pc->stores);
                return ERROR;
            }
        }
        Py_DECREF(pc->stores);
        // NOTE: Returning macros are safe again.
        if (m->guard) {
            RETURN_IF_ERROR(ensure_fail_pop(c, pc, 0));
            RETURN_IF_ERROR(compiler_jump_if(c, LOC(m->pattern), m->guard, pc->fail_pop[0], 0));
        }
        // Success! Pop the subject off, we're done with it:
        if (i != cases - has_default - 1) {
            ADDOP(c, LOC(m->pattern), POP_TOP);
        }
        VISIT_SEQ(c, stmt, m->body);
        ADDOP_JUMP(c, NO_LOCATION, JUMP, end);
        // If the pattern fails to match, we want the line number of the
        // cleanup to be associated with the failed pattern, not the last line
        // of the body
        RETURN_IF_ERROR(emit_and_reset_fail_pop(c, LOC(m->pattern), pc));
    }
    if (has_default) {
        // A trailing "case _" is common, and lets us save a bit of redundant
        // pushing and popping in the loop above:
        m = asdl_seq_GET(s->v.Match.cases, cases - 1);
        if (cases == 1) {
            // No matches. Done with the subject:
            ADDOP(c, LOC(m->pattern), POP_TOP);
        }
        else {
            // Show line coverage for default case (it doesn't create bytecode)
            ADDOP(c, LOC(m->pattern), NOP);
        }
        if (m->guard) {
            RETURN_IF_ERROR(compiler_jump_if(c, LOC(m->pattern), m->guard, end, 0));
        }
        VISIT_SEQ(c, stmt, m->body);
    }
    USE_LABEL(c, end);
    return SUCCESS;
}

static int
compiler_match(struct compiler *c, stmt_ty s)
{
    pattern_context pc;
    pc.fail_pop = NULL;
    int result = compiler_match_inner(c, s, &pc);
    PyObject_Free(pc.fail_pop);
    return result;
}

#undef WILDCARD_CHECK
#undef WILDCARD_STAR_CHECK


/* End of the compiler section, beginning of the assembler section */


struct assembler {
    PyObject *a_bytecode;  /* bytes containing bytecode */
    int a_offset;              /* offset into bytecode */
    PyObject *a_except_table;  /* bytes containing exception table */
    int a_except_table_off;    /* offset into exception table */
    /* Location Info */
    int a_lineno;          /* lineno of last emitted instruction */
    PyObject* a_linetable; /* bytes containing location info */
    int a_location_off;    /* offset of last written location info frame */
    PyObject *a_profiletable; /* table of offsets of profiled instructions */
};

static basicblock**
make_cfg_traversal_stack(basicblock *entryblock) {
    int nblocks = 0;
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        b->b_visited = 0;
        nblocks++;
    }
    basicblock **stack = (basicblock **)PyMem_Malloc(sizeof(basicblock *) * nblocks);
    if (!stack) {
        PyErr_NoMemory();
    }
    return stack;
}

Py_LOCAL_INLINE(void)
stackdepth_push(basicblock ***sp, basicblock *b, int depth)
{
    assert(b->b_startdepth < 0 || b->b_startdepth == depth);
    if (b->b_startdepth < depth && b->b_startdepth < 100) {
        assert(b->b_startdepth < 0);
        b->b_startdepth = depth;
        *(*sp)++ = b;
    }
}

/* Find the flow path that needs the largest stack.  We assume that
 * cycles in the flow graph have no net effect on the stack depth.
 */
static int
stackdepth(basicblock *entryblock, int code_flags)
{
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        b->b_startdepth = INT_MIN;
    }
    basicblock **stack = make_cfg_traversal_stack(entryblock);
    if (!stack) {
        return -1;
    }

    int maxdepth = 0;
    basicblock **sp = stack;
    if (code_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) {
        stackdepth_push(&sp, entryblock, 1);
    } else {
        stackdepth_push(&sp, entryblock, 0);
    }

    while (sp != stack) {
        basicblock *b = *--sp;
        int depth = b->b_startdepth;
        assert(depth >= 0);
        basicblock *next = b->b_next;
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            int effect = stack_effect(instr->i_opcode, instr->i_oparg, 0);
            if (effect == PY_INVALID_STACK_EFFECT) {
                PyErr_Format(PyExc_SystemError,
                             "compiler stack_effect(opcode=%d, arg=%i) failed",
                             instr->i_opcode, instr->i_oparg);
                return -1;
            }
            int new_depth = depth + effect;
            assert(new_depth >= 0); /* invalid code or bug in stackdepth() */
            if (new_depth > maxdepth) {
                maxdepth = new_depth;
            }
            if (HAS_TARGET(instr->i_opcode)) {
                effect = stack_effect(instr->i_opcode, instr->i_oparg, 1);
                assert(effect != PY_INVALID_STACK_EFFECT);
                int target_depth = depth + effect;
                assert(target_depth >= 0); /* invalid code or bug in stackdepth() */
                if (target_depth > maxdepth) {
                    maxdepth = target_depth;
                }
                stackdepth_push(&sp, instr->i_target, target_depth);
            }
            depth = new_depth;
            assert(!IS_ASSEMBLER_OPCODE(instr->i_opcode));
            if (IS_UNCONDITIONAL_JUMP_OPCODE(instr->i_opcode) ||
                IS_SCOPE_EXIT_OPCODE(instr->i_opcode))
            {
                /* remaining code is dead */
                next = NULL;
                break;
            }
            if (instr->i_opcode == YIELD_VALUE) {
                instr->i_oparg = depth;
            }
        }
        if (next != NULL) {
            assert(BB_HAS_FALLTHROUGH(b));
            stackdepth_push(&sp, next, depth);
        }
    }
    PyMem_Free(stack);
    return maxdepth;
}

static int
assemble_init(struct assembler *a, int firstlineno)
{
    memset(a, 0, sizeof(struct assembler));
    a->a_lineno = firstlineno;
    a->a_linetable = NULL;
    a->a_location_off = 0;
    a->a_except_table = NULL;
    a->a_bytecode = PyBytes_FromStringAndSize(NULL, DEFAULT_CODE_SIZE);
    if (a->a_bytecode == NULL) {
        goto error;
    }
    a->a_linetable = PyBytes_FromStringAndSize(NULL, DEFAULT_CNOTAB_SIZE);
    if (a->a_linetable == NULL) {
        goto error;
    }
    a->a_except_table = PyBytes_FromStringAndSize(NULL, DEFAULT_LNOTAB_SIZE);
    if (a->a_except_table == NULL) {
        goto error;
    }
    return 0;
error:
    Py_XDECREF(a->a_bytecode);
    Py_XDECREF(a->a_linetable);
    Py_XDECREF(a->a_except_table);
    return -1;
}

static void
assemble_free(struct assembler *a)
{
    Py_XDECREF(a->a_bytecode);
    Py_XDECREF(a->a_linetable);
    Py_XDECREF(a->a_except_table);
    Py_XDECREF(a->a_profiletable);
}

static int
blocksize(basicblock *b)
{
    int size = 0;
    for (int i = 0; i < b->b_iused; i++) {
        size += instr_size(&b->b_instr[i]);
    }
    return size;
}

static basicblock *
push_except_block(ExceptStack *stack, struct instr *setup) {
    assert(is_block_push(setup));
    int opcode = setup->i_opcode;
    basicblock * target = setup->i_target;
    if (opcode == SETUP_WITH || opcode == SETUP_CLEANUP) {
        target->b_preserve_lasti = 1;
    }
    stack->handlers[++stack->depth] = target;
    return target;
}

static basicblock *
pop_except_block(ExceptStack *stack) {
    assert(stack->depth > 0);
    return stack->handlers[--stack->depth];
}

static basicblock *
except_stack_top(ExceptStack *stack) {
    return stack->handlers[stack->depth];
}

static ExceptStack *
make_except_stack(void) {
    ExceptStack *new = PyMem_Malloc(sizeof(ExceptStack));
    if (new == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    new->depth = 0;
    new->handlers[0] = NULL;
    return new;
}

static ExceptStack *
copy_except_stack(ExceptStack *stack) {
    ExceptStack *copy = PyMem_Malloc(sizeof(ExceptStack));
    if (copy == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    memcpy(copy, stack, sizeof(ExceptStack));
    return copy;
}

static int
label_exception_targets(basicblock *entryblock) {
    basicblock **todo_stack = make_cfg_traversal_stack(entryblock);
    if (todo_stack == NULL) {
        return -1;
    }
    ExceptStack *except_stack = make_except_stack();
    if (except_stack == NULL) {
        PyMem_Free(todo_stack);
        PyErr_NoMemory();
        return -1;
    }
    except_stack->depth = 0;
    todo_stack[0] = entryblock;
    entryblock->b_visited = 1;
    entryblock->b_exceptstack = except_stack;
    basicblock **todo = &todo_stack[1];
    basicblock *handler = NULL;
    while (todo > todo_stack) {
        todo--;
        basicblock *b = todo[0];
        assert(b->b_visited == 1);
        except_stack = b->b_exceptstack;
        assert(except_stack != NULL);
        b->b_exceptstack = NULL;
        handler = except_stack_top(except_stack);
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (is_block_push(instr)) {
                if (!instr->i_target->b_visited) {
                    ExceptStack *copy = copy_except_stack(except_stack);
                    if (copy == NULL) {
                        goto error;
                    }
                    instr->i_target->b_exceptstack = copy;
                    todo[0] = instr->i_target;
                    instr->i_target->b_visited = 1;
                    todo++;
                }
                handler = push_except_block(except_stack, instr);
            }
            else if (instr->i_opcode == POP_BLOCK) {
                handler = pop_except_block(except_stack);
            }
            else if (is_jump(instr)) {
                instr->i_except = handler;
                assert(i == b->b_iused -1);
                if (!instr->i_target->b_visited) {
                    if (BB_HAS_FALLTHROUGH(b)) {
                        ExceptStack *copy = copy_except_stack(except_stack);
                        if (copy == NULL) {
                            goto error;
                        }
                        instr->i_target->b_exceptstack = copy;
                    }
                    else {
                        instr->i_target->b_exceptstack = except_stack;
                        except_stack = NULL;
                    }
                    todo[0] = instr->i_target;
                    instr->i_target->b_visited = 1;
                    todo++;
                }
            }
            else {
                instr->i_except = handler;
            }
        }
        if (BB_HAS_FALLTHROUGH(b) && !b->b_next->b_visited) {
            assert(except_stack != NULL);
            b->b_next->b_exceptstack = except_stack;
            todo[0] = b->b_next;
            b->b_next->b_visited = 1;
            todo++;
        }
        else if (except_stack != NULL) {
           PyMem_Free(except_stack);
        }
    }
#ifdef Py_DEBUG
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        assert(b->b_exceptstack == NULL);
    }
#endif
    PyMem_Free(todo_stack);
    return 0;
error:
    PyMem_Free(todo_stack);
    PyMem_Free(except_stack);
    return -1;
}


static int
mark_except_handlers(basicblock *entryblock) {
#ifndef NDEBUG
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        assert(!b->b_except_handler);
    }
#endif
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        for (int i=0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (is_block_push(instr)) {
                instr->i_target->b_except_handler = 1;
            }
        }
    }
    return 0;
}

static int
mark_warm(basicblock *entryblock) {
    basicblock **stack = make_cfg_traversal_stack(entryblock);
    if (stack == NULL) {
        return -1;
    }
    basicblock **sp = stack;

    *sp++ = entryblock;
    entryblock->b_visited = 1;
    while (sp > stack) {
        basicblock *b = *(--sp);
        assert(!b->b_except_handler);
        b->b_warm = 1;
        basicblock *next = b->b_next;
        if (next && BB_HAS_FALLTHROUGH(b) && !next->b_visited) {
            *sp++ = next;
            next->b_visited = 1;
        }
        for (int i=0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (is_jump(instr) && !instr->i_target->b_visited) {
                *sp++ = instr->i_target;
                instr->i_target->b_visited = 1;
            }
        }
    }
    PyMem_Free(stack);
    return 0;
}

static int
mark_cold(basicblock *entryblock) {
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        assert(!b->b_cold && !b->b_warm);
    }
    if (mark_warm(entryblock) < 0) {
        return -1;
    }

    basicblock **stack = make_cfg_traversal_stack(entryblock);
    if (stack == NULL) {
        return -1;
    }

    basicblock **sp = stack;
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        if (b->b_except_handler) {
            assert(!b->b_warm);
            *sp++ = b;
            b->b_visited = 1;
        }
    }

    while (sp > stack) {
        basicblock *b = *(--sp);
        b->b_cold = 1;
        basicblock *next = b->b_next;
        if (next && BB_HAS_FALLTHROUGH(b)) {
            if (!next->b_warm && !next->b_visited) {
                *sp++ = next;
                next->b_visited = 1;
            }
        }
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (is_jump(instr)) {
                assert(i == b->b_iused - 1);
                basicblock *target = b->b_instr[i].i_target;
                if (!target->b_warm && !target->b_visited) {
                    *sp++ = target;
                    target->b_visited = 1;
                }
            }
        }
    }
    PyMem_Free(stack);
    return 0;
}

static int
remove_redundant_jumps(cfg_builder *g);

static int
push_cold_blocks_to_end(cfg_builder *g, int code_flags) {
    basicblock *entryblock = g->g_entryblock;
    if (entryblock->b_next == NULL) {
        /* single basicblock, no need to reorder */
        return 0;
    }
    if (mark_cold(entryblock) < 0) {
        return -1;
    }

    /* If we have a cold block with fallthrough to a warm block, add */
    /* an explicit jump instead of fallthrough */
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        if (b->b_cold && BB_HAS_FALLTHROUGH(b) && b->b_next && b->b_next->b_warm) {
            basicblock *explicit_jump = cfg_builder_new_block(g);
            if (explicit_jump == NULL) {
                return -1;
            }
            basicblock_addop(explicit_jump, JUMP, b->b_next->b_label, NO_LOCATION);
            explicit_jump->b_cold = 1;
            explicit_jump->b_next = b->b_next;
            b->b_next = explicit_jump;

            /* set target */
            struct instr *last = basicblock_last_instr(explicit_jump);
            last->i_target = explicit_jump->b_next;
        }
    }

    assert(!entryblock->b_cold);  /* First block can't be cold */
    basicblock *cold_blocks = NULL;
    basicblock *cold_blocks_tail = NULL;

    basicblock *b = entryblock;
    while(b->b_next) {
        assert(!b->b_cold);
        while (b->b_next && !b->b_next->b_cold) {
            b = b->b_next;
        }
        if (b->b_next == NULL) {
            /* no more cold blocks */
            break;
        }

        /* b->b_next is the beginning of a cold streak */
        assert(!b->b_cold && b->b_next->b_cold);

        basicblock *b_end = b->b_next;
        while (b_end->b_next && b_end->b_next->b_cold) {
            b_end = b_end->b_next;
        }

        /* b_end is the end of the cold streak */
        assert(b_end && b_end->b_cold);
        assert(b_end->b_next == NULL || !b_end->b_next->b_cold);

        if (cold_blocks == NULL) {
            cold_blocks = b->b_next;
        }
        else {
            cold_blocks_tail->b_next = b->b_next;
        }
        cold_blocks_tail = b_end;
        b->b_next = b_end->b_next;
        b_end->b_next = NULL;
    }
    assert(b != NULL && b->b_next == NULL);
    b->b_next = cold_blocks;

    if (cold_blocks != NULL) {
        if (remove_redundant_jumps(g) < 0) {
            return -1;
        }
    }
    return 0;
}

static void
convert_exception_handlers_to_nops(basicblock *entryblock) {
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (is_block_push(instr) || instr->i_opcode == POP_BLOCK) {
                INSTR_SET_OP0(instr, NOP);
            }
        }
    }
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        remove_redundant_nops(b);
    }
}

static inline void
write_except_byte(struct assembler *a, int byte) {
    unsigned char *p = (unsigned char *) PyBytes_AS_STRING(a->a_except_table);
    p[a->a_except_table_off++] = byte;
}

#define CONTINUATION_BIT 64

static void
assemble_emit_exception_table_item(struct assembler *a, int value, int msb)
{
    assert ((msb | 128) == 128);
    assert(value >= 0 && value < (1 << 30));
    if (value >= 1 << 24) {
        write_except_byte(a, (value >> 24) | CONTINUATION_BIT | msb);
        msb = 0;
    }
    if (value >= 1 << 18) {
        write_except_byte(a, ((value >> 18)&0x3f) | CONTINUATION_BIT | msb);
        msb = 0;
    }
    if (value >= 1 << 12) {
        write_except_byte(a, ((value >> 12)&0x3f) | CONTINUATION_BIT | msb);
        msb = 0;
    }
    if (value >= 1 << 6) {
        write_except_byte(a, ((value >> 6)&0x3f) | CONTINUATION_BIT | msb);
        msb = 0;
    }
    write_except_byte(a, (value&0x3f) | msb);
}

/* See Objects/exception_handling_notes.txt for details of layout */
#define MAX_SIZE_OF_ENTRY 20

static int
assemble_emit_exception_table_entry(struct assembler *a, int start, int end, basicblock *handler)
{
    Py_ssize_t len = PyBytes_GET_SIZE(a->a_except_table);
    if (a->a_except_table_off + MAX_SIZE_OF_ENTRY >= len) {
        if (_PyBytes_Resize(&a->a_except_table, len * 2) < 0) {
            return -1;
        }
    }
    int size = end-start;
    assert(end > start);
    int target = handler->b_offset;
    int depth = handler->b_startdepth - 1;
    if (handler->b_preserve_lasti) {
        depth -= 1;
    }
    assert(depth >= 0);
    int depth_lasti = (depth<<1) | handler->b_preserve_lasti;
    assemble_emit_exception_table_item(a, start, (1<<7));
    assemble_emit_exception_table_item(a, size, 0);
    assemble_emit_exception_table_item(a, target, 0);
    assemble_emit_exception_table_item(a, depth_lasti, 0);
    return 0;
}

static int
assemble_exception_table(struct assembler *a, basicblock *entryblock)
{
    basicblock *b;
    int ioffset = 0;
    basicblock *handler = NULL;
    int start = -1;
    for (b = entryblock; b != NULL; b = b->b_next) {
        ioffset = b->b_offset;
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (instr->i_except != handler) {
                if (handler != NULL) {
                    if (assemble_emit_exception_table_entry(a, start, ioffset, handler) < 0) {
                        return -1;
                    }
                }
                start = ioffset;
                handler = instr->i_except;
            }
            ioffset += instr_size(instr);
        }
    }
    if (handler != NULL) {
        if (assemble_emit_exception_table_entry(a, start, ioffset, handler) < 0) {
            return -1;
        }
    }
    return 0;
}

/* Code location emitting code. See locations.md for a description of the format. */

#define MSB 0x80

static void
write_location_byte(struct assembler* a, int val)
{
    PyBytes_AS_STRING(a->a_linetable)[a->a_location_off] = val&255;
    a->a_location_off++;
}


static uint8_t *
location_pointer(struct assembler* a)
{
    return (uint8_t *)PyBytes_AS_STRING(a->a_linetable) +
        a->a_location_off;
}

static void
write_location_first_byte(struct assembler* a, int code, int length)
{
    a->a_location_off += write_location_entry_start(
        location_pointer(a), code, length);
}

static void
write_location_varint(struct assembler* a, unsigned int val)
{
    uint8_t *ptr = location_pointer(a);
    a->a_location_off += write_varint(ptr, val);
}


static void
write_location_signed_varint(struct assembler* a, int val)
{
    uint8_t *ptr = location_pointer(a);
    a->a_location_off += write_signed_varint(ptr, val);
}

static void
write_location_info_short_form(struct assembler* a, int length, int column, int end_column)
{
    assert(length > 0 &&  length <= 8);
    int column_low_bits = column & 7;
    int column_group = column >> 3;
    assert(column < 80);
    assert(end_column >= column);
    assert(end_column - column < 16);
    write_location_first_byte(a, PY_CODE_LOCATION_INFO_SHORT0 + column_group, length);
    write_location_byte(a, (column_low_bits << 4) | (end_column - column));
}

static void
write_location_info_oneline_form(struct assembler* a, int length, int line_delta, int column, int end_column)
{
    assert(length > 0 &&  length <= 8);
    assert(line_delta >= 0 && line_delta < 3);
    assert(column < 128);
    assert(end_column < 128);
    write_location_first_byte(a, PY_CODE_LOCATION_INFO_ONE_LINE0 + line_delta, length);
    write_location_byte(a, column);
    write_location_byte(a, end_column);
}

static void
write_location_info_long_form(struct assembler* a, location loc, int length)
{
    assert(length > 0 &&  length <= 8);
    write_location_first_byte(a, PY_CODE_LOCATION_INFO_LONG, length);
    write_location_signed_varint(a, loc.lineno - a->a_lineno);
    assert(loc.end_lineno >= loc.lineno);
    write_location_varint(a, loc.end_lineno - loc.lineno);
    write_location_varint(a, loc.col_offset + 1);
    write_location_varint(a, loc.end_col_offset + 1);
}

static void
write_location_info_none(struct assembler* a, int length)
{
    write_location_first_byte(a, PY_CODE_LOCATION_INFO_NONE, length);
}

static void
write_location_info_no_column(struct assembler* a, int length, int line_delta)
{
    write_location_first_byte(a, PY_CODE_LOCATION_INFO_NO_COLUMNS, length);
    write_location_signed_varint(a, line_delta);
}

#define THEORETICAL_MAX_ENTRY_SIZE 25 /* 1 + 6 + 6 + 6 + 6 */

static int
write_location_info_entry(struct assembler* a, location loc, int isize)
{
    Py_ssize_t len = PyBytes_GET_SIZE(a->a_linetable);
    if (a->a_location_off + THEORETICAL_MAX_ENTRY_SIZE >= len) {
        assert(len > THEORETICAL_MAX_ENTRY_SIZE);
        if (_PyBytes_Resize(&a->a_linetable, len*2) < 0) {
            return -1;
        }
    }
    if (loc.lineno < 0) {
        write_location_info_none(a, isize);
        return 0;
    }
    int line_delta = loc.lineno - a->a_lineno;
    int column = loc.col_offset;
    int end_column = loc.end_col_offset;
    assert(column >= -1);
    assert(end_column >= -1);
    if (column < 0 || end_column < 0) {
        if (loc.end_lineno == loc.lineno || loc.end_lineno == -1) {
            write_location_info_no_column(a, isize, line_delta);
            a->a_lineno = loc.lineno;
            return 0;
        }
    }
    else if (loc.end_lineno == loc.lineno) {
        if (line_delta == 0 && column < 80 && end_column - column < 16 && end_column >= column) {
            write_location_info_short_form(a, isize, column, end_column);
            return 0;
        }
        if (line_delta >= 0 && line_delta < 3 && column < 128 && end_column < 128) {
            write_location_info_oneline_form(a, isize, line_delta, column, end_column);
            a->a_lineno = loc.lineno;
            return 0;
        }
    }
    write_location_info_long_form(a, loc, isize);
    a->a_lineno = loc.lineno;
    return 0;
}

static int
assemble_emit_location(struct assembler* a, location loc, int isize)
{
    if (isize == 0) {
        return 0;
    }
    while (isize > 8) {
        if (write_location_info_entry(a, loc, 8)) {
            return -1;
        }
        isize -= 8;
    }
    return write_location_info_entry(a, loc, isize);
}

/* assemble_emit()
   Extend the bytecode with a new instruction.
   Update lnotab if necessary.
*/

static int
assemble_emit(struct assembler *a, struct instr *i)
{
    Py_ssize_t len = PyBytes_GET_SIZE(a->a_bytecode);
    _Py_CODEUNIT *code;

    int size = instr_size(i);
    if (a->a_offset + size >= len / (int)sizeof(_Py_CODEUNIT)) {
        if (len > PY_SSIZE_T_MAX / 2) {
            return -1;
        }
        if (_PyBytes_Resize(&a->a_bytecode, len * 2) < 0) {
            return -1;
        }
    }
    code = (_Py_CODEUNIT *)PyBytes_AS_STRING(a->a_bytecode) + a->a_offset;
    a->a_offset += size;
    write_instr(code, i, size);
    return 0;
}

static int
normalize_jumps_in_block(cfg_builder *g, basicblock *b) {
    struct instr *last = basicblock_last_instr(b);
    if (last == NULL || !is_jump(last)) {
        return 0;
    }
    assert(!IS_ASSEMBLER_OPCODE(last->i_opcode));
    bool is_forward = last->i_target->b_visited == 0;
    switch(last->i_opcode) {
        case JUMP:
            last->i_opcode = is_forward ? JUMP_FORWARD : JUMP_BACKWARD;
            return 0;
        case JUMP_NO_INTERRUPT:
            last->i_opcode = is_forward ?
                JUMP_FORWARD : JUMP_BACKWARD_NO_INTERRUPT;
            return 0;
    }
    int reversed_opcode = 0;
    switch(last->i_opcode) {
        case POP_JUMP_IF_NOT_NONE:
            reversed_opcode = POP_JUMP_IF_NONE;
            break;
        case POP_JUMP_IF_NONE:
            reversed_opcode = POP_JUMP_IF_NOT_NONE;
            break;
        case POP_JUMP_IF_FALSE:
            reversed_opcode = POP_JUMP_IF_TRUE;
            break;
        case POP_JUMP_IF_TRUE:
            reversed_opcode = POP_JUMP_IF_FALSE;
            break;
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            if (!is_forward) {
                /* As far as we can tell, the compiler never emits
                 * these jumps with a backwards target. If/when this
                 * exception is raised, we have found a use case for
                 * a backwards version of this jump (or to replace
                 * it with the sequence (COPY 1, POP_JUMP_IF_T/F, POP)
                 */
                PyErr_Format(PyExc_SystemError,
                    "unexpected %s jumping backwards",
                    last->i_opcode == JUMP_IF_TRUE_OR_POP ?
                        "JUMP_IF_TRUE_OR_POP" : "JUMP_IF_FALSE_OR_POP");
            }
            return 0;
    }
    if (is_forward) {
        return 0;
    }

    /* transform 'conditional jump T' to
     * 'reversed_jump b_next' followed by 'jump_backwards T'
     */

    basicblock *target = last->i_target;
    basicblock *backwards_jump = cfg_builder_new_block(g);
    if (backwards_jump == NULL) {
        return -1;
    }
    basicblock_addop(backwards_jump, JUMP, target->b_label, NO_LOCATION);
    backwards_jump->b_instr[0].i_target = target;
    last->i_opcode = reversed_opcode;
    last->i_target = b->b_next;

    backwards_jump->b_cold = b->b_cold;
    backwards_jump->b_next = b->b_next;
    b->b_next = backwards_jump;
    return 0;
}

static int
normalize_jumps(cfg_builder *g)
{
    basicblock *entryblock = g->g_entryblock;
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        b->b_visited = 0;
    }
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        b->b_visited = 1;
        if (normalize_jumps_in_block(g, b) < 0) {
            return -1;
        }
    }
    return 0;
}

static void
assemble_jump_offsets(basicblock *entryblock)
{
    int bsize, totsize, extended_arg_recompile;

    /* Compute the size of each block and fixup jump args.
       Replace block pointer with position in bytecode. */
    do {
        totsize = 0;
        for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
            bsize = blocksize(b);
            b->b_offset = totsize;
            totsize += bsize;
        }
        extended_arg_recompile = 0;
        for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
            bsize = b->b_offset;
            for (int i = 0; i < b->b_iused; i++) {
                struct instr *instr = &b->b_instr[i];
                int isize = instr_size(instr);
                /* Relative jumps are computed relative to
                   the instruction pointer after fetching
                   the jump instruction.
                */
                bsize += isize;
                if (is_jump(instr)) {
                    instr->i_oparg = instr->i_target->b_offset;
                    if (is_relative_jump(instr)) {
                        if (instr->i_oparg < bsize) {
                            assert(IS_BACKWARDS_JUMP_OPCODE(instr->i_opcode));
                            instr->i_oparg = bsize - instr->i_oparg;
                        }
                        else {
                            assert(!IS_BACKWARDS_JUMP_OPCODE(instr->i_opcode));
                            instr->i_oparg -= bsize;
                        }
                    }
                    else {
                        assert(!IS_BACKWARDS_JUMP_OPCODE(instr->i_opcode));
                    }
                    if (instr_size(instr) != isize) {
                        extended_arg_recompile = 1;
                    }
                }
            }
        }

    /* XXX: This is an awful hack that could hurt performance, but
        on the bright side it should work until we come up
        with a better solution.

        The issue is that in the first loop blocksize() is called
        which calls instr_size() which requires i_oparg be set
        appropriately. There is a bootstrap problem because
        i_oparg is calculated in the second loop above.

        So we loop until we stop seeing new EXTENDED_ARGs.
        The only EXTENDED_ARGs that could be popping up are
        ones in jump instructions.  So this should converge
        fairly quickly.
    */
    } while (extended_arg_recompile);
}


// helper functions for add_checks_for_loads_of_unknown_variables
static inline void
maybe_push(basicblock *b, uint64_t unsafe_mask, basicblock ***sp)
{
    // Push b if the unsafe mask is giving us any new information.
    // To avoid overflowing the stack, only allow each block once.
    // Use b->b_visited=1 to mean that b is currently on the stack.
    uint64_t both = b->b_unsafe_locals_mask | unsafe_mask;
    if (b->b_unsafe_locals_mask != both) {
        b->b_unsafe_locals_mask = both;
        // More work left to do.
        if (!b->b_visited) {
            // not on the stack, so push it.
            *(*sp)++ = b;
            b->b_visited = 1;
        }
    }
}

static void
scan_block_for_locals(basicblock *b, basicblock ***sp)
{
    // bit i is set if local i is potentially uninitialized
    uint64_t unsafe_mask = b->b_unsafe_locals_mask;
    for (int i = 0; i < b->b_iused; i++) {
        struct instr *instr = &b->b_instr[i];
        assert(instr->i_opcode != EXTENDED_ARG);
        assert(!IS_SUPERINSTRUCTION_OPCODE(instr->i_opcode));
        if (instr->i_except != NULL) {
            maybe_push(instr->i_except, unsafe_mask, sp);
        }
        if (instr->i_oparg >= 64) {
            continue;
        }
        assert(instr->i_oparg >= 0);
        uint64_t bit = (uint64_t)1 << instr->i_oparg;
        switch (instr->i_opcode) {
            case DELETE_FAST:
                unsafe_mask |= bit;
                break;
            case STORE_FAST:
                unsafe_mask &= ~bit;
                break;
            case LOAD_FAST_CHECK:
                // If this doesn't raise, then the local is defined.
                unsafe_mask &= ~bit;
                break;
            case LOAD_FAST:
                if (unsafe_mask & bit) {
                    instr->i_opcode = LOAD_FAST_CHECK;
                }
                unsafe_mask &= ~bit;
                break;
        }
    }
    if (b->b_next && BB_HAS_FALLTHROUGH(b)) {
        maybe_push(b->b_next, unsafe_mask, sp);
    }
    struct instr *last = basicblock_last_instr(b);
    if (last && is_jump(last)) {
        assert(last->i_target != NULL);
        maybe_push(last->i_target, unsafe_mask, sp);
    }
}

static int
fast_scan_many_locals(basicblock *entryblock, int nlocals)
{
    assert(nlocals > 64);
    Py_ssize_t *states = PyMem_Calloc(nlocals - 64, sizeof(Py_ssize_t));
    if (states == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    Py_ssize_t blocknum = 0;
    // state[i - 64] == blocknum if local i is guaranteed to
    // be initialized, i.e., if it has had a previous LOAD_FAST or
    // STORE_FAST within that basicblock (not followed by DELETE_FAST).
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        blocknum++;
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            assert(instr->i_opcode != EXTENDED_ARG);
            assert(!IS_SUPERINSTRUCTION_OPCODE(instr->i_opcode));
            int arg = instr->i_oparg;
            if (arg < 64) {
                continue;
            }
            assert(arg >= 0);
            switch (instr->i_opcode) {
                case DELETE_FAST:
                    states[arg - 64] = blocknum - 1;
                    break;
                case STORE_FAST:
                    states[arg - 64] = blocknum;
                    break;
                case LOAD_FAST:
                    if (states[arg - 64] != blocknum) {
                        instr->i_opcode = LOAD_FAST_CHECK;
                    }
                    states[arg - 64] = blocknum;
                    break;
                case LOAD_FAST_CHECK:
                    Py_UNREACHABLE();
            }
        }
    }
    PyMem_Free(states);
    return 0;
}

static int
add_checks_for_loads_of_uninitialized_variables(basicblock *entryblock,
                                                struct compiler *c)
{
    int nlocals = (int)PyDict_GET_SIZE(c->u->u_varnames);
    if (nlocals == 0) {
        return 0;
    }
    if (nlocals > 64) {
        // To avoid O(nlocals**2) compilation, locals beyond the first
        // 64 are only analyzed one basicblock at a time: initialization
        // info is not passed between basicblocks.
        if (fast_scan_many_locals(entryblock, nlocals) < 0) {
            return -1;
        }
        nlocals = 64;
    }
    basicblock **stack = make_cfg_traversal_stack(entryblock);
    if (stack == NULL) {
        return -1;
    }
    basicblock **sp = stack;

    // First origin of being uninitialized:
    // The non-parameter locals in the entry block.
    int nparams = (int)PyList_GET_SIZE(c->u->u_ste->ste_varnames);
    uint64_t start_mask = 0;
    for (int i = nparams; i < nlocals; i++) {
        start_mask |= (uint64_t)1 << i;
    }
    maybe_push(entryblock, start_mask, &sp);

    // Second origin of being uninitialized:
    // There could be DELETE_FAST somewhere, so
    // be sure to scan each basicblock at least once.
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        scan_block_for_locals(b, &sp);
    }

    // Now propagate the uncertainty from the origins we found: Use
    // LOAD_FAST_CHECK for any LOAD_FAST where the local could be undefined.
    while (sp > stack) {
        basicblock *b = *--sp;
        // mark as no longer on stack
        b->b_visited = 0;
        scan_block_for_locals(b, &sp);
    }
    PyMem_Free(stack);
    return 0;
}

static PyObject *
dict_keys_inorder(PyObject *dict, Py_ssize_t offset)
{
    PyObject *tuple, *k, *v;
    Py_ssize_t i, pos = 0, size = PyDict_GET_SIZE(dict);

    tuple = PyTuple_New(size);
    if (tuple == NULL)
        return NULL;
    while (PyDict_Next(dict, &pos, &k, &v)) {
        i = PyLong_AS_LONG(v);
        assert((i - offset) < size);
        assert((i - offset) >= 0);
        PyTuple_SET_ITEM(tuple, i - offset, Py_NewRef(k));
    }
    return tuple;
}

static PyObject *
consts_dict_keys_inorder(PyObject *dict)
{
    PyObject *consts, *k, *v;
    Py_ssize_t i, pos = 0, size = PyDict_GET_SIZE(dict);

    consts = PyList_New(size);   /* PyCode_Optimize() requires a list */
    if (consts == NULL)
        return NULL;
    while (PyDict_Next(dict, &pos, &k, &v)) {
        i = PyLong_AS_LONG(v);
        /* The keys of the dictionary can be tuples wrapping a constant.
         * (see dict_add_o and _PyCode_ConstantKey). In that case
         * the object we want is always second. */
        if (PyTuple_CheckExact(k)) {
            k = PyTuple_GET_ITEM(k, 1);
        }
        assert(i < size);
        assert(i >= 0);
        PyList_SET_ITEM(consts, i, Py_NewRef(k));
    }
    return consts;
}

static int
compute_code_flags(struct compiler *c)
{
    PySTEntryObject *ste = c->u->u_ste;
    int flags = 0;
    if (ste->ste_type == FunctionBlock) {
        flags |= CO_NEWLOCALS | CO_OPTIMIZED;
        if (ste->ste_nested)
            flags |= CO_NESTED;
        if (ste->ste_generator && !ste->ste_coroutine)
            flags |= CO_GENERATOR;
        if (!ste->ste_generator && ste->ste_coroutine)
            flags |= CO_COROUTINE;
        if (ste->ste_generator && ste->ste_coroutine)
            flags |= CO_ASYNC_GENERATOR;
        if (ste->ste_varargs)
            flags |= CO_VARARGS;
        if (ste->ste_varkeywords)
            flags |= CO_VARKEYWORDS;
    }

    /* (Only) inherit compilerflags in PyCF_MASK */
    flags |= (c->c_flags.cf_flags & PyCF_MASK);

    if ((IS_TOP_LEVEL_AWAIT(c)) &&
         ste->ste_coroutine &&
         !ste->ste_generator) {
        flags |= CO_COROUTINE;
    }

    return flags;
}

// Merge *obj* with constant cache.
// Unlike merge_consts_recursive(), this function doesn't work recursively.
static int
merge_const_one(PyObject *const_cache, PyObject **obj)
{
    PyDict_CheckExact(const_cache);
    PyObject *key = _PyCode_ConstantKey(*obj);
    if (key == NULL) {
        return -1;
    }

    // t is borrowed reference
    PyObject *t = PyDict_SetDefault(const_cache, key, key);
    Py_DECREF(key);
    if (t == NULL) {
        return -1;
    }
    if (t == key) {  // obj is new constant.
        return 0;
    }

    if (PyTuple_CheckExact(t)) {
        // t is still borrowed reference
        t = PyTuple_GET_ITEM(t, 1);
    }

    Py_SETREF(*obj, Py_NewRef(t));
    return 0;
}

// This is in codeobject.c.
extern void _Py_set_localsplus_info(int, PyObject *, unsigned char,
                                   PyObject *, PyObject *);

static void
compute_localsplus_info(struct compiler *c, int nlocalsplus,
                        PyObject *names, PyObject *kinds)
{
    PyObject *k, *v;
    Py_ssize_t pos = 0;
    while (PyDict_Next(c->u->u_varnames, &pos, &k, &v)) {
        int offset = (int)PyLong_AS_LONG(v);
        assert(offset >= 0);
        assert(offset < nlocalsplus);
        // For now we do not distinguish arg kinds.
        _PyLocals_Kind kind = CO_FAST_LOCAL;
        if (PyDict_GetItem(c->u->u_cellvars, k) != NULL) {
            kind |= CO_FAST_CELL;
        }
        _Py_set_localsplus_info(offset, k, kind, names, kinds);
    }
    int nlocals = (int)PyDict_GET_SIZE(c->u->u_varnames);

    // This counter mirrors the fix done in fix_cell_offsets().
    int numdropped = 0;
    pos = 0;
    while (PyDict_Next(c->u->u_cellvars, &pos, &k, &v)) {
        if (PyDict_GetItem(c->u->u_varnames, k) != NULL) {
            // Skip cells that are already covered by locals.
            numdropped += 1;
            continue;
        }
        int offset = (int)PyLong_AS_LONG(v);
        assert(offset >= 0);
        offset += nlocals - numdropped;
        assert(offset < nlocalsplus);
        _Py_set_localsplus_info(offset, k, CO_FAST_CELL, names, kinds);
    }

    pos = 0;
    while (PyDict_Next(c->u->u_freevars, &pos, &k, &v)) {
        int offset = (int)PyLong_AS_LONG(v);
        assert(offset >= 0);
        offset += nlocals - numdropped;
        assert(offset < nlocalsplus);
        _Py_set_localsplus_info(offset, k, CO_FAST_FREE, names, kinds);
    }
}

static PyCodeObject *
makecode(struct compiler *c, struct assembler *a, PyObject *constslist,
         int maxdepth, int nlocalsplus, int code_flags)
{
    PyCodeObject *co = NULL;
    PyObject *names = NULL;
    PyObject *consts = NULL;
    PyObject *localsplusnames = NULL;
    PyObject *localspluskinds = NULL;

    names = dict_keys_inorder(c->u->u_names, 0);
    if (!names) {
        goto error;
    }
    if (merge_const_one(c->c_const_cache, &names) < 0) {
        goto error;
    }

    consts = PyList_AsTuple(constslist); /* PyCode_New requires a tuple */
    if (consts == NULL) {
        goto error;
    }
    if (merge_const_one(c->c_const_cache, &consts) < 0) {
        goto error;
    }

    assert(c->u->u_posonlyargcount < INT_MAX);
    assert(c->u->u_argcount < INT_MAX);
    assert(c->u->u_kwonlyargcount < INT_MAX);
    int posonlyargcount = (int)c->u->u_posonlyargcount;
    int posorkwargcount = (int)c->u->u_argcount;
    assert(INT_MAX - posonlyargcount - posorkwargcount > 0);
    int kwonlyargcount = (int)c->u->u_kwonlyargcount;

    localsplusnames = PyTuple_New(nlocalsplus);
    if (localsplusnames == NULL) {
        goto error;
    }
    localspluskinds = PyBytes_FromStringAndSize(NULL, nlocalsplus);
    if (localspluskinds == NULL) {
        goto error;
    }
    compute_localsplus_info(c, nlocalsplus, localsplusnames, localspluskinds);

    struct _PyCodeConstructor con = {
        .filename = c->c_filename,
        .name = c->u->u_name,
        .qualname = c->u->u_qualname ? c->u->u_qualname : c->u->u_name,
        .flags = code_flags,

        .code = a->a_bytecode,
        .firstlineno = c->u->u_firstlineno,
        .linetable = a->a_linetable,

        .consts = consts,
        .names = names,

        .localsplusnames = localsplusnames,
        .localspluskinds = localspluskinds,

        .argcount = posonlyargcount + posorkwargcount,
        .posonlyargcount = posonlyargcount,
        .kwonlyargcount = kwonlyargcount,

        .stacksize = maxdepth,

        .exceptiontable = a->a_except_table,
        .profiletable = a->a_profiletable,
    };

    if (_PyCode_Validate(&con) < 0) {
        goto error;
    }

    if (merge_const_one(c->c_const_cache, &localsplusnames) < 0) {
        goto error;
    }
    con.localsplusnames = localsplusnames;

    co = _PyCode_New(&con);
    if (co == NULL) {
        goto error;
    }

 error:
    Py_XDECREF(names);
    Py_XDECREF(consts);
    Py_XDECREF(localsplusnames);
    Py_XDECREF(localspluskinds);
    return co;
}


/* For debugging purposes only */
#if 0
static void
dump_instr(struct instr *i)
{
    const char *jrel = (is_relative_jump(i)) ? "jrel " : "";
    const char *jabs = (is_jump(i) && !is_relative_jump(i))? "jabs " : "";

    char arg[128];

    *arg = '\0';
    if (HAS_ARG(i->i_opcode)) {
        sprintf(arg, "arg: %d ", i->i_oparg);
    }
    if (HAS_TARGET(i->i_opcode)) {
        sprintf(arg, "target: %p [%d] ", i->i_target, i->i_oparg);
    }
    fprintf(stderr, "line: %d, opcode: %d %s%s%s\n",
                    i->i_loc.lineno, i->i_opcode, arg, jabs, jrel);
}

static void
dump_basicblock(const basicblock *b)
{
    const char *b_return = basicblock_returns(b) ? "return " : "";
    fprintf(stderr, "%d: [EH=%d CLD=%d WRM=%d NO_FT=%d %p] used: %d, depth: %d, offset: %d %s\n",
        b->b_label, b->b_except_handler, b->b_cold, b->b_warm, BB_NO_FALLTHROUGH(b), b, b->b_iused,
        b->b_startdepth, b->b_offset, b_return);
    if (b->b_instr) {
        int i;
        for (i = 0; i < b->b_iused; i++) {
            fprintf(stderr, "  [%02d] ", i);
            dump_instr(b->b_instr + i);
        }
    }
}
#endif


static int
translate_jump_labels_to_targets(basicblock *entryblock);

static int
optimize_cfg(cfg_builder *g, PyObject *consts, PyObject *const_cache);

static int
remove_unused_consts(basicblock *entryblock, PyObject *consts);

/* Duplicates exit BBs, so that line numbers can be propagated to them */
static int
duplicate_exits_without_lineno(cfg_builder *g);

static int *
build_cellfixedoffsets(struct compiler *c)
{
    int nlocals = (int)PyDict_GET_SIZE(c->u->u_varnames);
    int ncellvars = (int)PyDict_GET_SIZE(c->u->u_cellvars);
    int nfreevars = (int)PyDict_GET_SIZE(c->u->u_freevars);

    int noffsets = ncellvars + nfreevars;
    int *fixed = PyMem_New(int, noffsets);
    if (fixed == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (int i = 0; i < noffsets; i++) {
        fixed[i] = nlocals + i;
    }

    PyObject *varname, *cellindex;
    Py_ssize_t pos = 0;
    while (PyDict_Next(c->u->u_cellvars, &pos, &varname, &cellindex)) {
        PyObject *varindex = PyDict_GetItem(c->u->u_varnames, varname);
        if (varindex != NULL) {
            assert(PyLong_AS_LONG(cellindex) < INT_MAX);
            assert(PyLong_AS_LONG(varindex) < INT_MAX);
            int oldindex = (int)PyLong_AS_LONG(cellindex);
            int argoffset = (int)PyLong_AS_LONG(varindex);
            fixed[oldindex] = argoffset;
        }
    }

    return fixed;
}

static int
insert_prefix_instructions(struct compiler *c, basicblock *entryblock,
                           int *fixed, int nfreevars, int code_flags)
{
    assert(c->u->u_firstlineno > 0);

    /* Add the generator prefix instructions. */
    if (code_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)) {
        struct instr make_gen = {
            .i_opcode = RETURN_GENERATOR,
            .i_oparg = 0,
            .i_loc = LOCATION(c->u->u_firstlineno, c->u->u_firstlineno, -1, -1),
            .i_target = NULL,
        };
        if (insert_instruction(entryblock, 0, &make_gen) < 0) {
            return -1;
        }
        struct instr pop_top = {
            .i_opcode = POP_TOP,
            .i_oparg = 0,
            .i_loc = NO_LOCATION,
            .i_target = NULL,
        };
        if (insert_instruction(entryblock, 1, &pop_top) < 0) {
            return -1;
        }
    }

    /* Set up cells for any variable that escapes, to be put in a closure. */
    const int ncellvars = (int)PyDict_GET_SIZE(c->u->u_cellvars);
    if (ncellvars) {
        // c->u->u_cellvars has the cells out of order so we sort them
        // before adding the MAKE_CELL instructions.  Note that we
        // adjust for arg cells, which come first.
        const int nvars = ncellvars + (int)PyDict_GET_SIZE(c->u->u_varnames);
        int *sorted = PyMem_RawCalloc(nvars, sizeof(int));
        if (sorted == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        for (int i = 0; i < ncellvars; i++) {
            sorted[fixed[i]] = i + 1;
        }
        for (int i = 0, ncellsused = 0; ncellsused < ncellvars; i++) {
            int oldindex = sorted[i] - 1;
            if (oldindex == -1) {
                continue;
            }
            struct instr make_cell = {
                .i_opcode = MAKE_CELL,
                // This will get fixed in offset_derefs().
                .i_oparg = oldindex,
                .i_loc = NO_LOCATION,
                .i_target = NULL,
            };
            if (insert_instruction(entryblock, ncellsused, &make_cell) < 0) {
                return -1;
            }
            ncellsused += 1;
        }
        PyMem_RawFree(sorted);
    }

    if (nfreevars) {
        struct instr copy_frees = {
            .i_opcode = COPY_FREE_VARS,
            .i_oparg = nfreevars,
            .i_loc = NO_LOCATION,
            .i_target = NULL,
        };
        if (insert_instruction(entryblock, 0, &copy_frees) < 0) {
            return -1;
        }

    }

    return 0;
}

/* Make sure that all returns have a line number, even if early passes
 * have failed to propagate a correct line number.
 * The resulting line number may not be correct according to PEP 626,
 * but should be "good enough", and no worse than in older versions. */
static void
guarantee_lineno_for_exits(basicblock *entryblock, int firstlineno) {
    int lineno = firstlineno;
    assert(lineno > 0);
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        struct instr *last = basicblock_last_instr(b);
        if (last == NULL) {
            continue;
        }
        if (last->i_loc.lineno < 0) {
            if (last->i_opcode == RETURN_VALUE) {
                for (int i = 0; i < b->b_iused; i++) {
                    assert(b->b_instr[i].i_loc.lineno < 0);

                    b->b_instr[i].i_loc.lineno = lineno;
                }
            }
        }
        else {
            lineno = last->i_loc.lineno;
        }
    }
}

static int
fix_cell_offsets(struct compiler *c, basicblock *entryblock, int *fixedmap)
{
    int nlocals = (int)PyDict_GET_SIZE(c->u->u_varnames);
    int ncellvars = (int)PyDict_GET_SIZE(c->u->u_cellvars);
    int nfreevars = (int)PyDict_GET_SIZE(c->u->u_freevars);
    int noffsets = ncellvars + nfreevars;

    // First deal with duplicates (arg cells).
    int numdropped = 0;
    for (int i = 0; i < noffsets ; i++) {
        if (fixedmap[i] == i + nlocals) {
            fixedmap[i] -= numdropped;
        }
        else {
            // It was a duplicate (cell/arg).
            numdropped += 1;
        }
    }

    // Then update offsets, either relative to locals or by cell2arg.
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *inst = &b->b_instr[i];
            // This is called before extended args are generated.
            assert(inst->i_opcode != EXTENDED_ARG);
            int oldoffset = inst->i_oparg;
            switch(inst->i_opcode) {
                case MAKE_CELL:
                case LOAD_CLOSURE:
                case LOAD_DEREF:
                case STORE_DEREF:
                case DELETE_DEREF:
                case LOAD_CLASSDEREF:
                    assert(oldoffset >= 0);
                    assert(oldoffset < noffsets);
                    assert(fixedmap[oldoffset] >= 0);
                    inst->i_oparg = fixedmap[oldoffset];
            }
        }
    }

    return numdropped;
}

static void
propagate_line_numbers(basicblock *entryblock);

#ifndef NDEBUG

static bool
no_redundant_nops(cfg_builder *g) {
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        if (remove_redundant_nops(b) != 0) {
            return false;
        }
    }
    return true;
}

static bool
no_redundant_jumps(cfg_builder *g) {
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        struct instr *last = basicblock_last_instr(b);
        if (last != NULL) {
            if (IS_UNCONDITIONAL_JUMP_OPCODE(last->i_opcode)) {
                assert(last->i_target != b->b_next);
                if (last->i_target == b->b_next) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool
opcode_metadata_is_sane(cfg_builder *g) {
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            int opcode = instr->i_opcode;
            assert(opcode <= MAX_REAL_OPCODE);
            int pushed = _PyOpcode_opcode_metadata[opcode].n_pushed;
            int popped = _PyOpcode_opcode_metadata[opcode].n_popped;
            assert((pushed < 0) == (popped < 0));
            if (pushed >= 0) {
                assert(_PyOpcode_opcode_metadata[opcode].valid_entry);
                int effect = stack_effect(opcode, instr->i_oparg, -1);
                if (effect != pushed - popped) {
                   fprintf(stderr,
                           "op=%d: stack_effect (%d) != pushed (%d) - popped (%d)\n",
                           opcode, effect, pushed, popped);
                   return false;
                }
            }
        }
    }
    return true;
}

static bool
no_empty_basic_blocks(cfg_builder *g) {
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        if (b->b_iused == 0) {
            return false;
        }
    }
    return true;
}
#endif

static int
remove_redundant_jumps(cfg_builder *g) {
    /* If a non-empty block ends with a jump instruction, check if the next
     * non-empty block reached through normal flow control is the target
     * of that jump. If it is, then the jump instruction is redundant and
     * can be deleted.
     */
    assert(no_empty_basic_blocks(g));
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        struct instr *last = basicblock_last_instr(b);
        assert(last != NULL);
        assert(!IS_ASSEMBLER_OPCODE(last->i_opcode));
        if (IS_UNCONDITIONAL_JUMP_OPCODE(last->i_opcode)) {
            if (last->i_target == NULL) {
                PyErr_SetString(PyExc_SystemError, "jump with NULL target");
                return -1;
            }
            if (last->i_target == b->b_next) {
                assert(b->b_next->b_iused);
                INSTR_SET_OP0(last, NOP);
            }
        }
    }
    return 0;
}

static int
prepare_localsplus(struct compiler* c, int code_flags)
{
    assert(PyDict_GET_SIZE(c->u->u_varnames) < INT_MAX);
    assert(PyDict_GET_SIZE(c->u->u_cellvars) < INT_MAX);
    assert(PyDict_GET_SIZE(c->u->u_freevars) < INT_MAX);
    int nlocals = (int)PyDict_GET_SIZE(c->u->u_varnames);
    int ncellvars = (int)PyDict_GET_SIZE(c->u->u_cellvars);
    int nfreevars = (int)PyDict_GET_SIZE(c->u->u_freevars);
    assert(INT_MAX - nlocals - ncellvars > 0);
    assert(INT_MAX - nlocals - ncellvars - nfreevars > 0);
    int nlocalsplus = nlocals + ncellvars + nfreevars;
    int* cellfixedoffsets = build_cellfixedoffsets(c);
    if (cellfixedoffsets == NULL) {
        return -1;
    }

    cfg_builder* g = CFG_BUILDER(c);

    // This must be called before fix_cell_offsets().
    if (insert_prefix_instructions(c, g->g_entryblock, cellfixedoffsets, nfreevars, code_flags)) {
        PyMem_Free(cellfixedoffsets);
        return -1;
    }

    int numdropped = fix_cell_offsets(c, g->g_entryblock, cellfixedoffsets);
    PyMem_Free(cellfixedoffsets);  // At this point we're done with it.
    cellfixedoffsets = NULL;
    if (numdropped < 0) {
        return -1;
    }
    nlocalsplus -= numdropped;
    return nlocalsplus;
}

static int
add_return_at_end_of_block(struct compiler *c, int addNone)
{
    /* Make sure every block that falls off the end returns None. */
    if (!basicblock_returns(CFG_BUILDER(c)->g_curblock)) {
        if (addNone) {
            ADDOP_LOAD_CONST(c, NO_LOCATION, Py_None);
        }
        ADDOP(c, NO_LOCATION, RETURN_VALUE);
    }
    return SUCCESS;
}

// This is in codeobject.c.
extern PyObject *_Py_compute_profiletable(PyObject *);

static PyCodeObject *
assemble(struct compiler *c, int addNone)
{
    PyCodeObject *co = NULL;
    PyObject *consts = NULL;
    struct assembler a;
    memset(&a, 0, sizeof(struct assembler));

    int code_flags = compute_code_flags(c);
    if (code_flags < 0) {
        return NULL;
    }

    if (add_return_at_end_of_block(c, addNone) < 0) {
        return NULL;
    }

    int nblocks = 0;
    for (basicblock *b = CFG_BUILDER(c)->g_block_list; b != NULL; b = b->b_list) {
        nblocks++;
    }
    if ((size_t)nblocks > SIZE_MAX / sizeof(basicblock *)) {
        PyErr_NoMemory();
        goto error;
    }

    cfg_builder *g = CFG_BUILDER(c);
    assert(g->g_entryblock != NULL);

    /* Set firstlineno if it wasn't explicitly set. */
    if (!c->u->u_firstlineno) {
        if (g->g_entryblock->b_instr && g->g_entryblock->b_instr->i_loc.lineno) {
            c->u->u_firstlineno = g->g_entryblock->b_instr->i_loc.lineno;
        }
        else {
            c->u->u_firstlineno = 1;
        }
    }

    /** Preprocessing **/
    /* Map labels to targets and mark exception handlers */
    if (translate_jump_labels_to_targets(g->g_entryblock)) {
        goto error;
    }
    if (mark_except_handlers(g->g_entryblock) < 0) {
        goto error;
    }
    if (label_exception_targets(g->g_entryblock)) {
        goto error;
    }

    /** Optimization **/
    consts = consts_dict_keys_inorder(c->u->u_consts);
    if (consts == NULL) {
        goto error;
    }
    if (optimize_cfg(g, consts, c->c_const_cache)) {
        goto error;
    }
    if (add_checks_for_loads_of_uninitialized_variables(g->g_entryblock, c) < 0) {
        goto error;
    }
    if (remove_unused_consts(g->g_entryblock, consts)) {
        goto error;
    }

    /** line numbers (TODO: move this before optimization stage) */
    if (duplicate_exits_without_lineno(g) < 0) {
        goto error;
    }
    propagate_line_numbers(g->g_entryblock);
    guarantee_lineno_for_exits(g->g_entryblock, c->u->u_firstlineno);

    if (push_cold_blocks_to_end(g, code_flags) < 0) {
        goto error;
    }

    /** Assembly **/

    int nlocalsplus = prepare_localsplus(c, code_flags);
    if (nlocalsplus < 0) {
        goto error;
    }

    int maxdepth = stackdepth(g->g_entryblock, code_flags);
    if (maxdepth < 0) {
        goto error;
    }
    /* TO DO -- For 3.12, make sure that `maxdepth <= MAX_ALLOWED_STACK_USE` */

    convert_exception_handlers_to_nops(g->g_entryblock);

    /* Order of basic blocks must have been determined by now */
    if (normalize_jumps(g) < 0) {
        goto error;
    }

    assert(no_redundant_jumps(g));
    assert(opcode_metadata_is_sane(g));

    /* Can't modify the bytecode after computing jump offsets. */
    assemble_jump_offsets(g->g_entryblock);

    /* Create assembler */
    if (assemble_init(&a, c->u->u_firstlineno) < 0) {
        goto error;
    }

    /* Emit code. */
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        for (int j = 0; j < b->b_iused; j++) {
            if (assemble_emit(&a, &b->b_instr[j]) < 0) {
                goto error;
            }
        }
    }

    /* Emit location info */
    a.a_lineno = c->u->u_firstlineno;
    location loc = NO_LOCATION;
    int size = 0;
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        for (int j = 0; j < b->b_iused; j++) {
            if (!same_location(loc, b->b_instr[j].i_loc)) {
                if (assemble_emit_location(&a, loc, size)) {
                    goto error;
                }
                loc = b->b_instr[j].i_loc;
                size = 0;
            }
            size += instr_size(&b->b_instr[j]);
        }
    }
    if (assemble_emit_location(&a, loc, size)) {
        goto error;
    }

    if (assemble_exception_table(&a, g->g_entryblock) < 0) {
        goto error;
    }
    if (_PyBytes_Resize(&a.a_except_table, a.a_except_table_off) < 0) {
        goto error;
    }
    if (merge_const_one(c->c_const_cache, &a.a_except_table) < 0) {
        goto error;
    }

    if (_PyBytes_Resize(&a.a_linetable, a.a_location_off) < 0) {
        goto error;
    }
    if (merge_const_one(c->c_const_cache, &a.a_linetable) < 0) {
        goto error;
    }

    if (_PyBytes_Resize(&a.a_bytecode, a.a_offset * sizeof(_Py_CODEUNIT)) < 0) {
        goto error;
    }
    if (merge_const_one(c->c_const_cache, &a.a_bytecode) < 0) {
        goto error;
    }
    a.a_profiletable = _Py_compute_profiletable(a.a_bytecode);
    if (a.a_profiletable == NULL) {
        goto error;
    }
    co = makecode(c, &a, consts, maxdepth, nlocalsplus, code_flags);
 error:
    Py_XDECREF(consts);
    assemble_free(&a);
    return co;
}

static PyObject*
get_const_value(int opcode, int oparg, PyObject *co_consts)
{
    PyObject *constant = NULL;
    assert(HAS_CONST(opcode));
    if (opcode == LOAD_CONST) {
        constant = PyList_GET_ITEM(co_consts, oparg);
    }

    if (constant == NULL) {
        PyErr_SetString(PyExc_SystemError,
                        "Internal error: failed to get value of a constant");
        return NULL;
    }
    return Py_NewRef(constant);
}

/* Replace LOAD_CONST c1, LOAD_CONST c2 ... LOAD_CONST cn, BUILD_TUPLE n
   with    LOAD_CONST (c1, c2, ... cn).
   The consts table must still be in list form so that the
   new constant (c1, c2, ... cn) can be appended.
   Called with codestr pointing to the first LOAD_CONST.
*/
static int
fold_tuple_on_constants(PyObject *const_cache,
                        struct instr *inst,
                        int n, PyObject *consts)
{
    /* Pre-conditions */
    assert(PyDict_CheckExact(const_cache));
    assert(PyList_CheckExact(consts));
    assert(inst[n].i_opcode == BUILD_TUPLE);
    assert(inst[n].i_oparg == n);

    for (int i = 0; i < n; i++) {
        if (!HAS_CONST(inst[i].i_opcode)) {
            return 0;
        }
    }

    /* Buildup new tuple of constants */
    PyObject *newconst = PyTuple_New(n);
    if (newconst == NULL) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        int op = inst[i].i_opcode;
        int arg = inst[i].i_oparg;
        PyObject *constant = get_const_value(op, arg, consts);
        if (constant == NULL) {
            return -1;
        }
        PyTuple_SET_ITEM(newconst, i, constant);
    }
    if (merge_const_one(const_cache, &newconst) < 0) {
        Py_DECREF(newconst);
        return -1;
    }

    Py_ssize_t index;
    for (index = 0; index < PyList_GET_SIZE(consts); index++) {
        if (PyList_GET_ITEM(consts, index) == newconst) {
            break;
        }
    }
    if (index == PyList_GET_SIZE(consts)) {
        if ((size_t)index >= (size_t)INT_MAX - 1) {
            Py_DECREF(newconst);
            PyErr_SetString(PyExc_OverflowError, "too many constants");
            return -1;
        }
        if (PyList_Append(consts, newconst)) {
            Py_DECREF(newconst);
            return -1;
        }
    }
    Py_DECREF(newconst);
    for (int i = 0; i < n; i++) {
        INSTR_SET_OP0(&inst[i], NOP);
    }
    INSTR_SET_OP1(&inst[n], LOAD_CONST, (int)index);
    return 0;
}

#define VISITED (-1)

// Replace an arbitrary run of SWAPs and NOPs with an optimal one that has the
// same effect.
static int
swaptimize(basicblock *block, int *ix)
{
    // NOTE: "./python -m test test_patma" serves as a good, quick stress test
    // for this function. Make sure to blow away cached *.pyc files first!
    assert(*ix < block->b_iused);
    struct instr *instructions = &block->b_instr[*ix];
    // Find the length of the current sequence of SWAPs and NOPs, and record the
    // maximum depth of the stack manipulations:
    assert(instructions[0].i_opcode == SWAP);
    int depth = instructions[0].i_oparg;
    int len = 0;
    int more = false;
    int limit = block->b_iused - *ix;
    while (++len < limit) {
        int opcode = instructions[len].i_opcode;
        if (opcode == SWAP) {
            depth = Py_MAX(depth, instructions[len].i_oparg);
            more = true;
        }
        else if (opcode != NOP) {
            break;
        }
    }
    // It's already optimal if there's only one SWAP:
    if (!more) {
        return 0;
    }
    // Create an array with elements {0, 1, 2, ..., depth - 1}:
    int *stack = PyMem_Malloc(depth * sizeof(int));
    if (stack == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    for (int i = 0; i < depth; i++) {
        stack[i] = i;
    }
    // Simulate the combined effect of these instructions by "running" them on
    // our "stack":
    for (int i = 0; i < len; i++) {
        if (instructions[i].i_opcode == SWAP) {
            int oparg = instructions[i].i_oparg;
            int top = stack[0];
            // SWAPs are 1-indexed:
            stack[0] = stack[oparg - 1];
            stack[oparg - 1] = top;
        }
    }
    // Now we can begin! Our approach here is based on a solution to a closely
    // related problem (https://cs.stackexchange.com/a/13938). It's easiest to
    // think of this algorithm as determining the steps needed to efficiently
    // "un-shuffle" our stack. By performing the moves in *reverse* order,
    // though, we can efficiently *shuffle* it! For this reason, we will be
    // replacing instructions starting from the *end* of the run. Since the
    // solution is optimal, we don't need to worry about running out of space:
    int current = len - 1;
    for (int i = 0; i < depth; i++) {
        // Skip items that have already been visited, or just happen to be in
        // the correct location:
        if (stack[i] == VISITED || stack[i] == i) {
            continue;
        }
        // Okay, we've found an item that hasn't been visited. It forms a cycle
        // with other items; traversing the cycle and swapping each item with
        // the next will put them all in the correct place. The weird
        // loop-and-a-half is necessary to insert 0 into every cycle, since we
        // can only swap from that position:
        int j = i;
        while (true) {
            // Skip the actual swap if our item is zero, since swapping the top
            // item with itself is pointless:
            if (j) {
                assert(0 <= current);
                // SWAPs are 1-indexed:
                instructions[current].i_opcode = SWAP;
                instructions[current--].i_oparg = j + 1;
            }
            if (stack[j] == VISITED) {
                // Completed the cycle:
                assert(j == i);
                break;
            }
            int next_j = stack[j];
            stack[j] = VISITED;
            j = next_j;
        }
    }
    // NOP out any unused instructions:
    while (0 <= current) {
        INSTR_SET_OP0(&instructions[current--], NOP);
    }
    PyMem_Free(stack);
    *ix += len - 1;
    return 0;
}

// This list is pretty small, since it's only okay to reorder opcodes that:
// - can't affect control flow (like jumping or raising exceptions)
// - can't invoke arbitrary code (besides finalizers)
// - only touch the TOS (and pop it when finished)
#define SWAPPABLE(opcode) \
    ((opcode) == STORE_FAST || (opcode) == POP_TOP)

static int
next_swappable_instruction(basicblock *block, int i, int lineno)
{
    while (++i < block->b_iused) {
        struct instr *instruction = &block->b_instr[i];
        if (0 <= lineno && instruction->i_loc.lineno != lineno) {
            // Optimizing across this instruction could cause user-visible
            // changes in the names bound between line tracing events!
            return -1;
        }
        if (instruction->i_opcode == NOP) {
            continue;
        }
        if (SWAPPABLE(instruction->i_opcode)) {
            return i;
        }
        return -1;
    }
    return -1;
}

// Attempt to apply SWAPs statically by swapping *instructions* rather than
// stack items. For example, we can replace SWAP(2), POP_TOP, STORE_FAST(42)
// with the more efficient NOP, STORE_FAST(42), POP_TOP.
static void
apply_static_swaps(basicblock *block, int i)
{
    // SWAPs are to our left, and potential swaperands are to our right:
    for (; 0 <= i; i--) {
        assert(i < block->b_iused);
        struct instr *swap = &block->b_instr[i];
        if (swap->i_opcode != SWAP) {
            if (swap->i_opcode == NOP || SWAPPABLE(swap->i_opcode)) {
                // Nope, but we know how to handle these. Keep looking:
                continue;
            }
            // We can't reason about what this instruction does. Bail:
            return;
        }
        int j = next_swappable_instruction(block, i, -1);
        if (j < 0) {
            return;
        }
        int k = j;
        int lineno = block->b_instr[j].i_loc.lineno;
        for (int count = swap->i_oparg - 1; 0 < count; count--) {
            k = next_swappable_instruction(block, k, lineno);
            if (k < 0) {
                return;
            }
        }
        // Success!
        INSTR_SET_OP0(swap, NOP);
        struct instr temp = block->b_instr[j];
        block->b_instr[j] = block->b_instr[k];
        block->b_instr[k] = temp;
    }
}

// Attempt to eliminate jumps to jumps by updating inst to jump to
// target->i_target using the provided opcode. Return whether or not the
// optimization was successful.
static bool
jump_thread(struct instr *inst, struct instr *target, int opcode)
{
    assert(is_jump(inst));
    assert(is_jump(target));
    // bpo-45773: If inst->i_target == target->i_target, then nothing actually
    // changes (and we fall into an infinite loop):
    if ((inst->i_loc.lineno == target->i_loc.lineno || target->i_loc.lineno == -1) &&
        inst->i_target != target->i_target)
    {
        inst->i_target = target->i_target;
        inst->i_opcode = opcode;
        return true;
    }
    return false;
}

/* Maximum size of basic block that should be copied in optimizer */
#define MAX_COPY_SIZE 4

/* Optimization */
static int
optimize_basic_block(PyObject *const_cache, basicblock *bb, PyObject *consts)
{
    assert(PyDict_CheckExact(const_cache));
    assert(PyList_CheckExact(consts));
    struct instr nop;
    INSTR_SET_OP0(&nop, NOP);
    struct instr *target;
    for (int i = 0; i < bb->b_iused; i++) {
        struct instr *inst = &bb->b_instr[i];
        int oparg = inst->i_oparg;
        int nextop = i+1 < bb->b_iused ? bb->b_instr[i+1].i_opcode : 0;
        if (HAS_TARGET(inst->i_opcode)) {
            assert(inst->i_target->b_iused > 0);
            target = &inst->i_target->b_instr[0];
            assert(!IS_ASSEMBLER_OPCODE(target->i_opcode));
        }
        else {
            target = &nop;
        }
        assert(!IS_ASSEMBLER_OPCODE(inst->i_opcode));
        switch (inst->i_opcode) {
            /* Remove LOAD_CONST const; conditional jump */
            case LOAD_CONST:
            {
                PyObject* cnt;
                int is_true;
                int jump_if_true;
                switch(nextop) {
                    case POP_JUMP_IF_FALSE:
                    case POP_JUMP_IF_TRUE:
                        cnt = get_const_value(inst->i_opcode, oparg, consts);
                        if (cnt == NULL) {
                            goto error;
                        }
                        is_true = PyObject_IsTrue(cnt);
                        Py_DECREF(cnt);
                        if (is_true == -1) {
                            goto error;
                        }
                        INSTR_SET_OP0(inst, NOP);
                        jump_if_true = nextop == POP_JUMP_IF_TRUE;
                        if (is_true == jump_if_true) {
                            bb->b_instr[i+1].i_opcode = JUMP;
                        }
                        else {
                            INSTR_SET_OP0(&bb->b_instr[i + 1], NOP);
                        }
                        break;
                    case JUMP_IF_FALSE_OR_POP:
                    case JUMP_IF_TRUE_OR_POP:
                        cnt = get_const_value(inst->i_opcode, oparg, consts);
                        if (cnt == NULL) {
                            goto error;
                        }
                        is_true = PyObject_IsTrue(cnt);
                        Py_DECREF(cnt);
                        if (is_true == -1) {
                            goto error;
                        }
                        jump_if_true = nextop == JUMP_IF_TRUE_OR_POP;
                        if (is_true == jump_if_true) {
                            bb->b_instr[i+1].i_opcode = JUMP;
                        }
                        else {
                            INSTR_SET_OP0(inst, NOP);
                            INSTR_SET_OP0(&bb->b_instr[i + 1], NOP);
                        }
                        break;
                    case IS_OP:
                        cnt = get_const_value(inst->i_opcode, oparg, consts);
                        if (cnt == NULL) {
                            goto error;
                        }
                        int jump_op = i+2 < bb->b_iused ? bb->b_instr[i+2].i_opcode : 0;
                        if (Py_IsNone(cnt) && (jump_op == POP_JUMP_IF_FALSE || jump_op == POP_JUMP_IF_TRUE)) {
                            unsigned char nextarg = bb->b_instr[i+1].i_oparg;
                            INSTR_SET_OP0(inst, NOP);
                            INSTR_SET_OP0(&bb->b_instr[i + 1], NOP);
                            bb->b_instr[i+2].i_opcode = nextarg ^ (jump_op == POP_JUMP_IF_FALSE) ?
                                    POP_JUMP_IF_NOT_NONE : POP_JUMP_IF_NONE;
                        }
                        Py_DECREF(cnt);
                        break;
                }
                break;
            }

                /* Try to fold tuples of constants.
                   Skip over BUILD_TUPLE(1) UNPACK_SEQUENCE(1).
                   Replace BUILD_TUPLE(2) UNPACK_SEQUENCE(2) with SWAP(2).
                   Replace BUILD_TUPLE(3) UNPACK_SEQUENCE(3) with SWAP(3). */
            case BUILD_TUPLE:
                if (nextop == UNPACK_SEQUENCE && oparg == bb->b_instr[i+1].i_oparg) {
                    switch(oparg) {
                        case 1:
                            INSTR_SET_OP0(inst, NOP);
                            INSTR_SET_OP0(&bb->b_instr[i + 1], NOP);
                            continue;
                        case 2:
                        case 3:
                            INSTR_SET_OP0(inst, NOP);
                            bb->b_instr[i+1].i_opcode = SWAP;
                            continue;
                    }
                }
                if (i >= oparg) {
                    if (fold_tuple_on_constants(const_cache, inst-oparg, oparg, consts)) {
                        goto error;
                    }
                }
                break;

                /* Simplify conditional jump to conditional jump where the
                   result of the first test implies the success of a similar
                   test or the failure of the opposite test.
                   Arises in code like:
                   "a and b or c"
                   "(a and b) and c"
                   "(a or b) or c"
                   "(a or b) and c"
                   x:JUMP_IF_FALSE_OR_POP y   y:JUMP_IF_FALSE_OR_POP z
                      -->  x:JUMP_IF_FALSE_OR_POP z
                   x:JUMP_IF_FALSE_OR_POP y   y:JUMP_IF_TRUE_OR_POP z
                      -->  x:POP_JUMP_IF_FALSE y+1
                   where y+1 is the instruction following the second test.
                */
            case JUMP_IF_FALSE_OR_POP:
                switch (target->i_opcode) {
                    case POP_JUMP_IF_FALSE:
                        i -= jump_thread(inst, target, POP_JUMP_IF_FALSE);
                        break;
                    case JUMP:
                    case JUMP_IF_FALSE_OR_POP:
                        i -= jump_thread(inst, target, JUMP_IF_FALSE_OR_POP);
                        break;
                    case JUMP_IF_TRUE_OR_POP:
                    case POP_JUMP_IF_TRUE:
                        if (inst->i_loc.lineno == target->i_loc.lineno) {
                            // We don't need to bother checking for loops here,
                            // since a block's b_next cannot point to itself:
                            assert(inst->i_target != inst->i_target->b_next);
                            inst->i_opcode = POP_JUMP_IF_FALSE;
                            inst->i_target = inst->i_target->b_next;
                            --i;
                        }
                        break;
                }
                break;
            case JUMP_IF_TRUE_OR_POP:
                switch (target->i_opcode) {
                    case POP_JUMP_IF_TRUE:
                        i -= jump_thread(inst, target, POP_JUMP_IF_TRUE);
                        break;
                    case JUMP:
                    case JUMP_IF_TRUE_OR_POP:
                        i -= jump_thread(inst, target, JUMP_IF_TRUE_OR_POP);
                        break;
                    case JUMP_IF_FALSE_OR_POP:
                    case POP_JUMP_IF_FALSE:
                        if (inst->i_loc.lineno == target->i_loc.lineno) {
                            // We don't need to bother checking for loops here,
                            // since a block's b_next cannot point to itself:
                            assert(inst->i_target != inst->i_target->b_next);
                            inst->i_opcode = POP_JUMP_IF_TRUE;
                            inst->i_target = inst->i_target->b_next;
                            --i;
                        }
                        break;
                }
                break;
            case POP_JUMP_IF_NOT_NONE:
            case POP_JUMP_IF_NONE:
                switch (target->i_opcode) {
                    case JUMP:
                        i -= jump_thread(inst, target, inst->i_opcode);
                }
                break;
            case POP_JUMP_IF_FALSE:
                switch (target->i_opcode) {
                    case JUMP:
                        i -= jump_thread(inst, target, POP_JUMP_IF_FALSE);
                }
                break;
            case POP_JUMP_IF_TRUE:
                switch (target->i_opcode) {
                    case JUMP:
                        i -= jump_thread(inst, target, POP_JUMP_IF_TRUE);
                }
                break;
            case JUMP:
                switch (target->i_opcode) {
                    case JUMP:
                        i -= jump_thread(inst, target, JUMP);
                }
                break;
            case FOR_ITER:
                if (target->i_opcode == JUMP) {
                    /* This will not work now because the jump (at target) could
                     * be forward or backward and FOR_ITER only jumps forward. We
                     * can re-enable this if ever we implement a backward version
                     * of FOR_ITER.
                     */
                    /*
                    i -= jump_thread(inst, target, FOR_ITER);
                    */
                }
                break;
            case SWAP:
                if (oparg == 1) {
                    INSTR_SET_OP0(inst, NOP);
                    break;
                }
                if (swaptimize(bb, &i)) {
                    goto error;
                }
                apply_static_swaps(bb, i);
                break;
            case KW_NAMES:
                break;
            case PUSH_NULL:
                if (nextop == LOAD_GLOBAL && (inst[1].i_opcode & 1) == 0) {
                    INSTR_SET_OP0(inst, NOP);
                    inst[1].i_oparg |= 1;
                }
                break;
            default:
                /* All HAS_CONST opcodes should be handled with LOAD_CONST */
                assert (!HAS_CONST(inst->i_opcode));
        }
    }
    return 0;
error:
    return -1;
}

/* If this block ends with an unconditional jump to a small exit block, then
 * remove the jump and extend this block with the target.
 * Returns 1 if extended, 0 if no change, and -1 on error.
 */
static int
inline_small_exit_blocks(basicblock *bb) {
    struct instr *last = basicblock_last_instr(bb);
    if (last == NULL) {
        return 0;
    }
    if (!IS_UNCONDITIONAL_JUMP_OPCODE(last->i_opcode)) {
        return 0;
    }
    basicblock *target = last->i_target;
    if (basicblock_exits_scope(target) && target->b_iused <= MAX_COPY_SIZE) {
        INSTR_SET_OP0(last, NOP);
        if (basicblock_append_instructions(bb, target) < 0) {
            return -1;
        }
        return 1;
    }
    return 0;
}

static int
remove_redundant_nops(basicblock *bb) {
    /* Remove NOPs when legal to do so. */
    int dest = 0;
    int prev_lineno = -1;
    for (int src = 0; src < bb->b_iused; src++) {
        int lineno = bb->b_instr[src].i_loc.lineno;
        if (bb->b_instr[src].i_opcode == NOP) {
            /* Eliminate no-op if it doesn't have a line number */
            if (lineno < 0) {
                continue;
            }
            /* or, if the previous instruction had the same line number. */
            if (prev_lineno == lineno) {
                continue;
            }
            /* or, if the next instruction has same line number or no line number */
            if (src < bb->b_iused - 1) {
                int next_lineno = bb->b_instr[src+1].i_loc.lineno;
                if (next_lineno == lineno) {
                    continue;
                }
                if (next_lineno < 0) {
                    bb->b_instr[src+1].i_loc = bb->b_instr[src].i_loc;
                    continue;
                }
            }
            else {
                basicblock* next = bb->b_next;
                while (next && next->b_iused == 0) {
                    next = next->b_next;
                }
                /* or if last instruction in BB and next BB has same line number */
                if (next) {
                    if (lineno == next->b_instr[0].i_loc.lineno) {
                        continue;
                    }
                }
            }

        }
        if (dest != src) {
            bb->b_instr[dest] = bb->b_instr[src];
        }
        dest++;
        prev_lineno = lineno;
    }
    assert(dest <= bb->b_iused);
    int num_removed = bb->b_iused - dest;
    bb->b_iused = dest;
    return num_removed;
}

static int
check_cfg(cfg_builder *g) {
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        /* Raise SystemError if jump or exit is not last instruction in the block. */
        for (int i = 0; i < b->b_iused; i++) {
            int opcode = b->b_instr[i].i_opcode;
            assert(!IS_ASSEMBLER_OPCODE(opcode));
            if (IS_TERMINATOR_OPCODE(opcode)) {
                if (i != b->b_iused - 1) {
                    PyErr_SetString(PyExc_SystemError, "malformed control flow graph.");
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int
mark_reachable(basicblock *entryblock) {
    basicblock **stack = make_cfg_traversal_stack(entryblock);
    if (stack == NULL) {
        return -1;
    }
    basicblock **sp = stack;
    entryblock->b_predecessors = 1;
    *sp++ = entryblock;
    while (sp > stack) {
        basicblock *b = *(--sp);
        b->b_visited = 1;
        if (b->b_next && BB_HAS_FALLTHROUGH(b)) {
            if (!b->b_next->b_visited) {
                assert(b->b_next->b_predecessors == 0);
                *sp++ = b->b_next;
            }
            b->b_next->b_predecessors++;
        }
        for (int i = 0; i < b->b_iused; i++) {
            basicblock *target;
            struct instr *instr = &b->b_instr[i];
            if (is_jump(instr) || is_block_push(instr)) {
                target = instr->i_target;
                if (!target->b_visited) {
                    assert(target->b_predecessors == 0 || target == b->b_next);
                    *sp++ = target;
                }
                target->b_predecessors++;
            }
        }
    }
    PyMem_Free(stack);
    return 0;
}

static void
eliminate_empty_basic_blocks(cfg_builder *g) {
    /* Eliminate empty blocks */
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        basicblock *next = b->b_next;
        while (next && next->b_iused == 0) {
            next = next->b_next;
        }
        b->b_next = next;
    }
    while(g->g_entryblock && g->g_entryblock->b_iused == 0) {
        g->g_entryblock = g->g_entryblock->b_next;
    }
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        assert(b->b_iused > 0);
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            if (HAS_TARGET(instr->i_opcode)) {
                basicblock *target = instr->i_target;
                while (target->b_iused == 0) {
                    target = target->b_next;
                }
                instr->i_target = target;
                assert(instr->i_target && instr->i_target->b_iused > 0);
            }
        }
    }
}


/* If an instruction has no line number, but it's predecessor in the BB does,
 * then copy the line number. If a successor block has no line number, and only
 * one predecessor, then inherit the line number.
 * This ensures that all exit blocks (with one predecessor) receive a line number.
 * Also reduces the size of the line number table,
 * but has no impact on the generated line number events.
 */
static void
propagate_line_numbers(basicblock *entryblock) {
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        struct instr *last = basicblock_last_instr(b);
        if (last == NULL) {
            continue;
        }

        location prev_location = NO_LOCATION;
        for (int i = 0; i < b->b_iused; i++) {
            if (b->b_instr[i].i_loc.lineno < 0) {
                b->b_instr[i].i_loc = prev_location;
            }
            else {
                prev_location = b->b_instr[i].i_loc;
            }
        }
        if (BB_HAS_FALLTHROUGH(b) && b->b_next->b_predecessors == 1) {
            assert(b->b_next->b_iused);
            if (b->b_next->b_instr[0].i_loc.lineno < 0) {
                b->b_next->b_instr[0].i_loc = prev_location;
            }
        }
        if (is_jump(last)) {
            basicblock *target = last->i_target;
            if (target->b_predecessors == 1) {
                if (target->b_instr[0].i_loc.lineno < 0) {
                    target->b_instr[0].i_loc = prev_location;
                }
            }
        }
    }
}


/* Calculate the actual jump target from the target_label */
static int
translate_jump_labels_to_targets(basicblock *entryblock)
{
    int max_label = -1;
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        if (b->b_label > max_label) {
            max_label = b->b_label;
        }
    }
    size_t mapsize = sizeof(basicblock *) * (max_label + 1);
    basicblock **label2block = (basicblock **)PyMem_Malloc(mapsize);
    if (!label2block) {
        PyErr_NoMemory();
        return -1;
    }
    memset(label2block, 0, mapsize);
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        if (b->b_label >= 0) {
            label2block[b->b_label] = b;
        }
    }
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            assert(instr->i_target == NULL);
            if (HAS_TARGET(instr->i_opcode)) {
                int lbl = instr->i_oparg;
                assert(lbl >= 0 && lbl <= max_label);
                instr->i_target = label2block[lbl];
                assert(instr->i_target != NULL);
                assert(instr->i_target->b_label == lbl);
            }
        }
    }
    PyMem_Free(label2block);
    return 0;
}

/* Perform optimizations on a control flow graph.
   The consts object should still be in list form to allow new constants
   to be appended.

   Code trasnformations that reduce code size initially fill the gaps with
   NOPs.  Later those NOPs are removed.
*/

static int
optimize_cfg(cfg_builder *g, PyObject *consts, PyObject *const_cache)
{
    assert(PyDict_CheckExact(const_cache));
    if (check_cfg(g) < 0) {
        return -1;
    }
    eliminate_empty_basic_blocks(g);
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        if (inline_small_exit_blocks(b) < 0) {
            return -1;
        }
    }
    assert(no_empty_basic_blocks(g));
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        if (optimize_basic_block(const_cache, b, consts)) {
            return -1;
        }
        remove_redundant_nops(b);
        assert(b->b_predecessors == 0);
    }
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        if (inline_small_exit_blocks(b) < 0) {
            return -1;
        }
    }
    if (mark_reachable(g->g_entryblock)) {
        return -1;
    }
    /* Delete unreachable instructions */
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
       if (b->b_predecessors == 0) {
            b->b_iused = 0;
       }
    }
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        remove_redundant_nops(b);
    }
    eliminate_empty_basic_blocks(g);
    assert(no_redundant_nops(g));
    if (remove_redundant_jumps(g) < 0) {
        return -1;
    }
    return 0;
}


static int
remove_unused_consts(basicblock *entryblock, PyObject *consts)
{
    assert(PyList_CheckExact(consts));
    Py_ssize_t nconsts = PyList_GET_SIZE(consts);
    if (nconsts == 0) {
        return 0;  /* nothing to do */
    }

    Py_ssize_t *index_map = NULL;
    Py_ssize_t *reverse_index_map = NULL;
    int err = 1;

    index_map = PyMem_Malloc(nconsts * sizeof(Py_ssize_t));
    if (index_map == NULL) {
        goto end;
    }
    for (Py_ssize_t i = 1; i < nconsts; i++) {
        index_map[i] = -1;
    }
    // The first constant may be docstring; keep it always.
    index_map[0] = 0;

    /* mark used consts */
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        for (int i = 0; i < b->b_iused; i++) {
            if (b->b_instr[i].i_opcode == LOAD_CONST ||
                b->b_instr[i].i_opcode == KW_NAMES) {

                int index = b->b_instr[i].i_oparg;
                index_map[index] = index;
            }
        }
    }
    /* now index_map[i] == i if consts[i] is used, -1 otherwise */

    /* condense consts */
    Py_ssize_t n_used_consts = 0;
    for (int i = 0; i < nconsts; i++) {
        if (index_map[i] != -1) {
            assert(index_map[i] == i);
            index_map[n_used_consts++] = index_map[i];
        }
    }
    if (n_used_consts == nconsts) {
        /* nothing to do */
        err = 0;
        goto end;
    }

    /* move all used consts to the beginning of the consts list */
    assert(n_used_consts < nconsts);
    for (Py_ssize_t i = 0; i < n_used_consts; i++) {
        Py_ssize_t old_index = index_map[i];
        assert(i <= old_index && old_index < nconsts);
        if (i != old_index) {
            PyObject *value = PyList_GET_ITEM(consts, index_map[i]);
            assert(value != NULL);
            PyList_SetItem(consts, i, Py_NewRef(value));
        }
    }

    /* truncate the consts list at its new size */
    if (PyList_SetSlice(consts, n_used_consts, nconsts, NULL) < 0) {
        goto end;
    }

    /* adjust const indices in the bytecode */
    reverse_index_map = PyMem_Malloc(nconsts * sizeof(Py_ssize_t));
    if (reverse_index_map == NULL) {
        goto end;
    }
    for (Py_ssize_t i = 0; i < nconsts; i++) {
        reverse_index_map[i] = -1;
    }
    for (Py_ssize_t i = 0; i < n_used_consts; i++) {
        assert(index_map[i] != -1);
        assert(reverse_index_map[index_map[i]] == -1);
        reverse_index_map[index_map[i]] = i;
    }

    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        for (int i = 0; i < b->b_iused; i++) {
            if (b->b_instr[i].i_opcode == LOAD_CONST ||
                b->b_instr[i].i_opcode == KW_NAMES) {

                int index = b->b_instr[i].i_oparg;
                assert(reverse_index_map[index] >= 0);
                assert(reverse_index_map[index] < n_used_consts);
                b->b_instr[i].i_oparg = (int)reverse_index_map[index];
            }
        }
    }

    err = 0;
end:
    PyMem_Free(index_map);
    PyMem_Free(reverse_index_map);
    return err;
}

static inline bool
is_exit_without_lineno(basicblock *b) {
    if (!basicblock_exits_scope(b)) {
        return false;
    }
    for (int i = 0; i < b->b_iused; i++) {
        if (b->b_instr[i].i_loc.lineno >= 0) {
            return false;
        }
    }
    return true;
}

/* PEP 626 mandates that the f_lineno of a frame is correct
 * after a frame terminates. It would be prohibitively expensive
 * to continuously update the f_lineno field at runtime,
 * so we make sure that all exiting instruction (raises and returns)
 * have a valid line number, allowing us to compute f_lineno lazily.
 * We can do this by duplicating the exit blocks without line number
 * so that none have more than one predecessor. We can then safely
 * copy the line number from the sole predecessor block.
 */
static int
duplicate_exits_without_lineno(cfg_builder *g)
{
    assert(no_empty_basic_blocks(g));
    /* Copy all exit blocks without line number that are targets of a jump.
     */
    basicblock *entryblock = g->g_entryblock;
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        struct instr *last = basicblock_last_instr(b);
        assert(last != NULL);
        if (is_jump(last)) {
            basicblock *target = last->i_target;
            if (is_exit_without_lineno(target) && target->b_predecessors > 1) {
                basicblock *new_target = copy_basicblock(g, target);
                if (new_target == NULL) {
                    return -1;
                }
                new_target->b_instr[0].i_loc = last->i_loc;
                last->i_target = new_target;
                target->b_predecessors--;
                new_target->b_predecessors = 1;
                new_target->b_next = target->b_next;
                target->b_next = new_target;
            }
        }
    }

    /* Any remaining reachable exit blocks without line number can only be reached by
     * fall through, and thus can only have a single predecessor */
    for (basicblock *b = entryblock; b != NULL; b = b->b_next) {
        if (BB_HAS_FALLTHROUGH(b) && b->b_next && b->b_iused > 0) {
            if (is_exit_without_lineno(b->b_next)) {
                struct instr *last = basicblock_last_instr(b);
                assert(last != NULL);
                b->b_next->b_instr[0].i_loc = last->i_loc;
            }
        }
    }
    return 0;
}


/* Access to compiler optimizations for unit tests.
 *
 * _PyCompile_CodeGen takes and AST, applies code-gen and
 * returns the unoptimized CFG as an instruction list.
 *
 * _PyCompile_OptimizeCfg takes an instruction list, constructs
 * a CFG, optimizes it and converts back to an instruction list.
 *
 * An instruction list is a PyList where each item is either
 * a tuple describing a single instruction:
 * (opcode, oparg, lineno, end_lineno, col, end_col), or
 * a jump target label marking the beginning of a basic block.
 */

static int
instructions_to_cfg(PyObject *instructions, cfg_builder *g)
{
    assert(PyList_Check(instructions));

    Py_ssize_t instr_size = PyList_GET_SIZE(instructions);
    for (Py_ssize_t i = 0; i < instr_size; i++) {
        PyObject *item = PyList_GET_ITEM(instructions, i);
        if (PyLong_Check(item)) {
            int lbl_id = PyLong_AsLong(item);
            if (PyErr_Occurred()) {
                return -1;
            }
            if (lbl_id <= 0 || lbl_id > instr_size) {
                /* expect label in a reasonable range */
                PyErr_SetString(PyExc_ValueError, "label out of range");
                return -1;
            }
            jump_target_label lbl = {lbl_id};
            if (cfg_builder_use_label(g, lbl) < 0) {
                return -1;
            }
        }
        else {
            if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 6) {
                PyErr_SetString(PyExc_ValueError, "expected a 6-tuple");
                return -1;
            }
            int opcode = PyLong_AsLong(PyTuple_GET_ITEM(item, 0));
            if (PyErr_Occurred()) {
                return -1;
            }
            int oparg = PyLong_AsLong(PyTuple_GET_ITEM(item, 1));
            if (PyErr_Occurred()) {
                return -1;
            }
            location loc;
            loc.lineno = PyLong_AsLong(PyTuple_GET_ITEM(item, 2));
            if (PyErr_Occurred()) {
                return -1;
            }
            loc.end_lineno = PyLong_AsLong(PyTuple_GET_ITEM(item, 3));
            if (PyErr_Occurred()) {
                return -1;
            }
            loc.col_offset = PyLong_AsLong(PyTuple_GET_ITEM(item, 4));
            if (PyErr_Occurred()) {
                return -1;
            }
            loc.end_col_offset = PyLong_AsLong(PyTuple_GET_ITEM(item, 5));
            if (PyErr_Occurred()) {
                return -1;
            }
            if (cfg_builder_addop(g, opcode, oparg, loc) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

static PyObject *
cfg_to_instructions(cfg_builder *g)
{
    PyObject *instructions = PyList_New(0);
    if (instructions == NULL) {
        return NULL;
    }
    int lbl = 1;
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        b->b_label = lbl++;
    }
    for (basicblock *b = g->g_entryblock; b != NULL; b = b->b_next) {
        PyObject *lbl = PyLong_FromLong(b->b_label);
        if (lbl == NULL) {
            goto error;
        }
        if (PyList_Append(instructions, lbl) != 0) {
            Py_DECREF(lbl);
            goto error;
        }
        Py_DECREF(lbl);
        for (int i = 0; i < b->b_iused; i++) {
            struct instr *instr = &b->b_instr[i];
            location loc = instr->i_loc;
            int arg = HAS_TARGET(instr->i_opcode) ?
                      instr->i_target->b_label : instr->i_oparg;

            PyObject *inst_tuple = Py_BuildValue(
                "(iiiiii)", instr->i_opcode, arg,
                loc.lineno, loc.end_lineno,
                loc.col_offset, loc.end_col_offset);
            if (inst_tuple == NULL) {
                goto error;
            }

            if (PyList_Append(instructions, inst_tuple) != 0) {
                Py_DECREF(inst_tuple);
                goto error;
            }
            Py_DECREF(inst_tuple);
        }
    }

    return instructions;
error:
    Py_DECREF(instructions);
    return NULL;
}

PyObject *
_PyCompile_CodeGen(PyObject *ast, PyObject *filename, PyCompilerFlags *pflags,
                   int optimize)
{
    PyObject *res = NULL;

    if (!PyAST_Check(ast)) {
        PyErr_SetString(PyExc_TypeError, "expected an AST");
        return NULL;
    }

    PyArena *arena = _PyArena_New();
    if (arena == NULL) {
        return NULL;
    }

    mod_ty mod = PyAST_obj2mod(ast, arena, 0 /* exec */);
    if (mod == NULL || !_PyAST_Validate(mod)) {
        _PyArena_Free(arena);
        return NULL;
    }

    struct compiler *c = new_compiler(mod, filename, pflags, optimize, arena);
    if (c == NULL) {
        _PyArena_Free(arena);
        return NULL;
    }

    if (compiler_codegen(c, mod) < 0) {
        goto finally;
    }

    cfg_builder *g = CFG_BUILDER(c);

    if (translate_jump_labels_to_targets(g->g_entryblock) < 0) {
        goto finally;
    }

    res = cfg_to_instructions(g);

finally:
    compiler_exit_scope(c);
    compiler_free(c);
    _PyArena_Free(arena);
    return res;
}

PyObject *
_PyCompile_OptimizeCfg(PyObject *instructions, PyObject *consts)
{
    PyObject *res = NULL;
    PyObject *const_cache = NULL;
    cfg_builder g;
    memset(&g, 0, sizeof(cfg_builder));
    if (cfg_builder_init(&g) < 0) {
        goto error;
    }
    if (instructions_to_cfg(instructions, &g) < 0) {
        goto error;
    }
    const_cache = PyDict_New();
    if (const_cache == NULL) {
        goto error;
    }
    if (translate_jump_labels_to_targets(g.g_entryblock)) {
        goto error;
    }
    if (optimize_cfg(&g, consts, const_cache) < 0) {
        goto error;
    }
    res = cfg_to_instructions(&g);
error:
    Py_XDECREF(const_cache);
    cfg_builder_fini(&g);
    return res;
}


/* Retained for API compatibility.
 * Optimization is now done in optimize_cfg */

PyObject *
PyCode_Optimize(PyObject *code, PyObject* Py_UNUSED(consts),
                PyObject *Py_UNUSED(names), PyObject *Py_UNUSED(lnotab_obj))
{
    return Py_NewRef(code);
}
