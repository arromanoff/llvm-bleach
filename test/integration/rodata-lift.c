// RUN: %config-gen-path/config-gen.rb --march rv64imfd \
// RUN:   --template-dir %config-gen-path/templates -o %t.yaml
// RUN: riscv64-unknown-linux-gnu-clang %s -O2 -o %t.out -march=rv64imfd \
// RUN:   -nostdlib -I %S/inputs
// RUN: %bin/llvm-bleach %t.out --instructions %t.yaml \
// RUN:   --state-struct-file %t.state.h > %t.ll
// RUN: sed 's|STATE|%t.state.h|g' %S/inputs/rodata-lift-bleached-main.c > \
// RUN:   %t.main.c
// RUN: clang %t.main.c %t.ll -o %t.native.out -I %S/inputs
// RUN: %t.native.out

#include "rodata-lift-foo.h"

int main() { return foo(3); }
