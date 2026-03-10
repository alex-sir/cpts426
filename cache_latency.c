#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <x86intrin.h> // for __rdtscp

typedef struct {
  size_t size_bytes;
  double cycles_per_access;
} sample_t;

static void pin_to_core(int core) {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core, &cs);
  if (sched_setaffinity(0, sizeof(cs), &cs) != 0) {
    perror("sched_setaffinity");
  }
}

// measure cycles for N dependent loads over an array of given size
static double measure_latency(size_t array_size_bytes, size_t stride) {
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

static double avg_latency_in_range(sample_t *samples, int n, size_t min_kb,
                                   size_t max_kb) {
  double sum = 0.0;
  int count = 0;
  for (int i = 0; i < n; i++) {
    size_t kb = samples[i].size_bytes / 1024;
    if (kb >= min_kb && kb <= max_kb) {
      sum += samples[i].cycles_per_access;
      count++;
    }
  }
  if (count == 0)
    return 0.0;
  return sum / count;
}

int main(void) {
  pin_to_core(0); // keep it on one core

  size_t sizes[] = {4 * 1024,         8 * 1024,        16 * 1024,
                    32 * 1024,        64 * 1024,       128 * 1024,
                    256 * 1024,       512 * 1024,      1024 * 1024,
                    2 * 1024 * 1024,  4 * 1024 * 1024, 8 * 1024 * 1024,
                    16 * 1024 * 1024, 32 * 1024 * 1024};
  const int N = (int)(sizeof(sizes) / sizeof(sizes[0]));
  sample_t samples[N];

  printf("Running latency sweep (pointer chasing)...\n\n");
  printf("%-8s %-16s\n", "Size(KB)", "Cycles/access");
  printf("--------------------------------\n");

  for (int i = 0; i < N; i++) {
    double c = measure_latency(sizes[i], 64); // 64B stride ~ one line
    samples[i].size_bytes = sizes[i];
    samples[i].cycles_per_access = c;
    printf("%-8zu %-16.2f\n", sizes[i] / 1024, c);
  }

  printf("\n");
  printf("Now choose size ranges (in KB) that best match each cache level.\n");
  printf("For a typical Xeon: L1D ~ 32KB, L2 ~ 1MB, L3 ~ tens of MB.\n");
  printf("Use the table above as a guide (look for plateaus).\n\n");

  size_t l1_min, l1_max, l2_min, l2_max, l3_min, l3_max;

  printf("Enter L1D min KB (e.g., 4): ");
  scanf("%zu", &l1_min);
  printf("Enter L1D max KB (e.g., 32): ");
  scanf("%zu", &l1_max);

  printf("Enter L2  min KB (e.g., 64): ");
  scanf("%zu", &l2_min);
  printf("Enter L2  max KB (e.g., 1024): ");
  scanf("%zu", &l2_max);

  printf("Enter L3  min KB (e.g., 2048): ");
  scanf("%zu", &l3_min);
  printf("Enter L3  max KB (e.g., 32768): ");
  scanf("%zu", &l3_max);

  double l1_cycles = avg_latency_in_range(samples, N, l1_min, l1_max);
  double l2_cycles = avg_latency_in_range(samples, N, l2_min, l2_max);
  double l3_cycles = avg_latency_in_range(samples, N, l3_min, l3_max);

  printf("\n================ Raw Latency Summary ================\n");
  printf("  L1D (avg over %zu–%zu KB):  %.2f cycles/access\n", l1_min, l1_max,
         l1_cycles);
  printf("  L2  (avg over %zu–%zu KB):  %.2f cycles/access\n", l2_min, l2_max,
         l2_cycles);
  printf("  L3  (avg over %zu–%zu KB):  %.2f cycles/access\n", l3_min, l3_max,
         l3_cycles);
  printf("=====================================================\n\n");

  printf("Note: this benchmark measures *data* loads (L1D/L2/L3).\n");
  printf("L1I (instruction cache) latency is similar in magnitude on modern "
         "Intel CPUs,\n");
  printf("but is typically characterized with different microbenchmarks.\n");

  return 0;
}
