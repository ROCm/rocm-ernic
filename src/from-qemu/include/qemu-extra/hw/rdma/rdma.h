#ifndef HW_RDMA_RDMA_H
#define HW_RDMA_RDMA_H

#include <stdint.h>

/*
 * RDMA Backend Function Declarations
 * Note: These are forward declarations for functions implemented in QEMU source
 * files
 */

struct RdmaDeviceResources;
struct RdmaBackendDev;
struct ibv_device_attr;

void rdma_backend_destroy(void *backend_dev);
int rdma_rm_init(struct RdmaDeviceResources *rdma_dev_res,
                 struct ibv_device_attr *dev_attr);
void rdma_rm_fini(struct RdmaDeviceResources *rdma_dev_res,
                  struct RdmaBackendDev *backend_dev, const char *ifname);

/* DMA mapping functions - forward declarations */
void *rdma_pci_dma_map(void *dev, uint64_t addr, uint64_t len);
void rdma_pci_dma_unmap(void *dev, void *buffer, uint64_t len);

#endif /* HW_RDMA_RDMA_H */
