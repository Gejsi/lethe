#include <cstdint>

#include "common.h"
#include "utils.h"

/* These are the RDMA resources needed to setup an RDMA connection */
/* Event channel, where connection management (cm) related events are relayed */
static struct rdma_event_channel *cm_event_channel = NULL;
static struct rdma_cm_id *cm_server_id = NULL, *cm_client_id = NULL;
static struct ibv_pd *pd = NULL;
static struct ibv_comp_channel *io_completion_channel = NULL;
static struct ibv_cq *cq = NULL;
static struct ibv_qp_init_attr qp_init_attr;
static struct ibv_qp *client_qp = NULL;
/* RDMA memory resources */
static struct ibv_mr *client_metadata_mr = NULL, *swap_area = NULL,
                     *server_metadata_mr = NULL;
static struct rdma_buffer_attr client_metadata_attr, server_metadata_attr;
static struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr = NULL;
static struct ibv_send_wr server_send_wr, *bad_server_send_wr = NULL;
static struct ibv_sge client_recv_sge, server_send_sge;

/* When we call this function cm_client_id must be set to a valid identifier.
 * This is where, we prepare client connection before we accept it. This
 * mainly involve pre-posting a receive buffer to receive client side
 * RDMA credentials
 */
static int setup_client_resources() {
  int ret = -1;
  if (!cm_client_id) {
    ERROR("Client id is still NULL");
    return -EINVAL;
  }

  /* We have a valid connection identifier, lets start to allocate
   * resources. We need:
   * 1. Protection Domains (PD)
   * 2. Memory Buffers
   * 3. Completion Queues (CQ)
   * 4. Queue Pair (QP)
   * Protection Domain (PD) is similar to a "process abstraction"
   * in the operating system. All resources are tied to a particular PD.
   * And accessing recourses across PD will result in a protection fault.
   */

  /* verbs defines a verb's provider,
   * i.e an RDMA device where the incoming
   * client connection came */
  pd = ibv_alloc_pd(cm_client_id->verbs);
  if (!pd) {
    ERROR("Failed to allocate a protection domain errno: %d", -errno);
    return -errno;
  }
  debug("A new protection domain is allocated at %p \n", pd);
  /* Now we need a completion channel, where the I/O completion
   * notifications are sent. Remember, this is different from connection
   * management (CM) event notifications.
   * A completion channel is also tied to an RDMA device, hence we will
   * use cm_client_id->verbs.
   */
  io_completion_channel = ibv_create_comp_channel(cm_client_id->verbs);
  if (!io_completion_channel) {
    ERROR("Failed to create an I/O completion event channel, %d", -errno);
    return -errno;
  }
  debug("An I/O completion event channel is created at %p \n",
        io_completion_channel);
  /* Now we create a completion queue (CQ) where actual I/O
   * completion metadata is placed. The metadata is packed into a structure
   * called struct ibv_wc (wc = work completion). ibv_wc has detailed
   * information about the work completion.
   * An I/O request in the RDMA world is called "work"
   */
  cq = ibv_create_cq(cm_client_id->verbs /* which device*/,
                     CQ_CAPACITY /* maximum capacity*/,
                     NULL /* user context, not used here */,
                     io_completion_channel /* which IO completion channel */,
                     0 /* signaling vector, not used here*/);
  if (!cq) {
    ERROR("Failed to create a completion queue (cq), errno: %d", -errno);
    return -errno;
  }
  debug("Completion queue (CQ) is created at %p with %d elements\n", cq,
        cq->cqe);
  /* Ask for the event for all activities in the completion queue*/
  ret = ibv_req_notify_cq(cq /* on which CQ */,
                          0 /* 0 = all event type, no filter*/);
  if (ret) {
    ERROR("Failed to request notifications on CQ errno: %d", -errno);
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
      cq; /* Where should I notify for receive completion operations */
  qp_init_attr.send_cq =
      cq; /* Where should I notify for send completion operations */
  /*Lets create a QP */
  ret = rdma_create_qp(cm_client_id /* which connection id */,
                       pd /* which protection domain*/,
                       &qp_init_attr /* Initial attributes */);
  if (ret) {
    ERROR("Failed to create QP due to errno: %d", -errno);
    return -errno;
  }
  /* Save the reference for handy typing but is not required */
  client_qp = cm_client_id->qp;
  debug("Client QP created at %p\n", client_qp);
  return ret;
}

/* Starts an RDMA server by allocating basic connection resources */
static int start_rdma_server(struct sockaddr_in *server_addr) {
  struct rdma_cm_event *cm_event = NULL;
  int ret = -1;
  /*  Open a channel used to report asynchronous communication event */
  cm_event_channel = rdma_create_event_channel();
  if (!cm_event_channel) {
    ERROR("Creating cm event channel failed with errno: %d", -errno);
    return -errno;
  }
  debug("RDMA CM event channel is created successfully at %p \n",
        cm_event_channel);
  /* rdma_cm_id is the connection identifier (like socket) which is used
   * to define an RDMA connection.
   */
  ret = rdma_create_id(cm_event_channel, &cm_server_id, NULL, RDMA_PS_TCP);
  if (ret) {
    ERROR("Creating server cm id failed with errno: %d", -errno);
    return -errno;
  }
  debug("A RDMA connection id for the server is created \n");
  /* Explicit binding of rdma cm id to the socket credentials */
  ret = rdma_bind_addr(cm_server_id, (struct sockaddr *)server_addr);
  if (ret) {
    ERROR("Failed to bind server address, errno: %d", -errno);
    return -errno;
  }
  debug("Server RDMA CM id is successfully binded\n");
  /* Now we start to listen on the passed IP and port. However unlike
   * normal TCP listen, this is a non-blocking call. When a new client is
   * connected, a new connection management (CM) event is generated on the
   * RDMA CM event channel from where the listening id was created. Here we
   * have only one channel, so it is easy. */
  ret = rdma_listen(cm_server_id,
                    8); /* backlog = 8 clients, same as TCP, see man listen*/
  if (ret) {
    ERROR("rdma_listen failed to listen on server address, errno: %d", -errno);
    return -errno;
  }
  printf("Server is listening successfully at: %s, port: %d\n",
         inet_ntoa(server_addr->sin_addr), ntohs(server_addr->sin_port));
  /* now, we expect a client to connect and generate a
   * RDMA_CM_EVNET_CONNECT_REQUEST. Wait (block) on the connection management
   * event channel for the connect event.
   */
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_CONNECT_REQUEST,
                              &cm_event);
  if (ret) {
    ERROR("Failed to get cm event, ret = %d", ret);
    return ret;
  }
  /* Much like TCP connection, listening returns a new connection identifier
   * for newly connected client. In the case of RDMA, this is stored in id
   * field. For more details: man rdma_get_cm_event
   */
  cm_client_id = cm_event->id;
  /* now we acknowledge the event. Acknowledging the event frees the resources
   * associated with the event structure. Hence any reference to the event
   * must be made before acknowledgment. Like, we have already saved the
   * client id from "id" field before acknowledging the event.
   */
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge the cm event errno: %d", -errno);
    return -errno;
  }
  debug("A new RDMA client connection id is stored at %p\n", cm_client_id);
  return ret;
}

/* Pre-posts a receive buffer and accepts an RDMA client connection */
static int accept_client_connection() {
  struct rdma_conn_param conn_param;
  struct rdma_cm_event *cm_event = NULL;
  int ret = -1;
  if (!cm_client_id || !client_qp) {
    ERROR("Client resources are not properly setup");
    return -EINVAL;
  }
  /* we prepare the receive buffer in which we will receive the client
   * metadata*/
  client_metadata_mr = rdma_buffer_register(
      pd /* which protection domain */, &client_metadata_attr /* what memory */,
      sizeof(client_metadata_attr),
      (IBV_ACCESS_LOCAL_WRITE) /* access permissions */);
  if (!client_metadata_mr) {
    ERROR("Failed to register client attr buffer");
    // we assume ENOMEM
    return -ENOMEM;
  }
  /* We pre-post this receive buffer on the QP. SGE credentials is where we
   * receive the metadata from the client */
  client_recv_sge.addr = (uint64_t)client_metadata_mr->addr;
  client_recv_sge.length = (uint32_t)client_metadata_mr->length;
  client_recv_sge.lkey = client_metadata_mr->lkey;
  /* Now we link this SGE to the work request (WR) */
  memset(&client_recv_wr, 0, sizeof(client_recv_wr));
  client_recv_wr.sg_list = &client_recv_sge;
  client_recv_wr.num_sge = 1; // only one SGE
  ret = ibv_post_recv(client_qp /* which QP */,
                      &client_recv_wr /* receive work request*/,
                      &bad_client_recv_wr /* error WRs */);
  if (ret) {
    ERROR("Failed to pre-post the receive buffer, errno: %d", ret);
    return ret;
  }
  debug("Receive buffer pre-posting is successful\n");
  /* Now we accept the connection. Recall we have not accepted the connection
   * yet because we have to do lots of resource pre-allocation */
  memset(&conn_param, 0, sizeof(conn_param));
  /* this tell how many outstanding requests can we handle */
  conn_param.initiator_depth =
      3; /* For this exercise, we put a small number here */
  /* This tell how many outstanding requests we expect other side to handle */
  conn_param.responder_resources =
      3; /* For this exercise, we put a small number */
  ret = rdma_accept(cm_client_id, &conn_param);
  if (ret) {
    ERROR("Failed to accept the connection, errno: %d", -errno);
    return -errno;
  }
  /* We expect an RDMA_CM_EVNET_ESTABLISHED to indicate that the RDMA
   * connection has been established and everything is fine on both, server
   * as well as the client sides.
   */
  debug("Going to wait for: RDMA_CM_EVENT_ESTABLISHED event\n");
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_ESTABLISHED,
                              &cm_event);
  if (ret) {
    ERROR("Failed to get the cm event, errnp: %d", -errno);
    return -errno;
  }
  /* We acknowledge the event */
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge the cm event %d", -errno);
    return -errno;
  }
  /* extract connection information for logs */
  struct sockaddr_in remote_sockaddr;
  memcpy(&remote_sockaddr /* where to save */,
         rdma_get_peer_addr(cm_client_id) /* gives you remote sockaddr */,
         sizeof(struct sockaddr_in) /* max size */);
  printf("A new connection is accepted at %s\n",
         inet_ntoa(remote_sockaddr.sin_addr));
  return ret;
}

/* This function sends server side buffer metadata to the connected client */
static int send_server_metadata_to_client() {
  struct ibv_wc wc;
  int ret = -1;
  /* Now, we first wait for the client to start the communication by
   * sending the server its metadata info. The server does not use it
   * in our example. We will receive a work completion notification for
   * our pre-posted receive request.
   */
  ret = await_work_completion_events(io_completion_channel, &wc, 1);
  if (ret != 1) {
    ERROR("Failed to receive: %d", ret);
    return ret;
  }
  /* check if have client's metadata */
  printf("Client side buffer information is received...\n");
  show_rdma_buffer_attr(&client_metadata_attr);
  printf("The client has requested buffer length of: %u bytes \n",
         client_metadata_attr.length);
  /* We need to setup requested memory buffer. This is where the client will
   * do RDMA READs and WRITEs. */
  DEBUG("AAAAAAAAAAAAAAA %u", client_metadata_attr.length);
  swap_area = rdma_buffer_alloc(
      pd, client_metadata_attr.length /* what size to allocate */,
      static_cast<ibv_access_flags>(
          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
          IBV_ACCESS_REMOTE_WRITE) /* access permissions */);
  if (!swap_area) {
    ERROR("Server failed to allocate the necessary swap area");
    /* we assume that it is due to out of memory error */
    return -ENOMEM;
  }
  /* This buffer is used to transmit information about the above
   * buffer to the client. So this contains the metadata about the server
   * buffer. Hence this is called metadata buffer. Since this is already
   * allocated, we just register it. We need to prepare a send I/O operation
   * that will tell the client the address of the server buffer.
   */
  server_metadata_attr.address = (uint64_t)swap_area->addr;
  server_metadata_attr.length = (uint32_t)swap_area->length;
  server_metadata_attr.stag.local_stag = (uint32_t)swap_area->lkey;
  server_metadata_mr = rdma_buffer_register(
      pd /* which protection domain*/,
      &server_metadata_attr /* which memory to register */,
      sizeof(server_metadata_attr) /* what is the size of memory */,
      IBV_ACCESS_LOCAL_WRITE /* what access permission */);
  if (!server_metadata_mr) {
    ERROR("Server couldn't allocate memory to send the client its metadata");
    /* we assume that this is due to out of memory error */
    return -ENOMEM;
  }
  /* We need to transmit this buffer. So we create a send request.
   * A send request consists of multiple SGE elements. In our case, we only
   * have one
   */
  server_send_sge.addr = (uint64_t)&server_metadata_attr;
  server_send_sge.length = sizeof(server_metadata_attr);
  server_send_sge.lkey = server_metadata_mr->lkey;
  /* now we link this sge to the send request */
  memset(&server_send_wr, 0, sizeof(server_send_wr));
  server_send_wr.sg_list = &server_send_sge;
  server_send_wr.num_sge = 1;          // only 1 SGE element in the array
  server_send_wr.opcode = IBV_WR_SEND; // This is a send request
  server_send_wr.send_flags = IBV_SEND_SIGNALED; // We want to get notification
  /* This is a fast data path operation. Posting an I/O request */
  ret = ibv_post_send(
      client_qp /* which QP */,
      &server_send_wr /* Send request that we prepared before */, &bad_server_send_wr /* In case of error, this will contain failed requests */);
  if (ret) {
    ERROR("Posting of server metdata failed, errno: %d", -errno);
    return -errno;
  }
  /* We check for completion notification */
  ret = await_work_completion_events(io_completion_channel, &wc, 1);
  if (ret != 1) {
    ERROR("Failed to send server metadata, ret = %d", ret);
    return ret;
  }
  debug("Local buffer metadata has been sent to the client \n");
  return 0;
}

static void cleanup() {
  int ret;

  debug("Forcing cleanup of all known RDMA resources...\n");

  // Order of destruction should generally be reverse of allocation,
  // or based on dependencies (e.g., QPs before CQs/PDs they use, MRs before
  // PDs).

  // Client-specific resources (QP, CM ID, MRs)
  if (cm_client_id) {
    if (cm_client_id->qp) {
      debug("Destroying QP associated with cm_client_id %p\n",
            (void *)cm_client_id);
      rdma_destroy_qp(cm_client_id);
    }
    client_qp = NULL;

    debug("Destroying client CM ID %p\n", (void *)cm_client_id);
    ret = rdma_destroy_id(cm_client_id);
    if (ret) {
      ERROR("Failed to destroy client CM ID cleanly, errno: %d. Continuing.",
            -errno);
    }
    cm_client_id = NULL;
  }
  client_qp = NULL;

  // Memory Regions
  if (swap_area) {
    debug("Freeing swap area %p\n", (void *)server_buffer_mr);
    rdma_buffer_free(swap_area);
    swap_area = NULL;
  }
  if (server_metadata_mr) {
    debug("Deregistering server_metadata_mr %p\n", (void *)server_metadata_mr);
    rdma_buffer_deregister(server_metadata_mr);
    server_metadata_mr = NULL;
  }
  if (client_metadata_mr) {
    debug("Deregistering client_metadata_mr %p\n", (void *)client_metadata_mr);
    rdma_buffer_deregister(client_metadata_mr);
    client_metadata_mr = NULL;
  }

  // Completion Queue and Channel
  if (cq) {
    debug("Destroying CQ %p\n", (void *)cq);
    ret = ibv_destroy_cq(cq);
    if (ret) {
      ERROR("Failed to destroy CQ cleanly, errno: %d. Continuing.", -errno);
    }
    cq = NULL;
  }
  if (io_completion_channel) {
    debug("Destroying IO completion channel %p\n",
          (void *)io_completion_channel);
    ret = ibv_destroy_comp_channel(io_completion_channel);
    if (ret) {
      ERROR("Failed to destroy IO completion channel cleanly, errno: %d. "
            "Continuing.",
            -errno);
    }
    io_completion_channel = NULL;
  }

  // Protection Domain
  if (pd) {
    debug("Deallocating PD %p\n", (void *)pd);
    ret = ibv_dealloc_pd(pd);
    if (ret) {
      ERROR("Failed to deallocate PD cleanly, errno: %d. Continuing.", -errno);
    }
    pd = NULL;
  }

  // Server's listening ID and main event channel
  if (cm_server_id) {
    debug("Destroying server CM ID %p\n", (void *)cm_server_id);
    ret = rdma_destroy_id(cm_server_id);
    if (ret) {
      ERROR("Failed to destroy server CM ID cleanly, errno: %d. Continuing.",
            -errno);
    }
    cm_server_id = NULL;
  }
  if (cm_event_channel) {
    debug("Destroying CM event channel %p\n", (void *)cm_event_channel);
    rdma_destroy_event_channel(cm_event_channel);
    cm_event_channel = NULL;
  }

  debug("Resources cleanup complete.\n");
}

/* This is server side logic. Server passively waits for the client to call
 * rdma_disconnect() and then it will clean up its resources */
static int disconnect_and_cleanup() {
  struct rdma_cm_event *cm_event = NULL; // Important: initialize to NULL
  int ret = -1;

  if (!cm_event_channel) {
    ERROR("CM Event Channel is NULL, cannot process disconnect event. "
          "Proceeding to force cleanup.\n");
    cleanup();
    return -1;
  }

  // Now we wait for the client to send us disconnect event
  debug("Waiting for cm event: RDMA_CM_EVENT_DISCONNECTED\n");
  ret = process_rdma_cm_event(cm_event_channel, RDMA_CM_EVENT_DISCONNECTED,
                              &cm_event);
  if (ret) {
    ERROR("Failed to get disconnect event, ret = %d. Proceeding to force "
          "cleanup.\n",
          ret);
    cleanup();
    return ret;
  }

  /* We acknowledge the event */
  ret = rdma_ack_cm_event(cm_event);
  if (ret) {
    ERROR("Failed to acknowledge the cm event errno: %d. Proceeding to force "
          "cleanup.\n",
          -errno);
    cleanup();
    return -errno;
  }
  printf("A disconnect event is received from the client...\n");

  cleanup();

  printf("Server shut-down is complete.\n");
  return 0;
}

void usage() {
  printf("Usage:\n");
  printf("server [-a <server_addr>] [-p <server_port>]\n");
  printf("(default port is %d)\n", DEFAULT_RDMA_PORT);
  exit(1);
}

int main(int argc, char **argv) {
  int ret, option;
  struct sockaddr_in server_sockaddr;
  memset(&server_sockaddr, 0, sizeof server_sockaddr);
  server_sockaddr.sin_family = AF_INET; /* standard IP NET address */
  server_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY); /* passed address */
  /* Parse Command Line Arguments, not the most reliable code */
  while ((option = getopt(argc, argv, "a:p:")) != -1) {
    switch (option) {
    case 'a':
      /* Remember, this will overwrite the port info */
      ret = get_addr(optarg, (struct sockaddr *)&server_sockaddr);
      if (ret) {
        ERROR("Invalid IP");
        return ret;
      }
      break;
    case 'p':
      /* passed port to listen on */
      server_sockaddr.sin_port = htons((uint16_t)strtol(optarg, NULL, 0));
      break;
    default:
      usage();
      break;
    }
  }
  if (!server_sockaddr.sin_port) {
    /* If still zero, that mean no port info provided */
    server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT); /* use default port */
  }
  ret = start_rdma_server(&server_sockaddr);
  if (ret) {
    ERROR("RDMA server failed to start cleanly, ret = %d", ret);
    goto cleanup_error;
  }
  ret = setup_client_resources();
  if (ret) {
    ERROR("Failed to setup client resources, ret = %d", ret);
    goto cleanup_error;
  }
  ret = accept_client_connection();
  if (ret) {
    ERROR("Failed to handle client cleanly, ret = %d", ret);
    goto cleanup_error;
  }
  ret = send_server_metadata_to_client();
  if (ret) {
    ERROR("Failed to send server metadata to the client, ret = %d", ret);
    goto cleanup_error;
  }
  ret = disconnect_and_cleanup();
  if (ret) {
    ERROR("Failed to clean up resources properly, ret = %d", ret);
  }

  return ret;

cleanup_error:
  ERROR("An error occurred. Cleaning up resources.\n");
  cleanup();
  return (ret == 0 ? -1 : ret);
}
