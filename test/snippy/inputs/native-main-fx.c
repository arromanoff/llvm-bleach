#include <STATE>
#include <stdio.h>
#include <string.h>

void print_one(uint64_t val) { printf("DEBUG: %lx\n", val); }

void bleached_print(struct register_state *st) {
  printf("PRINT: %lx %lx %lx %lx %lx %lx %lx %lx\n", st->GPR[10], st->GPR[11],
         st->GPR[12], st->GPR[13], st->FPR[10], st->FPR[11], st->FPR[12],
         st->FPR[13]);
}

int main() {
  struct register_state regs = {};
  bleached_SnippyFunction(&regs);
  return 0;
}
