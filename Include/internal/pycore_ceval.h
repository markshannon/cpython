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


/* --- _Py_EnterRecursiveCall() ----------------------------------------- */


#define BLOCK_SIZE 0x2000
#define BLOCK_MASK (-BLOCK_SIZE)

extern int _PyDoStackCheck(char *page_base, const char *where);

static inline int _Py_MakeRecCheck(PyThreadState *tstate) {
    return (++tstate->recursion_depth > tstate->interp->ceval.recursion_limit);
}

PyAPI_FUNC(int) _Py_CheckRecursiveCall(
    PyThreadState *tstate,
    const char *where);

int _Py_StackCheckFail(
    PyThreadState *tstate,
    const char *where);

PyAPI_FUNC(int) Py_CheckStackDepth(const char *where);

static inline int _Py_EnterRecursiveCall(PyThreadState *tstate,
                                         const char *where) {
    if (Py_CheckStackDepth(where)) {
        return -1;
    }
    return (_Py_MakeRecCheck(tstate) && _Py_CheckRecursiveCall(tstate, where));
}

static inline int _Py_EnterRecursiveCall_inline(const char *where) {
    PyThreadState *tstate = PyThreadState_GET();
    return _Py_EnterRecursiveCall(tstate, where);
}

int _Py_CheckStackDepthNoException(void);


static inline char *
_Py_Address_BaseNextPage(void) {
    char var;
    uintptr_t addr = (uintptr_t)&var;
#if C_STACK_GROWS_DOWN
    char *page_base = (char *)((addr-BLOCK_SIZE) & BLOCK_MASK);
#else
    char *page_base = (char *)((addr+BLOCK_SIZE*2) & BLOCK_MASK)-1;
#endif
    return page_base;
}

static inline int _Py_CheckStackDepth_inline(const char *where) {
    char *page_base = _Py_Address_BaseNextPage();
    if (*page_base == 0) {
        return _PyDoStackCheck(page_base, where);
    }
    return 0;
}

#define Py_EnterRecursiveCall(where) _Py_EnterRecursiveCall_inline(where)

#define Py_CheckStackDepth(where) _Py_CheckStackDepth_inline(where)


static inline void _Py_LeaveRecursiveCall(PyThreadState *tstate)  {
    tstate->recursion_depth--;
}

static inline void _Py_LeaveRecursiveCall_inline(void)  {
    PyThreadState *tstate = PyThreadState_GET();
    _Py_LeaveRecursiveCall(tstate);
}

#define Py_LeaveRecursiveCall() _Py_LeaveRecursiveCall_inline()


#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_CEVAL_H */
