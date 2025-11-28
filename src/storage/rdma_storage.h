#pragma once

#include "rdma/common.h"
#include "storage.h"

// An implementation of the Storage interface that uses RDMA for I/O.
class RDMAStorage : public Storage {
public:
  /**
   * @brief Constructs an RDMAStorage backend.
   *
   * @param qp The initialized and connected Queue Pair for RDMA operations.
   * @param comp_channel The completion channel to wait for WC events on.
   * @param local_cache_mr A pointer to the memory region of the local cache.
   * @param remote_swap_metadata The metadata (addr, rkey) of the remote swap
   * area.
   */
  RDMAStorage(struct ibv_qp *qp, struct ibv_comp_channel *comp_channel,
              struct ibv_mr *local_cache_mr,
              const rdma_buffer_attr &remote_swap_metadata);

  int read_page(void *local_dest, u64 remote_offset) override;

  int write_page(void *local_src, u64 remote_offset) override;

  void *get_cache_base_addr() const override;

private:
  // Performs a single, one-way RDMA operation (READ or WRITE)
  int perform_rdma_op(void *local_addr, u64 remote_offset,
                      ibv_wr_opcode opcode);

  // Non-owning pointers to the RDMA resources
  struct ibv_qp *qp_;
  struct ibv_comp_channel *comp_channel_;
  struct ibv_mr *local_cache_mr_;
  const rdma_buffer_attr &remote_swap_metadata_;
};
