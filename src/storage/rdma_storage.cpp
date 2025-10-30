#include "rdma_storage.h"
#include "swapper.h"
#include "utils.h"

RDMAStorage::RDMAStorage(struct ibv_qp *qp,
                         struct ibv_comp_channel *comp_channel,
                         struct ibv_mr *local_cache_mr,
                         const rdma_buffer_attr &remote_swap_metadata)
    : qp_(qp), comp_channel_(comp_channel), local_cache_mr_(local_cache_mr),
      remote_swap_metadata_(remote_swap_metadata) {
  // Ensure all required resources are provided.
  if (!qp_ || !comp_channel_ || !local_cache_mr_) {
    PANIC("RDMAStorage created with null resources.");
  }
}

int RDMAStorage::read_page(void *local_dest, u64 remote_offset) {
  return perform_rdma_op(local_dest, remote_offset, IBV_WR_RDMA_READ);
}

int RDMAStorage::write_page(void *local_src, u64 remote_offset) {
  return perform_rdma_op(local_src, remote_offset, IBV_WR_RDMA_WRITE);
}

int RDMAStorage::perform_rdma_op(void *local_addr, u64 remote_offset,
                                 ibv_wr_opcode opcode) {
  struct ibv_sge sge;
  sge.addr = (u64)local_addr;
  sge.length = PAGE_SIZE;
  sge.lkey = local_cache_mr_->lkey;

  struct ibv_send_wr wr, *bad_wr = nullptr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 0; // We use synchronous ops, so no ID is needed.
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = opcode;
  wr.send_flags = IBV_SEND_SIGNALED;

  // Set the remote address and key for the one-sided operation.
  wr.wr.rdma.remote_addr = remote_swap_metadata_.address + remote_offset;
  wr.wr.rdma.rkey = remote_swap_metadata_.stag.remote_stag;

  const char *op_str = (opcode == IBV_WR_RDMA_READ) ? "READ" : "WRITE";
  DEBUG("Posting RDMA %s: local=%p, remote_offset=0x%lx", op_str, local_addr,
        remote_offset);

  // Post the Work Request to the Send Queue.
  int ret = ibv_post_send(qp_, &wr, &bad_wr);
  if (ret) {
    ERROR("Failed to post RDMA %s, errno: %d", op_str, -errno);
    return -errno;
  }

  // Wait for the operation to complete.
  struct ibv_wc wc;
  ret = await_work_completion_events(comp_channel_, &wc, 1);
  if (ret != 1) {
    ERROR("RDMA %s failed to get completion, ret = %d", op_str, ret);
    return ret;
  }

  DEBUG("RDMA %s completed successfully.", op_str);
  return 0;
}

void *RDMAStorage::get_cache_base_addr() const { return local_cache_mr_->addr; }
