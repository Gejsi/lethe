#include <benchmark/benchmark.h>
#include <cstdio>
#include <cstdlib>
#include <volimem/mapper.h>
#include <volimem/vcpu.h>
#include <volimem/volimem.h>

#include "benchmark/bump_map.h"
#include "benchmark/std_map.h"
#include "common_client.h"
#include "logger.h"
#include "storage/rdma_storage.h"
#include "swapper.h"

struct VirtualMainArgs {
  s_benchmark_config_t *bench_config;
  SwapperConfig swapper_config;
};

void handle_fault(void *fault_addr, regstate_t *regstate) {
  if (!g_swapper) {
    PANIC("Swapper not setup");
  }

  g_swapper->handle_fault(fault_addr, regstate);
}

void virtual_main(void *any) {
  DEBUG("--- Inside VM ---");

  VirtualMainArgs *args = (VirtualMainArgs *)any;
  s_benchmark_config_t *bench_config = args->bench_config;

  // The cache where RDMA operations source and sink
  cache_area =
      rdma_buffer_alloc(pd, PAGE_SIZE, args->swapper_config.cache_size,
                        static_cast<ibv_access_flags>(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));
  if (!cache_area) {
    PANIC("Failed to allocate and register the cache inside VM");
  }
  INFO("Cache area allocated inside VM at %p", (void *)cache_area->addr);

  auto rdma_storage = std::make_unique<RDMAStorage>(
      client_qp, io_completion_channel, cache_area, swap_area_metadata);

  auto swapper = std::make_unique<Swapper>(std::move(args->swapper_config),
                                           std::move(rdma_storage));
  // global swapper set for the fault handler
  g_swapper = swapper.get();

  segment_t seg{g_swapper->config.heap_size, HEAP_START};
  mapper_t::assign_handler(&seg, handle_fault);
  INFO("Fault handling segment registered: [0x%lx, 0x%lx)", HEAP_START,
       (uptr)HEAP_START + g_swapper->config.heap_size);

  g_swapper->start_background_rebalancing();

  BumpMapDataLayer data_layer(HEAP_START, g_swapper->config.heap_size);
  run_benchmark(bench_config, &data_layer);

  g_swapper->print_stats();
  g_swapper->stop_background_rebalancing();

  DEBUG("--- Exiting VM ---");
}

static void print_usage(const char *prog, const SwapperConfig &config) {
  printf("Usage: %s [options]\n\n", prog);
  printf("Benchmark options:\n");
  printf("  -t, --threads <n>      Number of threads (default: 1)\n");
  printf(
      "  -k, --keys <n>         Number of keys to load (default: 3000000)\n");
  printf("  -o, --ops <n>          Number of operations to run (default: "
         "1000000)\n");
  printf("  -d, --dist <type>      Distribution: uniform|zipfian (default: "
         "zipfian)\n");
  printf("  -w, --workload <A-D>   YCSB workload: A|B|C|D (default: A)\n");
  printf("\nSwapper options:\n");
  printf("  -m, --cache-mb <n>     Cache size in MB (default: %zu)\n",
         config.cache_size / MB);
  printf("  -c, --cache-gb <n>     Cache size in GB\n");
  printf("  -r, --rebalance <0|1>  Toggle rebalancer (default: %d)\n",
         config.rebalancer_enabled);
  printf("  -S, --num-shards <n>   Number of LRU shards (default: %zu)\n",
         config.num_shards);
  printf("\nRDMA options:\n");
  printf("  -a, --addr <ip>        Server address (default: %s)\n",
         DEFAULT_SERVER_ADDR);
  printf("  -p, --port <n>         Server port (default: %d)\n",
         DEFAULT_RDMA_PORT);
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
  PANIC("Invalid workload '%s'. Choose A, B, C, or D", arg);
}

int main(int argc, char **argv) {
  // AsyncLogger::instance().init("swapper.log");
  SwapperConfig swapper_config;

  // Benchmark defaults
  u32 num_threads = 1;
  u32 load_num_keys = 3000000;
  u32 num_ops = 1000000;
  enum distribution dist = ZIPFIAN;
  u8 workload = 0; // workload A

  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof(server_sockaddr));
  server_sockaddr.sin_family = AF_INET;

  int ret;
  ret = inet_pton(AF_INET, DEFAULT_SERVER_ADDR, &server_sockaddr.sin_addr);
  if (ret <= 0) {
    if (ret == 0) {
      ERROR("Invalid address string: %s", DEFAULT_SERVER_ADDR);
    } else {
      perror("inet_pton");
    }
    return EXIT_FAILURE;
  }

  int option_index = 0;
  struct option long_options[] = {{"threads", required_argument, 0, 't'},
                                  {"keys", required_argument, 0, 'k'},
                                  {"ops", required_argument, 0, 'o'},
                                  {"dist", required_argument, 0, 'd'},
                                  {"workload", required_argument, 0, 'w'},
                                  {"cache-mb", required_argument, 0, 'm'},
                                  {"cache-gb", required_argument, 0, 'c'},
                                  {"rebalance", required_argument, 0, 'r'},
                                  {"num-shards", required_argument, 0, 'S'},
                                  {"addr", required_argument, 0, 'a'},
                                  {"port", required_argument, 0, 'p'},
                                  {"help", no_argument, 0, 'h'},
                                  {0, 0, 0, 0}};

  int option;
  while ((option = getopt_long(argc, argv, "t:k:o:d:w:m:c:r:S:a:p:h",
                               long_options, &option_index)) != -1) {
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
    case 'm':
      swapper_config.cache_size = (usize)atol(optarg) * MB;
      break;
    case 'c':
      swapper_config.cache_size = (usize)atol(optarg) * GB;
      break;
    case 'r':
      swapper_config.rebalancer_enabled = (bool)atoi(optarg);
      break;
    case 'S':
      swapper_config.num_shards = (usize)atol(optarg);
      break;
    case 'a':
      ret = get_addr(optarg, (struct sockaddr *)&server_sockaddr);
      if (ret) {
        PANIC("Invalid address provided");
      }
      break;
    case 'p':
      server_sockaddr.sin_port = htons((u16)strtol(optarg, NULL, 0));
      break;
    case 'h':
      print_usage(argv[0], swapper_config);
      return EXIT_SUCCESS;
    default:
      print_usage(argv[0], swapper_config);
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
                                    .tsc = 2095008,
                                    .metric = METRIC::THROUGHPUT,
                                    .hook = NULL,
                                    .args = NULL};

  if (!server_sockaddr.sin_port) {
    server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);
  }

  ret = prepare_connection(&server_sockaddr);
  if (ret) {
    ERROR("Failed to setup client connection, ret = %d", ret);
    return ret;
  }
  ret = pre_post_recv_buffer();
  if (ret) {
    ERROR("Failed to setup client connection, ret = %d", ret);
    return ret;
  }

  ret = connect_to_server();
  if (ret) {
    ERROR("Failed to setup client connection, ret = %d", ret);
    return ret;
  }

  ret = receive_server_metadata();
  if (ret) {
    ERROR("Failed to setup client connection, ret = %d", ret);
    return ret;
  }

  constexpr s_volimem_config_t voli_config{
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
      .print_kvm_stats = false};
  volimem_set_config(&voli_config);

  VirtualMainArgs vm_args{.bench_config = &bench_config,
                          .swapper_config = std::move(swapper_config)};
  volimem_start(&vm_args, virtual_main);

  ret = disconnect_and_cleanup();
  if (ret) {
    ERROR("Failed to cleanly disconnect and clean up resources");
    return ret;
  }

  return EXIT_SUCCESS;
}
