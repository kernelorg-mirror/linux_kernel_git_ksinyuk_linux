/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */

/*
 * DRM Fabric driver API: common object model for GPU interconnect topology
 * (fabric, endpoint, port and peer relationships).
 */

#ifndef __DRM_FABRIC_H__
#define __DRM_FABRIC_H__

#include <linux/completion.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/xarray.h>

enum drm_fabric_type {
	/* Zero is invalid; concrete fabric types start at 1. */
	DRM_FABRIC_TYPE_SYNTHETIC = 1,
};

enum drm_fabric_port_state {
	DRM_FABRIC_PORT_STATE_UNKNOWN,
	DRM_FABRIC_PORT_STATE_INACTIVE,
	DRM_FABRIC_PORT_STATE_ACTIVE,
	DRM_FABRIC_PORT_STATE_DEGRADED,
};

enum drm_fabric_peer_type {
	DRM_FABRIC_PEER_TYPE_ACCEL = 1,
	DRM_FABRIC_PEER_TYPE_SWITCH,
};

struct device;

/**
 * struct drm_fabric_port_desc - Port descriptor for drm_fabric_endpoint_register()
 */
struct drm_fabric_port_desc {
	/** @index: per-endpoint port index */
	u32 index;
	/** @max_lane_count: maximum provider-reported link width capability, 0 if not reported */
	u32 max_lane_count;
	/**
	 * @max_lane_signaling_rate_mbps: maximum provider-reported per-lane
	 * signaling rate in decimal megabits per second before encoding, FEC
	 * and protocol overhead, 0 if not reported. A capability, not the
	 * negotiated rate: multiplying it by @max_lane_count does not give
	 * usable bandwidth.
	 */
	u32 max_lane_signaling_rate_mbps;
};

/**
 * struct drm_fabric_endpoint_desc - Endpoint descriptor for drm_fabric_endpoint_register()
 */
struct drm_fabric_endpoint_desc {
	/**
	 * @fabric_ep_id: accelerator identity within the fabric's identity
	 * domain, unique among its members and stable for the duration of
	 * membership; a duplicate is rejected with -EEXIST. Distinct from the
	 * core-assigned &drm_fabric_endpoint.id.
	 */
	u64 fabric_ep_id;
	/** @name: human-readable endpoint name */
	const char *name;
	/** @parent: required backing device, provides dev_name and bus */
	struct device *parent;
	/** @ops: provider driver callbacks */
	const struct drm_fabric_ops *ops;
	/**
	 * @priv: provider cookie reachable from every @ops callback via @ep
	 * or @port. The core stores it, never dereferences it, never frees it.
	 */
	void *priv;

	/** @ports: fixed set of ports, copied by the core */
	const struct drm_fabric_port_desc *ports;
	/** @num_ports: number of entries in @ports */
	unsigned int num_ports;
};

/**
 * struct drm_fabric_desc - Fabric descriptor for drm_fabric_register()
 */
struct drm_fabric_desc {
	/** @type: fabric interconnect technology */
	enum drm_fabric_type type;
	/** @name: human-readable fabric name */
	const char *name;
	/** @instance_id: vendor-unique instance identifier */
	u64 instance_id;
};

/**
 * struct drm_fabric - Fabric object
 */
struct drm_fabric {
	/** @id: kernel-local identifier, assigned by the core */
	u32 id;
	/** @type: fabric interconnect technology */
	enum drm_fabric_type type;
	/** @instance_id: vendor-unique identifier within @type */
	u64 instance_id;
	/** @name: human-readable fabric name */
	char name[32];

	/** @refs: reference count */
	refcount_t refs;
};

/**
 * struct drm_fabric_endpoint - Endpoint object
 */
struct drm_fabric_endpoint {
	/** @id: kernel-local identifier, assigned by the core */
	u32 id;
	/** @fabric_ep_id: provider's stable fabric-local identity */
	u64 fabric_ep_id;
	/** @name: human-readable endpoint name */
	char name[32];
	/** @parent: backing device, provides dev_name and bus_name */
	struct device *parent;

	/** @fabric: parent fabric */
	struct drm_fabric *fabric;

	/** @ops: provider driver callbacks */
	const struct drm_fabric_ops *ops;
	/** @priv: provider cookie, as supplied at registration */
	void *priv;

	/** @ports: xarray of &struct drm_fabric_port owned by this endpoint */
	struct xarray ports;
	/** @num_ports: number of ports in @ports */
	unsigned int num_ports;

	/** @refs: reference count */
	refcount_t refs;
	/** @unregistered: completed once the endpoint is fully unregistered */
	struct completion unregistered;
};

/**
 * drm_fabric_endpoint_fabric_id() - Wire fabric-id for an endpoint
 * @ep: endpoint to query
 *
 * Return: the parent fabric id.
 */
static inline u32
drm_fabric_endpoint_fabric_id(const struct drm_fabric_endpoint *ep)
{
	return ep->fabric->id;
}

/**
 * struct drm_fabric_peer - Directly adjacent far-end identity
 *
 * A value descriptor, not a reference to a live object. While its owning port
 * remains registered, it is retained until explicitly retracted even if the
 * object it names stops resolving locally. See "Peer semantics" in
 * Documentation/gpu/drm-fabric.rst.
 */
struct drm_fabric_peer {
	/**
	 * @peer_id: identity of the far-end object, read per @peer_type: an
	 * accelerator's fabric_ep_id, or an opaque switch identity. The two
	 * are separate namespaces, so one value names different objects
	 * under each type, and neither has to resolve locally.
	 */
	u64 peer_id;

	/** @peer_type: kind of far-end device; selects the @peer_id namespace */
	enum drm_fabric_peer_type peer_type;
	/** @port_index: far-end port index within the object @peer_id names */
	u32 port_index;
};

/**
 * struct drm_fabric_port - Port object
 */
struct drm_fabric_port {
	/** @index: per-endpoint port index */
	u32 index;
	/** @oper_state: operational (link) state */
	enum drm_fabric_port_state oper_state;
	/** @max_lane_count: as in &struct drm_fabric_port_desc */
	u32 max_lane_count;
	/** @max_lane_signaling_rate_mbps: as in &struct drm_fabric_port_desc */
	u32 max_lane_signaling_rate_mbps;

	/** @has_peer: whether @peer holds a valid descriptor */
	bool has_peer;
	/** @peer: neighbor description, valid only while @has_peer is set */
	struct drm_fabric_peer peer;

	/** @endpoint: parent endpoint */
	struct drm_fabric_endpoint *endpoint;
};

/**
 * struct drm_fabric_port_stats - Per-port statistics for the port_stats_get() callback
 *
 * Counters are monotonic for the lifetime of the provider's registration and
 * are not clearable through this uAPI. Link error accounting belongs to DRM
 * RAS and is deliberately absent here.
 */
struct drm_fabric_port_stats {
	/** @read_bytes: bytes received on the port */
	u64 read_bytes;
	/** @write_bytes: bytes transmitted on the port */
	u64 write_bytes;
	/** @link_down_count: link-down transitions */
	u64 link_down_count;
	/** @retrain_count: link retrain events */
	u64 retrain_count;
};

/**
 * struct drm_fabric_ops - Provider driver callbacks
 *
 * A callback that is not supplied makes the matching netlink operation return
 * -EOPNOTSUPP.
 */
struct drm_fabric_ops {
	/**
	 * @port_stats_get: read per-port statistics. The core pins the port
	 * through its endpoint and calls without drm_fabric_lock held, so this
	 * may sleep. It must not re-enter a core API that takes the lock or take
	 * a provider lock from which the core may be called.
	 *
	 * Populate every field or return -EOPNOTSUPP. Zero is a valid count,
	 * not an unsupported marker.
	 */
	int (*port_stats_get)(struct drm_fabric_port *port,
			      struct drm_fabric_port_stats *stats);
};

struct drm_fabric *drm_fabric_register(const struct drm_fabric_desc *desc);
int drm_fabric_unregister(struct drm_fabric *fabric);

struct drm_fabric_endpoint *
drm_fabric_endpoint_register(struct drm_fabric *fabric,
			     const struct drm_fabric_endpoint_desc *desc);
void drm_fabric_endpoint_unregister(struct drm_fabric_endpoint *ep);

struct drm_fabric_port *
drm_fabric_endpoint_port(struct drm_fabric_endpoint *ep, u32 port_index);

int drm_fabric_port_set_peer(struct drm_fabric_port *port,
			     const struct drm_fabric_peer *peer);
int drm_fabric_port_unset_peer(struct drm_fabric_port *port);

void drm_fabric_port_set_oper(struct drm_fabric_port *port,
			      enum drm_fabric_port_state state);

#endif /* __DRM_FABRIC_H__ */
