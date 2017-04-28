/***************************************************************
 *
 * (C) 2011-16 Nicola Bonelli <nicola@pfq.io>
 *             Andrea Di Pietro <andrea.dipietro@for.unipi.it>
 * 	       Loris Gazzarrini <loris.gazzarrini@iet.unipi.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#include <lang/engine.h>
#include <lang/symtable.h>

#include <pfq/GC.h>
#include <pfq/bitops.h>
#include <pfq/core.h>
#include <pfq/devmap.h>
#include <pfq/global.h>
#include <pfq/io.h>
#include <pfq/kcompat.h>
#include <pfq/memory.h>
#include <pfq/percpu.h>
#include <pfq/prefetch.h>
#include <pfq/qbuff.h>
#include <pfq/queue.h>
#include <pfq/sock.h>
#include <pfq/vlan.h>


/* send this packet to selected sockets */

static inline
void mask_to_sock_queue(unsigned long n, unsigned long mask, unsigned long long *sock_queue)
{
	unsigned long int bit;
        pfq_bitwise_foreach(mask, bit,
	{
	        int index = (int)pfq_ctz(bit);
                sock_queue[index] |= 1UL << n;
        })
}

/*
 * Find the next power of two.
 * from "Hacker's Delight, Henry S. Warren."
 */

static inline
unsigned clp2(unsigned int x)
{
        x = x - 1;
        x = x | (x >> 1);
        x = x | (x >> 2);
        x = x | (x >> 4);
        x = x | (x >> 8);
        x = x | (x >> 16);
        return x + 1;
}

/*
 * Optimized folding operation...
 */

static inline
uint32_t prefold(uint32_t hash)
{
	return hash ^ (hash >> 8) ^ (hash >> 16) ^ (hash >> 24);
}


static inline
unsigned int pfq_fold(unsigned int a, unsigned int b)
{
	unsigned int c;
	if (b == 1)
		return 0;
        c = b - 1;
        if (likely((b & c) == 0))
		return a & c;
        switch(b)
        {
        case 3:  return a % 3;
        case 5:  return a % 5;
        case 6:  return a % 6;
        case 7:  return a % 7;
        default: {
                const unsigned int p = clp2(b);
                const unsigned int r = a & (p-1);
                return r < b ? r : a % b;
            }
        }
}


int pfq_process_batch(struct pfq_percpu_data *data,
		       struct pfq_percpu_pool *pool,
		       struct GC_data *GC_ptr,
		       int cpu)
{
	unsigned long long sock_queue[Q_BUFF_BATCH_LEN] = { 0 };
        unsigned long all_group_mask, socket_mask;
	struct pfq_endpoint_info endpoints;
	struct pfq_lang_monad monad;
        long unsigned n, bit, lb;
	size_t current_batch_len;
        struct qbuff *buff;

#if 0
	for_each_qbuff(PFQ_QBUFF_QUEUE(&GC_ptr->pool), buff, n)
	{
		qbuff_free(buff, &pool->rx);
	}

	GC_reset(GC_ptr);
	return 0;
#endif

	PFQ_BUILD_BUG_ON_MSG(Q_BUFF_BATCH_LEN > (sizeof(sock_queue[0]) << 3), "qbuff batch overflow");

	/* setup parameters */

	current_batch_len = GC_size(GC_ptr);
	all_group_mask = 0;

        /* setup the qbuff in GC */

	for_each_qbuff(&GC_ptr->pool, buff, n)
        {
		uint16_t queue = qbuff_get_rx_queue(buff);
		unsigned long group_mask = pfq_devmap_get_groups(qbuff_get_ifindex(buff), queue);

		all_group_mask |= group_mask;
		buff->group_mask = group_mask;
		buff->monad = &monad;
		buff->counter = data->counter++;
	}


	/* update stats */

	__sparse_add(global->percpu_stats, recv, current_batch_len, cpu);


        /* process all groups enabled for the packets */

	pfq_bitwise_foreach(all_group_mask, bit,
	{
		pfq_gid_t gid = (__force pfq_gid_t)pfq_ctz(bit);

		struct pfq_group * this_group = pfq_group_get(gid);

		bool bf_filt_enabled = atomic_long_read(&this_group->bp_filter);
		bool vlan_filt_enabled = pfq_group_vlan_filters_enabled(gid);

		struct pfq_ref_batch refs = { .len = 0 };

		socket_mask = 0;

		for_each_qbuff_upto(current_batch_len, &GC_ptr->pool, buff, n)
		{
			struct pfq_lang_computation_tree *prg;
			unsigned long sock_mask = 0;

			/* skip this packet for this group ? */

			if ((buff->group_mask & bit) == 0) {
				refs.ref[refs.len++] = NULL;
				continue;
			}

			/* increment counter for this group */

			__sparse_inc(this_group->stats, recv, cpu);

			/* check if bp filter is enabled */

			if (bf_filt_enabled) {
				if (!qbuff_run_bp_filter(buff, this_group)) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.ref[refs.len++] = NULL;
					continue;
				}
			}

			/* check vlan filter */

			if (vlan_filt_enabled) {
				if (!qbuff_run_vlan_filter(buff, (pfq_gid_t)gid)) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.ref[refs.len++] = NULL;
					continue;
				}
			}

			/* evaluate the computation of the current group */

			buff->state = 0;

			prg = (struct pfq_lang_computation_tree *)atomic_long_read(&this_group->comp);
			if (prg) {
				unsigned long cbit, eligible_mask = 0;
				size_t to_kernel = buff->log->to_kernel;
				size_t num_fwd = buff->log->num_devs;

				/* setup monad for this computation */

				monad.fanout.class_mask = Q_CLASS_DEFAULT;
				monad.fanout.type = fanout_copy;
				monad.group = this_group;
				monad.state = 0;
				monad.shift = 0;
				monad.ipoff = 0;
				monad.ipproto = IPPROTO_NONE;
				monad.ep_ctx = EPOINT_SRC | EPOINT_DST;

				/* run the functional program */

				buff = pfq_lang_run(buff, prg).qbuff;
				if (buff == NULL) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.ref[refs.len++] = NULL;
					continue;
				}

				/* park the monad state */

				buff->state = monad.state;

				/* update stats */

                                __sparse_add(this_group->stats, frwd, buff->log->num_devs -num_fwd, cpu);
                                __sparse_add(this_group->stats, kern, buff->log->to_kernel - to_kernel, cpu);

				/* skip the packet? */

				if (is_drop(monad.fanout)) {
					__sparse_inc(this_group->stats, drop, cpu);
					refs.ref[refs.len++] = NULL;
					continue;
				}

				/* save a reference to the current packet */

				refs.ref[refs.len++] = buff;

				/* compute the eligible mask of sockets enabled for this packet... */

				pfq_bitwise_foreach(monad.fanout.class_mask, cbit,
				{
					int class = (int)pfq_ctz(cbit);
					eligible_mask |= (unsigned long)atomic_long_read(&this_group->sock_id[class]);
				})

				/* logical dependency: when sock_masks of a
				 * given group is modified, it is necessary to
				 * invalidate the per-cpu sock->eligible_mask cache */

				if (is_steering(monad.fanout)) { /* single or double */

					/* cache the number of sockets in the mask */

					if (eligible_mask != data->sock_eligible_mask) {
						unsigned long ebit;
						data->sock_eligible_mask = eligible_mask;
						data->sock_cnt = 0;
						pfq_bitwise_foreach(eligible_mask, ebit,
						{
							pfq_id_t id = (__force pfq_id_t)pfq_ctz(ebit);
							struct pfq_sock * so = pfq_sock_get_by_id(id);
                                                        int i;

							/* max weight = Q_MAX_SOCK_MASK / Q_MAX_ID */

							for(i = 0; i < so->weight; ++i)
								data->sock_mask[data->sock_cnt++] = ebit;
						})
					}

					if (likely(data->sock_cnt)) {

						sock_mask |= data->sock_mask[pfq_fold(prefold(monad.fanout.hash), (unsigned int)data->sock_cnt)];

						if (is_double_steering(monad.fanout))
							sock_mask |= data->sock_mask[pfq_fold(prefold(monad.fanout.hash2), (unsigned int)data->sock_cnt)];
					}
				}
				else {  /* broadcast */

					sock_mask |= eligible_mask;
				}
			}
			else {
				/* save a reference to the current packet */
				refs.ref[refs.len++] = buff;
				sock_mask |= (unsigned long)atomic_long_read(&this_group->sock_id[0]);
			}

			mask_to_sock_queue(n, sock_mask, sock_queue);
			socket_mask |= sock_mask;
		}

		/* copy payloads to endpoints... */

		pfq_bitwise_foreach(socket_mask, lb,
		{
			pfq_id_t id = (__force pfq_id_t)pfq_ctz(lb);
			struct pfq_sock * so = pfq_sock_get_by_id(id);
			pfq_copy_to_endpoint_qbuffs(so, PFQ_QBUFF_REFS(&refs), sock_queue[(int __force)id], cpu, gid);
		})
	});

	/* forward buffs to network devices */

	GC_get_lazy_endpoints(GC_ptr, &endpoints);
	if (endpoints.cnt_total)
	{
		size_t total = (size_t)pfq_qbuff_lazy_xmit_run(PFQ_QBUFF_QUEUE(&GC_ptr->pool), &endpoints);
		__sparse_add(global->percpu_stats, frwd, total, cpu);
		__sparse_add(global->percpu_stats, disc, endpoints.cnt_total - total, cpu);
	}


	/* forward buffs to kernel and release them... */

	for_each_qbuff(PFQ_QBUFF_QUEUE(&GC_ptr->pool), buff, n)
	{
		if (fwd_to_kernel(buff)) {

			bool peeked = QBUFF_SKB(buff)->peeked;

			qbuff_move_or_copy_to_kernel(buff, GFP_KERNEL);

			/* only if peeked we need to free/recycle the qbuff/skb */
			if (peeked)
				qbuff_free(buff, &pool->rx);

			__sparse_inc(global->percpu_stats, kern, cpu);
		}
		else {
			/* Peeked or not, always free the qbuff/skb here */
			qbuff_free(buff, &pool->rx);
		}
	}

	/* reset the GC */

	GC_reset(GC_ptr);
        return 0;
}
