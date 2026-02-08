#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
// generated header
#include <STATE>

#include "sort-global-array.h"

double uitod(int64_t n) {
  double res = 0.0;
  memcpy(&res, &n, sizeof(res));
  return res;
}

int64_t dtosi(double n) {
  int64_t res = 0;
  memcpy(&res, &n, sizeof(res));
  return res;
}

void run_test() {
  struct register_state regs = {};
  printf("BEFORE\n");
  bleached_test(&regs);
  printf("AFTER\n");
  long long res = regs.GPR[10];
  long long ans = test();
  printf("result = %llx; correct = %llx\n", res, ans);
  if (res != ans)
    printf("FAILED\n");
}

int main() {
  run_test();
  return 0;
}
