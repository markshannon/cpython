#ifndef Py_INTERNAL_CFG_H
#define Py_INTERNAL_CFG_H

#ifdef __cplusplus
extern "C" {
#endif
    

#define INSTRUCTION_IS_BRANCH 1
#define INSTRUCTION_IS_TERMINATOR 2
#define INSTRUCTION_END_OF_JUMP_LIST 4
    
#define CFG_DUMP 1

typedef struct instr {
    unsigned char i_opcode;
    unsigned char i_flags;
    unsigned int sentinel1;
    unsigned char instruction_size;  /* Used by assembler */
    unsigned int sentinel2;
    unsigned int i_oparg;
    int i_lineno;
} Instruction;

/*
 * This file contains code to create and manipulate the control flow graph.
 */

typedef struct basicblock_ {
    /* Index of first instruction in basic block */
    int b_start;
    /* Index of last instruction plus one in basic block */
    int b_end;
    /* Index of fall-through basic block, -1 if no fall-through */
    int b_fallthrough;
    /* Flags */
    unsigned char is_reachable;
    unsigned char grey; /* Marker for worklist algorithms. */
    unsigned char is_exit;
    int stack_depth_at_start;
    int assembler_offset; /* Used by assembler */
    int assembler_size; /* Used by assembler */
} BasicBlock;


typedef struct cfg_ {
    Instruction *instructions;
    BasicBlock *blocks;
    int instruction_count; 
    int block_count;
    int allocated_blocks;
} ControlFlowGraph;


typedef struct _assembler {
    PyObject *a_bytecode;  /* string containing bytecode */
    int a_offset;          /* offset into bytecode */
    PyObject *a_lnotab;    /* string containing lnotab */
    int a_lnotab_off;      /* offset into lnotab */
    int a_lineno;          /* last lineno of emitted instruction */
    int a_lineno_off;      /* bytecode offset of last lineno */
} Assembler;


ControlFlowGraph *createCfg(Instruction *instructions, int count);

void _PyCfg_MarkReachable(ControlFlowGraph *cfg);

int _PyCfg_CopyBlock(ControlFlowGraph *cfg, int block);

int _Py_Optimize(ControlFlowGraph *cfg, PyObject* consts);

void _Py_FreeCfg(ControlFlowGraph *cfg);

void _Py_FreeAssembler(Assembler *assembler);

int
computeMaxStackDepth(ControlFlowGraph *cfg);

Assembler *assemble(ControlFlowGraph *cfg, int firstline);


/** Gets the first instruction that occurs not before the start of
 * this basic block. Accounts for empty blocks. 
 */
static inline Instruction *
first_instruction(ControlFlowGraph *cfg, int b) {
    BasicBlock *block = &cfg->blocks[b];
    while(block->b_end == block->b_start) {
        /* A block can only become empty if it doesn't end in an exit,
         * so there must be a fallthrough */
        assert(block->b_fallthrough > 0);
        block = &cfg->blocks[block->b_fallthrough];
    }
    return &cfg->instructions[block->b_start];
}

#ifdef NDEBUG

#define CFG_ASSERT(assertion) ((void)0)

#else
extern void _PyCfg_AssertFail(char *msg, ControlFlowGraph *cfg);

#define CFG_ASSERT(assertion) if (!(assertion)) { _PyCfg_AssertFail(#assertion, cfg); }

#endif


#ifdef __cplusplus
}
#endif

#endif /* Py_INTERNAL_CFG_H */
