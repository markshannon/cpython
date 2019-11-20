
#include "Python.h"
#include "internal/pycore_stacks.h"


int
_Py_MoveDataStackChunkToHeap(PyDataStackChunk *chunk) {
    assert(chunk->on_heap == 0);
    PyObject **array = PyMem_Malloc(sizeof(PyObject**)*chunk->limit);
    if (array == NULL) {
        return -1;
    }
    for (Py_ssize_t i = 0; i < chunk->stack_offset; i++) {
        array[i] = chunk->base[i];
    }
    chunk->base = array;
    chunk->on_heap = 1;
    return 0;
}

/* Use same arena size as Object malloc */
#define ARENA_SIZE              (256 << 10)     /* 256KB */
#define PAGES_PER_ARENA (ARENA_SIZE/PAGE_SIZE)

typedef struct _Arena {
    PyDataStackPage pages[PAGES_PER_ARENA];
} Arena;

extern PyObjectArenaAllocator _PyObject_Arena;

static Arena *
new_arena(void) {
    return _PyObject_Arena.alloc(_PyObject_Arena.ctx, ARENA_SIZE);
}

/* TO DO -- Make this a subinterpreter field */
static PyDataStackPage *free_pages = NULL;

void _PyDataStack_FreePage(PyDataStackPage *page) {
    page->header.previous = free_pages;
    free_pages = page;
}

static int next_page_id = 1;

static PyDataStackPage *get_free_page(void) {
    if (free_pages == NULL) {
        Arena *arena = new_arena();
        if (arena == NULL) {
            return NULL;
        }
        for (unsigned int i = 0; i < PAGES_PER_ARENA; i++) {
            PyDataStackPage *page = (PyDataStackPage *)(((char *)arena) + i*PAGE_SIZE);
            page->header.previous = free_pages;
            free_pages = page;
            page->header.page_id = next_page_id++;
        }
    }
    PyDataStackPage *page = free_pages;
    free_pages = page->header.previous;
    return page;
}

static PyDataStackPage *get_new_page(PyDataStack *stack) {
    if (free_pages == NULL) {
        Arena *arena = new_arena();
        if (arena == NULL) {
            return NULL;
        }
        for (unsigned int i = 0; i < PAGES_PER_ARENA; i++) {
            PyDataStackPage *page = (PyDataStackPage *)(((char *)arena) + i*PAGE_SIZE);
            page->header.previous = free_pages;
            free_pages = page;
            page->header.page_id = next_page_id++;
        }
    }
    PyDataStackPage *page = free_pages;
    free_pages = page->header.previous;
    PyDataStackPage *previous = current_page(stack);
    previous->header.limit_pointer = stack->limit_pointer;
    page->header.previous = previous;
    stack->limit_pointer = &page->data[0];
    return page;
}

int
init_datastack(PyDataStack *stack) {
    PyDataStackPage *page = get_free_page();
    if (page == NULL) {
        return -1;
    }
    page->header.previous = NULL;
    /* Set limit_pointer to just above the base to avoid popping the base page */
    stack->limit_pointer = &page->data[1];
    stack->chunk_id = 0;
    return 0;
}

int
Py_data_stack_chunk_overflow(PyDataStack *stack, unsigned int size, PyDataStackChunk *chunk) {
    if (too_large_for_stack(size)) {
        chunk->base = PyMem_Malloc(size*sizeof(PyObject *));
        if (chunk->base == NULL) {
            return -1;
        }
        chunk->stack_offset = 0;
        chunk->limit = size;
        chunk->on_heap = 1;
        print_event("pushing", chunk);
        return 0;
    }
    PyDataStackPage *page = get_free_page();
    if (page == NULL) {
        return -1;
    }
    PyDataStackPage *previous = current_page(stack);
    previous->header.limit_pointer = stack->limit_pointer;
    page->header.previous = previous;
    chunk->base = &page->data[0];
    chunk->stack_offset = 0;
    chunk->limit = size;
    stack->limit_pointer = &page->data[size];
    chunk->on_heap = 0;
    print_event("pushing", chunk);
    return 0;
}

