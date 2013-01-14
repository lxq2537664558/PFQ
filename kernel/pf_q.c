/***************************************************************
 *                                                
 * (C) 2011-12 Nicola Bonelli <nicola.bonelli@cnit.it>   
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

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/semaphore.h>
#include <linux/socket.h>  
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>
#include <linux/ip.h>
#include <linux/poll.h>
#include <linux/etherdevice.h>

#include <linux/percpu.h>

#include <net/sock.h>
#ifdef CONFIG_INET
#include <net/inet_common.h>
#endif

#include <linux/pf_q.h>

#include <pf_q-common.h>
#include <pf_q-devmap.h>
#include <pf_q-group.h>
#include <pf_q-queue.h>
#include <pf_q-steer.h>
#include <pf_q-bits.h>
#include <pf_q-vlan.h>
#include <pf_q-bpf.h>

#include <pf_q-mpdb-queue.h>

struct net_proto_family  pfq_family_ops;
struct packet_type       pfq_prot_hook;
struct proto             pfq_proto;
struct proto_ops         pfq_ops; 

static int direct_capture = 0;

static int sniff_incoming = 1;
static int sniff_outgoing = 0;
static int sniff_loopback = 0;

static int queue_slots  = 131072; // slots per queue
static int cap_len      = 1514;
static int prefetch_len = 1;
static int flow_control = 0;

MODULE_LICENSE("GPL");

MODULE_AUTHOR("Nicola Bonelli <nicola.bonelli@cnit.it>");

MODULE_DESCRIPTION("Packet capture system for 64bit multi-core architectures");

module_param(direct_capture,  int, 0644);
module_param(sniff_incoming,  int, 0644);
module_param(sniff_outgoing,  int, 0644);
module_param(sniff_loopback,  int, 0644);


module_param(cap_len,         int, 0644);
module_param(queue_slots,     int, 0644);
module_param(prefetch_len,    int, 0644);
module_param(flow_control,    int, 0644);

MODULE_PARM_DESC(direct_capture," Direct capture packets: (0 default)");
MODULE_PARM_DESC(sniff_incoming," Sniff incoming packets: (1 default)");
MODULE_PARM_DESC(sniff_outgoing," Sniff outgoing packets: (0 default)");
MODULE_PARM_DESC(sniff_loopback," Sniff lookback packets: (0 default)");

MODULE_PARM_DESC(cap_len,       " Default capture length (bytes)");
MODULE_PARM_DESC(queue_slots,   " Queue slots (default=131072)");
MODULE_PARM_DESC(prefetch_len,  " Prefetch queue length");
MODULE_PARM_DESC(flow_control,  " Flow control value (default=0)");

/* vector of pointers to pfq_opt */

atomic_long_t pfq_vector[Q_MAX_ID]; 

/* timestamp toggle */

atomic_t timestamp_toggle;

/* per-cpu data... */

struct local_data 
{
  	unsigned long           eligible_mask;
	unsigned long           sock_mask [Q_MAX_ID];
        int                     sock_cnt;
        int 			flowctrl;
        struct pfq_queue_skb    prefetch_queue;
};

struct local_data __percpu    * cpu_data;


/* uhm okay, this is a legit form of static polymorphism */

static inline struct pfq_sock *
pfq_sk(struct sock *sk)
{
        return (struct pfq_sock *)(sk);
}


inline 
int pfq_get_free_id(struct pfq_opt * pq)
{
        int n = 0;
        for(; n < Q_MAX_ID; n++)
        {            
                if (!atomic_long_cmpxchg(pfq_vector + n, 0, (long)pq))
                        return n;         
        }
        return -1;
}


inline 
struct pfq_opt * 
pfq_get_opt(size_t id)
{
        struct pfq_opt * opt;
        if (unlikely(id >= Q_MAX_ID))
        {
                pr_devel("[PFQ] pfq_devmap_freeid: bad id=%zd!\n", id);
                return NULL;
        }
	opt = (struct pfq_opt *)atomic_long_read(&pfq_vector[id]);  
	smp_read_barrier_depends();
	return opt;  
}


inline 
void pfq_release_id(int id)
{
        if (unlikely(id >= Q_MAX_ID || id < 0))
        {
                pr_devel("[PFQ] pfq_devmap_freeid: bad id=%d!\n", id);
                return;
        }
        atomic_long_set(pfq_vector + id, 0);
}


inline
bool pfq_copy_to_user_skbs(struct pfq_opt *pq, int cpu, unsigned long batch_queue, struct pfq_queue_skb *skbs)
{
        /* enqueue the sk_buff: it's wait-free. */

        int len = 0; size_t sent = 0;

        if (likely(pq->q_active)) 
        {
        	smp_rmb();

                len  = (int)hweight64(batch_queue); 
                sent = mpdb_enqueue_batch(pq, batch_queue, len, skbs);
        	
        	__sparse_add(&pq->q_stat.recv, cpu, sent);
        
		if (len > sent) 
		{
			__sparse_add(&pq->q_stat.lost, cpu, len - sent);
			return false; 
		}
        }
        return true;
}


#if 0

struct pfq_steering_cache
{
	steering_function_t fun;
	void * state;
	steering_t ret;
};


inline steering_t
pfq_memoized_call(struct pfq_steering_cache *mem, steering_function_t fun, 
		  const struct sk_buff *skb, void *state)
{
	if (unlikely(mem->fun != fun || mem->state != state)) {
		mem->state = state;
		mem->fun = fun;
		mem->ret = fun(skb, state);
	} 
	return mem->ret; 
}

#endif

/* send this packet to selected sockets */

inline
void pfq_enqueue_mask_to_batch(unsigned long j, unsigned long mask, unsigned long *batch_queue)
{
	unsigned long bit;
       	bitwise_foreach(mask, bit)
	{
	        int index = pfq_ctz(bit);
                batch_queue[index] |= 1UL << j;
        }
}


void
pfq_dump_skb(struct sk_buff const *skb)
{
        unsigned char * p;

        printk(KERN_INFO "[PFQ] skb type:%d mac_len:%d proto:%x %pM -> %pM tci:%d %pI4 -> %pI4\n", 
                skb->pkt_type,
                skb->mac_len,
                htons(eth_hdr(skb)->h_proto),
                eth_hdr(skb)->h_source,
                eth_hdr(skb)->h_dest,
                skb->vlan_tci & VLAN_VID_MASK,
                &ip_hdr(skb)->saddr,
                &ip_hdr(skb)->daddr
               );

        p = (unsigned char *)eth_hdr(skb);

        printk(KERN_INFO "[PFQ] %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x\n",
                       p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
                       p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23], p[24], p[25], p[26], p[27], p[28], p[29]);
}
 
/*
 * Find the next power of two.
 * from "Hacker's Delight, Henry S. Warren."
 */

inline 
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
 * Optimized folding operation..
 */

inline
unsigned int pfq_fold(unsigned int a, unsigned int b)
{
        const unsigned int c = b - 1;
        if (b & c)
        {
                switch(b)
                {
                case 3:  return a % 3;
                case 5:  return a % 5;
                case 6:  return a % 6;
                case 9:  return a % 9;
                case 10: return a % 10;
                case 11: return a % 11;
                case 12: return a % 12;
                case 13: return a % 13;
                case 17: return a % 17;
                case 18: return a % 18;
                case 19: return a % 19;
                case 20: return a % 20;
                default: {
                        const unsigned int p = clp2(b);
                        const unsigned int r = a & (p-1);
                        return likely(r < b) ? r : a % b;
                    }
                }
        }
        else
        {
                return a & c;
        }
}        


struct pfq_skb_cb
{
	unsigned long group_mask;
        
	bool direct_skb;
        bool stolen_skb;
        bool send_to_kernel;
};


int 
pfq_receive(struct sk_buff *skb, bool direct)
{       
        struct local_data * local_cache = this_cpu_ptr(cpu_data);
        struct pfq_queue_skb * prefetch_queue = &local_cache->prefetch_queue;
        unsigned long group_mask, global_mask;
        unsigned long batch_queue[sizeof(unsigned long) << 3];
        struct pfq_skb_cb *cb; 
        long unsigned n, bit;

#ifdef PFQ_USE_FLOW_CONTROL
	
	/* flow control */
	
	if (local_cache->flowctrl && 
	    local_cache->flowctrl--) 
	{
                if (direct)
                        __kfree_skb(skb);
                else
                        kfree_skb(skb);
		
		return 0;
	}
#endif


#ifdef PFQ_USE_VLAN_UNTAGGING
#pragma message "[PFQ] using vlan untagging"
        
        /* if vlan header is present, remove it */
        if (skb->protocol == cpu_to_be16(ETH_P_8021Q)) {
                skb = pfq_vlan_untag(skb);
                if (unlikely(!skb))
                        return -1;
        }
#endif
        
        /* reset mac len */

        skb_reset_mac_len(skb);
        
        /* push the mac header: reset skb->data to the beginning of the packet */

        if (likely(skb->pkt_type != PACKET_OUTGOING))
        {
            skb_push(skb, skb->mac_len);
        }

	/* if required, timestamp this packet now */

        if (atomic_read(&timestamp_toggle) && skb->tstamp.tv64 == 0) {
                __net_timestamp(skb);
        }

	/* enqueue the packet to the prefetch queue */
                
        cb = (struct pfq_skb_cb *) skb->cb;

        cb->direct_skb      = direct;
        cb->stolen_skb      = false;
        cb->send_to_kernel  = false;

        /* enqueue this skb ... */

        pfq_queue_skb_push(prefetch_queue, skb);

        if (pfq_queue_skb_size(prefetch_queue) < prefetch_len) {
                return 0;
	}

	/* initialize data */
	
        memset(batch_queue, 0, sizeof(batch_queue));

        global_mask = 0;

	const int cpu = get_cpu();

#ifdef PFQ_STEERING_PROFILE
	cycles_t a = get_cycles();
#endif

#ifdef PFQ_STEERING_ENGINE_V1
#pragma message "[PFQ] using steering engine v1"

	/* for each packet in the prefetch queue */
        
	queue_for_each(skb, n, prefetch_queue)
        {
		/* reset steering function in cache */

                /* get the balancing groups bitmap */

		group_mask = __pfq_devmap_get_groups(skb->dev->ifindex, skb_get_rx_queue(skb));   

                unsigned long sock_mask = 0;
		
		/* prefetch the cpu value */

		// __builtin_prefetch(&cpu, 0, 2);

                /* for each group in this mask ... */
			

                bitwise_foreach(group_mask, bit)
                {
                        
			int gindex = pfq_ctz(bit);
                        struct sk_filter *bpf;

                        steering_t ret;
                        steering_function_t steer_fun;

                        /* increment recv counter for this group */

                        __sparse_inc(&pfq_groups[gindex].recv, cpu);
	

                        /* check bpf filter */

                        if ((bpf = (struct sk_filter *)atomic_long_read(&pfq_groups[gindex].filter)))
                        {
                                if (!sk_run_filter(skb, bpf->insns))
                                        continue;
                        }
                        
                        /* check vlan filter */

                        if (__pfq_vlan_filters_enabled(gindex))
                        {
                                if (!__pfq_check_group_vlan_filter(gindex, skb->vlan_tci & ~VLAN_TAG_PRESENT))
                                        continue;
                        }

                        /* retrieve the steering function for this group */
                        
                        steer_fun = (steering_function_t) atomic_long_read(&pfq_groups[gindex].steering);
                        
			if (steer_fun) 
                        {
                                /* call the steering function */

                                ret = steer_fun(skb, (void *)atomic_long_read(&pfq_groups[gindex].state));

                                if (ret.type & action_steal)
                                {
                                        cb = (struct pfq_skb_cb *) skb->cb;
                                        cb->stolen_skb = true;
                                        goto next_skb;
                                }

                                if (ret.type & action_pass)
                                {
                                        cb = (struct pfq_skb_cb *) skb->cb;
                                        cb->send_to_kernel = true;
                                }

                                if (likely((ret.type & action_drop) == 0)) 
                                {
                                        unsigned long eligible_mask = 0;
                                        unsigned long cbit;

                                        bitwise_foreach(ret.class, cbit)
                                        {
                                                int cindex = pfq_ctz(cbit);
                                                eligible_mask |= atomic_long_read(&pfq_groups[gindex].sock_mask[cindex]);
                                        }

                                        if (unlikely(ret.type & action_clone)) {

                                                sock_mask |= eligible_mask;
                                                continue; 
                                        }

                                        if (unlikely(eligible_mask != local_cache->eligible_mask)) {

                                                unsigned long ebit;

                                                local_cache->eligible_mask = eligible_mask;
                                                local_cache->sock_cnt = 0;
                                                
                                                bitwise_foreach(eligible_mask, ebit) 
                                                {
                                                        local_cache->sock_mask[local_cache->sock_cnt++] = ebit;
                                                }
                                        }

                                        if (likely(local_cache->sock_cnt))
                                        {
		        			unsigned int h = ret.hash ^ (ret.hash >> 8) ^ (ret.hash >> 16);
		        			sock_mask |= local_cache->sock_mask[pfq_fold(h, local_cache->sock_cnt)];
		        		}
                                }
                        }
                        else 
                        {
                                sock_mask |= atomic_long_read(&pfq_groups[gindex].sock_mask[0]);
                        }
                }
                

                pfq_enqueue_mask_to_batch(n, sock_mask, batch_queue);
                
		global_mask |= sock_mask;
        
        next_skb: ;
        
        }

#elif PFQ_STEERING_ENGINE_V2
#pragma message "[PFQ] using steering engine v2"

	unsigned long global_group_mask = 0;
        queue_for_each(skb, n, prefetch_queue)
        {
                struct pfq_skb_cb *cb = (struct pfq_skb_cb *)skb->cb;
		unsigned long group_mask = __pfq_devmap_get_groups(skb->dev->ifindex, skb_get_rx_queue(skb));  
		
		global_group_mask |= group_mask;

		cb->group_mask = group_mask;
	}


        bitwise_foreach(global_group_mask, bit)
        {
		int gindex = pfq_ctz(bit);
                
                struct sk_filter *bpf = (struct sk_filter *)atomic_long_read(&pfq_groups[gindex].filter);

                steering_function_t steer_fun = (steering_function_t) atomic_long_read(&pfq_groups[gindex].steering);

                bool vlan_filter_enabled = __pfq_vlan_filters_enabled(gindex);

        	queue_for_each(skb, n, prefetch_queue)
		{
                	struct pfq_skb_cb *cb = (struct pfq_skb_cb *)skb->cb;
                	
			unsigned long sock_mask = 0;
                        
			steering_t ret;

			if ((cb->group_mask & bit) == 0)
                         	continue;

                        /* increment recv counter for this group */

                        __sparse_inc(&pfq_groups[gindex].recv, cpu);
	

                        /* check bpf filter */

                        if (bpf)
                        {
                                if (!sk_run_filter(skb, bpf->insns))
                                        continue;
                        }
                        
                        /* check vlan filter */

                        if (vlan_filter_enabled)
                        {
                                if (!__pfq_check_group_vlan_filter(gindex, skb->vlan_tci & ~VLAN_TAG_PRESENT))
                                        continue;
                        }

                        /* retrieve the steering function for this group */
                        
			if (steer_fun) 
                        {
                                /* call the steering function */

                                ret = steer_fun(skb, (void *)atomic_long_read(&pfq_groups[gindex].state));

                                if (ret.type & action_steal)
                                {
                                        cb->stolen_skb = true;
                                        continue;
                                }

                                if (ret.type & action_pass)
                                {
                                        cb->send_to_kernel = true;
                                }

                                if (likely((ret.type & action_drop) == 0)) 
                                {
                                        unsigned long eligible_mask = 0;
                                        unsigned long cbit;

                                        bitwise_foreach(ret.class, cbit)
                                        {
                                                int cindex = pfq_ctz(cbit);
                                                eligible_mask |= atomic_long_read(&pfq_groups[gindex].sock_mask[cindex]);
                                        }

                                        if (unlikely(ret.type & action_clone)) {

                                                sock_mask |= eligible_mask;
                                                continue; 
                                        }

                                        if (unlikely(eligible_mask != local_cache->eligible_mask)) {

                                                unsigned long ebit;

                                                local_cache->eligible_mask = eligible_mask;
                                                local_cache->sock_cnt = 0;
                                                
                                                bitwise_foreach(eligible_mask, ebit) 
                                                {
                                                        local_cache->sock_mask[local_cache->sock_cnt++] = ebit;
                                                }
                                        }

                                        if (likely(local_cache->sock_cnt))
                                        {
		        			unsigned int h = ret.hash ^ (ret.hash >> 8) ^ (ret.hash >> 16);
		        			sock_mask |= local_cache->sock_mask[pfq_fold(h, local_cache->sock_cnt)];
		        		}
                                }
                        }
                        else 
                        {
                                sock_mask |= atomic_long_read(&pfq_groups[gindex].sock_mask[0]);
                        }

			
			pfq_enqueue_mask_to_batch(n, sock_mask, batch_queue);
			global_mask |= sock_mask;

		}
                
	}
#else
#error PFQ_STEERING_ENGINE_Vx not defined!
#endif

#ifdef PFQ_STEERING_PROFILE
	cycles_t b = get_cycles();
			
	if (printk_ratelimit())
		printk(KERN_INFO "-> %llu\n", (b-a)/prefetch_len);
#endif

	/* copy packets to pfq sockets... */

	bitwise_foreach(global_mask, bit)
        {
                int n = pfq_ctz(bit);
                struct pfq_opt * pq = pfq_get_opt(n);
                if (likely(pq)) 
                {
#ifdef PFQ_USE_FLOW_CONTROL
                        if (!pfq_copy_to_user_skbs(pq, cpu, batch_queue[n], prefetch_queue))
                        	local_cache->flowctrl = flow_control;
#else
			pfq_copy_to_user_skbs(pq, cpu, batch_queue[n], prefetch_queue);
#endif
                }
        }

        /* free skb, or route them to kernel... */
        
        queue_for_each(skb, n, prefetch_queue)
        {
                cb = (struct pfq_skb_cb *)skb->cb;
        
                if (unlikely(cb->stolen_skb))
                        continue;

                if (likely(cb->direct_skb))
		{
		        if (unlikely(!sniff_incoming && cb->send_to_kernel))
		                netif_receive_skb(skb);
                        else
                                __kfree_skb(skb);
		}
                else
                {
                        /* to avoid loops, sniffed packets are not passed back to kernel */
                        kfree_skb(skb);
                }
        }       
	
        pfq_queue_skb_flush(prefetch_queue);
        return 0;
}


/* simple packet HANDLER */       

int 
pfq_packet_rcv
(
    struct sk_buff *skb, struct net_device *dev,
    struct packet_type *pt
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16))
    ,struct net_device *orig_dev
#endif
    )
{
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (unlikely(!skb))
		return 0;

        switch(skb->pkt_type)
        {
            case PACKET_OUTGOING: {
                if (!sniff_outgoing)
                        return 0;
                skb->mac_len = ETH_HLEN;
            } break;
            
            case PACKET_LOOPBACK: {
                if (!sniff_loopback)
                        return 0;
            } break;

            default: /* PACKET_INCOMING */
                if (!sniff_incoming)
                        return 0;
        }

        return pfq_receive(skb, false);
}


static int 
pfq_ctor(struct pfq_opt *pq)
{
        /* set to 0 by default */
        memset(pq, 0, sizeof(struct pfq_opt));

        /* get a unique id for this queue */
        pq->q_id = pfq_get_free_id(pq);
        if (pq->q_id == -1)
        {
                printk(KERN_WARNING "[PFQ] no queue available!\n");
                return -EBUSY;
        }
        
        /* disable tiemstamping by default */
        pq->q_tstamp = false;

        /* queue is alloc when the socket is enabled */
        pq->q_addr = NULL; 
        pq->q_queue_mem = 0;
        
        /* set q_slots and q_caplen default values */
        
        pq->q_caplen    = cap_len;
        pq->q_offset    = 0;
        pq->q_slot_size = DBMP_QUEUE_SLOT_SIZE(cap_len);
        pq->q_slots     = queue_slots;

        /* disabled by default */
        pq->q_active = false;
        
        /* initialize waitqueue */
        init_waitqueue_head(&pq->q_waitqueue);
        
        /* reset stats */
        sparse_set(&pq->q_stat.recv, 0);
        sparse_set(&pq->q_stat.lost, 0);
        sparse_set(&pq->q_stat.drop, 0);

        return 0;
}


static void 
pfq_dtor(struct pfq_opt *pq)
{
        pfq_release_id(pq->q_id); 

        mpdb_queue_free(pq);
}


static int 
pfq_create(
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24))
    struct net *net,
#endif
    struct socket *sock, int protocol
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33))
    , int kern
#endif
    )
{
        struct pfq_opt *pq;
        struct sock *sk;
        struct pfq_sock *psk;
        int err = -ENOMEM;

        /* security and sanity check */
        if (!capable(CAP_NET_ADMIN))
                return -EPERM;
        if (sock->type != SOCK_RAW)
                return -ESOCKTNOSUPPORT;
        if (protocol != __constant_htons(ETH_P_ALL))
                return -EPROTONOSUPPORT;

#if(LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
        sk = sk_alloc(PF_Q, GFP_KERNEL, 1, NULL);
#else
#if(LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24))
        // BD: API changed in 2.6.12, ref:
        // http://svn.clkao.org/svnweb/linux/revision/?rev=28201
        sk = sk_alloc(PF_Q, GFP_ATOMIC, &q_proto, 1);
#else
        sk = sk_alloc(net, PF_INET, GFP_KERNEL, &pfq_proto);
#endif
#endif
        if (sk == NULL)
                goto out;

        sock->ops = &pfq_ops;
        sock_init_data(sock,sk);

#if(LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,11))
        sk_set_owner(sk, THIS_MODULE);
#endif
        /* alloc memory for this pq */

        pq = (struct pfq_opt *)kmalloc(sizeof(struct pfq_opt), GFP_KERNEL);
        if (!pq) 
        {
                err = -ENOMEM;
                goto pq_err;
        }   

        /* construct pfq_opt */
        if (pfq_ctor(pq) != 0)
        {
                err = -ENOMEM;
                goto ctor_err;
        }

	smp_wmb();

        /* store the pq */
        psk = pfq_sk(sk);
	psk->opt = pq;
        return 0;

ctor_err:    
        kfree(pq);
pq_err:    
        sk_free(sk);
out:    
        return err;
}   


static int 
pfq_release(struct socket *sock)
{
        struct sock * sk = sock->sk;
        struct pfq_opt * pq;
	int id = -1;

	if (!sk)
		return 0;
	
	pq = pfq_sk(sk)->opt;
	if (pq) {

               	id = pq->q_id;
		
		/* decrease the timestamp_toggle counter */
		if (pq->q_tstamp) {
			atomic_dec(&timestamp_toggle);
			pr_devel("[PFQ|%d] timestamp_toggle => %d\n", pq->q_id, atomic_read(&timestamp_toggle));
		}

		pq->q_active = false;

        	pfq_leave_all_groups(pq->q_id);
                
                msleep(GRACE_PERIOD);

		pfq_dtor(pq);

		/* Convenient way to avoid a race condition,
		 * without using expensive rw-mutexes 
		 */

                msleep(GRACE_PERIOD);

		kfree(pq);
	}
        
        sock_orphan(sk);
	sock->sk = NULL;
	sock_put(sk);
        
	pr_devel("[PFQ|%d] socket closed.\n", id);
	
        return 0;
}


static
int pfq_getsockopt(struct socket *sock,
                   int level, int optname,
                   char __user * optval, int __user * optlen)
{
        int len;
        struct pfq_opt *pq = pfq_sk(sock->sk)->opt;

        if (pq == NULL)
                return -EFAULT;
        
	if (get_user(len, optlen))
                return -EFAULT;
        
	if (len < 0)
		return -EINVAL;

        switch(optname)
        {

        case Q_SO_GROUP_JOIN: 
            {
                    struct pfq_group_join group;

                    if (len != sizeof(group)) 
                            return -EINVAL;
                    
		    if (copy_from_user(&group, optval, len)) 
                            return -EFAULT;

		    if (group.gid < Q_ANY_GROUP || group.gid >= Q_MAX_GROUP) {
			    pr_devel("[PFQ|%d] join error: bad gid:%d!\n", pq->q_id, group.gid);
			    return -EINVAL;
		    }

		    if (group.class_mask == 0) {
			    pr_devel("[PFQ|%d] join error: bad class_mask(%x)!\n", pq->q_id, group.class_mask);
			    return -EINVAL;
		    }

                    if (group.gid == Q_ANY_GROUP) {

                            group.gid = pfq_join_free_group(pq->q_id, group.class_mask, group.policy);
                            if (group.gid < 0)
                                    return -EFAULT;
                            if (copy_to_user(optval, &group, len))
                                    return -EFAULT;
                    } 
                    else {
			    if (pfq_join_group(group.gid, pq->q_id, group.class_mask, group.policy) < 0) {
				    pr_devel("[PFQ|%d] join error: gid:%d no permission!\n", pq->q_id, group.gid);
				    return -EPERM;
			    }
		    }                    
		    pr_devel("[PFQ|%d] join -> gid:%d class_mask:%x\n", pq->q_id, group.gid, group.class_mask);
            } break;

        case Q_SO_GET_ID: 
            {
                    if (len != sizeof(pq->q_id))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_id, sizeof(pq->q_id)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_STATUS: 
            {
                    if (len != sizeof(pq->q_active)) 
                            return -EINVAL;
                    
                    if (copy_to_user(optval, &pq->q_active, sizeof(pq->q_active))) 
                            return -EFAULT;
                    
            } break;

        case Q_SO_GET_STATS: 
            {
                    struct pfq_stats stat;
                    if (len != sizeof(struct pfq_stats))
                            return -EINVAL;

                    stat.recv = sparse_read(&pq->q_stat.recv);
                    stat.lost = sparse_read(&pq->q_stat.lost);
                    stat.drop = sparse_read(&pq->q_stat.drop);

                    if (copy_to_user(optval, &stat, sizeof(stat)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_TSTAMP: 
            {
                    if (len != sizeof(pq->q_tstamp))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_tstamp, sizeof(pq->q_tstamp)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_QUEUE_MEM: 
            {
                    if (len != sizeof(pq->q_queue_mem))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_queue_mem, sizeof(pq->q_queue_mem)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_CAPLEN: 
            {
                    if (len != sizeof(pq->q_caplen))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_caplen, sizeof(pq->q_caplen)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_SLOTS: 
            {
                    if (len != sizeof(pq->q_slots))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_slots, sizeof(pq->q_slots)))
                            return -EFAULT;
            } break;
        
        case Q_SO_GET_OFFSET: 
            {
                    if (len != sizeof(pq->q_offset))
                            return -EINVAL;
                    if (copy_to_user(optval, &pq->q_offset, sizeof(pq->q_offset)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_GROUPS:
            {
                    unsigned long grps;
                    if(len != sizeof(unsigned long))
                            return -EINVAL;
                    grps = pfq_get_groups(pq->q_id);
                    if (copy_to_user(optval, &grps, sizeof(grps)))
                            return -EFAULT;
            } break;

        case Q_SO_GET_GROUP_STATS: 
            {
                    struct pfq_stats stat;
                    int gid;

		    if (len != sizeof(stat)) 
                            return -EINVAL;

                    if (copy_from_user(&stat, optval, len)) 
                            return -EFAULT;
                    
		    gid = (int)stat.recv;
                    if (gid < 0  || gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] group stats error: gid:%d invalid argument!\n", pq->q_id, gid);
			    return -EINVAL;
		    }

		    /* check whether the group is joinable.. */
		    if (!__pfq_group_access(gid, pq->q_id, Q_GROUP_UNDEFINED, false)) {   
                    	    pr_devel("[PFQ|%d] group stats error: gid:%d access denied!\n", pq->q_id, gid);
			    return -EPERM;
		    }

                    stat.recv = sparse_read(&pfq_groups[gid].recv);
                    stat.lost = sparse_read(&pfq_groups[gid].lost);
                    stat.drop = sparse_read(&pfq_groups[gid].drop);

                    if (copy_to_user(optval, &stat, sizeof(stat)))
                            return -EFAULT;
            } break;

        default:
            return -EFAULT;
        }

        return 0;
}


static
int pfq_setsockopt(struct socket *sock,
                   int level, int optname,
                   char __user * optval,
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,31))
                   unsigned
#endif
                   int optlen)
{
        struct pfq_opt *pq = pfq_sk(sock->sk)->opt;
        bool found = true;

        if (pq == NULL)
                return -EINVAL;
	
        switch(optname)
        {
        case Q_SO_TOGGLE_QUEUE: 
            {
                    int active;
                    if (optlen != sizeof(active))
                            return -EINVAL;
                    if (copy_from_user(&active, optval, optlen))
                            return -EFAULT;

                    if (active)
                    {
                            if (!pq->q_addr)
                            {
                                    struct pfq_queue_descr *sq;

                                    /* alloc queue memory */
                                    pq->q_addr = mpdb_queue_alloc(pq, mpdb_queue_tot_mem(pq), &pq->q_queue_mem);
                                    if (pq->q_addr == NULL) {
                                            return -ENOMEM;
                                    }
                                    sq = (struct pfq_queue_descr *)pq->q_addr;
                                    sq->data      = (1L << 24);
                                    sq->poll_wait = 0;
                                    
				    smp_wmb();
                                    
                                    pq->q_active = true;
                            }
                    }
                    else {
                        pq->q_active = false;
			
                        msleep(GRACE_PERIOD);
                        
                        mpdb_queue_free(pq);
                    }

            } break;

        case Q_SO_ADD_BINDING: 
            {
                    struct pfq_binding bind;
                    if (optlen != sizeof(struct pfq_binding)) 
                            return -EINVAL;
                    
                    if (copy_from_user(&bind, optval, optlen))
                            return -EFAULT;
                    
                    if (bind.gid < 0 || bind.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] add binding error: gid:%d invalid group!\n", pq->q_id, bind.gid);
			    return -EINVAL; 
		    }

		    if (!__pfq_has_joined_group(bind.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] add binding error: gid:%d no permission!\n", pq->q_id, bind.gid);
			    return -EPERM;
		    }

                    pfq_devmap_update(map_set, bind.if_index, bind.hw_queue, bind.gid);
            } break;

        case Q_SO_REMOVE_BINDING: 
            {
                    struct pfq_binding bind;
                    if (optlen != sizeof(struct pfq_binding))
                            return -EINVAL;
                    
		    if (copy_from_user(&bind, optval, optlen))
                            return -EFAULT;

                    if (bind.gid < 0 || bind.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] remove binding error: gid:%d invalid group!\n", pq->q_id, bind.gid);
			    return -EINVAL;
		    }

		    if (!__pfq_has_joined_group(bind.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] remove binding error: gid:%d no permission!\n", pq->q_id, bind.gid);
			    return -EPERM;
		    }

                    pfq_devmap_update(map_reset, bind.if_index, bind.hw_queue, bind.gid);
            } break;

	case Q_SO_SET_TSTAMP: 
            {
                    int tstamp;
                    if (optlen != sizeof(pq->q_tstamp))
                            return -EINVAL;
                    if (copy_from_user(&tstamp, optval, optlen))
                            return -EFAULT;
                    if (tstamp != 0 && tstamp != 1)
                            return -EINVAL;

                    /* update the timestamp_toggle counter */
                    atomic_add(tstamp - pq->q_tstamp, &timestamp_toggle);
                    pq->q_tstamp = tstamp;
                    pr_devel("[PFQ|%d] timestamp_toggle => %d\n", pq->q_id, atomic_read(&timestamp_toggle));
            } break;
        
        case Q_SO_SET_CAPLEN: 
            {
                    if (optlen != sizeof(pq->q_caplen)) 
                            return -EINVAL;
                    if (copy_from_user(&pq->q_caplen, optval, optlen)) 
                            return -EFAULT;
                    pq->q_slot_size = DBMP_QUEUE_SLOT_SIZE(pq->q_caplen);
                    pr_devel("[PFQ|%d] caplen:%lu -> slot_size:%lu\n", 
                                    pq->q_id, pq->q_caplen, pq->q_slot_size);
            } break;

        case Q_SO_SET_SLOTS: 
            {
                    if (optlen != sizeof(pq->q_slots)) 
                            return -EINVAL;
                    if (copy_from_user(&pq->q_slots, optval, optlen)) 
                            return -EFAULT;
                    pr_devel("[PFQ|%d] queue_slots:%lu -> slot_size:%lu\n", 
                                    pq->q_id, pq->q_slots, pq->q_slot_size);
            } break;

        case Q_SO_SET_OFFSET: 
            {
                    if (optlen != sizeof(pq->q_offset)) 
                            return -EINVAL;
                    if (copy_from_user(&pq->q_offset, optval, optlen)) 
                            return -EFAULT;
                    pr_devel("[PFQ|%d] offset:%lu\n", pq->q_id, pq->q_offset);
            } break;

        case Q_SO_GROUP_LEAVE: 
            {
                    int gid;
                    if (optlen != sizeof(gid)) 
                            return -EINVAL;
                    
                    if (copy_from_user(&gid, optval, optlen)) 
                            return -EFAULT;
                    
                    if (pfq_leave_group(gid, pq->q_id) < 0) {
                            return -EFAULT;
                    }
                    
                    pr_devel("[PFQ|%d] leave: gid:%d\n", pq->q_id, gid);
            } break;

	case Q_SO_GROUP_STATE:
	    {
		    struct pfq_group_state s;
		    if (optlen != sizeof(s))
			    return -EINVAL;
		    
		    if (copy_from_user(&s, optval, optlen)) 
			    return -EFAULT;

                    if (s.gid < 0 || s.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] steering error: gid:%d invalid group!\n", pq->q_id, s.gid);
			    return -EINVAL;
		    }

		    if (!__pfq_has_joined_group(s.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] steering error: gid:%d no permission!\n", pq->q_id, s.gid);
			    return -EPERM;
		    }

		    if (s.size && s.state) 
		    {
                    	void * state = kmalloc(s.size, GFP_KERNEL);
			if (state == NULL) 
				return -ENOMEM;

			if(copy_from_user(state, s.state, s.size)) {
                         	kfree(state);
				return -EFAULT;
			}
			
			__pfq_set_group_state(s.gid, state);
                    	    
			pr_devel("[PFQ|%d] state: gid:%d (state of %zu bytes set)\n", pq->q_id, s.gid, s.size);
		    }
		    else { /* empty state */
			   
			__pfq_set_group_state(s.gid, NULL);
			
			pr_devel("[PFQ|%d] state: gid:%d (empty state set)\n", pq->q_id, s.gid);
		    }
	    } break;

 	case Q_SO_GROUP_STEER_FUN:
	    {
		    struct pfq_steering s;
		    
		    if (optlen != sizeof(s)) 
			    return -EINVAL;

		    if (copy_from_user(&s, optval, optlen)) 
			    return -EFAULT;
		    
                    if (s.gid < 0 || s.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] steering error: gid:%d invalid group!\n", pq->q_id, s.gid);
			    return -EINVAL;
		    }
		    
		    if (!__pfq_has_joined_group(s.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] steering error: gid:%d no permission!\n", pq->q_id, s.gid);
			    return -EPERM;
		    }

		    if (s.name == NULL) {
			__pfq_set_group_steering(s.gid, NULL);
                    	pr_devel("[PFQ|%d] steering: gid:%d (steering NONE)\n", pq->q_id, s.gid);
		    }
		    else {
                    	char name[Q_STEERING_NAME_LEN]; 
			steering_function_t fun;

                    	if (strncpy_from_user(name, s.name, Q_STEERING_NAME_LEN-1) < 0)
				return -EFAULT;

			name[Q_STEERING_NAME_LEN-1] = '\0';
                        
			fun = pfq_get_steering_function(name);
			if (fun == NULL) {
                    		pr_devel("[PFQ|%d] steering error: gid:%d '%s' unknown function!\n", pq->q_id, s.gid, name);
				return -EINVAL;
			}

			__pfq_set_group_steering(s.gid, fun);
                    	
			pr_devel("[PFQ|%d] steering gid:%d -> function '%s'\n", pq->q_id, s.gid, name);
		    }
	    } break;

	case Q_SO_GROUP_FPROG:
	    {
		    struct pfq_fprog fprog;
		    if (optlen != sizeof(fprog))
			    return -EINVAL;
		    
		    if (copy_from_user(&fprog, optval, optlen)) 
			    return -EFAULT;

                    if (fprog.gid < 0 || fprog.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] fprog error: gid:%d invalid group!\n", pq->q_id, fprog.gid);
			    return -EINVAL;
		    }

		    if (!__pfq_has_joined_group(fprog.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] fprog error: gid:%d no permission!\n", pq->q_id, fprog.gid);
			    return -EPERM;
		    }

                    if (fprog.fcode.len > 0)  /* set the filter */
		    {
			struct sk_filter *filter = pfq_alloc_sk_filter(&fprog.fcode);
		 	if (filter == NULL)
			{
                    	    pr_devel("[PFQ|%d] fprog error: prepare_sk_filter for gid:%d\n", pq->q_id, fprog.gid);
			    return -EINVAL;
			}

			__pfq_set_group_filter(fprog.gid, filter);

			pr_devel("[PFQ|%d] fprog: gid:%d (fprog len %d bytes)\n", pq->q_id, fprog.gid, fprog.fcode.len);
		    }
		    else 	/* reset the filter */
		    {
			__pfq_set_group_filter(fprog.gid, NULL);

			pr_devel("[PFQ|%d] fprog: gid:%d (resetting filter)\n", pq->q_id, fprog.gid);
		    }

	    } break;

        case Q_SO_GROUP_VLAN_FILT_TOGGLE:
            {
		    struct pfq_vlan_toggle vlan;

		    if (optlen != sizeof(vlan))
			    return -EINVAL;
		    
		    if (copy_from_user(&vlan, optval, optlen)) 
			    return -EFAULT;

                    if (vlan.gid < 0 || vlan.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] vlan_filt error: gid:%d invalid group!\n", pq->q_id, vlan.gid);
			    return -EINVAL;
		    }

		    if (!__pfq_has_joined_group(vlan.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] vlan_filt error: gid:%d no permission!\n", pq->q_id, vlan.gid);
			    return -EPERM;
                    }

		    __pfq_toggle_group_vlan_filters(vlan.gid, vlan.toggle);
 
                    pr_devel("[PFQ|%d] vlan filters %s for gid:%d\n", pq->q_id, (vlan.toggle ? "enabled" : "disabled"), vlan.gid);
            } break;

        case Q_SO_GROUP_VLAN_FILT:
            {
		    struct pfq_vlan_toggle filt;

		    if (optlen != sizeof(filt))
			    return -EINVAL;
		    
		    if (copy_from_user(&filt, optval, optlen)) 
			    return -EFAULT;

		    if (!__pfq_has_joined_group(filt.gid, pq->q_id)) {
                    	    pr_devel("[PFQ|%d] vlan_set error: gid:%d no permission!\n", pq->q_id, filt.gid);
			    return -EPERM;
		    }
               
                    if (filt.gid < 0 || filt.gid >= Q_MAX_GROUP) {
                    	    pr_devel("[PFQ|%d] vlan_set error: gid:%d invalid group!\n", pq->q_id, filt.gid);
			    return -EINVAL;
		    }

                    if (filt.vid < -1 || filt.vid > 4094) {
                    	    pr_devel("[PFQ|%d] vlan_set error: gid:%d invalid vid:%d!\n", pq->q_id, filt.gid, filt.vid);
			    return -EINVAL;
                    }
                
                    if (!__pfq_vlan_filters_enabled(filt.gid)) {
                    	    pr_devel("[PFQ|%d] vlan_set error: vlan filters disabled for gid:%d!\n", pq->q_id, filt.gid);
			    return -EINVAL;
                    }
                    
                    if (filt.vid  == -1) // any
                    {
                        int i;
                        for(i = 1; i < 4095; i++)
                                __pfq_set_group_vlan_filter(filt.gid, filt.toggle, i);
                    }
                    else 
                    {
                        __pfq_set_group_vlan_filter(filt.gid, filt.toggle, filt.vid);
                    }

                    pr_devel("[PFQ|%d] vlan_set filter vid %d for gid:%d\n", pq->q_id, filt.vid, filt.gid);
            } break;

        default: 
            {
                    found = false; 
            } break;
        }

        return found ? 0 : sock_setsockopt(sock, level, optname, optval, optlen);
}        


static inline
int
pfq_memory_mmap(struct vm_area_struct *vma,
                unsigned long size, char *ptr, unsigned int flags)
{
        vma->vm_flags |= flags;

        if (remap_vmalloc_range(vma, ptr, 0) != 0)
        {
                printk(KERN_WARNING "[PFQ] remap_vmalloc_range!\n");
                return -EAGAIN;
        }

        return 0;
}


static int 
pfq_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma)
{
        struct pfq_opt *pq = pfq_sk(sock->sk)->opt;
        unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
        int ret;

        if(size & (PAGE_SIZE-1)) {
                printk(KERN_WARNING "[PFQ] pfq_mmap: size not multiple of PAGE_SIZE!\n");
                return -EINVAL;
        }

        if(size > pq->q_queue_mem) {
                printk(KERN_WARNING "[PFQ] pfq_mmap: area too large!\n");
                return -EINVAL;
        }

        if((ret = pfq_memory_mmap(vma, size, pq->q_addr, VM_LOCKED)) < 0)
                return ret;

        return 0;
}


unsigned int 
pfq_poll(struct file *file, struct socket *sock, poll_table * wait)
{
        struct sock *sk = sock->sk;
        struct pfq_sock *po = pfq_sk(sk);
        struct pfq_opt *pq;
        struct pfq_queue_descr * q;
        unsigned int mask = 0;

        pq = po->opt;
        if (pq == NULL)
                return mask;
        q = (struct pfq_queue_descr *)pq->q_addr;
        if (q == NULL)
                return mask;

        if (mpdb_queue_len(pq) >= (pq->q_slots>>1)) {
                q->poll_wait = 0; 
                mask |= POLLIN | POLLRDNORM;
        }
        else if (!q->poll_wait) {
                q->poll_wait = 1;
                poll_wait(file, &pq->q_waitqueue, wait);
        }

        return mask;
}


static
int 
pfq_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
        switch (cmd) {
#ifdef CONFIG_INET
        case SIOCGIFFLAGS:
        case SIOCSIFFLAGS:
        case SIOCGIFCONF:
        case SIOCGIFMETRIC:
        case SIOCSIFMETRIC:
        case SIOCGIFMEM:
        case SIOCSIFMEM:
        case SIOCGIFMTU:
        case SIOCSIFMTU:
        case SIOCSIFLINK:
        case SIOCGIFHWADDR:
        case SIOCSIFHWADDR:
        case SIOCSIFMAP:
        case SIOCGIFMAP:
        case SIOCSIFSLAVE:
        case SIOCGIFSLAVE:
        case SIOCGIFINDEX:
        case SIOCGIFNAME:
        case SIOCGIFCOUNT:
        case SIOCSIFHWBROADCAST:
            return(inet_dgram_ops.ioctl(sock, cmd, arg));
#endif
        default:
            return -ENOIOCTLCMD;
        }

        return 0;
}


static
void pfq_proto_ops_init(void)
{
        pfq_ops = (struct proto_ops) 
        {
                .family = PF_Q,
                .owner = THIS_MODULE,

                /* Operations that make no sense on queue sockets. */
                .connect    = sock_no_connect,
                .socketpair = sock_no_socketpair,
                .accept     = sock_no_accept,
                .getname    = sock_no_getname,
                .listen     = sock_no_listen,
                .shutdown   = sock_no_shutdown,
                .sendpage   = sock_no_sendpage,

                /* Now the operations that really occur. */
                .release    = pfq_release,    
                .bind       = sock_no_bind,         // pfq_bind,
                .mmap       = pfq_mmap,             // pfq_mmap,
                .poll       = pfq_poll,             // pfq_poll,
                .setsockopt = pfq_setsockopt,       // pfq_setsockopt,
                .getsockopt = pfq_getsockopt,       // pfq_getsockopt,
                .ioctl      = pfq_ioctl,            // pfq_ioctl,
                .recvmsg    = sock_no_recvmsg,      // pfq_recvmsg,
                .sendmsg    = sock_no_sendmsg       // pfq_sendmsg,
        };
}


static
void pfq_proto_init(void)
{
        pfq_proto = (struct proto)
        {
                .name  = "PFQ",
                .owner = THIS_MODULE,
                .obj_size = sizeof(struct pfq_sock)
        };
}


static
void pfq_net_proto_family_init(void)
{
        pfq_family_ops = (struct net_proto_family)
        {
                .family = PF_Q,
                .create = pfq_create,
                .owner = THIS_MODULE,
        };
} 


static
void register_device_handler(void)
{
        if (sniff_incoming || sniff_outgoing || sniff_loopback)
        {
                pfq_prot_hook.func = pfq_packet_rcv;
                pfq_prot_hook.type = __constant_htons(ETH_P_ALL);
                dev_add_pack(&pfq_prot_hook);
        }
}


static
void unregister_device_handler(void) 
{
        if (sniff_incoming || sniff_outgoing || sniff_loopback)
        {
                dev_remove_pack(&pfq_prot_hook); /* Remove protocol hook */
        }
}


static int __init pfq_init_module(void)
{
        int n;
        printk(KERN_INFO "[PFQ] loading (%s)...\n", Q_VERSION);

        pfq_net_proto_family_init();
        pfq_proto_ops_init();
        pfq_proto_init();

        if (prefetch_len > PFQ_QUEUE_MAX_LEN) {
                printk(KERN_INFO "[PFQ] prefetch_len=%d not allowed (max=%lu)!\n", prefetch_len, (sizeof(unsigned long) << 3)-1);
                return -EFAULT;
        }

	/* create a per-cpu context */
	cpu_data = alloc_percpu(struct local_data);
	if (!cpu_data) {
                printk(KERN_WARNING "[PFQ] out of memory!\n");
		return -ENOMEM;
        }

        /* register pfq sniffer protocol */    
        n = proto_register(&pfq_proto, 0);
        if (n != 0)
                return n;

	/* register the pfq socket */
        sock_register(&pfq_family_ops);

        /* finally register the basic device handler */
        register_device_handler();

	/* register steering functions */

	pfq_steering_factory_init();
        
	printk(KERN_INFO "[PFQ] ready!\n");
        return 0;
}


static void __exit pfq_exit_module(void)
{        
        int cpu;

        /* unregister the basic device handler */
        unregister_device_handler();

        /* unregister the pfq socket */
        sock_unregister(PF_Q);

        /* unregister the pfq protocol */
        proto_unregister(&pfq_proto);

        /* disable direct capture */
        __pfq_devmap_monitor_reset();

        /* wait grace period */
        msleep(GRACE_PERIOD);

        /* destroy pipeline queues (of each cpu) */

        for_each_possible_cpu(cpu) {
		
                struct local_data *local_cache = per_cpu_ptr(cpu_data, cpu);
                struct pfq_queue_skb *this_queue = &local_cache->prefetch_queue;
                struct sk_buff *skb;
		int n = 0;
		queue_for_each(skb, n, this_queue)
		{
                        struct pfq_skb_cb *cb = (struct pfq_skb_cb *)skb->cb;
                        if (unlikely(cb->stolen_skb))
                                continue;

                 	kfree_skb(skb);
		}
       		pfq_queue_skb_flush(this_queue);
        }

        /* free per-cpu data */
	free_percpu(cpu_data);

	/* free steering functions */
	pfq_steering_factory_free();

        printk(KERN_INFO "[PFQ] unloaded.\n");
}


/* pfq direct capture drivers support */

inline
int pfq_direct_capture(const struct sk_buff *skb)
{
        return direct_capture && __pfq_devmap_monitor_get(skb->dev->ifindex);
}


inline
int pfq_normalize_skb(struct sk_buff *skb)
{
        skb_reset_network_header(skb);
	skb_reset_transport_header(skb);

#ifdef PFQ_USE_SKB_LINEARIZE
#pragma message "[PFQ] using skb_linearize"
	if(skb_linearize(skb) < 0)
	{
		__kfree_skb(skb);
		return -1;
	}
#endif
	return 0;
}


int
pfq_netif_receive_skb(struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb)))
        {
		if (pfq_normalize_skb(skb) < 0)
                	return NET_RX_DROP;

		pfq_receive(skb, true);
		return NET_RX_SUCCESS;
	}

	return netif_rx(skb);
}


int 
pfq_netif_rx(struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb)))
        {
		if (pfq_normalize_skb(skb) < 0)
                	return NET_RX_DROP;
		
		pfq_receive(skb, true);
		return NET_RX_SUCCESS;
	}

	return netif_receive_skb(skb);
}


gro_result_t 
pfq_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
        if (likely(pfq_direct_capture(skb)))
        {

		if (pfq_normalize_skb(skb) < 0)
                	return GRO_DROP;

                pfq_receive(skb, true);
                return GRO_NORMAL;
        }

        return napi_gro_receive(napi,skb);
}



EXPORT_SYMBOL_GPL(pfq_netif_rx);
EXPORT_SYMBOL_GPL(pfq_netif_receive_skb);
EXPORT_SYMBOL_GPL(pfq_gro_receive);

EXPORT_SYMBOL_GPL(pfq_register_steering_functions);
EXPORT_SYMBOL_GPL(pfq_unregister_steering_functions);

module_init(pfq_init_module);
module_exit(pfq_exit_module);
