/** @file
 * @brief ICMPv6 related functions
 */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(CONFIG_NET_DEBUG_IPV6)
#define SYS_LOG_DOMAIN "net/ipv6"
#define NET_LOG_ENABLED 1

/* By default this prints too much data, set the value to 1 to see
 * neighbor cache contents.
 */
#define NET_DEBUG_NBR 0
#endif

#include <errno.h>
#include <net/net_core.h>
#include <net/nbuf.h>
#include <net/net_stats.h>
#include <net/net_context.h>
#include <net/net_mgmt.h>
#include "net_private.h"
#include "icmpv6.h"
#include "ipv6.h"
#include "nbr.h"
#include "6lo.h"
#include "route.h"
#include "rpl.h"
#include "net_stats.h"

#if defined(CONFIG_NET_IPV6_ND)

#define MAX_MULTICAST_SOLICIT 3
#define MAX_UNICAST_SOLICIT   3
#define DELAY_FIRST_PROBE_TIME (5 * MSEC_PER_SEC) /* RFC 4861 ch 10 */
#define RETRANS_TIMER 1000 /* in ms, RFC 4861 ch 10 */

extern void net_neighbor_data_remove(struct net_nbr *nbr);
extern void net_neighbor_table_clear(struct net_nbr_table *table);
static void nd_reachable_timeout(struct k_work *work);

NET_NBR_POOL_INIT(net_neighbor_pool,
		  CONFIG_NET_IPV6_MAX_NEIGHBORS,
		  sizeof(struct net_ipv6_nbr_data),
		  net_neighbor_data_remove,
		  0);

NET_NBR_TABLE_INIT(NET_NBR_GLOBAL,
		   neighbor,
		   net_neighbor_pool,
		   net_neighbor_table_clear);

const char *net_nbr_state2str(enum net_nbr_state state)
{
	switch (state) {
	case NET_NBR_INCOMPLETE:
		return "incomplete";
	case NET_NBR_REACHABLE:
		return "reachable";
	case NET_NBR_STALE:
		return "stale";
	case NET_NBR_DELAY:
		return "delay";
	case NET_NBR_PROBE:
		return "probe";
	}

	return "<invalid state>";
}

static void nbr_set_state(struct net_nbr *nbr, enum net_nbr_state new_state)
{
	if (new_state == net_ipv6_nbr_data(nbr)->state) {
		return;
	}

	NET_DBG("nbr %p %s -> %s", nbr,
		net_nbr_state2str(net_ipv6_nbr_data(nbr)->state),
		net_nbr_state2str(new_state));

	net_ipv6_nbr_data(nbr)->state = new_state;
}

static inline bool net_is_solicited(struct net_buf *buf)
{
	return NET_ICMPV6_NA_BUF(buf)->flags & NET_ICMPV6_NA_FLAG_SOLICITED;
}

static inline bool net_is_router(struct net_buf *buf)
{
	return NET_ICMPV6_NA_BUF(buf)->flags & NET_ICMPV6_NA_FLAG_ROUTER;
}

static inline bool net_is_override(struct net_buf *buf)
{
	return NET_ICMPV6_NA_BUF(buf)->flags & NET_ICMPV6_NA_FLAG_OVERRIDE;
}

static inline struct net_nbr *get_nbr(int idx)
{
	return &net_neighbor_pool[idx].nbr;
}

static inline struct net_nbr *get_nbr_from_data(struct net_ipv6_nbr_data *data)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (nbr->data == (uint8_t *)data) {
			return nbr;
		}
	}

	return NULL;
}

void net_ipv6_nbr_foreach(net_nbr_cb_t cb, void *user_data)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		cb(nbr, user_data);
	}
}

#if NET_DEBUG_NBR
void nbr_print(void)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		NET_DBG("[%d] %p %d/%d/%d/%d/%d pending %p iface %p idx %d "
			"ll %s addr %s",
			i, nbr, nbr->ref, net_ipv6_nbr_data(nbr)->ns_count,
			net_ipv6_nbr_data(nbr)->is_router,
			net_ipv6_nbr_data(nbr)->state,
			net_ipv6_nbr_data(nbr)->link_metric,
			net_ipv6_nbr_data(nbr)->pending,
			nbr->iface, nbr->idx,
			nbr->idx == NET_NBR_LLADDR_UNKNOWN ? "?" :
			net_sprint_ll_addr(
				net_nbr_get_lladdr(nbr->idx)->addr,
				net_nbr_get_lladdr(nbr->idx)->len),
			net_sprint_ipv6_addr(&net_ipv6_nbr_data(nbr)->addr));
	}
}
#else
#define nbr_print(...)
#endif

static struct net_nbr *nbr_lookup(struct net_nbr_table *table,
				  struct net_if *iface,
				  struct in6_addr *addr)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		if (nbr->iface == iface &&
		    net_ipv6_addr_cmp(&net_ipv6_nbr_data(nbr)->addr, addr)) {
			return nbr;
		}
	}

	return NULL;
}

struct net_ipv6_nbr_data *net_ipv6_get_nbr_by_index(uint8_t idx)
{
	struct net_nbr *nbr = get_nbr(idx);

	NET_ASSERT_INFO(nbr, "Invalid ll index %d", idx);

	return net_ipv6_nbr_data(nbr);
}

static inline void nbr_clear_ns_pending(struct net_ipv6_nbr_data *data)
{
	int ret;

	ret = k_delayed_work_cancel(&data->send_ns);
	if (ret < 0) {
		NET_DBG("Cannot cancel NS work (%d)", ret);
	}

	if (data->pending) {
		net_nbuf_unref(data->pending);
		data->pending = NULL;
	}
}

static inline void nbr_free(struct net_nbr *nbr)
{
	NET_DBG("nbr %p", nbr);

	nbr_clear_ns_pending(net_ipv6_nbr_data(nbr));

	k_delayed_work_cancel(&net_ipv6_nbr_data(nbr)->reachable);

	net_nbr_unref(nbr);
}

bool net_ipv6_nbr_rm(struct net_if *iface, struct in6_addr *addr)
{
	struct net_nbr *nbr;

	nbr = nbr_lookup(&net_neighbor.table, iface, addr);
	if (!nbr) {
		return false;
	}

	nbr_free(nbr);

	return true;
}

struct net_nbr *net_ipv6_nbr_add(struct net_if *iface,
				 struct in6_addr *addr,
				 struct net_linkaddr *lladdr,
				 bool is_router,
				 enum net_nbr_state state)
{
	struct net_nbr *nbr = net_nbr_get(&net_neighbor.table);

	if (!nbr) {
		return NULL;
	}

	if (net_nbr_link(nbr, iface, lladdr)) {
		nbr_free(nbr);
		return NULL;
	}

	net_ipaddr_copy(&net_ipv6_nbr_data(nbr)->addr, addr);
	nbr_set_state(nbr, state);
	net_ipv6_nbr_data(nbr)->is_router = is_router;

	NET_DBG("[%d] nbr %p state %d router %d IPv6 %s ll %s",
		nbr->idx, nbr, state, is_router,
		net_sprint_ipv6_addr(addr),
		net_sprint_ll_addr(lladdr->addr, lladdr->len));

	return nbr;
}

static inline struct net_nbr *nbr_add(struct net_buf *buf,
				      struct in6_addr *addr,
				      struct net_linkaddr *lladdr,
				      bool is_router,
				      enum net_nbr_state state)
{
	return net_ipv6_nbr_add(net_nbuf_iface(buf), addr, lladdr,
				is_router, state);
}

static struct net_nbr *nbr_new(struct net_if *iface,
			       struct in6_addr *addr,
			       enum net_nbr_state state)
{
	struct net_nbr *nbr = net_nbr_get(&net_neighbor.table);

	if (!nbr) {
		return NULL;
	}

	nbr->idx = NET_NBR_LLADDR_UNKNOWN;
	nbr->iface = iface;

	net_ipaddr_copy(&net_ipv6_nbr_data(nbr)->addr, addr);
	nbr_set_state(nbr, state);
	net_ipv6_nbr_data(nbr)->pending = NULL;

	NET_DBG("nbr %p iface %p state %d IPv6 %s",
		nbr, iface, state, net_sprint_ipv6_addr(addr));

	return nbr;
}

void net_neighbor_data_remove(struct net_nbr *nbr)
{
	NET_DBG("Neighbor %p removed", nbr);

	return;
}

void net_neighbor_table_clear(struct net_nbr_table *table)
{
	NET_DBG("Neighbor table %p cleared", table);
}

struct in6_addr *net_ipv6_nbr_lookup_by_index(struct net_if *iface,
					      uint8_t idx)
{
	int i;

	if (idx == NET_NBR_LLADDR_UNKNOWN) {
		return NULL;
	}

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		if (iface && nbr->iface != iface) {
			continue;
		}

		if (nbr->idx == idx) {
			return &net_ipv6_nbr_data(nbr)->addr;
		}
	}

	return NULL;
}
#endif /* CONFIG_NET_IPV6_ND */

const struct in6_addr *net_ipv6_unspecified_address(void)
{
	static const struct in6_addr addr = IN6ADDR_ANY_INIT;

	return &addr;
}

struct net_buf *net_ipv6_create_raw(struct net_buf *buf,
				    const struct in6_addr *src,
				    const struct in6_addr *dst,
				    struct net_if *iface,
				    uint8_t next_header)
{
	struct net_buf *header;

	header = net_nbuf_get_frag(buf, K_FOREVER);

	net_buf_frag_insert(buf, header);

	NET_IPV6_BUF(buf)->vtc = 0x60;
	NET_IPV6_BUF(buf)->tcflow = 0;
	NET_IPV6_BUF(buf)->flow = 0;

	NET_IPV6_BUF(buf)->nexthdr = 0;
	NET_IPV6_BUF(buf)->hop_limit = net_if_ipv6_get_hop_limit(iface);

	net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, dst);
	net_ipaddr_copy(&NET_IPV6_BUF(buf)->src, src);

	NET_IPV6_BUF(buf)->nexthdr = next_header;

	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));
	net_nbuf_set_family(buf, AF_INET6);

	net_buf_add(header, sizeof(struct net_ipv6_hdr));

	return buf;
}

struct net_buf *net_ipv6_create(struct net_context *context,
				struct net_buf *buf,
				const struct in6_addr *src,
				const struct in6_addr *dst)
{
	NET_ASSERT(((struct sockaddr_in6_ptr *)&context->local)->sin6_addr);

	if (!src) {
		src = ((struct sockaddr_in6_ptr *)&context->local)->sin6_addr;
	}

	if (net_is_ipv6_addr_unspecified(src)
	    || net_is_ipv6_addr_mcast(src)) {
		src = net_if_ipv6_select_src_addr(net_nbuf_iface(buf),
						  (struct in6_addr *)dst);
	}

	return net_ipv6_create_raw(buf,
				   src,
				   dst,
				   net_context_get_iface(context),
				   net_context_get_ip_proto(context));
}

int net_ipv6_finalize_raw(struct net_buf *buf, uint8_t next_header)
{
	/* Set the length of the IPv6 header */
	size_t total_len;

#if defined(CONFIG_NET_UDP) && defined(CONFIG_NET_RPL_INSERT_HBH_OPTION)
	if (next_header != IPPROTO_TCP && next_header != IPPROTO_ICMPV6) {
		/* Check if we need to add RPL header to sent UDP packet. */
		if (net_rpl_insert_header(buf) < 0) {
			NET_DBG("RPL HBHO insert failed");
			return -EINVAL;
		}
	}
#endif

	net_nbuf_compact(buf);

	total_len = net_buf_frags_len(buf->frags);

	total_len -= sizeof(struct net_ipv6_hdr);

	NET_IPV6_BUF(buf)->len[0] = total_len / 256;
	NET_IPV6_BUF(buf)->len[1] = total_len - NET_IPV6_BUF(buf)->len[0] * 256;

#if defined(CONFIG_NET_UDP)
	if (next_header == IPPROTO_UDP) {
		NET_UDP_BUF(buf)->chksum = 0;
		NET_UDP_BUF(buf)->chksum = ~net_calc_chksum_udp(buf);
	} else
#endif

#if defined(CONFIG_NET_TCP)
	if (next_header == IPPROTO_TCP) {
		NET_TCP_BUF(buf)->chksum = 0;
		NET_TCP_BUF(buf)->chksum = ~net_calc_chksum_tcp(buf);
	} else
#endif

	if (next_header == IPPROTO_ICMPV6) {
		NET_ICMP_BUF(buf)->chksum = 0;
		NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum(buf,
							     IPPROTO_ICMPV6);
	}

	return 0;
}

int net_ipv6_finalize(struct net_context *context, struct net_buf *buf)
{
	return net_ipv6_finalize_raw(buf, net_context_get_ip_proto(context));
}

#if defined(CONFIG_NET_IPV6_DAD)
int net_ipv6_start_dad(struct net_if *iface, struct net_if_addr *ifaddr)
{
	return net_ipv6_send_ns(iface, NULL, NULL, NULL,
				&ifaddr->address.in6_addr, true);
}

static inline bool dad_failed(struct net_if *iface, struct in6_addr *addr)
{
	if (net_is_ipv6_ll_addr(addr)) {
		NET_ERR("DAD failed, no ll IPv6 address!");
		return false;
	}

	net_if_ipv6_addr_rm(iface, addr);

	return true;
}
#endif /* CONFIG_NET_IPV6_DAD */

#if defined(CONFIG_NET_DEBUG_IPV6)
static inline void dbg_update_neighbor_lladdr(struct net_linkaddr *new_lladdr,
				struct net_linkaddr_storage *old_lladdr,
				struct in6_addr *addr)
{
	char out[sizeof("xx:xx:xx:xx:xx:xx:xx:xx")];

	snprintk(out, sizeof(out), "%s",
		 net_sprint_ll_addr(old_lladdr->addr, old_lladdr->len));

	NET_DBG("Updating neighbor %s lladdr %s (was %s)",
		net_sprint_ipv6_addr(addr),
		net_sprint_ll_addr(new_lladdr->addr, new_lladdr->len),
		out);
}

static inline void dbg_update_neighbor_lladdr_raw(uint8_t *new_lladdr,
				struct net_linkaddr_storage *old_lladdr,
				struct in6_addr *addr)
{
	struct net_linkaddr lladdr = {
		.len = old_lladdr->len,
		.addr = new_lladdr,
	};

	dbg_update_neighbor_lladdr(&lladdr, old_lladdr, addr);
}
#else
#define dbg_update_neighbor_lladdr(...)
#define dbg_update_neighbor_lladdr_raw(...)
#endif /* CONFIG_NET_DEBUG_IPV6 */

#if defined(CONFIG_NET_DEBUG_IPV6)
#define dbg_addr(action, pkt_str, src, dst)				\
	do {								\
		char out[NET_IPV6_ADDR_LEN];				\
									\
		snprintk(out, sizeof(out), "%s",			\
			 net_sprint_ipv6_addr(dst));			\
									\
		NET_DBG("%s %s from %s to %s", action,			\
			pkt_str, net_sprint_ipv6_addr(src), out);	\
									\
	} while (0)

#define dbg_addr_recv(pkt_str, src, dst)	\
	dbg_addr("Received", pkt_str, src, dst)

#define dbg_addr_sent(pkt_str, src, dst)	\
	dbg_addr("Sent", pkt_str, src, dst)

#define dbg_addr_with_tgt(action, pkt_str, src, dst, target)		\
	do {								\
		char out[NET_IPV6_ADDR_LEN];				\
		char tgt[NET_IPV6_ADDR_LEN];				\
									\
		snprintk(out, sizeof(out), "%s",			\
			 net_sprint_ipv6_addr(dst));			\
		snprintk(tgt, sizeof(tgt), "%s",			\
			 net_sprint_ipv6_addr(target));			\
									\
		NET_DBG("%s %s from %s to %s, target %s", action,	\
			pkt_str, net_sprint_ipv6_addr(src), out, tgt);	\
									\
	} while (0)

#define dbg_addr_recv_tgt(pkt_str, src, dst, tgt)		\
	dbg_addr_with_tgt("Received", pkt_str, src, dst, tgt)

#define dbg_addr_sent_tgt(pkt_str, src, dst, tgt)		\
	dbg_addr_with_tgt("Sent", pkt_str, src, dst, tgt)
#else
#define dbg_addr(...)
#define dbg_addr_recv(...)
#define dbg_addr_sent(...)

#define dbg_addr_with_tgt(...)
#define dbg_addr_recv_tgt(...)
#define dbg_addr_sent_tgt(...)
#endif /* CONFIG_NET_DEBUG_IPV6 */

#if defined(CONFIG_NET_IPV6_ND)
#define NS_REPLY_TIMEOUT MSEC_PER_SEC

static void ns_reply_timeout(struct k_work *work)
{
	/* We did not receive reply to a sent NS */
	struct net_ipv6_nbr_data *data = CONTAINER_OF(work,
						      struct net_ipv6_nbr_data,
						      send_ns);

	struct net_nbr *nbr = get_nbr_from_data(data);

	if (!nbr) {
		NET_DBG("NS timeout but no nbr data");
		return;
	}

	if (!data->pending) {
		/* Silently return, this is not an error as the work
		 * cannot be cancelled in certain cases.
		 */
		return;
	}

	NET_DBG("NS nbr %p pending %p timeout to %s", nbr, data->pending,
		net_sprint_ipv6_addr(&NET_IPV6_BUF(data->pending)->dst));

	/* To unref when pending variable was set */
	net_nbuf_unref(data->pending);

	/* To unref the original buf allocation */
	net_nbuf_unref(data->pending);

	data->pending = NULL;

	net_nbr_unref(nbr);
}

/* If the reserve has changed, we need to adjust it accordingly in the
 * fragment chain. This can only happen in IEEE 802.15.4 where the link
 * layer header size can change if the destination address changes.
 * Thus we need to check it here. Note that this cannot happen for IPv4
 * as 802.15.4 supports IPv6 only.
 */
static struct net_buf *update_ll_reserve(struct net_buf *buf,
					 struct in6_addr *addr)
{
	/* We need to go through all the fragments and adjust the
	 * fragment data size.
	 */
	uint16_t reserve, room_len, copy_len, pos;
	struct net_buf *orig_frag, *frag;

	reserve = net_if_get_ll_reserve(net_nbuf_iface(buf), addr);
	if (reserve == net_nbuf_ll_reserve(buf)) {
		return buf;
	}

	NET_DBG("Adjust reserve old %d new %d",
		net_nbuf_ll_reserve(buf), reserve);

	net_nbuf_set_ll_reserve(buf, reserve);

	orig_frag = buf->frags;
	copy_len = orig_frag->len;
	pos = 0;

	buf->frags = NULL;
	room_len = 0;
	frag = NULL;

	while (orig_frag) {
		if (!room_len) {
			frag = net_nbuf_get_frag(buf, K_FOREVER);

			net_buf_frag_add(buf, frag);

			room_len = net_buf_tailroom(frag);
		}

		if (room_len >= copy_len) {
			memcpy(net_buf_add(frag, copy_len),
			       orig_frag->data + pos, copy_len);

			room_len -= copy_len;
			copy_len = 0;
		} else {
			memcpy(net_buf_add(frag, room_len),
			       orig_frag->data + pos, room_len);

			copy_len -= room_len;
			pos += room_len;
			room_len = 0;
		}

		if (!copy_len) {
			struct net_buf *tmp = orig_frag;

			orig_frag = orig_frag->frags;

			tmp->frags = NULL;
			net_nbuf_unref(tmp);

			if (!orig_frag) {
				break;
			}

			copy_len = orig_frag->len;
			pos = 0;
		}
	}

	return buf;
}

struct net_buf *net_ipv6_prepare_for_send(struct net_buf *buf)
{
	struct in6_addr *nexthop = NULL;
	struct net_if *iface = NULL;
	struct net_nbr *nbr;

	NET_ASSERT(buf && buf->frags);

	/* Workaround Linux bug, see:
	 * https://jira.zephyrproject.org/browse/ZEP-1656
	 */
	if (atomic_test_bit(net_nbuf_iface(buf)->flags, NET_IF_POINTOPOINT)) {
		return buf;
	}

	if (net_nbuf_ll_dst(buf)->addr ||
	    net_is_ipv6_addr_mcast(&NET_IPV6_BUF(buf)->dst)) {
		return update_ll_reserve(buf, &NET_IPV6_BUF(buf)->dst);
	}

	if (net_if_ipv6_addr_onlink(&iface,
				    &NET_IPV6_BUF(buf)->dst)) {
		nexthop = &NET_IPV6_BUF(buf)->dst;
		net_nbuf_set_iface(buf, iface);
	} else {
		/* We need to figure out where the destination
		 * host is located.
		 */
		struct net_route_entry *route;
		struct net_if_router *router;

		route = net_route_lookup(NULL, &NET_IPV6_BUF(buf)->dst);
		if (route) {
			nexthop = net_route_get_nexthop(route);
			if (!nexthop) {
				net_route_del(route);

				net_rpl_global_repair(route);

				NET_DBG("No route to host %s",
					net_sprint_ipv6_addr(
						&NET_IPV6_BUF(buf)->dst));

				net_nbuf_unref(buf);
				return NULL;
			}
		} else {
			/* No specific route to this host, use the default
			 * route instead.
			 */
			router = net_if_ipv6_router_find_default(NULL,
						&NET_IPV6_BUF(buf)->dst);
			if (!router) {
				NET_DBG("No default route to %s",
					net_sprint_ipv6_addr(
						&NET_IPV6_BUF(buf)->dst));

				/* Try to send the packet anyway */
				nexthop = &NET_IPV6_BUF(buf)->dst;
				goto try_send;
			}

			nexthop = &router->address.in6_addr;
		}
	}

	if (net_rpl_update_header(buf, nexthop) < 0) {
		net_nbuf_unref(buf);
		return NULL;
	}

	if (!iface) {
		/* This means that the dst was not onlink, so try to
		 * figure out the interface using nexthop instead.
		 */
		if (net_if_ipv6_addr_onlink(&iface, nexthop)) {
			net_nbuf_set_iface(buf, iface);
		}

		/* If the above check returns null, we try to send
		 * the packet and hope for the best.
		 */
	}

try_send:
	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf), nexthop);

	NET_DBG("Neighbor lookup %p (%d) iface %p addr %s state %s", nbr,
		nbr ? nbr->idx : NET_NBR_LLADDR_UNKNOWN,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(nexthop),
		net_nbr_state2str(net_ipv6_nbr_data(nbr)->state));

	if (nbr && nbr->idx != NET_NBR_LLADDR_UNKNOWN) {
		struct net_linkaddr_storage *lladdr;

		lladdr = net_nbr_get_lladdr(nbr->idx);

		net_nbuf_ll_dst(buf)->addr = lladdr->addr;
		net_nbuf_ll_dst(buf)->len = lladdr->len;

		NET_DBG("Neighbor %p addr %s", nbr,
			net_sprint_ll_addr(lladdr->addr, lladdr->len));

		/* Start the NUD if we are in STALE state.
		 * See RFC 4861 ch 7.3.3 for details.
		 */
#if defined(CONFIG_NET_IPV6_ND)
		if (net_ipv6_nbr_data(nbr)->state == NET_NBR_STALE) {
			nbr_set_state(nbr, NET_NBR_DELAY);

			k_delayed_work_init(&net_ipv6_nbr_data(nbr)->reachable,
					    nd_reachable_timeout);

			k_delayed_work_submit(
				&net_ipv6_nbr_data(nbr)->reachable,
				DELAY_FIRST_PROBE_TIME);
		}
#endif

		return update_ll_reserve(buf, nexthop);
	}

	/* We need to send NS and wait for NA before sending the packet. */
	if (net_ipv6_send_ns(net_nbuf_iface(buf),
			     buf,
			     &NET_IPV6_BUF(buf)->src,
			     NULL,
			     nexthop,
			     false) < 0) {
		/* In case of an error, the NS send function will unref
		 * the buf.
		 */
		return NULL;
	}

	NET_DBG("Buf %p (frag %p) will be sent later", buf, buf->frags);

	return NULL;
}

struct net_nbr *net_ipv6_nbr_lookup(struct net_if *iface,
				    struct in6_addr *addr)
{
	return nbr_lookup(&net_neighbor.table, iface, addr);
}

struct net_nbr *net_ipv6_get_nbr(struct net_if *iface, uint8_t idx)
{
	int i;

	if (idx == NET_NBR_LLADDR_UNKNOWN) {
		return NULL;
	}

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (nbr->ref) {
			if (iface && nbr->iface != iface) {
				continue;
			}

			if (nbr->idx == idx) {
				return nbr;
			}
		}
	}

	return NULL;
}

static inline uint8_t get_llao_len(struct net_if *iface)
{
	if (iface->link_addr.len == 6) {
		return 8;
	} else if (iface->link_addr.len == 8) {
		return 16;
	}

	/* What else could it be? */
	NET_ASSERT_INFO(0, "Invalid link address length %d",
			iface->link_addr.len);

	return 0;
}

static inline void set_llao(struct net_linkaddr *lladdr,
			    uint8_t *llao, uint8_t llao_len, uint8_t type)
{
	llao[NET_ICMPV6_OPT_TYPE_OFFSET] = type;
	llao[NET_ICMPV6_OPT_LEN_OFFSET] = llao_len >> 3;

	memcpy(&llao[NET_ICMPV6_OPT_DATA_OFFSET], lladdr->addr, lladdr->len);

	memset(&llao[NET_ICMPV6_OPT_DATA_OFFSET + lladdr->len], 0,
	       llao_len - lladdr->len - 2);
}

static void setup_headers(struct net_buf *buf, uint8_t nd6_len,
			  uint8_t icmp_type)
{
	NET_IPV6_BUF(buf)->vtc = 0x60;
	NET_IPV6_BUF(buf)->tcflow = 0;
	NET_IPV6_BUF(buf)->flow = 0;
	NET_IPV6_BUF(buf)->len[0] = 0;
	NET_IPV6_BUF(buf)->len[1] = NET_ICMPH_LEN + nd6_len;

	NET_IPV6_BUF(buf)->nexthdr = IPPROTO_ICMPV6;
	NET_IPV6_BUF(buf)->hop_limit = NET_IPV6_ND_HOP_LIMIT;

	NET_ICMP_BUF(buf)->type = icmp_type;
	NET_ICMP_BUF(buf)->code = 0;
}

static inline void handle_ns_neighbor(struct net_buf *buf,
				      struct net_icmpv6_nd_opt_hdr *hdr)
{
	struct net_nbr *nbr;
	struct net_linkaddr lladdr = {
		.len = 8 * hdr->len - 2,
		.addr = (uint8_t *)hdr + 2,
	};

	/**
	 * IEEE802154 lladdress is 8 bytes long, so it requires
	 * 2 * 8 bytes - 2 - padding.
	 * The formula above needs to be adjusted.
	 */
	if (net_nbuf_ll_src(buf)->len < lladdr.len) {
		lladdr.len = net_nbuf_ll_src(buf)->len;
	}

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_IPV6_BUF(buf)->src);

	NET_DBG("Neighbor lookup %p iface %p addr %s", nbr,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));

	if (!nbr) {
		nbr_print();

		nbr = nbr_new(net_nbuf_iface(buf),
			      &NET_IPV6_BUF(buf)->src, NET_NBR_INCOMPLETE);
		if (nbr) {
			NET_DBG("Added %s to nbr cache",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));
		} else {
			NET_ERR("Could not add neighbor %s",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));

			return;
		}

		/* Send NS so that we can verify that the neighbor is
		 * reachable.
		 */
		net_ipv6_send_ns(net_nbuf_iface(buf), NULL, NULL, NULL,
				 &net_ipv6_nbr_data(nbr)->addr, false);
	}

	if (net_nbr_link(nbr, net_nbuf_iface(buf), &lladdr) == -EALREADY) {
		/* Update the lladdr if the node was already known */
		struct net_linkaddr_storage *cached_lladdr;

		cached_lladdr = net_nbr_get_lladdr(nbr->idx);

		if (memcmp(cached_lladdr->addr, lladdr.addr, lladdr.len)) {

			dbg_update_neighbor_lladdr(&lladdr,
						   cached_lladdr,
						   &NET_IPV6_BUF(buf)->src);

			net_linkaddr_set(cached_lladdr, lladdr.addr,
					 lladdr.len);

			nbr_set_state(nbr, NET_NBR_STALE);
		} else {
			if (net_ipv6_nbr_data(nbr)->state ==
							NET_NBR_INCOMPLETE) {
				nbr_set_state(nbr, NET_NBR_STALE);
			}
		}
	}
}

int net_ipv6_send_na(struct net_if *iface, struct in6_addr *src,
		     struct in6_addr *dst, struct in6_addr *tgt,
		     uint8_t flags)
{
	struct net_buf *buf, *frag;
	uint8_t llao_len;

	buf = net_nbuf_get_reserve_tx(net_if_get_ll_reserve(iface, dst),
				      K_FOREVER);

	NET_ASSERT_INFO(buf, "Out of TX buffers");

	frag = net_nbuf_get_frag(buf, K_FOREVER);

	NET_ASSERT_INFO(frag, "Out of DATA buffers");

	net_buf_frag_add(buf, frag);

	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_family(buf, AF_INET6);
	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));

	net_nbuf_ll_clear(buf);

	llao_len = get_llao_len(iface);

	net_nbuf_set_ext_len(buf, 0);

	setup_headers(buf, sizeof(struct net_icmpv6_na_hdr) + llao_len,
		      NET_ICMPV6_NA);

	net_ipaddr_copy(&NET_IPV6_BUF(buf)->src, src);
	net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, dst);
	net_ipaddr_copy(&NET_ICMPV6_NA_BUF(buf)->tgt, tgt);

	set_llao(&net_nbuf_iface(buf)->link_addr,
		 net_nbuf_icmp_data(buf) + sizeof(struct net_icmp_hdr) +
					sizeof(struct net_icmpv6_na_hdr),
		 llao_len, NET_ICMPV6_ND_OPT_TLLAO);

	NET_ICMPV6_NA_BUF(buf)->flags = flags;

	net_nbuf_set_len(buf->frags, NET_IPV6ICMPH_LEN +
			 sizeof(struct net_icmpv6_na_hdr) +
			 llao_len);

	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

	dbg_addr_sent_tgt("Neighbor Advertisement",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	if (net_send_data(buf) < 0) {
		goto drop;
	}

	net_stats_update_ipv6_nd_sent();

	return 0;

drop:
	net_nbuf_unref(buf);
	net_stats_update_ipv6_nd_drop();

	return -EINVAL;
}

static enum net_verdict handle_ns_input(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct net_icmpv6_nd_opt_hdr *hdr;
	struct net_if_addr *ifaddr;
	uint8_t flags = 0, prev_opt_len = 0;
	int ret;
	size_t left_len;

	dbg_addr_recv_tgt("Neighbor Solicitation",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	net_stats_update_ipv6_nd_recv();

	if ((total_len < (sizeof(struct net_ipv6_hdr) +
			  sizeof(struct net_icmp_hdr) +
			  sizeof(struct net_icmpv6_ns_hdr))) ||
	    (NET_ICMP_BUF(buf)->code != 0) ||
	    (NET_IPV6_BUF(buf)->hop_limit != NET_IPV6_ND_HOP_LIMIT) ||
	    net_is_ipv6_addr_mcast(&NET_ICMPV6_NS_BUF(buf)->tgt)) {
		NET_DBG("Preliminary check failed %u/%zu, code %u, hop %u",
			total_len, (sizeof(struct net_ipv6_hdr) +
				    sizeof(struct net_icmp_hdr) +
				    sizeof(struct net_icmpv6_ns_hdr)),
			NET_ICMP_BUF(buf)->code, NET_IPV6_BUF(buf)->hop_limit);
		goto drop;
	}

	net_nbuf_set_ext_opt_len(buf, sizeof(struct net_icmpv6_ns_hdr));
	hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);

	/* The parsing gets tricky if the ND struct is split
	 * between two fragments. FIXME later.
	 */
	if (buf->frags->len < ((uint8_t *)hdr - buf->frags->data)) {
		NET_DBG("NS struct split between fragments");
		goto drop;
	}

	left_len = buf->frags->len - (sizeof(struct net_ipv6_hdr) +
				      sizeof(struct net_icmp_hdr));

	while (net_nbuf_ext_opt_len(buf) < left_len &&
	       left_len < buf->frags->len) {

		if (!hdr->len) {
			break;
		}

		switch (hdr->type) {
		case NET_ICMPV6_ND_OPT_SLLAO:
			if (net_is_ipv6_addr_unspecified(
				    &NET_IPV6_BUF(buf)->src)) {
				goto drop;
			}

			handle_ns_neighbor(buf, hdr);
			break;

		default:
			NET_DBG("Unknown ND option 0x%x", hdr->type);
			break;
		}

		prev_opt_len = net_nbuf_ext_opt_len(buf);

		net_nbuf_set_ext_opt_len(buf, net_nbuf_ext_opt_len(buf) +
					 (hdr->len << 3));

		if (prev_opt_len == net_nbuf_ext_opt_len(buf)) {
			NET_ERR("Corrupted NS message");
			goto drop;
		}

		hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);
	}

	ifaddr = net_if_ipv6_addr_lookup_by_iface(net_nbuf_iface(buf),
						  &NET_ICMPV6_NS_BUF(buf)->tgt);
	if (!ifaddr) {
		NET_DBG("No such interface address %s",
			net_sprint_ipv6_addr(&NET_ICMPV6_NS_BUF(buf)->tgt));
		goto drop;
	}

#if !defined(CONFIG_NET_IPV6_DAD)
	if (net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src)) {
		goto drop;
	}

#else /* CONFIG_NET_IPV6_DAD */

	/* Do DAD */
	if (net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src)) {

		if (!net_is_ipv6_addr_solicited_node(&NET_IPV6_BUF(buf)->dst)) {
			NET_DBG("Not solicited node addr %s",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->dst));
			goto drop;
		}

		if (ifaddr->addr_state == NET_ADDR_TENTATIVE) {
			NET_DBG("DAD failed for %s iface %p",
				net_sprint_ipv6_addr(&ifaddr->address.in6_addr),
				net_nbuf_iface(buf));

			dad_failed(net_nbuf_iface(buf),
				   &ifaddr->address.in6_addr);
			goto drop;
		}

		/* We reuse the received buffer to send the NA */
		net_ipv6_addr_create_ll_allnodes_mcast(&NET_IPV6_BUF(buf)->dst);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				net_if_ipv6_select_src_addr(net_nbuf_iface(buf),
						&NET_IPV6_BUF(buf)->dst));
		flags = NET_ICMPV6_NA_FLAG_OVERRIDE;
		goto send_na;
	}
#endif /* CONFIG_NET_IPV6_DAD */

	if (net_is_my_ipv6_addr(&NET_IPV6_BUF(buf)->src)) {
		NET_DBG("Duplicate IPv6 %s address",
			net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));
		goto drop;
	}

	/* Address resolution */
	if (net_is_ipv6_addr_solicited_node(&NET_IPV6_BUF(buf)->dst)) {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst,
				&NET_IPV6_BUF(buf)->src);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				&NET_ICMPV6_NS_BUF(buf)->tgt);
		flags = NET_ICMPV6_NA_FLAG_SOLICITED |
			NET_ICMPV6_NA_FLAG_OVERRIDE;
		goto send_na;
	}

	/* Neighbor Unreachability Detection (NUD) */
	if (net_if_ipv6_addr_lookup_by_iface(net_nbuf_iface(buf),
					     &NET_IPV6_BUF(buf)->dst)) {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst,
				&NET_IPV6_BUF(buf)->src);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				&NET_ICMPV6_NS_BUF(buf)->tgt);
		flags = NET_ICMPV6_NA_FLAG_SOLICITED |
			NET_ICMPV6_NA_FLAG_OVERRIDE;
		goto send_na;
	} else {
		NET_DBG("NUD failed");
		goto drop;
	}

send_na:
	ret = net_ipv6_send_na(net_nbuf_iface(buf),
			       &NET_IPV6_BUF(buf)->src,
			       &NET_IPV6_BUF(buf)->dst,
			       &ifaddr->address.in6_addr,
			       flags);
	if (!ret) {
		net_nbuf_unref(buf);
		return NET_OK;
	}

	return NET_DROP;

drop:
	net_stats_update_ipv6_nd_drop();

	return NET_DROP;
}

static void nd_reachable_timeout(struct k_work *work)
{
	struct net_ipv6_nbr_data *data = CONTAINER_OF(work,
						      struct net_ipv6_nbr_data,
						      reachable);

	struct net_nbr *nbr = get_nbr_from_data(data);

	if (!data || !nbr) {
		NET_DBG("ND reachable timeout but no nbr data "
			"(nbr %p data %p)", nbr, data);
		return;
	}

	switch (data->state) {

	case NET_NBR_INCOMPLETE:
		if (data->ns_count >= MAX_MULTICAST_SOLICIT) {
			nbr_free(nbr);
		} else {
			data->ns_count++;

			NET_DBG("nbr %p incomplete count %u", nbr,
				data->ns_count);

			net_ipv6_send_ns(nbr->iface, NULL, NULL, NULL,
					 &data->addr, false);
		}
		break;

	case NET_NBR_REACHABLE:
		data->state = NET_NBR_STALE;

		NET_DBG("nbr %p moving %s state to STALE (%d)",
			nbr, net_sprint_ipv6_addr(&data->addr), data->state);
		break;

	case NET_NBR_STALE:
		NET_DBG("nbr %p removing stale address %s",
			nbr, net_sprint_ipv6_addr(&data->addr));
		nbr_free(nbr);
		break;

	case NET_NBR_DELAY:
		data->state = NET_NBR_PROBE;
		data->ns_count = 0;

		NET_DBG("nbr %p moving %s state to PROBE (%d)",
			nbr, net_sprint_ipv6_addr(&data->addr), data->state);

		/* Intentionally continuing to probe state */

	case NET_NBR_PROBE:
		if (data->ns_count >= MAX_UNICAST_SOLICIT) {
			struct net_if_router *router;

			router = net_if_ipv6_router_lookup(nbr->iface,
							   &data->addr);
			if (router && !router->is_infinite) {
				NET_DBG("nbr %p address %s PROBE ended (%d)",
					nbr, net_sprint_ipv6_addr(&data->addr),
					data->state);

				net_if_router_rm(router);
				nbr_free(nbr);
			}
		} else {
			data->ns_count++;

			NET_DBG("nbr %p probe count %u", nbr,
				data->ns_count);

			net_ipv6_send_ns(nbr->iface, NULL, NULL, NULL,
					 &data->addr, false);

			k_delayed_work_init(&net_ipv6_nbr_data(nbr)->reachable,
					    nd_reachable_timeout);

			k_delayed_work_submit(
				&net_ipv6_nbr_data(nbr)->reachable,
				RETRANS_TIMER);
		}
		break;
	}
}

void net_ipv6_nbr_set_reachable_timer(struct net_if *iface, struct net_nbr *nbr)
{
	uint32_t time;

	time = net_if_ipv6_get_reachable_time(iface);

	NET_ASSERT_INFO(time, "Zero reachable timeout!");

	NET_DBG("Starting reachable timer nbr %p data %p time %d ms",
		nbr, net_ipv6_nbr_data(nbr), time);

	k_delayed_work_init(&net_ipv6_nbr_data(nbr)->reachable,
			    nd_reachable_timeout);

	k_delayed_work_submit(&net_ipv6_nbr_data(nbr)->reachable, time);
}

static inline bool handle_na_neighbor(struct net_buf *buf,
				      struct net_icmpv6_nd_opt_hdr *hdr,
				      uint8_t *tllao)
{
	bool lladdr_changed = false;
	struct net_nbr *nbr;
	struct net_linkaddr_storage *cached_lladdr;
	struct net_buf *pending;

	ARG_UNUSED(hdr);

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_ICMPV6_NS_BUF(buf)->tgt);

	NET_DBG("Neighbor lookup %p iface %p addr %s", nbr,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_ICMPV6_NS_BUF(buf)->tgt));

	if (!nbr) {
		nbr_print();

		NET_DBG("No such neighbor found, msg discarded");
		return false;
	}

	if (nbr->idx == NET_NBR_LLADDR_UNKNOWN) {
		struct net_linkaddr lladdr;

		if (!tllao) {
			NET_DBG("No target link layer address.");
			return false;
		}

		lladdr.len = net_nbuf_iface(buf)->link_addr.len;
		lladdr.addr = &tllao[NET_ICMPV6_OPT_DATA_OFFSET];

		if (net_nbr_link(nbr, net_nbuf_iface(buf), &lladdr)) {
			nbr_free(nbr);
			return false;
		}

		NET_DBG("[%d] nbr %p state %d IPv6 %s ll %s",
			nbr->idx, nbr, net_ipv6_nbr_data(nbr)->state,
			net_sprint_ipv6_addr(&NET_ICMPV6_NS_BUF(buf)->tgt),
			net_sprint_ll_addr(lladdr.addr, lladdr.len));
	}

	cached_lladdr = net_nbr_get_lladdr(nbr->idx);
	if (!cached_lladdr) {
		NET_DBG("No lladdr but index defined");
		return false;
	}

	if (tllao) {
		lladdr_changed = memcmp(&tllao[NET_ICMPV6_OPT_DATA_OFFSET],
					cached_lladdr->addr,
					cached_lladdr->len);
	}

	/* Update the cached address if we do not yet known it */
	if (net_ipv6_nbr_data(nbr)->state == NET_NBR_INCOMPLETE) {
		if (!tllao) {
			return false;
		}

		if (lladdr_changed) {
			dbg_update_neighbor_lladdr_raw(
				&tllao[NET_ICMPV6_OPT_DATA_OFFSET],
				cached_lladdr,
				&NET_ICMPV6_NS_BUF(buf)->tgt);

			net_linkaddr_set(cached_lladdr,
					 &tllao[NET_ICMPV6_OPT_DATA_OFFSET],
					 cached_lladdr->len);
		}

		if (net_is_solicited(buf)) {
			nbr_set_state(nbr, NET_NBR_REACHABLE);
			net_ipv6_nbr_data(nbr)->ns_count = 0;

			/* We might have active timer from PROBE */
			k_delayed_work_cancel(
				&net_ipv6_nbr_data(nbr)->reachable);

			net_ipv6_nbr_set_reachable_timer(net_nbuf_iface(buf),
							 nbr);
		} else {
			nbr_set_state(nbr, NET_NBR_STALE);
		}

		net_ipv6_nbr_data(nbr)->is_router = net_is_router(buf);

		goto send_pending;
	}

	/* We do not update the address if override bit is not set
	 * and we have a valid address in the cache.
	 */
	if (!net_is_override(buf) && lladdr_changed) {
		if (net_ipv6_nbr_data(nbr)->state == NET_NBR_REACHABLE) {
			nbr_set_state(nbr, NET_NBR_STALE);
		}

		return false;
	}

	if (net_is_override(buf) ||
	    (!net_is_override(buf) && tllao && !lladdr_changed)) {

		if (lladdr_changed) {
			dbg_update_neighbor_lladdr_raw(
				&tllao[NET_ICMPV6_OPT_DATA_OFFSET],
				cached_lladdr,
				&NET_ICMPV6_NS_BUF(buf)->tgt);

			net_linkaddr_set(cached_lladdr,
					 &tllao[NET_ICMPV6_OPT_DATA_OFFSET],
					 cached_lladdr->len);
		}

		if (net_is_solicited(buf)) {
			nbr_set_state(nbr, NET_NBR_REACHABLE);

			/* We might have active timer from PROBE */
			k_delayed_work_cancel(
				&net_ipv6_nbr_data(nbr)->reachable);

			net_ipv6_nbr_set_reachable_timer(net_nbuf_iface(buf),
							 nbr);
		} else {
			if (lladdr_changed) {
				nbr_set_state(nbr, NET_NBR_STALE);
			}
		}
	}

	if (net_ipv6_nbr_data(nbr)->is_router && !net_is_router(buf)) {
		/* Update the routing if the peer is no longer
		 * a router.
		 */
		/* FIXME */
	}

	net_ipv6_nbr_data(nbr)->is_router = net_is_router(buf);

send_pending:
	/* Next send any pending messages to the peer. */
	pending = net_ipv6_nbr_data(nbr)->pending;

	if (pending) {
		NET_DBG("Sending pending %p to %s lladdr %s", pending,
			net_sprint_ipv6_addr(&NET_IPV6_BUF(pending)->dst),
			net_sprint_ll_addr(cached_lladdr->addr,
					   cached_lladdr->len));

		if (net_send_data(pending) < 0) {
			nbr_clear_ns_pending(net_ipv6_nbr_data(nbr));
		} else {
			net_ipv6_nbr_data(nbr)->pending = NULL;
		}

		net_nbuf_unref(pending);
	}

	return true;
}

static enum net_verdict handle_na_input(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct net_icmpv6_nd_opt_hdr *hdr;
	struct net_if_addr *ifaddr;
	uint8_t *tllao = NULL;
	uint8_t prev_opt_len = 0;
	size_t left_len;

	dbg_addr_recv_tgt("Neighbor Advertisement",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	net_stats_update_ipv6_nd_recv();

	if ((total_len < (sizeof(struct net_ipv6_hdr) +
			  sizeof(struct net_icmp_hdr) +
			  sizeof(struct net_icmpv6_na_hdr) +
			  sizeof(struct net_icmpv6_nd_opt_hdr))) ||
	    (NET_ICMP_BUF(buf)->code != 0) ||
	    (NET_IPV6_BUF(buf)->hop_limit != NET_IPV6_ND_HOP_LIMIT) ||
	    net_is_ipv6_addr_mcast(&NET_ICMPV6_NS_BUF(buf)->tgt) ||
	    (net_is_solicited(buf) &&
	     net_is_ipv6_addr_mcast(&NET_IPV6_BUF(buf)->dst))) {
		goto drop;
	}

	net_nbuf_set_ext_opt_len(buf, sizeof(struct net_icmpv6_na_hdr));
	hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);

	/* The parsing gets tricky if the ND struct is split
	 * between two fragments. FIXME later.
	 */
	if (buf->frags->len < ((uint8_t *)hdr - buf->frags->data)) {
		NET_DBG("NA struct split between fragments");
		goto drop;
	}

	left_len = buf->frags->len - (sizeof(struct net_ipv6_hdr) +
				      sizeof(struct net_icmp_hdr));

	while (net_nbuf_ext_opt_len(buf) < left_len &&
	       left_len < buf->frags->len) {

		if (!hdr->len) {
			break;
		}

		switch (hdr->type) {
		case NET_ICMPV6_ND_OPT_TLLAO:
			tllao = (uint8_t *)hdr;
			break;

		default:
			NET_DBG("Unknown ND option 0x%x", hdr->type);
			break;
		}

		prev_opt_len = net_nbuf_ext_opt_len(buf);

		net_nbuf_set_ext_opt_len(buf, net_nbuf_ext_opt_len(buf) +
					 (hdr->len << 3));

		if (prev_opt_len == net_nbuf_ext_opt_len(buf)) {
			NET_ERR("Corrupted NA message");
			goto drop;
		}

		hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);
	}

	ifaddr = net_if_ipv6_addr_lookup_by_iface(net_nbuf_iface(buf),
						  &NET_ICMPV6_NA_BUF(buf)->tgt);
	if (ifaddr) {
		NET_DBG("Interface %p already has address %s",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&NET_ICMPV6_NA_BUF(buf)->tgt));

#if defined(CONFIG_NET_IPV6_DAD)
		if (ifaddr->addr_state == NET_ADDR_TENTATIVE) {
			dad_failed(net_nbuf_iface(buf),
				   &NET_ICMPV6_NA_BUF(buf)->tgt);
		}
#endif /* CONFIG_NET_IPV6_DAD */

		goto drop;
	}

	if (!handle_na_neighbor(buf, hdr, tllao)) {
		goto drop;
	}

	net_nbuf_unref(buf);

	net_stats_update_ipv6_nd_sent();

	return NET_OK;

drop:
	net_stats_update_ipv6_nd_drop();

	return NET_DROP;
}

int net_ipv6_send_ns(struct net_if *iface,
		     struct net_buf *pending,
		     struct in6_addr *src,
		     struct in6_addr *dst,
		     struct in6_addr *tgt,
		     bool is_my_address)
{
	struct net_buf *buf, *frag;
	struct net_nbr *nbr;
	uint8_t llao_len;

	buf = net_nbuf_get_reserve_tx(net_if_get_ll_reserve(iface, dst),
				      K_FOREVER);

	NET_ASSERT_INFO(buf, "Out of TX buffers");

	frag = net_nbuf_get_frag(buf, K_FOREVER);

	NET_ASSERT_INFO(frag, "Out of DATA buffers");

	net_buf_frag_add(buf, frag);

	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_family(buf, AF_INET6);
	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));

	net_nbuf_ll_clear(buf);

	llao_len = get_llao_len(net_nbuf_iface(buf));

	setup_headers(buf, sizeof(struct net_icmpv6_ns_hdr) + llao_len,
		      NET_ICMPV6_NS);

	if (!dst) {
		net_ipv6_addr_create_solicited_node(tgt,
						    &NET_IPV6_BUF(buf)->dst);
	} else {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, dst);
	}

	NET_ICMPV6_NS_BUF(buf)->reserved = 0;

	net_ipaddr_copy(&NET_ICMPV6_NS_BUF(buf)->tgt, tgt);

	if (is_my_address) {
		/* DAD */
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				net_ipv6_unspecified_address());

		NET_IPV6_BUF(buf)->len[1] -= llao_len;

		net_buf_add(frag,
			    sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_ns_hdr));
	} else {
		if (src) {
			net_ipaddr_copy(&NET_IPV6_BUF(buf)->src, src);
		} else {
			net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
					net_if_ipv6_select_src_addr(
						net_nbuf_iface(buf),
						&NET_IPV6_BUF(buf)->dst));
		}

		if (net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src)) {
			NET_DBG("No source address for NS");
			goto drop;
		}

		set_llao(&net_nbuf_iface(buf)->link_addr,
			 net_nbuf_icmp_data(buf) +
			 sizeof(struct net_icmp_hdr) +
			 sizeof(struct net_icmpv6_ns_hdr),
			 llao_len, NET_ICMPV6_ND_OPT_SLLAO);

		net_buf_add(frag,
			    sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_ns_hdr) + llao_len);
	}

	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_ICMPV6_NS_BUF(buf)->tgt);
	if (!nbr) {
		nbr_print();

		nbr = nbr_new(net_nbuf_iface(buf),
			      &NET_ICMPV6_NS_BUF(buf)->tgt,
			      NET_NBR_INCOMPLETE);
		if (!nbr) {
			NET_DBG("Could not create new neighbor %s",
				net_sprint_ipv6_addr(
						&NET_ICMPV6_NS_BUF(buf)->tgt));
			goto drop;
		}
	}

	if (pending) {
		if (!net_ipv6_nbr_data(nbr)->pending) {
			net_ipv6_nbr_data(nbr)->pending = net_nbuf_ref(pending);
		} else {
			NET_DBG("Buffer %p already pending for "
				"operation. Discarding pending %p and buf %p",
				net_ipv6_nbr_data(nbr)->pending, pending, buf);
			net_nbuf_unref(pending);
			goto drop;
		}

		NET_DBG("Setting timeout %d for NS", NS_REPLY_TIMEOUT);

		k_delayed_work_init(&net_ipv6_nbr_data(nbr)->send_ns,
				    ns_reply_timeout);
		k_delayed_work_submit(&net_ipv6_nbr_data(nbr)->send_ns,
				      NS_REPLY_TIMEOUT);
	}

	dbg_addr_sent_tgt("Neighbor Solicitation",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	if (net_send_data(buf) < 0) {
		NET_DBG("Cannot send NS %p (pending %p)", buf, pending);

		if (pending) {
			nbr_clear_ns_pending(net_ipv6_nbr_data(nbr));
		}

		goto drop;
	}

	net_stats_update_ipv6_nd_sent();

	return 0;

drop:
	net_nbuf_unref(buf);
	net_stats_update_ipv6_nd_drop();

	return -EINVAL;
}

int net_ipv6_send_rs(struct net_if *iface)
{
	struct net_buf *buf, *frag;
	bool unspec_src;
	uint8_t llao_len = 0;

	buf = net_nbuf_get_reserve_tx(net_if_get_ll_reserve(iface, NULL),
				      K_FOREVER);

	frag = net_nbuf_get_frag(buf, K_FOREVER);

	net_buf_frag_add(buf, frag);

	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_family(buf, AF_INET6);
	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));

	net_nbuf_ll_clear(buf);

	net_ipv6_addr_create_ll_allnodes_mcast(&NET_IPV6_BUF(buf)->dst);

	net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
			net_if_ipv6_select_src_addr(iface,
						    &NET_IPV6_BUF(buf)->dst));

	unspec_src = net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src);
	if (!unspec_src) {
		llao_len = get_llao_len(net_nbuf_iface(buf));
	}

	setup_headers(buf, sizeof(struct net_icmpv6_rs_hdr) + llao_len,
		      NET_ICMPV6_RS);

	if (!unspec_src) {
		set_llao(&net_nbuf_iface(buf)->link_addr,
			 net_nbuf_icmp_data(buf) +
			 sizeof(struct net_icmp_hdr) +
			 sizeof(struct net_icmpv6_rs_hdr),
			 llao_len, NET_ICMPV6_ND_OPT_SLLAO);

		net_buf_add(frag, sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_rs_hdr) +
			    llao_len);
	} else {
		net_buf_add(frag, sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_rs_hdr));
	}

	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

	dbg_addr_sent("Router Solicitation",
		      &NET_IPV6_BUF(buf)->src,
		      &NET_IPV6_BUF(buf)->dst);

	if (net_send_data(buf) < 0) {
		goto drop;
	}

	net_stats_update_ipv6_nd_sent();

	return 0;

drop:
	net_nbuf_unref(buf);
	net_stats_update_ipv6_nd_drop();

	return -EINVAL;
}

int net_ipv6_start_rs(struct net_if *iface)
{
	return net_ipv6_send_rs(iface);
}

static inline struct net_buf *handle_ra_neighbor(struct net_buf *buf,
						 struct net_buf *frag,
						 uint8_t len,
						 uint16_t offset, uint16_t *pos,
						 struct net_nbr **nbr)

{
	struct net_linkaddr lladdr;
	struct net_linkaddr_storage llstorage;
	uint8_t padding;

	if (!nbr) {
		return NULL;
	}

	llstorage.len = NET_LINK_ADDR_MAX_LENGTH;
	lladdr.len = NET_LINK_ADDR_MAX_LENGTH;
	lladdr.addr = llstorage.addr;
	if (net_nbuf_ll_src(buf)->len < lladdr.len) {
		lladdr.len = net_nbuf_ll_src(buf)->len;
	}

	frag = net_nbuf_read(frag, offset, pos, lladdr.len, lladdr.addr);
	if (!frag && offset) {
		return NULL;
	}

	padding = len * 8 - 2 - lladdr.len;
	if (padding) {
		frag = net_nbuf_read(frag, *pos, pos, padding, NULL);
		if (!frag && *pos) {
			return NULL;
		}
	}

	*nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			  &NET_IPV6_BUF(buf)->src);

	NET_DBG("Neighbor lookup %p iface %p addr %s", *nbr,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));

	if (!*nbr) {
		nbr_print();

		*nbr = nbr_add(buf, &NET_IPV6_BUF(buf)->src, &lladdr,
			       true, NET_NBR_STALE);
		if (!*nbr) {
			NET_ERR("Could not add router neighbor %s [%s]",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src),
				net_sprint_ll_addr(lladdr.addr, lladdr.len));
			return NULL;
		}
	}

	if (net_nbr_link(*nbr, net_nbuf_iface(buf), &lladdr) == -EALREADY) {
		/* Update the lladdr if the node was already known */
		struct net_linkaddr_storage *cached_lladdr;

		cached_lladdr = net_nbr_get_lladdr((*nbr)->idx);

		if (memcmp(cached_lladdr->addr, lladdr.addr, lladdr.len)) {

			dbg_update_neighbor_lladdr(&lladdr,
						   cached_lladdr,
						   &NET_IPV6_BUF(buf)->src);

			net_linkaddr_set(cached_lladdr, lladdr.addr,
					 lladdr.len);

			nbr_set_state(*nbr, NET_NBR_STALE);
		} else {
			if (net_ipv6_nbr_data(*nbr)->state ==
							NET_NBR_INCOMPLETE) {
				nbr_set_state(*nbr, NET_NBR_STALE);
			}
		}
	}

	net_ipv6_nbr_data(*nbr)->is_router = true;

	return frag;
}

static inline void handle_prefix_onlink(struct net_buf *buf,
			struct net_icmpv6_nd_opt_prefix_info *prefix_info)
{
	struct net_if_ipv6_prefix *prefix;

	prefix = net_if_ipv6_prefix_lookup(net_nbuf_iface(buf),
					   &prefix_info->prefix,
					   prefix_info->len);
	if (!prefix) {
		if (!prefix_info->valid_lifetime) {
			return;
		}

		prefix = net_if_ipv6_prefix_add(net_nbuf_iface(buf),
					&prefix_info->prefix,
					prefix_info->len,
					prefix_info->valid_lifetime);
		if (prefix) {
			NET_DBG("Interface %p add prefix %s/%d lifetime %u",
				net_nbuf_iface(buf),
				net_sprint_ipv6_addr(&prefix_info->prefix),
				prefix_info->prefix_len,
				prefix_info->valid_lifetime);
		} else {
			NET_ERR("Prefix %s/%d could not be added to iface %p",
				net_sprint_ipv6_addr(&prefix_info->prefix),
				prefix_info->len,
				net_nbuf_iface(buf));

			return;
		}
	}

	switch (prefix_info->valid_lifetime) {
	case 0:
		NET_DBG("Interface %p delete prefix %s/%d",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&prefix_info->prefix),
			prefix_info->len);

		net_if_ipv6_prefix_rm(net_nbuf_iface(buf),
				      &prefix->prefix,
				      prefix->len);
		break;

	case NET_IPV6_ND_INFINITE_LIFETIME:
		NET_DBG("Interface %p prefix %s/%d infinite",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&prefix->prefix),
			prefix->len);

		net_if_ipv6_prefix_set_lf(prefix, true);
		break;

	default:
		NET_DBG("Interface %p update prefix %s/%u lifetime %u",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&prefix_info->prefix),
			prefix_info->prefix_len,
			prefix_info->valid_lifetime);

		net_if_ipv6_prefix_set_lf(prefix, false);
		net_if_ipv6_prefix_set_timer(prefix,
					prefix_info->valid_lifetime);
		break;
	}
}

#define TWO_HOURS (2 * 60 * 60)

static inline uint32_t remaining(struct k_delayed_work *work)
{
	return k_delayed_work_remaining_get(work) / MSEC_PER_SEC;
}

static inline void handle_prefix_autonomous(struct net_buf *buf,
			struct net_icmpv6_nd_opt_prefix_info *prefix_info)
{
	struct in6_addr addr = { };
	struct net_if_addr *ifaddr;

	/* Create IPv6 address using the given prefix and iid. We first
	 * setup link local address, and then copy prefix over first 8
	 * bytes of that address.
	 */
	net_ipv6_addr_create_iid(&addr,
				 net_if_get_link_addr(net_nbuf_iface(buf)));
	memcpy(&addr, &prefix_info->prefix, sizeof(struct in6_addr) / 2);

	ifaddr = net_if_ipv6_addr_lookup(&addr, NULL);
	if (ifaddr && ifaddr->addr_type == NET_ADDR_AUTOCONF) {
		if (prefix_info->valid_lifetime ==
		    NET_IPV6_ND_INFINITE_LIFETIME) {
			net_if_addr_set_lf(ifaddr, true);
			return;
		}

		/* RFC 4862 ch 5.5.3 */
		if ((prefix_info->valid_lifetime > TWO_HOURS) ||
		    (prefix_info->valid_lifetime >
		     remaining(&ifaddr->lifetime))) {
			NET_DBG("Timer updating for address %s "
				"long lifetime %u secs",
				net_sprint_ipv6_addr(&addr),
				prefix_info->valid_lifetime);

			net_if_ipv6_addr_update_lifetime(ifaddr,
						  prefix_info->valid_lifetime);
		} else {
			NET_DBG("Timer updating for address %s "
				"lifetime %u secs",
				net_sprint_ipv6_addr(&addr), TWO_HOURS);

			net_if_ipv6_addr_update_lifetime(ifaddr, TWO_HOURS);
		}

		net_if_addr_set_lf(ifaddr, false);
	} else {
		if (prefix_info->valid_lifetime ==
		    NET_IPV6_ND_INFINITE_LIFETIME) {
			net_if_ipv6_addr_add(net_nbuf_iface(buf),
					     &addr, NET_ADDR_AUTOCONF, 0);
		} else {
			net_if_ipv6_addr_add(net_nbuf_iface(buf),
					     &addr, NET_ADDR_AUTOCONF,
					     prefix_info->valid_lifetime);
		}
	}
}

static inline struct net_buf *handle_ra_prefix(struct net_buf *buf,
					       struct net_buf *frag,
					       uint8_t len,
					       uint16_t offset, uint16_t *pos)
{
	struct net_icmpv6_nd_opt_prefix_info prefix_info;

	prefix_info.type = NET_ICMPV6_ND_OPT_PREFIX_INFO;
	prefix_info.len = len * 8 - 2;

	frag = net_nbuf_read(frag, offset, pos, 1, &prefix_info.prefix_len);
	frag = net_nbuf_read(frag, *pos, pos, 1, &prefix_info.flags);
	frag = net_nbuf_read_be32(frag, *pos, pos, &prefix_info.valid_lifetime);
	frag = net_nbuf_read_be32(frag, *pos, pos,
				  &prefix_info.preferred_lifetime);
	/* Skip reserved bytes */
	frag = net_nbuf_skip(frag, *pos, pos, 4);
	frag = net_nbuf_read(frag, *pos, pos, sizeof(struct in6_addr),
			     prefix_info.prefix.s6_addr);
	if (!frag && *pos) {
		return NULL;
	}

	if (prefix_info.valid_lifetime >= prefix_info.preferred_lifetime &&
	    !net_is_ipv6_ll_addr(&prefix_info.prefix)) {

		if (prefix_info.flags & NET_ICMPV6_RA_FLAG_ONLINK) {
			handle_prefix_onlink(buf, &prefix_info);
		}

		if ((prefix_info.flags & NET_ICMPV6_RA_FLAG_AUTONOMOUS) &&
		    prefix_info.valid_lifetime &&
		    (prefix_info.prefix_len == NET_IPV6_DEFAULT_PREFIX_LEN)) {
			handle_prefix_autonomous(buf, &prefix_info);
		}
	}

	return frag;
}

#if defined(CONFIG_NET_6LO_CONTEXT)
/* 6lowpan Context Option RFC 6775, 4.2 */
static inline struct net_buf *handle_ra_6co(struct net_buf *buf,
					    struct net_buf *frag,
					    uint8_t len,
					    uint16_t offset, uint16_t *pos)
{
	struct net_icmpv6_nd_opt_6co context;

	context.type = NET_ICMPV6_ND_OPT_6CO;
	context.len = len * 8 - 2;

	frag = net_nbuf_read_u8(frag, offset, pos, &context.context_len);

	/* RFC 6775, 4.2
	 * Context Length: 8-bit unsigned integer.  The number of leading
	 * bits in the Context Prefix field that are valid.  The value ranges
	 * from 0 to 128.  If it is more than 64, then the Length MUST be 3.
	 */
	if (context.context_len > 64 && len != 3) {
		return NULL;
	}

	if (context.context_len <= 64 && len != 2) {
		return NULL;
	}

	context.context_len = context.context_len / 8;
	frag = net_nbuf_read_u8(frag, *pos, pos, &context.flag);

	/* Skip reserved bytes */
	frag = net_nbuf_skip(frag, *pos, pos, 2);
	frag = net_nbuf_read_be16(frag, *pos, pos, &context.lifetime);

	/* RFC 6775, 4.2 (Length field). Length can be 2 or 3 depending
	 * on the length of context prefix field.
	 */
	if (len == 3) {
		frag = net_nbuf_read(frag, *pos, pos, sizeof(struct in6_addr),
				     context.prefix.s6_addr);
	} else if (len == 2) {
		/* If length is 2 means only 64 bits of context prefix
		 * is available, rest set to zeros.
		 */
		frag = net_nbuf_read(frag, *pos, pos, 8,
				     context.prefix.s6_addr);
	}

	if (!frag && *pos) {
		return NULL;
	}

	/* context_len: The number of leading bits in the Context Prefix
	 * field that are valid. So set remaining data to zero.
	 */
	if (context.context_len != sizeof(struct in6_addr)) {
		memset(context.prefix.s6_addr + context.context_len, 0,
		       sizeof(struct in6_addr) - context.context_len);
	}

	net_6lo_set_context(net_nbuf_iface(buf), &context);

	return frag;
}
#endif

static enum net_verdict handle_ra_input(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct net_nbr *nbr = NULL;
	struct net_if_router *router;
	struct net_buf *frag;
	uint16_t router_lifetime;
	uint32_t reachable_time;
	uint32_t retrans_timer;
	uint8_t hop_limit;
	uint16_t offset;
	uint8_t length;
	uint8_t type;
	uint32_t mtu;

	dbg_addr_recv("Router Advertisement",
		      &NET_IPV6_BUF(buf)->src,
		      &NET_IPV6_BUF(buf)->dst);

	net_stats_update_ipv6_nd_recv();

	if ((total_len < (sizeof(struct net_ipv6_hdr) +
			  sizeof(struct net_icmp_hdr) +
			  sizeof(struct net_icmpv6_ra_hdr) +
			  sizeof(struct net_icmpv6_nd_opt_hdr))) ||
	    (NET_ICMP_BUF(buf)->code != 0) ||
	    (NET_IPV6_BUF(buf)->hop_limit != NET_IPV6_ND_HOP_LIMIT) ||
	    !net_is_ipv6_ll_addr(&NET_IPV6_BUF(buf)->src)) {
		goto drop;
	}

	frag = buf->frags;
	offset = sizeof(struct net_ipv6_hdr) + net_nbuf_ext_len(buf) +
		sizeof(struct net_icmp_hdr);

	frag = net_nbuf_read_u8(frag, offset, &offset, &hop_limit);
	frag = net_nbuf_skip(frag, offset, &offset, 1); /* flags */
	if (!frag) {
		goto drop;
	}

	if (hop_limit) {
		net_ipv6_set_hop_limit(net_nbuf_iface(buf), hop_limit);
		NET_DBG("New hop limit %d",
			net_if_ipv6_get_hop_limit(net_nbuf_iface(buf)));
	}

	frag = net_nbuf_read_be16(frag, offset, &offset, &router_lifetime);
	frag = net_nbuf_read_be32(frag, offset, &offset, &reachable_time);
	frag = net_nbuf_read_be32(frag, offset, &offset, &retrans_timer);
	if (!frag) {
		goto drop;
	}

	if (reachable_time &&
	    (net_if_ipv6_get_reachable_time(net_nbuf_iface(buf)) !=
	     NET_ICMPV6_RA_BUF(buf)->reachable_time)) {
		net_if_ipv6_set_base_reachable_time(net_nbuf_iface(buf),
						    reachable_time);

		net_if_ipv6_set_reachable_time(net_nbuf_iface(buf));
	}

	if (retrans_timer) {
		net_if_ipv6_set_retrans_timer(net_nbuf_iface(buf),
					      retrans_timer);
	}

	while (frag) {
		frag = net_nbuf_read(frag, offset, &offset, 1, &type);
		frag = net_nbuf_read(frag, offset, &offset, 1, &length);
		if (!frag) {
			goto drop;
		}

		switch (type) {
		case NET_ICMPV6_ND_OPT_SLLAO:
			frag = handle_ra_neighbor(buf, frag, length, offset,
						  &offset, &nbr);
			if (!frag && offset) {
				goto drop;
			}

			break;
		case NET_ICMPV6_ND_OPT_MTU:
			/* MTU has reserved 2 bytes, so skip it. */
			frag = net_nbuf_skip(frag, offset, &offset, 2);
			frag = net_nbuf_read_be32(frag, offset, &offset, &mtu);
			if (!frag && offset) {
				goto drop;
			}

			net_if_set_mtu(net_nbuf_iface(buf), mtu);

			if (mtu > 0xffff) {
				/* TODO: discard packet? */
				NET_ERR("MTU %u, max is %u", mtu, 0xffff);
			}

			break;
		case NET_ICMPV6_ND_OPT_PREFIX_INFO:
			frag = handle_ra_prefix(buf, frag, length, offset,
						&offset);
			if (!frag && offset) {
				goto drop;
			}

			break;
#if defined(CONFIG_NET_6LO_CONTEXT)
		case NET_ICMPV6_ND_OPT_6CO:
			/* RFC 6775, 4.2 (Length)*/
			if (!(length == 2 || length == 3)) {
				NET_ERR("Invalid 6CO length %d", length);
				goto drop;
			}

			frag = handle_ra_6co(buf, frag, length, offset,
					     &offset);
			if (!frag && offset) {
				goto drop;
			}

			break;
#endif
		case NET_ICMPV6_ND_OPT_ROUTE:
			NET_DBG("Route option (0x%x) skipped", type);
			goto skip;

#if defined(CONFIG_NET_IPV6_RA_RDNSS)
		case NET_ICMPV6_ND_OPT_RDNSS:
			NET_DBG("RDNSS option (0x%x) skipped", type);
			goto skip;
#endif

		case NET_ICMPV6_ND_OPT_DNSSL:
			NET_DBG("DNSSL option (0x%x) skipped", type);
			goto skip;

		default:
			NET_DBG("Unknown ND option 0x%x", type);
		skip:
			frag = net_nbuf_skip(frag, offset, &offset,
					     length * 8 - 2);
			if (!frag && offset) {
				goto drop;
			}

			break;
		}
	}

	router = net_if_ipv6_router_lookup(net_nbuf_iface(buf),
					   &NET_IPV6_BUF(buf)->src);
	if (router) {
		if (!router_lifetime) {
			/*TODO: Start rs_timer on iface if no routers
			 * at all available on iface.
			 */
			net_if_router_rm(router);
		} else {
			if (nbr) {
				net_ipv6_nbr_data(nbr)->is_router = true;
			}

			net_if_ipv6_router_update_lifetime(router,
							   router_lifetime);
		}
	} else {
		net_if_ipv6_router_add(net_nbuf_iface(buf),
				       &NET_IPV6_BUF(buf)->src,
				       router_lifetime);
	}

	if (nbr && net_ipv6_nbr_data(nbr)->pending) {
		NET_DBG("Sending pending buf %p to %s",
			net_ipv6_nbr_data(nbr)->pending,
			net_sprint_ipv6_addr(&NET_IPV6_BUF(
					net_ipv6_nbr_data(nbr)->pending)->dst));

		if (net_send_data(net_ipv6_nbr_data(nbr)->pending) < 0) {
			net_nbuf_unref(net_ipv6_nbr_data(nbr)->pending);
		}

		nbr_clear_ns_pending(net_ipv6_nbr_data(nbr));
	}

	/* Cancel the RS timer on iface */
	k_delayed_work_cancel(&net_nbuf_iface(buf)->rs_timer);

	net_nbuf_unref(buf);

	return NET_OK;

drop:
	net_stats_update_ipv6_nd_drop();

	return NET_DROP;
}
#endif /* CONFIG_NET_IPV6_ND */

#if defined(CONFIG_NET_IPV6_MLD)
#define MLDv2_LEN (2 + 1 + 1 + 2 + sizeof(struct in6_addr) * 2)

static struct net_buf *create_mldv2(struct net_buf *buf,
				    const struct in6_addr *addr,
				    uint16_t record_type,
				    uint8_t num_sources)
{
	net_nbuf_append_u8(buf, record_type);
	net_nbuf_append_u8(buf, 0); /* aux data len */
	net_nbuf_append_be16(buf, num_sources); /* number of addresses */
	net_nbuf_append(buf, sizeof(struct in6_addr), addr->s6_addr,
			K_FOREVER);

	if (num_sources > 0) {
		/* All source addresses, RFC 3810 ch 3 */
		net_nbuf_append(buf, sizeof(struct in6_addr),
				net_ipv6_unspecified_address()->s6_addr,
				K_FOREVER);
	}

	return buf;
}

static int send_mldv2_raw(struct net_if *iface, struct net_buf *frags)
{
	struct net_buf *buf;
	struct in6_addr dst;
	uint16_t pos;
	int ret;

	/* Sent to all MLDv2-capable routers */
	net_ipv6_addr_create(&dst, 0xff02, 0, 0, 0, 0, 0, 0, 0x0016);

	buf = net_nbuf_get_reserve_tx(net_if_get_ll_reserve(iface, &dst),
				      K_FOREVER);

	buf = net_ipv6_create_raw(buf,
				  net_if_ipv6_select_src_addr(iface, &dst),
				  &dst,
				  iface,
				  NET_IPV6_NEXTHDR_HBHO);

	NET_IPV6_BUF(buf)->hop_limit = 1; /* RFC 3810 ch 7.4 */

	/* Add hop-by-hop option and router alert option, RFC 3810 ch 5. */
	net_nbuf_append_u8(buf, IPPROTO_ICMPV6);
	net_nbuf_append_u8(buf, 0); /* length (0 means 8 bytes) */

#define ROUTER_ALERT_LEN 8

	/* IPv6 router alert option is described in RFC 2711. */
	net_nbuf_append_be16(buf, 0x0502); /* RFC 2711 ch 2.1 */
	net_nbuf_append_be16(buf, 0); /* pkt contains MLD msg */

	net_nbuf_append_u8(buf, 0); /* padding */
	net_nbuf_append_u8(buf, 0); /* padding */

	/* ICMPv6 header */
	net_nbuf_append_u8(buf, NET_ICMPV6_MLDv2); /* type */
	net_nbuf_append_u8(buf, 0); /* code */
	net_nbuf_append_be16(buf, 0); /* chksum */

	net_nbuf_set_len(buf->frags, NET_IPV6ICMPH_LEN + ROUTER_ALERT_LEN);
	net_nbuf_set_iface(buf, iface);

	net_nbuf_append_be16(buf, 0); /* reserved field */

	/* Insert the actual multicast record(s) here */
	net_buf_frag_add(buf, frags);

	ret = net_ipv6_finalize_raw(buf, NET_IPV6_NEXTHDR_HBHO);
	if (ret < 0) {
		goto drop;
	}

	net_nbuf_set_ext_len(buf, ROUTER_ALERT_LEN);

	net_nbuf_write_be16(buf, buf->frags,
			    NET_IPV6H_LEN + ROUTER_ALERT_LEN + 2,
			    &pos, ntohs(~net_calc_chksum_icmpv6(buf)));

	ret = net_send_data(buf);
	if (ret < 0) {
		goto drop;
	}

	net_stats_update_icmp_sent();
	net_stats_update_ipv6_mld_sent();

	return 0;

drop:
	net_nbuf_unref(buf);
	net_stats_update_icmp_drop();
	net_stats_update_ipv6_mld_drop();

	return ret;
}

static int send_mldv2(struct net_if *iface, const struct in6_addr *addr,
		      uint8_t mode)
{
	struct net_buf *buf;
	int ret;

	buf = net_nbuf_get_reserve_tx(net_if_get_ll_reserve(iface, NULL),
				      K_FOREVER);

	net_nbuf_append_be16(buf, 1); /* number of records */

	buf = create_mldv2(buf, addr, mode, 1);

	ret = send_mldv2_raw(iface, buf->frags);

	buf->frags = NULL;

	net_nbuf_unref(buf);

	return ret;
}

int net_ipv6_mld_join(struct net_if *iface, const struct in6_addr *addr)
{
	struct net_if_mcast_addr *maddr;
	int ret;

	maddr = net_if_ipv6_maddr_lookup(addr, &iface);
	if (maddr && net_if_ipv6_maddr_is_joined(maddr)) {
		return -EALREADY;
	}

	if (!maddr) {
		maddr = net_if_ipv6_maddr_add(iface, addr);
		if (!maddr) {
			return -ENOMEM;
		}
	}

	ret = send_mldv2(iface, addr, NET_IPV6_MLDv2_MODE_IS_EXCLUDE);
	if (ret < 0) {
		return ret;
	}

	net_if_ipv6_maddr_join(maddr);

	net_mgmt_event_notify(NET_EVENT_IPV6_MCAST_JOIN, iface);

	return ret;
}

int net_ipv6_mld_leave(struct net_if *iface, const struct in6_addr *addr)
{
	int ret;

	if (!net_if_ipv6_maddr_rm(iface, addr)) {
		return -EINVAL;
	}

	ret = send_mldv2(iface, addr, NET_IPV6_MLDv2_MODE_IS_INCLUDE);
	if (ret < 0) {
		return ret;
	}

	net_mgmt_event_notify(NET_EVENT_IPV6_MCAST_LEAVE, iface);

	return ret;
}

static void send_mld_report(struct net_if *iface)
{
	struct net_buf *buf;
	int i, count = 0;

	buf = net_nbuf_get_reserve_tx(net_if_get_ll_reserve(iface, NULL),
				      K_FOREVER);

	net_nbuf_append_u8(buf, 0); /* This will be the record count */

	for (i = 0; i < NET_IF_MAX_IPV6_MADDR; i++) {
		if (!iface->ipv6.mcast[i].is_used ||
		    !iface->ipv6.mcast[i].is_joined) {
			continue;
		}

		buf = create_mldv2(buf, &iface->ipv6.mcast[i].address.in6_addr,
				   NET_IPV6_MLDv2_MODE_IS_EXCLUDE, 0);
		count++;
	}

	if (count > 0) {
		uint16_t pos;

		/* Write back the record count */
		net_nbuf_write_u8(buf, buf->frags, 0, &pos, count);

		send_mldv2_raw(iface, buf->frags);

		buf->frags = NULL;
	}

	net_nbuf_unref(buf);
}

static enum net_verdict handle_mld_query(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct in6_addr mcast;
	uint16_t max_rsp_code, num_src, pkt_len;
	uint16_t offset, pos;
	struct net_buf *frag;

	dbg_addr_recv("Multicast Listener Query",
		      &NET_IPV6_BUF(buf)->src,
		      &NET_IPV6_BUF(buf)->dst);

	net_stats_update_ipv6_mld_recv();

	/* offset tells now where the ICMPv6 header is starting */
	offset = net_nbuf_icmp_data(buf) - net_nbuf_ip_data(buf);
	offset += sizeof(struct net_icmp_hdr);

	frag = net_nbuf_read_be16(buf->frags, offset, &pos, &max_rsp_code);
	frag = net_nbuf_skip(frag, pos, &pos, 2); /* two reserved bytes */
	frag = net_nbuf_read(frag, pos, &pos, sizeof(mcast), mcast.s6_addr);
	frag = net_nbuf_skip(frag, pos, &pos, 2); /* skip S, QRV & QQIC */
	frag = net_nbuf_read_be16(buf->frags, pos, &pos, &num_src);
	if (!frag && pos == 0xffff) {
		goto drop;
	}

	pkt_len = sizeof(struct net_ipv6_hdr) +	net_nbuf_ext_len(buf) +
		sizeof(struct net_icmp_hdr) + (2 + 2 + 16 + 2 + 2) +
		sizeof(struct in6_addr) * num_src;

	if ((total_len < pkt_len || pkt_len > NET_IPV6_MTU ||
	     (NET_ICMP_BUF(buf)->code != 0) ||
	     (NET_IPV6_BUF(buf)->hop_limit != 1))) {
		NET_DBG("Preliminary check failed %u/%u, code %u, hop %u",
			total_len, pkt_len,
			NET_ICMP_BUF(buf)->code, NET_IPV6_BUF(buf)->hop_limit);
		goto drop;
	}

	/* Currently we only support a unspecified address query. */
	if (!net_ipv6_addr_cmp(&mcast, net_ipv6_unspecified_address())) {
		NET_DBG("Only supporting unspecified address query (%s)",
			net_sprint_ipv6_addr(&mcast));
		goto drop;
	}

	send_mld_report(net_nbuf_iface(buf));

drop:
	net_stats_update_ipv6_mld_drop();

	return NET_DROP;
}

static struct net_icmpv6_handler mld_query_input_handler = {
	.type = NET_ICMPV6_MLD_QUERY,
	.code = 0,
	.handler = handle_mld_query,
};
#endif /* CONFIG_NET_IPV6_MLD */

#if defined(CONFIG_NET_IPV6_ND)
static struct net_icmpv6_handler ns_input_handler = {
	.type = NET_ICMPV6_NS,
	.code = 0,
	.handler = handle_ns_input,
};

static struct net_icmpv6_handler na_input_handler = {
	.type = NET_ICMPV6_NA,
	.code = 0,
	.handler = handle_na_input,
};

static struct net_icmpv6_handler ra_input_handler = {
	.type = NET_ICMPV6_RA,
	.code = 0,
	.handler = handle_ra_input,
};
#endif /* CONFIG_NET_IPV6_ND */

void net_ipv6_init(void)
{
#if defined(CONFIG_NET_IPV6_ND)
	net_icmpv6_register_handler(&ns_input_handler);
	net_icmpv6_register_handler(&na_input_handler);
	net_icmpv6_register_handler(&ra_input_handler);
#endif
#if defined(CONFIG_NET_IPV6_MLD)
	net_icmpv6_register_handler(&mld_query_input_handler);
#endif
}
