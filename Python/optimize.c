

#include "Python.h"
#include "cpython/cfg.h"
#include "opcode.h"

typedef enum _status_code {
    UNCHANGED = 0,
    MODIFIED = 1,
    FAILURE = -1
} StatusCode;

static void remove_instructions(ControlFlowGraph* cfg, BasicBlock *block, int start, int count) {
    /* Compact block */
    block->b_end -= count;
    assert(start <= block->b_end);
    for (int i = start; i < block->b_end; i++) {
        cfg->instructions[i] = cfg->instructions[i+count];
    }
}

static StatusCode
fold_tuple_on_constants(ControlFlowGraph* cfg, BasicBlock *block, PyObject* consts, int index)
{
    assert(cfg->instructions[index].i_opcode == BUILD_TUPLE);
    int n = cfg->instructions[index].i_oparg;
    if (index - n < block->b_start) {
        return UNCHANGED;
    }
    /* Are all the values used to build the tuple constants? */
    for (int i = 0; i < n; i++) {
        if (cfg->instructions[index-n+i].i_opcode != LOAD_CONST) {
            return UNCHANGED;
        }
    }
    /* Buildup new tuple of constants */
    PyObject *newconst = PyTuple_New(n);
    if (newconst == NULL) {
        return FAILURE;
    }
    for (int i = 0; i < n; i++) {
        Instruction *inst = &cfg->instructions[index-n+i];
        PyObject *constant = PyList_GET_ITEM(consts, inst->i_oparg);
        Py_INCREF(constant);
        PyTuple_SET_ITEM(newconst, i, constant);
    }

    Py_ssize_t size = PyList_GET_SIZE(consts);
#if SIZEOF_SIZE_T > SIZEOF_INT
    if ((size_t)size >= UINT_MAX - 1) {
        Py_DECREF(newconst);
        PyErr_SetString(PyExc_OverflowError, "too many constants");
        return FAILURE;
    }
#endif

    /* Append folded constant onto consts */
    if (PyList_Append(consts, newconst)) {
        Py_DECREF(newconst);
        return FAILURE;
    }
    Py_DECREF(newconst);
    cfg->instructions[index-n].i_opcode = LOAD_CONST;
    cfg->instructions[index-n].i_oparg = size;
    remove_instructions(cfg, block, index-n+1, n);
    return MODIFIED;
}

static void
make_jump_unconditional(ControlFlowGraph* cfg, BasicBlock *block, int index) {
    assert(index = block->b_end-1);
    block->is_exit = 1;
    assert(block->b_fallthrough > 0);
    assert(cfg->instructions[index].i_flags & INSTRUCTION_IS_BRANCH);
    cfg->instructions[index].i_flags |= INSTRUCTION_IS_TERMINATOR;
    block->b_fallthrough = -1;
    cfg->instructions[index].i_opcode = JUMP_ABSOLUTE;
}

static StatusCode
remove_conditional_branch_on_constant(ControlFlowGraph* cfg, BasicBlock *block, PyObject* consts, int index)
{
    if (index == block->b_start) {
        return UNCHANGED;
    }
    if (cfg->instructions[index-1].i_opcode != LOAD_CONST) {
        return UNCHANGED;
    }
    PyObject* cnt = PyList_GET_ITEM(consts, cfg->instructions[index-1].i_oparg);
    int is_true = PyObject_IsTrue(cnt);
    if (is_true == -1) {
        return FAILURE;
    }
    switch(cfg->instructions[index].i_opcode) {
        case JUMP_IF_FALSE_OR_POP:
            if (is_true) {
                remove_instructions(cfg, block, index-1, 2);
            } 
            else {
                make_jump_unconditional(cfg, block, index);
            }
            break;
        case JUMP_IF_TRUE_OR_POP:
            if (is_true) {
                make_jump_unconditional(cfg, block, index);
            }
            else {
                remove_instructions(cfg, block, index-1, 2);
            }
            break;
        case POP_JUMP_IF_FALSE:
            if (is_true) {
                remove_instructions(cfg, block, index-1, 2);
            }
            else {
                remove_instructions(cfg, block, index-1, 1);
                make_jump_unconditional(cfg, block, index-1);
            }
            break;
        case POP_JUMP_IF_TRUE:
            if (is_true) {
                remove_instructions(cfg, block, index-1, 1);
                make_jump_unconditional(cfg, block, index-1);
            }
            else {
                remove_instructions(cfg, block, index-1, 2);
            }
            break;
        default:
             Py_UNREACHABLE();
    }
    return MODIFIED;
}

static StatusCode
optimise_basic_block(ControlFlowGraph* cfg, BasicBlock *block, PyObject* consts) {
    assert(PyList_CheckExact(consts));
    for (int i = block->b_start; i < block->b_end; i++) {
        StatusCode status = UNCHANGED;
        switch(cfg->instructions[i].i_opcode) {
            case BUILD_TUPLE:
                status = fold_tuple_on_constants(cfg, block, consts, i);
                break;
            case JUMP_IF_FALSE_OR_POP:
            case JUMP_IF_TRUE_OR_POP:
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
                status = remove_conditional_branch_on_constant(cfg, block, consts, i);
                break;
        }
        if (status != UNCHANGED) {
            return status;
        }
    }
    return UNCHANGED;
}

#define MAX_LENGTH_FOR_DUPLICATING 6

static int
can_be_duplicated(BasicBlock *block) {
    return block->is_exit && block->b_end - block->b_start <= MAX_LENGTH_FOR_DUPLICATING;
}

static int
is_branch(Instruction *inst) {
    return inst->i_flags & INSTRUCTION_IS_BRANCH;
}

static int
is_unconditional(Instruction *inst) {
    assert(is_branch(inst));
    return (inst->i_opcode == JUMP_ABSOLUTE || inst->i_opcode == JUMP_FORWARD);
}

static Instruction *last_instruction(ControlFlowGraph *cfg, BasicBlock *block) {
    assert(block->b_end > block->b_start);
    return &cfg->instructions[block->b_end-1];
}


static StatusCode
optimise_flow(ControlFlowGraph* cfg, int b) {
    BasicBlock *block = &cfg->blocks[b];
    if (block->b_end == block->b_start) {
        return UNCHANGED;
    }
    Instruction *last = last_instruction(cfg, block);
    if (!is_branch(last)) {
        return UNCHANGED;
    }
    int target = last->i_oparg;
    Instruction *target_inst = first_instruction(cfg, target);
    if (is_branch(target_inst)) {
        /* If block starts with a branch it must be exactly one instruction long */
        assert(cfg->blocks[target].b_start == cfg->blocks[target].b_end-1);
        if (is_unconditional(target_inst)) {
            /* Branch to unconditional branch. */
            last->i_oparg = target_inst->i_oparg;
            return MODIFIED;
        }
        else {
            if (last->i_opcode == target_inst->i_opcode &&
                (last->i_opcode == JUMP_IF_FALSE_OR_POP ||
                 last->i_opcode == JUMP_IF_TRUE_OR_POP)) 
            {
                /* Second jump is guaranteed to be taken */
                last->i_oparg = target_inst->i_oparg;
                return MODIFIED;
            }
        }
    } 
    else if (is_unconditional(last)) {
        assert(block->b_fallthrough < 0);
        if (can_be_duplicated(&cfg->blocks[target])) {
            int new_target = _PyCfg_CopyBlock(cfg, target);
            if (new_target < 0) {
                return FAILURE;
            }
            /* Blocks may have reallocated */
            block = &cfg->blocks[b];
            block->b_fallthrough = new_target;
            block->is_exit = 0;
            /* Remove last instruction */ 
            block->b_end--;
            return MODIFIED;
        }
    }
    return UNCHANGED;   
}

extern void cfgDump(ControlFlowGraph *cfg);

int
_Py_Optimize(ControlFlowGraph *cfg, PyObject* consts) {
    int rescan = 1;
    assert(PyList_CheckExact(consts));
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *block = &cfg->blocks[b];
        block->grey = 1;
    }
    while (rescan) {
        rescan = 0;
        for (int b = 0; b < cfg->block_count; b++) {
            BasicBlock *block = &cfg->blocks[b];
            if (!block->grey) {
                continue;
            }
            StatusCode status = optimise_basic_block(cfg, block, consts);
            if (status == FAILURE) {
                return -1;
            }
            StatusCode inter_block = optimise_flow(cfg, b);
            if (inter_block == FAILURE) {
                return -1;
            }
            if (status == UNCHANGED && inter_block == UNCHANGED) {
                block->grey = 0;
            } else {
                rescan = 1;
            }
        }
    }
    return 0;
}

