/* Generator object implementation */

#define _PY_INTERPRETER

#include "Python.h"

#include "pycore_pyerrors.h"      // _PyErr_ClearExcState()
#include "pystate.h"
#include "pycore_call.h"
#include "pycore_object.h"
#include "pycore_pystate.h"       // _PyThreadState_GET()

#include "pystats.h"

static uint64_t next_continuation_id;

typedef struct _continuation {
    PyObject_HEAD

    PyStack stack;
    uint64_t id;
    uint64_t instrumentation_version;
    _PyInterpreterFrame *top_frame;
    _PyInterpreterFrame *root_frame;
    _PyStackChunk *datastack_top;
    _PyStackChunk *datastack_root;
    PyObject *cont_weakreflist;
    int executing;
} PyContinuationObject;


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

static void
attach(PyContinuationObject *cont, PyThreadState *tstate)
{
    assert(cont->executing == 0);
    cont->executing = 1;
    cont->root_frame->previous = tstate->current_frame;
    tstate->current_frame = cont->top_frame;
    cont->datastack_root->previous = tstate->datastack_chunk;
    tstate->datastack_chunk = cont->datastack_top;
}

static void
detach(PyContinuationObject *cont, PyThreadState *tstate)
{
    assert(cont->executing == 1);
    cont->top_frame = tstate->current_frame;
    tstate->current_frame = cont->root_frame->previous;
    cont->root_frame->previous = NULL;
    cont->datastack_top = tstate->datastack_chunk;
    tstate->datastack_chunk = cont->datastack_root->previous;
    cont->datastack_root->previous = NULL;
    cont->executing = 0;
}


static PyObject *
continuation_throw(PyObject *self, PyObject *arg)
{
    PyContinuationObject *cont = (PyContinuationObject *)self;
    PyThreadState *tstate = PyThreadState_GET();
    // Attach continuation to current thread and throw into it.
    if (PyExceptionClass_Check(arg)) {
        arg = _PyObject_CallNoArgs(arg);
        if (arg == NULL) {
            return NULL;
        }
    }
    else if (PyExceptionInstance_Check(arg)) {
        Py_INCREF(arg);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "exceptions must be classes or instances "
                     "deriving from BaseException, not %s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    attach(cont, tstate);
    PyErr_SetRaisedException(arg);
    PyObject *result = _PyEval_EvalFrame(tstate, tstate->stack.frame, 1);
    detach(cont, tstate);
    return result;
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
    PyThreadState *tstate = PyThreadState_GET();
    attach(cont, tstate);
    PyErr_SetNone(PyExc_ContinuationExit);
    PyObject *result = _PyEval_EvalFrame(tstate, tstate->stack.current_frame, 1);
    detach(cont, tstate);
    if (result == NULL && PyErr_ExceptionMatches(PyExc_ContinuationExit) {
        PyErr_Clear();
        result = Py_None;
    }
    return result;
}

static PyObject *
continuation_repr(PyObject *self)
{
    return PyUnicode_FromFormat("<continuation object %ll>",
                                ((PyContinuationObject *)self)->id);
}

static void
continuation_finalize(PyObject *self)
{
    PyObject *result = continuation_close(self);
    if (result == NULL) {
        assert(PyErr_Occurred());
        PyErr_WriteUnraisable(self);
    }
    else {
        Py_DECREF(result);
    }
}

static PyMethodDef continuation_methods[] = {
    {"send", continuation_send, METH_O, send_doc},
    {"throw", _PyCFunction_CAST(continuation_throw), METH_O, throw_doc},
    {"close", continuation_close, METH_NOARGS, close_doc},
    {NULL, NULL}        /* Sentinel */
};

PyTypeObject PyContinuation_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    "continuation",
    0,
    sizeof(PyContinuationObject),
    .tp_dealloc = continuation_dealloc,
    .tp_repr = continuation_repr,
    .tp_methods = continuation_methods,
    .tp_finalize = continuation_finalize,
};
