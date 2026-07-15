// SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause)
/* Do not edit directly, auto-generated from: */
/*	Documentation/netlink/specs/drm_fabric.yaml */
/* YNL-GEN kernel source */
/* To regenerate run: tools/net/ynl/ynl-regen.sh */

#include <net/netlink.h>
#include <net/genetlink.h>

#include "drm_fabric_nl.h"

#include <uapi/drm/drm_fabric.h>

/* DRM_FABRIC_CMD_FABRIC_GET - do */
static const struct nla_policy drm_fabric_fabric_get_nl_policy[DRM_FABRIC_A_FABRIC_ID + 1] = {
	[DRM_FABRIC_A_FABRIC_ID] = { .type = NLA_U32, },
};

/* DRM_FABRIC_CMD_ENDPOINT_GET - do */
static const struct nla_policy drm_fabric_endpoint_get_do_nl_policy[DRM_FABRIC_A_BUS_NAME + 1] = {
	[DRM_FABRIC_A_ENDPOINT_ID] = { .type = NLA_U32, },
	[DRM_FABRIC_A_DEV_NAME] = { .type = NLA_NUL_STRING, },
	[DRM_FABRIC_A_BUS_NAME] = { .type = NLA_NUL_STRING, },
};

/* DRM_FABRIC_CMD_ENDPOINT_GET - dump */
static const struct nla_policy drm_fabric_endpoint_get_dump_nl_policy[DRM_FABRIC_A_FABRIC_ID + 1] = {
	[DRM_FABRIC_A_FABRIC_ID] = { .type = NLA_U32, },
};

/* DRM_FABRIC_CMD_PORT_GET - do */
static const struct nla_policy drm_fabric_port_get_do_nl_policy[DRM_FABRIC_A_PORT_INDEX + 1] = {
	[DRM_FABRIC_A_ENDPOINT_ID] = { .type = NLA_U32, },
	[DRM_FABRIC_A_PORT_INDEX] = { .type = NLA_U32, },
};

/* DRM_FABRIC_CMD_PORT_GET - dump */
static const struct nla_policy drm_fabric_port_get_dump_nl_policy[DRM_FABRIC_A_ENDPOINT_ID + 1] = {
	[DRM_FABRIC_A_ENDPOINT_ID] = { .type = NLA_U32, },
};

/* DRM_FABRIC_CMD_PORT_STATS_GET - do */
static const struct nla_policy drm_fabric_port_stats_get_do_nl_policy[DRM_FABRIC_A_PORT_INDEX + 1] = {
	[DRM_FABRIC_A_ENDPOINT_ID] = { .type = NLA_U32, },
	[DRM_FABRIC_A_PORT_INDEX] = { .type = NLA_U32, },
};

/* DRM_FABRIC_CMD_PORT_STATS_GET - dump */
static const struct nla_policy drm_fabric_port_stats_get_dump_nl_policy[DRM_FABRIC_A_ENDPOINT_ID + 1] = {
	[DRM_FABRIC_A_ENDPOINT_ID] = { .type = NLA_U32, },
};

/* Ops table for drm_fabric */
static const struct genl_split_ops drm_fabric_nl_ops[] = {
	{
		.cmd		= DRM_FABRIC_CMD_FABRIC_GET,
		.doit		= drm_fabric_nl_fabric_get_doit,
		.policy		= drm_fabric_fabric_get_nl_policy,
		.maxattr	= DRM_FABRIC_A_FABRIC_ID,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd	= DRM_FABRIC_CMD_FABRIC_GET,
		.dumpit	= drm_fabric_nl_fabric_get_dumpit,
		.flags	= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRM_FABRIC_CMD_ENDPOINT_GET,
		.doit		= drm_fabric_nl_endpoint_get_doit,
		.policy		= drm_fabric_endpoint_get_do_nl_policy,
		.maxattr	= DRM_FABRIC_A_BUS_NAME,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRM_FABRIC_CMD_ENDPOINT_GET,
		.dumpit		= drm_fabric_nl_endpoint_get_dumpit,
		.policy		= drm_fabric_endpoint_get_dump_nl_policy,
		.maxattr	= DRM_FABRIC_A_FABRIC_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRM_FABRIC_CMD_PORT_GET,
		.doit		= drm_fabric_nl_port_get_doit,
		.policy		= drm_fabric_port_get_do_nl_policy,
		.maxattr	= DRM_FABRIC_A_PORT_INDEX,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRM_FABRIC_CMD_PORT_GET,
		.dumpit		= drm_fabric_nl_port_get_dumpit,
		.policy		= drm_fabric_port_get_dump_nl_policy,
		.maxattr	= DRM_FABRIC_A_ENDPOINT_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
	{
		.cmd		= DRM_FABRIC_CMD_PORT_STATS_GET,
		.doit		= drm_fabric_nl_port_stats_get_doit,
		.policy		= drm_fabric_port_stats_get_do_nl_policy,
		.maxattr	= DRM_FABRIC_A_PORT_INDEX,
		.flags		= GENL_CMD_CAP_DO,
	},
	{
		.cmd		= DRM_FABRIC_CMD_PORT_STATS_GET,
		.dumpit		= drm_fabric_nl_port_stats_get_dumpit,
		.policy		= drm_fabric_port_stats_get_dump_nl_policy,
		.maxattr	= DRM_FABRIC_A_ENDPOINT_ID,
		.flags		= GENL_CMD_CAP_DUMP,
	},
};

static const struct genl_multicast_group drm_fabric_nl_mcgrps[] = {
	[DRM_FABRIC_NLGRP_MONITOR] = { "monitor", },
};

struct genl_family drm_fabric_nl_family __ro_after_init = {
	.name		= DRM_FABRIC_FAMILY_NAME,
	.version	= DRM_FABRIC_FAMILY_VERSION,
	.netnsok	= true,
	.parallel_ops	= true,
	.module		= THIS_MODULE,
	.split_ops	= drm_fabric_nl_ops,
	.n_split_ops	= ARRAY_SIZE(drm_fabric_nl_ops),
	.mcgrps		= drm_fabric_nl_mcgrps,
	.n_mcgrps	= ARRAY_SIZE(drm_fabric_nl_mcgrps),
};
