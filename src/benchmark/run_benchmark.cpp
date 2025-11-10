#include <benchmark/benchmark.h>
#include <cstdio>

#include "dummy_interface.h"
#include "heap_map.h"
// #include "lethe_map.h"
#include "std_map.h"

// bin/run_benchmark 1 10000 100000 uniform 0
int main(int argc, char **argv) {
  if (argc < 6) {
    printf("Usage: main <NUM_THREADS> <LOAD_NUM_KEYS> <NUM_OPS> <DISTRIBUTION> "
           "<WORKLOAD>\n<DISTRIBUTION> = uniform | zipf\n");
    exit(1);
  }

  const unsigned int num_threads = (unsigned int)atoi(((char **)argv)[1]);
  const unsigned int load_num_keys = (unsigned int)atoi(((char **)argv)[2]);
  const unsigned int num_ops = (unsigned int)atoi(((char **)argv)[3]);
  const enum distribution distribution =
      strcmp(((char **)argv)[4], "uniform") == 0 ? UNIFORM : ZIPFIAN;
  const uint8_t workload = (uint8_t)atoi(((char **)argv)[5]);
  if (workload > 3) {
    printf("error: only 4 workloads are supported\n");
    exit(1);
  }
  s_benchmark_config_t bench_config{.num_threads = num_threads,
                                    .load_num_keys = load_num_keys,
                                    .num_ops = num_ops,
                                    .distribution = distribution,
                                    .workload = workload,
                                    .output_file = "./data/outputfile",
                                    .data_dir = "./data",
                                    .tsc = 2095008,
                                    .metric = METRIC::THROUGHPUT,
                                    .hook = NULL,
                                    .args = NULL};

  StdMap data_layer;
  run_benchmark(&bench_config, &data_layer);
  // data_layer.insert(69, 420);
  // printf("Hello! %lu\n", data_layer.get(69));
  // printf("Bu! %lu\n", data_layer.dummy());
}
