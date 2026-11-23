% MCTOMIR(1) bleach mctomir user manual | Version 0.1.0 % Alexander Romanov % July 19, 2025

Machine code to LLVM MIR lifter tool.

# SYNOPSIS

```sh
mctomir ELF_FILE [OPTIONS]
```

# DESCRIPTION

mctomir is a simple tool to lift LLVM MIR code from target ELF file. Lifted MIR code can be used as input file of
llvm-bleach.

# Input formats

Input is an ELF object, executable or shared library with defined `STT_FUNC`
symbols in executable sections. Only symbol-covered code is lifted. A
zero-size symbol extends to the next function in the same section or the
section end. Aliases, overlapping ranges, duplicate names, invalid words
and truncation are errors. C67x+ requires little-endian ELF32 with
`EM_TI_C6000`; raw words and TI COFF are not CLI input. In particular the
installed TI CGT 8.3.15 `asm6x -mv6700` reference `.obj` is not ELF. Use
checked encoding bytes in an ELF fixture; CGT 8.3.15 is not an independent
validator for C67x+-only instructions.

## C67x+ structural MIR contract

For little-endian C6000 ELF, `C6_PACKET section-index, address, words, cycles`
introduces an execute packet. Its next `words` source instructions correspond
to consecutive 4-byte words. SPRU733A §2.1 and ch. 4 distinguish p-bit-linked
execute packets (up to eight words) from aligned 32-byte fetch packets. C67x+
execute packets may span fetch packets. The lifter checks packet length and
duplicate functional units (§3.7.1); it is not a general schedule verifier.
Zero cycles marks IDLE's indeterminate time until wake-up.

`C6_ISSUE raw-word, ...` retains the complete encoding and original decoded
branch operands (including predicate, displacement/register and delay). It
samples its inputs at issue. A commit refers to the issue by its address;
it does not read those registers again. `C6_COMMIT[_COND] issue-address, %bb`
transfers locally; `C6_EXIT[_COND] issue-address` represents an external or
indirect destination. Conditional commits also fall through. The five delay
cycles count execute packets, with a serial NOP consuming its count
(SPRU733A ch. 3, B and NOP). The current lifter rejects a transfer inside a
multicycle NOP, multicycle parallel NOPs, overlapping delayed branches,
entries into the middle of a packet or a delay window, a conditional commit
without an in-function continuation, and relocations against code. A direct
branch outside its function keeps its signed word displacement and original
PC (`PCE1 = PC & ~31`); it does not acquire an inter-function CFG edge.

These records support MIR printing, parsing and verification. They are not
inputs to optimization, scheduling or executable lowering. Parallel operations
read packet-start registers; conflicting writes, repeated reads and delayed
results require architectural timing. False predicates preserve destinations,
including load results and base writeback. The opcode distinguishes 40-bit
integers from 64-bit register pairs. AMR circular addressing, saturation,
floating-point and interrupt state have incomplete dependencies; see the
backend's `BackendOverview.md` and SPRU733A instruction pipeline tables.

For example, Table 4-2 gives these **result delay slots**, distinct from the
per-packet issue time recorded here: single-cycle and stores 0; 16×16
multiply, two-cycle DP and DP compare 1; four-cycle operations 3; loads and
INTDP 4; ADDDP/SUBDP 6; MPYI 8; MPYID/MPYDP 9; MPYSPDP 6; MPYSP2DP 4.
MPYI/MPYID read operands through E4; MPYDP reads different halves through
E4, and MPYSPDP rereads its scalar input in E2. These are opcode semantics,
not additional synthetic reads/writes emitted by mctomir.

# OPTIONS

**--help**

```
Display available options.
```

**-o \<PATH>**

```
Output file for LLVM MIR.
```

**-symtab-file=\<PATH>**

```
File to save YAML-serialized symbol table to. This file can be used later by
llvm-bleach for lifting indirect branches.
```

# EXAMPLES

1. Lift ELF code from ./myfunc.elf and print lifted LLVM MIR on stdout.

```sh
mctomir ./myfunc.elf -o -
```

1. Lift ELF code from ./riscv-foo.elf file, print lifted LLVM MIR to ./riscv-foo.mir.

```sh
mctomir ./riscv-foo.elf -o ./riscv-foo.mir
```

1. Lift ELF code from ./riscv-foo.elf file, print lifted LLVM MIR to ./riscv-foo.mir. Save symbol table to symtab.yaml

```sh
mctomir ./riscv-foo.elf -o ./riscv-foo.mir -symtab-file=symtab.yaml
```
