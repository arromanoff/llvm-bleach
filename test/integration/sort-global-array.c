// RUN: riscv64-unknown-linux-gnu-clang %s -O2 -o %t.out -march=rv64imfd \
// RUN:   -static -nostdlib -I %S/inputs
// RUN: %config-gen-path/config-gen.rb --march rv64imfd \
// RUN:   --template-dir %config-gen-path/templates -o %t.yaml
// RUN: %bin/llvm-bleach %t.out --instructions=%t.yaml \
// RUN:   --state-struct-file=%t.state.h > %t.lifted.ll
// RUN: sed 's|STATE|%t.state.h|g' %S/inputs/sort-global-array-main.c \
// RUN:   > %t.main.c
// RUN: clang %t.main.c %t.lifted.ll -o %t.native.out -I %S/inputs
// RUN: %t.native.out | FileCheck %s

// CHECK-NOT: FAILED

#include "sort-global-array.h"
