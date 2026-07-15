/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/drm_fabric.yaml */
/* YNL-GEN kernel header */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#ifndef _LINUX_DRM_FABRIC_GEN_H
#define _LINUX_DRM_FABRIC_GEN_H

#include <net/netlink.h>
#include <net/genetlink.h>

#include <uapi/drm/drm_fabric.h>

int drm_fabric_nl_fabric_get_doit(struct sk_buff *skb, struct genl_info *info);
int drm_fabric_nl_fabric_get_dumpit(struct sk_buff *skb,
				    struct netlink_callback *cb);
int drm_fabric_nl_endpoint_get_doit(struct sk_buff *skb,
				    struct genl_info *info);
int drm_fabric_nl_endpoint_get_dumpit(struct sk_buff *skb,
				      struct netlink_callback *cb);
int drm_fabric_nl_port_get_doit(struct sk_buff *skb, struct genl_info *info);
int drm_fabric_nl_port_get_dumpit(struct sk_buff *skb,
				  struct netlink_callback *cb);
int drm_fabric_nl_port_stats_get_doit(struct sk_buff *skb,
				      struct genl_info *info);
int drm_fabric_nl_port_stats_get_dumpit(struct sk_buff *skb,
					struct netlink_callback *cb);

enum {
	DRM_FABRIC_NLGRP_MONITOR,
};

extern struct genl_family drm_fabric_nl_family;

#endif /* _LINUX_DRM_FABRIC_GEN_H */
