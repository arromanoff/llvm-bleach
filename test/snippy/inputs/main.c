#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t bitcast(double v) {
  uint64_t r;
  memcpy(&r, &v, sizeof(double));
  return r;
}

extern void SnippyFunction();

void print(double d10, double d11, double d12, double d13) {
  printf("PRINT: %lx %lx %lx %lx\n", bitcast(d10), bitcast(d11), bitcast(d12),
         bitcast(d13));
}

int main() {
  SnippyFunction();
  return 0;
}
