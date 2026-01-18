#include <STATE>

#include "rodata-lift-foo.h"
#include <stdio.h>
#include <string.h>

int main() {
  struct register_state s = {};
  bleached_main(&s);
  int reference = (int)foo(3);
  int64_t result = s.GPR[10];
  printf("result: %ld reference: %d\n", result, reference);
  return result != reference;
}
