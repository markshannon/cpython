


#include "Python.h"
#include "cpython/cfg.h"

#include "opcode.h"
#include "wordcode_helpers.h"



static void
swap_blocks(ControlFlowGraph *cfg, int i, int j) {
    if (i != j) {
        BasicBlock tmp = cfg->blocks[i];
        cfg->blocks[i] = cfg->blocks[j];
        cfg->blocks[j] = tmp;
    }
}

/*  Sort block into a suitable order for emitting code: each block
 *  must be immediately followed by its fallthrough, if any. */
static void
sort_blocks(ControlFlowGraph *cfg) {
    assert(cfg->block_count > 0);
    int i = 0;

    // Each iteration of the loop will put together all the blocks
    // that are fallthrough one after the other until we reach some
    // other block that is independent (does not fallthrough from
    // the block which we start from in every iteration). It will also
    // make sure that we start the next iteration from some block that
    // has fallthrough blocks itself.
    while (1) {
        // Fix the order until we found a block that does not
        // have a fallthrough.
        while (cfg->blocks[i].b_fallthrough > 0) {
            swap_blocks(cfg, i+1, cfg->blocks[i].b_fallthrough);
            i++;
        }

        // Now we are pointing to the first block after the end of
        // the previous fallthrough chain.
        i++;
        if (i == cfg->block_count) {
            return;
        }

        // Locate the first block from our position that is not the fallthrough
        // of some other block (because these may get swapped) or unreachable
        // (in which case we don't need to consider it).
        int j = i;
        while(cfg->blocks[j].is_fallthrough || !cfg->blocks[i].is_reachable) {
            j++;
            if (j == cfg->block_count) {
                return;
            }
        }

        // Put the block that we found (that is not fallthrough) in our position
        // and continue from there.
        swap_blocks(cfg, i, j);
    }
}

static int branch_oparg(Instruction *inst, int size, int target_address) {
    assert(inst->i_flags & INSTRUCTION_IS_BRANCH);
    switch(inst->i_opcode) {
        case JUMP_FORWARD:
        case SETUP_FINALLY:
        case SETUP_WITH:
        case SETUP_ASYNC_WITH:
        case FOR_ITER:
            /* Relative jump */
            assert(target_address >= (inst->assembler_offset+size));
            return (target_address - (inst->assembler_offset+size))*sizeof(_Py_CODEUNIT);
        case JUMP_IF_FALSE_OR_POP:
        case JUMP_IF_TRUE_OR_POP:
        case JUMP_ABSOLUTE:
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
            /* Absolute jump */
            return target_address*sizeof(_Py_CODEUNIT);
        default:
             Py_UNREACHABLE();
    }
}

void compute_offsets(ControlFlowGraph *cfg) {
    int offset = 0;
    /* Perform an initial pass that populates the initial values
     * of the offsets taking into account the number of code units
     * the bytecode needs. This step does not take into account the
     * adjustments needed by the presence of branches */
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *bb = &cfg->blocks[b];
        if (!bb->is_reachable) {
            continue;
        }
        for (int i = bb->b_start; i < bb->b_end; i++) {
            bb->assembler_branch_size = 1;
            Instruction *inst = &cfg->instructions[i];
            inst->assembler_offset = offset;
            if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                offset++;
            }
            else {
                offset += instrsize(inst->i_oparg);
            }
        }
    }

    /* Perform a second pass that fixes the offset taking into account
     * the size of the instruction size of thearguments of each branch
     * (in the previous pass we only counted the branch instruction itself).
     */

    int adjust = 1;
    while (adjust) {
        adjust = 0;
        int b;
        for (b = 0; b < cfg->block_count; b++) {
            BasicBlock *bb = &cfg->blocks[b];
            if (!bb->is_reachable) {
                continue;
            }
            for (int i = bb->b_start; i < bb->b_end; i++) {
                Instruction *inst = &cfg->instructions[i];
                inst->assembler_offset += adjust;
                if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                    assert(i == bb->b_end-1);
                    /* Figure out the target of the branch */
                    int target_index = cfg->blocks[inst->i_oparg].b_start;
                    int target_address = cfg->instructions[target_index].assembler_offset;
                    if (target_index > i) {
                        target_address += adjust;
                    }
                    /* Get the of the opcode argument and calculate its instruction size instruction */
                    int oparg = branch_oparg(inst, bb->assembler_branch_size, target_address);
                    int argsize = instrsize(oparg);
                    /* Adjust the branch size to the new value and add the difference to the
                     * current offset adjustment */
                    if (bb->assembler_branch_size != argsize) {
                        assert(bb->assembler_branch_size < argsize);
                        adjust += argsize - bb->assembler_branch_size;
                        bb->assembler_branch_size = argsize;
                    }
                }
            }
        }
    }
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

static SuccessCode
emit_instruction(Assembler *a, Instruction *inst, int oparg) {
    Py_ssize_t len = PyBytes_GET_SIZE(a->a_bytecode);
    assert(a->a_offset == inst->assembler_offset);
    int ilen = instrsize(oparg);
    if (a->a_offset + ilen >= len/sizeof(_Py_CODEUNIT)) {
        if (len > PY_SSIZE_T_MAX / 2)
            return FAILURE;
        if (_PyBytes_Resize(&a->a_bytecode, len * 2) < 0)
            return FAILURE;
    }
    _Py_CODEUNIT *code = ((_Py_CODEUNIT *)PyBytes_AS_STRING(a->a_bytecode)) + a->a_offset;
    write_op_arg(code, inst->i_opcode, oparg, ilen);
    if (emit_line_table(a, inst) != SUCCESS) {
        return FAILURE;
    }
    a->a_offset += ilen;
    return SUCCESS;
}


static SuccessCode
emit_code(ControlFlowGraph *cfg, Assembler *assembler) {
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *bb = &cfg->blocks[b];
        if (!bb->is_reachable) {
            continue;
        }
        for (int i = bb->b_start; i < bb->b_end; i++) {
            Instruction *inst = &cfg->instructions[i];
            int oparg;
            if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                int target_address = cfg->instructions[cfg->blocks[inst->i_oparg].b_start].assembler_offset;
                oparg = branch_oparg(inst, bb->assembler_branch_size, target_address);
            }
            else {
                oparg = inst->i_oparg;
            }
            if (emit_instruction(assembler, inst, oparg) != SUCCESS) {
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

extern void cfgDump(ControlFlowGraph *cfg);

// Take cfg and produce assembled bytecode and line number table.
Assembler *assemble(ControlFlowGraph *cfg, int firstline) {

    compute_offsets(cfg);
    Assembler *assembler = PyObject_Malloc(sizeof(Assembler));
    if (assembler == NULL) {
        return NULL;
    }
    assembler->a_bytecode = PyBytes_FromStringAndSize(NULL, DEFAULT_CODE_SIZE);
    if (assembler->a_bytecode == NULL) {
        PyObject_Free(assembler);
        return NULL;
    }
    assembler->a_lnotab = PyBytes_FromStringAndSize(NULL, DEFAULT_LNOTAB_SIZE);
    if (assembler->a_lnotab == NULL) {
        goto error;
    }
    assembler->a_offset = 0;
    assembler->a_lnotab_off = 0;
    assembler->a_lineno = firstline;
    assembler->a_lineno_off = 0;
    sort_blocks(cfg);

    //Check cfg is still ok
    computeMaxStackDepth(cfg);

    if (emit_code(cfg, assembler) != SUCCESS) {
        goto error;
    }
    assert(assembler->a_offset > 0);
    assert(assembler->a_lnotab_off >= 0);
    
    if (_PyBytes_Resize(&assembler->a_bytecode, assembler->a_offset * sizeof(_Py_CODEUNIT)) < 0) {
        goto error;
    }
    //cfgDump(cfg);
    //dumpBytecode(assembler->a_bytecode);
    
    if (_PyBytes_Resize(&assembler->a_lnotab, assembler->a_lnotab_off) < 0) {
        goto error;
    }

    return assembler;

error:
    _Py_FreeAssembler(assembler);
    return NULL;

}

void _Py_FreeAssembler(Assembler *assembler) {
    Py_XDECREF(assembler->a_bytecode);
    Py_XDECREF(assembler->a_lnotab);
    PyObject_Free(assembler);
}

