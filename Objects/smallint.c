/* Integer object -- Single word values */

#include "Python.h"
#include "pycore_bitutils.h"      // _Py_popcount32()
#include "pycore_interp.h"        // _PY_NSMALLPOSINTS
#include "pycore_object.h"        // _PyObject_InitVar()
#include "pycore_pystate.h"       // _Py_IsMainInterpreter()
#include "longintrepr.h"

#include <float.h>
#include <ctype.h>
#include <stddef.h>

#define INT_TAG 1
#define INT_TAG_SHIFT 1
#define TAG_MASK ((1<<INT_TAG_SHIFT)-1)

typedef struct _intobject SmallIntObject;


#define ASR(x, y) Py_ARITHMETIC_RIGHT_SHIFT(Py_ssize_t, x, y)
#define DETAG(x) ASR(x, INT_TAG_SHIFT)

#define TAG(x) (((x)<<INT_TAG_SHIFT)+INT_TAG)
#define IS_TAGGED(x) (((x)& TAG_MASK) == INT_TAG)

PyObject *small_int_add(PyObject *a, PyObject *b)
{
    Py_ssize_t l = ((SmallIntObject *)a)->tagged;
    Py_ssize_t r = ((SmallIntObject *)b)->tagged;
    Py_ssize_t result;
    if (Py_add_with_overflow(l-INT_TAG, r, &result)) {
        return PyLong_FromSsize_t(DETAG(l) + DETAG(r));
    }
    else {
        return SmallInt_NewPreTagged(result);
    }
}

PyObject *small_int_sub(PyObject *a, PyObject *b)
{
    Py_ssize_t l = ((SmallIntObject *)a)->tagged;
    Py_ssize_t r = ((SmallIntObject *)b)->tagged;
    Py_ssize_t result;
    if (Py_sub_with_overflow(l, r-INT_TAG, &result)) {
        return PyLong_FromSsize_t(DETAG(l) - DETAG(r));
    }
    else {
        return SmallInt_NewPreTagged(result);
    }
}

PyObject *small_int_mul(PyObject *a, PyObject *b)
{
    Py_ssize_t l = ((SmallIntObject *)a)->tagged;
    Py_ssize_t r = ((SmallIntObject *)b)->tagged;
    Py_ssize_t result;
    if (Py_mul_with_overflow(l-INT_TAG, DETAG(r), &result)) {
        // TO DO... Need method in PyLong for this...
        // return PyLong_FromMultiplied(DETAG(l), DETAG(r));
    }
    else {
        return SmallInt_NewPreTagged(result+INT_TAG);
    }
}

SmallIntObject _PySmallIntZero = {
    {
        _PyObject_EXTRA_INIT
        1, &PyLong_Type,
    },
    TAG(0)
};

SmallIntObject _PySmallIntOne = {
    {
        _PyObject_EXTRA_INIT
        1, &PyLong_Type,
    },
    TAG(1)
};

PyObject *_PyLong_Zero = (PyObject *)&_PySmallIntZero;
PyObject *_PyLong_One = (PyObject *)&_PySmallIntOne;



