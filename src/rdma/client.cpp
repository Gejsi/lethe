#include <cstdio>
#include <list>
#include <mutex>
#include <thread>
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
std::list<Page *> active_pages, inactive_pages; // mapped pages
std::list<Page *> free_pages;                   // track available cache slots
std::mutex pages_mutex;

std::unordered_map<uptr, PageState> state_map;

// NOTE: caller must hold the lists mutex
void print_lists(const char *caller) {
  printf("[%s] Free: %zu, Inactive: %zu, Active: %zu\n", caller,
         free_pages.size(), inactive_pages.size(), active_pages.size());

  printf("=== Free Pages ===\n");
  for (auto p : free_pages) {
    p->print();
  }

  printf("=== Inactive Pages ===\n");
  for (auto p : inactive_pages) {
    p->print();
  }

  printf("=== Active Pages ===\n");
  for (auto p : active_pages) {
    p->print();
  }

  printf("=== States ===\n");
  for (auto p : state_map) {
    printf("0x%lx -> %s\n", p.first, page_state_to_str(p.second));
  }
}

void print_perms(uptr vaddr) {
  auto perms = mapper_t::get_protect(vaddr);
  DEBUG("==============================================");
  DEBUG("Permissions for gva 0x%lx", vaddr);
  DEBUG("Raw PTE value: 0x%lx", perms);
  DEBUG("Present:  %s", bool_to_str(pte_is_present(perms)));
  DEBUG("Writable: %s", bool_to_str(pte_is_writable(perms)));
  DEBUG("Accessed: %s", bool_to_str(pte_is_accessed(perms)));
  DEBUG("Dirty:    %s", bool_to_str(pte_is_dirty(perms)));
  DEBUG("==============================================");
}

/**
 * @brief Retrieves a free page from the cache.
 *
 * @return A pointer to a free Page object, or nullptr if none are available.
 */
Page *get_free_page() {
  if (free_pages.empty()) {
    DEBUG("No free cache slots available");
    return nullptr;
  }

  Page *p = free_pages.front();
  free_pages.pop_front();
  DEBUG("Free cache slot selected: %zu", (usize)(p - pages));
  return p;
}

uptr get_cache_gva(Page *page) {
  usize page_idx = (usize)(page - pages);
  auto cache_offset = page_idx * PAGE_SIZE;
  return (uptr)cache_area->addr + cache_offset;
}

// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------
// NOTE: All the methods ABOVE are temporary and will either be removed
// or refactored into more appropriate abstractions.
// Most of the methods BELOW can stay in this client as-is.
// --------------------------------------------------------------------
// --------------------------------------------------------------------
// --------------------------------------------------------------------

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
      rdma_buffer_alloc(pd, PAGE_SIZE, CACHE_SIZE,
                        static_cast<ibv_access_flags>(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));

  if (!cache_area) {
    ERROR("Failed to allocate and register the cache_area");
    return -1;
  }

  pages = new Page[NUM_PAGES];
  for (usize i = 0; i < NUM_PAGES; i++) {
    free_pages.push_back(&pages[i]);
  }

  INFO("Cache of %zu pages allocated at %p and registered.", NUM_PAGES,
       cache_area->addr);

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
  return 0;
}

void swap_in(uptr vaddr, uptr remote_swap_offset, uptr cache_gva) {
  int ret = rdma_read_page((void *)cache_gva, remote_swap_offset);
  if (ret != 0) {
    ERROR("Failed to swap in: RDMA READ errored (%d)", ret);
    return;
  }

  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);
  map(vaddr, cache_gpa);
}

void swap_out(uptr vaddr, uptr remote_swap_offset, uptr cache_gva) {
  int ret = rdma_write_page((void *)cache_gva, remote_swap_offset);
  if (ret != 0) {
    ERROR("Failed to swap out: RDMA WRITE errored (%d)", ret);
    return;
  }

  unmap(vaddr);
}

void swap_in_page(Page *page, uptr aligned_fault_vaddr) {
  if (page == nullptr) {
    ERROR("Failed to swap in: null page");
    return;
  }

  auto target_offset = aligned_fault_vaddr - HEAP_START;
  auto cache_gva = get_cache_gva(page);

  INFO("Swapping IN: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx",
       aligned_fault_vaddr, cache_gva, mapper_t::gva_to_gpa((void *)cache_gva));

  swap_in(aligned_fault_vaddr, target_offset, cache_gva);

  page->vaddr = aligned_fault_vaddr;
}

void swap_out_page(Page *page) {
  if (page == nullptr) {
    ERROR("Failed to swap out: null page");
    return;
  }

  // victim page is the n-th page within the heap
  auto victim_vaddr = page->vaddr;
  // Calculate the offset in the remote swap area
  auto victim_offset = victim_vaddr - HEAP_START;
  auto cache_gva = get_cache_gva(page);

  INFO("Swapping OUT: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx", victim_vaddr,
       cache_gva, mapper_t::gva_to_gpa((void *)victim_vaddr));

  swap_out(victim_vaddr, victim_offset, cache_gva);
  // auto perms = mapper_t::get_protect(victim_vaddr);

  // if (pte_is_dirty(perms)) {
  //   INFO("Page %zu (GVA 0x%lx) is DIRTY. Writing back to server.",
  //        (usize)(page - pages), victim_vaddr);
  //   swap_out(victim_vaddr, victim_offset, cache_gva);
  // } else {
  //   INFO("Page %zu (GVA 0x%lx) is CLEAN. Skipping write-back.",
  //        (usize)(page - pages), victim_vaddr);
  //   unmap(victim_vaddr); // just unmap it without an RDMA WRITE
  // }

  page->reset();
}

void handle_fault(void *fault_addr, regstate_t *regstate) {
  DEBUG("Handling fault at %p. Error code: %lu", fault_addr,
        regstate->error_code);

  // align to page boundary
  uptr aligned_fault_vaddr = (uptr)fault_addr & ~(PAGE_SIZE - 1);

  // faulting address should be within the heap
  if (aligned_fault_vaddr < HEAP_START ||
      aligned_fault_vaddr >= HEAP_START + HEAP_SIZE) {
    ERROR("Fault address 0x%lx outside of heap range [0x%lx, 0x%lx)",
          aligned_fault_vaddr, HEAP_START, HEAP_START + HEAP_SIZE);
    return;
  }

  std::lock_guard<std::mutex> lock(pages_mutex);

  Page *page = nullptr;
  if (!free_pages.empty()) {
    page = free_pages.front();
    free_pages.pop_front();
  } else {
    // eviction is necessary
    Page *victim_page = nullptr;
    if (!inactive_pages.empty()) {
      victim_page = inactive_pages.back();
      inactive_pages.pop_back();
    } else if (!active_pages.empty()) {
      victim_page = active_pages.back();
      active_pages.pop_back();
    } else {
      UNREACHABLE("No pages available to evict from any list");
    }

    auto victim_vaddr = victim_page->vaddr;

    if (auto perms = mapper_t::get_protect(victim_vaddr); pte_is_dirty(perms)) {
      swap_out_page(victim_page);
      state_map[victim_vaddr] = PageState::RemotelyMapped;
    } else {
      INFO("Page %zu (GVA 0x%lx) is CLEAN. Skipping write-back.",
           (usize)(victim_page - pages), victim_vaddr);

      unmap(victim_vaddr);
      victim_page->reset();
      state_map[victim_vaddr] = PageState::Unmapped;
    }

    page = victim_page;
  }

  PageState page_state = state_map.contains(aligned_fault_vaddr)
                             ? state_map.at(aligned_fault_vaddr)
                             : PageState::Unmapped;

  if (page_state == PageState::Unmapped) {
    DEBUG("Assigning page for gva 0x%lx", aligned_fault_vaddr);
    // TODO: this logic is almost identical to the swap-in,
    // but without the RDMA read. Refactor this.
    auto cache_gva = get_cache_gva(page);
    memset((void *)cache_gva, 0, PAGE_SIZE);
    auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);
    map(aligned_fault_vaddr, cache_gpa);
    page->vaddr = aligned_fault_vaddr;
  } else if (page_state == PageState::RemotelyMapped) {
    swap_in_page(page, aligned_fault_vaddr);
  } else {
    UNREACHABLE("A %s page (0x%lx) shouldn't cause a fault.",
                page_state_to_str(page_state), aligned_fault_vaddr);
  }

  state_map[aligned_fault_vaddr] = PageState::Mapped;
  active_pages.push_front(page);
}

void rebalance_lists() {
  DEBUG("----------START REBALANCING----------");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    // Demotion: hot -> cold
    for (auto it = active_pages.begin(); it != active_pages.end();) {
      auto hot_page = *it;

      if (auto perms = mapper_t::get_protect(hot_page->vaddr);
          pte_is_accessed(perms)) {
        // page is still hot, clear the A-bit and keep it in the active list
        clear_permissions(hot_page->vaddr, PTE_A);
        ++it;
        DEBUG("Page %zu is still hot, giving another chance",
              (usize)(hot_page - pages));
      } else {
        // page has become cold, demote it to the inactive list
        inactive_pages.push_front(hot_page);
        it = active_pages.erase(it); // returns iterator to the next element
        DEBUG("Demote page %zu (gva 0x%lx) to inactive list",
              (usize)(hot_page - pages), hot_page->vaddr);
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    // Promotion: cold -> hot
    for (auto it = inactive_pages.begin(); it != inactive_pages.end();) {
      auto cold_page = *it;

      if (auto perms = mapper_t::get_protect(cold_page->vaddr);
          pte_is_accessed(perms)) {
        // page has become hot, promote to active
        active_pages.push_front(cold_page);
        it = inactive_pages.erase(it);
        clear_permissions(cold_page->vaddr, PTE_A);
        DEBUG("Promoted page %zu (gva 0x%lx) to active list",
              (usize)(cold_page - pages), cold_page->vaddr);
      } else {
        // page is still cold, keep it in the inactive list
        ++it;
        DEBUG("Page %zu is still cold", (usize)(cold_page - pages));
      }
    }
  }

  DEBUG("----------END REBALANCING----------");
}

void handle_rebalance() {
  while (true) {
    sleep_ms(200);
    rebalance_lists();
  }
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

  std::thread rebalance_thread(handle_rebalance);
  rebalance_thread.detach();

  // ============================================================
  // MANUAL TESTING
  // ============================================================

  volatile u32 *p[NUM_PAGES + 2];
  for (size_t i = 0; i < NUM_PAGES + 2; ++i) {
    p[i] = (volatile u32 *)(HEAP_START + i * PAGE_SIZE);
  }

  // ===== 1. Setup: Create one clean (P0) and one dirty (P1) page =====
  INFO("\n[1] Faulting in P0 (clean) and P1 (dirty)...");
  *p[0];
  *p[1] = 0xCAFEBABE;
  INFO("✓ P0 and P1 are on the active list.");

  // ===== 2. Setup: Fill the rest of the cache =====
  for (size_t i = 2; i < NUM_PAGES; ++i) {
    *p[i];
  }
  INFO("✓ Cache is now full.");
  print_lists("After cache fill");

  // ===== 3. Test: Wait for ALL pages to be demoted =====
  INFO("\n[2] Waiting 1s for all pages to become inactive...");
  sleep_ms(1000); // Long sleep to ensure all pages are demoted.
  print_lists("After all pages inactive");
  // EXPECT: All pages are on the inactive list. The oldest should be P0, then
  // P1.

  // ===== 4. Test: Evict the clean page =====
  INFO("\n[3] Faulting on a new page to evict the COLDEST page...");
  *p[NUM_PAGES]; // This should evict the page at the back of the inactive list.
  INFO("✓ First eviction complete. Check log for victim.");
  print_lists("After first eviction");

  // ===== 5. Test: Evict the dirty page =====
  INFO("\n[4] Faulting on another new page to evict the NEXT COLDEST page...");
  *p[NUM_PAGES + 1];
  INFO("✓ Second eviction complete. Check log for victim.");
  print_lists("After second eviction");

  // ===== 6. Verification =====
  INFO("\n[5] Verifying data...");
  u32 p0_val = *p[0]; // Should be demand-zeroed -> 0
  u32 p1_val = *p[1]; // Should be swapped in -> 0xCAFEBABE
  INFO("Value of P0: 0x%x. Value of P1: 0x%x", p0_val, p1_val);
  ENSURE(p0_val == 0, "Clean-evicted page should be zero.");
  ENSURE(p1_val == 0xCAFEBABE, "Dirty-evicted page should have its data.");
  INFO("✓ All tests passed!");

  /*
  volatile u32 *p[NUM_PAGES + 1];
  for (size_t i = 0; i < NUM_PAGES + 1; ++i) {
    p[i] = (volatile u32 *)(HEAP_START + i * PAGE_SIZE);
  }

  // ===== 1. Fault in P0 and make it dirty =====
  INFO("\n[1] Faulting in P0 and writing 0xCAFEBABE...");
  *p[0] = 0xCAFEBABE;

  // ===== 2. Wait for ONLY P0 to be demoted =====
  INFO("\n[2] Waiting 500ms for P0 to be demoted to inactive...");
  sleep_ms(500);
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    print_lists("After P0 demotion");
    // EXPECT: P0 is on the inactive list.
  }

  // ===== 3. Fault in the rest of the cache (P1, P2, P3) =====
  INFO("\n[3] Filling the rest of the cache to make P0 the oldest...");
  for (u32 i = 1; i < NUM_PAGES; ++i) {
    *p[i] = i; // Fault in and dirty the other pages
  }
  INFO("✓ Cache is now full.");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    print_lists("After filling cache");
    // EXPECT: P1, P2, P3 are on the active list. P0 is on the inactive list.
  }

  // ===== 4. Wait for all other pages to be demoted =====
  INFO("\n[4] Waiting 500ms for P1, P2, P3 to be demoted...");
  sleep_ms(500);
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    print_lists("After all pages are inactive");
    // EXPECT: All pages are on the inactive list. P0 should be at the back.
  }

  // ===== 5. Force eviction of the original dirty page (P0) =====
  INFO("\n[5] Faulting on P4 to force eviction of the coldest page (P0)...");
  *p[NUM_PAGES]; // Access p[4]
  INFO("✓ Eviction triggered.");
  {
    std::lock_guard<std::mutex> lock(pages_mutex);
    print_lists("After evicting P0");
    // EXPECT: The log should show P0 was DIRTY and swapped out.
    // The map state for P0 should be RemotelyMapped.
  }

  // ===== 6. Swap P0 back in and verify its contents =====
  INFO("\n[6] Faulting on P0 to trigger SWAP-IN...");
  u32 final_value = *p[0];
  INFO("✓ Read value from swapped-in P0: 0x%x", final_value);
  ENSURE(final_value == 0xCAFEBABE,
         "DATA CORRUPTION! Dirty page data was lost.");
  INFO("✓ SUCCESS: Dirty page write-back and swap-in confirmed!");
  */

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
      .log_level = INFO,
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
