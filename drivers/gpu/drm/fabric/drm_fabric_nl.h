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

/* Common nested types */
extern const struct nla_policy drm_fabric_fabric_new_params_nl_policy[DRM_FABRIC_A_FABRIC_NEW_PARAMS_INSTANCE_ID + 1];
extern const struct nla_policy drm_fabric_peer_nl_policy[DRM_FABRIC_A_PEER_ATTRS_PORT_INDEX + 1];

int drm_fabric_nl_pre_doit(const struct genl_split_ops *ops,
			   struct sk_buff *skb, struct genl_info *info);
int drm_fabric_nl_endpoint_pre_doit(const struct genl_split_ops *ops,
				    struct sk_buff *skb,
				    struct genl_info *info);
int drm_fabric_nl_port_pre_doit(const struct genl_split_ops *ops,
				struct sk_buff *skb, struct genl_info *info);
void
drm_fabric_nl_post_doit(const struct genl_split_ops *ops, struct sk_buff *skb,
			struct genl_info *info);
void
drm_fabric_nl_endpoint_post_doit(const struct genl_split_ops *ops,
				 struct sk_buff *skb, struct genl_info *info);
void
drm_fabric_nl_port_post_doit(const struct genl_split_ops *ops,
			     struct sk_buff *skb, struct genl_info *info);

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
int drm_fabric_nl_fabric_new_doit(struct sk_buff *skb, struct genl_info *info);
int drm_fabric_nl_fabric_del_doit(struct sk_buff *skb, struct genl_info *info);
int drm_fabric_nl_endpoint_set_doit(struct sk_buff *skb,
				    struct genl_info *info);
int drm_fabric_nl_port_set_doit(struct sk_buff *skb, struct genl_info *info);
int drm_fabric_nl_port_peer_new_doit(struct sk_buff *skb,
				     struct genl_info *info);
int drm_fabric_nl_port_peer_del_doit(struct sk_buff *skb,
				     struct genl_info *info);

enum {
	DRM_FABRIC_NLGRP_MONITOR,
};

extern struct genl_family drm_fabric_nl_family;

#endif /* _LINUX_DRM_FABRIC_GEN_H */
