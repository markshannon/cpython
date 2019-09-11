
#include "Python.h"

/* Kept for backwards compatibility of the C API.
 * Does nothing except incref the code object.
 */
PyObject *
PyCode_Optimize(
    PyObject *code, PyObject* _unused1,
    PyObject *_unused2, PyObject *_unused3
) {
    Py_XINCREF(code);
    (void)_unused1;
    (void)_unused2;
    (void)_unused3;
    return code;
}
