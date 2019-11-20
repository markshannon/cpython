#ifndef Py_LIMITED_API
#ifndef Py_INTERNAL_PYSTACKS_H
#define Py_INTERNAL_PYSTACKS_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
    
#define PAGE_SIZE (4<<10)
    
typedef struct _DataStackPageHeader {
    struct _DataStackPage *previous;
    PyObject **limit_pointer;
    int page_id;
} DataStackPageHeader;

typedef struct _DataStackPage {
    DataStackPageHeader header;
    PyObject *data[1];
} PyDataStackPage;

typedef struct _datastack_chunk {
    PyObject **base;
    unsigned int on_heap:1;
    unsigned int stack_offset:31;
    unsigned int limit;
    int chunk_id;
} PyDataStackChunk;

typedef struct _datastack {
    PyObject **limit_pointer;
    int chunk_id;
} PyDataStack;

int Py_data_stack_chunk_overflow(PyDataStack *stack, unsigned int size, PyDataStackChunk *chunk);

extern int init_datastack(PyDataStack *stack);

static inline int
crosses_boundary(void *addr, unsigned int size) {
    unsigned int available = ((-((intptr_t)addr)) & (PAGE_SIZE-1));
    return available < size*sizeof(PyObject *);
}

static inline int
points_to_data_start(void *ptr) {
    return (((uintptr_t)ptr) & (PAGE_SIZE-1)) == offsetof(PyDataStackPage, data);
}

static inline PyDataStackPage *
pointer_to_page(void *ptr) {
    return (PyDataStackPage *)(((uintptr_t)ptr)&(-PAGE_SIZE));
}

static inline PyDataStackPage *
current_page(PyDataStack *stack) {
    /* Limit pointer points to one beyond last allocated,
     * so we need to subtract one to ensure pointer is within page. */
    return pointer_to_page(stack->limit_pointer-1);
}

static inline int
too_large_for_stack(unsigned int size) {
    return size + sizeof(DataStackPageHeader)/sizeof(void *) > PAGE_SIZE/sizeof(PyObject *);
}

#if 1
static inline void print_event(char *what, PyDataStackChunk *chunk) {
    (void)what; (void)chunk;
}
#else
static void print_event(char *what, PyDataStackChunk *chunk) {
    printf(" %s", what);
    if (chunk->on_heap) {
        printf("heap-allocated chunk %d, size %d\n", chunk->chunk_id, chunk->limit);
    } else {
        PyDataStackPage *page = pointer_to_page(chunk->base);
        int offset = chunk->base - page->data;
        printf("chunk %d, %d/%d:%d\n", chunk->chunk_id, page->header.page_id, offset, chunk->limit);
    }
}
#endif

static inline int
push_new_datastack_chunk(PyDataStack *stack, unsigned int size, PyDataStackChunk *chunk) {
    chunk->chunk_id = ++stack->chunk_id;
    if (crosses_boundary(stack->limit_pointer, size)) {
        return Py_data_stack_chunk_overflow(stack, size, chunk);
    }
    chunk->base = stack->limit_pointer;
    chunk->stack_offset = 0;
    chunk->limit = size;
    stack->limit_pointer += size;
    chunk->on_heap = 0;
    print_event("pushing", chunk);
    return 0;
}

extern void _PyDataStack_FreePage(PyDataStackPage *page);

static inline void
pop_datastack_chunk(PyDataStack *stack, PyDataStackChunk *top)
{
    print_event("popping", top);
    assert(top->chunk_id == stack->chunk_id);
    stack->chunk_id--;
    assert(top->on_heap || top->stack_offset == 0);
    assert (top->on_heap || 
        (top->base + top->limit == stack->limit_pointer && 
        "pop violates FIFO stack discipline."));
    /* It is important that the following code does not use any field
     * from `top` other than `limit`, as that is the only field 
     * that is not changed when the chunk is heap allocated.
     */
    if (!too_large_for_stack(top->limit)) {
        PyObject **limit_pointer = stack->limit_pointer - top->limit;
        if (points_to_data_start(limit_pointer)) {
            /* Different page */
            PyDataStackPage *page = current_page(stack);
            PyDataStackPage *previous = page->header.previous;
            stack->limit_pointer = previous->header.limit_pointer;
            _PyDataStack_FreePage(page);
        } else {
            stack->limit_pointer = limit_pointer;
        }
    }
}

extern int
_Py_MoveDataStackChunkToHeap(PyDataStackChunk *chunk);

static inline int
persist_datastack_chunk(PyDataStackChunk *chunk) {
    if (chunk->on_heap) {
        return 0;
    }
    if (_Py_MoveDataStackChunkToHeap(chunk)) {
        return -1;
    }
    return 0;
}

static inline void
datastack_chunk_clear(PyDataStackChunk *chunk) {
    unsigned int limit = chunk->stack_offset;
    chunk->stack_offset = 0;
    for (unsigned int i = 0; i < limit; i++) {
        Py_CLEAR(chunk->base[i]);
    }
}

static inline void
datastack_chunk_dealloc(PyDataStackChunk *chunk) {
    datastack_chunk_clear(chunk);
    if (chunk->on_heap && chunk->base != NULL) {
        PyMem_Free(chunk->base);
        chunk->base = NULL;
    }
}

static inline int
datastack_chunk_traverse(PyDataStackChunk *chunk, visitproc visit, void *arg) {
    unsigned int offset = chunk->stack_offset;
    while (offset) {
        PyObject *obj = chunk->base[--offset];
        Py_VISIT(obj);
    }
    return 0;
}

/** Stores `sp` to the stack pointer of this chunk and returns the previous stack pointer */
static inline PyObject **
PyDataStackChunk_SwapStackPointer(PyDataStackChunk *chunk, PyObject **sp) {
    PyObject **result = chunk->base + chunk->stack_offset;
    assert(chunk->base <= sp && "Stack pointer underflows chunk");
    assert(sp <= chunk->base+chunk->limit && "Stack pointer overflows chunk");
    chunk->stack_offset = sp - chunk->base;
    return result;
}


static inline void
datastack_chunk_push_value(PyDataStackChunk *chunk, PyObject *value) {
    assert(chunk->stack_offset < chunk->limit);
    Py_INCREF(value);
    chunk->base[chunk->stack_offset++] = value;
}

static inline PyObject *
datastack_chunk_pop_value(PyDataStackChunk *chunk) {
    assert(chunk->stack_offset > 0);
    return chunk->base[--chunk->stack_offset];
}

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_PYSTACKS_H */
#endif /* !Py_LIMITED_API */

