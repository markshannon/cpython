
typedef struct {
    PyObject_HEAD
    PyObject *globals;     /* A dictionary (other mappings won't do) */
    PyObject *builtins;
    PyObject *name;
    PyObject *qualname;
} PyFrameDescriptor;

PyObject *_PyEval_BuiltinsFromGlobals(PyObject *globals);

PyFrameDescriptor *_PyEval_NewFrameDescriptor(PyObject *globals, PyObject *builtins, PyObject *name, PyObject *qualname);
