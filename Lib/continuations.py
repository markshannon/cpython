
import _continuations
import opcode

def replace_opcode(op, replacement):
    def deco(func):
        code = func.__code__
        new_bytecode = code.co_code.replace(bytes([opcode.opmap[op]]), bytes([opcode.opmap[replacement]]))
        new_code = code.replace(co_code = new_bytecode)
        func.__code__ = new_code
        return func
    return deco

class Continuation(_continuations._Continuation):

    __slots__ = ()

    @replace_opcode("IS_OP", "RESUME_CONTINUATION")
    def send(self, value):
        return self is value

    @staticmethod
    @replace_opcode("UNARY_NEGATIVE", "PAUSE_CONTINUATION")
    def pause(value):
        return -value

