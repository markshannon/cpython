#ifndef Py_BINARYOPERATORS_H
#define Py_BINARYOPERATORS_H


#ifndef Py_LIMITED_API

#ifdef __cplusplus
extern "C" {
#endif


typedef enum _binaryoperator {
    BINARY_OPERATOR_ADD = 0,
    BINARY_OPERATOR_SUB = 1,
    BINARY_OPERATOR_MUL = 2,

} PyBinaryOperator;

/* Use bits 5-8 as an index for up to 15 classes, 0 is reserved for object. */
#define Py_TYPE_ID_OFFSET 5
#define Py_TYPE_ID_BITS 4
#define Py_TYPE_ID_MASK 0x1e0

static inline int
_Py_Binary_Function_Index(PyTypeObject *ltype, PyTypeObject *rtype, PyBinaryOperator op)
{
    assert(op < (1 << Py_TYPE_ID_BITS));
    int top_bits = (ltype->tp_flags & Py_TYPE_ID_MASK) <<  Py_TYPE_ID_BITS;
    int mid_bits = rtype->tp_flags & Py_TYPE_ID_MASK;
    return top_bits | mid_bits | op;
}

extern unsigned char _Py_Function_Indices[16*16*32];

extern binaryfunc _Py_binary_functions[];

int
_Py_AddBinaryFastFunction(PyBinaryOperator op,
    PyTypeObject *ltype, PyTypeObject *rtype, binaryfunc f);

static inline PyObject *_PyCallBinaryFastFunction(PyBinaryOperator op, PyObject *v, PyObject *w) {
    int index = _Py_Binary_Function_Index(Py_TYPE(v), Py_TYPE(w), op);
    int func_index = _Py_Function_Indices[index];
    return _Py_binary_functions[func_index](v, w);
}

void
PyBinaryOperators_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* !Py_LIMITED_API */


#endif /* !Py_BINARYOPERATORS_H */
