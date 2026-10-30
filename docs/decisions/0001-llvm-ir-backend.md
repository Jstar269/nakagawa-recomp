# 0001: The direct backend lowers to LLVM IR

Date: 2026-10-29
Status: Accepted (owner)

## Context

Today every function goes from MIPS to C, and an external C compiler turns the C into machine code. #330 asks for
a direct backend that lowers Nakagawa's own analysis to native code. Three routes were weighed: LLVM IR compiled
by the clang that the MSYS2 toolchain already carries, an in-process Cranelift engine, and a project-written x86-64
encoder.

## Decision

The first direct backend emits LLVM IR. The lowering is written so it can later run in process (for example
through LLVM's own JIT and object APIs) without changing its input or its rules. Cranelift and a hand-written
encoder are not pursued now.

## Consequences

- No new language or toolchain enters the build; clang 23 is already installed with MSYS2.
- The backend, not a C compiler, decides how delay slots, guest-register write-backs and state hand-offs are
  emitted, which makes those rules testable on their own.
- Cranelift would bring a Rust toolchain into the build; that stays out until a later decision.
- The C emitter stays as the reference path. The backend is opt-in, and equivalence with the interpreter in the
  cosimulation corpus is a hard veto.

## Links

- Issue [#330](https://github.com/Jstar269/nakagawa-recomp/issues/330); first stage in
  [#1663](https://github.com/Jstar269/nakagawa-recomp/pull/1663).
- [`docs/AOT_PRODUCTIZATION_ARCHITECTURE.md`](../AOT_PRODUCTIZATION_ARCHITECTURE.md) (Option E).
