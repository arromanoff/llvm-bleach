#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
// generated header
#include <STATE>

#include "pi-calculation.h"

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

double lifted_calculate_pi(struct register_state *regs) {
  // According to RISC-V calling convention F10 is a return value
  bleached_calculate_pi(regs);
  return uitod(regs->FPR[10]);
}

void test() {
  struct register_state regs = {};
  double res = lifted_calculate_pi(&regs);
  double ans = calculate_pi();
  printf("pi() = %lf; correct = %lf\n", res, ans);
  if (fabs(res - ans) > 1e-5)
    printf("FAILED\n");
}

int main() {
  test();
  return 0;
}
