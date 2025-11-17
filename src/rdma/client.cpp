#include <benchmark/benchmark.h>
#include <cstdio>
#include <volimem/mapper.h>
#include <volimem/vcpu.h>
#include <volimem/volimem.h>

#include "benchmark/bump_map.h"
// #include "benchmark/linked_bump_map.h"
#include "common_client.h"
#include "storage/rdma_storage.h"

void usage() {
  printf("Usage:\n");
  printf("client [-a <server_addr>] [-p <server_port>]\n");
  printf("(default port: %d)\n", DEFAULT_RDMA_PORT);
  exit(1);
}

void handle_fault(void *fault_addr, regstate_t *regstate) {
  ENSURE(g_swapper != nullptr, "Swapper not setup");

  g_swapper->handle_fault(fault_addr, regstate);
}

void virtual_main(void *any) {
  s_benchmark_config_t *bench_config = (s_benchmark_config_t *)any;

  DEBUG("--- Inside VM ---");
  DEBUG("Running on the vCPU apic %lu", local_vcpu->lapic_id);
  DEBUG("Root page table is at %p", (void *)mapper_t::get_root());

  auto seg = new segment_t(HEAP_SIZE, HEAP_START);
  mapper_t::assign_handler(seg, handle_fault);
  INFO("Fault handling segment registered: [0x%lx, 0x%lx)", HEAP_START,
       (uptr)HEAP_START + HEAP_SIZE);

  // g_swapper->start_background_rebalancing();

  BumpMapDataLayer data_layer;
  run_benchmark(bench_config, &data_layer);

  g_swapper->print_stats();

  DEBUG("--- Exiting VM ---");
}

int main(int argc, char **argv) {
  if (argc < 6) {
    printf("Usage: %s <NUM_THREADS> <LOAD_NUM_KEYS> <NUM_OPS> <DISTRIBUTION> "
           "<WORKLOAD>\n",
           argv[0]);
    exit(1);
  }
  const unsigned int num_threads = (unsigned int)atoi(argv[1]);
  const unsigned int load_num_keys = (unsigned int)atoi(argv[2]);
  const unsigned int num_ops = (unsigned int)atoi(argv[3]);
  const enum distribution distribution =
      strcmp(argv[4], "uniform") == 0 ? UNIFORM : ZIPFIAN;
  const uint8_t workload = (uint8_t)atoi(argv[5]);

  s_benchmark_config_t bench_config{.num_threads = num_threads,
                                    .load_num_keys = load_num_keys,
                                    .num_ops = num_ops,
                                    .distribution = distribution,
                                    .workload = workload,
                                    .output_file = "./multidata/outputfile",
                                    .data_dir = "./multidata",
                                    .tsc = 2095008,
                                    .metric = METRIC::THROUGHPUT,
                                    .hook = NULL,
                                    .args = NULL};

  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof(server_sockaddr));
  server_sockaddr.sin_family = AF_INET;

  int ret;
  ret = inet_pton(AF_INET, DEFAULT_SERVER_ADDR, &server_sockaddr.sin_addr);
  if (ret <= 0) {
    if (ret == 0)
      ERROR("Invalid address string: %s", DEFAULT_SERVER_ADDR);
    else
      perror("inet_pton");
    return -1;
  }

  int option;
  while ((option = getopt(argc, argv, "a:p:")) != -1) {
    switch (option) {
    case 'a':
      ret = get_addr(optarg, (struct sockaddr *)&server_sockaddr);
      if (ret) {
        ERROR("Invalid address provided");
        return ret;
      }
      break;
    case 'p':
      server_sockaddr.sin_port = htons((u16)strtol(optarg, NULL, 0));
      break;
    default:
      usage();
      break;
    }
  }

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

  /* The cache where RDMA operations source and sink */
  cache_area =
      rdma_buffer_alloc(pd, PAGE_SIZE, CACHE_SIZE,
                        static_cast<ibv_access_flags>(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));
  if (!cache_area) {
    PANIC("Failed to allocate and register the cache");
  }

  {
    auto rdma_storage = std::make_unique<RDMAStorage>(
        client_qp, io_completion_channel, cache_area, swap_area_metadata);

    auto swapper = std::make_unique<Swapper>(std::move(rdma_storage));
    g_swapper = swapper.get();

    constexpr s_volimem_config_t voli_config{
        .log_level = INFO,
        .host_page_type = VOLIMEM_NORMAL_PAGES,
        .guest_page_type = VOLIMEM_NORMAL_PAGES,
        .print_kvm_stats = false};
    volimem_set_config(&voli_config);
    volimem_start(&bench_config, virtual_main);
  }

  ret = disconnect_and_cleanup();
  if (ret) {
    ERROR("Failed to cleanly disconnect and clean up resources ");
  }

  return ret;
}
