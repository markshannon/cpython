
import opcode
import timeit

YIELD_UP = opcode.opmap["YIELD_UP"]
UNARY_POSITIVE = opcode.opmap["UNARY_POSITIVE"]

def convert_pos_to_yield_up(func):
    code = func.__code__.co_code
    for index, op in enumerate(code[::2]):
        if op == UNARY_POSITIVE:
            new_code = code[:index*2] + bytes([YIELD_UP]) + code[index*2+1:]
            func.__code__ = func.__code__.replace(co_code = new_code)
            break
    return func

@convert_pos_to_yield_up
def yieldUp(arg):
    return +arg

class Node:
    __slots__ = "left", "right", "value"

    def __init__(self, left, value, right):
        self.left = left
        self.value = value
        self.right = right

next_value = 0

def create_tree(depth):
    global next_value
    next_value += 1
    if depth > 1:
        return Node(create_tree(depth-1), next_value, create_tree(depth-1))
    else:
        return Node(None, next_value, None)

print("Creating tree")
TREE = create_tree(20)
print(f"Created tree of {next_value} nodes")

def gen_iter(tree):
    if tree is None:
        return
    yield from gen_iter(tree.left)
    yield tree
    yield from gen_iter(tree.right)

def count(iterator):
    t = 0
    for _ in iterator:
        t += 1
    return t

print("Generator:")
print(timeit.timeit("count(gen_iter(TREE))", globals=globals(), number=1))


def walk(tree):
    if tree is None:
        return
    walk(tree.left)
    yieldUp(tree)
    walk(tree.right)

def fiber_iter(tree):
    tree_walker = Fiber(walk)
    node = tree_walker.start(tree)
    while node is not None:
        yield node
        node = tree_walker.send(None)

print("Fiber (library only):")
print(timeit.timeit("count(fiber_iter(TREE))", globals=globals(), number=1))

@convert_pos_to_yield_up
def walk(tree):
    if tree is None:
        return
    walk(tree.left)
    +tree
    walk(tree.right)

print("Fiber (with custom syntax):")
print(timeit.timeit("count(fiber_iter(TREE))", globals=globals(), number=1))

def test_throw(tree):
    tree_walker = Fiber(walk)
    node = tree_walker.start(tree)
    for _ in range(10000):
        tree_walker.send(None)
    try:
        tree_walker.throw(Exception)
    except Exception as ex:
        import traceback
        traceback.print_exc()

