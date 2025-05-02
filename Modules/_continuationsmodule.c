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
/*
static PyObject *
continuation_close(PyObject *self)
{
    PyContinuationObject *cont = (PyContinuationObject *)self;
    if (cont->executing) {
        char *msg = "Cannot close running continuation";
        PyErr_SetString(PyExc_ValueError, msg);
        return NULL;
    }
    PyThreadState *tstate = PyThreadState_GET();
    attach(cont, tstate);
    PyErr_SetNone(PyExc_ContinuationExit);
    PyObject *result = _PyEval_EvalFrame(tstate, tstate->current_frame, 1);
    detach(cont, tstate);
    if (result == NULL && PyErr_ExceptionMatches(PyExc_ContinuationExit) {
        PyErr_Clear();
        result = Py_None;
    }
    return result;
}*/

static PyObject *
continuation_repr(PyObject *self)
{
    return PyUnicode_FromFormat("<Continuation object %llu>",
                                ((PyContinuationObject *)self)->id);
}

static void
continuation_finalize(PyObject *self)
{
//     PyObject *result = continuation_close(self);
//     if (result == NULL) {
//         assert(PyErr_Occurred());
//         PyErr_WriteUnraisable(self);
//     }
//     else {
//         Py_DECREF(result);
//     }
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

static void
attach(PyThreadState *tstate, PyContinuationObject *cont)
{
    assert(tstate->current_continuation == NULL);
    cont->root_frame->previous = tstate->current_frame;
    tstate->current_frame = cont->current_frame;
    tstate->current_continuation = cont;
    cont->root_chunk->previous = tstate->datastack_chunk;
    set_stack_chunk(tstate, cont->top_chunk);
}

static void
detach(PyThreadState *tstate, PyContinuationObject *cont)
{
    cont->top_chunk = tstate->datastack_chunk;
    set_stack_chunk(tstate, cont->root_chunk->previous);
    cont->root_chunk->previous = NULL;
    cont->current_frame = tstate->current_frame;
    tstate->current_frame = cont->root_frame->previous;
    cont->root_frame->previous = NULL;
    tstate->current_continuation = NULL;
}

extern _PyStackChunk*
_Py_PushChunk(PyThreadState *tstate,  size_t size);

extern _PyInterpreterFrame *
_PyEvalFramePushAndInit_Ex(PyThreadState *tstate, _PyStackRef func,
    PyObject *locals, Py_ssize_t nargs, PyObject *callargs, PyObject *kwargs, _PyInterpreterFrame *previous);

int
_Py_InitContinuation(PyContinuationObject *cont, PyObject *func, PyObject *args, PyObject *kwargs)
{
    if (cont->root_frame != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Continuation has already been started");
        return -1;
    }
    if (!PyTuple_CheckExact(args)) {
        PyErr_SetString(PyExc_TypeError, "args must be a tuple");
        return -1;
    }
    if (kwargs != NULL && !PyDict_CheckExact(kwargs)) {
        PyErr_SetString(PyExc_TypeError, "kwargs must be a dict");
        return -1;
    }
    PyThreadState *tstate = PyThreadState_GET();
    _PyStackChunk *temp = tstate->datastack_chunk;
    PyFunctionObject *func_obj = (PyFunctionObject *)func;
    PyCodeObject *code = (PyCodeObject *)func_obj->func_code;
    _PyStackChunk *chunk = _Py_PushChunk(tstate, code->co_framesize);
    if (chunk == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    cont->top_chunk = cont->root_chunk = chunk;
    tstate->current_continuation = cont;
    assert(cont->root_chunk->previous == temp);
    _PyInterpreterFrame *top_frame = tstate->current_frame;
    Py_INCREF(args);
    Py_XINCREF(kwargs);
    _PyStackRef func_st = PyStackRef_FromPyObjectNew(func);
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    _PyInterpreterFrame *new_frame = _PyEvalFramePushAndInit_Ex(
                        tstate, func_st, NULL,
                        nargs, args, kwargs, NULL);
    cont->current_frame = cont->root_frame = new_frame;
    cont->top_chunk = cont->root_chunk = tstate->datastack_chunk;
    cont->root_chunk->previous = NULL;
    set_stack_chunk(tstate, temp);
    tstate->current_frame = top_frame;
    tstate->current_continuation = NULL;
    if (new_frame == NULL) {
        return -1;
    }
    new_frame->previous = NULL;
    return 0;
}

int
_Py_ResumeContinuation(PyThreadState *tstate, PyObject *continuation, _PyInterpreterFrame *entry_frame)
{
    // TO DO -- Check that continuation is actually a Continuation.
    // if (Py_TYPE(continuation) != &PyContinuation_Type) {
    //     PyErr_SetString(PyExc_TypeError, "Expected a continuation");
    //     return -1;
    // }
    PyContinuationObject *cont = (PyContinuationObject *)continuation;
    assert(cont->root_frame != NULL);
    PyContinuationObject *prev = tstate->current_continuation;
    if (prev == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot resume a continuation from outside a continuation");
        return -1;
    }
    if (prev->root_frame->previous != entry_frame) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot resume continuation across builtin function call");
        return -1;
    }
    if (cont->completed) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot resume a completed continuation");
        return -1;
    }
    Py_INCREF(cont);
    detach(tstate, prev);
    attach(tstate, cont);
    Py_DECREF(prev);
    return 0;
}

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
    PyObject *func;
    PyObject *callargs;
    PyObject *kwargs;
    static char *kwlist[] = {"func", "args", "kwrgs", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "OOO", kwlist,
        &func, &callargs, &kwargs)) {
        return NULL;
    }
    if (_Py_InitContinuation(cont, func, callargs, kwargs) < 0) {
        Py_DECREF(cont);
        return NULL;
    }
    return (PyObject *)cont;
}

extern PyObject *_PyEval_EvalFrames(PyThreadState *tstate, _PyInterpreterFrame *base, _PyInterpreterFrame *top, int throwflag);


static PyObject *
run(PyThreadState *tstate, PyContinuationObject *cont, PyObject *value)
{
    cont->started = 1;
    _PyInterpreterFrame *start = tstate->current_frame;
    _PyStackChunk *chunk = tstate->datastack_chunk;
    assert(start != NULL);
    Py_INCREF(cont);
    tstate->current_continuation = cont;
    cont->root_chunk->previous = tstate->datastack_chunk;
    set_stack_chunk(tstate, cont->top_chunk);
    PyContinuationObject *prev = tstate->current_continuation;
    cont->executing = 1;
    _PyInterpreterFrame *frame = cont->current_frame;
    PyObject *result;
    if (value != NULL) {
        _PyStackRef ref = PyStackRef_FromPyObjectNew(value);
        _PyFrame_StackPush(frame, ref);
        frame->instr_ptr += frame->return_offset;
        frame->return_offset = 0;
        result = _PyEval_EvalFrames(tstate, cont->root_frame, cont->current_frame, 0);
    }
    else {
        result = _PyEval_EvalFrame(tstate, frame, 0);
    }
    set_stack_chunk(tstate, chunk);
    tstate->current_frame = start;
    cont = tstate->current_continuation;
    if (cont->executing) {
        cont->executing = 0;
        cont->completed = 1;
    }
    tstate->current_continuation = prev;
    Py_DECREF(cont);
    return result;

}

static PyObject *
continuation_start(PyObject *self, PyObject *Py_UNUSED(unused))
{
    // TO DO -- Check that continuation is actually a Continuation.
    // if (Py_TYPE(continuation) != &PyContinuation_Type) {
    //     PyErr_SetString(PyExc_TypeError, "Expected a continuation");
    //     return -1;
    // }
    PyThreadState *tstate = PyThreadState_GET();
    PyContinuationObject *cont = (PyContinuationObject *)self;
    assert(cont->root_frame == cont->current_frame);
    assert(cont->root_frame != NULL);
    if (cont->started) {
        PyErr_SetString(PyExc_RuntimeError, "Cannot start an already started continuation");
        return NULL;
    }
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
    {"start", _PyCFunction_CAST(continuation_start), METH_NOARGS, NULL },
    {"run", _PyCFunction_CAST(continuation_run), METH_O, NULL },
    // {"close", _PyCFunction_CAST(continuation_close), METH_NOARGS, NULL},
    {NULL, NULL}        /* Sentinel */
};

static int
continuation_traverse(PyObject *self, visitproc visit, void *arg)
{
    // TO DO ...
    return 0;
}

static PyMemberDef continuation_members[] = {
    { "started", Py_T_BOOL, offsetof(PyContinuationObject, started), Py_READONLY },
    { "executing", Py_T_BOOL, offsetof(PyContinuationObject, executing), Py_READONLY },
    { "completed", Py_T_BOOL, offsetof(PyContinuationObject, completed), Py_READONLY },
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
