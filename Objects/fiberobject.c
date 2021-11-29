
#include "Python.h"
#include "pycore_call.h"
#include "pycore_ceval.h"
#include "pycore_frame.h"
#include "pycore_fiber.h"
#include "pycore_pymem.h"
#include "pycore_pystate.h"

#define FIBER_STACK_CHUNK_SIZE 1024

static int
create_frame_stack(PyFiberObject *fiber)
{
    fiber->datastack_chunk = _PyObject_VirtualAlloc(FIBER_STACK_CHUNK_SIZE);
    if (fiber->datastack_chunk == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    fiber->datastack_chunk->previous = NULL;
    fiber->datastack_chunk->size = FIBER_STACK_CHUNK_SIZE;
    /* If top points to entry 0, then _PyThreadState_PopFrame will try to pop this chunk */
    fiber->datastack_top = &fiber->datastack_chunk->data[1];
    fiber->datastack_limit = (PyObject **)(((char *)fiber->datastack_chunk) + FIBER_STACK_CHUNK_SIZE);
    return 0;
}

static void
swap_stacks(PyFiberObject *fiber, PyThreadState *tstate)
{
    _PyStackChunk *datastack_chunk = tstate->datastack_chunk;
    PyObject **datastack_top = tstate->datastack_top;
    PyObject **datastack_limit = tstate->datastack_limit;
    tstate->datastack_chunk = fiber->datastack_chunk;
    tstate->datastack_top = fiber->datastack_top;
    tstate->datastack_limit = fiber->datastack_limit;
    fiber->datastack_chunk = datastack_chunk;
    fiber->datastack_top = datastack_top;
    fiber->datastack_limit = datastack_limit;
}

PyObject *
fiber_start(PyFiberObject *self, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames)
{
    PyThreadState *tstate = _PyThreadState_GET();
    swap_stacks(self, tstate);
    for(Py_ssize_t i = 0; i < nargs; i++) {
        Py_INCREF(args[i]);
    }
    self->entry_frame = _PyEvalFramePushAndInit(tstate, (PyFunctionObject *)self->callable,
                                                NULL, args, nargs, kwnames);
    if (self->entry_frame == NULL) {
        return NULL;
    }
    self->entry_frame->previous = tstate->cframe->current_frame;
    PyFiberObject *previous = tstate->current_fiber;
    tstate->current_fiber = self;
    PyObject *result = _PyEval_EvalFrame(tstate, self->entry_frame, 0);
    self->entry_frame->previous = NULL;
    if (self->suspended_frame == NULL) {
        /* Completed */
        _PyEvalFrameClearAndPop(tstate, self->entry_frame);
        self->entry_frame = NULL;
    }
    tstate->current_fiber = NULL;
    swap_stacks(self, tstate);
    tstate->current_fiber = previous;
    return result;
}

PyObject *
fiber_send(PyFiberObject *self, PyObject *obj)
{
    if (self->suspended_frame == NULL) {
        if (self->entry_frame == NULL) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "cannot send to fiber that has not been started");
        }
        else {
            PyErr_SetString(
                PyExc_RuntimeError,
                "cannot send to a running fiber.");
        }
        return NULL;
    }
    assert(self->entry_frame != NULL);
    PyThreadState *tstate = _PyThreadState_GET();
    Py_INCREF(obj);
    InterpreterFrame *frame = self->suspended_frame;
    self->suspended_frame = NULL;
    _PyFrame_StackPush(frame, obj);
    self->entry_frame->previous = tstate->cframe->current_frame;
    /* TO DO -- Handle exc_info */
    swap_stacks(self, tstate);
    PyFiberObject *previous = tstate->current_fiber;
    tstate->current_fiber = self;
    PyObject *result = _PyEval_EvalFrame(tstate, frame, 0);
    self->entry_frame->previous = NULL;
    if (self->suspended_frame == NULL) {
        /* Completed */
        _PyEvalFrameClearAndPop(tstate, self->entry_frame);
        self->entry_frame = NULL;
    }
    tstate->current_fiber = NULL;
    swap_stacks(self, tstate);
    tstate->current_fiber = previous;
    return result;
}

PyObject *
fiber_throw(PyFiberObject *self, PyObject *exc)
{
    PyObject *type = NULL, *value = NULL;
    if (self->suspended_frame == NULL) {
        if (self->entry_frame == NULL) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "cannot throw into fiber that has not been started");
        }
        else {
            PyErr_SetString(
                PyExc_RuntimeError,
                "cannot throw into a running fiber.");
        }
        return NULL;
    }
    if (PyExceptionClass_Check(exc)) {
        type = exc;
        value = _PyObject_CallNoArgs(exc);
        if (value == NULL) {
            return NULL;
        }
        if (!PyExceptionInstance_Check(value)) {
            PyErr_Format(PyExc_TypeError,
                          "calling %R should have returned an instance of "
                          "BaseException, not %R",
                          type, Py_TYPE(value));
            return NULL;
        }
    }
    else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        type = PyExceptionInstance_Class(exc);
        Py_INCREF(type);
    }
    else {
        /* Not something you can raise.  You get an exception
           anyway, just not what you specified :-) */
        Py_DECREF(exc);
        PyErr_SetString(PyExc_TypeError,
                         "exceptions must derive from BaseException");
        return NULL;
    }
    PyErr_SetObject(type, value);
    /* _PyErr_SetObject incref's its arguments */
    Py_DECREF(value);
    Py_DECREF(type);
    assert(self->entry_frame != NULL);
    PyThreadState *tstate = _PyThreadState_GET();
    InterpreterFrame *frame = self->suspended_frame;
    self->suspended_frame = NULL;
    self->entry_frame->previous = tstate->cframe->current_frame;
    /* TO DO -- Handle exc_info */
    swap_stacks(self, tstate);
    PyFiberObject *previous = tstate->current_fiber;
    tstate->current_fiber = self;
    PyObject *result = _PyEval_EvalFrame(tstate, frame, 1);
    self->entry_frame->previous = NULL;
    if (self->suspended_frame == NULL) {
        /* Completed */
        _PyEvalFrameClearAndPop(tstate, self->entry_frame);
        self->entry_frame = NULL;
    }
    tstate->current_fiber = NULL;
    swap_stacks(self, tstate);
    tstate->current_fiber = previous;
    return result;
}

static PyMethodDef fiber_methods[] = {
    {"send", (PyCFunction)fiber_send, METH_O, NULL},
    {"throw", (PyCFunction)fiber_throw, METH_O, NULL},
    {"start", (PyCFunction)fiber_start, METH_FASTCALL | METH_KEYWORDS, NULL},
    {NULL, NULL}        /* Sentinel */
};


static PyObject *
fiber_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = {"callable", NULL};
    PyObject *callable;
    if (PyArg_ParseTupleAndKeywords(args, kwds, "O", kwlist, &callable) == 0) {
        return NULL;
    }
    PyFiberObject *fiber = PyObject_New(PyFiberObject, type);
    if (fiber == NULL) {
        return NULL;
    }
    Py_INCREF(callable);
    fiber->callable = callable;
    if (create_frame_stack(fiber)) {
        Py_DECREF(fiber);
        return NULL;
    }
    return (PyObject *)fiber;
}

static void
fiber_dealloc(PyFiberObject *self)
{
    /* TO DO -- If suspended, clear all frames.*/
    _PyStackChunk *chunk = self->datastack_chunk;
    self->datastack_chunk = NULL;
    while (chunk != NULL) {
        _PyStackChunk *prev = chunk->previous;
        _PyObject_VirtualFree(chunk, chunk->size);
        chunk = prev;
    }
    Py_CLEAR(self->callable);
}

PyTypeObject PyFiber_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0)
    .tp_name = "Fiber",
    .tp_basicsize = sizeof(PyFiberObject),
    .tp_dealloc = (destructor)fiber_dealloc,
    .tp_new = fiber_new,
    .tp_methods = fiber_methods,
    .tp_finalize = NULL, // TO DO: fiber_finalize
};

void
_PyFiber_Init(void) {
    // Add Python methods to Fiber
}
