#include <volimem/mapper.h>
#include <volimem/vcpu.h>
#include <volimem/volimem.h>

#include "common.h"
#include "swapper.h"
#include "utils.h"

/* RDMA connection related resources */
static struct rdma_event_channel *cm_event_channel = NULL;
static struct rdma_cm_id *cm_client_id = NULL;
static struct ibv_pd *pd = NULL; // protection domain
static struct ibv_comp_channel *io_completion_channel = NULL;
static struct ibv_cq *client_cq = NULL; // completion queue
static struct ibv_qp_init_attr qp_init_attr;
static struct ibv_qp *client_qp; // queue pair

// Metadata Exchange: before one side can RDMA READ or WRITE to the other
// side's memory, it needs to know:
//  - The virtual address of the remote buffer.
//  - The length of the remote buffer.
//  - The remote memory key (rkey) authorizing access to that buffer.
// This information (often called "buffer attributes" or "metadata") is
// typically exchanged using SEND/RECV operations after the connection is
// established.
static struct ibv_mr *swap_area = NULL;
static struct rdma_buffer_attr swap_area_metadata;
/* the cache where RDMA operations source and sink */
static struct ibv_mr *cache_area = NULL;

// Pages metadata
Page *pages;

/* This function prepares client side connection resources for an RDMA
 * connection */
static int prepare_connection(struct sockaddr_in *s_addr) {
  struct rdma_cm_event *cm_event = NULL;
  int ret = -1;

  /*  Open a channel used to report asynchronous communication event */
  cm_event_channel = rdma_create_event_channel();

  if (!cm_event_channel) {
    ERROR("Creating cm event channel failed, errno: %d", -errno);
    return -errno;
  }

  /* rdma_cm_id is the connection identifier (like socket) which is used
   * to define an RDMA connection.
   */
  ret = rdma_create_id(cm_event_channel, &cm_client_id, NULL, RDMA_PS_TCP);
  if (ret) {
    ERROR("Creating cm id failed with errno: %d ", -errno);
    return -errno;
  }
  /* Resolve destination and optional source addresses from IP addresses  to
   * an RDMA address.  If successful, the specified rdma_cm_id will be bound
   * to a local device. */
  ret = rdma_resolve_addr(cm_client_id, NULL, (struct sockaddr *)s_addr, 2000);
  if (ret) {
    ERROR("Failed to resolve address, errno: %d", -errno);
    return -errno;
  }
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ADDR_RESOLVED,
                              &cm_event);
  if (ret) {
    ERROR("Failed to receive a valid event, ret = %d ", ret);
    return ret;
  }
  /* we ack the event */
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge the CM event, errno: %d", -errno);
    return -errno;
  }

  /* Resolves an RDMA route to the destination address in order to
   * establish a connection */
  ret = rdma_resolve_route(cm_client_id, 2000);
  if (ret) {
    ERROR("Failed to resolve route, erno: %d ", -errno);
    return -errno;
  }
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ROUTE_RESOLVED,
                              &cm_event);
  if (ret) {
    ERROR("Failed to receive a valid event, ret = %d ", ret);
    return ret;
  }
  /* we ack the event */
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge the CM event, errno: %d ", -errno);
    return -errno;
  }
  INFO("Trying to connect to server at: %s port: %d",
       inet_ntoa(s_addr->sin_addr), ntohs(s_addr->sin_port));
  /* Protection Domain (PD) is similar to a "process abstraction"
   * in the operating system. All resources are tied to a particular PD.
   * And accessing recourses across PD will result in a protection fault.
   */
  pd = ibv_alloc_pd(cm_client_id->verbs);
  if (!pd) {
    ERROR("Failed to alloc pd, errno: %d ", -errno);
    return -errno;
  }
  /* Now we need a completion channel, were the I/O completion
   * notifications are sent. Remember, this is different from connection
   * management (CM) event notifications.
   * A completion channel is also tied to an RDMA device, hence we will
   * use cm_client_id->verbs.
   */
  io_completion_channel = ibv_create_comp_channel(cm_client_id->verbs);
  if (!io_completion_channel) {
    ERROR("Failed to create IO completion event channel, errno: %d", -errno);
    return -errno;
  }
  /* Now we create a completion queue (CQ) where actual I/O
   * completion metadata is placed. The metadata is packed into a structure
   * called struct ibv_wc (wc = work completion). ibv_wc has detailed
   * information about the work completion. An I/O request in RDMA world
   * is called "work" ;)
   */
  client_cq = ibv_create_cq(
      cm_client_id->verbs /* which device*/, CQ_CAPACITY /* maximum capacity*/,
      NULL /* user context, not used here */,
      io_completion_channel /* which IO completion channel */,
      0 /* signaling vector, not used here*/);
  if (!client_cq) {
    ERROR("Failed to create CQ, errno: %d ", -errno);
    return -errno;
  }
  ret = ibv_req_notify_cq(client_cq, 0);
  if (ret) {
    ERROR("Failed to request notifications, errno: %d", -errno);
    return -errno;
  }
  /* Now the last step, set up the queue pair (send, recv) queues and their
   * capacity. The capacity here is define statically but this can be probed
   * from the device. We just use a small number as defined in rdma_common.h */
  memset(&qp_init_attr, 0, sizeof(qp_init_attr));
  qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
  qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
  qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
  qp_init_attr.cap.max_send_wr = MAX_WR;   /* Maximum send posting capacity */
  qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
  /* We use same completion queue, but one can use different queues */
  qp_init_attr.recv_cq =
      client_cq; /* Where should I notify for receive completion operations */
  qp_init_attr.send_cq =
      client_cq; /* Where should I notify for send completion operations */
  /*Lets create a QP */
  ret = rdma_create_qp(cm_client_id /* which connection id */,
                       pd /* which protection domain*/,
                       &qp_init_attr /* Initial attributes */);
  if (ret) {
    ERROR("Failed to create QP, errno: %d ", -errno);
    return -errno;
  }
  client_qp = cm_client_id->qp;
  return 0;
}

/* Pre-posts a receive buffer before calling rdma_connect() */
static int pre_post_recv_buffer() {
  int ret = -1;
  swap_area =
      rdma_buffer_register(pd, &swap_area_metadata, sizeof(swap_area_metadata),
                           (IBV_ACCESS_LOCAL_WRITE));
  if (!swap_area) {
    ERROR("Failed to setup the server metadata mr, -ENOMEM");
    return -ENOMEM;
  }
  struct ibv_sge server_recv_sge;
  server_recv_sge.addr = (u64)swap_area->addr;
  server_recv_sge.length = (u32)swap_area->length;
  server_recv_sge.lkey = (u32)swap_area->lkey;
  /* now we link it to the request */
  struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr = NULL;
  memset(&server_recv_wr, 0, sizeof(server_recv_wr));
  server_recv_wr.sg_list = &server_recv_sge;
  server_recv_wr.num_sge = 1;
  ret = ibv_post_recv(client_qp /* which QP */,
                      &server_recv_wr /* receive work request*/,
                      &bad_server_recv_wr /* error WRs */);
  if (ret) {
    ERROR("Failed to pre-post the receive buffer, errno: %d", ret);
    return ret;
  }

  DEBUG("Allocated buffer to receive server metadata");

  return 0;
}

/* Connects to the RDMA server */
static int connect_to_server() {
  struct rdma_conn_param conn_param;
  struct rdma_cm_event *cm_event = NULL;
  int ret = -1;
  memset(&conn_param, 0, sizeof(conn_param));
  conn_param.initiator_depth = 3;
  conn_param.responder_resources = 3;
  conn_param.retry_count = 3; // if fail, then how many times to retry
  ret = rdma_connect(cm_client_id, &conn_param);
  if (ret) {
    ERROR("Failed to connect to remote host , errno: %d", -errno);
    return -errno;
  }
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ESTABLISHED,
                              &cm_event);
  if (ret) {
    ERROR("Failed to get cm event, ret = %d ", ret);
    return ret;
  }
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge cm event, errno: %d", -errno);
    return -errno;
  }
  INFO("The client is connected successfully");
  return 0;
}

/**
 * @brief Allocates the client-side resources required for the swapper.
 *
 * This includes the cache and the page metadata array.
 * This should be called after the Protection Domain (pd) is available.
 * @return 0 on success, -1 on failure.
 */
static int cache_alloc() {
  cache_area =
      rdma_buffer_alloc(pd, PAGE_SIZE, PAGE_SIZE,
                        static_cast<ibv_access_flags>(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));

  if (!cache_area) {
    ERROR("Failed to allocate and register the cache_area");
    return -1;
  }

  pages = new Page[1];

  INFO("Cache allocated at %p and registered.", cache_area->addr);
  return 0;
}

/**
 * @brief Waits to receive the server's swap area metadata.
 *
 * This function assumes that a receive buffer has already been pre-posted.
 * It blocks until the server's metadata arrives.
 * @return 0 on success, error code on failure.
 */
static int receive_server_metadata() {
  struct ibv_wc wc;
  int ret = -1;

  // expect one completion: the reception of the server's metadata.
  ret = await_work_completion_events(io_completion_channel, &wc, 1);
  if (ret != 1) {
    ERROR("Failed to receive server metadata, ret = %d", ret);
    return ret;
  }

  DEBUG("Server's swap area metadata received. addr: %p, len: %u, stag: 0x%x",
        (void *)swap_area_metadata.address,
        (unsigned int)swap_area_metadata.length,
        swap_area_metadata.stag.local_stag);

  return 0;
}

/* This function disconnects the RDMA connection from the server and cleans up
 * all the resources.
 */
static int disconnect_and_cleanup() {
  struct rdma_cm_event *cm_event = NULL;
  int ret = -1;
  /* active disconnect from the client side */
  ret = rdma_disconnect(cm_client_id);
  if (ret) {
    ERROR("Failed to disconnect, errno: %d ", -errno);
    // continuing anyways
  }
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_DISCONNECTED,
                              &cm_event);
  if (ret) {
    ERROR("Failed to get RDMA_CM_EVENT_DISCONNECTED event, ret = %d", ret);
    // continuing anyways
  }
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge cm event, errno: %d", -errno);
    // continuing anyways
  }
  /* Destroy QP */
  rdma_destroy_qp(cm_client_id);
  /* Destroy client cm id */
  ret = rdma_destroy_id(cm_client_id);
  if (ret) {
    ERROR("Failed to destroy client id cleanly, %d ", -errno);
    // we continue anyways;
  }
  /* Destroy CQ */
  ret = ibv_destroy_cq(client_cq);
  if (ret) {
    ERROR("Failed to destroy completion queue cleanly, %d ", -errno);
    // we continue anyways;
  }
  /* Destroy completion channel */
  ret = ibv_destroy_comp_channel(io_completion_channel);
  if (ret) {
    ERROR("Failed to destroy completion channel cleanly, %d ", -errno);
    // we continue anyways;
  }

  // Free the cache and its RDMA registered area
  if (cache_area) {
    rdma_buffer_free(cache_area);
    cache_area = NULL;
  }

  // Deregister the memory region we used to receive the server's metadata
  if (swap_area) {
    rdma_buffer_deregister(swap_area);
    swap_area = NULL;
  }

  // Free the page metadata array
  if (pages) {
    delete[] pages;
    pages = NULL;
  }

  /* Destroy protection domain */
  ret = ibv_dealloc_pd(pd);
  if (ret) {
    ERROR("Failed to destroy client protection domain cleanly, %d ", -errno);
    // we continue anyways;
  }
  rdma_destroy_event_channel(cm_event_channel);

  INFO("Client shut-down is complete.");
  return 0;
}

void usage() {
  printf("Usage:\n");
  printf("client [-a <server_addr>] [-p <server_port>]\n");
  printf("(default port: %d)\n", DEFAULT_RDMA_PORT);
  exit(1);
}

/**
 * @brief Performs an RDMA READ to fetch a page from the remote swap area.
 *
 * @param local_addr Local buffer where data should be placed (i.e., in cache)
 * @param remote_offset Offset in the remote swap area
 * @return 0 on success, error code on failure
 */
static int rdma_read_page(void *local_addr, u64 remote_offset) {
  struct ibv_sge sge;
  sge.addr = (u64)local_addr;
  sge.length = PAGE_SIZE;
  sge.lkey = cache_area->lkey;

  // Prepare the READ work request
  struct ibv_send_wr wr, *bad_wr = NULL;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.send_flags = IBV_SEND_SIGNALED;

  // Set the remote address and key
  wr.wr.rdma.remote_addr = swap_area_metadata.address + remote_offset;
  wr.wr.rdma.rkey = swap_area_metadata.stag.remote_stag;

  DEBUG("Posting RDMA READ: local=%p, remote=0x%lx, rkey=0x%x", local_addr,
        wr.wr.rdma.remote_addr, wr.wr.rdma.rkey);

  int ret;
  // Post the READ operation
  ret = ibv_post_send(client_qp, &wr, &bad_wr);
  if (ret) {
    ERROR("Failed to post RDMA READ, errno: %d", -errno);
    return -errno;
  }

  struct ibv_wc wc;
  // Wait for completion
  ret = await_work_completion_events(io_completion_channel, &wc, 1);
  if (ret != 1) {
    ERROR("RDMA READ failed, ret = %d", ret);
    return ret;
  }

  DEBUG("RDMA READ completed successfully");
  return 0;
}

/**
 * @brief Performs an RDMA WRITE to evict a page to the remote swap area.
 *
 * @param local_addr Local buffer to write from (i.e., in cache)
 * @param remote_offset Offset in the remote swap area
 * @return 0 on success, error code on failure
 */
static int rdma_write_page(void *local_addr, u64 remote_offset) {
  struct ibv_sge sge;
  sge.addr = (u64)local_addr;
  sge.length = PAGE_SIZE;
  sge.lkey = cache_area->lkey;

  // Prepare the WRITE work request
  struct ibv_send_wr wr, *bad_wr = NULL;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = IBV_SEND_SIGNALED;

  // Set the remote address and key
  wr.wr.rdma.remote_addr = swap_area_metadata.address + remote_offset;
  wr.wr.rdma.rkey = swap_area_metadata.stag.remote_stag;

  DEBUG("Posting RDMA WRITE: local=%p, remote=0x%lx, rkey=0x%x", local_addr,
        wr.wr.rdma.remote_addr, wr.wr.rdma.rkey);

  int ret;
  // Post the WRITE operation
  ret = ibv_post_send(client_qp, &wr, &bad_wr);
  if (ret) {
    ERROR("Failed to post RDMA WRITE, errno: %d", -errno);
    return -errno;
  }

  struct ibv_wc wc;
  // Wait for completion
  ret = await_work_completion_events(io_completion_channel, &wc, 1);
  if (ret != 1) {
    ERROR("RDMA WRITE failed, ret = %d", ret);
    return ret;
  }

  DEBUG("RDMA WRITE completed successfully");
  return 0;
}

void swap_in(uptr target_vaddr, uptr remote_swap_offset, uptr cache_gva,
             uptr cache_gpa) {
  int ret = rdma_read_page((void *)cache_gva, remote_swap_offset);
  if (ret != 0) {
    ERROR("RDMA READ failed during swap-in");
    return;
  }

  // map the PF in the guest page table (so the GVA -> GPA mapping exists)
  mapper_t::map_gpt(target_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
}

void swap_out(uptr victim_vaddr, uptr remote_swap_offset, uptr cache_gva) {
  int ret = rdma_write_page((void *)cache_gva, remote_swap_offset);
  if (ret != 0) {
    ERROR("RDMA WRITE failed during swap-out");
    return;
  }

  // unmap the page from the guest page table
  mapper_t::unmap(victim_vaddr, PAGE_SIZE);
  // ensure that unmap actually invalidates the TLB entry
  mapper_t::flush(victim_vaddr, PAGE_SIZE);
}

void swap_in_page(usize target_idx, uptr aligned_fault_vaddr) {
  auto cache_offset = target_idx * PAGE_SIZE;
  auto cache_gva = (uptr)cache_area->addr + cache_offset;
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);
  INFO("Swapping IN: 0x%lx. Cache (GVA->GPA): 0x%lx->0x%lx",
       aligned_fault_vaddr, cache_gva, cache_gpa);

  auto target_offset = aligned_fault_vaddr - HEAP_START;
  // auto swap_src = (uptr)swap_area->addr + target_offset;
  swap_in(aligned_fault_vaddr, target_offset, cache_gva, cache_gpa);

  auto &target_page = pages[target_idx];
  target_page.vaddr = aligned_fault_vaddr;
  target_page.state = PageState::Mapped;
}

void swap_out_page(usize victim_idx) {
  // victim page is the i-th page within the heap
  auto &victim_page = pages[victim_idx];

  if (victim_page.state != PageState::Mapped) {
    ERROR("Cannot swap out non-mapped page %zu", victim_idx);
    return;
  }

  auto victim_vaddr = victim_page.vaddr;
  INFO("Swapping OUT: 0x%lx. GPA = 0x%lx. State = %s", victim_vaddr,
       mapper_t::gva_to_gpa((void *)victim_vaddr),
       page_state_to_str(victim_page.state));

  // Calculate the offset in the remote swap area
  auto victim_offset = victim_vaddr - HEAP_START;

  // Calculate the cache slot GVA
  auto cache_offset = victim_idx * PAGE_SIZE;
  auto cache_gva = (uptr)cache_area->addr + cache_offset;

  swap_out(victim_vaddr, victim_offset, cache_gva);

  victim_page.reset();

  INFO("Page %zu swapped out successfully", victim_idx);
}

void handle_fault(void *fault_addr, regstate_t *regstate) {
  DEBUG("Handling fault at %p. Error code: %lu", fault_addr,
        regstate->error_code);

  // align to page boundary
  uptr aligned_fault_vaddr = (uptr)fault_addr & ~(PAGE_SIZE - 1);

  // sanity check: should be within our heap
  if (aligned_fault_vaddr < HEAP_START ||
      aligned_fault_vaddr >= HEAP_START + HEAP_SIZE) {
    ERROR("Fault address 0x%lx outside of heap range [0x%lx, 0x%lx)",
          aligned_fault_vaddr, HEAP_START, HEAP_START + HEAP_SIZE);
    return;
  }

  swap_in_page(0, aligned_fault_vaddr);

  INFO("Page fault handled successfully");
}

void virtual_main(void *any) {
  UNUSED(any);

  DEBUG("--- Inside VM ---");
  DEBUG("Running on the vCPU apic %lu", local_vcpu->lapic_id);
  DEBUG("Root page table is at %p", (void *)mapper_t::get_root());

  auto seg = new segment_t(HEAP_SIZE, HEAP_START);
  mapper_t::assign_handler(seg, handle_fault);
  INFO("Fault handling segment registered: [0x%lx, 0x%lx)", HEAP_START,
       (uptr)HEAP_START + HEAP_SIZE);

  // Trigger the first swap by reading from the start of the heap
  /*
  volatile u32 *heap_ptr = (volatile u32 *)(HEAP_START + PAGE_SIZE);
  DEBUG("About to trigger first page fault by reading from 0x%lx", HEAP_START);
  u32 value = *heap_ptr; // This will cause a fault
  DEBUG("Read value: 0x%x", value);
  ENSURE(value == 0xDEADBEEF, "Read value must be 0xDEADBEEF");
  */

  // ===== TEST 1: SWAP IN (RDMA READ) =====
  DEBUG("\n=== TEST 1: SWAP IN (RDMA READ) ===");
  volatile u32 *heap_ptr = (volatile u32 *)HEAP_START;
  DEBUG("Triggering page fault by reading from 0x%lx", HEAP_START);

  u32 value = *heap_ptr; // Fault -> swap_in_page(0, HEAP_START)
  DEBUG("Read value: 0x%x", value);
  ENSURE(value == 0xDEADBEEF, "Read value must be 0xDEADBEEF");
  INFO("✓ SWAP IN test passed!");

  *heap_ptr = 0xCAFEBABE;
  ENSURE(*heap_ptr == 0xCAFEBABE, "Modified value must be 0xCAFEBABE");
  INFO("✓ Page modification successful!");

  // ===== TEST 2: SWAP OUT (RDMA WRITE) =====
  DEBUG("\n=== TEST 3: SWAP OUT (RDMA WRITE) ===");
  swap_out_page(0); // Evict the page we just modified
  INFO("✓ SWAP OUT test passed!");

  // ===== TEST 3: VERIFY SWAP OUT =====
  DEBUG("\n=== TEST 4: VERIFY SWAP OUT ===");
  // Try to read again - should fault and swap back in
  DEBUG("Triggering page fault again by reading from 0x%lx", HEAP_START);
  value = *heap_ptr; // Fault -> swap_in_page(0, HEAP_START)
  DEBUG("Read value after swap-in: 0x%x", value);
  ENSURE(value == 0xCAFEBABE, "Value must be 0xCAFEBABE (our modified value)");
  INFO("✓ SWAP OUT persistence verified!");

  DEBUG("--- Exiting VM ---");
}

int main(int argc, char **argv) {
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

  ret = cache_alloc();
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
      .log_level = DEBUG,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
      .print_kvm_stats = false};

  volimem_set_config(&voli_config);
  volimem_start(nullptr, virtual_main);

  ret = disconnect_and_cleanup();
  if (ret) {
    ERROR("Failed to cleanly disconnect and clean up resources ");
  }

  return ret;
}
