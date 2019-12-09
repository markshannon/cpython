

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

static StatusCode
remove_build_unpack_pairs(ControlFlowGraph* cfg, BasicBlock *block, int index) {
    Instruction *inst = &cfg->instructions[index];
    Instruction *prev = &cfg->instructions[index-1];
    assert(inst->i_opcode == UNPACK_SEQUENCE);
    if ((index == block->b_start) ||
        (prev->i_opcode != BUILD_TUPLE) ||
        (prev->i_oparg != inst->i_oparg))
    {
        return UNCHANGED;
    }
    switch(inst->i_oparg) {
        case 0:
        case 1:
            remove_instructions(cfg, block, index-1, 2);
            return MODIFIED;
        case 2:
            prev->i_opcode = ROT_TWO;
            prev->i_oparg = 0;
            remove_instructions(cfg, block, index, 1);
            return MODIFIED;
        case 3:
            prev->i_opcode = ROT_THREE;
            prev->i_oparg = 0;
            inst->i_opcode = ROT_TWO;
            inst->i_oparg = 0;
            return MODIFIED;
        default:
            return UNCHANGED;
    }
}

static StatusCode
remove_popped_values(ControlFlowGraph* cfg, BasicBlock *block, int index)
{
    if (index == block->b_start) {
        return UNCHANGED;
    }
    Instruction *inst = &cfg->instructions[index];
    Instruction *prev = &cfg->instructions[index-1];
    assert(inst->i_opcode == POP_TOP);
    switch(prev->i_opcode) {
        case DUP_TOP:
        case LOAD_CONST:
            remove_instructions(cfg, block, index-1, 2);
            return MODIFIED;
        case BUILD_LIST:
        case BUILD_TUPLE:
            switch(prev->i_oparg) {
                case 0:
                    remove_instructions(cfg, block, index-1, 2);
                    return MODIFIED;
                case 1:
                    remove_instructions(cfg, block, index-1, 1);
                    return MODIFIED;
                case 2:
                    prev->i_opcode = POP_TOP;
                    prev->i_oparg = 0;
                    return MODIFIED;
            }
    }
    return UNCHANGED;
}


static void
make_jump_unconditional(ControlFlowGraph* cfg, BasicBlock *block, int index)
{
    assert(index == block->b_end-1);
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
            case UNPACK_SEQUENCE:
                status = remove_build_unpack_pairs(cfg, block, i);
                break;
            case POP_TOP:
                status = remove_popped_values(cfg, block, i);
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
is_unconditional_branch(Instruction *inst) {
    return (inst->i_opcode == JUMP_ABSOLUTE || inst->i_opcode == JUMP_FORWARD);
}

static Instruction *last_instruction(ControlFlowGraph *cfg, BasicBlock *block) {
    assert(block->b_end > block->b_start);
    return &cfg->instructions[block->b_end-1];
}

static inline int
is_jump_or_pop(Instruction *inst) {
    return inst->i_opcode == JUMP_IF_FALSE_OR_POP ||
        inst->i_opcode == JUMP_IF_TRUE_OR_POP;
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
    if (last == target_inst) {
        return UNCHANGED;
    }
    if (is_branch(target_inst)) {
        if (is_unconditional_branch(target_inst)) {
            /* Branch to unconditional jump. */
            last->i_oparg = target_inst->i_oparg;
            return MODIFIED;
        }
        else if (is_jump_or_pop(last) &&
            is_jump_or_pop(target_inst)) {
            if (last->i_opcode == target_inst->i_opcode) {
                /* Second jump is guaranteed to be taken */
                last->i_oparg = target_inst->i_oparg;
                return MODIFIED;
            }
            else {
                /* Second jump is guaranteed not to be taken,
                 so value will always be popped and if first branch is taken
                 then flow will always go to fallthrough of target block. */
                last->i_opcode = (last->i_opcode == JUMP_IF_FALSE_OR_POP) ?
                    POP_JUMP_IF_FALSE : POP_JUMP_IF_TRUE;
                assert(cfg->blocks[target].b_fallthrough > 0);
                assert(cfg->blocks[target].b_start + 1 == cfg->blocks[target].b_end);
                last->i_oparg = cfg->blocks[target].b_fallthrough;
                return MODIFIED;
            }
        }
    }
    else if (last->i_opcode == POP_JUMP_IF_FALSE || last->i_opcode == POP_JUMP_IF_TRUE) {
        assert(block->b_fallthrough > 0);
        Instruction *fallthrough = first_instruction(cfg, block->b_fallthrough);
        if (is_unconditional_branch(fallthrough)) {
            assert(cfg->blocks[block->b_fallthrough].b_start + 1 == cfg->blocks[block->b_fallthrough].b_end);
            int fallthrough_target = fallthrough->i_oparg;
            /* Can swap sense of conditional branch and swap targets, preserving semantics.
             * Do so only if profitable. */
            if (can_be_duplicated(&cfg->blocks[target]) &&
                !can_be_duplicated(&cfg->blocks[fallthrough_target]))
            {
                last->i_oparg = fallthrough_target;
                fallthrough->i_oparg = target;
                if (last->i_opcode == POP_JUMP_IF_FALSE) {
                    last->i_opcode = POP_JUMP_IF_TRUE;
                }
                else {
                    last->i_opcode = POP_JUMP_IF_FALSE;
                }
                return MODIFIED;
            }
        }
    }
    else if (is_unconditional_branch(last)) {
        assert(block->b_fallthrough < 0);
        if (can_be_duplicated(&cfg->blocks[target])) {
            int new_target = _PyCfg_CopyBlock(cfg, target);
            if (new_target < 0) {
                return FAILURE;
            }
            /* Blocks and instructions may have been moved */
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

static void set_id_of_run(ControlFlowGraph *cfg, BasicBlock *start, int id) {
    BasicBlock *block = start;
    start->assembler_offset = id;
    while (block->b_fallthrough > 0) {
        block = &cfg->blocks[block->b_fallthrough];
        block->assembler_offset = id;
    }
}

extern void cfgDump(ControlFlowGraph *cfg);

static void
replace_jumps_with_fallthrough(ControlFlowGraph *cfg) {
    _PyCfg_MarkReachable(cfg);
    /* Mark all blocks that are fallthrough targets */
    for (int b = 0; b < cfg->block_count; b++) {
        cfg->blocks[b].grey = 0;
        cfg->blocks[b].assembler_offset = 0;
    }
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *block = &cfg->blocks[b];
        if (!block->is_reachable) {
            continue;
        }
        if (block->b_fallthrough > 0) {
            cfg->blocks[block->b_fallthrough].grey = 1;
        }
    }
    /* Use assembler_offset as a temporary to record which run of blocks a block belongs to */
    int run_id = 0;
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *block = &cfg->blocks[b];
        if (!block->is_reachable || block->b_end == block->b_start || block->grey) {
            continue;
        }
        set_id_of_run(cfg, block, run_id++);
    }
    /* Replace some unconditional jumps with fallthrough */
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *block = &cfg->blocks[b];
        if (!block->is_reachable || block->b_end == block->b_start) {
            continue;
        }
        Instruction *last = last_instruction(cfg, block);
        if (is_branch(last) && is_unconditional_branch(last)) {
            BasicBlock *target = &cfg->blocks[last->i_oparg];
            /* Check run_id to avoid making loops via fallthrough or
             moving entry run. */
            if (!target->grey &&
                target->assembler_offset != 0 &&
                target->assembler_offset != block->assembler_offset)
            {
                /* Renumber target run */
                set_id_of_run(cfg, target, block->assembler_offset);
                assert(block->b_fallthrough < 0);
                block->b_fallthrough = last->i_oparg;
                block->is_exit = 0;
                /* Remove jump */
                block->b_end--;
                target->grey = 1;
            }
        }
    }
}

int
_Py_Optimize(ControlFlowGraph *cfg, PyObject* consts) {
    int rescan = 1;
    while (rescan) {
        rescan = 0;
        for (int b = 0; b < cfg->block_count; b++) {
            BasicBlock *block = &cfg->blocks[b];
            StatusCode status = optimise_basic_block(cfg, block, consts);
            if (status == FAILURE) {
                return -1;
            }
            StatusCode inter_block = optimise_flow(cfg, b);
            if (inter_block == FAILURE) {
                return -1;
            }
            if (status == MODIFIED || inter_block == MODIFIED) {
                rescan = 1;
            }
        }
    }
    replace_jumps_with_fallthrough(cfg);
    return 0;
}

