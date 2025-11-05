#ifndef QEMU_TIMER_H
#define QEMU_TIMER_H

#include <stdint.h>
#include <poll.h>

/*
 * QEMU Timer and Polling Stubs
 */

#define QEMU_CLOCK_REALTIME  0
#define QEMU_CLOCK_VIRTUAL   1
#define QEMU_CLOCK_HOST      2

int64_t qemu_clock_get_ns(int type);
int qemu_poll_ns(struct pollfd *fds, nfds_t nfds, int64_t timeout_ns);

#endif /* QEMU_TIMER_H */

