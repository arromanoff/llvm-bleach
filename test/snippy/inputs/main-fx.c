#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t bitcast(double v) {
  uint64_t r;
  memcpy(&r, &v, sizeof(double));
  return r;
}

extern void SnippyFunction();

void print(int64_t x10, int64_t x11, int64_t x12, int64_t x13, double d10,
           double d11, double d12, double d13) {
  printf("PRINT: %lx %lx %lx %lx %lx %lx %lx %lx\n", x10, x11, x12, x13,
         bitcast(d10), bitcast(d11), bitcast(d12), bitcast(d13));
}

int main() {
  SnippyFunction();
  return 0;
}
