// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/module.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>

#include <drm/drm_fabric.h>
#include <uapi/drm/drm_fabric.h>

#include "drm_fabric_internal.h"
#include "drm_fabric_nl.h"

struct drm_fabric_dump_ctx {
	unsigned long idx;
};

static struct drm_fabric_dump_ctx *
drm_fabric_dump_context(struct netlink_callback *cb)
{
	return (struct drm_fabric_dump_ctx *)cb->ctx;
}

/* Resume cursor for the nested per-port dumps (PORT_GET, PORT_STATS_GET). */
struct drm_fabric_port_dump_ctx {
	unsigned long ep_idx;
	unsigned long port_idx;
};

static struct drm_fabric_port_dump_ctx *
drm_fabric_port_dump_context(struct netlink_callback *cb)
{
	return (struct drm_fabric_port_dump_ctx *)cb->ctx;
}

/* netnsok so a non-init_net caller gets -EPERM, not a missing family. */
static int drm_fabric_nl_host_only(const struct net *net)
{
	return net_eq(net, &init_net) ? 0 : -EPERM;
}

static int drm_fabric_fill_fabric(struct sk_buff *skb,
				  struct drm_fabric *fabric)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, DRM_FABRIC_A_FABRIC);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(skb, DRM_FABRIC_A_FABRIC_ATTRS_FABRIC_ID, fabric->id) ||
	    nla_put_u32(skb, DRM_FABRIC_A_FABRIC_ATTRS_TYPE, fabric->type) ||
	    nla_put_string(skb, DRM_FABRIC_A_FABRIC_ATTRS_NAME, fabric->name) ||
	    nla_put_u64_64bit(skb, DRM_FABRIC_A_FABRIC_ATTRS_INSTANCE_ID,
			      fabric->instance_id, DRM_FABRIC_A_FABRIC_ATTRS_PAD)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);
	return 0;
}

static int drm_fabric_fill_endpoint(struct sk_buff *skb,
				    struct drm_fabric_endpoint *ep)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, DRM_FABRIC_A_ENDPOINT);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(skb, DRM_FABRIC_A_ENDPOINT_ATTRS_ENDPOINT_ID, ep->id) ||
	    nla_put_u32(skb, DRM_FABRIC_A_ENDPOINT_ATTRS_FABRIC_ID,
			drm_fabric_endpoint_fabric_id(ep)) ||
	    nla_put_u64_64bit(skb, DRM_FABRIC_A_ENDPOINT_ATTRS_FABRIC_EP_ID,
			      ep->fabric_ep_id, DRM_FABRIC_A_ENDPOINT_ATTRS_PAD) ||
	    nla_put_string(skb, DRM_FABRIC_A_ENDPOINT_ATTRS_NAME, ep->name) ||
	    nla_put_string(skb, DRM_FABRIC_A_ENDPOINT_ATTRS_DEV_NAME,
			   dev_name(ep->parent)) ||
	    nla_put_string(skb, DRM_FABRIC_A_ENDPOINT_ATTRS_BUS_NAME,
			   dev_bus_name(ep->parent))) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);
	return 0;
}

static int drm_fabric_fill_peer(struct sk_buff *skb,
				struct drm_fabric_port *port)
{
	struct nlattr *nest;

	if (!port->has_peer)
		return 0;

	nest = nla_nest_start(skb, DRM_FABRIC_A_PORT_ATTRS_PEER);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u64_64bit(skb, DRM_FABRIC_A_PEER_ATTRS_PEER_ID,
			      port->peer.peer_id, DRM_FABRIC_A_PEER_ATTRS_PAD) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PEER_ATTRS_TYPE,
			port->peer.peer_type) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PEER_ATTRS_PORT_INDEX,
			port->peer.port_index)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);
	return 0;
}

static int drm_fabric_fill_port(struct sk_buff *skb,
				struct drm_fabric_port *port)
{
	struct nlattr *nest;
	int ret;

	nest = nla_nest_start(skb, DRM_FABRIC_A_PORT);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(skb, DRM_FABRIC_A_PORT_ATTRS_PORT_INDEX, port->index) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PORT_ATTRS_ENDPOINT_ID, port->endpoint->id) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PORT_ATTRS_OPER_STATE,
			port->oper_state) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PORT_ATTRS_MAX_LANE_COUNT,
			port->max_lane_count) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PORT_ATTRS_MAX_LANE_SIGNALING_RATE_MBPS,
			port->max_lane_signaling_rate_mbps)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	ret = drm_fabric_fill_peer(skb, port);
	if (ret) {
		nla_nest_cancel(skb, nest);
		return ret;
	}

	nla_nest_end(skb, nest);
	return 0;
}

static int drm_fabric_fill_port_stats(struct sk_buff *skb,
				      struct drm_fabric_port *port,
				      struct drm_fabric_port_stats *stats)
{
	struct nlattr *nest;

	nest = nla_nest_start(skb, DRM_FABRIC_A_PORT_STATS);
	if (!nest)
		return -EMSGSIZE;

	if (nla_put_u32(skb, DRM_FABRIC_A_PORT_STATS_ATTRS_ENDPOINT_ID,
			port->endpoint->id) ||
	    nla_put_u32(skb, DRM_FABRIC_A_PORT_STATS_ATTRS_PORT_INDEX, port->index) ||
	    nla_put_u64_64bit(skb, DRM_FABRIC_A_PORT_STATS_ATTRS_READ_BYTES,
			      stats->read_bytes, DRM_FABRIC_A_PORT_STATS_ATTRS_PAD) ||
	    nla_put_u64_64bit(skb, DRM_FABRIC_A_PORT_STATS_ATTRS_WRITE_BYTES,
			      stats->write_bytes, DRM_FABRIC_A_PORT_STATS_ATTRS_PAD) ||
	    nla_put_u64_64bit(skb, DRM_FABRIC_A_PORT_STATS_ATTRS_LINK_DOWN_COUNT,
			      stats->link_down_count, DRM_FABRIC_A_PORT_STATS_ATTRS_PAD) ||
	    nla_put_u64_64bit(skb, DRM_FABRIC_A_PORT_STATS_ATTRS_RETRAIN_COUNT,
			      stats->retrain_count, DRM_FABRIC_A_PORT_STATS_ATTRS_PAD)) {
		nla_nest_cancel(skb, nest);
		return -EMSGSIZE;
	}

	nla_nest_end(skb, nest);
	return 0;
}

int drm_fabric_nl_fabric_get_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct drm_fabric *fabric;
	struct sk_buff *msg;
	u32 fabric_id;
	void *hdr;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	if (GENL_REQ_ATTR_CHECK(info, DRM_FABRIC_A_FABRIC_ID))
		return -EINVAL;

	fabric_id = nla_get_u32(info->attrs[DRM_FABRIC_A_FABRIC_ID]);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &drm_fabric_nl_family, 0,
			  DRM_FABRIC_CMD_FABRIC_GET);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	scoped_guard(mutex, &drm_fabric_lock) {
		fabric = drm_fabric_find_by_id(fabric_id);
		if (!fabric) {
			nlmsg_free(msg);
			return -ENOENT;
		}

		ret = drm_fabric_fill_fabric(msg, fabric);
		if (ret) {
			nlmsg_free(msg);
			return ret;
		}

		if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION,
				drm_fabric_base_seq)) {
			nlmsg_free(msg);
			return -EMSGSIZE;
		}
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
}

int drm_fabric_nl_fabric_get_dumpit(struct sk_buff *skb,
				    struct netlink_callback *cb)
{
	struct drm_fabric_dump_ctx *ctx = drm_fabric_dump_context(cb);
	struct drm_fabric *fabric;
	unsigned long fidx;
	void *hdr;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(genl_info_dump(cb)));
	if (ret)
		return ret;

	scoped_guard(mutex, &drm_fabric_lock) {
		cb->seq = drm_fabric_base_seq;
		xa_for_each_start(&drm_fabric_xa, fidx, fabric, ctx->idx) {
			hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
					  cb->nlh->nlmsg_seq,
					  &drm_fabric_nl_family, NLM_F_MULTI,
					  DRM_FABRIC_CMD_FABRIC_GET);
			if (!hdr) {
				ret = -EMSGSIZE;
				break;
			}
			genl_dump_check_consistent(cb, hdr);

			if (nla_put_u32(skb, DRM_FABRIC_A_TOPOLOGY_GENERATION,
					drm_fabric_base_seq)) {
				genlmsg_cancel(skb, hdr);
				ret = -EMSGSIZE;
				break;
			}

			ret = drm_fabric_fill_fabric(skb, fabric);
			if (ret) {
				genlmsg_cancel(skb, hdr);
				break;
			}

			genlmsg_end(skb, hdr);
		}
	}

	if (ret == -EMSGSIZE) {
		ctx->idx = fidx;
		return skb->len;
	}
	return ret;
}

/* Identified by core-assigned id, or by bus + dev name. */
static struct drm_fabric_endpoint *
drm_fabric_resolve_endpoint(struct genl_info *info)
{
	struct nlattr **attrs = info->attrs;
	const char *devname = NULL;
	const char *busname = NULL;
	struct drm_fabric_endpoint *ep;

	lockdep_assert_held(&drm_fabric_lock);

	if (attrs[DRM_FABRIC_A_DEV_NAME])
		devname = nla_data(attrs[DRM_FABRIC_A_DEV_NAME]);
	if (attrs[DRM_FABRIC_A_BUS_NAME])
		busname = nla_data(attrs[DRM_FABRIC_A_BUS_NAME]);

	if (attrs[DRM_FABRIC_A_ENDPOINT_ID]) {
		u32 ep_id = nla_get_u32(attrs[DRM_FABRIC_A_ENDPOINT_ID]);

		ep = drm_fabric_endpoint_find_by_id(ep_id);
		if (!ep)
			return ERR_PTR(-ENOENT);

		if (devname && strcmp(dev_name(ep->parent), devname))
			return ERR_PTR(-EINVAL);
		if (busname && strcmp(dev_bus_name(ep->parent), busname))
			return ERR_PTR(-EINVAL);

		return ep;
	}

	if (!devname)
		return ERR_PTR(-EINVAL);

	ep = drm_fabric_endpoint_find_by_dev_name(devname, busname);
	if (IS_ERR(ep))
		return ep;
	if (!ep)
		return ERR_PTR(-ENOENT);

	return ep;
}

int drm_fabric_nl_endpoint_get_doit(struct sk_buff *skb,
				    struct genl_info *info)
{
	struct drm_fabric_endpoint *ep;
	struct sk_buff *msg;
	void *hdr;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &drm_fabric_nl_family, 0,
			  DRM_FABRIC_CMD_ENDPOINT_GET);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	scoped_guard(mutex, &drm_fabric_lock) {
		ep = drm_fabric_resolve_endpoint(info);
		if (IS_ERR(ep)) {
			nlmsg_free(msg);
			return PTR_ERR(ep);
		}

		ret = drm_fabric_fill_endpoint(msg, ep);
		if (ret) {
			nlmsg_free(msg);
			return ret;
		}

		if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION,
				drm_fabric_base_seq)) {
			nlmsg_free(msg);
			return -EMSGSIZE;
		}
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
}

int drm_fabric_nl_endpoint_get_dumpit(struct sk_buff *skb,
				      struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct drm_fabric_dump_ctx *ctx = drm_fabric_dump_context(cb);
	u32 filter_fabric_id = 0;
	bool has_filter = false;
	struct drm_fabric_endpoint *ep;
	unsigned long eidx;
	void *hdr;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	if (info->attrs[DRM_FABRIC_A_FABRIC_ID]) {
		filter_fabric_id =
			nla_get_u32(info->attrs[DRM_FABRIC_A_FABRIC_ID]);
		has_filter = true;
	}

	scoped_guard(mutex, &drm_fabric_lock) {
		cb->seq = drm_fabric_base_seq;
		xa_for_each_start(&drm_fabric_ep_xa, eidx, ep, ctx->idx) {
			if (has_filter &&
			    drm_fabric_endpoint_fabric_id(ep) != filter_fabric_id)
				continue;

			hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
					  cb->nlh->nlmsg_seq,
					  &drm_fabric_nl_family, NLM_F_MULTI,
					  DRM_FABRIC_CMD_ENDPOINT_GET);
			if (!hdr) {
				ret = -EMSGSIZE;
				break;
			}
			genl_dump_check_consistent(cb, hdr);

			if (nla_put_u32(skb, DRM_FABRIC_A_TOPOLOGY_GENERATION,
					drm_fabric_base_seq)) {
				genlmsg_cancel(skb, hdr);
				ret = -EMSGSIZE;
				break;
			}

			ret = drm_fabric_fill_endpoint(skb, ep);
			if (ret) {
				genlmsg_cancel(skb, hdr);
				break;
			}

			genlmsg_end(skb, hdr);
		}
	}

	if (ret == -EMSGSIZE) {
		ctx->idx = eidx;
		return skb->len;
	}
	return ret;
}

static int drm_fabric_port_key(struct genl_info *info, u32 *ep_id, u32 *port_idx)
{
	if (GENL_REQ_ATTR_CHECK(info, DRM_FABRIC_A_ENDPOINT_ID) ||
	    GENL_REQ_ATTR_CHECK(info, DRM_FABRIC_A_PORT_INDEX))
		return -EINVAL;

	*ep_id = nla_get_u32(info->attrs[DRM_FABRIC_A_ENDPOINT_ID]);
	*port_idx = nla_get_u32(info->attrs[DRM_FABRIC_A_PORT_INDEX]);
	return 0;
}

int drm_fabric_nl_port_get_doit(struct sk_buff *skb,
				struct genl_info *info)
{
	struct drm_fabric_port *port;
	struct sk_buff *msg;
	u32 ep_id, port_idx;
	void *hdr;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	ret = drm_fabric_port_key(info, &ep_id, &port_idx);
	if (ret)
		return ret;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &drm_fabric_nl_family, 0,
			  DRM_FABRIC_CMD_PORT_GET);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}

	scoped_guard(mutex, &drm_fabric_lock) {
		port = drm_fabric_port_find(ep_id, port_idx);
		if (!port) {
			nlmsg_free(msg);
			return -ENOENT;
		}

		ret = drm_fabric_fill_port(msg, port);
		if (ret) {
			nlmsg_free(msg);
			return ret;
		}

		if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION,
				drm_fabric_base_seq)) {
			nlmsg_free(msg);
			return -EMSGSIZE;
		}
	}

	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);
}

static int
drm_fabric_dump_endpoint_ports(struct sk_buff *skb,
			       struct netlink_callback *cb,
			       struct drm_fabric_endpoint *ep, u8 cmd,
			       int (*fill)(struct sk_buff *,
					   struct drm_fabric_port *),
			       unsigned long *s_port_idx)
{
	struct drm_fabric_port *port;
	unsigned long pidx;
	void *hdr;
	int ret = 0;

	lockdep_assert_held(&drm_fabric_lock);

	xa_for_each_start(&ep->ports, pidx, port, *s_port_idx) {
		hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
				  cb->nlh->nlmsg_seq, &drm_fabric_nl_family,
				  NLM_F_MULTI, cmd);
		if (!hdr) {
			ret = -EMSGSIZE;
			break;
		}
		genl_dump_check_consistent(cb, hdr);

		/* PORT_STATS_GET replies carry no topology-generation. */
		if (cmd == DRM_FABRIC_CMD_PORT_GET &&
		    nla_put_u32(skb, DRM_FABRIC_A_TOPOLOGY_GENERATION,
				drm_fabric_base_seq)) {
			genlmsg_cancel(skb, hdr);
			ret = -EMSGSIZE;
			break;
		}

		ret = fill(skb, port);
		if (ret) {
			genlmsg_cancel(skb, hdr);
			break;
		}

		genlmsg_end(skb, hdr);
	}

	/* Only a partially emitted endpoint keeps its port cursor. */
	*s_port_idx = (ret == -EMSGSIZE) ? pidx : 0;
	return ret;
}

static int drm_fabric_port_dump(struct sk_buff *skb,
				struct netlink_callback *cb,
				int cmd,
				int (*fill)(struct sk_buff *, struct drm_fabric_port *))
{
	const struct genl_info *info = genl_info_dump(cb);
	struct drm_fabric_port_dump_ctx *ctx = drm_fabric_port_dump_context(cb);
	u32 filter_ep_id = 0;
	bool has_ep_filter = false;
	struct drm_fabric_endpoint *ep;
	unsigned long eidx;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	if (info->attrs[DRM_FABRIC_A_ENDPOINT_ID]) {
		filter_ep_id = nla_get_u32(info->attrs[DRM_FABRIC_A_ENDPOINT_ID]);
		has_ep_filter = true;
	}

	scoped_guard(mutex, &drm_fabric_lock) {
		bool first = true;

		cb->seq = drm_fabric_base_seq;
		xa_for_each_start(&drm_fabric_ep_xa, eidx, ep, ctx->ep_idx) {
			/*
			 * port_idx belongs to ep_idx; honor it only if
			 * that endpoint still resolves.
			 */
			if (first) {
				if (eidx != ctx->ep_idx)
					ctx->port_idx = 0;
				first = false;
			}

			if (has_ep_filter && ep->id != filter_ep_id) {
				ctx->port_idx = 0;
				continue;
			}

			ret = drm_fabric_dump_endpoint_ports(skb, cb, ep,
							     cmd,
							     fill,
							     &ctx->port_idx);
			if (ret)
				break;
		}
	}

	if (ret == -EMSGSIZE) {
		ctx->ep_idx = eidx;
		return skb->len;
	}
	return ret;
}

int drm_fabric_nl_port_get_dumpit(struct sk_buff *skb,
				  struct netlink_callback *cb)
{
	return drm_fabric_port_dump(skb, cb, DRM_FABRIC_CMD_PORT_GET,
				    drm_fabric_fill_port);
}

/* Advance the cursor to the next port at or after it, or return NULL. */
static struct drm_fabric_port *
drm_fabric_stats_dump_next(unsigned long *ep_idx, unsigned long *port_idx,
			   bool has_ep_filter, u32 filter_ep_id)
{
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;
	unsigned long eidx;
	bool first = true;

	lockdep_assert_held(&drm_fabric_lock);

	xa_for_each_start(&drm_fabric_ep_xa, eidx, ep, *ep_idx) {
		if (first) {
			if (eidx != *ep_idx)
				*port_idx = 0;
			first = false;
		}

		if (has_ep_filter && ep->id != filter_ep_id) {
			*port_idx = 0;
			continue;
		}

		port = xa_find(&ep->ports, port_idx, ULONG_MAX, XA_PRESENT);
		if (port) {
			*ep_idx = eidx;
			return port;
		}

		*port_idx = 0;
	}

	/* Mark the endpoint cursor exhausted. */
	*ep_idx = ULONG_MAX;
	return NULL;
}

/*
 * Port indices are provider-chosen u32s, so port_index may be U32_MAX;
 * adding one there would wrap to 0 and re-dump the endpoint forever.
 */
static void
drm_fabric_stats_cursor_advance(unsigned long *ep_idx, unsigned long *port_idx,
				u32 port_index)
{
	if (port_index == U32_MAX) {
		(*ep_idx)++;
		*port_idx = 0;
	} else {
		*port_idx = port_index + 1;
	}
}

/* Advance past an unsupported port so a resumed dump cannot retry it. */
static void
drm_fabric_stats_dump_skip(struct sk_buff *skb, void *hdr,
			   struct drm_fabric_port_dump_ctx *ctx,
			   struct drm_fabric_port *port,
			   u32 port_index)
{
	genlmsg_cancel(skb, hdr);
	drm_fabric_port_put(port);
	drm_fabric_stats_cursor_advance(&ctx->ep_idx, &ctx->port_idx,
					port_index);
}

int drm_fabric_nl_port_stats_get_doit(struct sk_buff *skb,
				      struct genl_info *info)
{
	const struct drm_fabric_ops *ops;
	struct drm_fabric_port_stats stats = {};
	struct drm_fabric_port *port;
	struct sk_buff *msg;
	u32 ep_id, port_idx;
	void *hdr;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	ret = drm_fabric_port_key(info, &ep_id, &port_idx);
	if (ret)
		return ret;

	/*
	 * Pins the port through its endpoint across the unlocked provider
	 * callback; endpoint_unregister() waits on this pin, so it cannot be
	 * freed underneath us.
	 */
	port = drm_fabric_port_find_get(ep_id, port_idx);
	if (IS_ERR(port))
		return PTR_ERR(port);

	ops = port->endpoint->ops;
	if (!ops || !ops->port_stats_get) {
		ret = -EOPNOTSUPP;
		goto out_put;
	}

	lockdep_assert_not_held(&drm_fabric_lock);
	ret = ops->port_stats_get(port, &stats);
	if (ret)
		goto out_put;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_put;
	}

	hdr = genlmsg_put(msg, info->snd_portid, info->snd_seq,
			  &drm_fabric_nl_family, 0,
			  DRM_FABRIC_CMD_PORT_STATS_GET);
	if (!hdr) {
		ret = -EMSGSIZE;
		goto out_free;
	}

	ret = drm_fabric_fill_port_stats(msg, port, &stats);
	if (ret)
		goto out_free;

	genlmsg_end(msg, hdr);
	drm_fabric_port_put(port);
	return genlmsg_reply(msg, info);

out_free:
	nlmsg_free(msg);
out_put:
	drm_fabric_port_put(port);
	return ret;
}

/*
 * Statistics callbacks may sleep. Pin the endpoint, drop the lock, then
 * resample cb->seq so a mutation in that window sets NLM_F_DUMP_INTR.
 */
int drm_fabric_nl_port_stats_get_dumpit(struct sk_buff *skb,
					struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct drm_fabric_port_dump_ctx *ctx = drm_fabric_port_dump_context(cb);
	u32 filter_ep_id = 0;
	bool has_ep_filter = false;
	int ret;

	ret = drm_fabric_nl_host_only(genl_info_net(info));
	if (ret)
		return ret;

	if (info->attrs[DRM_FABRIC_A_ENDPOINT_ID]) {
		filter_ep_id = nla_get_u32(info->attrs[DRM_FABRIC_A_ENDPOINT_ID]);
		has_ep_filter = true;
	}

	for (;;) {
		const struct drm_fabric_ops *ops;
		struct drm_fabric_port_stats stats = {};
		struct drm_fabric_endpoint *ep;
		struct drm_fabric_port *port;
		u32 port_index;
		void *hdr;

		scoped_guard(mutex, &drm_fabric_lock) {
			/*
			 * Sampled before the cursor check so even a batch
			 * that emits nothing can report a late mutation.
			 */
			cb->seq = drm_fabric_base_seq;

			port = drm_fabric_stats_dump_next(&ctx->ep_idx,
							  &ctx->port_idx,
							  has_ep_filter,
							  filter_ep_id);
			/*
			 * Exhausted: 0 terminates the dump with NLMSG_DONE
			 * instead of re-running the parked cursor.
			 */
			if (!port)
				return 0;

			ep = port->endpoint;
			ops = ep->ops;
			port_index = port->index;
			drm_fabric_endpoint_get(ep);
		}

		/* Reserve the reply before invoking the sleeping provider callback. */
		hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
				  cb->nlh->nlmsg_seq, &drm_fabric_nl_family,
				  NLM_F_MULTI, DRM_FABRIC_CMD_PORT_STATS_GET);
		if (!hdr) {
			drm_fabric_port_put(port);
			return skb->len;
		}

		if (!ops || !ops->port_stats_get) {
			drm_fabric_stats_dump_skip(skb, hdr, ctx, port, port_index);
			continue;
		}

		lockdep_assert_not_held(&drm_fabric_lock);
		ret = ops->port_stats_get(port, &stats);
		if (ret == -EOPNOTSUPP) {
			drm_fabric_stats_dump_skip(skb, hdr, ctx, port, port_index);
			continue;
		}
		if (ret) {
			genlmsg_cancel(skb, hdr);
			drm_fabric_port_put(port);
			break;
		}

		genl_dump_check_consistent(cb, hdr);

		if (drm_fabric_fill_port_stats(skb, port, &stats)) {
			genlmsg_cancel(skb, hdr);
			drm_fabric_port_put(port);
			return skb->len;
		}

		genlmsg_end(skb, hdr);
		drm_fabric_port_put(port);

		drm_fabric_stats_cursor_advance(&ctx->ep_idx, &ctx->port_idx,
						port_index);
	}

	return ret;
}

void drm_fabric_emit_port_change(struct drm_fabric_port *port, u32 generation)
{
	struct sk_buff *msg;
	void *hdr;

	lockdep_assert_held(&drm_fabric_lock);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return;

	hdr = genlmsg_put(msg, 0, 0, &drm_fabric_nl_family, 0, DRM_FABRIC_CMD_PORT_CHANGE_NTF);
	if (!hdr) {
		nlmsg_free(msg);
		return;
	}

	if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION, generation) ||
	    drm_fabric_fill_port(msg, port)) {
		nlmsg_free(msg);
		return;
	}

	genlmsg_end(msg, hdr);
	genlmsg_multicast(&drm_fabric_nl_family, msg, 0,
			  DRM_FABRIC_NLGRP_MONITOR, GFP_KERNEL);
}

static void drm_fabric_port_peer_event_send(enum drm_fabric_cmd cmd,
					    struct drm_fabric_port *port,
					    const struct drm_fabric_peer *peer,
					    u32 generation)
{
	struct sk_buff *msg;
	void *hdr;
	struct nlattr *nest;

	lockdep_assert_held(&drm_fabric_lock);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return;

	hdr = genlmsg_put(msg, 0, 0, &drm_fabric_nl_family, 0, cmd);
	if (!hdr) {
		nlmsg_free(msg);
		return;
	}

	if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION, generation) ||
	    nla_put_u32(msg, DRM_FABRIC_A_ENDPOINT_ID, port->endpoint->id) ||
	    nla_put_u32(msg, DRM_FABRIC_A_PORT_INDEX, port->index)) {
		nlmsg_free(msg);
		return;
	}

	nest = nla_nest_start(msg, DRM_FABRIC_A_PEER);
	if (!nest) {
		nlmsg_free(msg);
		return;
	}

	if (nla_put_u64_64bit(msg, DRM_FABRIC_A_PEER_ATTRS_PEER_ID,
			      peer->peer_id, DRM_FABRIC_A_PEER_ATTRS_PAD) ||
	    nla_put_u32(msg, DRM_FABRIC_A_PEER_ATTRS_TYPE, peer->peer_type) ||
	    nla_put_u32(msg, DRM_FABRIC_A_PEER_ATTRS_PORT_INDEX, peer->port_index)) {
		nla_nest_cancel(msg, nest);
		nlmsg_free(msg);
		return;
	}

	nla_nest_end(msg, nest);

	genlmsg_end(msg, hdr);
	genlmsg_multicast(&drm_fabric_nl_family, msg, 0,
			  DRM_FABRIC_NLGRP_MONITOR, GFP_KERNEL);
}

void drm_fabric_emit_port_peer_create(struct drm_fabric_port *port,
				   const struct drm_fabric_peer *peer,
				   u32 generation)
{
	drm_fabric_port_peer_event_send(DRM_FABRIC_CMD_PORT_PEER_CREATE_NTF,
					port, peer, generation);
}

void drm_fabric_emit_port_peer_delete(struct drm_fabric_port *port,
				   const struct drm_fabric_peer *peer,
				   u32 generation)
{
	drm_fabric_port_peer_event_send(DRM_FABRIC_CMD_PORT_PEER_DELETE_NTF,
					port, peer, generation);
}

static void drm_fabric_endpoint_event_send(enum drm_fabric_cmd cmd,
					   struct drm_fabric_endpoint *ep,
					   u32 generation)
{
	struct sk_buff *msg;
	void *hdr;

	lockdep_assert_held(&drm_fabric_lock);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return;

	hdr = genlmsg_put(msg, 0, 0, &drm_fabric_nl_family, 0, cmd);
	if (!hdr) {
		nlmsg_free(msg);
		return;
	}

	if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION, generation) ||
	    drm_fabric_fill_endpoint(msg, ep)) {
		nlmsg_free(msg);
		return;
	}

	genlmsg_end(msg, hdr);
	genlmsg_multicast(&drm_fabric_nl_family, msg, 0,
			  DRM_FABRIC_NLGRP_MONITOR, GFP_KERNEL);
}

void drm_fabric_emit_endpoint_create(struct drm_fabric_endpoint *ep, u32 generation)
{
	drm_fabric_endpoint_event_send(DRM_FABRIC_CMD_ENDPOINT_CREATE_NTF, ep,
				       generation);
}

void drm_fabric_emit_endpoint_delete(struct drm_fabric_endpoint *ep, u32 generation)
{
	drm_fabric_endpoint_event_send(DRM_FABRIC_CMD_ENDPOINT_DELETE_NTF, ep,
				       generation);
}

static void drm_fabric_fabric_event_send(enum drm_fabric_cmd cmd,
					 struct drm_fabric *fabric,
					 u32 generation)
{
	struct sk_buff *msg;
	void *hdr;

	lockdep_assert_held(&drm_fabric_lock);

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return;

	hdr = genlmsg_put(msg, 0, 0, &drm_fabric_nl_family, 0, cmd);
	if (!hdr) {
		nlmsg_free(msg);
		return;
	}

	if (nla_put_u32(msg, DRM_FABRIC_A_TOPOLOGY_GENERATION, generation) ||
	    drm_fabric_fill_fabric(msg, fabric)) {
		nlmsg_free(msg);
		return;
	}

	genlmsg_end(msg, hdr);
	genlmsg_multicast(&drm_fabric_nl_family, msg, 0,
			  DRM_FABRIC_NLGRP_MONITOR, GFP_KERNEL);
}

void drm_fabric_emit_fabric_create(struct drm_fabric *fabric, u32 generation)
{
	drm_fabric_fabric_event_send(DRM_FABRIC_CMD_FABRIC_CREATE_NTF, fabric,
				     generation);
}

void drm_fabric_emit_fabric_delete(struct drm_fabric *fabric, u32 generation)
{
	drm_fabric_fabric_event_send(DRM_FABRIC_CMD_FABRIC_DELETE_NTF, fabric,
				     generation);
}

int drm_fabric_netlink_register(void)
{
	return genl_register_family(&drm_fabric_nl_family);
}

void drm_fabric_netlink_unregister(void)
{
	genl_unregister_family(&drm_fabric_nl_family);
}
