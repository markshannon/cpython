#include "Python.h"
#include "binaryoperators.h"

static int next_type_id = 1;
static int next_function_slot = 32;

static int
get_type_index(PyTypeObject *tp) {
    if (tp == NULL) {
        return 0;
    }
    if ((tp->tp_flags & Py_TYPE_ID_MASK) == 0) {
        if (next_type_id < (1 << Py_TYPE_ID_BITS)) {
            tp->tp_flags |= (next_type_id << Py_TYPE_ID_OFFSET);
            next_type_id++;
        }
        else {
            return -1;
        }
    }
    return (tp->tp_flags & Py_TYPE_ID_MASK) >> Py_TYPE_ID_OFFSET;
}

/* This should be no more than 256, as it is indexed by a byte. */
#define BINARY_FUNCTION_COUNT 120

static int
get_index_for_func(binaryfunc f) {
    if (next_function_slot < BINARY_FUNCTION_COUNT) {
        int res = next_function_slot;
        _Py_binary_functions[res] = f;
        next_function_slot++;
        return res;
    }
    else {
        return -1;
    }
}

int
_Py_AddBinaryFastFunction(PyBinaryOperator op, PyTypeObject *l_type,
                          PyTypeObject *r_type, binaryfunc f)
{
    int l_index = get_type_index(l_type);
    if (l_index < 0) {
        return -1;
    }
    int r_index = get_type_index(r_type);
    if (r_index < 0) {
        return -1;
    }
    int res = get_index_for_func(f);
    if (res >= 0) {
        int index = _Py_Binary_Function_Index(l_type, r_type, op);
        _Py_Function_Indices[index] = res;
        _Py_binary_functions[res] = f;
    }
    return res;
}

PyObject *
_Py_binary_default_add(PyObject *v, PyObject *w) {
    PyObject *res = PyNumber_Add(v, w);
    Py_DECREF(v);
    return res;
}

PyObject *
_Py_binary_default_sub(PyObject *v, PyObject *w) {
    PyObject *res = PyNumber_Subtract(v, w);
    Py_DECREF(v);
    return res;
}

PyObject *
_Py_binary_default_mul(PyObject *v, PyObject *w) {
    PyObject *res = PyNumber_Multiply(v, w);
    Py_DECREF(v);
    return res;
}

unsigned char _Py_Function_Indices[16*16*32] = { 0 };

binaryfunc _Py_binary_functions[BINARY_FUNCTION_COUNT] = {
    [BINARY_OPERATOR_ADD] = _Py_binary_default_add,
    [BINARY_OPERATOR_SUB] = _Py_binary_default_sub,
    [BINARY_OPERATOR_MUL] = _Py_binary_default_mul
};

extern PyObject *_Py_add_long_long(PyObject *v, PyObject *w);
extern PyObject *_Py_add_long_float(PyObject *v, PyObject *w);
extern PyObject *_Py_add_float_long(PyObject *v, PyObject *w);
extern PyObject *_Py_add_float_float(PyObject *v, PyObject *w);

void
PyBinaryOperators_Init(void) {
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            for (int k = 0; k < 32; k++) {
                _Py_Function_Indices[i*16*32+j*32+k] = k;
            }
        }
    }
    _Py_AddBinaryFastFunction(BINARY_OPERATOR_ADD, &PyLong_Type, &PyLong_Type, _Py_add_long_long);
    _Py_AddBinaryFastFunction(BINARY_OPERATOR_ADD, &PyFloat_Type, &PyFloat_Type, _Py_add_float_float);
    _Py_AddBinaryFastFunction(BINARY_OPERATOR_ADD, &PyFloat_Type, &PyLong_Type, _Py_add_float_long);
    _Py_AddBinaryFastFunction(BINARY_OPERATOR_ADD, &PyLong_Type, &PyFloat_Type, _Py_add_long_float);
}
