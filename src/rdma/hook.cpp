#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#include <volimem/mapper.h>
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

void handle_fault(void *fault_addr, regstate_t *regstate) {
  if (!g_swapper) {
    PANIC("Swapper not setup");
  }

  g_swapper->handle_fault(fault_addr, regstate);
}

static void virtual_main(void *any) {
  struct libc_start_params *params = (struct libc_start_params *)any;

  DEBUG("--- Inside VM ---");
  DEBUG("Running on the vCPU apic %lu", local_vcpu->lapic_id);
  DEBUG("Root page table is at %p", (void *)mapper_t::get_root());

  auto seg = new segment_t(HEAP_SIZE, HEAP_START);
  mapper_t::assign_handler(seg, handle_fault);
  INFO("Fault handling segment registered: [0x%lx, 0x%lx)", HEAP_START,
       (uptr)HEAP_START + HEAP_SIZE);

  // g_swapper->start_background_rebalancing();

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

  /* The cache where RDMA operations source and sink */
  cache_area =
      rdma_buffer_alloc(pd, PAGE_SIZE, CACHE_SIZE,
                        static_cast<ibv_access_flags>(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));
  if (!cache_area) {
    PANIC("Failed to allocate and register the cache");
  }

  auto rdma_storage = std::make_unique<RDMAStorage>(
      client_qp, io_completion_channel, cache_area, swap_area_metadata);
  auto swapper = std::make_unique<Swapper>(std::move(rdma_storage));
  g_swapper = swapper.get();

  static struct libc_start_params param;
  param.__libc_start_main =
      (__libc_start_main_t)dlsym(RTLD_NEXT, "__libc_start_main");
  real_main = main;
  param.main = call_real_main;
  param.argc = argc;
  param.argv = argv;
  param.init = init;
  param.fini = fini;
  param.rtld_fini = rtld_fini;
  param.stack_end = stack_end;

  constexpr s_volimem_config_t voli_config = {
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
      .print_kvm_stats = false};
  volimem_set_config(&voli_config);
  INFO("PID %d: Swapper started", getpid());

  // Uncomment to be able to use GDB and avoid "program exited during startup"
  // raise(SIGTRAP);
  ret = volimem_start(&param, virtual_main);

  UNREACHABLE("PID %d: volimem_start shouldn't return (ret = %d)", getpid(),
              ret);

  return ret;
}
