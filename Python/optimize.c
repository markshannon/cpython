
#include "Python.h"

#include "pycore_cfg.h"
/*
 * This file optimizes the control-flow graph used as an intermediate
 * representation by compile.c
 *
 */

static inline int conditional_branch(struct instr* instr) {
    return instr->i_opcode == JUMP_IF_TRUE_OR_POP ||
        instr->i_opcode == JUMP_IF_FALSE_OR_POP ||
        instr->i_opcode == POP_JUMP_IF_FALSE ||
        instr->i_opcode == POP_JUMP_IF_TRUE;
}

/** Returns 1 if evaluates to True,
 * 0 if evaluates to False,
 * -1 if doesn't evaluate to a boolean constant.
 */
static int evaluates_to_constant_bool(PyObject* consts, struct instr* instr) {
    if (instr->i_opcode != LOAD_CONST) {
        return -1;
    }
    assert(instr->i_oparg < PyList_GET_SIZE(consts));
    PyObject* cnt = PyList_GET_ITEM(consts, instr->i_oparg);
    assert(cnt != NULL);
    return PyObject_IsTrue(cnt);
}

/** Returns 1 on success, -1 on error */
static int extend_block(basicblock *block, basicblock *extend) {
    int size = extend->b_iused;
    for (int i = 0; i < size; i++) {
        int offset = compiler_next_instr(block);
        if (offset < 0) {
            return -1;
        }
        block->b_instr[offset] = extend->b_instr[i];
    }
    assert(block->b_next == NULL);
    assert(extend->b_next == NULL);
    return 1;
}

static void remove_instructions(basicblock *block, int start, int count) {
    /* Compact block */
    assert(start+count <= block->b_iused);
    for (int i = start; i < block->b_iused-count; i++) {
        block->b_instr[i] = block->b_instr[i+count];
    }
    block->b_iused -= count;
}

#define BLOCK_APPEND_LIMIT 6
#define MAX_ITERATIONS 12


static int jump_optimize_basic_block(basicblock *block) {
    int last_index = block->b_iused-1;
    struct instr *last = &block->b_instr[last_index];
    basicblock *target = last->i_target;
    if (target == block) {
        return 0;
    }
    if (target != NULL && target->b_iused == 0) {
        assert(target->b_next);
        last->i_target = target->b_next;
        return 1;
    }
    struct instr *target0;
    if (target != NULL) {
        target0 = &target->b_instr[0];
    }
    switch(last->i_opcode) {
        case JUMP_FORWARD:
            if (target0->i_opcode == JUMP_ABSOLUTE) {
                *last = *target0;
                return 1;
            }
            break;
        case JUMP_ABSOLUTE:
            if (target0->i_opcode == JUMP_ABSOLUTE) {
                last->i_target = target0->i_target;
                return 1;
            }
            if (target->b_prev == NULL) {
                remove_instructions(block, last_index, 1);
                set_next(block, NULL);
                set_next(block, target);
                return 1;
            }
            if (target->b_next == NULL && target->b_iused > 0 && target->b_iused <= BLOCK_APPEND_LIMIT) {
                set_next(block, NULL);
                remove_instructions(block, last_index, 1);
                return extend_block(block, target);
            }
            if (block->b_next == target) {
                remove_instructions(block, last_index, 1);
                return 1;
            }
            if (block->b_next != NULL) {
                set_next(block, NULL);
                return 1;
            }
            break;
        case JUMP_IF_FALSE_OR_POP:
            switch(target0->i_opcode) {
                case JUMP_IF_FALSE_OR_POP:
                case JUMP_ABSOLUTE:
                    last->i_target = target0->i_target;
                    return 1;
                case JUMP_IF_TRUE_OR_POP:
                    assert(target->b_iused == 1);
                    last->i_opcode = POP_JUMP_IF_FALSE;
                    last->i_target = target->b_next;
                    return 1;
            }
            break;
        case JUMP_IF_TRUE_OR_POP:
            switch(target0->i_opcode) {
                case JUMP_IF_TRUE_OR_POP:
                case JUMP_ABSOLUTE:
                    last->i_target = target0->i_target;
                    return 1;
                case JUMP_IF_FALSE_OR_POP:
                    assert(target->b_iused == 1);
                    last->i_opcode = POP_JUMP_IF_TRUE;
                    last->i_target = target->b_next;
                    return 1;
                
            }
            break;
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
            if (target0->i_opcode == JUMP_ABSOLUTE) {
                last->i_target = target0->i_target;
                return 1;
            }
            break;
    }
    return 0;
}

/* Replace LOAD_CONST c1, LOAD_CONST c2 ... LOAD_CONST cn, BUILD_TUPLE n
   with    LOAD_CONST (c1, c2, ... cn).
   The consts table must still be in list form so that the
   new constant (c1, c2, ... cn) can be appended.
   Called with codestr pointing to the first LOAD_CONST.
*/
static Py_ssize_t
fold_tuple_on_constants(basicblock *block, PyObject* consts, int index)
{
    /* Pre-conditions */
    assert(PyList_CheckExact(consts));
    int n = block->b_instr[index].i_oparg;
    assert(block->b_instr[index].i_opcode == BUILD_TUPLE);
    if (index < n) {
        return 0;
    }
    /* Are all the values used to build the tuple constants? */
    for (int i = 0; i < n; i++) {
        if (block->b_instr[index-n+i].i_opcode != LOAD_CONST) {
            return 0;
        }
    }
    /* Buildup new tuple of constants */
    PyObject *newconst = PyTuple_New(n);
    if (newconst == NULL) {
        return -1;
    }
    for (int i = 0; i < n; i++) {
        struct instr *inst = &block->b_instr[index-n+i];
        PyObject *constant = PyList_GET_ITEM(consts, inst->i_oparg);
        Py_INCREF(constant);
        PyTuple_SET_ITEM(newconst, i, constant);
    }

    Py_ssize_t size = PyList_GET_SIZE(consts);
#if SIZEOF_SIZE_T > SIZEOF_INT
    if ((size_t)size >= UINT_MAX - 1) {
        Py_DECREF(newconst);
        PyErr_SetString(PyExc_OverflowError, "too many constants");
        return -1;
    }
#endif

    /* Append folded constant onto consts */
    if (PyList_Append(consts, newconst)) {
        Py_DECREF(newconst);
        return -1;
    }
    Py_DECREF(newconst);
    block->b_instr[index-n].i_opcode = LOAD_CONST;
    block->b_instr[index-n].i_oparg = size;
    remove_instructions(block, index-n+1, n);
    return 1;
}


/** Optimize a single basic block. 
 * Returns 1 if the block was modified,
 * 0 if it was not and -1 if there was an error.
 */
static int optimize_basic_block(basicblock *block, PyObject* consts) {
    if (block->b_optimized || block->b_iused == 0) {
        return 0;
    }
    assert(block->b_instr != NULL);
    int jump_optimized = jump_optimize_basic_block(block);
    // For efficiency, only perform intra-block optimizations,
    // once inter-block optimizations are done.
    if (jump_optimized) {
        return jump_optimized;
    }
    if (block->b_iused < 2) {
        return 0;
    }
    for (int index = 1; index < block->b_iused; index++) {
        struct instr *inst = &block->b_instr[index];
        int bconst = evaluates_to_constant_bool(consts, &block->b_instr[index-1]);
        switch(inst->i_opcode) {
            case LOAD_FAST:
                if (block->b_instr[index-1].i_opcode == STORE_FAST &&
                    inst->i_oparg == block->b_instr[index-1].i_oparg) {
                    block->b_instr[index-1].i_opcode = DUP_TOP;
                    inst->i_opcode = STORE_FAST;
                    return 1;
                }
                break;
            case UNPACK_SEQUENCE:
                /* Remove temporary tuples.
                   Skip over BUILD_TUPLE 1 UNPACK_SEQN 1.
                   Replace BUILD_TUPLE 2 UNPACK_SEQN 2 with ROT2.
                   Replace BUILD_TUPLE 3 UNPACK_SEQN 3 with ROT3 ROT2. */
                if (block->b_instr[index-1].i_opcode == BUILD_TUPLE) {
                    if (inst->i_oparg == block->b_instr[index-1].i_oparg) {
                        switch(inst->i_oparg) {
                            case 1:
                                remove_instructions(block, index-1, 2);
                                return 1;
                            case 2:
                                block->b_instr[index-1].i_opcode = ROT_TWO;
                                remove_instructions(block, index, 1);
                                return 1;
                            case 3:
                                block->b_instr[index-1].i_opcode = ROT_THREE;
                                inst->i_opcode = ROT_TWO;
                                return 1;
                        }
                    }
                }
                break;
            case BUILD_TUPLE:
            {
                int fold = fold_tuple_on_constants(block, consts, index);
                if (fold != 0) {
                    return fold;
                }
                break;
            }
            case RAISE_VARARGS:
            case RETURN_VALUE:
                // Rest of the block is unreachable
                set_next(block, NULL);
                if (block->b_iused > index+1) {
                    block->b_iused = index+1;
                    return 1;
                }
                break;
            case JUMP_IF_TRUE_OR_POP:
                if (bconst == 1) {
                    inst->i_oparg = JUMP_ABSOLUTE;
                    set_next(block, NULL);
                    block->b_iused = index+1;
                    return 1;
                }
                else if (bconst == 0) {
                    remove_instructions(block, index-1, 2);
                    return 1;
                }
                break;
            case JUMP_IF_FALSE_OR_POP:
                if (bconst == 0) {
                    inst->i_oparg = JUMP_ABSOLUTE;
                    set_next(block, NULL);
                    block->b_iused = index+1;
                    return 1;
                }
                else if (bconst == 1) {
                    remove_instructions(block, index-1, 2);
                    return 1;
                }
                break;
            case POP_JUMP_IF_TRUE:
                if (bconst == 1) {
                    inst->i_opcode = JUMP_ABSOLUTE;
                    remove_instructions(block, index-1, 1);
                    block->b_iused = index;
                    set_next(block, NULL);
                    return 1;
                }
                else if (bconst == 0) {
                    remove_instructions(block, index-1, 2);
                    return 1;
                }
                else if (inst->i_target == block->b_next && index == block->b_iused-1) {
                    inst->i_opcode = POP_TOP;
                    inst->i_jrel = inst->i_jabs = 0;
                    inst->i_target = NULL;
                    return 1;
                }
                break;
            case POP_JUMP_IF_FALSE:
                if (bconst == 0) {
                    inst->i_opcode = JUMP_ABSOLUTE;
                    remove_instructions(block, index-1, 1);
                    block->b_iused = index;
                    set_next(block, NULL);
                    return 1;
                }
                else if (bconst == 1) {
                    remove_instructions(block, index-1, 2);
                    return 1;
                }
                else if (inst->i_target == block->b_next && index == block->b_iused-1) {
                    inst->i_opcode = POP_TOP;
                    inst->i_jrel = inst->i_jabs = 0;
                    inst->i_target = NULL;
                    return 1;
                }
                break;
        }
    }
    block->b_optimized = 1;
    return 0;
}

static int optimize_code_unit(struct compiler_unit *unit, PyObject* consts) {
    int block_modified;
    int modified = 0;
    basicblock *block = unit->u_blocks;
    while (block != NULL) {
        block_modified = optimize_basic_block(block, consts);
        if (block_modified == -1) {
            return -1;
        }
        modified |= block_modified;
        block = block->b_list;
    }
    return modified;
}

int mark_reachable(struct compiler_unit *unit) {
    size_t block_count = 0;
    basicblock *block = unit->u_blocks;
    basicblock *entry;
    do {
        entry = block;
        block = block->b_list;
        block_count++;
    } while (block != NULL);
    basicblock **stack = PyObject_Malloc(sizeof(basicblock *)*block_count);
    size_t depth = 0;
    if (stack == NULL) {
        return -1;
    }
    entry->b_reachable = 1;
    stack[0] = entry;
    depth = 1;
    while(depth) {
        /* Pop block from stack */
        block = stack[--depth];
        basicblock *next = block->b_next;
        if (next != NULL && !next->b_reachable) {
            stack[depth++] = next;
            next->b_reachable = 1;
        }
        for (int i = 0; i < block->b_iused; i++) {
            basicblock *target = block->b_instr[i].i_target;
            if (target != NULL) {
                while (target->b_iused == 0) {
                    assert(target->b_next);
                    target = target->b_next;
                    block->b_instr[i].i_target = target;
                }
                if (!target->b_reachable) {
                    stack[depth++] = target;
                    target->b_reachable = 1;
                }
            }
        }
    }
    PyObject_Free(stack);
    return 0;
}


int _PyOptimize_CodeUnit(struct compiler_unit *unit, PyObject* consts) {
    for(int pass = 0; pass < MAX_ITERATIONS; pass++) {
        int opt = optimize_code_unit(unit, consts);
        if (opt == -1) {
            return -1;
        }
        if (opt == 0) {
            break;
        }
    }
    return 0;
}
