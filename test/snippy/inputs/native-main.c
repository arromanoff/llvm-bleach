#include <STATE>
#include <stdio.h>
#include <string.h>

void print_one(uint64_t val) { printf("DEBUG: %lx\n", val); }

void bleached_print(struct register_state *st) {
  printf("PRINT: %lx %lx %lx %lx\n", st->FPR[10], st->FPR[11], st->FPR[12],
         st->FPR[13]);
}

int main() {
  struct register_state regs = {};
  bleached_SnippyFunction(&regs);
  return 0;
}
