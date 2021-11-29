#ifndef Py_INTERNAL_FIBER_H
#define Py_INTERNAL_FIBER_H
#ifdef __cplusplus
extern "C" {
#endif


typedef struct _fiberobject {
    PyObject_HEAD
    PyObject *callable;
    struct _interpreter_frame *entry_frame;
    struct _interpreter_frame *suspended_frame;
    /* Space for frames */
    _PyStackChunk *datastack_chunk;
    PyObject **datastack_top;
    PyObject **datastack_limit;
} PyFiberObject;

extern PyTypeObject PyFiber_Type;


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_FIBER_H */
