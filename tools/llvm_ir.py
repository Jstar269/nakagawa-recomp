# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""A structured LLVM IR builder that mirrors the LLVM C API data model (#330).

The LLVM backend (``tools/codegen_llvm.py``) never pastes IR text together. It
builds a module out of the same objects the LLVM C API exposes -- a module
(``LLVMModuleRef``) owns functions (``LLVMValueRef`` of function type), a function
owns parameters and basic blocks (``LLVMBasicBlockRef``), and a builder
(``LLVMBuilderRef``) appends typed instructions at the end of one block -- and
prints the finished module as textual IR only at the very end, for delivery.

Keeping that shape is what makes the delivery swappable (docs/LLVM_BACKEND.md):
today the printed module is compiled by an external Clang, and later the same
calls can be forwarded one for one to ``LLVMBuildAdd``/``LLVMAppendBasicBlock``
and friends in an in-process LLVM, with no change to the lowering.

The builder checks what LLVM's own verifier would reject before anything is
printed: operand types must agree, a branch condition is ``i1``, a call matches
its callee's function type, every block ends in exactly one terminator, no
instruction follows a terminator, and a branch never leaves its function. That
makes a malformed lowering a Python error at the offending call, not a verifier
message about printed text. ``opt -passes=verify`` still checks every module the
tests produce (tools/test_llvm_ir.py, tools/test_codegen_llvm.py).

Only the subset the backend uses is modelled. The printed IR keeps to syntax that
LLVM 15 and later accept (opaque pointers, no newer instruction flags), so the
public Linux runners' system LLVM can check it too.
"""

from __future__ import annotations

import re

# ---------------------------------------------------------------------------
# Types (LLVMTypeRef)
# ---------------------------------------------------------------------------


class Type:
    """An LLVM first-class or function type. Types compare by value."""

    def __eq__(self, other):
        return type(self) is type(other) and self._key() == other._key()

    def __hash__(self):
        return hash((type(self).__name__, self._key()))

    def _key(self):
        return ()

    def __repr__(self):
        return f"<{type(self).__name__} {self}>"


class VoidType(Type):
    def __str__(self):
        return "void"


class IntType(Type):
    def __init__(self, bits: int):
        if bits not in (1, 8, 16, 32, 64, 128):
            raise ValueError(f"unsupported integer width {bits}")
        self.bits = bits

    def _key(self):
        return (self.bits,)

    def __str__(self):
        return f"i{self.bits}"


class PointerType(Type):
    """The opaque pointer type ``ptr`` (address space 0)."""

    def __str__(self):
        return "ptr"


class FunctionType(Type):
    def __init__(self, ret: Type, params: tuple[Type, ...] | list[Type]):
        if isinstance(ret, FunctionType):
            raise TypeError("a function cannot return a function type")
        self.ret = ret
        self.params = tuple(params)
        for param in self.params:
            if isinstance(param, (VoidType, FunctionType)):
                raise TypeError(f"invalid parameter type {param}")

    def _key(self):
        return (self.ret, self.params)

    def __str__(self):
        return f"{self.ret} ({', '.join(str(p) for p in self.params)})"


void = VoidType()
i1 = IntType(1)
i8 = IntType(8)
i16 = IntType(16)
i32 = IntType(32)
i64 = IntType(64)
ptr = PointerType()


# ---------------------------------------------------------------------------
# Values (LLVMValueRef)
# ---------------------------------------------------------------------------

_IDENT = re.compile(r"[-a-zA-Z$._][-a-zA-Z$._0-9]*\Z")


def _symbol(sigil: str, name: str) -> str:
    if _IDENT.match(name):
        return f"{sigil}{name}"
    escaped = "".join(ch if 0x20 <= ord(ch) < 0x7F and ch not in '"\\' else f"\\{ord(ch):02X}"
                      for ch in name)
    return f'{sigil}"{escaped}"'


class Value:
    type: Type

    def ref(self) -> str:
        raise NotImplementedError

    def typed(self) -> str:
        return f"{self.type} {self.ref()}"


class ConstantInt(Value):
    def __init__(self, ty: IntType, value: int):
        if not isinstance(ty, IntType):
            raise TypeError(f"integer constant of non-integer type {ty}")
        mask = (1 << ty.bits) - 1
        self.type = ty
        self.value = value & mask           # stored as the unsigned bit pattern

    def signed(self) -> int:
        top = 1 << (self.type.bits - 1)
        return self.value - (1 << self.type.bits) if self.value & top else self.value

    def ref(self) -> str:
        if self.type.bits == 1:
            return "true" if self.value else "false"
        return str(self.signed())


class ConstantNull(Value):
    """``LLVMConstPointerNull``: the null ``ptr``."""

    type = PointerType()

    def ref(self) -> str:
        return "null"


def null() -> ConstantNull:
    return ConstantNull()


def const(ty: IntType, value: int) -> ConstantInt:
    """``LLVMConstInt``: the constant ``value`` of integer type ``ty`` (wraps)."""
    return ConstantInt(ty, value)


class Argument(Value):
    def __init__(self, function: "Function", ty: Type, index: int, name: str):
        self.function = function
        self.type = ty
        self.index = index
        self.name = name

    def ref(self) -> str:
        return _symbol("%", self.name)


class Instruction(Value):
    """One instruction: an opcode, a result type and its printed operand text."""

    def __init__(self, block: "BasicBlock", ty: Type, text, *, terminator=False,
                 successors=(), name: str | None = None):
        self.block = block
        self.type = ty
        self.text = text            # a callable: operands render when the module prints
        self.terminator = terminator
        self.successors = tuple(successors)
        self.name = name

    def ref(self) -> str:
        if self.name is None:
            raise TypeError("a void instruction has no value")
        return _symbol("%", self.name)

    def render(self) -> str:
        if self.name is None:
            return f"  {self.text()}"
        return f"  {_symbol('%', self.name)} = {self.text()}"


class Phi(Instruction):
    def __init__(self, block, ty, name):
        super().__init__(block, ty, lambda: "", name=name)
        self.incoming: list[tuple[Value, BasicBlock]] = []

    def add_incoming(self, value: Value, block: "BasicBlock") -> None:
        """``LLVMAddIncoming`` for one (value, predecessor) pair."""
        if value.type != self.type:
            raise TypeError(f"phi of {self.type} given {value.type}")
        if block.function is not self.block.function:
            raise ValueError("phi predecessor from another function")
        self.incoming.append((value, block))

    def render(self) -> str:
        if not self.incoming:
            raise ValueError(f"phi {self.name} has no incoming values")
        pairs = ", ".join(f"[ {v.ref()}, {b.ref()} ]" for v, b in self.incoming)
        return f"  {_symbol('%', self.name)} = phi {self.type} {pairs}"


# ---------------------------------------------------------------------------
# Basic blocks, functions and modules
# ---------------------------------------------------------------------------


class BasicBlock:
    def __init__(self, function: "Function", name: str):
        self.function = function
        self.name = name
        self.instructions: list[Instruction] = []

    @property
    def terminator(self) -> Instruction | None:
        if self.instructions and self.instructions[-1].terminator:
            return self.instructions[-1]
        return None

    def ref(self) -> str:
        return _symbol("%", self.name)


class Function(Value):
    """A function: a declaration until its first basic block is appended."""

    def __init__(self, module: "Module", name: str, fn_type: FunctionType,
                 param_names=(), attributes=(), linkage: str = ""):
        self.module = module
        self.name = name
        self.fn_type = fn_type
        self.type = ptr
        self.attributes = tuple(attributes)
        self.linkage = linkage
        names = list(param_names) or [f"a{i}" for i in range(len(fn_type.params))]
        if len(names) != len(fn_type.params):
            raise ValueError(f"{name}: {len(names)} parameter names for "
                             f"{len(fn_type.params)} parameters")
        self.params = [Argument(self, t, i, n) for i, (t, n) in enumerate(zip(fn_type.params, names, strict=True))]
        self.blocks: list[BasicBlock] = []
        self._block_names: set[str] = set()
        self._value_names: set[str] = {p.name for p in self.params}
        self._counter = 0

    @property
    def is_declaration(self) -> bool:
        return not self.blocks

    def ref(self) -> str:
        return _symbol("@", self.name)

    def append_basic_block(self, name: str) -> BasicBlock:
        """``LLVMAppendBasicBlock``. Block names are unique within the function."""
        if name in self._block_names:
            raise ValueError(f"{self.name}: duplicate block {name}")
        self._block_names.add(name)
        block = BasicBlock(self, name)
        self.blocks.append(block)
        return block

    def fresh_name(self, hint: str = "v") -> str:
        while True:
            self._counter += 1
            name = f"{hint}{self._counter}"
            if name not in self._value_names:
                self._value_names.add(name)
                return name

    def check(self) -> None:
        """The structural rules the builder can prove before printing."""
        if self.is_declaration:
            return
        for block in self.blocks:
            if block.terminator is None:
                raise ValueError(f"{self.name}: block {block.name} has no terminator")
            for inst in block.instructions[:-1]:
                if inst.terminator:
                    raise ValueError(f"{self.name}: terminator inside block {block.name}")
            for successor in block.terminator.successors:
                if successor.function is not self:
                    raise ValueError(f"{self.name}: branch from {block.name} leaves the function")
        entry = self.blocks[0]
        for block in self.blocks:
            for inst in block.instructions:
                if isinstance(inst, Phi) and block is entry:
                    raise ValueError(f"{self.name}: phi in the entry block")

    def render(self) -> list[str]:
        params = ", ".join(f"{p.type} {p.ref()}" if not self.is_declaration else str(p.type)
                           for p in self.params)
        attrs = (" " + " ".join(self.attributes)) if self.attributes else ""
        if self.is_declaration:
            return [f"declare {self.fn_type.ret} {self.ref()}({params}){attrs}"]
        self.check()
        linkage = f"{self.linkage} " if self.linkage else ""
        lines = [f"define {linkage}{self.fn_type.ret} {self.ref()}({params}){attrs} {{"]
        for index, block in enumerate(self.blocks):
            if index:
                lines.append("")
            lines.append(f"{_symbol('', block.name)}:")
            lines.extend(inst.render() for inst in block.instructions)
        lines.append("}")
        return lines


class Module:
    """``LLVMModuleRef``: named functions, printed definitions first.

    ``link_in`` merges another module the way ``LLVMLinkModules2`` would for the
    bodies this backend produces: a definition replaces a declaration of the same
    name, two definitions of one name are an error, and every use of a name must
    agree on its type. References stay attached to function objects, so
    ``rename_symbols`` renames a function and every call or address use of it.
    """

    def __init__(self, name: str):
        self.name = name
        self.functions: dict[str, Function] = {}
        self._members: list[Function] = []

    def add_function(self, name: str, fn_type: FunctionType, param_names=(),
                     attributes=(), linkage: str = "") -> Function:
        """``LLVMAddFunction``. A name is defined once; see ``get_or_declare``."""
        if name in self.functions:
            raise ValueError(f"function {name} already in module {self.name}")
        function = Function(self, name, fn_type, param_names, attributes, linkage)
        self.functions[name] = function
        self._members.append(function)
        return function

    def get_or_declare(self, name: str, fn_type: FunctionType, attributes=()) -> Function:
        """``LLVMGetNamedFunction``, else a new declaration. The type must agree."""
        existing = self.functions.get(name)
        if existing is not None:
            if existing.fn_type != fn_type:
                raise TypeError(f"{name} declared as {existing.fn_type}, used as {fn_type}")
            return existing
        return self.add_function(name, fn_type, attributes=attributes)

    def link_in(self, other: "Module") -> None:
        for function in other._members:
            self._members.append(function)
            present = self.functions.get(function.name)
            if present is None:
                self.functions[function.name] = function
                continue
            if present.fn_type != function.fn_type:
                raise TypeError(f"{function.name} is {present.fn_type} here and "
                                f"{function.fn_type} in {other.name}")
            if not function.is_declaration:
                if not present.is_declaration:
                    raise ValueError(f"{function.name} defined in {self.name} and {other.name}")
                self.functions[function.name] = function

    def rename_symbols(self, renames: dict[str, str]) -> None:
        if not renames:
            return
        for function in self._members:
            function.name = renames.get(function.name, function.name)
        self.functions = {f.name: f for f in self.functions.values()}

    def definitions(self) -> list[Function]:
        return [f for f in self.functions.values() if not f.is_declaration]

    def render(self) -> str:
        lines = [f"; ModuleID = '{self.name}'", f'source_filename = "{self.name}"', ""]
        for function in self.definitions():
            lines.extend(function.render())
            lines.append("")
        for function in self.functions.values():
            if function.is_declaration:
                lines.extend(function.render())
        lines.append("")
        return "\n".join(lines)


# ---------------------------------------------------------------------------
# The builder (LLVMBuilderRef)
# ---------------------------------------------------------------------------

ICMP_PREDICATES = frozenset({"eq", "ne", "ugt", "uge", "ult", "ule", "sgt", "sge", "slt", "sle"})


class Builder:
    """Appends instructions at the end of one block (``LLVMPositionBuilderAtEnd``)."""

    def __init__(self):
        self.block: BasicBlock | None = None
        self._index: int | None = None     # None: append at the end of the block

    def position_at_end(self, block: BasicBlock) -> None:
        self.block = block
        self._index = None

    def position_before(self, inst: Instruction) -> None:
        """``LLVMPositionBuilderBefore``: insert ahead of ``inst`` in its block."""
        self.block = inst.block
        self._index = inst.block.instructions.index(inst)

    @property
    def function(self) -> Function:
        return self._block().function

    def _block(self) -> BasicBlock:
        if self.block is None:
            raise ValueError("builder is not positioned")
        return self.block

    def _append(self, ty: Type, text: str, *, terminator=False, successors=(),
                hint: str = "v") -> Instruction:
        block = self._block()
        if self._index is None:
            if block.terminator is not None:
                raise ValueError(f"{block.function.name}: instruction after the terminator of {block.name}")
        elif terminator:
            raise ValueError("a terminator can only be appended at the end of a block")
        name = None if isinstance(ty, VoidType) else block.function.fresh_name(hint)
        inst = Instruction(block, ty, text, terminator=terminator, successors=successors, name=name)
        if self._index is None:
            block.instructions.append(inst)
        else:
            block.instructions.insert(self._index, inst)
            self._index += 1
        return inst

    def _own(self, *values: Value) -> None:
        function = self._block().function
        for value in values:
            if isinstance(value, (Argument, Instruction)):
                owner = value.function if isinstance(value, Argument) else value.block.function
                if owner is not function:
                    raise ValueError("operand defined in another function")

    # -- integer arithmetic ------------------------------------------------
    def _binop(self, opcode: str, a: Value, b: Value, hint: str) -> Instruction:
        if not isinstance(a.type, IntType) or a.type != b.type:
            raise TypeError(f"{opcode} of {a.type} and {b.type}")
        self._own(a, b)
        return self._append(a.type, lambda: f"{opcode} {a.type} {a.ref()}, {b.ref()}", hint=hint)

    def add(self, a, b, hint="add"): return self._binop("add", a, b, hint)
    def sub(self, a, b, hint="sub"): return self._binop("sub", a, b, hint)
    def mul(self, a, b, hint="mul"): return self._binop("mul", a, b, hint)
    def udiv(self, a, b, hint="udiv"): return self._binop("udiv", a, b, hint)
    def sdiv(self, a, b, hint="sdiv"): return self._binop("sdiv", a, b, hint)
    def urem(self, a, b, hint="urem"): return self._binop("urem", a, b, hint)
    def srem(self, a, b, hint="srem"): return self._binop("srem", a, b, hint)
    def and_(self, a, b, hint="and"): return self._binop("and", a, b, hint)
    def or_(self, a, b, hint="or"): return self._binop("or", a, b, hint)
    def xor(self, a, b, hint="xor"): return self._binop("xor", a, b, hint)
    def shl(self, a, b, hint="shl"): return self._binop("shl", a, b, hint)
    def lshr(self, a, b, hint="lshr"): return self._binop("lshr", a, b, hint)
    def ashr(self, a, b, hint="ashr"): return self._binop("ashr", a, b, hint)

    def icmp(self, predicate: str, a: Value, b: Value, hint="cmp") -> Instruction:
        if predicate not in ICMP_PREDICATES:
            raise ValueError(f"unknown icmp predicate {predicate}")
        if not isinstance(a.type, (IntType, PointerType)) or a.type != b.type:
            raise TypeError(f"icmp of {a.type} and {b.type}")
        self._own(a, b)
        return self._append(i1, lambda: f"icmp {predicate} {a.type} {a.ref()}, {b.ref()}", hint=hint)

    def select(self, cond: Value, a: Value, b: Value, hint="sel") -> Instruction:
        if cond.type != i1:
            raise TypeError(f"select condition of type {cond.type}")
        if a.type != b.type:
            raise TypeError(f"select of {a.type} and {b.type}")
        self._own(cond, a, b)
        return self._append(a.type, lambda: f"select i1 {cond.ref()}, {a.typed()}, {b.typed()}", hint=hint)

    def _cast(self, opcode: str, value: Value, to: IntType, wider: bool, hint: str) -> Instruction:
        if not isinstance(value.type, IntType) or not isinstance(to, IntType):
            raise TypeError(f"{opcode} from {value.type} to {to}")
        if wider and not to.bits > value.type.bits or not wider and not to.bits < value.type.bits:
            raise TypeError(f"{opcode} from {value.type} to {to}")
        self._own(value)
        return self._append(to, lambda: f"{opcode} {value.typed()} to {to}", hint=hint)

    def zext(self, v, to, hint="zext"): return self._cast("zext", v, to, True, hint)
    def sext(self, v, to, hint="sext"): return self._cast("sext", v, to, True, hint)
    def trunc(self, v, to, hint="trunc"): return self._cast("trunc", v, to, False, hint)

    # -- memory -------------------------------------------------------------
    def byte_gep(self, base: Value, offset: int, hint="p") -> Instruction:
        """``getelementptr inbounds i8, ptr base, i64 offset``: a field address."""
        if base.type != ptr:
            raise TypeError(f"GEP base of type {base.type}")
        self._own(base)
        return self._append(ptr, lambda: f"getelementptr inbounds i8, ptr {base.ref()}, i64 {int(offset)}",
                            hint=hint)

    def load(self, ty: Type, address: Value, align: int, hint="ld") -> Instruction:
        if address.type != ptr or isinstance(ty, (VoidType, FunctionType)):
            raise TypeError(f"load of {ty} through {address.type}")
        self._own(address)
        return self._append(ty, lambda: f"load {ty}, ptr {address.ref()}, align {align}", hint=hint)

    def store(self, value: Value, address: Value, align: int) -> Instruction:
        if address.type != ptr:
            raise TypeError(f"store through {address.type}")
        self._own(value, address)
        return self._append(void, lambda: f"store {value.typed()}, ptr {address.ref()}, align {align}")

    # -- calls ---------------------------------------------------------------
    def call(self, callee: Function, args: list[Value] | tuple[Value, ...], hint="call") -> Instruction:
        fn_type = callee.fn_type
        if len(args) != len(fn_type.params):
            raise TypeError(f"call to {callee.name} with {len(args)} of {len(fn_type.params)} arguments")
        for index, (arg, want) in enumerate(zip(args, fn_type.params, strict=True)):
            if arg.type != want:
                raise TypeError(f"call to {callee.name}: argument {index} is {arg.type}, wants {want}")
        if callee.module is not self.function.module:
            raise ValueError(f"call to {callee.name} from another module")
        self._own(*args)
        args = tuple(args)
        return self._append(
            fn_type.ret, lambda: f"call {fn_type.ret} {callee.ref()}({', '.join(a.typed() for a in args)})",
            hint=hint)

    # -- terminators ---------------------------------------------------------
    def _target(self, block: BasicBlock) -> BasicBlock:
        if block.function is not self.function:
            raise ValueError(f"branch to {block.name} in another function")
        return block

    def br(self, target: BasicBlock) -> Instruction:
        self._target(target)
        return self._append(void, lambda: f"br label {target.ref()}", terminator=True, successors=(target,))

    def cond_br(self, cond: Value, if_true: BasicBlock, if_false: BasicBlock) -> Instruction:
        if cond.type != i1:
            raise TypeError(f"branch condition of type {cond.type}")
        self._own(cond)
        self._target(if_true)
        self._target(if_false)
        return self._append(
            void, lambda: f"br i1 {cond.ref()}, label {if_true.ref()}, label {if_false.ref()}",
            terminator=True, successors=(if_true, if_false))

    def ret_void(self) -> Instruction:
        if self.function.fn_type.ret != void:
            raise TypeError(f"ret void from {self.function.name}")
        return self._append(void, lambda: "ret void", terminator=True)

    def unreachable(self) -> Instruction:
        return self._append(void, lambda: "unreachable", terminator=True)

    def phi(self, ty: Type, hint="phi") -> Phi:
        block = self._block()
        if any(not isinstance(inst, Phi) for inst in block.instructions):
            raise ValueError("phi after a non-phi instruction")
        inst = Phi(block, ty, block.function.fresh_name(hint))
        block.instructions.append(inst)
        return inst
