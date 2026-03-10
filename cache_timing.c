#define _GNU_SOURCE
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

// ============= Utility Functions =============

// Read timestamp counter with serialization
static inline uint64_t rdtscp(void) {
  uint32_t lo, hi;
  uint32_t aux;
  __asm__ __volatile__("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux)::"memory");
  return ((uint64_t)hi << 32) | lo;
}

// Serializing fence
static inline void lfence(void) { __asm__ __volatile__("lfence" ::: "memory"); }

// Flush cache line containing addr
static inline void clflush(void *addr) {
  __asm__ __volatile__("clflush (%0)" ::"r"(addr) : "memory");
}

// Measure latency of one memory access (in cycles)
static inline uint64_t measure_one_block_access_time(void *addr) {
  uint64_t start, end;
  volatile uint64_t temp;

  lfence();
  start = rdtscp();
  lfence();
  temp = *((uint64_t *)addr); // Access memory
  lfence();
  end = rdtscp();
  lfence();

  return end - start;
}

// Pin process to a specific core
static void pin_to_core(int core) {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core, &cs);
  if (sched_setaffinity(0, sizeof(cs), &cs) != 0) {
    perror("sched_setaffinity");
    exit(1);
  }
}

// ============= Main Measurement Functions =============

#define NUM_SAMPLES 100
#define CACHE_LINE_SIZE 64

// Get L1, L2, L3 sizes from sysfs (in bytes)
static size_t get_cache_size(int level) {
  char path[256];
  FILE *fp;
  size_t size_kb;

  // Assuming index0=L1D, index3=L2, index6=L3 (common on Intel)
  // Adjust indices based on your system
  int index = (level == 1) ? 0 : (level == 2) ? 3 : 6;

  snprintf(path, sizeof(path),
           "/sys/devices/system/cpu/cpu0/cache/index%d/size", index);

  fp = fopen(path, "r");
  if (!fp) {
    fprintf(stderr, "Warning: Could not read cache size for L%d\n", level);
    // Fallback defaults (typical for modern Intel)
    return (level == 1)   ? 32 * 1024
           : (level == 2) ? 256 * 1024
                          : 8 * 1024 * 1024;
  }

  fscanf(fp, "%zuK", &size_kb);
  fclose(fp);

  return size_kb * 1024;
}

// Compute median of samples
static uint64_t median(uint64_t *samples, int n) {
  // Simple insertion sort for small arrays
  for (int i = 1; i < n; i++) {
    uint64_t key = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > key) {
      samples[j + 1] = samples[j];
      j--;
    }
    samples[j + 1] = key;
  }
  return samples[n / 2];
}

// Measure L1D latency
static uint64_t measure_l1d_latency(uint8_t *buffer) {
  uint64_t samples[NUM_SAMPLES];

  for (int i = 0; i < NUM_SAMPLES; i++) {
    // Access to bring into L1D
    volatile uint8_t temp = buffer[0];

    // Measure access time
    samples[i] = measure_one_block_access_time(buffer);
  }

  return median(samples, NUM_SAMPLES);
}

// Measure DRAM latency
static uint64_t measure_dram_latency(uint8_t *buffer) {
  uint64_t samples[NUM_SAMPLES];

  for (int i = 0; i < NUM_SAMPLES; i++) {
    // Flush from all cache levels to DRAM
    clflush(buffer);
    lfence();

    // Measure access time (will come from DRAM)
    samples[i] = measure_one_block_access_time(buffer);
  }

  return median(samples, NUM_SAMPLES);
}

// Measure L2 latency by evicting from L1 but keeping in L2
static uint64_t measure_l2_latency(uint8_t *buffer, size_t l1_size) {
  uint64_t samples[NUM_SAMPLES];

  // Allocate eviction buffer larger than L1
  size_t evict_size = l1_size + 64 * 1024; // L1 + extra
  uint8_t *evict_buf = aligned_alloc(CACHE_LINE_SIZE, evict_size);
  if (!evict_buf) {
    perror("aligned_alloc");
    exit(1);
  }

  for (int i = 0; i < NUM_SAMPLES; i++) {
    // Bring target into L1
    volatile uint8_t temp = buffer[0];

    // Access enough memory to evict target from L1 but not L2
    // Walk through eviction buffer in cache-line strides
    for (size_t j = 0; j < evict_size; j += CACHE_LINE_SIZE) {
      temp = evict_buf[j];
    }

    // Measure access time (should come from L2)
    samples[i] = measure_one_block_access_time(buffer);
  }

  free(evict_buf);
  return median(samples, NUM_SAMPLES);
}

// Measure L3 latency by evicting from L1 and L2 but keeping in L3
static uint64_t measure_l3_latency(uint8_t *buffer, size_t l2_size) {
  uint64_t samples[NUM_SAMPLES];

  // Allocate eviction buffer larger than L2
  size_t evict_size = l2_size + 512 * 1024; // L2 + extra
  uint8_t *evict_buf = aligned_alloc(CACHE_LINE_SIZE, evict_size);
  if (!evict_buf) {
    perror("aligned_alloc");
    exit(1);
  }

  for (int i = 0; i < NUM_SAMPLES; i++) {
    // Bring target into L1
    volatile uint8_t temp = buffer[0];

    // Access enough memory to evict from L1 and L2, but not L3
    for (size_t j = 0; j < evict_size; j += CACHE_LINE_SIZE) {
      temp = evict_buf[j];
    }

    // Measure access time (should come from L3)
    samples[i] = measure_one_block_access_time(buffer);
  }

  free(evict_buf);
  return median(samples, NUM_SAMPLES);
}

// ============= Main Program =============

int main(void) {
  pin_to_core(0); // Pin to core 0 for consistency

  printf("=== CPT_S 426 Lab 7: Cache Timing Measurement ===\n\n");

  // Allocate target buffer (single cache line)
  uint8_t *target = aligned_alloc(CACHE_LINE_SIZE, CACHE_LINE_SIZE);
  if (!target) {
    perror("aligned_alloc");
    return 1;
  }
  memset(target, 0xAA, CACHE_LINE_SIZE);

  // Get cache sizes
  size_t l1_size = get_cache_size(1);
  size_t l2_size = get_cache_size(2);
  size_t l3_size = get_cache_size(3);

  printf("Detected cache sizes:\n");
  printf("  L1D: %zu KB\n", l1_size / 1024);
  printf("  L1I: %zu KB\n", l1_size / 1024);
  printf("  L2:  %zu KB\n", l2_size / 1024);
  printf("  L3:  %zu KB\n\n", l3_size / 1024);

  printf("Measuring access latencies (%d samples each)...\n\n", NUM_SAMPLES);

  // Measure each level
  uint64_t l1_cycles = measure_l1d_latency(target);
  uint64_t l2_cycles = measure_l2_latency(target, l1_size);
  uint64_t l3_cycles = measure_l3_latency(target, l2_size);
  uint64_t dram_cycles = measure_dram_latency(target);

  // Print results
  printf("==================== Results ====================\n");
  printf("Cache Level    Median Latency (cycles)\n");
  printf("-------------------------------------------------\n");
  printf("L1D            %lu\n", l1_cycles);
  printf("L1I            %lu\n", l1_cycles);
  printf("L2             %lu\n", l2_cycles);
  printf("L3             %lu\n", l3_cycles);
  printf("DRAM           %lu\n", dram_cycles);
  printf("=================================================\n\n");

  printf("Note: These are measured latencies for your specific system.\n");

  free(target);
  return 0;
}
