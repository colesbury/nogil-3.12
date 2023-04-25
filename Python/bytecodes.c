// This file contains instruction definitions.
// It is read by Tools/cases_generator/generate_cases.py
// to generate Python/generated_cases.c.h.
// Note that there is some dummy C code at the top and bottom of the file
// to fool text editors like VS Code into believing this is valid C code.
// The actual instruction definitions start at // BEGIN BYTECODES //.
// See Tools/cases_generator/README.md for more information.

#include "Python.h"
#include "pycore_abstract.h"      // _PyIndex_Check()
#include "pycore_call.h"          // _PyObject_FastCallDictTstate()
#include "pycore_ceval.h"         // _PyEval_SignalAsyncExc()
#include "pycore_code.h"
#include "pycore_critical_section.h"
#include "pycore_function.h"
#include "pycore_intrinsics.h"
#include "pycore_long.h"          // _PyLong_GetZero()
#include "pycore_object.h"        // _PyObject_GC_TRACK()
#include "pycore_moduleobject.h"  // PyModuleObject
#include "pycore_opcode.h"        // EXTRA_CASES
#include "pycore_pyerrors.h"      // _PyErr_Fetch()
#include "pycore_pymem.h"         // _PyMem_IsPtrFreed()
#include "pycore_pystate.h"       // _PyInterpreterState_GET()
#include "pycore_range.h"         // _PyRangeIterObject
#include "pycore_sliceobject.h"   // _PyBuildSlice_ConsumeRefs
#include "pycore_sysmodule.h"     // _PySys_Audit()
#include "pycore_tuple.h"         // _PyTuple_ITEMS()
#include "pycore_emscripten_signal.h"  // _Py_CHECK_EMSCRIPTEN_SIGNALS

#include "pycore_dict.h"
#include "dictobject.h"
#include "pycore_frame.h"
#include "opcode.h"
#include "pydtrace.h"
#include "setobject.h"
#include "structmember.h"         // struct PyMemberDef, T_OFFSET_EX

void _PyFloat_ExactDealloc(PyObject *);
void _PyUnicode_ExactDealloc(PyObject *);

/* Stack effect macros
 * These will be mostly replaced by stack effect descriptions,
 * but the tooling need to recognize them.
 */
#define SET_TOP(v)        (stack_pointer[-1] = (v))
#define SET_SECOND(v)     (stack_pointer[-2] = (v))
#define PEEK(n)           (stack_pointer[-(n)])
#define POKE(n, v)        (stack_pointer[-(n)] = (v))
#define PUSH(val)         (*(stack_pointer++) = (val))
#define POP()             (*(--stack_pointer))
#define TOP()             PEEK(1)
#define SECOND()          PEEK(2)
#define STACK_GROW(n)     (stack_pointer += (n))
#define STACK_SHRINK(n)   (stack_pointer -= (n))
#define EMPTY()           1
#define STACK_LEVEL()     2

/* Local variable macros */
#define GETLOCAL(i)     (frame->localsplus[i])
#define SETLOCAL(i, val)  \
do { \
    PyObject *_tmp = frame->localsplus[i]; \
    frame->localsplus[i] = (val); \
    Py_XDECREF(_tmp); \
} while (0)

/* Flow control macros */
#define DEOPT_IF(cond, instname) ((void)0)
#define DEOPT_UNLOCK_IF(cond, instname) ((void)0)
#define ERROR_IF(cond, labelname) ((void)0)
#define JUMPBY(offset) ((void)0)
#define GO_TO_INSTRUCTION(instname) ((void)0)
#define DISPATCH_SAME_OPARG() ((void)0)

#define inst(name, ...) case name:
#define op(name, ...) /* NAME is ignored */
#define macro(name) static int MACRO_##name
#define super(name) static int SUPER_##name
#define family(name, ...) static int family_##name

#define NAME_ERROR_MSG \
    "name '%.200s' is not defined"

// Dummy variables for stack effects.
static PyObject *value, *value1, *value2, *left, *right, *res, *sum, *prod, *sub;
static PyObject *container, *start, *stop, *v, *lhs, *rhs;
static PyObject *list, *tuple, *dict, *owner;
static PyObject *exit_func, *lasti, *val, *retval, *obj, *iter;
static PyObject *aiter, *awaitable, *iterable, *w, *exc_value, *bc;
static PyObject *orig, *excs, *update, *b, *fromlist, *level, *from;
static size_t jump;
// Dummy variables for cache effects
static uint16_t when_to_jump_mask, invert, counter, index, hint;
static uint32_t type_version;
// Dummy opcode names for 'op' opcodes
#define _COMPARE_OP_FLOAT 1003
#define _COMPARE_OP_INT 1004
#define _COMPARE_OP_STR 1005
#define _JUMP_IF 1006

static PyObject *
dummy_func(
    PyThreadState *tstate,
    _PyInterpreterFrame *frame,
    unsigned char opcode,
    unsigned int oparg,
    _Py_atomic_int * const eval_breaker,
    _PyCFrame cframe,
    PyObject *names,
    PyObject *consts,
    _Py_CODEUNIT *next_instr,
    PyObject **stack_pointer,
    PyObject *kwnames,
    int throwflag,
    binaryfunc binary_ops[]
)
{
    _PyInterpreterFrame  entry_frame;

    switch (opcode) {

// BEGIN BYTECODES //
        inst(NOP, (--)) {
        }

        inst(RESUME, (--)) {
            assert(tstate->cframe == &cframe);
            assert(frame == cframe.current_frame);
            if (oparg < 2) {
                CHECK_EVAL_BREAKER();
            }
        }

        inst(LOAD_CLOSURE, (-- value)) {
            /* We keep LOAD_CLOSURE so that the bytecode stays more readable. */
            value = GETLOCAL(oparg);
            ERROR_IF(value == NULL, unbound_local_error);
            Py_INCREF(value);
        }

        inst(LOAD_FAST_CHECK, (-- value)) {
            value = GETLOCAL(oparg);
            ERROR_IF(value == NULL, unbound_local_error);
            Py_INCREF(value);
        }

        inst(LOAD_FAST, (-- value)) {
            value = GETLOCAL(oparg);
            assert(value != NULL);
            Py_INCREF(value);
        }

        inst(LOAD_CONST, (-- value)) {
            value = GETITEM(consts, oparg);
            Py_INCREF(value);
        }

        inst(STORE_FAST, (value --)) {
            SETLOCAL(oparg, value);
        }

        super(LOAD_FAST__LOAD_FAST) = LOAD_FAST + LOAD_FAST;
        super(LOAD_FAST__LOAD_CONST) = LOAD_FAST + LOAD_CONST;
        super(STORE_FAST__LOAD_FAST)  = STORE_FAST + LOAD_FAST;
        super(STORE_FAST__STORE_FAST) = STORE_FAST + STORE_FAST;
        super(LOAD_CONST__LOAD_FAST) = LOAD_CONST + LOAD_FAST;

        inst(POP_TOP, (value --)) {
            DECREF_INPUTS();
        }

        inst(PUSH_NULL, (-- res)) {
            res = NULL;
        }

        macro(END_FOR) = POP_TOP + POP_TOP;

        inst(UNARY_NEGATIVE, (value -- res)) {
            res = PyNumber_Negative(value);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(UNARY_NOT, (value -- res)) {
            int err = PyObject_IsTrue(value);
            DECREF_INPUTS();
            ERROR_IF(err < 0, error);
            if (err == 0) {
                res = Py_True;
            }
            else {
                res = Py_False;
            }
            Py_INCREF(res);
        }

        inst(UNARY_INVERT, (value -- res)) {
            res = PyNumber_Invert(value);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        family(binary_op, INLINE_CACHE_ENTRIES_BINARY_OP) = {
            BINARY_OP,
            BINARY_OP_GENERIC,
            BINARY_OP_ADD_FLOAT,
            BINARY_OP_ADD_INT,
            BINARY_OP_ADD_UNICODE,
            // BINARY_OP_INPLACE_ADD_UNICODE,  // This is an odd duck.
            BINARY_OP_MULTIPLY_FLOAT,
            BINARY_OP_MULTIPLY_INT,
            BINARY_OP_SUBTRACT_FLOAT,
            BINARY_OP_SUBTRACT_INT,
        };


        inst(BINARY_OP_MULTIPLY_INT, (unused/1, left, right -- prod)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyLong_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyLong_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            prod = _PyLong_Multiply((PyLongObject *)left, (PyLongObject *)right);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            ERROR_IF(prod == NULL, error);
        }

        inst(BINARY_OP_MULTIPLY_FLOAT, (unused/1, left, right -- prod)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyFloat_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyFloat_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            double dprod = ((PyFloatObject *)left)->ob_fval *
                ((PyFloatObject *)right)->ob_fval;
            prod = PyFloat_FromDouble(dprod);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            ERROR_IF(prod == NULL, error);
        }

        inst(BINARY_OP_SUBTRACT_INT, (unused/1, left, right -- sub)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyLong_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyLong_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            sub = _PyLong_Subtract((PyLongObject *)left, (PyLongObject *)right);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            ERROR_IF(sub == NULL, error);
        }

        inst(BINARY_OP_SUBTRACT_FLOAT, (unused/1, left, right -- sub)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyFloat_CheckExact(left), BINARY_OP);
            DEOPT_IF(!PyFloat_CheckExact(right), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            double dsub = ((PyFloatObject *)left)->ob_fval - ((PyFloatObject *)right)->ob_fval;
            sub = PyFloat_FromDouble(dsub);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            ERROR_IF(sub == NULL, error);
        }

        inst(BINARY_OP_ADD_UNICODE, (unused/1, left, right -- res)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyUnicode_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            res = PyUnicode_Concat(left, right);
            _Py_DECREF_SPECIALIZED(left, _PyUnicode_ExactDealloc);
            _Py_DECREF_SPECIALIZED(right, _PyUnicode_ExactDealloc);
            ERROR_IF(res == NULL, error);
        }

        // This is a subtle one. It's a super-instruction for
        // BINARY_OP_ADD_UNICODE followed by STORE_FAST
        // where the store goes into the left argument.
        // So the inputs are the same as for all BINARY_OP
        // specializations, but there is no output.
        // At the end we just skip over the STORE_FAST.
        inst(BINARY_OP_INPLACE_ADD_UNICODE, (left, right --)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyUnicode_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            _Py_CODEUNIT true_next = next_instr[INLINE_CACHE_ENTRIES_BINARY_OP];
            assert(_Py_OPCODE(true_next) == STORE_FAST ||
                   _Py_OPCODE(true_next) == STORE_FAST__LOAD_FAST);
            PyObject **target_local = &GETLOCAL(_Py_OPARG(true_next));
            DEOPT_IF(*target_local != left, BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            /* Handle `left = left + right` or `left += right` for str.
             *
             * When possible, extend `left` in place rather than
             * allocating a new PyUnicodeObject. This attempts to avoid
             * quadratic behavior when one neglects to use str.join().
             *
             * If `left` has only two references remaining (one from
             * the stack, one in the locals), DECREFing `left` leaves
             * only the locals reference, so PyUnicode_Append knows
             * that the string is safe to mutate.
             */
            assert(Py_REFCNT(left) >= 2);
            _Py_DECREF_NO_DEALLOC(left);
            PyUnicode_Append(target_local, right);
            _Py_DECREF_SPECIALIZED(right, _PyUnicode_ExactDealloc);
            ERROR_IF(*target_local == NULL, error);
            // The STORE_FAST is already done.
            JUMPBY(INLINE_CACHE_ENTRIES_BINARY_OP + 1);
        }

        inst(BINARY_OP_ADD_FLOAT, (unused/1, left, right -- sum)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyFloat_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            double dsum = ((PyFloatObject *)left)->ob_fval +
                ((PyFloatObject *)right)->ob_fval;
            sum = PyFloat_FromDouble(dsum);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            ERROR_IF(sum == NULL, error);
        }

        inst(BINARY_OP_ADD_INT, (unused/1, left, right -- sum)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyLong_CheckExact(left), BINARY_OP);
            DEOPT_IF(Py_TYPE(right) != Py_TYPE(left), BINARY_OP);
            STAT_INC(BINARY_OP, hit);
            sum = _PyLong_Add((PyLongObject *)left, (PyLongObject *)right);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            ERROR_IF(sum == NULL, error);
        }

        family(binary_subscr, INLINE_CACHE_ENTRIES_BINARY_SUBSCR) = {
            BINARY_SUBSCR,
            BINARY_SUBSCR_GENERIC,
            BINARY_SUBSCR_DICT,
            BINARY_SUBSCR_GETITEM,
            BINARY_SUBSCR_LIST_INT,
            BINARY_SUBSCR_TUPLE_INT,
        };

        inst(BINARY_SUBSCR, (unused/4, container, sub -- unused)) {
            _PyBinarySubscrCache *cache = (_PyBinarySubscrCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                next_instr--;
                _Py_Specialize_BinarySubscr(container, sub, next_instr);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(BINARY_SUBSCR, deferred);
            GO_TO_INSTRUCTION(BINARY_SUBSCR_GENERIC);
        }

        inst(BINARY_SUBSCR_GENERIC, (unused/4, container, sub -- res)) {
            res = PyObject_GetItem(container, sub);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(BINARY_SLICE, (container, start, stop -- res)) {
            PyObject *slice = _PyBuildSlice_ConsumeRefs(start, stop);
            // Can't use ERROR_IF() here, because we haven't
            // DECREF'ed container yet, and we still own slice.
            if (slice == NULL) {
                res = NULL;
            }
            else {
                res = PyObject_GetItem(container, slice);
                Py_DECREF(slice);
            }
            Py_DECREF(container);
            ERROR_IF(res == NULL, error);
        }

        inst(STORE_SLICE, (v, container, start, stop -- )) {
            PyObject *slice = _PyBuildSlice_ConsumeRefs(start, stop);
            int err;
            if (slice == NULL) {
                err = 1;
            }
            else {
                err = PyObject_SetItem(container, slice, v);
                Py_DECREF(slice);
            }
            Py_DECREF(v);
            Py_DECREF(container);
            ERROR_IF(err, error);
        }

        inst(BINARY_SUBSCR_LIST_INT, (unused/4, list, sub -- res)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyLong_CheckExact(sub), BINARY_SUBSCR);
            DEOPT_IF(!PyList_CheckExact(list), BINARY_SUBSCR);

            // Deopt unless 0 <= sub < PyList_Size(list)
            DEOPT_IF(!_PyLong_IsPositiveSingleDigit(sub), BINARY_SUBSCR);
            assert(((PyLongObject *)_PyLong_GetZero())->ob_digit[0] == 0);
            Py_ssize_t index = ((PyLongObject*)sub)->ob_digit[0];
            DEOPT_IF(index >= PyList_GET_SIZE(list), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            res = PyList_GET_ITEM(list, index);
            assert(res != NULL);
            Py_INCREF(res);
            _Py_DECREF_SPECIALIZED(sub, (destructor)PyObject_Free);
            Py_DECREF(list);
        }

        inst(BINARY_SUBSCR_TUPLE_INT, (unused/4, tuple, sub -- res)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyLong_CheckExact(sub), BINARY_SUBSCR);
            DEOPT_IF(!PyTuple_CheckExact(tuple), BINARY_SUBSCR);

            // Deopt unless 0 <= sub < PyTuple_Size(list)
            DEOPT_IF(!_PyLong_IsPositiveSingleDigit(sub), BINARY_SUBSCR);
            assert(((PyLongObject *)_PyLong_GetZero())->ob_digit[0] == 0);
            Py_ssize_t index = ((PyLongObject*)sub)->ob_digit[0];
            DEOPT_IF(index >= PyTuple_GET_SIZE(tuple), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            res = PyTuple_GET_ITEM(tuple, index);
            assert(res != NULL);
            Py_INCREF(res);
            _Py_DECREF_SPECIALIZED(sub, (destructor)PyObject_Free);
            Py_DECREF(tuple);
        }

        inst(BINARY_SUBSCR_DICT, (unused/4, dict, sub -- res)) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyDict_CheckExact(dict), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            res = PyDict_FetchItemWithError(dict, sub);
            if (res == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_SetKeyError(sub);
                }
                Py_DECREF(dict);
                Py_DECREF(sub);
                ERROR_IF(true, error);
            }
            DECREF_INPUTS();
        }

        inst(BINARY_SUBSCR_GETITEM, (unused/1, type_version/2, func_version/1, container, sub -- unused)) {
            PyTypeObject *tp = Py_TYPE(container);
            DEOPT_IF(tp->tp_version_tag != type_version, BINARY_SUBSCR);
            assert(tp->tp_flags & Py_TPFLAGS_HEAPTYPE);
            PyObject *cached = ((PyHeapTypeObject *)tp)->_spec_cache.getitem;
            assert(PyFunction_Check(cached));
            PyFunctionObject *getitem = (PyFunctionObject *)cached;
            DEOPT_IF(getitem->func_version != func_version, BINARY_SUBSCR);
            PyCodeObject *code = (PyCodeObject *)getitem->func_code;
            assert(code->co_argcount == 2);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), BINARY_SUBSCR);
            STAT_INC(BINARY_SUBSCR, hit);
            Py_INCREF(getitem);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, getitem, 2);
            STACK_SHRINK(2);
            new_frame->localsplus[0] = container;
            new_frame->localsplus[1] = sub;
            JUMPBY(INLINE_CACHE_ENTRIES_BINARY_SUBSCR);
            DISPATCH_INLINED(new_frame);
        }

        // Alternative: (list, unused[oparg], v -- list, unused[oparg])
        inst(LIST_APPEND, (v --)) {
            PyObject *list = PEEK(oparg + 1);  // +1 to account for v staying on stack
            ERROR_IF(_PyList_AppendTakeRef((PyListObject *)list, v) < 0, error);
            PREDICT(JUMP_BACKWARD);
        }

        // Alternative: (set, unused[oparg], v -- set, unused[oparg])
        inst(SET_ADD, (v --)) {
            PyObject *set = PEEK(oparg + 1);  // +1 to account for v staying on stack
            int err = PySet_Add(set, v);
            Py_DECREF(v);
            ERROR_IF(err, error);
            PREDICT(JUMP_BACKWARD);
        }

        family(store_subscr) = {
            STORE_SUBSCR,
            STORE_SUBSCR_GENERIC,
            STORE_SUBSCR_DICT,
            STORE_SUBSCR_LIST_INT,
        };

        inst(STORE_SUBSCR, (unused/1, unused, container, sub -- )) {
            _PyStoreSubscrCache *cache = (_PyStoreSubscrCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                next_instr--;
                _Py_Specialize_StoreSubscr(container, sub, next_instr);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(STORE_SUBSCR, deferred);
            GO_TO_INSTRUCTION(STORE_SUBSCR_GENERIC);
        }

        inst(STORE_SUBSCR_GENERIC, (unused/1, v, container, sub -- )) {
            /* container[sub] = v */
            int err = PyObject_SetItem(container, sub, v);
            DECREF_INPUTS();
            ERROR_IF(err, error);
        }

        inst(STORE_SUBSCR_LIST_INT, (unused/1, value, list, sub -- )) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyLong_CheckExact(sub), STORE_SUBSCR);
            DEOPT_IF(!PyList_CheckExact(list), STORE_SUBSCR);

            // Ensure nonnegative, zero-or-one-digit ints.
            DEOPT_IF(!_PyLong_IsPositiveSingleDigit(sub), STORE_SUBSCR);
            Py_ssize_t index = ((PyLongObject*)sub)->ob_digit[0];
            // Ensure index < len(list)
            DEOPT_IF(index >= PyList_GET_SIZE(list), STORE_SUBSCR);
            STAT_INC(STORE_SUBSCR, hit);

            PyObject *old_value = PyList_GET_ITEM(list, index);
            PyList_SET_ITEM(list, index, value);
            assert(old_value != NULL);
            Py_DECREF(old_value);
            _Py_DECREF_SPECIALIZED(sub, (destructor)PyObject_Free);
            Py_DECREF(list);
        }

        inst(STORE_SUBSCR_DICT, (unused/1, value, dict, sub -- )) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyDict_CheckExact(dict), STORE_SUBSCR);
            STAT_INC(STORE_SUBSCR, hit);
            int err = _PyDict_SetItem_Take2((PyDictObject *)dict, sub, value);
            Py_DECREF(dict);
            ERROR_IF(err, error);
        }

        inst(DELETE_SUBSCR, (container, sub --)) {
            /* del container[sub] */
            int err = PyObject_DelItem(container, sub);
            DECREF_INPUTS();
            ERROR_IF(err, error);
        }

        inst(CALL_INTRINSIC_1, (value -- res)) {
            assert(oparg <= MAX_INTRINSIC_1);
            res = _PyIntrinsics_UnaryFunctions[oparg](tstate, value);
            Py_DECREF(value);
            ERROR_IF(res == NULL, error);
        }

        // stack effect: (__array[oparg] -- )
        inst(RAISE_VARARGS) {
            PyObject *cause = NULL, *exc = NULL;
            switch (oparg) {
            case 2:
                cause = POP(); /* cause */
                /* fall through */
            case 1:
                exc = POP(); /* exc */
                /* fall through */
            case 0:
                if (do_raise(tstate, exc, cause)) {
                    goto exception_unwind;
                }
                break;
            default:
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "bad RAISE_VARARGS oparg");
                break;
            }
            goto error;
        }

        inst(INTERPRETER_EXIT, (retval --)) {
            assert(frame == &entry_frame);
            assert(_PyFrame_IsIncomplete(frame));
            STACK_SHRINK(1);  // Since we're not going to DISPATCH()
            assert(EMPTY());
            /* Restore previous cframe and return. */
            tstate->cframe = cframe.previous;
            tstate->cframe->use_tracing = cframe.use_tracing;
            assert(tstate->cframe->current_frame == frame->previous);
            assert(!_PyErr_Occurred(tstate));
            _Py_LeaveRecursiveCallTstate(tstate);
            return retval;
        }

        inst(RETURN_VALUE, (retval --)) {
            STACK_SHRINK(1);
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            TRACE_FUNCTION_EXIT();
            DTRACE_FUNCTION_EXIT();
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            // GH-99729: We need to unlink the frame *before* clearing it:
            _PyInterpreterFrame *dying = frame;
            frame = cframe.current_frame = dying->previous;
            _PyEvalFrameClearAndPop(tstate, dying);
            _PyFrame_StackPush(frame, retval);
            goto resume_frame;
        }

        inst(GET_AITER, (obj -- iter)) {
            unaryfunc getter = NULL;
            PyTypeObject *type = Py_TYPE(obj);

            if (type->tp_as_async != NULL) {
                getter = type->tp_as_async->am_aiter;
            }

            if (getter == NULL) {
                _PyErr_Format(tstate, PyExc_TypeError,
                              "'async for' requires an object with "
                              "__aiter__ method, got %.100s",
                              type->tp_name);
                DECREF_INPUTS();
                ERROR_IF(true, error);
            }

            iter = (*getter)(obj);
            DECREF_INPUTS();
            ERROR_IF(iter == NULL, error);

            if (Py_TYPE(iter)->tp_as_async == NULL ||
                    Py_TYPE(iter)->tp_as_async->am_anext == NULL) {

                _PyErr_Format(tstate, PyExc_TypeError,
                              "'async for' received an object from __aiter__ "
                              "that does not implement __anext__: %.100s",
                              Py_TYPE(iter)->tp_name);
                Py_DECREF(iter);
                ERROR_IF(true, error);
            }
        }

        inst(GET_ANEXT, (aiter -- aiter, awaitable)) {
            unaryfunc getter = NULL;
            PyObject *next_iter = NULL;
            PyTypeObject *type = Py_TYPE(aiter);

            if (PyAsyncGen_CheckExact(aiter)) {
                awaitable = type->tp_as_async->am_anext(aiter);
                if (awaitable == NULL) {
                    goto error;
                }
            } else {
                if (type->tp_as_async != NULL){
                    getter = type->tp_as_async->am_anext;
                }

                if (getter != NULL) {
                    next_iter = (*getter)(aiter);
                    if (next_iter == NULL) {
                        goto error;
                    }
                }
                else {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'async for' requires an iterator with "
                                  "__anext__ method, got %.100s",
                                  type->tp_name);
                    goto error;
                }

                awaitable = _PyCoro_GetAwaitableIter(next_iter);
                if (awaitable == NULL) {
                    _PyErr_FormatFromCause(
                        PyExc_TypeError,
                        "'async for' received an invalid object "
                        "from __anext__: %.100s",
                        Py_TYPE(next_iter)->tp_name);

                    Py_DECREF(next_iter);
                    goto error;
                } else {
                    Py_DECREF(next_iter);
                }
            }

            PREDICT(LOAD_CONST);
        }

        inst(GET_AWAITABLE, (iterable -- iter)) {
            iter = _PyCoro_GetAwaitableIter(iterable);

            if (iter == NULL) {
                format_awaitable_error(tstate, Py_TYPE(iterable), oparg);
            }

            DECREF_INPUTS();

            if (iter != NULL && PyCoro_CheckExact(iter)) {
                PyObject *yf = _PyGen_yf((PyGenObject*)iter);
                if (yf != NULL) {
                    /* `iter` is a coroutine object that is being
                       awaited, `yf` is a pointer to the current awaitable
                       being awaited on. */
                    Py_DECREF(yf);
                    Py_CLEAR(iter);
                    _PyErr_SetString(tstate, PyExc_RuntimeError,
                                     "coroutine is being awaited already");
                    /* The code below jumps to `error` if `iter` is NULL. */
                }
            }

            ERROR_IF(iter == NULL, error);

            PREDICT(LOAD_CONST);
        }

        // error: SEND stack effect depends on jump flag
        inst(SEND) {
            assert(frame != &entry_frame);
            assert(STACK_LEVEL() >= 2);
            PyObject *v = POP();
            PyObject *receiver = TOP();
            PySendResult gen_status;
            PyObject *retval;
            if (tstate->c_tracefunc == NULL) {
                gen_status = PyIter_Send(receiver, v, &retval);
            } else {
                if (Py_IsNone(v) && PyIter_Check(receiver)) {
                    retval = Py_TYPE(receiver)->tp_iternext(receiver);
                }
                else {
                    retval = PyObject_CallMethodOneArg(receiver, &_Py_ID(send), v);
                }
                if (retval == NULL) {
                    if (tstate->c_tracefunc != NULL
                            && _PyErr_ExceptionMatches(tstate, PyExc_StopIteration))
                        call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, frame);
                    if (_PyGen_FetchStopIterationValue(&retval) == 0) {
                        gen_status = PYGEN_RETURN;
                    }
                    else {
                        gen_status = PYGEN_ERROR;
                    }
                }
                else {
                    gen_status = PYGEN_NEXT;
                }
            }
            Py_DECREF(v);
            if (gen_status == PYGEN_ERROR) {
                assert(retval == NULL);
                goto error;
            }
            if (gen_status == PYGEN_RETURN) {
                assert(retval != NULL);
                Py_DECREF(receiver);
                SET_TOP(retval);
                JUMPBY(oparg);
            }
            else {
                assert(gen_status == PYGEN_NEXT);
                assert(retval != NULL);
                PUSH(retval);
            }
        }

        inst(YIELD_VALUE, (retval -- unused)) {
            // NOTE: It's important that YIELD_VALUE never raises an exception!
            // The compiler treats any exception raised here as a failed close()
            // or throw() call.
            assert(oparg == STACK_LEVEL());
            assert(frame != &entry_frame);
            PyGenObject *gen = _PyFrame_GetGenerator(frame);
            gen->gi_frame_state = FRAME_SUSPENDED;
            _PyFrame_SetStackPointer(frame, stack_pointer - 1);
            TRACE_FUNCTION_EXIT();
            DTRACE_FUNCTION_EXIT();
            tstate->exc_info = gen->gi_exc_state.previous_item;
            gen->gi_exc_state.previous_item = NULL;
            _Py_LeaveRecursiveCallPy(tstate);
            _PyInterpreterFrame *gen_frame = frame;
            frame = cframe.current_frame = frame->previous;
            gen_frame->previous = NULL;
            frame->prev_instr -= frame->yield_offset;
            _PyFrame_StackPush(frame, retval);
            goto resume_frame;
        }

        inst(POP_EXCEPT, (exc_value -- )) {
            _PyErr_StackItem *exc_info = tstate->exc_info;
            Py_XSETREF(exc_info->exc_value, exc_value);
        }

        // stack effect: (__0 -- )
        inst(RERAISE) {
            if (oparg) {
                PyObject *lasti = PEEK(oparg + 1);
                if (PyLong_Check(lasti)) {
                    frame->prev_instr = _PyCode_CODE(frame->f_code) + PyLong_AsLong(lasti);
                    assert(!_PyErr_Occurred(tstate));
                }
                else {
                    assert(PyLong_Check(lasti));
                    _PyErr_SetString(tstate, PyExc_SystemError, "lasti is not an int");
                    goto error;
                }
            }
            PyObject *val = POP();
            assert(val && PyExceptionInstance_Check(val));
            PyObject *exc = Py_NewRef(PyExceptionInstance_Class(val));
            PyObject *tb = PyException_GetTraceback(val);
            _PyErr_Restore(tstate, exc, val, tb);
            goto exception_unwind;
        }

        inst(PREP_RERAISE_STAR, (orig, excs -- val)) {
            assert(PyList_Check(excs));

            val = _PyExc_PrepReraiseStar(orig, excs);
            DECREF_INPUTS();

            ERROR_IF(val == NULL, error);
        }

        // stack effect: (__0, __1 -- )
        inst(END_ASYNC_FOR) {
            PyObject *val = POP();
            assert(val && PyExceptionInstance_Check(val));
            if (PyErr_GivenExceptionMatches(val, PyExc_StopAsyncIteration)) {
                Py_DECREF(val);
                Py_DECREF(POP());
            }
            else {
                PyObject *exc = Py_NewRef(PyExceptionInstance_Class(val));
                PyObject *tb = PyException_GetTraceback(val);
                _PyErr_Restore(tstate, exc, val, tb);
                goto exception_unwind;
            }
        }

        // stack effect: (__0, __1 -- )
        inst(CLEANUP_THROW) {
            assert(throwflag);
            PyObject *exc_value = TOP();
            assert(exc_value && PyExceptionInstance_Check(exc_value));
            if (PyErr_GivenExceptionMatches(exc_value, PyExc_StopIteration)) {
                PyObject *value = ((PyStopIterationObject *)exc_value)->value;
                Py_INCREF(value);
                Py_DECREF(POP());  // The StopIteration.
                Py_DECREF(POP());  // The last sent value.
                Py_DECREF(POP());  // The delegated sub-iterator.
                PUSH(value);
            }
            else {
                PyObject *exc_type = Py_NewRef(Py_TYPE(exc_value));
                PyObject *exc_traceback = PyException_GetTraceback(exc_value);
                _PyErr_Restore(tstate, exc_type, Py_NewRef(exc_value), exc_traceback);
                goto exception_unwind;
            }
        }

        inst(LOAD_ASSERTION_ERROR, ( -- value)) {
            value = Py_NewRef(PyExc_AssertionError);
        }

        inst(LOAD_BUILD_CLASS, ( -- bc)) {
            if (PyDict_CheckExact(BUILTINS())) {
                bc = _PyDict_GetItemWithError(BUILTINS(),
                                              &_Py_ID(__build_class__));
                if (bc == NULL) {
                    if (!_PyErr_Occurred(tstate)) {
                        _PyErr_SetString(tstate, PyExc_NameError,
                                         "__build_class__ not found");
                    }
                    ERROR_IF(true, error);
                }
                Py_INCREF(bc);
            }
            else {
                bc = PyObject_GetItem(BUILTINS(), &_Py_ID(__build_class__));
                if (bc == NULL) {
                    if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                        _PyErr_SetString(tstate, PyExc_NameError,
                                         "__build_class__ not found");
                    ERROR_IF(true, error);
                }
            }
        }

        inst(STORE_NAME, (v -- )) {
            PyObject *name = GETITEM(names, oparg);
            PyObject *ns = LOCALS();
            int err;
            if (ns == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals found when storing %R", name);
                DECREF_INPUTS();
                ERROR_IF(true, error);
            }
            if (PyDict_CheckExact(ns))
                err = PyDict_SetItem(ns, name, v);
            else
                err = PyObject_SetItem(ns, name, v);
            DECREF_INPUTS();
            ERROR_IF(err, error);
        }

        inst(DELETE_NAME, (--)) {
            PyObject *name = GETITEM(names, oparg);
            PyObject *ns = LOCALS();
            int err;
            if (ns == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals when deleting %R", name);
                goto error;
            }
            err = PyObject_DelItem(ns, name);
            // Can't use ERROR_IF here.
            if (err != 0) {
                format_exc_check_arg(tstate, PyExc_NameError,
                                     NAME_ERROR_MSG,
                                     name);
                goto error;
            }
        }

        // stack effect: (__0 -- __array[oparg])
        inst(UNPACK_SEQUENCE) {
            _PyUnpackSequenceCache *cache = (_PyUnpackSequenceCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                PyObject *seq = TOP();
                next_instr--;
                _Py_Specialize_UnpackSequence(seq, next_instr, oparg);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(UNPACK_SEQUENCE, deferred);
            GO_TO_INSTRUCTION(UNPACK_SEQUENCE_GENERIC);
        }

        // stack effect: (__0 -- __array[oparg])
        inst(UNPACK_SEQUENCE_GENERIC) {
            PyObject *seq = POP();
            PyObject **top = stack_pointer + oparg;
            if (!unpack_iterable(tstate, seq, oparg, -1, top)) {
                Py_DECREF(seq);
                goto error;
            }
            STACK_GROW(oparg);
            Py_DECREF(seq);
            JUMPBY(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
        }

        // stack effect: (__0 -- __array[oparg])
        inst(UNPACK_SEQUENCE_TWO_TUPLE) {
            PyObject *seq = TOP();
            DEOPT_IF(!PyTuple_CheckExact(seq), UNPACK_SEQUENCE);
            DEOPT_IF(PyTuple_GET_SIZE(seq) != 2, UNPACK_SEQUENCE);
            STAT_INC(UNPACK_SEQUENCE, hit);
            SET_TOP(Py_NewRef(PyTuple_GET_ITEM(seq, 1)));
            PUSH(Py_NewRef(PyTuple_GET_ITEM(seq, 0)));
            Py_DECREF(seq);
            JUMPBY(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
        }

        // stack effect: (__0 -- __array[oparg])
        inst(UNPACK_SEQUENCE_TUPLE) {
            PyObject *seq = TOP();
            DEOPT_IF(!PyTuple_CheckExact(seq), UNPACK_SEQUENCE);
            DEOPT_IF(PyTuple_GET_SIZE(seq) != oparg, UNPACK_SEQUENCE);
            STAT_INC(UNPACK_SEQUENCE, hit);
            STACK_SHRINK(1);
            PyObject **items = _PyTuple_ITEMS(seq);
            while (oparg--) {
                PUSH(Py_NewRef(items[oparg]));
            }
            Py_DECREF(seq);
            JUMPBY(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
        }

        // stack effect: (__0 -- __array[oparg])
        inst(UNPACK_SEQUENCE_LIST) {
            PyObject *seq = TOP();
            DEOPT_IF(!PyList_CheckExact(seq), UNPACK_SEQUENCE);
            DEOPT_IF(PyList_GET_SIZE(seq) != oparg, UNPACK_SEQUENCE);
            STAT_INC(UNPACK_SEQUENCE, hit);
            STACK_SHRINK(1);
            PyObject **items = _PyList_ITEMS(seq);
            while (oparg--) {
                PUSH(Py_NewRef(items[oparg]));
            }
            Py_DECREF(seq);
            JUMPBY(INLINE_CACHE_ENTRIES_UNPACK_SEQUENCE);
        }

        // error: UNPACK_EX has irregular stack effect
        inst(UNPACK_EX) {
            int totalargs = 1 + (oparg & 0xFF) + (oparg >> 8);
            PyObject *seq = POP();
            PyObject **top = stack_pointer + totalargs;
            if (!unpack_iterable(tstate, seq, oparg & 0xFF, oparg >> 8, top)) {
                Py_DECREF(seq);
                goto error;
            }
            STACK_GROW(totalargs);
            Py_DECREF(seq);
        }

        family(store_attr) = {
            STORE_ATTR,
            STORE_ATTR_GENERIC,
            STORE_ATTR_INSTANCE_VALUE,
            STORE_ATTR_SLOT,
            STORE_ATTR_WITH_HINT,
        };

        inst(STORE_ATTR, (unused/1, unused/3, unused, owner --)) {
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                PyObject *name = GETITEM(names, oparg);
                next_instr--;
                _Py_Specialize_StoreAttr(owner, next_instr, name);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(STORE_ATTR, deferred);
            GO_TO_INSTRUCTION(STORE_ATTR_GENERIC);
        }

        inst(STORE_ATTR_GENERIC, (unused/1, unused/3, v, owner --)) {
            PyObject *name = GETITEM(names, oparg);
            int err = PyObject_SetAttr(owner, name, v);
            Py_DECREF(v);
            Py_DECREF(owner);
            ERROR_IF(err, error);
        }

        inst(DELETE_ATTR, (owner --)) {
            PyObject *name = GETITEM(names, oparg);
            int err = PyObject_SetAttr(owner, name, (PyObject *)NULL);
            Py_DECREF(owner);
            ERROR_IF(err, error);
        }

        inst(STORE_GLOBAL, (v --)) {
            PyObject *name = GETITEM(names, oparg);
            int err = PyDict_SetItem(GLOBALS(), name, v);
            Py_DECREF(v);
            ERROR_IF(err, error);
        }

        inst(DELETE_GLOBAL, (--)) {
            PyObject *name = GETITEM(names, oparg);
            int err;
            err = PyDict_DelItem(GLOBALS(), name);
            // Can't use ERROR_IF here.
            if (err != 0) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                    format_exc_check_arg(tstate, PyExc_NameError,
                                         NAME_ERROR_MSG, name);
                }
                goto error;
            }
        }

        inst(LOAD_NAME, ( -- v)) {
            PyObject *name = GETITEM(names, oparg);
            PyObject *locals = LOCALS();
            if (locals == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals when loading %R", name);
                goto error;
            }
            if (PyDict_CheckExact(locals)) {
                v = PyDict_GetItemWithError(locals, name);
                if (v != NULL) {
                    Py_INCREF(v);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
            }
            else {
                v = PyObject_GetItem(locals, name);
                if (v == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError))
                        goto error;
                    _PyErr_Clear(tstate);
                }
            }
            if (v == NULL) {
                v = PyDict_GetItemWithError(GLOBALS(), name);
                if (v != NULL) {
                    Py_INCREF(v);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
                else {
                    if (PyDict_CheckExact(BUILTINS())) {
                        v = PyDict_GetItemWithError(BUILTINS(), name);
                        if (v == NULL) {
                            if (!_PyErr_Occurred(tstate)) {
                                format_exc_check_arg(
                                        tstate, PyExc_NameError,
                                        NAME_ERROR_MSG, name);
                            }
                            goto error;
                        }
                        Py_INCREF(v);
                    }
                    else {
                        v = PyObject_GetItem(BUILTINS(), name);
                        if (v == NULL) {
                            if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                                format_exc_check_arg(
                                            tstate, PyExc_NameError,
                                            NAME_ERROR_MSG, name);
                            }
                            goto error;
                        }
                    }
                }
            }
        }

        // error: LOAD_GLOBAL has irregular stack effect
        inst(LOAD_GLOBAL) {
            _PyLoadGlobalCache *cache = (_PyLoadGlobalCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                PyObject *name = GETITEM(names, oparg>>1);
                next_instr--;
                _Py_Specialize_LoadGlobal(GLOBALS(), BUILTINS(), next_instr, name);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(LOAD_GLOBAL, deferred);
            GO_TO_INSTRUCTION(LOAD_GLOBAL_GENERIC);
        }

        // error: LOAD_GLOBAL has irregular stack effect
        inst(LOAD_GLOBAL_GENERIC) {
            int push_null = oparg & 1;
            PEEK(0) = NULL;
            PyObject *name = GETITEM(names, oparg>>1);
            PyObject *v;
            if (PyDict_CheckExact(GLOBALS())
                && PyDict_CheckExact(BUILTINS()))
            {
                v = _PyDict_LoadGlobal((PyDictObject *)GLOBALS(),
                                       (PyDictObject *)BUILTINS(),
                                       name);
                if (v == NULL) {
                    if (!_PyErr_Occurred(tstate)) {
                        /* _PyDict_LoadGlobal() returns NULL without raising
                         * an exception if the key doesn't exist */
                        format_exc_check_arg(tstate, PyExc_NameError,
                                             NAME_ERROR_MSG, name);
                    }
                    goto error;
                }
            }
            else {
                /* Slow-path if globals or builtins is not a dict */

                /* namespace 1: globals */
                v = PyObject_GetItem(GLOBALS(), name);
                if (v == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);

                    /* namespace 2: builtins */
                    v = PyObject_GetItem(BUILTINS(), name);
                    if (v == NULL) {
                        if (_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                            format_exc_check_arg(
                                        tstate, PyExc_NameError,
                                        NAME_ERROR_MSG, name);
                        }
                        goto error;
                    }
                }
            }
            /* Skip over inline cache */
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_GLOBAL);
            STACK_GROW(push_null);
            PUSH(v);
        }

        // error: LOAD_GLOBAL has irregular stack effect
        inst(LOAD_GLOBAL_MODULE) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyDict_CheckExact(GLOBALS()), LOAD_GLOBAL);
            PyDictObject *dict = (PyDictObject *)GLOBALS();
            _PyLoadGlobalCache *cache = (_PyLoadGlobalCache *)next_instr;
            uint32_t version = read_u32(cache->module_keys_version);
            DEOPT_IF(dict->ma_keys->dk_version != version, LOAD_GLOBAL);
            assert(DK_IS_UNICODE(dict->ma_keys));
            PyDictUnicodeEntry *entries = DK_UNICODE_ENTRIES(dict->ma_keys);
            PyObject *res = entries[cache->index].me_value;
            DEOPT_IF(res == NULL, LOAD_GLOBAL);
            int push_null = oparg & 1;
            PEEK(0) = NULL;
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_GLOBAL);
            STAT_INC(LOAD_GLOBAL, hit);
            STACK_GROW(push_null+1);
            SET_TOP(Py_NewRef(res));
        }

        // error: LOAD_GLOBAL has irregular stack effect
        inst(LOAD_GLOBAL_BUILTIN) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(!PyDict_CheckExact(GLOBALS()), LOAD_GLOBAL);
            DEOPT_IF(!PyDict_CheckExact(BUILTINS()), LOAD_GLOBAL);
            PyDictObject *mdict = (PyDictObject *)GLOBALS();
            PyDictObject *bdict = (PyDictObject *)BUILTINS();
            _PyLoadGlobalCache *cache = (_PyLoadGlobalCache *)next_instr;
            uint32_t mod_version = read_u32(cache->module_keys_version);
            uint16_t bltn_version = cache->builtin_keys_version;
            DEOPT_IF(mdict->ma_keys->dk_version != mod_version, LOAD_GLOBAL);
            DEOPT_IF(bdict->ma_keys->dk_version != bltn_version, LOAD_GLOBAL);
            assert(DK_IS_UNICODE(bdict->ma_keys));
            PyDictUnicodeEntry *entries = DK_UNICODE_ENTRIES(bdict->ma_keys);
            PyObject *res = entries[cache->index].me_value;
            DEOPT_IF(res == NULL, LOAD_GLOBAL);
            int push_null = oparg & 1;
            PEEK(0) = NULL;
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_GLOBAL);
            STAT_INC(LOAD_GLOBAL, hit);
            STACK_GROW(push_null+1);
            SET_TOP(Py_NewRef(res));
        }

        inst(DELETE_FAST, (--)) {
            PyObject *v = GETLOCAL(oparg);
            ERROR_IF(v == NULL, unbound_local_error);
            SETLOCAL(oparg, NULL);
        }

        inst(MAKE_CELL, (--)) {
            // "initial" is probably NULL but not if it's an arg (or set
            // via PyFrame_LocalsToFast() before MAKE_CELL has run).
            PyObject *initial = GETLOCAL(oparg);
            PyObject *cell = PyCell_New(initial);
            if (cell == NULL) {
                goto resume_with_error;
            }
            SETLOCAL(oparg, cell);
        }

        inst(DELETE_DEREF, (--)) {
            PyCellObject *cell = (PyCellObject *)GETLOCAL(oparg);
            PyObject *oldobj = _Py_atomic_exchange_ptr(&cell->ob_ref, NULL);
            // Can't use ERROR_IF here.
            // Fortunately we don't need its superpower.
            if (oldobj == NULL) {
                format_exc_unbound(tstate, frame->f_code, oparg);
                goto error;
            }
            Py_DECREF(oldobj);
        }

        inst(LOAD_CLASSDEREF, ( -- value)) {
            PyObject *name, *locals = LOCALS();
            assert(locals);
            assert(oparg >= 0 && oparg < frame->f_code->co_nlocalsplus);
            name = PyTuple_GET_ITEM(frame->f_code->co_localsplusnames, oparg);
            if (PyDict_CheckExact(locals)) {
                value = PyDict_GetItemWithError(locals, name);
                if (value != NULL) {
                    Py_INCREF(value);
                }
                else if (_PyErr_Occurred(tstate)) {
                    goto error;
                }
            }
            else {
                value = PyObject_GetItem(locals, name);
                if (value == NULL) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_KeyError)) {
                        goto error;
                    }
                    _PyErr_Clear(tstate);
                }
            }
            if (!value) {
                PyObject *cell = GETLOCAL(oparg);
                value = PyCell_GET(cell);
                if (value == NULL) {
                    format_exc_unbound(tstate, frame->f_code, oparg);
                    goto error;
                }
                Py_INCREF(value);
            }
        }

        inst(LOAD_DEREF, ( -- value)) {
            PyCellObject *cell = (PyCellObject *)GETLOCAL(oparg);
            value = _Py_XFetchRef(&cell->ob_ref);
            if (value == NULL) {
                format_exc_unbound(tstate, frame->f_code, oparg);
                ERROR_IF(true, error);
            }
        }

        inst(STORE_DEREF, (v --)) {
            PyCellObject *cell = (PyCellObject *)GETLOCAL(oparg);
            _PyObject_SetMaybeWeakref(v);
            PyObject *oldobj = _Py_atomic_exchange_ptr(&cell->ob_ref, v);
            Py_XDECREF(oldobj);
        }

        inst(COPY_FREE_VARS, (--)) {
            /* Copy closure variables to free variables */
            PyCodeObject *co = frame->f_code;
            assert(PyFunction_Check(frame->f_funcobj));
            PyObject *closure = ((PyFunctionObject *)frame->f_funcobj)->func_closure;
            assert(oparg == co->co_nfreevars);
            int offset = co->co_nlocalsplus - oparg;
            for (int i = 0; i < oparg; ++i) {
                PyObject *o = PyTuple_GET_ITEM(closure, i);
                frame->localsplus[offset + i] = Py_NewRef(o);
            }
        }

        // stack effect: (__array[oparg] -- __0)
        inst(BUILD_STRING) {
            PyObject *str;
            str = _PyUnicode_JoinArray(&_Py_STR(empty),
                                       stack_pointer - oparg, oparg);
            if (str == NULL)
                goto error;
            while (--oparg >= 0) {
                PyObject *item = POP();
                Py_DECREF(item);
            }
            PUSH(str);
        }

        // stack effect: (__array[oparg] -- __0)
        inst(BUILD_TUPLE) {
            STACK_SHRINK(oparg);
            PyObject *tup = _PyTuple_FromArraySteal(stack_pointer, oparg);
            if (tup == NULL)
                goto error;
            PUSH(tup);
        }

        // stack effect: (__array[oparg] -- __0)
        inst(BUILD_LIST) {
            STACK_SHRINK(oparg);
            PyObject *list = _PyList_FromArraySteal(stack_pointer, oparg);
            if (list == NULL)
                goto error;
            PUSH(list);
        }

        inst(LIST_EXTEND, (iterable -- )) {
            PyObject *list = PEEK(oparg + 1);  // iterable is still on the stack
            PyObject *none_val = _PyList_Extend((PyListObject *)list, iterable);
            if (none_val == NULL) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
                   (Py_TYPE(iterable)->tp_iter == NULL && !PySequence_Check(iterable)))
                {
                    _PyErr_Clear(tstate);
                    _PyErr_Format(tstate, PyExc_TypeError,
                          "Value after * must be an iterable, not %.200s",
                          Py_TYPE(iterable)->tp_name);
                }
                DECREF_INPUTS();
                ERROR_IF(true, error);
            }
            Py_DECREF(none_val);
            DECREF_INPUTS();
        }

        inst(SET_UPDATE, (iterable --)) {
            PyObject *set = PEEK(oparg + 1);  // iterable is still on the stack
            int err = _PySet_Update(set, iterable);
            DECREF_INPUTS();
            ERROR_IF(err < 0, error);
        }

        // stack effect: (__array[oparg] -- __0)
        inst(BUILD_SET) {
            PyObject *set = PySet_New(NULL);
            int err = 0;
            int i;
            if (set == NULL)
                goto error;
            for (i = oparg; i > 0; i--) {
                PyObject *item = PEEK(i);
                if (err == 0)
                    err = PySet_Add(set, item);
                Py_DECREF(item);
            }
            STACK_SHRINK(oparg);
            if (err != 0) {
                Py_DECREF(set);
                goto error;
            }
            PUSH(set);
        }

        // stack effect: (__array[oparg*2] -- __0)
        inst(BUILD_MAP) {
            PyObject *map = _PyDict_FromItems(
                    &PEEK(2*oparg), 2,
                    &PEEK(2*oparg - 1), 2,
                    oparg);
            if (map == NULL)
                goto error;

            while (oparg--) {
                Py_DECREF(POP());
                Py_DECREF(POP());
            }
            PUSH(map);
        }

        inst(SETUP_ANNOTATIONS, (--)) {
            int err;
            PyObject *ann_dict;
            if (LOCALS() == NULL) {
                _PyErr_Format(tstate, PyExc_SystemError,
                              "no locals found when setting up annotations");
                ERROR_IF(true, error);
            }
            /* check if __annotations__ in locals()... */
            if (PyDict_CheckExact(LOCALS())) {
                ann_dict = _PyDict_GetItemWithError(LOCALS(),
                                                    &_Py_ID(__annotations__));
                if (ann_dict == NULL) {
                    ERROR_IF(_PyErr_Occurred(tstate), error);
                    /* ...if not, create a new one */
                    ann_dict = PyDict_New();
                    ERROR_IF(ann_dict == NULL, error);
                    err = PyDict_SetItem(LOCALS(), &_Py_ID(__annotations__),
                                         ann_dict);
                    Py_DECREF(ann_dict);
                    ERROR_IF(err, error);
                }
            }
            else {
                /* do the same if locals() is not a dict */
                ann_dict = PyObject_GetItem(LOCALS(), &_Py_ID(__annotations__));
                if (ann_dict == NULL) {
                    ERROR_IF(!_PyErr_ExceptionMatches(tstate, PyExc_KeyError), error);
                    _PyErr_Clear(tstate);
                    ann_dict = PyDict_New();
                    ERROR_IF(ann_dict == NULL, error);
                    err = PyObject_SetItem(LOCALS(), &_Py_ID(__annotations__),
                                           ann_dict);
                    Py_DECREF(ann_dict);
                    ERROR_IF(err, error);
                }
                else {
                    Py_DECREF(ann_dict);
                }
            }
        }

        // stack effect: (__array[oparg] -- )
        inst(BUILD_CONST_KEY_MAP) {
            PyObject *map;
            PyObject *keys = TOP();
            if (!PyTuple_CheckExact(keys) ||
                PyTuple_GET_SIZE(keys) != (Py_ssize_t)oparg) {
                _PyErr_SetString(tstate, PyExc_SystemError,
                                 "bad BUILD_CONST_KEY_MAP keys argument");
                goto error;
            }
            map = _PyDict_FromItems(
                    &PyTuple_GET_ITEM(keys, 0), 1,
                    &PEEK(oparg + 1), 1, oparg);
            if (map == NULL) {
                goto error;
            }

            Py_DECREF(POP());
            while (oparg--) {
                Py_DECREF(POP());
            }
            PUSH(map);
        }

        inst(DICT_UPDATE, (update --)) {
            PyObject *dict = PEEK(oparg + 1);  // update is still on the stack
            if (PyDict_Update(dict, update) < 0) {
                if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                    "'%.200s' object is not a mapping",
                                    Py_TYPE(update)->tp_name);
                }
                DECREF_INPUTS();
                ERROR_IF(true, error);
            }
            DECREF_INPUTS();
        }

        inst(DICT_MERGE, (update --)) {
            PyObject *dict = PEEK(oparg + 1);  // update is still on the stack

            if (_PyDict_MergeEx(dict, update, 2) < 0) {
                format_kwargs_error(tstate, PEEK(3 + oparg), update);
                DECREF_INPUTS();
                ERROR_IF(true, error);
            }
            DECREF_INPUTS();
            PREDICT(CALL_FUNCTION_EX);
        }

        inst(MAP_ADD, (key, value --)) {
            PyObject *dict = PEEK(oparg + 2);  // key, value are still on the stack
            assert(PyDict_CheckExact(dict));
            /* dict[key] = value */
            // Do not DECREF INPUTS because the function steals the references
            ERROR_IF(_PyDict_SetItem_Take2((PyDictObject *)dict, key, value) != 0, error);
            PREDICT(JUMP_BACKWARD);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR) {
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                PyObject *owner = TOP();
                PyObject *name = GETITEM(names, oparg>>1);
                next_instr--;
                _Py_Specialize_LoadAttr(owner, next_instr, name);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(LOAD_ATTR, deferred);
            GO_TO_INSTRUCTION(LOAD_ATTR_GENERIC);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_GENERIC) {
            PyObject *name = GETITEM(names, oparg >> 1);
            PyObject *owner = TOP();
            if (oparg & 1) {
                /* Designed to work in tandem with CALL. */
                PyObject* meth = NULL;

                int meth_found = _PyObject_GetMethod(owner, name, &meth);

                if (meth == NULL) {
                    /* Most likely attribute wasn't found. */
                    goto error;
                }

                if (meth_found) {
                    /* We can bypass temporary bound method object.
                       meth is unbound method and obj is self.

                       meth | self | arg1 | ... | argN
                     */
                    SET_TOP(meth);
                    PUSH(owner);  // self
                }
                else {
                    /* meth is not an unbound method (but a regular attr, or
                       something was returned by a descriptor protocol).  Set
                       the second element of the stack to NULL, to signal
                       CALL that it's not a method call.

                       NULL | meth | arg1 | ... | argN
                    */
                    SET_TOP(NULL);
                    Py_DECREF(owner);
                    PUSH(meth);
                }
            }
            else {
                PyObject *res = PyObject_GetAttr(owner, name);
                if (res == NULL) {
                    goto error;
                }
                Py_DECREF(owner);
                SET_TOP(res);
            }
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_INSTANCE_VALUE) {
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, LOAD_ATTR);
            assert(tp->tp_dictoffset < 0);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = _PyObject_DictOrValues(owner);
            DEOPT_IF(!_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            PyDictValues *dv = _PyDictOrValues_GetValues(dorv);
            res = _Py_TryXFetchRef(&dv->values[cache->index]);
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            SET_TOP(NULL);
            STACK_GROW((oparg & 1));
            SET_TOP(res);
            Py_DECREF(owner);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_MODULE) {
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            DEOPT_IF(!PyModule_CheckExact(owner), LOAD_ATTR);
            PyDictObject *dict = (PyDictObject *)((PyModuleObject *)owner)->md_dict;
            assert(dict != NULL);
            PyDictKeysObject *keys = _Py_atomic_load_ptr_relaxed(&dict->ma_keys);
            DEOPT_IF(keys->dk_version != read_u32(cache->version), LOAD_ATTR);
            assert(keys->dk_kind == DICT_KEYS_UNICODE);
            assert(cache->index < keys->dk_nentries);
            PyDictUnicodeEntry *ep = DK_UNICODE_ENTRIES(keys) + cache->index;
            res = _Py_TryXFetchRef(&ep->me_value);
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            SET_TOP(NULL);
            STACK_GROW((oparg & 1));
            SET_TOP(res);
            Py_DECREF(owner);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_WITH_HINT) {
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, LOAD_ATTR);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = _PyObject_DictOrValues(owner);
            DEOPT_IF(_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            PyDictObject *dict = (PyDictObject *)_PyDictOrValues_GetDict(dorv);
            DEOPT_IF(dict == NULL, LOAD_ATTR);
            assert(PyDict_CheckExact((PyObject *)dict));
            PyObject *name = GETITEM(names, oparg>>1);
            uint16_t hint = cache->index;
            DEOPT_IF(hint >= (size_t)dict->ma_keys->dk_nentries, LOAD_ATTR);
            if (DK_IS_UNICODE(dict->ma_keys)) {
                PyDictUnicodeEntry *ep = DK_UNICODE_ENTRIES(dict->ma_keys) + hint;
                DEOPT_IF(ep->me_key != name, LOAD_ATTR);
                res = ep->me_value;
            }
            else {
                PyDictKeyEntry *ep = DK_ENTRIES(dict->ma_keys) + hint;
                DEOPT_IF(ep->me_key != name, LOAD_ATTR);
                res = ep->me_value;
            }
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(res);
            SET_TOP(NULL);
            STACK_GROW((oparg & 1));
            SET_TOP(res);
            Py_DECREF(owner);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_SLOT) {
            assert(cframe.use_tracing == 0);
            PyObject *owner = TOP();
            PyObject *res;
            PyTypeObject *tp = Py_TYPE(owner);
            _PyAttrCache *cache = (_PyAttrCache *)next_instr;
            uint32_t type_version = read_u32(cache->version);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, LOAD_ATTR);
            char *addr = (char *)owner + cache->index;
            res = *(PyObject **)addr;
            DEOPT_IF(res == NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(res);
            SET_TOP(NULL);
            STACK_GROW((oparg & 1));
            SET_TOP(res);
            Py_DECREF(owner);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_CLASS) {
            assert(cframe.use_tracing == 0);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;

            PyObject *cls = TOP();
            DEOPT_IF(!PyType_Check(cls), LOAD_ATTR);
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(((PyTypeObject *)cls)->tp_version_tag != type_version,
                LOAD_ATTR);
            assert(type_version != 0);

            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            Py_INCREF(res);
            SET_TOP(NULL);
            STACK_GROW((oparg & 1));
            SET_TOP(res);
            Py_DECREF(cls);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_PROPERTY) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(tstate->interp->eval_frame, LOAD_ATTR);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;

            PyObject *owner = TOP();
            PyTypeObject *cls = Py_TYPE(owner);
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(type_version != 0);
            PyObject *fget = read_obj(cache->descr);
            assert(Py_IS_TYPE(fget, &PyFunction_Type));
            PyFunctionObject *f = (PyFunctionObject *)fget;
            uint32_t func_version = read_u32(cache->keys_version);
            assert(func_version != 0);
            DEOPT_IF(f->func_version != func_version, LOAD_ATTR);
            PyCodeObject *code = (PyCodeObject *)f->func_code;
            assert(code->co_argcount == 1);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            Py_INCREF(fget);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, f, 1);
            SET_TOP(NULL);
            int shrink_stack = !(oparg & 1);
            STACK_SHRINK(shrink_stack);
            new_frame->localsplus[0] = owner;
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            DISPATCH_INLINED(new_frame);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN) {
            assert(cframe.use_tracing == 0);
            DEOPT_IF(tstate->interp->eval_frame, LOAD_ATTR);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            PyObject *owner = TOP();
            PyTypeObject *cls = Py_TYPE(owner);
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(type_version != 0);
            PyObject *getattribute = read_obj(cache->descr);
            assert(Py_IS_TYPE(getattribute, &PyFunction_Type));
            PyFunctionObject *f = (PyFunctionObject *)getattribute;
            uint32_t func_version = read_u32(cache->keys_version);
            assert(func_version != 0);
            DEOPT_IF(f->func_version != func_version, LOAD_ATTR);
            PyCodeObject *code = (PyCodeObject *)f->func_code;
            assert(code->co_argcount == 2);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);

            PyObject *name = GETITEM(names, oparg >> 1);
            Py_INCREF(f);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, f, 2);
            SET_TOP(NULL);
            int shrink_stack = !(oparg & 1);
            STACK_SHRINK(shrink_stack);
            new_frame->localsplus[0] = owner;
            new_frame->localsplus[1] = Py_NewRef(name);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
            DISPATCH_INLINED(new_frame);
        }

        inst(STORE_ATTR_INSTANCE_VALUE, (unused/1, type_version/2, index/1, value, owner --)) {
            assert(cframe.use_tracing == 0);
            PyTypeObject *tp = Py_TYPE(owner);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, STORE_ATTR);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues *dorv_ptr = _PyObject_DictOrValuesPointer(owner);
            PyDictValues *values = _PyDictValues_Lock(dorv_ptr);
            DEOPT_IF(values == NULL, STORE_ATTR);
            STAT_INC(STORE_ATTR, hit);
            PyObject *old_value = values->values[index];
            _Py_atomic_store_ptr_release(&values->values[index], value);
            if (old_value == NULL) {
                _PyDictValues_AddToInsertionOrder(values, index);
            }
            _PyDictValues_Unlock(dorv_ptr);
            Py_XDECREF(old_value);
            Py_DECREF(owner);
        }

        inst(STORE_ATTR_WITH_HINT, (unused/1, type_version/2, hint/1, value, owner --)) {
            assert(cframe.use_tracing == 0);
            PyTypeObject *tp = Py_TYPE(owner);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, STORE_ATTR);
            assert(tp->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = _PyObject_DictOrValues(owner);
            DEOPT_IF(_PyDictOrValues_IsValues(dorv), STORE_ATTR);
            PyDictObject *dict = (PyDictObject *)_PyDictOrValues_GetDict(dorv);
            DEOPT_IF(dict == NULL, STORE_ATTR);
            assert(PyDict_CheckExact((PyObject *)dict));
            PyObject *name = GETITEM(names, oparg);
            Py_BEGIN_CRITICAL_SECTION(dict);
            DEOPT_UNLOCK_IF(hint >= (size_t)dict->ma_keys->dk_nentries, STORE_ATTR);
            PyObject *old_value;
            uint64_t new_version;
            if (DK_IS_UNICODE(dict->ma_keys)) {
                PyDictUnicodeEntry *ep = DK_UNICODE_ENTRIES(dict->ma_keys) + hint;
                DEOPT_UNLOCK_IF(ep->me_key != name, STORE_ATTR);
                old_value = ep->me_value;
                DEOPT_UNLOCK_IF(old_value == NULL, STORE_ATTR);
                new_version = _PyDict_NotifyEvent(PyDict_EVENT_MODIFIED, dict, name, value);
                _Py_atomic_store_ptr_relaxed(&ep->me_value, value);
            }
            else {
                PyDictKeyEntry *ep = DK_ENTRIES(dict->ma_keys) + hint;
                DEOPT_UNLOCK_IF(ep->me_key != name, STORE_ATTR);
                old_value = ep->me_value;
                DEOPT_UNLOCK_IF(old_value == NULL, STORE_ATTR);
                new_version = _PyDict_NotifyEvent(PyDict_EVENT_MODIFIED, dict, name, value);
                _Py_atomic_store_ptr_relaxed(&ep->me_value, value);
            }
            Py_DECREF(old_value);
            STAT_INC(STORE_ATTR, hit);
            /* Ensure dict is GC tracked if it needs to be */
            if (!_PyObject_GC_IS_TRACKED(dict) && _PyObject_GC_MAY_BE_TRACKED(value)) {
                _PyObject_GC_TRACK(dict);
            }
            /* PEP 509 */
            dict->ma_version_tag = new_version;
            Py_END_CRITICAL_SECTION;
            Py_DECREF(owner);
        }

        inst(STORE_ATTR_SLOT, (unused/1, type_version/2, index/1, value, owner --)) {
            assert(cframe.use_tracing == 0);
            PyTypeObject *tp = Py_TYPE(owner);
            assert(type_version != 0);
            DEOPT_IF(tp->tp_version_tag != type_version, STORE_ATTR);
            char *addr = (char *)owner + index;
            STAT_INC(STORE_ATTR, hit);
            PyObject *old_value = *(PyObject **)addr;
            *(PyObject **)addr = value;
            Py_XDECREF(old_value);
            Py_DECREF(owner);
        }

        family(compare_op) = {
            COMPARE_OP,
            COMPARE_OP_GENERIC,
            _COMPARE_OP_FLOAT,
            _COMPARE_OP_INT,
            _COMPARE_OP_STR,
        };

        inst(COMPARE_OP, (unused/2, left, right -- unused)) {
            _PyCompareOpCache *cache = (_PyCompareOpCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                next_instr--;
                _Py_Specialize_CompareOp(left, right, next_instr, oparg);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(COMPARE_OP, deferred);
            GO_TO_INSTRUCTION(COMPARE_OP_GENERIC);
        }

        inst(COMPARE_OP_GENERIC, (unused/2, left, right -- res)) {
            assert(oparg <= Py_GE);
            res = PyObject_RichCompare(left, right, oparg);
            Py_DECREF(left);
            Py_DECREF(right);
            ERROR_IF(res == NULL, error);
        }

        // The result is an int disguised as an object pointer.
        op(_COMPARE_OP_FLOAT, (unused/1, when_to_jump_mask/1, left, right -- jump: size_t)) {
            assert(cframe.use_tracing == 0);
            // Combined: COMPARE_OP (float ? float) + POP_JUMP_IF_(true/false)
            DEOPT_IF(!PyFloat_CheckExact(left), COMPARE_OP);
            DEOPT_IF(!PyFloat_CheckExact(right), COMPARE_OP);
            STAT_INC(COMPARE_OP, hit);
            double dleft = PyFloat_AS_DOUBLE(left);
            double dright = PyFloat_AS_DOUBLE(right);
            // 1 if NaN, 2 if <, 4 if >, 8 if ==; this matches when_to_jump_mask
            int sign_ish = 1 << (2 * (dleft >= dright) + (dleft <= dright));
            _Py_DECREF_SPECIALIZED(left, _PyFloat_ExactDealloc);
            _Py_DECREF_SPECIALIZED(right, _PyFloat_ExactDealloc);
            jump = sign_ish & when_to_jump_mask;
        }
        // The input is an int disguised as an object pointer!
        op(_JUMP_IF, (jump: size_t --)) {
            assert(opcode == POP_JUMP_IF_FALSE || opcode == POP_JUMP_IF_TRUE);
            if (jump) {
                JUMPBY(oparg);
            }
        }
        // We're praying that the compiler optimizes the flags manipuations.
        super(COMPARE_OP_FLOAT_JUMP) = _COMPARE_OP_FLOAT + _JUMP_IF;

        // Similar to COMPARE_OP_FLOAT
        op(_COMPARE_OP_INT, (unused/1, when_to_jump_mask/1, left, right -- jump: size_t)) {
            assert(cframe.use_tracing == 0);
            // Combined: COMPARE_OP (int ? int) + POP_JUMP_IF_(true/false)
            DEOPT_IF(!PyLong_CheckExact(left), COMPARE_OP);
            DEOPT_IF(!PyLong_CheckExact(right), COMPARE_OP);
            DEOPT_IF((size_t)(Py_SIZE(left) + 1) > 2, COMPARE_OP);
            DEOPT_IF((size_t)(Py_SIZE(right) + 1) > 2, COMPARE_OP);
            STAT_INC(COMPARE_OP, hit);
            assert(Py_ABS(Py_SIZE(left)) <= 1 && Py_ABS(Py_SIZE(right)) <= 1);
            Py_ssize_t ileft = Py_SIZE(left) * ((PyLongObject *)left)->ob_digit[0];
            Py_ssize_t iright = Py_SIZE(right) * ((PyLongObject *)right)->ob_digit[0];
            // 2 if <, 4 if >, 8 if ==; this matches when_to_jump_mask
            int sign_ish = 1 << (2 * (ileft >= iright) + (ileft <= iright));
            _Py_DECREF_SPECIALIZED(left, (destructor)PyObject_Free);
            _Py_DECREF_SPECIALIZED(right, (destructor)PyObject_Free);
            jump = sign_ish & when_to_jump_mask;
        }
        super(COMPARE_OP_INT_JUMP) = _COMPARE_OP_INT + _JUMP_IF;

        // Similar to COMPARE_OP_FLOAT, but for ==, != only
        op(_COMPARE_OP_STR, (unused/1, invert/1, left, right -- jump: size_t)) {
            assert(cframe.use_tracing == 0);
            // Combined: COMPARE_OP (str == str or str != str) + POP_JUMP_IF_(true/false)
            DEOPT_IF(!PyUnicode_CheckExact(left), COMPARE_OP);
            DEOPT_IF(!PyUnicode_CheckExact(right), COMPARE_OP);
            STAT_INC(COMPARE_OP, hit);
            int res = _PyUnicode_Equal(left, right);
            assert(oparg == Py_EQ || oparg == Py_NE);
            _Py_DECREF_SPECIALIZED(left, _PyUnicode_ExactDealloc);
            _Py_DECREF_SPECIALIZED(right, _PyUnicode_ExactDealloc);
            assert(res == 0 || res == 1);
            assert(invert == 0 || invert == 1);
            jump = res ^ invert;
        }
        super(COMPARE_OP_STR_JUMP) = _COMPARE_OP_STR + _JUMP_IF;

        inst(IS_OP, (left, right -- b)) {
            int res = Py_Is(left, right) ^ oparg;
            DECREF_INPUTS();
            b = Py_NewRef(res ? Py_True : Py_False);
        }

        inst(CONTAINS_OP, (left, right -- b)) {
            int res = PySequence_Contains(right, left);
            DECREF_INPUTS();
            ERROR_IF(res < 0, error);
            b = Py_NewRef((res^oparg) ? Py_True : Py_False);
        }

        // stack effect: ( -- )
        inst(CHECK_EG_MATCH) {
            PyObject *match_type = POP();
            if (check_except_star_type_valid(tstate, match_type) < 0) {
                Py_DECREF(match_type);
                goto error;
            }

            PyObject *exc_value = TOP();
            PyObject *match = NULL, *rest = NULL;
            int res = exception_group_match(exc_value, match_type,
                                            &match, &rest);
            Py_DECREF(match_type);
            if (res < 0) {
                goto error;
            }

            if (match == NULL || rest == NULL) {
                assert(match == NULL);
                assert(rest == NULL);
                goto error;
            }
            if (Py_IsNone(match)) {
                PUSH(match);
                Py_XDECREF(rest);
            }
            else {
                /* Total or partial match - update the stack from
                 * [val]
                 * to
                 * [rest, match]
                 * (rest can be Py_None)
                 */

                SET_TOP(rest);
                PUSH(match);
                PyErr_SetExcInfo(NULL, Py_NewRef(match), NULL);
                Py_DECREF(exc_value);
            }
        }

        inst(CHECK_EXC_MATCH, (left, right -- left, b)) {
            assert(PyExceptionInstance_Check(left));
            if (check_except_type_valid(tstate, right) < 0) {
                 DECREF_INPUTS();
                 ERROR_IF(true, error);
            }

            int res = PyErr_GivenExceptionMatches(left, right);
            DECREF_INPUTS();
            b = Py_NewRef(res ? Py_True : Py_False);
        }

         inst(IMPORT_NAME, (level, fromlist -- res)) {
            PyObject *name = GETITEM(names, oparg);
            res = import_name(tstate, frame, name, fromlist, level);
            DECREF_INPUTS();
            ERROR_IF(res == NULL, error);
        }

        inst(IMPORT_FROM, (from -- from, res)) {
            PyObject *name = GETITEM(names, oparg);
            res = import_from(tstate, from, name);
            ERROR_IF(res == NULL, error);
        }

        inst(JUMP_FORWARD, (--)) {
            JUMPBY(oparg);
        }

        inst(JUMP_BACKWARD, (--)) {
            assert(oparg < INSTR_OFFSET());
            JUMPBY(-oparg);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0 -- )
        inst(POP_JUMP_IF_FALSE) {
            PyObject *cond = POP();
            if (Py_IsTrue(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
            }
            else if (Py_IsFalse(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                JUMPBY(oparg);
            }
            else {
                int err = PyObject_IsTrue(cond);
                Py_DECREF(cond);
                if (err > 0)
                    ;
                else if (err == 0) {
                    JUMPBY(oparg);
                }
                else
                    goto error;
            }
        }

        // stack effect: (__0 -- )
        inst(POP_JUMP_IF_TRUE) {
            PyObject *cond = POP();
            if (Py_IsFalse(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
            }
            else if (Py_IsTrue(cond)) {
                _Py_DECREF_NO_DEALLOC(cond);
                JUMPBY(oparg);
            }
            else {
                int err = PyObject_IsTrue(cond);
                Py_DECREF(cond);
                if (err > 0) {
                    JUMPBY(oparg);
                }
                else if (err == 0)
                    ;
                else
                    goto error;
            }
        }

        // stack effect: (__0 -- )
        inst(POP_JUMP_IF_NOT_NONE) {
            PyObject *value = POP();
            if (!Py_IsNone(value)) {
                JUMPBY(oparg);
            }
            Py_DECREF(value);
        }

        // stack effect: (__0 -- )
        inst(POP_JUMP_IF_NONE) {
            PyObject *value = POP();
            if (Py_IsNone(value)) {
                _Py_DECREF_NO_DEALLOC(value);
                JUMPBY(oparg);
            }
            else {
                Py_DECREF(value);
            }
        }

        // error: JUMP_IF_FALSE_OR_POP stack effect depends on jump flag
        inst(JUMP_IF_FALSE_OR_POP) {
            PyObject *cond = TOP();
            int err;
            if (Py_IsTrue(cond)) {
                STACK_SHRINK(1);
                _Py_DECREF_NO_DEALLOC(cond);
            }
            else if (Py_IsFalse(cond)) {
                JUMPBY(oparg);
            }
            else {
                err = PyObject_IsTrue(cond);
                if (err > 0) {
                    STACK_SHRINK(1);
                    Py_DECREF(cond);
                }
                else if (err == 0) {
                    JUMPBY(oparg);
                }
                else {
                    goto error;
                }
            }
        }

        // error: JUMP_IF_TRUE_OR_POP stack effect depends on jump flag
        inst(JUMP_IF_TRUE_OR_POP) {
            PyObject *cond = TOP();
            int err;
            if (Py_IsFalse(cond)) {
                STACK_SHRINK(1);
                _Py_DECREF_NO_DEALLOC(cond);
            }
            else if (Py_IsTrue(cond)) {
                JUMPBY(oparg);
            }
            else {
                err = PyObject_IsTrue(cond);
                if (err > 0) {
                    JUMPBY(oparg);
                }
                else if (err == 0) {
                    STACK_SHRINK(1);
                    Py_DECREF(cond);
                }
                else {
                    goto error;
                }
            }
        }

        // stack effect: ( -- )
        inst(JUMP_BACKWARD_NO_INTERRUPT) {
            /* This bytecode is used in the `yield from` or `await` loop.
             * If there is an interrupt, we want it handled in the innermost
             * generator or coroutine, so we deliberately do not check it here.
             * (see bpo-30039).
             */
            JUMPBY(-oparg);
        }

        // stack effect: ( -- __0)
        inst(GET_LEN) {
            // PUSH(len(TOS))
            Py_ssize_t len_i = PyObject_Length(TOP());
            if (len_i < 0) {
                goto error;
            }
            PyObject *len_o = PyLong_FromSsize_t(len_i);
            if (len_o == NULL) {
                goto error;
            }
            PUSH(len_o);
        }

        // stack effect: (__0, __1 -- )
        inst(MATCH_CLASS) {
            // Pop TOS and TOS1. Set TOS to a tuple of attributes on success, or
            // None on failure.
            PyObject *names = POP();
            PyObject *type = POP();
            PyObject *subject = TOP();
            assert(PyTuple_CheckExact(names));
            PyObject *attrs = match_class(tstate, subject, type, oparg, names);
            Py_DECREF(names);
            Py_DECREF(type);
            if (attrs) {
                // Success!
                assert(PyTuple_CheckExact(attrs));
                SET_TOP(attrs);
            }
            else if (_PyErr_Occurred(tstate)) {
                // Error!
                goto error;
            }
            else {
                // Failure!
                SET_TOP(Py_None);
            }
            Py_DECREF(subject);
        }

        // stack effect: ( -- __0)
        inst(MATCH_MAPPING) {
            PyObject *subject = TOP();
            int match = Py_TYPE(subject)->tp_flags & Py_TPFLAGS_MAPPING;
            PyObject *res = match ? Py_True : Py_False;
            PUSH(Py_NewRef(res));
            PREDICT(POP_JUMP_IF_FALSE);
        }

        // stack effect: ( -- __0)
        inst(MATCH_SEQUENCE) {
            PyObject *subject = TOP();
            int match = Py_TYPE(subject)->tp_flags & Py_TPFLAGS_SEQUENCE;
            PyObject *res = match ? Py_True : Py_False;
            PUSH(Py_NewRef(res));
            PREDICT(POP_JUMP_IF_FALSE);
        }

        // stack effect: ( -- __0)
        inst(MATCH_KEYS) {
            // On successful match, PUSH(values). Otherwise, PUSH(None).
            PyObject *keys = TOP();
            PyObject *subject = SECOND();
            PyObject *values_or_none = match_keys(tstate, subject, keys);
            if (values_or_none == NULL) {
                goto error;
            }
            PUSH(values_or_none);
        }

        // stack effect: ( -- )
        inst(GET_ITER) {
            /* before: [obj]; after [getiter(obj)] */
            PyObject *iterable = TOP();
            PyObject *iter = PyObject_GetIter(iterable);
            Py_DECREF(iterable);
            SET_TOP(iter);
            if (iter == NULL)
                goto error;
        }

        // stack effect: ( -- )
        inst(GET_YIELD_FROM_ITER) {
            /* before: [obj]; after [getiter(obj)] */
            PyObject *iterable = TOP();
            PyObject *iter;
            if (PyCoro_CheckExact(iterable)) {
                /* `iterable` is a coroutine */
                if (!(frame->f_code->co_flags & (CO_COROUTINE | CO_ITERABLE_COROUTINE))) {
                    /* and it is used in a 'yield from' expression of a
                       regular generator. */
                    Py_DECREF(iterable);
                    SET_TOP(NULL);
                    _PyErr_SetString(tstate, PyExc_TypeError,
                                     "cannot 'yield from' a coroutine object "
                                     "in a non-coroutine generator");
                    goto error;
                }
            }
            else if (!PyGen_CheckExact(iterable)) {
                /* `iterable` is not a generator. */
                iter = PyObject_GetIter(iterable);
                Py_DECREF(iterable);
                SET_TOP(iter);
                if (iter == NULL)
                    goto error;
            }
            PREDICT(LOAD_CONST);
        }

        // stack effect: ( -- __0)
        inst(FOR_ITER) {
            _PyForIterCache *cache = (_PyForIterCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                next_instr--;
                _Py_Specialize_ForIter(TOP(), next_instr, oparg);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(FOR_ITER, deferred);
            GO_TO_INSTRUCTION(FOR_ITER_GENERIC);
        }

        // stack effect: ( -- __0)
        inst(FOR_ITER_GENERIC) {
            /* before: [iter]; after: [iter, iter()] *or* [] */
            PyObject *iter = TOP();
            PyObject *next = (*Py_TYPE(iter)->tp_iternext)(iter);
            if (next != NULL) {
                PUSH(next);
                JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER);
            }
            else {
                if (_PyErr_Occurred(tstate)) {
                    if (!_PyErr_ExceptionMatches(tstate, PyExc_StopIteration)) {
                        goto error;
                    }
                    else if (tstate->c_tracefunc != NULL) {
                        call_exc_trace(tstate->c_tracefunc, tstate->c_traceobj, tstate, frame);
                    }
                    _PyErr_Clear(tstate);
                }
                /* iterator ended normally */
                assert(_Py_OPCODE(next_instr[INLINE_CACHE_ENTRIES_FOR_ITER + oparg]) == END_FOR);
                STACK_SHRINK(1);
                Py_DECREF(iter);
                /* Skip END_FOR */
                JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER + oparg + 1);
            }
        }

        // stack effect: ( -- __0)
        inst(FOR_ITER_LIST) {
            assert(cframe.use_tracing == 0);
            _PyListIterObject *it = (_PyListIterObject *)TOP();
            DEOPT_IF(Py_TYPE(it) != &PyListIter_Type, FOR_ITER);
            STAT_INC(FOR_ITER, hit);
            PyListObject *seq = it->it_seq;
            Py_ssize_t index = _Py_atomic_load_ssize_relaxed(&it->it_index);
            Py_ssize_t size = _Py_atomic_load_ssize_relaxed(&((PyVarObject *)seq)->ob_size);
            if ((size_t)index < (size_t)size) {
                PyObject **ob_item = _Py_atomic_load_ptr_relaxed(&seq->ob_item);
                DEOPT_IF(index >= _PyList_Capacity(ob_item), FOR_ITER);
                PyObject *next = _Py_TryXFetchRef(&ob_item[index]);
                DEOPT_IF(next == NULL, FOR_ITER);
                _Py_atomic_store_ssize_relaxed(&it->it_index, index + 1);
                PUSH(next);
                JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER);
                goto end_for_iter_list;  // End of this instruction
            }
            _Py_atomic_store_ssize_relaxed(&it->it_index, -1);
            STACK_SHRINK(1);
            Py_DECREF(it);
            JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER + oparg + 1);
        end_for_iter_list:
        }

        // stack effect: ( -- __0)
        inst(FOR_ITER_TUPLE) {
            assert(cframe.use_tracing == 0);
            _PyTupleIterObject *it = (_PyTupleIterObject *)TOP();
            DEOPT_IF(Py_TYPE(it) != &PyTupleIter_Type, FOR_ITER);
            STAT_INC(FOR_ITER, hit);
            PyTupleObject *seq = it->it_seq;
            if (seq) {
                if (it->it_index < PyTuple_GET_SIZE(seq)) {
                    PyObject *next = PyTuple_GET_ITEM(seq, it->it_index++);
                    PUSH(Py_NewRef(next));
                    JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER);
                    goto end_for_iter_tuple;  // End of this instruction
                }
                it->it_seq = NULL;
                Py_DECREF(seq);
            }
            STACK_SHRINK(1);
            Py_DECREF(it);
            JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER + oparg + 1);
        end_for_iter_tuple:
        }

        // stack effect: ( -- __0)
        inst(FOR_ITER_RANGE) {
            assert(cframe.use_tracing == 0);
            _PyRangeIterObject *r = (_PyRangeIterObject *)TOP();
            DEOPT_IF(Py_TYPE(r) != &PyRangeIter_Type, FOR_ITER);
            STAT_INC(FOR_ITER, hit);
            _Py_CODEUNIT next = next_instr[INLINE_CACHE_ENTRIES_FOR_ITER];
            assert(_PyOpcode_Deopt[_Py_OPCODE(next)] == STORE_FAST);
            if (r->len <= 0) {
                STACK_SHRINK(1);
                Py_DECREF(r);
                JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER + oparg + 1);
            }
            else {
                long value = r->start;
                r->start = value + r->step;
                r->len--;
                if (_PyLong_AssignValue(&GETLOCAL(_Py_OPARG(next)), value) < 0) {
                    goto error;
                }
                // The STORE_FAST is already done.
                JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER + 1);
            }
        }

        inst(FOR_ITER_GEN) {
            assert(cframe.use_tracing == 0);
            PyGenObject *gen = (PyGenObject *)TOP();
            DEOPT_IF(Py_TYPE(gen) != &PyGen_Type, FOR_ITER);
            DEOPT_IF(gen->gi_frame_state >= FRAME_EXECUTING, FOR_ITER);
            STAT_INC(FOR_ITER, hit);
            _PyInterpreterFrame *gen_frame = (_PyInterpreterFrame *)gen->gi_iframe;
            frame->yield_offset = oparg;
            _PyFrame_StackPush(gen_frame, Py_None);
            gen->gi_frame_state = FRAME_EXECUTING;
            gen->gi_exc_state.previous_item = tstate->exc_info;
            tstate->exc_info = &gen->gi_exc_state;
            JUMPBY(INLINE_CACHE_ENTRIES_FOR_ITER + oparg);
            assert(_Py_OPCODE(*next_instr) == END_FOR);
            DISPATCH_INLINED(gen_frame);
        }

        // stack effect: ( -- __0)
        inst(BEFORE_ASYNC_WITH) {
            PyObject *mgr = TOP();
            PyObject *res;
            PyObject *enter = _PyObject_LookupSpecial(mgr, &_Py_ID(__aenter__));
            if (enter == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "asynchronous context manager protocol",
                                  Py_TYPE(mgr)->tp_name);
                }
                goto error;
            }
            PyObject *exit = _PyObject_LookupSpecial(mgr, &_Py_ID(__aexit__));
            if (exit == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "asynchronous context manager protocol "
                                  "(missed __aexit__ method)",
                                  Py_TYPE(mgr)->tp_name);
                }
                Py_DECREF(enter);
                goto error;
            }
            SET_TOP(exit);
            Py_DECREF(mgr);
            res = _PyObject_CallNoArgs(enter);
            Py_DECREF(enter);
            if (res == NULL)
                goto error;
            PUSH(res);
            PREDICT(GET_AWAITABLE);
        }

        // stack effect: ( -- __0)
        inst(BEFORE_WITH) {
            PyObject *mgr = TOP();
            PyObject *res;
            PyObject *enter = _PyObject_LookupSpecial(mgr, &_Py_ID(__enter__));
            if (enter == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "context manager protocol",
                                  Py_TYPE(mgr)->tp_name);
                }
                goto error;
            }
            PyObject *exit = _PyObject_LookupSpecial(mgr, &_Py_ID(__exit__));
            if (exit == NULL) {
                if (!_PyErr_Occurred(tstate)) {
                    _PyErr_Format(tstate, PyExc_TypeError,
                                  "'%.200s' object does not support the "
                                  "context manager protocol "
                                  "(missed __exit__ method)",
                                  Py_TYPE(mgr)->tp_name);
                }
                Py_DECREF(enter);
                goto error;
            }
            SET_TOP(exit);
            Py_DECREF(mgr);
            res = _PyObject_CallNoArgs(enter);
            Py_DECREF(enter);
            if (res == NULL) {
                goto error;
            }
            PUSH(res);
        }

        inst(WITH_EXCEPT_START, (exit_func, lasti, unused, val -- exit_func, lasti, unused, val, res)) {
            /* At the top of the stack are 4 values:
               - val: TOP = exc_info()
               - unused: SECOND = previous exception
               - lasti: THIRD = lasti of exception in exc_info()
               - exit_func: FOURTH = the context.__exit__ bound method
               We call FOURTH(type(TOP), TOP, GetTraceback(TOP)).
               Then we push the __exit__ return value.
            */
            PyObject *exc, *tb;

            assert(val && PyExceptionInstance_Check(val));
            exc = PyExceptionInstance_Class(val);
            tb = PyException_GetTraceback(val);
            Py_XDECREF(tb);
            assert(PyLong_Check(lasti));
            (void)lasti; // Shut up compiler warning if asserts are off
            PyObject *stack[4] = {NULL, exc, val, tb};
            res = PyObject_Vectorcall(exit_func, stack + 1,
                    3 | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
            ERROR_IF(res == NULL, error);
        }

        // stack effect: ( -- __0)
        inst(PUSH_EXC_INFO) {
            PyObject *value = TOP();

            _PyErr_StackItem *exc_info = tstate->exc_info;
            if (exc_info->exc_value != NULL) {
                SET_TOP(exc_info->exc_value);
            }
            else {
                SET_TOP(Py_None);
            }

            PUSH(Py_NewRef(value));
            assert(PyExceptionInstance_Check(value));
            exc_info->exc_value = value;

        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_METHOD_WITH_VALUES) {
            /* Cached method object */
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            assert(type_version != 0);
            DEOPT_IF(self_cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(self_cls->tp_flags & Py_TPFLAGS_MANAGED_DICT);
            PyDictOrValues dorv = _PyObject_DictOrValues(self);
            DEOPT_IF(!_PyDictOrValues_IsValues(dorv), LOAD_ATTR);
            PyHeapTypeObject *self_heap_type = (PyHeapTypeObject *)self_cls;
            DEOPT_IF(self_heap_type->ht_cached_keys->dk_version !=
                     read_u32(cache->keys_version), LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            SET_TOP(Py_NewRef(res));
            PUSH(self);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_METHOD_NO_DICT) {
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(self_cls->tp_version_tag != type_version, LOAD_ATTR);
            assert(self_cls->tp_dictoffset == 0);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            SET_TOP(Py_NewRef(res));
            PUSH(self);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // error: LOAD_ATTR has irregular stack effect
        inst(LOAD_ATTR_METHOD_LAZY_DICT) {
            assert(cframe.use_tracing == 0);
            PyObject *self = TOP();
            PyTypeObject *self_cls = Py_TYPE(self);
            _PyLoadMethodCache *cache = (_PyLoadMethodCache *)next_instr;
            uint32_t type_version = read_u32(cache->type_version);
            DEOPT_IF(self_cls->tp_version_tag != type_version, LOAD_ATTR);
            Py_ssize_t dictoffset = self_cls->tp_dictoffset;
            assert(dictoffset > 0);
            PyObject *dict = *(PyObject **)((char *)self + dictoffset);
            /* This object has a __dict__, just not yet created */
            DEOPT_IF(dict != NULL, LOAD_ATTR);
            STAT_INC(LOAD_ATTR, hit);
            PyObject *res = read_obj(cache->descr);
            assert(res != NULL);
            assert(_PyType_HasFeature(Py_TYPE(res), Py_TPFLAGS_METHOD_DESCRIPTOR));
            SET_TOP(Py_NewRef(res));
            PUSH(self);
            JUMPBY(INLINE_CACHE_ENTRIES_LOAD_ATTR);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_BOUND_METHOD_EXACT_ARGS) {
            DEOPT_IF(is_method(stack_pointer, oparg), CALL);
            PyObject *function = PEEK(oparg + 1);
            DEOPT_IF(Py_TYPE(function) != &PyMethod_Type, CALL);
            STAT_INC(CALL, hit);
            PyObject *self = ((PyMethodObject *)function)->im_self;
            PEEK(oparg + 1) = Py_NewRef(self);
            PyObject *meth = ((PyMethodObject *)function)->im_func;
            PEEK(oparg + 2) = Py_NewRef(meth);
            Py_DECREF(function);
            GO_TO_INSTRUCTION(CALL_PY_EXACT_ARGS);
        }

        // stack effect: ( -- )
        inst(KW_NAMES) {
            assert(kwnames == NULL);
            assert(oparg < PyTuple_GET_SIZE(consts));
            kwnames = GETITEM(consts, oparg);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL) {
            _PyCallCache *cache = (_PyCallCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                int is_meth = is_method(stack_pointer, oparg);
                int nargs = oparg + is_meth;
                PyObject *callable = PEEK(nargs + 1);
                next_instr--;
                _Py_Specialize_Call(callable, next_instr, nargs, kwnames);
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(CALL, deferred);
            GO_TO_INSTRUCTION(CALL_GENERIC);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_GENERIC) {
            int total_args, is_meth;
            is_meth = is_method(stack_pointer, oparg);
            PyObject *function = PEEK(oparg + 1);
            if (!is_meth && Py_TYPE(function) == &PyMethod_Type) {
                PyObject *self = ((PyMethodObject *)function)->im_self;
                PEEK(oparg+1) = Py_NewRef(self);
                PyObject *meth = ((PyMethodObject *)function)->im_func;
                PEEK(oparg+2) = Py_NewRef(meth);
                Py_DECREF(function);
                is_meth = 1;
            }
            total_args = oparg + is_meth;
            function = PEEK(total_args + 1);
            int positional_args = total_args - KWNAMES_LEN();
            // Check if the call can be inlined or not
            if (Py_TYPE(function) == &PyFunction_Type &&
                tstate->interp->eval_frame == NULL &&
                ((PyFunctionObject *)function)->vectorcall == _PyFunction_Vectorcall)
            {
                int code_flags = ((PyCodeObject*)PyFunction_GET_CODE(function))->co_flags;
                PyObject *locals = code_flags & CO_OPTIMIZED ? NULL : Py_NewRef(PyFunction_GET_GLOBALS(function));
                STACK_SHRINK(total_args);
                _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit(
                    tstate, (PyFunctionObject *)function, locals,
                    stack_pointer, positional_args, kwnames
                );
                kwnames = NULL;
                STACK_SHRINK(2-is_meth);
                // The frame has stolen all the arguments from the stack,
                // so there is no need to clean them up.
                if (new_frame == NULL) {
                    goto error;
                }
                JUMPBY(INLINE_CACHE_ENTRIES_CALL);
                DISPATCH_INLINED(new_frame);
            }
            /* Callable is not a normal Python function */
            PyObject *res;
            if (cframe.use_tracing) {
                res = trace_call_function(
                    tstate, function, stack_pointer-total_args,
                    positional_args, kwnames);
            }
            else {
                res = PyObject_Vectorcall(
                    function, stack_pointer-total_args,
                    positional_args | PY_VECTORCALL_ARGUMENTS_OFFSET,
                    kwnames);
            }
            kwnames = NULL;
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            Py_DECREF(function);
            /* Clear the stack */
            STACK_SHRINK(total_args);
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            STACK_SHRINK(2-is_meth);
            PUSH(res);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_PY_EXACT_ARGS) {
            assert(kwnames == NULL);
            DEOPT_IF(tstate->interp->eval_frame, CALL);
            _PyCallCache *cache = (_PyCallCache *)next_instr;
            int is_meth = is_method(stack_pointer, oparg);
            int argcount = oparg + is_meth;
            PyObject *callable = PEEK(argcount + 1);
            DEOPT_IF(!PyFunction_Check(callable), CALL);
            PyFunctionObject *func = (PyFunctionObject *)callable;
            DEOPT_IF(func->func_version != read_u32(cache->func_version), CALL);
            PyCodeObject *code = (PyCodeObject *)func->func_code;
            DEOPT_IF(code->co_argcount != argcount, CALL);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), CALL);
            STAT_INC(CALL, hit);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, func, argcount);
            STACK_SHRINK(argcount);
            for (int i = 0; i < argcount; i++) {
                new_frame->localsplus[i] = stack_pointer[i];
            }
            STACK_SHRINK(2-is_meth);
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            DISPATCH_INLINED(new_frame);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_PY_WITH_DEFAULTS) {
            assert(kwnames == NULL);
            DEOPT_IF(tstate->interp->eval_frame, CALL);
            _PyCallCache *cache = (_PyCallCache *)next_instr;
            int is_meth = is_method(stack_pointer, oparg);
            int argcount = oparg + is_meth;
            PyObject *callable = PEEK(argcount + 1);
            DEOPT_IF(!PyFunction_Check(callable), CALL);
            PyFunctionObject *func = (PyFunctionObject *)callable;
            DEOPT_IF(func->func_version != read_u32(cache->func_version), CALL);
            PyCodeObject *code = (PyCodeObject *)func->func_code;
            DEOPT_IF(argcount > code->co_argcount, CALL);
            int minargs = cache->min_args;
            DEOPT_IF(argcount < minargs, CALL);
            DEOPT_IF(!_PyThreadState_HasStackSpace(tstate, code->co_framesize), CALL);
            STAT_INC(CALL, hit);
            _PyInterpreterFrame *new_frame = _PyFrame_PushUnchecked(tstate, func, code->co_argcount);
            STACK_SHRINK(argcount);
            for (int i = 0; i < argcount; i++) {
                new_frame->localsplus[i] = stack_pointer[i];
            }
            for (int i = argcount; i < code->co_argcount; i++) {
                PyObject *def = PyTuple_GET_ITEM(func->func_defaults,
                                                 i - minargs);
                new_frame->localsplus[i] = Py_NewRef(def);
            }
            STACK_SHRINK(2-is_meth);
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            DISPATCH_INLINED(new_frame);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_TYPE_1) {
            assert(kwnames == NULL);
            assert(cframe.use_tracing == 0);
            assert(oparg == 1);
            DEOPT_IF(is_method(stack_pointer, 1), CALL);
            PyObject *obj = TOP();
            PyObject *callable = SECOND();
            DEOPT_IF(callable != (PyObject *)&PyType_Type, CALL);
            STAT_INC(CALL, hit);
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            PyObject *res = Py_NewRef(Py_TYPE(obj));
            Py_DECREF(callable);
            Py_DECREF(obj);
            STACK_SHRINK(2);
            SET_TOP(res);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_STR_1) {
            assert(kwnames == NULL);
            assert(cframe.use_tracing == 0);
            assert(oparg == 1);
            DEOPT_IF(is_method(stack_pointer, 1), CALL);
            PyObject *callable = PEEK(2);
            DEOPT_IF(callable != (PyObject *)&PyUnicode_Type, CALL);
            STAT_INC(CALL, hit);
            PyObject *arg = TOP();
            PyObject *res = PyObject_Str(arg);
            Py_DECREF(arg);
            Py_DECREF(&PyUnicode_Type);
            STACK_SHRINK(2);
            SET_TOP(res);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_TUPLE_1) {
            assert(kwnames == NULL);
            assert(oparg == 1);
            DEOPT_IF(is_method(stack_pointer, 1), CALL);
            PyObject *callable = PEEK(2);
            DEOPT_IF(callable != (PyObject *)&PyTuple_Type, CALL);
            STAT_INC(CALL, hit);
            PyObject *arg = TOP();
            PyObject *res = PySequence_Tuple(arg);
            Py_DECREF(arg);
            Py_DECREF(&PyTuple_Type);
            STACK_SHRINK(2);
            SET_TOP(res);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_BUILTIN_CLASS) {
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            int kwnames_len = KWNAMES_LEN();
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyType_Check(callable), CALL);
            PyTypeObject *tp = (PyTypeObject *)callable;
            DEOPT_IF(tp->tp_vectorcall == NULL, CALL);
            STAT_INC(CALL, hit);
            STACK_SHRINK(total_args);
            PyObject *res = tp->tp_vectorcall((PyObject *)tp, stack_pointer,
                                              total_args-kwnames_len, kwnames);
            kwnames = NULL;
            /* Free the arguments. */
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            Py_DECREF(tp);
            STACK_SHRINK(1-is_meth);
            SET_TOP(res);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_BUILTIN_O) {
            assert(cframe.use_tracing == 0);
            /* Builtin METH_O functions */
            assert(kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            DEOPT_IF(total_args != 1, CALL);
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyCFunction_CheckExact(callable), CALL);
            DEOPT_IF(PyCFunction_GET_FLAGS(callable) != METH_O, CALL);
            STAT_INC(CALL, hit);
            PyCFunction cfunc = PyCFunction_GET_FUNCTION(callable);
            // This is slower but CPython promises to check all non-vectorcall
            // function calls.
            if (_Py_EnterRecursiveCallTstate(tstate, " while calling a Python object")) {
                goto error;
            }
            PyObject *arg = TOP();
            PyObject *res = _PyCFunction_TrampolineCall(cfunc, PyCFunction_GET_SELF(callable), arg);
            _Py_LeaveRecursiveCallTstate(tstate);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            Py_DECREF(arg);
            Py_DECREF(callable);
            STACK_SHRINK(2-is_meth);
            SET_TOP(res);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_BUILTIN_FAST) {
            assert(cframe.use_tracing == 0);
            /* Builtin METH_FASTCALL functions, without keywords */
            assert(kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyCFunction_CheckExact(callable), CALL);
            DEOPT_IF(PyCFunction_GET_FLAGS(callable) != METH_FASTCALL,
                CALL);
            STAT_INC(CALL, hit);
            PyCFunction cfunc = PyCFunction_GET_FUNCTION(callable);
            STACK_SHRINK(total_args);
            /* res = func(self, args, nargs) */
            PyObject *res = ((_PyCFunctionFast)(void(*)(void))cfunc)(
                PyCFunction_GET_SELF(callable),
                stack_pointer,
                total_args);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            /* Free the arguments. */
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            STACK_SHRINK(2-is_meth);
            PUSH(res);
            Py_DECREF(callable);
            if (res == NULL) {
                /* Not deopting because this doesn't mean our optimization was
                   wrong. `res` can be NULL for valid reasons. Eg. getattr(x,
                   'invalid'). In those cases an exception is set, so we must
                   handle it.
                */
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_BUILTIN_FAST_WITH_KEYWORDS) {
            assert(cframe.use_tracing == 0);
            /* Builtin METH_FASTCALL | METH_KEYWORDS functions */
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(!PyCFunction_CheckExact(callable), CALL);
            DEOPT_IF(PyCFunction_GET_FLAGS(callable) !=
                (METH_FASTCALL | METH_KEYWORDS), CALL);
            STAT_INC(CALL, hit);
            STACK_SHRINK(total_args);
            /* res = func(self, args, nargs, kwnames) */
            _PyCFunctionFastWithKeywords cfunc =
                (_PyCFunctionFastWithKeywords)(void(*)(void))
                PyCFunction_GET_FUNCTION(callable);
            PyObject *res = cfunc(
                PyCFunction_GET_SELF(callable),
                stack_pointer,
                total_args - KWNAMES_LEN(),
                kwnames
            );
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            kwnames = NULL;

            /* Free the arguments. */
            for (int i = 0; i < total_args; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            STACK_SHRINK(2-is_meth);
            PUSH(res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_LEN) {
            assert(cframe.use_tracing == 0);
            assert(kwnames == NULL);
            /* len(o) */
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            DEOPT_IF(total_args != 1, CALL);
            PyObject *callable = PEEK(total_args + 1);
            PyInterpreterState *interp = _PyInterpreterState_GET();
            DEOPT_IF(callable != interp->callable_cache.len, CALL);
            STAT_INC(CALL, hit);
            PyObject *arg = TOP();
            Py_ssize_t len_i = PyObject_Length(arg);
            if (len_i < 0) {
                goto error;
            }
            PyObject *res = PyLong_FromSsize_t(len_i);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            STACK_SHRINK(2-is_meth);
            SET_TOP(res);
            Py_DECREF(callable);
            Py_DECREF(arg);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_ISINSTANCE) {
            assert(cframe.use_tracing == 0);
            assert(kwnames == NULL);
            /* isinstance(o, o2) */
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyObject *callable = PEEK(total_args + 1);
            DEOPT_IF(total_args != 2, CALL);
            PyInterpreterState *interp = _PyInterpreterState_GET();
            DEOPT_IF(callable != interp->callable_cache.isinstance, CALL);
            STAT_INC(CALL, hit);
            PyObject *cls = POP();
            PyObject *inst = TOP();
            int retval = PyObject_IsInstance(inst, cls);
            if (retval < 0) {
                Py_DECREF(cls);
                goto error;
            }
            PyObject *res = PyBool_FromLong(retval);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));

            STACK_SHRINK(2-is_meth);
            SET_TOP(res);
            Py_DECREF(inst);
            Py_DECREF(cls);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_LIST_APPEND) {
            assert(cframe.use_tracing == 0);
            assert(kwnames == NULL);
            assert(oparg == 1);
            PyObject *callable = PEEK(3);
            PyInterpreterState *interp = _PyInterpreterState_GET();
            DEOPT_IF(callable != interp->callable_cache.list_append, CALL);
            PyObject *self = SECOND();
            DEOPT_IF(!PyList_Check(self), CALL);
            STAT_INC(CALL, hit);
            PyObject *arg = POP();
            PyListObject *list = (PyListObject *)self;
            int err;
            Py_BEGIN_CRITICAL_SECTION(list);
            err = _PyList_AppendTakeRef(list, arg);
            Py_END_CRITICAL_SECTION;
            if (err < 0) {
                goto error;
            }
            STACK_SHRINK(2);
            Py_DECREF(list);
            Py_DECREF(callable);
            // CALL + POP_TOP
            JUMPBY(INLINE_CACHE_ENTRIES_CALL + 1);
            assert(_Py_OPCODE(next_instr[-1]) == POP_TOP);
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_METHOD_DESCRIPTOR_O) {
            assert(kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyMethodDescrObject *callable =
                (PyMethodDescrObject *)PEEK(total_args + 1);
            DEOPT_IF(total_args != 2, CALL);
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            DEOPT_IF(meth->ml_flags != METH_O, CALL);
            PyObject *arg = TOP();
            PyObject *self = SECOND();
            DEOPT_IF(!Py_IS_TYPE(self, callable->d_common.d_type), CALL);
            STAT_INC(CALL, hit);
            PyCFunction cfunc = meth->ml_meth;
            // This is slower but CPython promises to check all non-vectorcall
            // function calls.
            if (_Py_EnterRecursiveCallTstate(tstate, " while calling a Python object")) {
                goto error;
            }
            PyObject *res = _PyCFunction_TrampolineCall(cfunc, self, arg);
            _Py_LeaveRecursiveCallTstate(tstate);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            Py_DECREF(self);
            Py_DECREF(arg);
            STACK_SHRINK(oparg + 1);
            SET_TOP(res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS) {
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyMethodDescrObject *callable =
                (PyMethodDescrObject *)PEEK(total_args + 1);
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            DEOPT_IF(meth->ml_flags != (METH_FASTCALL|METH_KEYWORDS), CALL);
            PyTypeObject *d_type = callable->d_common.d_type;
            PyObject *self = PEEK(total_args);
            DEOPT_IF(!Py_IS_TYPE(self, d_type), CALL);
            STAT_INC(CALL, hit);
            int nargs = total_args-1;
            STACK_SHRINK(nargs);
            _PyCFunctionFastWithKeywords cfunc =
                (_PyCFunctionFastWithKeywords)(void(*)(void))meth->ml_meth;
            PyObject *res = cfunc(self, stack_pointer, nargs - KWNAMES_LEN(),
                                  kwnames);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            kwnames = NULL;

            /* Free the arguments. */
            for (int i = 0; i < nargs; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            Py_DECREF(self);
            STACK_SHRINK(2-is_meth);
            SET_TOP(res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS) {
            assert(kwnames == NULL);
            assert(oparg == 0 || oparg == 1);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            DEOPT_IF(total_args != 1, CALL);
            PyMethodDescrObject *callable = (PyMethodDescrObject *)SECOND();
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            PyObject *self = TOP();
            DEOPT_IF(!Py_IS_TYPE(self, callable->d_common.d_type), CALL);
            DEOPT_IF(meth->ml_flags != METH_NOARGS, CALL);
            STAT_INC(CALL, hit);
            PyCFunction cfunc = meth->ml_meth;
            // This is slower but CPython promises to check all non-vectorcall
            // function calls.
            if (_Py_EnterRecursiveCallTstate(tstate, " while calling a Python object")) {
                goto error;
            }
            PyObject *res = _PyCFunction_TrampolineCall(cfunc, self, NULL);
            _Py_LeaveRecursiveCallTstate(tstate);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            Py_DECREF(self);
            STACK_SHRINK(oparg + 1);
            SET_TOP(res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // stack effect: (__0, __array[oparg] -- )
        inst(CALL_NO_KW_METHOD_DESCRIPTOR_FAST) {
            assert(kwnames == NULL);
            int is_meth = is_method(stack_pointer, oparg);
            int total_args = oparg + is_meth;
            PyMethodDescrObject *callable =
                (PyMethodDescrObject *)PEEK(total_args + 1);
            /* Builtin METH_FASTCALL methods, without keywords */
            DEOPT_IF(!Py_IS_TYPE(callable, &PyMethodDescr_Type), CALL);
            PyMethodDef *meth = callable->d_method;
            DEOPT_IF(meth->ml_flags != METH_FASTCALL, CALL);
            PyObject *self = PEEK(total_args);
            DEOPT_IF(!Py_IS_TYPE(self, callable->d_common.d_type), CALL);
            STAT_INC(CALL, hit);
            _PyCFunctionFast cfunc =
                (_PyCFunctionFast)(void(*)(void))meth->ml_meth;
            int nargs = total_args-1;
            STACK_SHRINK(nargs);
            PyObject *res = cfunc(self, stack_pointer, nargs);
            assert((res != NULL) ^ (_PyErr_Occurred(tstate) != NULL));
            /* Clear the stack of the arguments. */
            for (int i = 0; i < nargs; i++) {
                Py_DECREF(stack_pointer[i]);
            }
            Py_DECREF(self);
            STACK_SHRINK(2-is_meth);
            SET_TOP(res);
            Py_DECREF(callable);
            if (res == NULL) {
                goto error;
            }
            JUMPBY(INLINE_CACHE_ENTRIES_CALL);
            CHECK_EVAL_BREAKER();
        }

        // error: CALL_FUNCTION_EX has irregular stack effect
        inst(CALL_FUNCTION_EX) {
            PyObject *func, *callargs, *kwargs = NULL, *result;
            if (oparg & 0x01) {
                kwargs = POP();
                // DICT_MERGE is called before this opcode if there are kwargs.
                // It converts all dict subtypes in kwargs into regular dicts.
                assert(PyDict_CheckExact(kwargs));
            }
            callargs = POP();
            func = TOP();
            if (!PyTuple_CheckExact(callargs)) {
                if (check_args_iterable(tstate, func, callargs) < 0) {
                    Py_DECREF(callargs);
                    goto error;
                }
                Py_SETREF(callargs, PySequence_Tuple(callargs));
                if (callargs == NULL) {
                    goto error;
                }
            }
            assert(PyTuple_CheckExact(callargs));

            result = do_call_core(tstate, func, callargs, kwargs, cframe.use_tracing);
            Py_DECREF(func);
            Py_DECREF(callargs);
            Py_XDECREF(kwargs);

            STACK_SHRINK(1);
            assert(TOP() == NULL);
            SET_TOP(result);
            if (result == NULL) {
                goto error;
            }
            CHECK_EVAL_BREAKER();
        }

        // error: MAKE_FUNCTION has irregular stack effect
        inst(MAKE_FUNCTION) {
            PyObject *codeobj = POP();
            PyFunctionObject *func = (PyFunctionObject *)
                PyFunction_New(codeobj, GLOBALS());

            Py_DECREF(codeobj);
            if (func == NULL) {
                goto error;
            }

            if (oparg & 0x08) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_closure = POP();
            }
            if (oparg & 0x04) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_annotations = POP();
            }
            if (oparg & 0x02) {
                assert(PyDict_CheckExact(TOP()));
                func->func_kwdefaults = POP();
            }
            if (oparg & 0x01) {
                assert(PyTuple_CheckExact(TOP()));
                func->func_defaults = POP();
            }

            func->func_version = ((PyCodeObject *)codeobj)->co_version;
            PUSH((PyObject *)func);
        }

        // stack effect: ( -- )
        inst(RETURN_GENERATOR) {
            assert(PyFunction_Check(frame->f_funcobj));
            PyFunctionObject *func = (PyFunctionObject *)frame->f_funcobj;
            PyGenObject *gen = (PyGenObject *)_Py_MakeCoro(func);
            if (gen == NULL) {
                goto error;
            }
            assert(EMPTY());
            _PyFrame_SetStackPointer(frame, stack_pointer);
            _PyInterpreterFrame *gen_frame = (_PyInterpreterFrame *)gen->gi_iframe;
            _PyFrame_Copy(frame, gen_frame);
            assert(frame->frame_obj == NULL);
            gen->gi_frame_state = FRAME_CREATED;
            gen_frame->owner = FRAME_OWNED_BY_GENERATOR;
            _Py_LeaveRecursiveCallPy(tstate);
            assert(frame != &entry_frame);
            _PyInterpreterFrame *prev = frame->previous;
            _PyThreadState_PopFrame(tstate, frame);
            frame = cframe.current_frame = prev;
            _PyFrame_StackPush(frame, (PyObject *)gen);
            goto resume_frame;
        }

        // error: BUILD_SLICE has irregular stack effect
        inst(BUILD_SLICE) {
            PyObject *start, *stop, *step, *slice;
            if (oparg == 3)
                step = POP();
            else
                step = NULL;
            stop = POP();
            start = TOP();
            slice = PySlice_New(start, stop, step);
            Py_DECREF(start);
            Py_DECREF(stop);
            Py_XDECREF(step);
            SET_TOP(slice);
            if (slice == NULL)
                goto error;
        }

        // error: FORMAT_VALUE has irregular stack effect
        inst(FORMAT_VALUE) {
            /* Handles f-string value formatting. */
            PyObject *result;
            PyObject *fmt_spec;
            PyObject *value;
            PyObject *(*conv_fn)(PyObject *);
            int which_conversion = oparg & FVC_MASK;
            int have_fmt_spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC;

            fmt_spec = have_fmt_spec ? POP() : NULL;
            value = POP();

            /* See if any conversion is specified. */
            switch (which_conversion) {
            case FVC_NONE:  conv_fn = NULL;           break;
            case FVC_STR:   conv_fn = PyObject_Str;   break;
            case FVC_REPR:  conv_fn = PyObject_Repr;  break;
            case FVC_ASCII: conv_fn = PyObject_ASCII; break;
            default:
                _PyErr_Format(tstate, PyExc_SystemError,
                              "unexpected conversion flag %d",
                              which_conversion);
                goto error;
            }

            /* If there's a conversion function, call it and replace
               value with that result. Otherwise, just use value,
               without conversion. */
            if (conv_fn != NULL) {
                result = conv_fn(value);
                Py_DECREF(value);
                if (result == NULL) {
                    Py_XDECREF(fmt_spec);
                    goto error;
                }
                value = result;
            }

            /* If value is a unicode object, and there's no fmt_spec,
               then we know the result of format(value) is value
               itself. In that case, skip calling format(). I plan to
               move this optimization in to PyObject_Format()
               itself. */
            if (PyUnicode_CheckExact(value) && fmt_spec == NULL) {
                /* Do nothing, just transfer ownership to result. */
                result = value;
            } else {
                /* Actually call format(). */
                result = PyObject_Format(value, fmt_spec);
                Py_DECREF(value);
                Py_XDECREF(fmt_spec);
                if (result == NULL) {
                    goto error;
                }
            }

            PUSH(result);
        }

        // stack effect: ( -- __0)
        inst(COPY) {
            assert(oparg != 0);
            PyObject *peek = PEEK(oparg);
            PUSH(Py_NewRef(peek));
        }

        inst(BINARY_OP, (unused/1, lhs, rhs -- unused)) {
            _PyBinaryOpCache *cache = (_PyBinaryOpCache *)next_instr;
            if (DECREMENT_ADAPTIVE_COUNTER(&cache->counter)) {
                _PyMutex_lock(&_PyRuntime.mutex);
                assert(cframe.use_tracing == 0);
                next_instr--;
                _Py_Specialize_BinaryOp(lhs, rhs, next_instr, oparg, &GETLOCAL(0));
                _PyMutex_unlock(&_PyRuntime.mutex);
                DISPATCH_SAME_OPARG();
            }
            STAT_INC(BINARY_OP, deferred);
            GO_TO_INSTRUCTION(BINARY_OP_GENERIC);
        }

        inst(BINARY_OP_GENERIC, (unused/1, lhs, rhs -- res)) {
            assert(0 <= oparg);
            assert((unsigned)oparg < Py_ARRAY_LENGTH(binary_ops));
            assert(binary_ops[oparg]);
            res = binary_ops[oparg](lhs, rhs);
            Py_DECREF(lhs);
            Py_DECREF(rhs);
            ERROR_IF(res == NULL, error);
        }

        // stack effect: ( -- )
        inst(SWAP) {
            assert(oparg != 0);
            PyObject *top = TOP();
            SET_TOP(PEEK(oparg));
            PEEK(oparg) = top;
        }

        // stack effect: ( -- )
        inst(EXTENDED_ARG) {
            assert(oparg);
            assert(cframe.use_tracing == 0);
            opcode = _Py_OPCODE(*next_instr);
            oparg = oparg << 8 | _Py_OPARG(*next_instr);
            PRE_DISPATCH_GOTO();
            DISPATCH_GOTO();
        }

        // stack effect: ( -- )
        inst(CACHE) {
            Py_UNREACHABLE();
        }


// END BYTECODES //

    }
 error:
 exception_unwind:
 handle_eval_breaker:
 resume_frame:
 resume_with_error:
 start_frame:
 unbound_local_error:
    ;
}

// Future families go below this point //

family(call) = {
    CALL, CALL_GENERIC, CALL_PY_EXACT_ARGS,
    CALL_PY_WITH_DEFAULTS, CALL_BOUND_METHOD_EXACT_ARGS, CALL_BUILTIN_CLASS,
    CALL_BUILTIN_FAST_WITH_KEYWORDS, CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS, CALL_NO_KW_BUILTIN_FAST,
    CALL_NO_KW_BUILTIN_O, CALL_NO_KW_ISINSTANCE, CALL_NO_KW_LEN,
    CALL_NO_KW_LIST_APPEND, CALL_NO_KW_METHOD_DESCRIPTOR_FAST, CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS,
    CALL_NO_KW_METHOD_DESCRIPTOR_O, CALL_NO_KW_STR_1, CALL_NO_KW_TUPLE_1,
    CALL_NO_KW_TYPE_1 };
family(for_iter) = {
    FOR_ITER, FOR_ITER_GENERIC, FOR_ITER_LIST,
    FOR_ITER_RANGE };
family(load_attr) = {
    LOAD_ATTR, LOAD_ATTR_GENERIC, LOAD_ATTR_CLASS,
    LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN, LOAD_ATTR_INSTANCE_VALUE, LOAD_ATTR_MODULE,
    LOAD_ATTR_PROPERTY, LOAD_ATTR_SLOT, LOAD_ATTR_WITH_HINT,
    LOAD_ATTR_METHOD_LAZY_DICT, LOAD_ATTR_METHOD_NO_DICT,
    LOAD_ATTR_METHOD_WITH_VALUES };
family(load_global) = {
    LOAD_GLOBAL, LODA_GLOBAL_GENERIC, LOAD_GLOBAL_BUILTIN,
    LOAD_GLOBAL_MODULE };
family(store_fast) = { STORE_FAST, STORE_FAST__LOAD_FAST, STORE_FAST__STORE_FAST };
family(unpack_sequence) = {
    UNPACK_SEQUENCE, UNPACK_SEQUENCE_GENERIC, UNPACK_SEQUENCE_LIST,
    UNPACK_SEQUENCE_TUPLE, UNPACK_SEQUENCE_TWO_TUPLE };
