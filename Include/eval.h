
/* Interface to execute compiled code */

#ifndef Py_EVAL_H
#define Py_EVAL_H
#ifdef __cplusplus
extern "C" {
#endif

PyAPI_FUNC(PyObject *) PyEval_EvalCode(PyObject *, PyObject *, PyObject *);

PyObject* PyEval_EvalCodeEx(PyObject* _co, PyObject* globals, PyObject* locals, PyObject*const* args, int argcount, PyObject*const* kws, int kwcount, PyObject*const* defs, int defcount, PyObject* kwdefs, PyObject* closure);

#ifndef Py_LIMITED_API
PyAPI_FUNC(PyObject *) _PyEval_EvalCodeInternal(
    PyObject *co,
    PyFrameDescriptor *desc, PyObject *locals,
    PyObject *const *args, Py_ssize_t argcount,
    PyObject *const *kwnames, PyObject *const *kwargs,
    Py_ssize_t kwcount, int kwstep,
    PyObject *const *defs, Py_ssize_t defcount,
    PyObject *kwdefs, PyObject *closure,
    PyObject *name, PyObject *qualname);

PyAPI_FUNC(PyObject *) _PyEval_CallTracing(PyObject *func, PyObject *args);
#endif

#ifdef __cplusplus
}
#endif
#endif /* !Py_EVAL_H */
