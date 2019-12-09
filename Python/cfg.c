
#include "Python.h"
#include "cpython/cfg.h"
#include "opcode.h"

#define INSTRUCTION_STARTS_BB 16
#define MINIMUM_BLOCKS 16

static void findBasicBlocks(Instruction *instructions, int count) {
    instructions[0].i_flags |= INSTRUCTION_STARTS_BB;
    for (int i = 0; i < count-1; i++) {
        Instruction *instr = &instructions[i];
        if (instr->i_flags & INSTRUCTION_IS_BRANCH) {
            instructions[instr->i_oparg].i_flags |= INSTRUCTION_STARTS_BB;
            instructions[i+1].i_flags |= INSTRUCTION_STARTS_BB;
        }
        else if (instr->i_flags & INSTRUCTION_IS_TERMINATOR) {
            instructions[i+1].i_flags |= INSTRUCTION_STARTS_BB;
        }
    }
    Instruction *last = &instructions[count-1];
    if (last->i_flags & INSTRUCTION_IS_BRANCH) {
        instructions[last->i_oparg].i_flags |= INSTRUCTION_STARTS_BB;
    }
}


static int allocate_blocks(ControlFlowGraph *cfg, int blocks) {
    if (blocks <= cfg->allocated_blocks) {
        return 0;
    }
    do {
        cfg->allocated_blocks *= 2;
    } while (cfg->allocated_blocks < blocks);
    cfg->blocks = PyObject_Realloc(cfg->blocks, sizeof(BasicBlock) * cfg->allocated_blocks);
    if (cfg->blocks == NULL) {
        return -1;
    }
    return 0;
}

int _PyCfg_CopyBlock(ControlFlowGraph *cfg,  int block) {
    printf("Copying block %d\n", block);
    /* Copying a block with a fallthrough would invalidate the CFG */
    assert(cfg->blocks[block].b_fallthrough < 0);
    int copy = cfg->block_count++;
    if (allocate_blocks(cfg, cfg->block_count) < 0) {
        return -1;
    }
    cfg->blocks[copy] = cfg->blocks[block];
    return copy;
}

static int
createBasicBlocks(ControlFlowGraph *cfg) {
    cfg->blocks = PyObject_Malloc(MINIMUM_BLOCKS * sizeof(BasicBlock));
    if (cfg->blocks == NULL) {
        return -1;
    }
    cfg->allocated_blocks = MINIMUM_BLOCKS;
    int next_block = 0;
    assert(cfg->instructions);
    assert(cfg->instruction_count > 0);
    assert(cfg->instructions[0].i_flags & INSTRUCTION_STARTS_BB);
    for (int i = 0; i < cfg->instruction_count; i++) {
        if (cfg->instructions[i].i_flags & INSTRUCTION_STARTS_BB) {
            if (allocate_blocks(cfg, next_block+1) < 0) {
                return -1;
            }
            if (next_block > 0) {
                cfg->blocks[next_block-1].b_end = i;
            }
            cfg->blocks[next_block].b_start = i;
            cfg->blocks[next_block].is_exit = 0;
            cfg->blocks[next_block].is_reachable = 1;
            cfg->blocks[next_block].stack_depth_at_start = INT_MIN;
            /* Temporary store block index into line number of first instruction
             * and line number of first instruction into basic block.
             */
            cfg->blocks[next_block].b_fallthrough = cfg->instructions[i].i_lineno;
            cfg->instructions[i].i_lineno = next_block;
            next_block++;
        }
    }
    cfg->blocks[0].stack_depth_at_start = 0;
    cfg->blocks[next_block-1].b_end = cfg->instruction_count;
    cfg->block_count = next_block;
    return 0;
}

static void
convertBranchTargets(ControlFlowGraph *cfg)
{
    for (int i = 0; i < cfg->instruction_count; i++) {
        Instruction *instr = &cfg->instructions[i];
        if (instr->i_flags & INSTRUCTION_IS_BRANCH) {
            int block_id = cfg->instructions[instr->i_oparg].i_lineno;
            instr->i_oparg = block_id;
        }
    }
}

static void
fixup(ControlFlowGraph *cfg) {
    for (int i = 0; i < cfg->block_count; i++) {
        BasicBlock *block = &cfg->blocks[i];
        cfg->instructions[block->b_start].i_lineno = block->b_fallthrough;
        if (block->b_start == block->b_end || (cfg->instructions[block->b_end-1].i_flags & INSTRUCTION_IS_TERMINATOR) == 0) {
            block->b_fallthrough = i+1;
        } else {
            block->b_fallthrough = -1;
            if (cfg->instructions[block->b_end-1].i_flags & INSTRUCTION_IS_TERMINATOR) {
                block->is_exit = 1;
            }
        }
    }
}
    
ControlFlowGraph *createCfg(Instruction *instructions, int count) {
    ControlFlowGraph *cfg = PyObject_Malloc(sizeof(ControlFlowGraph));
    if (cfg == NULL) {
        return NULL;
    }
    assert(instructions);
    assert(count > 0);
    cfg->instructions = instructions;
    cfg->instruction_count = count;
    findBasicBlocks(instructions, count);
    if (createBasicBlocks(cfg) < 0) {
        PyObject_Free(cfg);
        return NULL;
    }
    convertBranchTargets(cfg);
    fixup(cfg);
    return cfg;
}

void _Py_FreeCfg(ControlFlowGraph *cfg) {
    if (cfg != NULL) {
        PyObject_Free(cfg->instructions);
        PyObject_Free(cfg->blocks);
        PyObject_Free(cfg);
    }
}

static int
bb_is_branch(ControlFlowGraph *cfg, BasicBlock *block) {
    return block->b_end > block->b_start && (cfg->instructions[block->b_end-1].i_flags & INSTRUCTION_IS_BRANCH);
}

static BasicBlock *
bb_get_branch_target(ControlFlowGraph *cfg, BasicBlock *block) {
    return &cfg->blocks[cfg->instructions[block->b_end-1].i_oparg];
}

void
_PyCfg_MarkReachable(ControlFlowGraph *cfg) {
    cfg->blocks[0].grey = 1;
    cfg->blocks[0].is_reachable = 1;
    for (int i = 1; i < cfg->block_count; i++) {
        BasicBlock *block = &cfg->blocks[i];
        block->grey = 0;
        block->is_reachable = 0;
    }
    int scan = 1;
    while (scan) {
        scan = 0;
        for (int i = 0; i < cfg->block_count; i++) {
            BasicBlock *block = &cfg->blocks[i];
            if (!block->grey) {
                continue;
            }
            block->grey = 0;
            if (block->b_fallthrough > 0) {
                BasicBlock *fallthrough = &cfg->blocks[block->b_fallthrough];
                if (!fallthrough->is_reachable) {
                    fallthrough->is_reachable = 1;
                    fallthrough->grey = 1;
                }
            }
            if (bb_is_branch(cfg, block)) {
                BasicBlock *target = bb_get_branch_target(cfg, block);
                if (!target->is_reachable) {
                    target->is_reachable = 1;
                    target->grey = 1;
                    if (target < block) {
                        // Backwards branch, so need to rescan.
                        scan = 1;
                    }
                }
            }
        }
    }
}


/* Return the minimum stack depth required for this opcode 
 */
static int
stack_input(int opcode, int oparg)
{
    switch (opcode) {
        case NOP:
        case EXTENDED_ARG:
            return 0;

        /* Stack manipulation */
        case POP_TOP:
            return 1;
        case ROT_TWO:
            return 2;
        case ROT_THREE:
            return 3;
        case ROT_FOUR:
            return 4;
        case DUP_TOP:
            return 1;
        case DUP_TOP_TWO:
            return 2;

        /* Unary operators */
        case UNARY_POSITIVE:
        case UNARY_NEGATIVE:
        case UNARY_NOT:
        case UNARY_INVERT:
            return 1;

        case SET_ADD:
        case LIST_APPEND:
        case MAP_ADD:
            return oparg;

        /* Binary operators */
        case BINARY_POWER:
        case BINARY_MULTIPLY:
        case BINARY_MATRIX_MULTIPLY:
        case BINARY_MODULO:
        case BINARY_ADD:
        case BINARY_SUBTRACT:
        case BINARY_SUBSCR:
        case BINARY_FLOOR_DIVIDE:
        case BINARY_TRUE_DIVIDE:
        case INPLACE_FLOOR_DIVIDE:
        case INPLACE_TRUE_DIVIDE:
        case INPLACE_ADD:
        case INPLACE_SUBTRACT:
        case INPLACE_MULTIPLY:
        case INPLACE_MATRIX_MULTIPLY:
        case INPLACE_MODULO:
            return 2;
        case STORE_SUBSCR:
            return 3;
        case DELETE_SUBSCR:
            return 2;

        case BINARY_LSHIFT:
        case BINARY_RSHIFT:
        case BINARY_AND:
        case BINARY_XOR:
        case BINARY_OR:
        case INPLACE_POWER:
            return 2;
        case GET_ITER:
            return 1;

        case PRINT_EXPR:
            return 1;
        case LOAD_BUILD_CLASS:
            return 0;
        case INPLACE_LSHIFT:
        case INPLACE_RSHIFT:
        case INPLACE_AND:
        case INPLACE_XOR:
        case INPLACE_OR:
            return 2;

        case SETUP_WITH:
            /* 1 in the normal flow.
             * Restore the stack position and push 6 values before jumping to
             * the handler if an exception be raised. */
            return 1;
        case RETURN_VALUE:
            return 1;
        case IMPORT_STAR:
            return 1;
        case SETUP_ANNOTATIONS:
            return 0;
        case YIELD_VALUE:
            return 1;
        case YIELD_FROM:
            return 1;
        case POP_BLOCK:
            return 0;
        case POP_EXCEPT:
            return 3;

        case STORE_NAME:
            return 1;
        case DELETE_NAME:
            return 0;
        case UNPACK_SEQUENCE:
            return 1;
        case UNPACK_EX:
            return 1;
        case FOR_ITER:
            return 1;

        case STORE_ATTR:
            return 2;
        case DELETE_ATTR:
            return 1;
        case STORE_GLOBAL:
            return 1;
        case DELETE_GLOBAL:
            return 0;
        case LOAD_CONST:
            return 0;
        case LOAD_NAME:
            return 0;
        case BUILD_TUPLE:
        case BUILD_LIST:
        case BUILD_SET:
        case BUILD_STRING:
            return oparg;
        case BUILD_LIST_UNPACK:
        case BUILD_TUPLE_UNPACK:
        case BUILD_TUPLE_UNPACK_WITH_CALL:
        case BUILD_SET_UNPACK:
        case BUILD_MAP_UNPACK:
        case BUILD_MAP_UNPACK_WITH_CALL:
            return oparg;
        case BUILD_MAP:
            return 2*oparg;
        case BUILD_CONST_KEY_MAP:
            return oparg;
        case LOAD_ATTR:
            return 0;
        case COMPARE_OP:
            return 2;
        case IMPORT_NAME:
            return 1;
        case IMPORT_FROM:
            return 0;

        /* Jumps */
        case JUMP_FORWARD:
        case JUMP_ABSOLUTE:
            return 0;

        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            return 1;

        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
            return -1;

        case LOAD_GLOBAL:
            return 0;

        /* Exception handling */
        case SETUP_FINALLY:
            /* 0 in the normal flow.
             * Restore the stack position and push 6 values before jumping to
             * the handler if an exception be raised. */
            return 0;
        case RERAISE:
            return 3;

        case WITH_EXCEPT_START:
            return 7;

        case LOAD_FAST:
            return 0;
        case STORE_FAST:
            return 1;
        case DELETE_FAST:
            return 0;

        case RAISE_VARARGS:
            return oparg;

        /* Functions and calls */
        case CALL_FUNCTION:
            return oparg+1;
        case CALL_METHOD:
            return oparg+2;
        case CALL_FUNCTION_KW:
            return oparg+2;
        case CALL_FUNCTION_EX:
            return 1 + ((oparg & 0x01) != 0);
        case MAKE_FUNCTION:
            return ((oparg & 0x01) != 0) + ((oparg & 0x02) != 0) +
                ((oparg & 0x04) != 0) + ((oparg & 0x08) != 0);
        case BUILD_SLICE:
            if (oparg == 3)
                return 3;
            else
                return 2;

        /* Closures */
        case LOAD_CLOSURE:
            return 0;
        case LOAD_DEREF:
        case LOAD_CLASSDEREF:
            return 0;
        case STORE_DEREF:
            return 1;
        case DELETE_DEREF:
            return 0;

        /* Iterators and generators */
        case GET_AWAITABLE:
            return 1;
        case SETUP_ASYNC_WITH:
            /* 0 in the normal flow.
             * Restore the stack position to the position before the result
             * of __aenter__ and push 6 values before jumping to the handler
             * if an exception be raised. */
            return 1;
        case BEFORE_ASYNC_WITH:
            return 1;
        case GET_AITER:
            return 1;
        case GET_ANEXT:
            return 1;
        case GET_YIELD_FROM_ITER:
            return 1;
        case END_ASYNC_FOR:
            return 7;
        case FORMAT_VALUE:
            /* If there's a fmt_spec on the stack, we go from 2->1,
               else 1->1. */
            return (oparg & FVS_MASK) == FVS_HAVE_SPEC ? 2 : 1;
        case LOAD_METHOD:
            return 1;
        case LOAD_ASSERTION_ERROR:
            return 0;
        default:
            return PY_INVALID_STACK_EFFECT;
    }
    return PY_INVALID_STACK_EFFECT; /* not reachable */
}

/* Return the stack effect of opcode with argument oparg.

   Some opcodes have different stack effect when jump to the target and
   when not jump. The 'jump' parameter specifies the case:

   * 0 -- when not jump
   * 1 -- when jump
   * -1 -- maximal
 */
/* XXX Make the stack effect of WITH_CLEANUP_START and
   WITH_CLEANUP_FINISH deterministic. */
static int
stack_effect(int opcode, int oparg, int jump)
{
    switch (opcode) {
        case NOP:
        case EXTENDED_ARG:
            return 0;

        /* Stack manipulation */
        case POP_TOP:
            return -1;
        case ROT_TWO:
        case ROT_THREE:
        case ROT_FOUR:
            return 0;
        case DUP_TOP:
            return 1;
        case DUP_TOP_TWO:
            return 2;

        /* Unary operators */
        case UNARY_POSITIVE:
        case UNARY_NEGATIVE:
        case UNARY_NOT:
        case UNARY_INVERT:
            return 0;

        case SET_ADD:
        case LIST_APPEND:
            return -1;
        case MAP_ADD:
            return -2;

        /* Binary operators */
        case BINARY_POWER:
        case BINARY_MULTIPLY:
        case BINARY_MATRIX_MULTIPLY:
        case BINARY_MODULO:
        case BINARY_ADD:
        case BINARY_SUBTRACT:
        case BINARY_SUBSCR:
        case BINARY_FLOOR_DIVIDE:
        case BINARY_TRUE_DIVIDE:
            return -1;
        case INPLACE_FLOOR_DIVIDE:
        case INPLACE_TRUE_DIVIDE:
            return -1;

        case INPLACE_ADD:
        case INPLACE_SUBTRACT:
        case INPLACE_MULTIPLY:
        case INPLACE_MATRIX_MULTIPLY:
        case INPLACE_MODULO:
            return -1;
        case STORE_SUBSCR:
            return -3;
        case DELETE_SUBSCR:
            return -2;

        case BINARY_LSHIFT:
        case BINARY_RSHIFT:
        case BINARY_AND:
        case BINARY_XOR:
        case BINARY_OR:
            return -1;
        case INPLACE_POWER:
            return -1;
        case GET_ITER:
            return 0;

        case PRINT_EXPR:
            return -1;
        case LOAD_BUILD_CLASS:
            return 1;
        case INPLACE_LSHIFT:
        case INPLACE_RSHIFT:
        case INPLACE_AND:
        case INPLACE_XOR:
        case INPLACE_OR:
            return -1;

        case SETUP_WITH:
            /* 1 in the normal flow.
             * Restore the stack position and push 6 values before jumping to
             * the handler if an exception be raised. */
            return jump ? 6 : 1;
        case RETURN_VALUE:
            return -1;
        case IMPORT_STAR:
            return -1;
        case SETUP_ANNOTATIONS:
            return 0;
        case YIELD_VALUE:
            return 0;
        case YIELD_FROM:
            return -1;
        case POP_BLOCK:
            return 0;
        case POP_EXCEPT:
            return -3;

        case STORE_NAME:
            return -1;
        case DELETE_NAME:
            return 0;
        case UNPACK_SEQUENCE:
            return oparg-1;
        case UNPACK_EX:
            return (oparg&0xFF) + (oparg>>8);
        case FOR_ITER:
            /* -1 at end of iterator, 1 if continue iterating. */
            return jump > 0 ? -1 : 1;

        case STORE_ATTR:
            return -2;
        case DELETE_ATTR:
            return -1;
        case STORE_GLOBAL:
            return -1;
        case DELETE_GLOBAL:
            return 0;
        case LOAD_CONST:
            return 1;
        case LOAD_NAME:
            return 1;
        case BUILD_TUPLE:
        case BUILD_LIST:
        case BUILD_SET:
        case BUILD_STRING:
            return 1-oparg;
        case BUILD_LIST_UNPACK:
        case BUILD_TUPLE_UNPACK:
        case BUILD_TUPLE_UNPACK_WITH_CALL:
        case BUILD_SET_UNPACK:
        case BUILD_MAP_UNPACK:
        case BUILD_MAP_UNPACK_WITH_CALL:
            return 1 - oparg;
        case BUILD_MAP:
            return 1 - 2*oparg;
        case BUILD_CONST_KEY_MAP:
            return -oparg;
        case LOAD_ATTR:
            return 0;
        case COMPARE_OP:
            return -1;
        case IMPORT_NAME:
            return -1;
        case IMPORT_FROM:
            return 1;

        /* Jumps */
        case JUMP_FORWARD:
        case JUMP_ABSOLUTE:
            return 0;

        case JUMP_IF_TRUE_OR_POP:
        case JUMP_IF_FALSE_OR_POP:
            return jump ? 0 : -1;

        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
            return -1;

        case LOAD_GLOBAL:
            return 1;

        /* Exception handling */
        case SETUP_FINALLY:
            /* 0 in the normal flow.
             * Restore the stack position and push 6 values before jumping to
             * the handler if an exception be raised. */
            return jump ? 6 : 0;
        case RERAISE:
            return -3;

        case WITH_EXCEPT_START:
            return 1;

        case LOAD_FAST:
            return 1;
        case STORE_FAST:
            return -1;
        case DELETE_FAST:
            return 0;

        case RAISE_VARARGS:
            return -oparg;

        /* Functions and calls */
        case CALL_FUNCTION:
            return -oparg;
        case CALL_METHOD:
            return -oparg-1;
        case CALL_FUNCTION_KW:
            return -oparg-1;
        case CALL_FUNCTION_EX:
            return -1 - ((oparg & 0x01) != 0);
        case MAKE_FUNCTION:
            return -1 - ((oparg & 0x01) != 0) - ((oparg & 0x02) != 0) -
                ((oparg & 0x04) != 0) - ((oparg & 0x08) != 0);
        case BUILD_SLICE:
            if (oparg == 3)
                return -2;
            else
                return -1;

        /* Closures */
        case LOAD_CLOSURE:
            return 1;
        case LOAD_DEREF:
        case LOAD_CLASSDEREF:
            return 1;
        case STORE_DEREF:
            return -1;
        case DELETE_DEREF:
            return 0;

        /* Iterators and generators */
        case GET_AWAITABLE:
            return 0;
        case SETUP_ASYNC_WITH:
            /* 0 in the normal flow.
             * Restore the stack position to the position before the result
             * of __aenter__ and push 6 values before jumping to the handler
             * if an exception be raised. */
            return jump ? -1 + 6 : 0;
        case BEFORE_ASYNC_WITH:
            return 1;
        case GET_AITER:
            return 0;
        case GET_ANEXT:
            return 1;
        case GET_YIELD_FROM_ITER:
            return 0;
        case END_ASYNC_FOR:
            return -7;
        case FORMAT_VALUE:
            /* If there's a fmt_spec on the stack, we go from 2->1,
               else 1->1. */
            return (oparg & FVS_MASK) == FVS_HAVE_SPEC ? -1 : 0;
        case LOAD_METHOD:
            return 1;
        case LOAD_ASSERTION_ERROR:
            return 1;
        default:
            return PY_INVALID_STACK_EFFECT;
    }
    return PY_INVALID_STACK_EFFECT; /* not reachable */
}

int
PyCompile_OpcodeStackEffectWithJump(int opcode, int oparg, int jump)
{
    return stack_effect(opcode, oparg, jump);
}

int
PyCompile_OpcodeStackEffect(int opcode, int oparg)
{
    return stack_effect(opcode, oparg, -1);
}

void cfgDump(ControlFlowGraph *cfg) {
    printf("\n%d instructions, %d blocks\n", cfg->instruction_count, cfg->block_count);
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *bb = &cfg->blocks[b];
        if (!bb->is_reachable) {
            printf("(Unreachable block %d)\n", b);
            continue;
        }
        printf("  Block %d (offset=%d, reachable=%d, exit=%d):\n", b, bb->assembler_offset, bb->is_reachable, bb->is_exit);
        for (int i = bb->b_start; i < bb->b_end; i++) {
            Instruction *inst = &cfg->instructions[i];
            if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                printf("    %d: %d -> %d @ %d (size = %d)\n", i, inst->i_opcode, inst->i_oparg, inst->i_lineno, inst->instruction_size);
            } else {
                printf("    %d: %d[%d] @ %d (size = %d)\n", i, inst->i_opcode, inst->i_oparg, inst->i_lineno, inst->instruction_size);
            }
        }
        if (bb->b_fallthrough > 0) {
            printf("    *fallthrough to %d\n", bb->b_fallthrough);
        }
        if (bb->is_exit) {
            printf("    *exit\n");
        }
    }
}


void 
_PyCfg_AssertFail(char *msg, ControlFlowGraph *cfg) {
    printf("Invalid cfg: (%s) is false.\n", msg);
    cfgDump(cfg);
    abort();
}


#define CFG_ASSERT(assertion) if (!(assertion)) { _PyCfg_AssertFail(#assertion, cfg); }

void cfgSanity(ControlFlowGraph *cfg) {
    for (int b = 0; b < cfg->block_count; b++) {
        cfg->blocks[b].grey = 0;
    }
    for (int b = 0; b < cfg->block_count; b++) {
        BasicBlock *bb = &cfg->blocks[b];
        CFG_ASSERT(bb->b_end >= bb->b_start);
        CFG_ASSERT(bb->b_end <= cfg->instruction_count);
        /* Must be terminated correctly */
        if (bb->b_fallthrough > 0) {
            CFG_ASSERT(bb->b_fallthrough < cfg->block_count);
            CFG_ASSERT(bb->b_end == bb->b_start || (cfg->instructions[bb->b_end-1].i_flags & INSTRUCTION_IS_TERMINATOR) == 0);
            /* Each block can be the target of at most one fallthrough */
            CFG_ASSERT(cfg->blocks[bb->b_fallthrough].grey == 0);
            cfg->blocks[bb->b_fallthrough].grey = 1;
        } else {
            CFG_ASSERT(bb->is_exit);
            CFG_ASSERT(cfg->instructions[bb->b_end-1].i_flags & INSTRUCTION_IS_TERMINATOR);
        }
        for (int i = bb->b_start; i < bb->b_end; i++) {
            Instruction *inst = &cfg->instructions[i];
            if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                CFG_ASSERT(inst->i_oparg < cfg->block_count);
            }
        }
    }
}


int
computeMaxStackDepth(ControlFlowGraph *cfg) {
    //cfgDump(cfg);
    cfgSanity(cfg);
    int max_depth = 0;
    int scan = 1;
    cfg->blocks[0].grey = 1;
    while (scan) {
        scan = 0;
        for (int i = 0; i < cfg->block_count; i++) {
            BasicBlock *block = &cfg->blocks[i];
            if (!block->is_reachable) {
                continue;
            }
            int stack_depth = block->stack_depth_at_start;
            if (stack_depth == INT_MIN) {
                scan = 1;
                continue;
            }
            CFG_ASSERT(stack_depth >= 0);
            if (block->grey == 0) {
                continue;
            }
            block->grey = 0;
            for (int i = block->b_start; i < block->b_end; i++) {
                if (stack_depth > max_depth) {
                    max_depth = stack_depth;
                }
                Instruction *inst = &cfg->instructions[i];
                int opcode = inst->i_opcode;
                int oparg = inst->i_oparg;
                CFG_ASSERT(stack_depth >= stack_input(opcode, oparg));
                if (inst->i_flags & INSTRUCTION_IS_BRANCH) {
                    int delta = stack_effect(opcode, oparg, 1);
                    CFG_ASSERT(delta > -8);
                    int target_depth = stack_depth + delta;
                    BasicBlock *target = &cfg->blocks[inst->i_oparg];
                    CFG_ASSERT(target->stack_depth_at_start == INT_MIN || target->stack_depth_at_start == target_depth);
                    if (target->stack_depth_at_start == INT_MIN) {
                        target->stack_depth_at_start = target_depth;
                        target->grey = 1;
                    }
                }
                stack_depth += stack_effect(opcode, oparg, 0);
                CFG_ASSERT(stack_depth >= 0);
            }
            if (block->b_fallthrough > 0) {
                BasicBlock *fallthrough = &cfg->blocks[block->b_fallthrough];
                if (fallthrough->stack_depth_at_start == INT_MIN) {
                    fallthrough->stack_depth_at_start = stack_depth;
                    fallthrough->grey = 1;
                }
            }
        }
    }
    return max_depth;
}
