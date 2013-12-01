/*
 * Linux kernel aio headers
 */

#include <linux/aio_abi.h>
#include <sys/time.h>

int io_setup(unsigned nr_events, aio_context_t *ctx_idp);
int io_submit(aio_context_t ctx_id, long nr, struct iocb **iocbpp);
int io_destroy(aio_context_t ctx_id);
int io_getevents(aio_context_t ctx_id, long min_nr, long nr, struct io_event *events, struct timespec *timeout);


