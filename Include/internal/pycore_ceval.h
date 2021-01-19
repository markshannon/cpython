#ifndef Py_INTERNAL_CEVAL_H
#define Py_INTERNAL_CEVAL_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* Forward declarations */
struct pyruntimestate;
struct _ceval_runtime_state;

#include "pycore_interp.h"   /* PyInterpreterState.eval_frame */

extern void _Py_FinishPendingCalls(PyThreadState *tstate);
extern void _PyEval_InitRuntimeState(struct _ceval_runtime_state *);
extern int _PyEval_InitState(struct _ceval_state *ceval);
extern void _PyEval_FiniState(struct _ceval_state *ceval);
PyAPI_FUNC(void) _PyEval_SignalReceived(PyInterpreterState *interp);
PyAPI_FUNC(int) _PyEval_AddPendingCall(
    PyInterpreterState *interp,
    int (*func)(void *),
    void *arg);
PyAPI_FUNC(void) _PyEval_SignalAsyncExc(PyInterpreterState *interp);
#ifdef HAVE_FORK
extern PyStatus _PyEval_ReInitThreads(PyThreadState *tstate);
#endif
PyAPI_FUNC(void) _PyEval_SetCoroutineOriginTrackingDepth(
    PyThreadState *tstate,
    int new_depth);

void _PyEval_Fini(void);


extern PyObject* _PyEval_GetBuiltins(PyThreadState *tstate);
extern PyObject *_PyEval_BuiltinsFromGlobals(
    PyThreadState *tstate,
    PyObject *globals);


static inline PyObject*
_PyEval_EvalFrame(PyThreadState *tstate, PyFrameObject *f, int throwflag)
{
    return tstate->interp->eval_frame(tstate, f, throwflag);
}

extern PyObject *
_PyEval_Vector(PyThreadState *tstate,
            PyFrameConstructor *desc, PyObject *locals,
            PyObject* const* args, size_t argcount,
            PyObject *kwnames);

#ifdef EXPERIMENTAL_ISOLATED_SUBINTERPRETERS
extern int _PyEval_ThreadsInitialized(PyInterpreterState *interp);
#else
extern int _PyEval_ThreadsInitialized(struct pyruntimestate *runtime);
#endif
extern PyStatus _PyEval_InitGIL(PyThreadState *tstate);
extern void _PyEval_FiniGIL(PyInterpreterState *interp);

extern void _PyEval_ReleaseLock(PyThreadState *tstate);


static inline int _Py_MakePyRecCheck(PyThreadState *tstate) {
    return (++tstate->recursion_depth > tstate->interp->ceval.recursion_limit);
}

/* --- _Py_EnterRecursiveCall() ----------------------------------------- */


static inline int _Py_MakeRecCheck(PyThreadState *tstate) {
    char var;
#if STACK_GROWS_DOWN
    return &var < stack_limit_pointer;
#else
    return &var > stack_limit_pointer;
#endif
}



static inline int _Py_EnterRecursivePythonCall(PyThreadState *tstate,
                                         const char *where) {
    if (++tstate->recursion_depth <= tstate->interp->ceval.recursion_limit) {
        return 0;
    }
    if (tstate->recursion_headroom) {
        if (tstate->recursion_depth > tstate->interp->ceval.recursion_limit + 50) {
            /* Overflowing while handling an overflow. Give up. */
            Py_FatalError("Cannot recover from stack overflow.");
        }
        return 0;
    }
    else {
        tstate->recursion_headroom++;
        PyErr_Format(PyExc_RecursionOverflow,
                    "maximum recursion depth exceeded%s",
                    where);
        --tstate->recursion_depth;
        tstate->recursion_headroom--;
        return -1;
    }
}

static inline int _Py_EnterRecursiveCall(PyThreadState *tstate,
                                         const char *where) {
    if (_Py_MakeRecCheck(tstate)) {
#if defined(Py_DEBUG)
        char var;
#if STACK_GROWS_DOWN
        assert(stack_limit_pointer != (char *)((uintptr_t)-1));
        assert(&var > stack_limit_pointer-1024);
#else
        assert(stack_limit_pointer != NULL);
        assert(&var < stack_limit_pointer+1024);
#endif
#endif
        PyErr_Format(PyExc_StackOverflow,
                "stack overflow%s",
                where);
        return -1;
    }
    return 0;
}

static inline int _Py_EnterRecursiveCall_inline(const char *where) {
    PyThreadState *tstate = PyThreadState_GET();
    return _Py_EnterRecursiveCall(tstate, where);
}

static inline int Py_StackCheck(const char *where) {
    return _Py_EnterRecursiveCall_inline(where);
}

#define Py_EnterRecursiveCall(where) Py_StackCheck(where)

static inline void _Py_LeaveRecursiveCall(PyThreadState *tstate)  {
    (void)tstate;
}

static inline void _Py_LeaveRecursiveCall_inline(void)  {
}

#define Py_LeaveRecursiveCall() _Py_LeaveRecursiveCall_inline()


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CEVAL_H */
