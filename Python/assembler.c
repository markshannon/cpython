


#include "Python.h"
#include "cpython/cfg.h"

#include "opcode.h"
#include "wordcode_helpers.h"

static int is_forward_branch(ControlFlowGraph *cfg, BasicBlock *block, Instruction *inst) {
    assert(inst->i_flags & INSTRUCTION_IS_BRANCH);
    assert(&cfg->instructions[block->b_end-1] == inst);
    BasicBlock *target = &cfg->blocks[inst->i_oparg];
    int target_address = target->assembler_offset;
    int from_address = block->assembler_offset+block->assembler_size;
    return (target_address >= from_address);
}

static int branch_oparg(ControlFlowGraph *cfg, BasicBlock *block, Instruction *inst) {
    assert(inst->i_flags & INSTRUCTION_IS_BRANCH);
    assert(&cfg->instructions[block->b_end-1] == inst);
    BasicBlock *target = &cfg->blocks[inst->i_oparg];
    int target_address = target->assembler_offset;
    int from_address;
    switch(inst->i_opcode) {
        case JUMP_FORWARD:
            from_address = block->assembler_offset+block->assembler_size;
            return (target_address - from_address)*sizeof(_Py_CODEUNIT);
        case JUMP_IF_FALSE_OR_POP:
        case JUMP_IF_TRUE_OR_POP:
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
        case SETUP_FINALLY:
        case SETUP_WITH:
        case SETUP_ASYNC_WITH:
        case FOR_ITER:
            /* Absolute jump */
            return target_address*sizeof(_Py_CODEUNIT);
        case JUMP_ABSOLUTE:
            if (is_forward_branch(cfg, block, inst)) {
                from_address = block->assembler_offset+block->assembler_size;
                /* Forward jump (will be emitted as relative JUMP_FORWARD) */
                return (target_address - from_address)*sizeof(_Py_CODEUNIT);
            }
            else {
                /* Absolute jump */
                return target_address*sizeof(_Py_CODEUNIT);
            }
        default:
             Py_UNREACHABLE();
    }
}  

int compute_oparg(ControlFlowGraph *cfg, BasicBlock *block, Instruction *inst) {
    if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
        CFG_ASSERT(inst == &cfg->instructions[block->b_end-1]);
        return branch_oparg(cfg, block, inst);
    }
    else {
        return inst->i_oparg;
    }
}

int instruction_size(ControlFlowGraph *cfg, BasicBlock *block, Instruction *inst) {
    return instrsize(compute_oparg(cfg, block, inst));
}

#ifdef NDEBUG
static inline void
check_block_order(ControlFlowGraph *cfg, int *order) {
    (void)cfg;
}
static inline void
offset_sanity_check(ControlFlowGraph *cfg, int *order) {
    (void)cfg;
}
#else

void 
_assembler_assert_fail(char *msg, int *order, ControlFlowGraph *cfg) {
    printf("[%d", order[0]);
    for(int *block = order+1; *block >= 0; block++) {
        printf(", %d", *block);
    }
    printf("]\n");
    _PyCfg_AssertFail(msg, cfg);
}

#define ASSEMBLER_ASSERT(assertion) if (!(assertion)) { _assembler_assert_fail(#assertion, order, cfg); }

static void
check_block_order(ControlFlowGraph *cfg, int *order) {
    ASSEMBLER_ASSERT(order[0] == 0);
    for(int *block = order; *block >= 0; block++) {
        cfg->blocks[*block].grey = 0;
    }
    for (int *block = order; *block >= 0; block++) {
        BasicBlock *bb = &cfg->blocks[*block];
        /* Is block valid */
        ASSEMBLER_ASSERT(*block < cfg->block_count);
        /* Check that we only see each block once */
        ASSEMBLER_ASSERT(bb->grey == 0);
        bb->grey = 1;
        int fallthrough = bb->b_fallthrough;
        /* Check that all blocks are followed by their fall-through block */
        if (fallthrough >= 0) {
            ASSEMBLER_ASSERT(block[1] == fallthrough);
        }
    }
    /* Check that all reachable blocks are present */
    for (int b = 0; b < cfg->block_count; b++) {
        cfg->blocks[b].grey = 0;
    }
    for (int *block = order; *block >= 0; block++) {
        cfg->blocks[*block].grey = 1;
    }
    for (int b = 0; b < cfg->block_count; b++) {
        ASSEMBLER_ASSERT(!cfg->blocks[b].is_reachable || cfg->blocks[b].grey);
    }
}


static void
offset_sanity_check(ControlFlowGraph *cfg, int *order) {
    check_block_order(cfg, order);
    int offset = 0;
    for(int *block = order; *block >= 0; block++) {
        BasicBlock *bb = &cfg->blocks[*block];
        assert(bb->is_reachable);
        ASSEMBLER_ASSERT(offset == bb->assembler_offset);
        for (int i = bb->b_start; i < bb->b_end; i++) {
            Instruction *inst = &cfg->instructions[i];
            ASSEMBLER_ASSERT(inst->instruction_size == instruction_size(cfg, bb, inst));
            offset += inst->instruction_size;
        }
        ASSEMBLER_ASSERT(offset == bb->assembler_offset + bb->assembler_size);
    }
}
#endif


typedef enum _success_code {
    SUCCESS = 0,
    FAILURE = -1
} SuccessCode;

/* Returns an array of BB indices, terminated by a negative number */
static int *
basic_block_order(ControlFlowGraph *cfg) {
    assert(cfg->block_count > 0);
    int *order = PyObject_Malloc((cfg->block_count+1)*sizeof(int));
    if (order == NULL) {
        return NULL;
    }
    /* Use the 'grey' field to mark fallthrough blocks */
    for (int b = 0; b < cfg->block_count; b++) {
        cfg->blocks[b].grey = 0;
    }
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *block = &cfg->blocks[b];
        if (!block->is_reachable) {
            continue;
        }
        if (block->b_fallthrough > 0) {
            assert(cfg->blocks[block->b_fallthrough].grey == 0);
            cfg->blocks[block->b_fallthrough].grey = 1;
        }
    }
    int index = 0;
    for(int b = 0; b < cfg->block_count; b++) {
        BasicBlock *block = &cfg->blocks[b];
        if (!block->is_reachable || block->grey) {
            continue;
        }
        order[index++] = b;
        while (block->b_fallthrough > 0) {
            order[index++] = block->b_fallthrough;
            block = &cfg->blocks[block->b_fallthrough];
            assert(block->b_fallthrough < 0 || block->grey);
        }
    }
    order[index] = -1;
    assert(index > 0);
    assert(index <= cfg->block_count);
    check_block_order(cfg, order);
    return order;
}

#ifndef NDEBUG
/* Get the offset of the given instruction. 
 *This is slow, use for debugging only */
static int
debug_address(ControlFlowGraph *cfg, BasicBlock *block, Instruction *instruction) {
    int index = instruction - &cfg->instructions[0];
    assert(block->b_start <= index && index < block->b_end);
    int address = block->assembler_offset;
    for (int i = block->b_start; i < index; i++) {
        address += cfg->instructions[i].instruction_size;
    }
    return address;
}
#endif


void cfgDump(ControlFlowGraph *cfg);

static void initialize_offsets(ControlFlowGraph *cfg, int *order) {
    int max_branch_size = instrsize(cfg->instruction_count*sizeof(_Py_CODEUNIT));
    int offset = 0;
    for (int *block = order; *block >= 0; block++) {
        BasicBlock *bb = &cfg->blocks[*block];
        CFG_ASSERT(bb->is_reachable);
        bb->assembler_offset = offset;
        for (int i = bb->b_start; i < bb->b_end; i++) {
            Instruction *inst = &cfg->instructions[i];
            if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                inst->instruction_size = max_branch_size;
                offset += max_branch_size;
            }
            else {
                inst->instruction_size = instrsize(inst->i_oparg);
                offset += inst->instruction_size;
            }
        }
        bb->assembler_size = offset - bb->assembler_offset;
    }
}

static void compute_offsets(ControlFlowGraph *cfg, int *order) {
    initialize_offsets(cfg, order);
    int adjust = 1;
    while (adjust) {
        adjust = 0;
        for (int *block = order; *block >= 0; block++) {
            BasicBlock *bb = &cfg->blocks[*block];
            CFG_ASSERT(bb->is_reachable);
            bb->assembler_offset += adjust;
            if (bb->b_start == bb->b_end) {
                continue;
            }
            Instruction *last = &cfg->instructions[bb->b_end-1];
            if (last->i_flags & INSTRUCTION_IS_BRANCH) {
                int instsize = instruction_size(cfg, bb, last);
                if (last->instruction_size > instsize) {
                    int diff = instsize - last->instruction_size;
                    adjust += diff;
                    bb->assembler_size += diff;
                    last->instruction_size = instsize;
                }
            }
        }
    }
    offset_sanity_check(cfg, order);
}

#define DEFAULT_CODE_SIZE 128
#define DEFAULT_LNOTAB_SIZE 16
    
static SuccessCode
emit_line_delta(Assembler *a, int b_delta, int l_delta) {
    assert(0 <= b_delta && b_delta < 256);
    assert(-128 <= l_delta && l_delta < 128);
    assert(b_delta || l_delta);
    int nbytes = a->a_lnotab_off + 2;
    int len = PyBytes_GET_SIZE(a->a_lnotab);
    if (nbytes >= len) {
        if ((len <= INT_MAX / 2) && (len * 2 < nbytes))
            len = nbytes;
        else if (len <= INT_MAX / 2)
            len *= 2;
        else {
            PyErr_NoMemory();
            return FAILURE;
        }
        if (_PyBytes_Resize(&a->a_lnotab, len) < 0)
            return FAILURE;
    }
    unsigned char *lnotab = (unsigned char *)
                   PyBytes_AS_STRING(a->a_lnotab) + a->a_lnotab_off;
    lnotab[0] = b_delta;
    lnotab[1] = l_delta;
    a->a_lnotab_off += 2;
    return SUCCESS;
}

static SuccessCode
emit_line_table(Assembler *a, Instruction *inst) {
    if (a->a_lineno == inst->i_lineno) {
        return SUCCESS;
    }
    int d_bytecode, d_lineno;
    d_bytecode = (a->a_offset - a->a_lineno_off) * sizeof(_Py_CODEUNIT);
    d_lineno = inst->i_lineno - a->a_lineno;
    while(d_bytecode > 255) {
        if (emit_line_delta(a, 255, 0) != SUCCESS) {
            return FAILURE;
        }
        d_bytecode -= 255;
    }
    while (d_lineno > 127) {
        if (emit_line_delta(a, d_bytecode, 127) != SUCCESS) {
            return FAILURE;
        }
        d_bytecode = 0;
        d_lineno -= 127;
    }
    while (d_lineno < -128) {
        if (emit_line_delta(a, d_bytecode, -128) != SUCCESS) {
            return FAILURE;
        }
        d_bytecode = 0;
        d_lineno += 128;
    }
    assert(d_bytecode || d_lineno);
    if (emit_line_delta(a, d_bytecode, d_lineno) != SUCCESS) {
        return FAILURE;
    }
    a->a_lineno = inst->i_lineno;
    a->a_lineno_off = a->a_offset;
    return SUCCESS;
}

/** Emit the bytecode for a single instruction.
 */
static SuccessCode
emit_instruction(Assembler *a, ControlFlowGraph *cfg, BasicBlock *block, Instruction *inst, int oparg) {
    Py_ssize_t len = PyBytes_GET_SIZE(a->a_bytecode);
    assert(a->a_offset == debug_address(cfg, block, inst));
    int ilen = instrsize(oparg);
    if (a->a_offset + ilen >= len/((unsigned int)sizeof(_Py_CODEUNIT))) {
        if (len > PY_SSIZE_T_MAX / 2)
            return FAILURE;
        if (_PyBytes_Resize(&a->a_bytecode, len * 2) < 0)
            return FAILURE;
    }
    _Py_CODEUNIT *code = ((_Py_CODEUNIT *)PyBytes_AS_STRING(a->a_bytecode)) + a->a_offset;
    CFG_ASSERT(ilen == inst->instruction_size);
    if (inst->i_opcode == JUMP_ABSOLUTE && is_forward_branch(cfg, block, inst)) {
        write_op_arg(code, JUMP_FORWARD, oparg, ilen);
    }
    else {
        write_op_arg(code, inst->i_opcode, oparg, ilen);
    }
    if (emit_line_table(a, inst) != SUCCESS) {
        return FAILURE;
    }
    a->a_offset += ilen;
    return SUCCESS;
}


static SuccessCode
emit_code(ControlFlowGraph *cfg, Assembler *assembler, int *order) {
    offset_sanity_check(cfg, order);
    for (int *block = order; *block >= 0; block++) {
        BasicBlock *bb = &cfg->blocks[*block];
        assert(bb->is_reachable);
        for (int i = bb->b_start; i < bb->b_end; i++) {
            Instruction *inst = &cfg->instructions[i];
            int oparg = compute_oparg(cfg, bb, inst);
            assert(instrsize(oparg) == inst->instruction_size);
            if (emit_instruction(assembler, cfg, bb, inst, oparg) != SUCCESS) {
                Py_DECREF(assembler->a_bytecode);
                assembler->a_bytecode = NULL;
                return FAILURE;
            }
        }
    }
    return SUCCESS;
}

void dumpBytecode(PyObject *bytecode) {
    int size = PyBytes_GET_SIZE(bytecode);
    printf("%d opcodes\n", size/2);
    unsigned char *bytes = (unsigned char *)PyBytes_AS_STRING(bytecode);
    for (int i = 0; i < size; i+=2) {
        printf("%d %d\n", bytes[i], bytes[i+1]);
    }
}


Assembler *assemble(ControlFlowGraph *cfg, int firstline) {
    // Take cfg and produce assembled bytecode and line number table.
    Assembler *assembler = PyObject_Malloc(sizeof(Assembler));
    if (assembler == NULL) {
        return NULL;
    }
    assembler->a_bytecode = PyBytes_FromStringAndSize(NULL, DEFAULT_CODE_SIZE);
    if (assembler->a_bytecode == NULL) {
        PyObject_Free(assembler);
        return NULL;
    }
    int *order = NULL;
    
    assembler->a_lnotab = PyBytes_FromStringAndSize(NULL, DEFAULT_LNOTAB_SIZE);
    if (assembler->a_lnotab == NULL) {
        goto error;
    }
    assembler->a_offset = 0;
    assembler->a_lnotab_off = 0;
    assembler->a_lineno = firstline;
    assembler->a_lineno_off = 0;
    order = basic_block_order(cfg);
    if (order == NULL) {
        goto error;
    }
    compute_offsets(cfg, order);
    
    //Check cfg is still ok
    //computeMaxStackDepth(cfg);
    
    if (emit_code(cfg, assembler, order) != SUCCESS) {
        goto error;
    }
    assert(assembler->a_offset > 0);
    assert(assembler->a_lnotab_off >= 0);
    
    if (_PyBytes_Resize(&assembler->a_bytecode, assembler->a_offset * sizeof(_Py_CODEUNIT)) < 0) {
        goto error;
    }

    //dumpBytecode(assembler->a_bytecode);
    
    if (_PyBytes_Resize(&assembler->a_lnotab, assembler->a_lnotab_off) < 0) {
        goto error;
    }

    PyObject_Free(order);
    return assembler;

error:
    if (order != NULL) {
        PyObject_Free(order);
    }
    _Py_FreeAssembler(assembler);
    return NULL;

}

void _Py_FreeAssembler(Assembler *assembler) {
    Py_XDECREF(assembler->a_bytecode);
    Py_XDECREF(assembler->a_lnotab);
    PyObject_Free(assembler);
}

