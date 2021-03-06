/***************************************************************
 *
 * (C) 2011-16 Nicola Bonelli <nicola@pfq.io>
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
#include <linux/module.h>
#include <linux/init.h>

#include <linux/pf_q.h>

#include "../../lang/symtable.h"
#include "../../lang/module.h"

#include "../../pfq/nethdr.h"
#include "../../pfq/qbuff.h"

MODULE_LICENSE("GPL");


/* Basic RTP header */
struct rtphdr {
	uint8_t  rh_flags;	/* T:2 P:1 X:1 CC:4 */
	uint8_t  rh_pt;		/* M:1 PT:7 */
	uint16_t rh_seqno;	/* sequence number */
	uint32_t rh_ts;		/* media-specific time stamp */
	uint32_t rh_ssrc;	/* synchronization src id */
	/* data sources follow per cc */
};

struct rtcphdr {
	uint8_t  rh_flags;	/* T:2 P:1 CNT:5 */
	uint8_t  rh_type;	/* type */
	uint16_t rh_len;	/* length of message (in bytes) */
	uint32_t rh_ssrc;	/* synchronization src id */
};

/* RTP/RTCP packet */

struct headers {
	struct udphdr udp;
	union
	{
		struct rtphdr	rtp;
		struct rtcphdr  rtcp;
	} un;

} __attribute__((packed));


static inline
bool valid_codec(uint8_t c)
{
	return c < 19  ? true :
	       c > 95  ? true :
	       c == 25 ? true :
	       c == 26 ? true :
	       c == 28 ? true :
	       c == 31 ? true :
	       c == 32 ? true :
	       c == 33 ? true :
	       c == 34 ? true : false;
}


enum htype
{
	type_unknown = 0,
	type_rtp,
	type_rtcp,
	type_sip
};


struct hret
{
	uint32_t	hash;
	enum htype	type;
};


static struct hret
heuristic_voip(struct qbuff * buff, bool steer)
{
	struct hret ret = { 0, type_unknown };

	if (qbuff_eth_hdr(buff)->h_proto == __constant_htons(ETH_P_IP))
	{
		struct iphdr _iph;
		const struct iphdr *ip;

		struct headers _hdr;
		const struct headers *hdr;

                uint16_t source,dest;

		ip = qbuff_header_pointer(buff, QBUFF_SKB(buff)->mac_len, sizeof(_iph), &_iph);
		if (ip == NULL)
			return ret;

		hdr = qbuff_header_pointer(buff, QBUFF_SKB(buff)->mac_len + (ip->ihl<<2), sizeof(_hdr), &_hdr);
		if (hdr == NULL)
			return ret;

		dest = be16_to_cpu(hdr->udp.dest);
		source = be16_to_cpu(hdr->udp.source);

		/* check for SIP packets */

		if (dest == 5060 || source == 5060 ||
		    dest == 5061 || source == 5061) {
			ret.type = type_sip;
			return ret;
		}

                if (ip->protocol != IPPROTO_UDP)
			return ret;

		/* version => 2 */

		if (!((be16_to_cpu(hdr->un.rtp.rh_flags) & 0xc000) == 0x8000))
			return ret;

		if (dest < 1024 || source < 1024)
			return ret;

		if ((dest & 1) && (source & 1)) {		/* RTCP? */
			if (hdr->un.rtcp.rh_type != 200)	/* SR  */
				return ret;
			ret.type = type_rtcp;
		}
		else {						/* RTP? */
			if (!((dest & 1) || (source & 1))) {
				uint8_t pt = hdr->un.rtp.rh_pt;
				if (!valid_codec(pt))
					return ret;
			}
			ret.type = type_rtp;
		}

		if (steer) {
			ret.hash = (ip->saddr ^ (ip->saddr >> 8) ^ (ip->saddr >> 16) ^ (ip->saddr >> 24) ^
				    ip->daddr ^ (ip->daddr >> 8) ^ (ip->daddr >> 16) ^ (ip->daddr >> 24) ^
						(hdr->udp.source >> 1) ^ (hdr->udp.dest >> 1));
		}

		return ret;
	}

        return ret;
}


static bool
is_rtp(arguments_t arg, struct qbuff * buff)
{
	return heuristic_voip(buff, false).type == type_rtp;
}

static bool
is_rtcp(arguments_t arg, struct qbuff * buff)
{
	return heuristic_voip(buff, false).type == type_rtcp;
}

static bool
is_sip(arguments_t arg, struct qbuff * buff)
{
	return heuristic_voip(buff, false).type == type_sip;
}

static bool
is_voip(arguments_t arg, struct qbuff * buff)
{
	return heuristic_voip(buff, false).type != type_unknown;
}


static ActionQbuff
filter_rtp(arguments_t arg, struct qbuff * buff)
{
	if (is_rtp(arg, buff))
		return Pass(buff);
	return Drop(buff);
}

static ActionQbuff
filter_rtcp(arguments_t arg, struct qbuff * buff)
{
	if (is_rtcp(arg, buff))
		return Pass(buff);
	return Drop(buff);
}

static ActionQbuff
filter_sip(arguments_t arg, struct qbuff * buff)
{
	if (is_sip(arg, buff))
		return Pass(buff);
	return Drop(buff);
}

static ActionQbuff
filter_voip(arguments_t arg, struct qbuff * buff)
{
	if (is_voip(arg, buff))
		return Pass(buff);
	return Drop(buff);
}


static ActionQbuff
steering_rtp(arguments_t arg, struct qbuff * buff)
{
	struct hret ret = heuristic_voip(buff, true);

	switch(ret.type)
	{
	case type_unknown: return Drop(buff);
	case type_rtp:	   return Steering(buff, ret.hash);
	case type_rtcp:    return Steering(buff, ret.hash);
	case type_sip:     return Drop(buff);
	}

	return Drop(buff);
}


static ActionQbuff
steering_voip(arguments_t arg, struct qbuff * buff)
{
	struct hret ret = heuristic_voip(buff, true);

	switch(ret.type)
	{
	case type_unknown: return Drop(buff);
	case type_rtp:	   return Steering(buff, ret.hash);
	case type_rtcp:    return Steering(buff, ret.hash);
	case type_sip:     return Broadcast(buff);
	}

	return Drop(buff);
}


static struct pfq_lang_function_descr rtp_hooks[] = {

	{ "rtp"		, "Qbuff -> Action Qbuff", filter_rtp    , NULL, NULL},
	{ "rtcp"	, "Qbuff -> Action Qbuff", filter_rtcp   , NULL, NULL},
	{ "sip"		, "Qbuff -> Action Qbuff", filter_sip    , NULL, NULL},
	{ "voip"	, "Qbuff -> Action Qbuff", filter_voip   , NULL, NULL},
	{ "steer_rtp"	, "Qbuff -> Action Qbuff", steering_rtp  , NULL, NULL},
	{ "steer_voip"	, "Qbuff -> Action Qbuff", steering_voip , NULL, NULL},

	{ "is_rtp"	, "Qbuff -> Bool"	 , is_rtp	 , NULL, NULL},
	{ "is_rtcp"	, "Qbuff -> Bool"	 , is_rtcp	 , NULL, NULL},
	{ "is_sip"	, "Qbuff -> Bool"	 , is_sip	 , NULL, NULL},
	{ "is_voip"	, "Qbuff -> Bool"	 , is_voip	 , NULL, NULL},

	{ NULL }};


static int __init usr_init_module(void)
{
	int i = 0;
	for(; rtp_hooks[i].symbol; i++)
	{
		printk(KERN_INFO "[RTp] registiering %s\n", rtp_hooks[i].symbol);
	}

	printk(KERN_INFO "[RTR] registeering@%p...\n", rtp_hooks);

	if (pfq_lang_register_functions("[RTP]", rtp_hooks) < 0)
	{
		return -EPERM;
	}

	return 0;
}


static void __exit usr_exit_module(void)
{
	pfq_lang_unregister_functions("[RTP]", rtp_hooks);
}


module_init(usr_init_module);
module_exit(usr_exit_module);


