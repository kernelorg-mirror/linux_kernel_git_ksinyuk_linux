/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

#ifndef __DRM_FABRIC_INTERNAL_H__
#define __DRM_FABRIC_INTERNAL_H__

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/drm_fabric.h>

extern struct mutex drm_fabric_lock;
extern struct xarray drm_fabric_xa;    /* Fabric registry */
extern struct xarray drm_fabric_ep_xa; /* Endpoint registry */

extern u32 drm_fabric_base_seq;        /* Dump consistency sequence */

int drm_fabric_netlink_register(void);
void drm_fabric_netlink_unregister(void);

struct drm_fabric *drm_fabric_find_by_id(u32 id);
struct drm_fabric_endpoint *drm_fabric_endpoint_find_by_id(u32 id);
struct drm_fabric_endpoint *
drm_fabric_endpoint_find_by_dev_name(const char *devname, const char *busname);
struct drm_fabric_port *drm_fabric_port_find(u32 ep_id, u32 port_idx);

struct drm_fabric_port *drm_fabric_port_find_get(u32 ep_id, u32 port_idx);
void drm_fabric_port_put(struct drm_fabric_port *port);
struct drm_fabric_endpoint *drm_fabric_endpoint_get(struct drm_fabric_endpoint *ep);
void drm_fabric_endpoint_put(struct drm_fabric_endpoint *ep);
struct drm_fabric *drm_fabric_get(struct drm_fabric *fabric);
void drm_fabric_put(struct drm_fabric *fabric);

/* Emits use the post-change generation. */
void drm_fabric_emit_endpoint_create(struct drm_fabric_endpoint *ep, u32 generation);
void drm_fabric_emit_endpoint_delete(struct drm_fabric_endpoint *ep, u32 generation);

void drm_fabric_emit_port_peer_create(struct drm_fabric_port *port,
				      const struct drm_fabric_peer *peer,
				      u32 generation);
void drm_fabric_emit_port_peer_delete(struct drm_fabric_port *port,
				      const struct drm_fabric_peer *peer,
				      u32 generation);

void drm_fabric_emit_port_change(struct drm_fabric_port *port, u32 generation);

void drm_fabric_emit_fabric_create(struct drm_fabric *fabric, u32 generation);
void drm_fabric_emit_fabric_delete(struct drm_fabric *fabric, u32 generation);

#endif /* __DRM_FABRIC_INTERNAL_H__ */
