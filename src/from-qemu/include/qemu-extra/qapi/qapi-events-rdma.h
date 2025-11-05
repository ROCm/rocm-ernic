#ifndef QAPI_EVENTS_RDMA_H
#define QAPI_EVENTS_RDMA_H

#include <stdint.h>

/*
 * QAPI RDMA Event Stubs
 */

void qapi_event_send_rdma_gid_status_changed(const char *netdev,
                                              bool gid_status_changed,
                                              uint64_t subnet_prefix,
                                              uint64_t interface_id);

#endif /* QAPI_EVENTS_RDMA_H */

