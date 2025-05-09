/* Base continuation object implementation */

#include "Python.h"

#include "opcode_ids.h"
#include "pycore_pyerrors.h"      // _PyErr_ClearExcState()
#include "pystate.h"
#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_interpframe.h"
#include "pycore_object.h"
#include "pycore_pystate.h"       // _PyThreadState_GET()()
#include "pycore_modsupport.h"    // _PyArg_CheckPositional()
#include "pycore_stackref.h"

#include "pystats.h"

static void
continuation_dealloc(PyObject *self)
{
    PyContinuationObject *cont = (PyContinuationObject *)self;
    _PyObject_GC_UNTRACK(self);
    if (cont->cont_weakreflist != NULL) {
        PyObject_ClearWeakRefs(self);
    }
    _PyObject_GC_TRACK(self);
    if (PyObject_CallFinalizerFromDealloc(self)) {
        // resurrected.
        return;
    }
    _PyObject_GC_UNTRACK(self);
    PyObject_GC_Del(self);
}

static PyObject *
continuation_repr(PyObject *self)
{
    return PyUnicode_FromFormat("<Continuation object %llu>",
                                ((PyContinuationObject *)self)->id);
}

static void
set_stack_chunk(PyThreadState *tstate, _PyStackChunk *chunk)
{
    assert(tstate->datastack_chunk != NULL);
    // Save top
    tstate->datastack_chunk->top = tstate->datastack_top -
                                    &tstate->datastack_chunk->data[0];
    assert(tstate->datastack_chunk->size == (uintptr_t)(((char *)tstate->datastack_limit) - ((char *)tstate->datastack_chunk)));
    tstate->datastack_chunk = chunk;
    assert(chunk != NULL);
    tstate->datastack_top = &chunk->data[chunk->top];
    tstate->datastack_limit = (PyObject **)(((char *)chunk) + chunk->size);
}

static int
computed_stack_depth(PyContinuationObject *cont) {
    int depth = 1;
    _PyInterpreterFrame *f = cont->current_frame;
    while (f != cont->root_frame) {
        if (f->owner < FRAME_OWNED_BY_INTERPRETER) {
            depth++;
        }
        f = f->previous;
    }
    assert(cont->root_frame->owner < FRAME_OWNED_BY_INTERPRETER);
    return depth;
}


extern PyObject *_PyEval_EvalFrames(PyThreadState *tstate, _PyInterpreterFrame *base, _PyInterpreterFrame *top, int throwflag);

/* Remove this */
extern int get_recursion_depth(_PyInterpreterFrame *f);

void
_Py_Continuation_Attach(PyThreadState *tstate, PyContinuationObject *cont)
{
    assert(cont->stack_depth == computed_stack_depth(cont));
    assert(cont->root_frame->previous == NULL);
    int start_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
    assert(start_depth == get_recursion_depth(tstate->current_frame));
    tstate->py_recursion_remaining -= cont->stack_depth;
    cont->depth_before_root = start_depth;
    cont->stack_depth = -1;
    cont->root_frame->previous = tstate->current_frame;
    tstate->current_frame = cont->current_frame;
    tstate->current_continuation = cont;
    cont->root_chunk->previous = tstate->datastack_chunk;
    set_stack_chunk(tstate, cont->top_chunk);
    assert(cont->depth_before_root == get_recursion_depth(cont->root_frame->previous));
    assert(tstate->py_recursion_limit - tstate->py_recursion_remaining == get_recursion_depth(tstate->current_frame));
    cont->executing = 1;
}

void
_Py_Continuation_Detach(PyThreadState *tstate)
{
    assert(tstate->py_recursion_limit - tstate->py_recursion_remaining == get_recursion_depth(tstate->current_frame));
    PyContinuationObject *cont = tstate->current_continuation;
    int current_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
    assert(current_depth == get_recursion_depth(tstate->current_frame));
    assert(cont->depth_before_root == get_recursion_depth(cont->root_frame->previous));
    cont->stack_depth = current_depth - cont->depth_before_root;
    tstate->py_recursion_remaining = tstate->py_recursion_limit - cont->depth_before_root;
    cont->depth_before_root = -1;
    cont->top_chunk = tstate->datastack_chunk;
    set_stack_chunk(tstate, cont->root_chunk->previous);
    cont->root_chunk->previous = NULL;
    cont->current_frame = tstate->current_frame;
    tstate->current_frame = cont->root_frame->previous;
    cont->root_frame->previous = NULL;
    tstate->current_continuation = NULL;
    assert(cont->stack_depth == computed_stack_depth(cont));
    assert(tstate->py_recursion_limit - tstate->py_recursion_remaining == get_recursion_depth(tstate->current_frame));
    cont->executing = 0;
    Py_DECREF(cont);
}

extern _PyStackChunk*
_Py_PushChunk(PyThreadState *tstate,  size_t size);

extern _PyInterpreterFrame *
_PyEvalFramePushAndInit_Ex(PyThreadState *tstate, _PyStackRef func,
    PyObject *locals, Py_ssize_t nargs, PyObject *callargs, PyObject *kwargs, _PyInterpreterFrame *previous);


PyObject *
continuation_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    PyContinuationObject *cont = (PyContinuationObject *)_PyObject_GC_New(type);
    if (cont == NULL) {
        return PyErr_NoMemory();
    }
    cont->top_chunk = NULL;
    cont->root_chunk = NULL;
    cont->current_frame = NULL;
    cont->exc_info = NULL;
    cont->root_exc_info = NULL;
    cont->root_frame = NULL;
    cont->cont_weakreflist = NULL;
    cont->started = 0;
    cont->executing = 0;
    cont->completed = 0;
    cont->stack_depth = -1;
    cont->depth_before_root = -1;
    PyObject *func;
    static char *kwlist[] = {"func", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist,
        &func)) {
        Py_DECREF(cont);
        return NULL;
    }
    cont->func = Py_NewRef(func);
    return (PyObject *)cont;
}

static PyObject *
run(PyThreadState *tstate, PyContinuationObject *cont, PyObject *value)
{
    assert(tstate->py_recursion_limit - tstate->py_recursion_remaining == get_recursion_depth(tstate->current_frame));
    cont->started = 1;
    _PyInterpreterFrame *start = tstate->current_frame;
    _PyStackChunk *chunk = tstate->datastack_chunk;
    assert(start != NULL);
    assert(tstate->current_continuation == NULL);
    Py_INCREF(cont);
    tstate->current_continuation = cont;
    cont->root_chunk->previous = tstate->datastack_chunk;
    assert(cont->stack_depth > 0);
    if (value == NULL) { assert(cont->stack_depth == 1); }
    assert(cont->stack_depth == computed_stack_depth(cont));
    int start_depth = tstate->py_recursion_limit - tstate->py_recursion_remaining;
    assert(start_depth == get_recursion_depth(tstate->current_frame));
    tstate->py_recursion_remaining -= cont->stack_depth;
    cont->depth_before_root = start_depth;
    cont->stack_depth = -1;
    set_stack_chunk(tstate, cont->top_chunk);
    cont->executing = 1;
    _PyInterpreterFrame *frame = cont->current_frame;
    PyObject *result;
    if (value != NULL) {
        _PyStackRef ref = PyStackRef_FromPyObjectNew(value);
        _PyFrame_StackPush(frame, ref);
        frame->instr_ptr += frame->return_offset;
        frame->return_offset = 0;
        tstate->py_recursion_remaining++;
        result = _PyEval_EvalFrames(tstate, cont->root_frame, cont->current_frame, 0);
    }
    else {
        tstate->py_recursion_remaining++;
        result = _PyEval_EvalFrame(tstate, frame, 0);
    }
    assert(tstate->py_recursion_limit - tstate->py_recursion_remaining == get_recursion_depth(tstate->current_frame));
    if (tstate->current_continuation == NULL) {
        // Detached.
        assert(tstate->datastack_chunk == chunk);
    }
    else {
        cont = tstate->current_continuation;
        assert(cont->executing);
        cont->executing = 0;
        cont->completed = 1;
        set_stack_chunk(tstate, chunk);
        tstate->current_continuation = NULL;
    }
    assert(tstate->current_frame == start);
    assert(tstate->py_recursion_limit - tstate->py_recursion_remaining == get_recursion_depth(tstate->current_frame));
    assert(tstate->current_continuation == NULL);
    return result;

}

static PyObject *
continuation_start(PyObject *self, PyObject *args, PyObject *kwargs)
{
    assert(PyTuple_CheckExact(args));
    assert(kwargs == NULL || PyDict_CheckExact(kwargs));
    // TO DO -- Check that continuation is actually a Continuation.
    // if (Py_TYPE(continuation) != &PyContinuation_Type) {
    //     PyErr_SetString(PyExc_TypeError, "Expected a continuation");
    //     return -1;
    // }
    PyThreadState *tstate = PyThreadState_GET();
    PyContinuationObject *cont = (PyContinuationObject *)self;
    if (cont->started) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot start an already started continuation");
        return NULL;
    }
    PyObject *func;
    func = PyObject_GetAttrString((PyObject *)Py_TYPE(self), "_start");
    if (func == NULL) {
        return NULL;
    }
    assert(PyFunction_Check(func));    _PyStackChunk *temp = tstate->datastack_chunk;
    PyFunctionObject *func_obj = (PyFunctionObject *)func;
    PyCodeObject *code = (PyCodeObject *)func_obj->func_code;
    _PyStackChunk *chunk = _Py_PushChunk(tstate, code->co_framesize);
    if (chunk == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    PyObject *self_arg = PyTuple_Pack(1, (PyObject *)cont);
    if (self_arg == NULL) {
        return NULL;
    }
    PyObject *callargs = PyTuple_Type.tp_as_sequence->sq_concat(self_arg, args);
    Py_DECREF(self_arg);
    if (callargs == NULL) {
        return NULL;
    }
    cont->top_chunk = cont->root_chunk = chunk;
    tstate->current_continuation = cont;
    assert(cont->root_chunk->previous == temp);
    _PyInterpreterFrame *top_frame = tstate->current_frame;
    Py_XINCREF(kwargs);
    _PyStackRef func_st = PyStackRef_FromPyObjectSteal(func);
    Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
    _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit_Ex(
                        tstate, func_st, NULL,
                        nargs, callargs, kwargs, NULL);
    tstate->current_frame = top_frame;
    tstate->current_continuation = NULL;
    if (new_frame == NULL) {
        return NULL;
    }
    cont->current_frame = cont->root_frame = new_frame;
    cont->stack_depth = 1;
    assert(cont->stack_depth == computed_stack_depth(cont));
    cont->top_chunk = cont->root_chunk = tstate->datastack_chunk;
    cont->root_chunk->previous = NULL;
    set_stack_chunk(tstate, temp);
    if (new_frame == NULL) {
        return NULL;
    }
    new_frame->previous = NULL;
    assert(cont->root_frame == cont->current_frame);
    assert(cont->root_frame != NULL);
    return run(tstate, cont, NULL);
}

static PyObject *
continuation_run(PyObject *self, PyObject *arg)
{
    // TO DO -- Check that continuation is actually a Continuation.
    // if (Py_TYPE(continuation) != &PyContinuation_Type) {
    //     PyErr_SetString(PyExc_TypeError, "Expected a continuation");
    //     return -1;
    // }
    PyThreadState *tstate = PyThreadState_GET();
    PyContinuationObject *cont = (PyContinuationObject *)self;
    assert(cont->root_frame != NULL);
    if (!cont->started) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot run an unstarted continuation");
        return NULL;
    }
    return run(tstate, cont, arg);
}

static PyMethodDef continuation_methods[] = {
    {"start", _PyCFunction_CAST(continuation_start), METH_VARARGS | METH_KEYWORDS, NULL },
    {"run", _PyCFunction_CAST(continuation_run), METH_O, NULL },
    // {"close", _PyCFunction_CAST(continuation_close), METH_NOARGS, NULL},
    {NULL, NULL}        /* Sentinel */
};

static int
continuation_traverse(PyObject *self, visitproc visit, void *arg)
{
    PyContinuationObject *cont = (PyContinuationObject *)self;
    if (!cont->started || cont->completed) {
        return 0;
    }
    for (_PyInterpreterFrame *frame = cont->current_frame; frame != NULL; frame = frame->previous) {
        if (frame->owner == FRAME_OWNED_BY_THREAD) {
            if (_PyFrame_Traverse(frame, visit, arg)) {
                return -1;
            }
        }
    }
    return 0;
}

static int
continuation_clear(PyObject *self)
{
    // TO DO -- raise an exception to unwind the continuation.

    PyContinuationObject *cont = (PyContinuationObject *)self;
    assert (!cont->executing);
    if (!cont->started || cont->completed) {
        return 0;
    }
    for (_PyInterpreterFrame *frame = cont->current_frame; frame != NULL; frame = frame->previous) {
        if (frame->owner == FRAME_OWNED_BY_THREAD) {
            _PyFrame_ClearLocals(frame);
        }
    }
    cont->completed = 0;
    return 0;
}

static PyObject *
continuation_close(PyObject *self)
{
    PyContinuationObject *cont = (PyContinuationObject *)self;
    if (cont->executing) {
        char *msg = "Cannot close running continuation";
        PyErr_SetString(PyExc_ValueError, msg);
        return NULL;
    }
    continuation_clear(self);
    Py_RETURN_NONE;
}

static void
continuation_finalize(PyObject *self)
{
    PyObject *result = continuation_close(self);
    if (result == NULL) {
        assert(PyErr_Occurred());
        PyErr_WriteUnraisable(self);
    }
}

static PyMemberDef continuation_members[] = {
    { "started", Py_T_BOOL, offsetof(PyContinuationObject, started), Py_READONLY },
    { "executing", Py_T_BOOL, offsetof(PyContinuationObject, executing), Py_READONLY },
    { "completed", Py_T_BOOL, offsetof(PyContinuationObject, completed), Py_READONLY },
    { "_func", Py_T_OBJECT_EX, offsetof(PyContinuationObject, func), Py_READONLY },
    { NULL }
};

PyTypeObject PyContinuation_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "_Continuation",
    .tp_basicsize = sizeof(PyContinuationObject),
    .tp_flags = _Py_TPFLAGS_STATIC_BUILTIN | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_IMMUTABLETYPE | Py_TPFLAGS_HAVE_GC,
    .tp_new = continuation_new,
    .tp_dealloc = continuation_dealloc,
    .tp_repr = continuation_repr,
    .tp_methods = continuation_methods,
    .tp_finalize = continuation_finalize,
    .tp_traverse = continuation_traverse,
    .tp_clear = continuation_clear,
    .tp_members = continuation_members,
};


static int
continuations_exec(PyObject *module) {
    PyInterpreterState *interp = PyInterpreterState_Get();
    if (_PyStaticType_InitForExtension(interp, &PyContinuation_Type) < 0) {
        return -1;
    }
    if (PyModule_AddType(module, &PyContinuation_Type) < 0) {
        return -1;
    }
   return 0;
}

static PyObject *
get_current_continuation(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (tstate->current_continuation == NULL) {
        Py_RETURN_NONE;
    }
    return Py_NewRef(tstate->current_continuation);
}

static PyMethodDef continuations_methods[] = {
    { "get_current_continuation", (PyCFunction)get_current_continuation, METH_NOARGS, NULL },
    {NULL,      NULL}        /* Sentinel */
};

static struct PyModuleDef_Slot continuations_slots[] = {
    {Py_mod_exec, continuations_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef _continuationsmodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_continuations",
    .m_methods = continuations_methods,
    .m_slots = continuations_slots,
};

PyMODINIT_FUNC
PyInit__continuations(void)
{
    return PyModuleDef_Init(&_continuationsmodule);
}
