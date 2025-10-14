#pragma once

#include <arpa/inet.h>
#include <cstdio>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "utils.h"

#ifdef ACN_RDMA_DEBUG
/* Debug Macro */
#define debug(msg, ...)                                                        \
  do {                                                                         \
    printf("DEBUG: " msg, ##__VA_ARGS__);                                      \
  } while (0)
#else
#define debug(...)
#endif /* ACN_RDMA_DEBUG */

/*
 * MAX work requests (WR).
 * This defines the capacity of the Send Queue and Receive Queue within a
 * Queue Pair (QP). It limits the number of RDMA operations you can post
 * before you absolutely *must* poll the CQ to see if some have completed.
 *
 * The Send Queue (SQ) and Receive Queue (RQ) are queues on the RNIC where
 * the "Work Requests" (WRs) are posted using ibv_post_send() and
 * ibv_post_recv(). This number is their depth.
 */
constexpr usize MAX_WR = 512;

/*
 * Capacity of the completion queue (CQ).
 *
 * The Completion Queue (CQ) is a circular buffer in memory where
 * the RDMA hardware (RNIC) places a "Work Completion" (WC) entry every time
 * an operation (like a READ, WRITE, or SEND) finishes. This value defines the
 * maximum number of completion entries the CQ can hold.
 * A good rule of thumb is CQ_CAPACITY >= (2 * MAX_WR).
 */
constexpr usize CQ_CAPACITY = 1024;

/*
 * An SGE (struct ibv_sge) describes a contiguous block of memory
 * with its address, length, and lkey. This defines the maximum number of SGEs
 * you can have in a single Work Request.
 *
 * SGE stands for Scatter/Gather Element. It describes
 * a single RDMA operation that reads from multiple separate memory locations
 * and writes them into a single contiguous block on the remote end (gather),
 * or reads from a single block and writes it to multiple locations (scatter).
 *
 * Since the swapper operates on single, contiguous pages, each RDMA operation
 * (READ or WRITE) only ever requires one SGE. A value of 1 is sufficient.
 */
constexpr usize MAX_SGE = 1;

/* Default address and port where the RDMA server is listening */
constexpr const char *DEFAULT_SERVER_ADDR = "10.0.0.1";
constexpr u16 DEFAULT_RDMA_PORT = 20886;

/*
 * Use this structure to exchange information between the server
 * and the client. Pack it to avoid padding from the compiler.
 */
struct __attribute((packed)) rdma_buffer_attr {
  uint64_t address;
  uint32_t length;
  // Steering Tag (or memory key)
  union stag {
    /* if we send, we call it local stag */
    uint32_t local_stag;
    /* if we receive, we call it remote stag */
    uint32_t remote_stag;
  } stag;
};

/* resolves a given destination name to sin_addr */
int get_addr(char *dst, struct sockaddr *addr);

/*
 * Processes an RDMA connection management (CM) event.
 * @echannel: CM event channel where the event is expected.
 * @expected_event: Expected event type
 * @cm_event: where the event will be stored
 */
int process_rdma_cm_event(struct rdma_event_channel *echannel,
                          enum rdma_cm_event_type expected_event,
                          struct rdma_cm_event **cm_event);

/* Allocates an RDMA buffer of size 'length' with permissions 'permission'. This
 * function will also register the memory and returns a memory region (MR)
 * identifier or NULL on error.
 * @pd: Protection domain where the buffer should be allocated
 * @length: Length of the buffer
 * @permission: OR of IBV_ACCESS_* permissions as defined for the enum
 * ibv_access_flags
 */
struct ibv_mr *rdma_buffer_alloc(struct ibv_pd *pd, usize alignment, usize size,
                                 enum ibv_access_flags permission);

/* Frees a previously allocated RDMA buffer. The buffer must be allocated by
 * calling rdma_buffer_alloc();
 * @mr: RDMA memory region to free
 */
void rdma_buffer_free(struct ibv_mr *mr);

/* This function registers a previously allocated memory. Returns a memory
 * region (MR) identifier or NULL on error.
 * @pd: protection domain where to register memory
 * @addr: Buffer address
 * @length: Length of the buffer
 * @permission: OR of IBV_ACCESS_* permissions as defined for the enum
 * ibv_access_flags
 */
struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd, void *addr,
                                    uint32_t length,
                                    enum ibv_access_flags permission);

/* Deregisters a previously register memory
 * @mr: Memory region to deregister
 */
void rdma_buffer_deregister(struct ibv_mr *mr);

/* Processes a work completion (WC) notification.
 * @comp_channel: Completion channel where the notifications are expected to
 * arrive
 * @wc: Array where to hold the work completion elements
 * @max_wc: Maximum number of expected work completion (WC) elements. wc must be
 *          atleast this size.
 */
int await_work_completion_events(struct ibv_comp_channel *comp_channel,
                                 struct ibv_wc *wc, int max_wc);

/* prints some details from the cm id */
void show_rdma_cmid(struct rdma_cm_id *id);
