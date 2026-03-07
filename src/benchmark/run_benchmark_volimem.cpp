#include <benchmark/benchmark.h>
#include <cstdio>
#include <cstdlib>
#include <getopt.h>
#include <volimem/volimem.h>

#include "benchmark/std_map.h"
#include "types.h"

static void print_usage(const char *prog) {
  printf("Usage: %s [options]\n\n", prog);
  printf("Benchmark options:\n");
  printf("  -t, --threads <n>      Number of threads (default: 1)\n");
  printf(
      "  -k, --keys <n>         Number of keys to load (default: 60000000)\n");
  printf("  -o, --ops <n>          Number of operations (default: 20000000)\n");
  printf("  -d, --dist <type>      Distribution: uniform|zipfian (default: "
         "zipfian)\n");
  printf("  -w, --workload <A-D>   YCSB workload: A|B|C|D (default: A)\n");
  printf("\n  -h, --help             Show this help message\n");
}

static u8 parse_workload(const char *arg) {
  if (strlen(arg) == 1) {
    switch (arg[0]) {
    case 'A':
    case 'a':
    case '0':
      return 0;
    case 'B':
    case 'b':
    case '1':
      return 1;
    case 'C':
    case 'c':
    case '2':
      return 2;
    case 'D':
    case 'd':
    case '3':
      return 3;
    default:
      break;
    }
  }
  fprintf(stderr, "Invalid workload '%s'. Choose A, B, C, or D\n", arg);
  exit(EXIT_FAILURE);
}

void virtual_main(void *any) {
  s_benchmark_config_t *bench_config = (s_benchmark_config_t *)any;

  StdMap data_layer;
  run_benchmark(bench_config, &data_layer);
}

int main(int argc, char **argv) {
  u32 num_threads = 1;
  u32 load_num_keys = 60000000;
  u32 num_ops = 20000000;
  enum distribution dist = ZIPFIAN;
  u8 workload = 0;

  int option_index = 0;
  struct option long_options[] = {{"threads", required_argument, 0, 't'},
                                  {"keys", required_argument, 0, 'k'},
                                  {"ops", required_argument, 0, 'o'},
                                  {"dist", required_argument, 0, 'd'},
                                  {"workload", required_argument, 0, 'w'},
                                  {"help", no_argument, 0, 'h'},
                                  {0, 0, 0, 0}};

  int option;
  while ((option = getopt_long(argc, argv, "t:k:o:d:w:h", long_options,
                               &option_index)) != -1) {
    switch (option) {
    case 't':
      num_threads = (u32)atoi(optarg);
      break;
    case 'k':
      load_num_keys = (u32)atol(optarg);
      break;
    case 'o':
      num_ops = (u32)atol(optarg);
      break;
    case 'd':
      dist = strcmp(optarg, "uniform") == 0 ? UNIFORM : ZIPFIAN;
      break;
    case 'w':
      workload = parse_workload(optarg);
      break;
    case 'h':
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    default:
      print_usage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  s_benchmark_config_t bench_config{.num_threads = num_threads,
                                    .load_num_keys = load_num_keys,
                                    .num_ops = num_ops,
                                    .distribution = dist,
                                    .workload = workload,
                                    .output_file = "./data/outputfile",
                                    .data_dir = "./data",
                                    .tsc = 0,
                                    .metric = METRIC::THROUGHPUT,
                                    .hook = NULL,
                                    .args = NULL};

  constexpr s_volimem_config_t voli_config{
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
      .print_kvm_stats = false};
  volimem_set_config(&voli_config);

  volimem_start(&bench_config, virtual_main);

  return EXIT_SUCCESS;
}
