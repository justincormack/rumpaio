/*-
 * Copyright (c) 2013 Antti Kantee, Justin Cormack.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "rumpuser_port.h"

#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rump/rumpuser.h>

#include "rumpuser_int.h"
#include "aio.h"

struct rumpuser_bio {
	int bio_fd;
	int bio_op;
	void *bio_data;
	size_t bio_dlen;
	off_t bio_off;

	rump_biodone_fn bio_done;
	void *bio_donearg;

	struct iocb iocb;
};

#define N_BIOS 32
static pthread_mutex_t biomtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t notfull = PTHREAD_COND_INITIALIZER;
static int bio_head, bio_tail;
static struct rumpuser_bio bios[N_BIOS];
static aio_context_t ctx = 0;
static struct io_event ioev[N_BIOS];

static void *
riothread(void *arg)
{
	struct rumpuser_bio *biop;
	struct iocb *iocb;
	int i, n, dummy, rv, sync;
	int res, error;

	rumpuser__hyp.hyp_schedule();
	rv = rumpuser__hyp.hyp_lwproc_newlwp(0);
	assert(rv == 0);
	rumpuser__hyp.hyp_unschedule();

	for (;;) {
		n = io_getevents(ctx, 1, N_BIOS, ioev, NULL);
		rumpkern_sched(0, NULL);
		for (i = 0; i < n; i++) {	
			biop = (struct rumpuser_bio *) ioev[i].data;
			iocb = (struct iocb *) ioev[i].obj;
			sync = biop->bio_op & RUMPUSER_BIO_WRITE && biop->bio_op & RUMPUSER_BIO_SYNC;
			/* if it is sync, sync it now */
			if (sync) {
				fdatasync(biop->bio_fd);
			}
			error = 0;
			res = ioev[i].res;
			if (res < 0) {
				error = -res;
				res = -1;
			}
			biop->bio_done(biop->bio_donearg, (size_t) res, error);
			biop->bio_donearg = NULL; /* paranoia */
			NOFAIL_ERRNO(pthread_mutex_lock(&biomtx));
			bio_tail = (bio_tail+1) % N_BIOS;
			pthread_mutex_unlock(&biomtx);
			pthread_cond_signal(&notfull);
		}
		rumpkern_unsched(&dummy, NULL);
	}
	/* unreachable */
	abort();
}

static void
dobio(struct rumpuser_bio *biop)
{
	struct iocb *iocb = &biop->iocb;
	struct iocb *iocbpp[1];
	int ret;
	const struct timespec shorttime = {0, 1000};

	memset(iocb, 0, sizeof(struct iocb));
        iocbpp[0] = iocb;

	iocb->aio_fildes = biop->bio_fd;
	iocb->aio_buf = (uint64_t) biop->bio_data;
	iocb->aio_nbytes = biop->bio_dlen;
	iocb->aio_offset = biop->bio_off;
	iocb->aio_data = (uint64_t) biop;

	assert(biop->bio_donearg != NULL);
	if (biop->bio_op & RUMPUSER_BIO_READ) {
		iocb->aio_lio_opcode = IOCB_CMD_PREAD;
	} else {
		iocb->aio_lio_opcode = IOCB_CMD_PWRITE;
	}

	// May have to retry (very unlikely)
	do {
		ret = io_submit(ctx, 1, iocbpp);
		if (ret > 0) break;
		assert(errno == EAGAIN);
		nanosleep(&shorttime, NULL);
	} while (ret < 0);
}

void
rumpuser_bio(int fd, int op, void *data, size_t dlen, int64_t doff,
	rump_biodone_fn biodone, void *bioarg)
{
	struct rumpuser_bio *bio;
	static int inited = 0;
	int nlocks;

	rumpkern_unsched(&nlocks, NULL);

	if (!inited) {
		pthread_mutex_lock(&biomtx);
		if (!inited) {
			pthread_t rt;
			io_setup(N_BIOS, &ctx);
			pthread_create(&rt, NULL, riothread, NULL);
			inited = 1;
		}
		pthread_mutex_unlock(&biomtx);
		assert(inited);
	}

	pthread_mutex_lock(&biomtx);
	while ((bio_head+1) % N_BIOS == bio_tail)
		pthread_cond_wait(&notfull, &biomtx);

	bio = &bios[bio_head];
	bio->bio_fd = fd;
	bio->bio_op = op;
	bio->bio_data = data;
	bio->bio_dlen = dlen;
	bio->bio_off = (off_t)doff;
	bio->bio_done = biodone;
	bio->bio_donearg = bioarg;

	dobio(bio); // this might do EAGAIN (unlikely) - TODO release lock if does
	bio_head = (bio_head+1) % N_BIOS;

	pthread_mutex_unlock(&biomtx);

	rumpkern_sched(nlocks, NULL);
}
