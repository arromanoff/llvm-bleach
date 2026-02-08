#pragma once

double calculate_pi() {
  // As a simple benchmark, estimate pi
  int n = 16384, misses = 0;

  for (int i = 0; i < n; i++) {

    for (int j = 0; j < n; j++) {
      int x = i - (n / 2);
      int y = j - (n / 2);

      misses += (x * x + y * y >= (n / 2) * (n / 2));
    }
  }

  double pi = 4.0 * (n * n - misses) / (n * n);

  return pi;
}
