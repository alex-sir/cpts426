#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <x86intrin.h> // for __rdtscp

static void pin_to_core(int core) {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core, &cs);
  if (sched_setaffinity(0, sizeof(cs), &cs) != 0) {
    perror("sched_setaffinity");
  }
}

// measure cycles for N dependent loads over an array of given size
double measure_latency(size_t array_size_bytes, size_t stride) {
  size_t n = array_size_bytes / sizeof(uint64_t);
  if (n < 2)
    n = 2;

  uint64_t *a;
  if (posix_memalign((void **)&a, 64, n * sizeof(uint64_t)) != 0) {
    perror("posix_memalign");
    exit(1);
  }

  // build pointer-chasing ring
  for (size_t i = 0; i < n; i++) {
    a[i] = ((i + stride / sizeof(uint64_t)) % n);
  }

  // warm up
  volatile uint64_t idx = 0;
  for (size_t i = 0; i < 100000; i++) {
    idx = a[idx];
  }

  unsigned int aux;
  uint64_t start = __rdtscp(&aux);
  size_t iters = 1000000;
  for (size_t i = 0; i < iters; i++) {
    idx = a[idx];
  }
  uint64_t end = __rdtscp(&aux);

  double cycles_per_access = (double)(end - start) / iters;
  free(a);
  return cycles_per_access;
}

int main(void) {
  pin_to_core(0); // keep it on one core

  size_t sizes[] = {4 * 1024,        8 * 1024,        16 * 1024,  32 * 1024,
                    64 * 1024,       256 * 1024,      512 * 1024, 1024 * 1024,
                    4 * 1024 * 1024, 16 * 1024 * 1024};

  printf("# size(KB)\tcycles/access\n");
  for (int i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
    double c = measure_latency(sizes[i], 64); // 64B stride ~ one cache line
    printf("%6zu\t\t%.2f\n", sizes[i] / 1024, c);
  }

  return 0;
}
