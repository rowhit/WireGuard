/*
 * Copyright 2009-2015 Samy Al Bahra.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef MPMC_RING_H
#define MPMC_RING_H

#ifndef CK_CC_INLINE
#define CK_CC_INLINE inline /* inline is discouraged in the kernel */
#endif

#ifndef CK_CC_FORCE_INLINE
#define CK_CC_FORCE_INLINE __always_inline
#endif

#include <stdbool.h>
#include <linux/string.h>

#include <linux/processor.h>
#include <linux/compiler.h>
#include <linux/atomic.h>
#include <linux/cache.h>

/* http://concurrencykit.org/doc/ck_pr_load.html */
#define ck_pr_load_uint(SRC) atomic_read(SRC)

/* http://concurrencykit.org/doc/ck_pr_fence_load.html */
#define ck_pr_fence_load() smp_rmb()

/* http://concurrencykit.org/doc/ck_pr_fence_store.html */
#define ck_pr_fence_store() smp_wmb()

/* http://concurrencykit.org/doc/ck_pr_stall.html */
#define ck_pr_stall() cpu_relax()

/* http://concurrencykit.org/doc/ck_pr_fence_store_atomic.html */
/* this actually resolves to __asm__ __volatile__("" ::: "memory"); in x86-64 */
/* so basically a compiler barrier? */
#define ck_pr_fence_store_atomic() smp_mb__before_atomic() /* TODO: probably overkill? */

/* http://concurrencykit.org/doc/ck_pr_cas.html */
/*
    ck_pr_cas_uint_value(unsigned int *target, unsigned int compare,
                         unsigned int set, unsigned int *v) {
    _Bool
    z; __asm__ __volatile__("lock " "cmpxchg" "l" " %3, %0;" "mov %% " "eax" ", %2;" "setz %1;" : "+m" (*(unsigned int *)target), "=a" (z), "=m" (*(unsigned int *)v) : "q" (set), "a" (compare) : "memory", "cc");
    return z; }
*/
__always_inline static
bool ck_pr_cas_uint_value(atomic_t *target, uint old, uint new, uint *v) {
	uint prev = atomic_cmpxchg(target, old, new);
	*v = new;
	return prev != old;
}

/* http://concurrencykit.org/doc/ck_pr_cas.html */
#define ck_pr_cas_uint(t, old, new) atomic_cmpxchg(t, old, new) != old

/* http://concurrencykit.org/doc/ck_pr_store.html */
// TODO: compiler barrier?
#define ck_pr_store_uint(A, B) atomic_set((A), (B))

/*
 * Concurrent ring buffer.
 */

struct ck_ring {
	/* TODO: is the aligment correct? */
	atomic_t c_head ____cacheline_aligned_in_smp;
	atomic_t p_tail ____cacheline_aligned_in_smp;
	atomic_t p_head;
	unsigned int size ____cacheline_aligned_in_smp;
	unsigned int mask;
};
typedef struct ck_ring ck_ring_t;

struct ck_ring_buffer {
	void *value;
};
typedef struct ck_ring_buffer ck_ring_buffer_t;

CK_CC_INLINE static void
ck_ring_init(struct ck_ring *ring, unsigned int size)
{
	ring->size = size;
	ring->mask = size - 1;
	atomic_set(&ring->p_tail, 0);
	atomic_set(&ring->p_head, 0); // TODO: barrier?
	atomic_set(&ring->c_head, 0);
	return;
}

CK_CC_FORCE_INLINE static bool
_ck_ring_enqueue_mp(struct ck_ring *ring,
    void *buffer,
    const void *entry,
    unsigned int ts,
    unsigned int *size)
{
	const unsigned int mask = ring->mask;
	unsigned int producer, consumer, delta;
	bool r = true;

	producer = ck_pr_load_uint(&ring->p_head);

	for (;;) {
		/*
		 * The snapshot of producer must be up to date with respect to
		 * consumer.
		 */
		ck_pr_fence_load();
		consumer = ck_pr_load_uint(&ring->c_head);

		delta = producer + 1;

		/*
		 * Only try to CAS if the producer is not clearly stale (not
		 * less than consumer) and the buffer is definitely not full.
		 */
		if (likely((producer - consumer) < mask)) {
			if (ck_pr_cas_uint_value(&ring->p_head,
			    producer, delta, &producer) == true) {
				break;
			}
		} else {
			unsigned int new_producer;

			/*
			 * Slow path.  Either the buffer is full or we have a
			 * stale snapshot of p_head.  Execute a second read of
			 * p_read that must be ordered wrt the snapshot of
			 * c_head.
			 */
			ck_pr_fence_load();
			new_producer = ck_pr_load_uint(&ring->p_head);

			/*
			 * Only fail if we haven't made forward progress in
			 * production: the buffer must have been full when we
			 * read new_producer (or we wrapped around UINT_MAX
			 * during this iteration).
			 */
			if (producer == new_producer) {
				r = false;
				goto leave;
			}

			/*
			 * p_head advanced during this iteration. Try again.
			 */
			producer = new_producer;
		}
	}

	buffer = (char *)buffer + ts * (producer & mask);
	memcpy(buffer, entry, ts);

	/*
	 * Wait until all concurrent producers have completed writing
	 * their data into the ring buffer.
	 */
	while (ck_pr_load_uint(&ring->p_tail) != producer)
		ck_pr_stall();

	/*
	 * Ensure that copy is completed before updating shared producer
	 * counter.
	 */
	ck_pr_fence_store();
	ck_pr_store_uint(&ring->p_tail, delta);

leave:
	if (size != NULL)
		*size = (producer - consumer) & mask;

	return r;
}

CK_CC_FORCE_INLINE static bool
_ck_ring_enqueue_mp_size(struct ck_ring *ring,
    void *buffer,
    const void *entry,
    unsigned int ts,
    unsigned int *size)
{
	unsigned int sz;
	bool r;

	r = _ck_ring_enqueue_mp(ring, buffer, entry, ts, &sz);
	*size = sz;
	return r;
}

CK_CC_FORCE_INLINE static bool
_ck_ring_trydequeue_mc(struct ck_ring *ring,
    const void *buffer,
    void *data,
    unsigned int size)
{
	const unsigned int mask = ring->mask;
	unsigned int consumer, producer;

	consumer = ck_pr_load_uint(&ring->c_head);
	ck_pr_fence_load();
	producer = ck_pr_load_uint(&ring->p_tail);

	if (unlikely(consumer == producer))
		return false;

	ck_pr_fence_load();

	buffer = (const char *)buffer + size * (consumer & mask);
	memcpy(data, buffer, size);

	ck_pr_fence_store_atomic();
	return ck_pr_cas_uint(&ring->c_head, consumer, consumer + 1);
}

CK_CC_FORCE_INLINE static bool
_ck_ring_dequeue_mc(struct ck_ring *ring,
    const void *buffer,
    void *data,
    unsigned int ts)
{
	const unsigned int mask = ring->mask;
	unsigned int consumer, producer;

	consumer = ck_pr_load_uint(&ring->c_head);

	do {
		const char *target;

		/*
		 * Producer counter must represent state relative to
		 * our latest consumer snapshot.
		 */
		ck_pr_fence_load();
		producer = ck_pr_load_uint(&ring->p_tail);

		if (unlikely(consumer == producer))
			return false;

		ck_pr_fence_load();

		target = (const char *)buffer + ts * (consumer & mask);
		memcpy(data, target, ts);

		/* Serialize load with respect to head update. */
		ck_pr_fence_store_atomic();
	} while (ck_pr_cas_uint_value(&ring->c_head,
				      consumer,
				      consumer + 1,
				      &consumer) == false);

	return true;
}

/*
 * The ck_ring_*_mpmc namespace is the public interface for interacting with a
 * ring buffer containing pointers. Correctness is provided for any number of
 * producers and consumers.
 */
CK_CC_INLINE static bool
ck_ring_enqueue_mpmc(struct ck_ring *ring,
    struct ck_ring_buffer *buffer,
    const void *entry)
{

	return _ck_ring_enqueue_mp(ring, buffer, &entry,
	    sizeof(entry), NULL);
}

CK_CC_INLINE static bool
ck_ring_enqueue_mpmc_size(struct ck_ring *ring,
    struct ck_ring_buffer *buffer,
    const void *entry,
    unsigned int *size)
{

	return _ck_ring_enqueue_mp_size(ring, buffer, &entry,
	    sizeof(entry), size);
}

CK_CC_INLINE static bool
ck_ring_trydequeue_mpmc(struct ck_ring *ring,
    const struct ck_ring_buffer *buffer,
    void *data)
{

	return _ck_ring_trydequeue_mc(ring,
	    buffer, (void **)data, sizeof(void *));
}

CK_CC_INLINE static bool
ck_ring_dequeue_mpmc(struct ck_ring *ring,
    const struct ck_ring_buffer *buffer,
    void *data)
{

	return _ck_ring_dequeue_mc(ring, buffer, (void **)data,
	    sizeof(void *));
}

#endif /* _WG_MPMC_RING_H */