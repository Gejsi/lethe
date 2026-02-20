#include <benchmark/benchmark.h>
#include <cstdio>
#include <cstdlib>

#include "bump_map.h"
#include "std_map.h"
#include "swapper.h"

// LD_PRELOAD=lib/liblethe.so bin/run_benchmark 1 10000 100000 zipfian 0
int main(int argc, char **argv) {
  if (argc < 6) {
    printf("Usage: main <NUM_THREADS> <LOAD_NUM_KEYS> <NUM_OPS> <DISTRIBUTION> "
           "<WORKLOAD>\n<DISTRIBUTION> = uniform | zipf\n");
    exit(EXIT_FAILURE);
  }

  const u32 num_threads = (u32)atoi(((char **)argv)[1]);
  const u32 load_num_keys = (u32)atoi(((char **)argv)[2]);
  const u32 num_ops = (u32)atoi(((char **)argv)[3]);
  const enum distribution distribution =
      strcmp(((char **)argv)[4], "uniform") == 0 ? UNIFORM : ZIPFIAN;
  const u8 workload = (u8)atoi(((char **)argv)[5]);

  s_benchmark_config_t bench_config{.num_threads = num_threads,
                                    .load_num_keys = load_num_keys,
                                    .num_ops = num_ops,
                                    .distribution = distribution,
                                    .workload = workload,
                                    .output_file = "./data/outputfile",
                                    .data_dir = "./data",
                                    .tsc = 0,
                                    .metric = METRIC::THROUGHPUT,
                                    .hook = NULL,
                                    .args = NULL};

  // BumpMapDataLayer data_layer(HEAP_START, 0);
  StdMap data_layer;
  run_benchmark(&bench_config, &data_layer);

  return EXIT_SUCCESS;
}
