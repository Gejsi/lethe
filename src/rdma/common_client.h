#pragma once

#include "common.h"
#include "swapper.h"

/* RDMA connection related resources */
extern struct rdma_event_channel *cm_event_channel;
extern struct rdma_cm_id *cm_client_id;
extern struct ibv_pd *pd; // protection domain
extern struct ibv_comp_channel *io_completion_channel;
extern struct ibv_cq *client_cq; // completion queue
extern struct ibv_qp *client_qp; // queue pair

// Metadata Exchange: before one side can RDMA READ or WRITE to the other
// side's memory, it needs to know:
//  - The virtual address of the remote buffer.
//  - The length of the remote buffer.
//  - The remote memory key (rkey) authorizing access to that buffer.
// This information (often called "buffer attributes" or "metadata") is
// typically exchanged using SEND/RECV operations after the connection is
// established.
extern struct ibv_mr *swap_area;
extern struct rdma_buffer_attr swap_area_metadata;
/* the cache where RDMA operations source and sink */
extern struct ibv_mr *cache_area;

// Global pointer used in the VoliMem fault handler callback
extern Swapper *g_swapper;

/* This function prepares client side connection resources
 * for an RDMA connection */
int prepare_connection(struct sockaddr_in *s_addr);

/* Pre-posts a receive buffer before calling rdma_connect() */
int pre_post_recv_buffer();

/* Connects to the RDMA server */
int connect_to_server();

/**
 * @brief Waits to receive the server's swap area metadata.
 *
 * This function assumes that a receive buffer has already been pre-posted.
 * It blocks until the server's metadata arrives.
 * @return 0 on success, error code on failure.
 */
int receive_server_metadata();

/* This function disconnects the RDMA connection from the server and cleans up
 * all the resources.
 */
int disconnect_and_cleanup();
