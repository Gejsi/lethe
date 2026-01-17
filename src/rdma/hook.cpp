#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#include <volimem/mapper.h>
#include <volimem/syscall_table.h>
#include <volimem/vcpu.h>
#include <volimem/volimem.h>

#include "common_client.h"
#include "storage/rdma_storage.h"
#include "utils.h"

typedef int (*__libc_start_main_t)(int (*main)(int, char **, char **), int argc,
                                   char **argv,
                                   int (*init)(int, char **, char **),
                                   void (*fini)(), void (*rtld_fini)(),
                                   void *stack_end);

// Hold all the arguments needed to call the original
// __libc_start_main from inside the VM.
struct libc_start_params {
  __libc_start_main_t __libc_start_main;
  int (*main)(int, char **, char **);
  int argc;
  char **argv;
  int (*init)(int, char **, char **);
  void (*fini)();
  void (*rtld_fini)();
  void *stack_end;
};

static int (*real_main)(int, char **, char **);

static int call_real_main(int argc, char **argv, char **env) {
  int ret = real_main(argc, argv, env);

  atexit([]() {
    INFO("PID %d: Target application exited", getpid());
    g_swapper->print_stats();
    disconnect_and_cleanup();
  });

  return ret;
}

struct VirtualMainContext {
  SwapperConfig swapper_config;
  struct libc_start_params *libc_params;
};

void handle_fault(void *fault_addr, regstate_t *regstate) {
  if (!g_swapper) {
    PANIC("Swapper not setup");
  }

  static usize counter = 0;

  // printf("CALLING FAULT HANDLER at %p. Error code: %lu\n", fault_addr,
  //        regstate->error_code);

  g_swapper->handle_fault(fault_addr, regstate);

  // printf("Resolved %lu\n", counter++);
}

static void virtual_main(void *any) {
  DEBUG("--- Inside VM ---");

  auto *ctx = static_cast<VirtualMainContext *>(any);
  auto &swapper_config = ctx->swapper_config;
  libc_start_params *params = ctx->libc_params;

  // The cache where RDMA operations source and sink
  cache_area =
      rdma_buffer_alloc(pd, PAGE_SIZE, swapper_config.cache_size,
                        static_cast<ibv_access_flags>(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));
  if (!cache_area) {
    PANIC("Failed to allocate and register the cache");
  } else {
    INFO("Cache area allocated at %p", (void *)cache_area);
  }

  auto rdma_storage = std::make_unique<RDMAStorage>(
      client_qp, io_completion_channel, cache_area, swap_area_metadata);
  auto swapper = std::make_unique<Swapper>(std::move(swapper_config),
                                           std::move(rdma_storage));
  g_swapper = swapper.get();

  usize infinite_size = ALIGN_DOWN(UINT64_MAX);
  auto seg = new segment_t(infinite_size, 0);
  mapper_t::assign_handler(seg, handle_fault);
  INFO("Fault handling segment registered: [0x%lx, 0x%lx)", (uptr)seg->start,
       (uptr)seg->start + seg->size);

  g_alloc_hook = [](u64, u64, u64) {};
  g_dealloc_hook = [](u64, u64) {};

  // std::thread t1([]() { ERROR("Foo"); });
  // t1.join();

  params->__libc_start_main(params->main, params->argc, params->argv,
                            params->init, params->fini, params->rtld_fini,
                            params->stack_end);

  UNREACHABLE("--- Exiting VM ---");
}

// hook to hijack the real application main
extern "C" int __libc_start_main(int (*main)(int, char **, char **), int argc,
                                 char **argv,
                                 int (*init)(int, char **, char **),
                                 void (*fini)(), void (*rtld_fini)(),
                                 void *stack_end) {
  // TODO: make these values configurable
  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof(server_sockaddr));
  server_sockaddr.sin_family = AF_INET;
  inet_pton(AF_INET, DEFAULT_SERVER_ADDR, &server_sockaddr.sin_addr);
  server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);

  int ret;
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

  libc_start_params params;
  params.__libc_start_main =
      (__libc_start_main_t)dlsym(RTLD_NEXT, "__libc_start_main");
  real_main = main;
  params.main = call_real_main;
  params.argc = argc;
  params.argv = argv;
  params.init = init;
  params.fini = fini;
  params.rtld_fini = rtld_fini;
  params.stack_end = stack_end;

  constexpr s_volimem_config_t voli_config = {
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
      .print_kvm_stats = false};
  volimem_set_config(&voli_config);
  INFO("PID %d: Swapper started", getpid());

  SwapperConfig swapper_config;
  VirtualMainContext vm_ctx{
      .swapper_config = std::move(swapper_config),
      .libc_params = &params,
  };
  // Uncomment to be able to use GDB and avoid "program exited during startup"
  // raise(SIGTRAP);
  ret = volimem_start(&vm_ctx, virtual_main);

  UNREACHABLE("PID %d: volimem_start shouldn't return here (ret = %d)",
              getpid(), ret);

  return ret;
}
