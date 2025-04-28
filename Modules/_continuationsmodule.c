/* Base continuation object implementation */

#include "Python.h"

#include "opcode_ids.h"
#include "pycore_pyerrors.h"      // _PyErr_ClearExcState()
#include "pystate.h"
#include "pycore_call.h"
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

extern int
_Py_InitContinuation(PyContinuationObject *cont, PyObject *func, PyObject *args, PyObject *kwargs);

extern PyObject *
_Py_StartContinuation(PyThreadState *tstate, PyObject *continuation);

PyObject *
continuation_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    PyContinuationObject *cont = (PyContinuationObject *)_PyObject_GC_New(type);
    if (cont == NULL) {
        return PyErr_NoMemory();
    }
    cont->stack.datastack_chunk = NULL;
    cont->stack.datastack_top = NULL;
    cont->stack.datastack_limit = NULL;
    cont->current_frame = NULL;
    cont->exc_info = NULL;
    cont->root_exc_info = NULL;
    cont->root_frame = NULL;
    cont->cont_weakreflist = NULL;
    cont->executing = 0;
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

static PyObject *
continuation_start(PyObject *self, PyObject *Py_UNUSED(unused))
{
    PyThreadState *tstate = PyThreadState_GET();
    return _Py_StartContinuation(tstate, self);
}

static PyMethodDef continuation_methods[] = {
    {"start", _PyCFunction_CAST(continuation_start), METH_NOARGS, NULL },
    // {"close", _PyCFunction_CAST(continuation_close), METH_NOARGS, NULL},
    {NULL, NULL}        /* Sentinel */
};

static int
continuation_traverse(PyObject *self, visitproc visit, void *arg)
{
    // TO DO ...
    return 0;
}

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

static struct PyModuleDef_Slot continuations_slots[] = {
    {Py_mod_exec, continuations_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};

static struct PyModuleDef _continuationsmodule = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "_continuations",
    .m_slots = continuations_slots,
};

PyMODINIT_FUNC
PyInit__continuations(void)
{
    return PyModuleDef_Init(&_continuationsmodule);
}
