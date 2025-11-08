#include "common_client.h"

struct rdma_event_channel *cm_event_channel = NULL;
struct rdma_cm_id *cm_client_id = NULL;
struct ibv_pd *pd = NULL;
struct ibv_comp_channel *io_completion_channel = NULL;
struct ibv_cq *client_cq = NULL;
struct ibv_qp *client_qp;
struct ibv_mr *swap_area = NULL;
struct rdma_buffer_attr swap_area_metadata;
struct ibv_mr *cache_area = NULL;
Swapper *g_swapper = nullptr;

/* This function prepares client side connection resources for an RDMA
 * connection */
int prepare_connection(struct sockaddr_in *s_addr) {
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
  struct ibv_qp_init_attr qp_init_attr;
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
int pre_post_recv_buffer() {
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
int connect_to_server() {
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
 * @brief Waits to receive the server's swap area metadata.
 *
 * This function assumes that a receive buffer has already been pre-posted.
 * It blocks until the server's metadata arrives.
 * @return 0 on success, error code on failure.
 */
int receive_server_metadata() {
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
int disconnect_and_cleanup() {
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
