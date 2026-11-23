"""Exercise fail-closed boundaries with small ELF32/C6000 objects."""

import pathlib
import subprocess
import sys

yaml2obj, lifter, tmp = sys.argv[1:]
output = pathlib.Path(tmp + ".mir")


def reject(words, diagnostic, *, extra_symbol="", size=0, reloc=False):
    yaml = '''--- !ELF
FileHeader: {Class: ELFCLASS32, Data: ELFDATA2LSB, Type: ET_REL, Machine: EM_TI_C6000}
Sections:
  - Name: .text
    Type: SHT_PROGBITS
    Flags: [ SHF_ALLOC, SHF_EXECINSTR ]
    Content: "''' + words + '"\n'
    if reloc:
        yaml += """  - Name: .rel.text
    Type: SHT_REL
    Info: .text
    Link: .symtab
    Relocations:
      - {Offset: 0, Symbol: f, Type: 1}
"""
    yaml += f"Symbols:\n  - {{Name: f, Type: STT_FUNC, Section: .text, Size: {size}}}\n"
    if extra_symbol:
        yaml += f"  - {{Name: other, Type: STT_FUNC, Section: .text, Value: {extra_symbol}}}\n"
    subprocess.run([yaml2obj, "-o", tmp], input=yaml, text=True, check=True)
    output.unlink(missing_ok=True)
    result = subprocess.run([lifter, tmp, "-o", output], capture_output=True, text=True)
    if result.returncode != 1 or diagnostic not in result.stderr or output.exists():
        raise RuntimeError(f"{words}: expected {diagnostic!r}, got {result}")


# SPRU733A B/NOP encodings; reserved load mode from extended-invalid.txt.
reject("000000", "invalid or truncated instruction")
reject("26640802", "invalid or truncated instruction")
reject("01000000", "incomplete or oversized execute packet")
reject("01000000" * 8 + "00000000", "oversized execute packet")
reject("A9FF7F08A8FF7F08", "functional unit used twice")
reject("10000000", "incomplete branch delay window")
reject("1000000010000000", "overlapping delayed branches")
reject("1000000000000100", "transfer inside multicycle NOP")
reject("9000000000800000", "entry into packet or delay window")
reject("01000000000000009000000000800000", "entry into packet or delay window")
reject("00000000", "function range outside executable section", size=8)
reject("00000000", "aliased or overlapping function", extra_symbol="0")
reject("0000000000000000", "aliased or overlapping function", size=8, extra_symbol="4")
reject("1000000000800000", "executable section relocation", reloc=True)
