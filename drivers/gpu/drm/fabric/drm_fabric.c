// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_fabric.h>

#include "drm_fabric_internal.h"

/**
 * DOC: DRM Fabric core
 *
 * The core keeps a registry of fabric, endpoint, port and peer objects behind
 * a small provider-facing API. A provider registers a fabric, attaches
 * endpoints with their fixed set of ports, then reports link changes and peer
 * adjacency through drm_fabric_port_set_oper(), drm_fabric_port_set_peer() and
 * drm_fabric_port_unset_peer().
 *
 * The core owns object identity and lifetime: it assigns kernel-local IDs,
 * refcounts objects, serialises object state under drm_fabric_lock and
 * topology mutation under drm_fabric_mutation_lock, and advances a topology
 * generation on every change so a concurrent netlink dump can detect a torn
 * snapshot. See Documentation/gpu/drm-fabric.rst for the fuller object
 * lifetime and locking treatment.
 */

/* Global lock for all fabric, endpoint and port state. */
DEFINE_MUTEX(drm_fabric_lock);

/*
 * Lock order is mutation_lock -> drm_fabric_lock; a provider callback already
 * holds this and must not re-enter registration/unregistration.
 */
DEFINE_MUTEX(drm_fabric_mutation_lock);

/* ID 0 is the orphan sentinel, so allocated IDs start at 1. */
DEFINE_XARRAY_ALLOC1(drm_fabric_xa);
DEFINE_XARRAY_ALLOC(drm_fabric_ep_xa);

/*
 * Family-global topology change token, exposed to user space as
 * topology-generation and used as the netlink dump-consistency sequence. It is
 * nonzero and never emits zero across the u32 wrap. Increment it on a
 * committed topology mutation so a subsequent GET/DUMP reports the
 * post-change generation.
 */
u32 drm_fabric_base_seq = 1;

static u32 drm_fabric_base_seq_inc(void)
{
	lockdep_assert_held(&drm_fabric_lock);
	/*
	 * Generation feeds cb->seq; netlink treats 0 as "no dump in progress",
	 * so skip it on wrap.
	 */
	if (++drm_fabric_base_seq == 0)
		drm_fabric_base_seq = 1;
	return drm_fabric_base_seq;
}

struct drm_fabric *drm_fabric_find_by_id(u32 id)
{
	lockdep_assert_held(&drm_fabric_lock);
	return xa_load(&drm_fabric_xa, id);
}

struct drm_fabric_endpoint *drm_fabric_endpoint_find_by_id(u32 id)
{
	lockdep_assert_held(&drm_fabric_lock);
	return xa_load(&drm_fabric_ep_xa, id);
}

/*
 * Returns NULL if no endpoint matches, or ERR_PTR(-EINVAL) if @devname is
 * ambiguous across buses and @busname does not disambiguate it.
 */
struct drm_fabric_endpoint *
drm_fabric_endpoint_find_by_dev_name(const char *devname, const char *busname)
{
	struct drm_fabric_endpoint *match = NULL;
	struct drm_fabric_endpoint *ep;
	unsigned long idx;

	lockdep_assert_held(&drm_fabric_lock);
	xa_for_each(&drm_fabric_ep_xa, idx, ep) {
		if (strcmp(dev_name(ep->parent), devname))
			continue;

		if (busname) {
			if (strcmp(dev_bus_name(ep->parent), busname))
				continue;
			return ep;
		}

		if (match)
			return ERR_PTR(-EINVAL);

		match = ep;
	}

	return match;
}

/*
 * fabric_ep_id must be unique per fabric or peer resolution is ambiguous;
 * orphans (@fabric == NULL) do not participate.
 */
static bool drm_fabric_ep_id_in_use(const struct drm_fabric *fabric, u64 fabric_ep_id)
{
	struct drm_fabric_endpoint *ep;
	unsigned long idx;

	lockdep_assert_held(&drm_fabric_lock);

	if (!fabric)
		return false;

	xa_for_each(&drm_fabric_ep_xa, idx, ep)
		if (ep->fabric == fabric &&
		    ep->fabric_ep_id == fabric_ep_id)
			return true;

	return false;
}

static bool drm_fabric_has_instance(const struct drm_fabric *fabric)
{
	struct drm_fabric *other;
	unsigned long idx;

	lockdep_assert_held(&drm_fabric_lock);

	xa_for_each(&drm_fabric_xa, idx, other)
		if (other->type == fabric->type &&
		    other->instance_id == fabric->instance_id)
			return true;

	return false;
}

/* @owner is set once here, before publication, so no reader needs a lock for it. */
static struct drm_fabric *
__drm_fabric_register(const struct drm_fabric_desc *desc,
		      enum drm_fabric_owner owner)
{
	struct drm_fabric *fabric __free(kfree) = NULL;

	fabric = kzalloc_obj(*fabric);
	if (!fabric)
		return ERR_PTR(-ENOMEM);

	fabric->type = desc->type;
	fabric->instance_id = desc->instance_id;
	fabric->owner = owner;
	refcount_set(&fabric->refs, 1);

	if (desc->name &&
	    strscpy(fabric->name, desc->name, sizeof(fabric->name)) < 0)
		return ERR_PTR(-ENAMETOOLONG);

	scoped_guard(mutex, &drm_fabric_lock) {
		int ret;

		if (drm_fabric_has_instance(fabric))
			return ERR_PTR(-EEXIST);

		ret = xa_alloc(&drm_fabric_xa, &fabric->id, fabric,
			       xa_limit_32b, GFP_KERNEL);
		if (ret)
			return ERR_PTR(ret);
		drm_fabric_emit_fabric_create(fabric, drm_fabric_base_seq_inc());
	}

	return_ptr(fabric);
}

static bool drm_fabric_type_valid(enum drm_fabric_type type)
{
	switch (type) {
	case DRM_FABRIC_TYPE_SYNTHETIC:
		return true;
	}

	return false;
}

/**
 * drm_fabric_register() - Register a new fabric
 * @desc: fabric description (type, instance id, name)
 *
 * Allocates the fabric and inserts it into the registry as provider-owned.
 * Rejects zero and out-of-range types before allocating.
 *
 * Context: May sleep. Acquires drm_fabric_lock.
 * Return: the registered fabric, or an ERR_PTR() on failure, -EINVAL for a
 *         type this kernel does not define.
 */
struct drm_fabric *drm_fabric_register(const struct drm_fabric_desc *desc)
{
	if (WARN_ON_ONCE(!drm_fabric_type_valid(desc->type)))
		return ERR_PTR(-EINVAL);

	return __drm_fabric_register(desc, DRM_FABRIC_OWNER_PROVIDER);
}
EXPORT_SYMBOL(drm_fabric_register);

struct drm_fabric *drm_fabric_get(struct drm_fabric *fabric)
{
	lockdep_assert_held(&drm_fabric_lock);
	refcount_inc(&fabric->refs);
	return fabric;
}

void drm_fabric_put(struct drm_fabric *fabric)
{
	if (refcount_dec_and_test(&fabric->refs))
		kfree(fabric);
}

static bool drm_fabric_has_members(const struct drm_fabric *fabric)
{
	struct drm_fabric_endpoint *ep;
	unsigned long idx;

	lockdep_assert_held(&drm_fabric_lock);

	xa_for_each(&drm_fabric_ep_xa, idx, ep)
		if (ep->fabric == fabric)
			return true;

	return false;
}

/*
 * Compare the possibly stale pointer by address without dereferencing it.
 * A reused address passes as the later object; providers own incarnation
 * tracking.
 */
static bool drm_fabric_is_registered(const struct drm_fabric *fabric)
{
	struct drm_fabric *entry;
	unsigned long idx;

	lockdep_assert_held(&drm_fabric_lock);

	xa_for_each(&drm_fabric_xa, idx, entry)
		if (entry == fabric)
			return true;

	return false;
}

/**
 * drm_fabric_unregister() - Unregister a fabric
 * @fabric: fabric to remove
 *
 * Removes an empty @fabric from the registry and frees it. A provider must
 * unregister all member endpoints first.
 *
 * @fabric must still be registered. The pointer is compared against the
 * registry before anything dereferences it, so a stale or repeated
 * unregister is rejected rather than acted on. That comparison proves
 * current address membership only: it cannot tell an earlier incarnation
 * from another fabric registered later at the same address, which remains
 * the provider's obligation.
 *
 * Context: May sleep. Acquires drm_fabric_mutation_lock, then drm_fabric_lock.
 *          Must not be called from a provider mutation callback (endpoint_set /
 *          port_set / port_peer_*), which already holds the mutation lock and
 *          would self-deadlock.
 * Return: 0 once the fabric is removed. -ENODEV if @fabric is not currently
 *         registered, which takes precedence over the member check. -EBUSY
 *         if member endpoints remain, in which case the fabric stays fully
 *         registered and the provider must unregister the members before
 *         retrying -- a non-empty unregister is a provider teardown-ordering
 *         bug, so it also warns.
 */
int drm_fabric_unregister(struct drm_fabric *fabric)
{
	/*
	 * The mutation lock below blocks a racing unregister or endpoint_set attach
	 * on this fabric.
	 */
	lockdep_assert_not_held(&drm_fabric_mutation_lock);

	scoped_guard(mutex, &drm_fabric_mutation_lock) {
		scoped_guard(mutex, &drm_fabric_lock) {
			if (!drm_fabric_is_registered(fabric))
				return -ENODEV;
			/*
			 * Members still reference ep->fabric; freeing it here
			 * would leave stale pointers.
			 */
			if (WARN_ON_ONCE(drm_fabric_has_members(fabric)))
				return -EBUSY;
			/* Emit before the erase, while @fabric is still live. */
			drm_fabric_emit_fabric_delete(fabric,
						      drm_fabric_base_seq_inc());
			xa_erase(&drm_fabric_xa, fabric->id);
		}
	}

	drm_fabric_put(fabric);
	return 0;
}
EXPORT_SYMBOL(drm_fabric_unregister);

static void drm_fabric_ports_destroy(struct drm_fabric_endpoint *ep)
{
	struct drm_fabric_port *port;
	unsigned long index;

	xa_for_each(&ep->ports, index, port) {
		kfree(port);
		ep->num_ports--;
	}
	xa_destroy(&ep->ports);
}

static int drm_fabric_ports_create(struct drm_fabric_endpoint *ep,
				   const struct drm_fabric_port_desc *descs,
				   unsigned int num_ports)
{
	unsigned int i;

	for (i = 0; i < num_ports; i++) {
		struct drm_fabric_port *port;
		int ret;

		port = kzalloc_obj(*port);
		if (!port) {
			drm_fabric_ports_destroy(ep);
			return -ENOMEM;
		}

		port->index = descs[i].index;
		port->max_lane_count = descs[i].max_lane_count;
		port->max_lane_signaling_rate_mbps =
			descs[i].max_lane_signaling_rate_mbps;
		port->oper_state = DRM_FABRIC_PORT_STATE_UNKNOWN;
		port->admin_state = DRM_FABRIC_ADMIN_STATE_DOWN;
		port->peer_mode = descs[i].peer_mode;
		port->has_peer = false;
		port->endpoint = ep;

		ret = xa_insert(&ep->ports, port->index, port, GFP_KERNEL);
		if (ret) {
			/* A duplicate port index is a provider bug. */
			WARN_ON_ONCE(ret == -EBUSY);
			kfree(port);
			drm_fabric_ports_destroy(ep);
			return ret;
		}

		ep->num_ports++;
	}

	return 0;
}

/**
 * drm_fabric_endpoint_register() - Register an endpoint
 * @fabric: fabric the endpoint belongs to
 * @desc: endpoint description, including its fixed set of ports
 *
 * Registers the endpoint and emits an ENDPOINT_CREATE event. When the endpoint
 * joins a fabric its desc->fabric_ep_id must be unique among that fabric's
 * members; a duplicate is rejected with -EEXIST.
 *
 * A NULL @fabric registers an orphan endpoint, which a later ENDPOINT_SET
 * attach can join to a fabric.
 *
 * A fabric that has already left the registry is rejected, but that check
 * matches on address and cannot distinguish incarnations; only the provider
 * knows its own object lifecycle.
 *
 * Publication joins the mutation transaction domain
 * (drm_fabric_mutation_lock -> drm_fabric_lock), so a provider must not call
 * this from within a mutation callback (that would self-deadlock).
 *
 * Context: May sleep. Acquires drm_fabric_mutation_lock, then drm_fabric_lock.
 * Return: the registered endpoint, or an ERR_PTR() on failure: -EINVAL if
 * @desc->parent is NULL, or @desc claims ports without supplying a port array,
 * -ENODEV if @fabric is no longer registered, -EEXIST if @desc->fabric_ep_id is
 * already in use within @fabric.
 */
struct drm_fabric_endpoint *
drm_fabric_endpoint_register(struct drm_fabric *fabric,
			     const struct drm_fabric_endpoint_desc *desc)
{
	struct drm_fabric_endpoint *ep __free(kfree) = NULL;
	int ret;

	lockdep_assert_not_held(&drm_fabric_mutation_lock);

	/* Supplies dev_name()/bus for the query paths; pinned below. */
	if (!desc->parent)
		return ERR_PTR(-EINVAL);

	if (desc->num_ports && !desc->ports)
		return ERR_PTR(-EINVAL);

	ep = kzalloc_obj(*ep);
	if (!ep)
		return ERR_PTR(-ENOMEM);

	ep->fabric_ep_id = desc->fabric_ep_id;
	ep->fabric = fabric;
	ep->parent = desc->parent;
	ep->ops = desc->ops;
	ep->priv = desc->priv;
	ep->admin_state = fabric ? DRM_FABRIC_ADMIN_STATE_UP : DRM_FABRIC_ADMIN_STATE_DOWN;
	refcount_set(&ep->refs, 1);
	init_completion(&ep->unregistered);
	xa_init(&ep->ports);

	if (desc->name &&
	    strscpy(ep->name, desc->name, sizeof(ep->name)) < 0)
		return ERR_PTR(-ENAMETOOLONG);

	ret = drm_fabric_ports_create(ep, desc->ports, desc->num_ports);
	if (ret)
		return ERR_PTR(ret);

	get_device(ep->parent);

	ret = 0;
	/*
	 * Under the mutation lock a concurrent ENDPOINT_SET attach cannot also
	 * claim this (fabric, fabric_ep_id).
	 */
	scoped_guard(mutex, &drm_fabric_mutation_lock) {
		scoped_guard(mutex, &drm_fabric_lock) {
			if (fabric && !drm_fabric_is_registered(fabric)) {
				ret = -ENODEV;
				break;
			}

			if (drm_fabric_ep_id_in_use(fabric, ep->fabric_ep_id)) {
				ret = -EEXIST;
				break;
			}

			ret = xa_alloc(&drm_fabric_ep_xa, &ep->id, ep, xa_limit_32b, GFP_KERNEL);
			if (ret)
				break;
			if (fabric)
				drm_fabric_get(fabric);

			drm_fabric_emit_endpoint_create(ep, drm_fabric_base_seq_inc());
		}
	}

	if (ret) {
		put_device(ep->parent);
		drm_fabric_ports_destroy(ep);
		return ERR_PTR(ret);
	}

	return_ptr(ep);
}
EXPORT_SYMBOL(drm_fabric_endpoint_register);

/**
 * drm_fabric_endpoint_port() - Look up a port of a registered endpoint
 * @ep: provider-owned endpoint
 * @port_index: per-endpoint port index
 *
 * An endpoint's set of ports is fixed for its registration lifetime, so a
 * provider that serializes the endpoint lifecycle may call this without
 * holding drm_fabric_lock. The returned pointer is borrowed and remains valid
 * only while @ep is registered; it must not be retained across
 * drm_fabric_endpoint_unregister().
 *
 * Return: the port at @port_index, or NULL if no such port exists.
 */
struct drm_fabric_port *
drm_fabric_endpoint_port(struct drm_fabric_endpoint *ep, u32 port_index)
{
	return xa_load(&ep->ports, port_index);
}
EXPORT_SYMBOL(drm_fabric_endpoint_port);

struct drm_fabric_endpoint *drm_fabric_endpoint_get(struct drm_fabric_endpoint *ep)
{
	lockdep_assert_held(&drm_fabric_lock);
	refcount_inc(&ep->refs);
	return ep;
}

void drm_fabric_endpoint_put(struct drm_fabric_endpoint *ep)
{
	if (refcount_dec_and_test(&ep->refs))
		complete(&ep->unregistered);
}

/*
 * Same contract as drm_fabric_is_registered(): a stale, possibly freed @ep is
 * compared but never dereferenced, and an address reused by a later
 * registration passes as that later endpoint.
 */
static bool drm_fabric_ep_is_registered(const struct drm_fabric_endpoint *ep)
{
	struct drm_fabric_endpoint *entry;
	unsigned long idx;

	lockdep_assert_held(&drm_fabric_lock);

	xa_for_each(&drm_fabric_ep_xa, idx, entry)
		if (entry == ep)
			return true;

	return false;
}

/**
 * drm_fabric_endpoint_unregister() - Unregister and free an endpoint
 * @ep: endpoint to remove
 *
 * Removes @ep from the registry and frees it along with its ports, advancing
 * the topology generation. Peers on other endpoints that name @ep are left
 * untouched: the core never scans or retracts a half-edge on unregister.
 *
 * The provider must first quiesce its own use of @ep and any borrowed
 * drm_fabric_endpoint_port() pointer. The core waits only for references it
 * issued itself; concurrent netlink readers need no provider action.
 *
 * @ep must still be registered. The pointer is compared against the registry
 * before anything dereferences it, so a stale or repeated unregister warns
 * and performs no teardown; there is no error return to report it. As for a
 * fabric, the comparison proves current address membership only.
 *
 * Context: May sleep. Acquires drm_fabric_mutation_lock, then drm_fabric_lock.
 *          Must not be called from a provider mutation callback, which already
 *          holds the mutation lock and would self-deadlock.
 */
void drm_fabric_endpoint_unregister(struct drm_fabric_endpoint *ep)
{
	lockdep_assert_not_held(&drm_fabric_mutation_lock);

	/*
	 * Blocks a racing mutator: @ep cannot resolve once erased, so no
	 * ENDPOINT_CHANGE follows.
	 */
	scoped_guard(mutex, &drm_fabric_mutation_lock) {
		scoped_guard(mutex, &drm_fabric_lock) {
			if (WARN_ON_ONCE(!drm_fabric_ep_is_registered(ep)))
				return;
			/* Emit before the erase, while @ep is still live. */
			drm_fabric_emit_endpoint_delete(ep,
							drm_fabric_base_seq_inc());
			xa_erase(&drm_fabric_ep_xa, ep->id);
		}
	}

	/* Wait for in-flight operations holding endpoint pins. */
	drm_fabric_endpoint_put(ep);
	wait_for_completion(&ep->unregistered);

	/* An orphan endpoint holds no fabric reference. */
	if (ep->fabric)
		drm_fabric_put(ep->fabric);

	drm_fabric_ports_destroy(ep);
	put_device(ep->parent);
	kfree(ep);
}
EXPORT_SYMBOL(drm_fabric_endpoint_unregister);

/* Borrowed: valid only while drm_fabric_lock is held. */
struct drm_fabric_port *drm_fabric_port_find(u32 ep_id, u32 port_idx)
{
	struct drm_fabric_endpoint *ep;

	lockdep_assert_held(&drm_fabric_lock);
	ep = drm_fabric_endpoint_find_by_id(ep_id);
	if (!ep)
		return NULL;
	return xa_load(&ep->ports, port_idx);
}

/*
 * No refcount of its own: pinned through its owning endpoint and released
 * with drm_fabric_port_put(). ERR_PTR(-ENOENT) if no such port exists.
 */
struct drm_fabric_port *drm_fabric_port_find_get(u32 ep_id, u32 port_idx)
{
	struct drm_fabric_port *port;

	scoped_guard(mutex, &drm_fabric_lock) {
		port = drm_fabric_port_find(ep_id, port_idx);
		if (!port)
			return ERR_PTR(-ENOENT);
		drm_fabric_endpoint_get(port->endpoint);
	}

	return port;
}

void drm_fabric_port_put(struct drm_fabric_port *port)
{
	drm_fabric_endpoint_put(port->endpoint);
}

static bool drm_fabric_peer_type_valid(enum drm_fabric_peer_type type)
{
	switch (type) {
	case DRM_FABRIC_PEER_TYPE_ACCEL:
	case DRM_FABRIC_PEER_TYPE_SWITCH:
		return true;
	}

	return false;
}

/**
 * drm_fabric_port_set_peer() - Set a neighbor (called by the provider)
 * @port: local port
 * @peer: descriptor of the endpoint on the other end
 *
 * Emits a PORT_PEER_NEW event with new peer details on success. Only valid on a
 * provider-managed port; a userspace-managed port (%DRM_FABRIC_PEER_MODE_USERSPACE)
 * is programmed through the PORT_PEER_NEW uAPI instead.
 *
 * Context: May sleep. Acquires drm_fabric_lock.
 * Return: -EOPNOTSUPP on a userspace-managed port, -EINVAL if @peer carries an
 * unknown peer type, -EEXIST if the port already has a peer, 0 on success.
 */
int drm_fabric_port_set_peer(struct drm_fabric_port *port,
			     const struct drm_fabric_peer *peer)
{
	if (port->peer_mode != DRM_FABRIC_PEER_MODE_PROVIDER)
		return -EOPNOTSUPP;

	/*
	 * An unknown type is a provider bug, and the netlink path serialises
	 * the value verbatim into an enum-typed attribute, so refusing it here
	 * keeps it off the wire.
	 */
	if (WARN_ON_ONCE(!drm_fabric_peer_type_valid(peer->peer_type)))
		return -EINVAL;

	scoped_guard(mutex, &drm_fabric_lock) {
		if (port->has_peer)
			return -EEXIST;
		port->peer = *peer;
		port->has_peer = true;
		drm_fabric_emit_port_peer_create(port, peer,
						 drm_fabric_base_seq_inc());
	}

	return 0;
}
EXPORT_SYMBOL(drm_fabric_port_set_peer);

/**
 * drm_fabric_port_unset_peer() - Remove a neighbor (called by the provider)
 * @port: local port
 *
 * Inverse of drm_fabric_port_set_peer(). Only valid on a provider-managed port.
 * Emits a PORT_PEER_DEL event carrying the removed peer on success.
 *
 * Edge retraction is always explicit (provider- or controller-driven); the
 * core never removes a peer implicitly.
 *
 * Context: May sleep. Acquires drm_fabric_lock.
 * Return: -EOPNOTSUPP on a userspace-managed port, -ENOENT if no peer set,
 * 0 on success.
 */
int drm_fabric_port_unset_peer(struct drm_fabric_port *port)
{
	if (port->peer_mode != DRM_FABRIC_PEER_MODE_PROVIDER)
		return -EOPNOTSUPP;

	scoped_guard(mutex, &drm_fabric_lock) {
		if (!port->has_peer)
			return -ENOENT;
		/* Emit before clearing to show the peer being removed. */
		drm_fabric_emit_port_peer_delete(port, &port->peer,
						 drm_fabric_base_seq_inc());
		port->has_peer = false;
		memset(&port->peer, 0, sizeof(port->peer));
	}

	return 0;
}
EXPORT_SYMBOL(drm_fabric_port_unset_peer);

static bool drm_fabric_port_oper_state_valid(enum drm_fabric_port_state state)
{
	switch (state) {
	case DRM_FABRIC_PORT_STATE_UNKNOWN:
	case DRM_FABRIC_PORT_STATE_INACTIVE:
	case DRM_FABRIC_PORT_STATE_ACTIVE:
	case DRM_FABRIC_PORT_STATE_DEGRADED:
		return true;
	}

	return false;
}

/**
 * drm_fabric_port_set_oper() - Update a port's operational state
 * @port: port to update
 * @state: new operational state
 *
 * Advances the topology generation if the state actually changed. Invalid
 * states are provider bugs; they are warned about and ignored.
 *
 * Context: May sleep. Acquires drm_fabric_lock.
 */
void drm_fabric_port_set_oper(struct drm_fabric_port *port,
			      enum drm_fabric_port_state state)
{
	if (WARN_ON_ONCE(!drm_fabric_port_oper_state_valid(state)))
		return;

	scoped_guard(mutex, &drm_fabric_lock) {
		enum drm_fabric_port_state old = port->oper_state;

		port->oper_state = state;
		if (old != state) {
			u32 gen = drm_fabric_base_seq_inc();

			drm_fabric_emit_port_change(port, gen);
		}
	}
}
EXPORT_SYMBOL(drm_fabric_port_set_oper);

/* FABRIC_NEW yields an empty fabric; endpoints join only via ENDPOINT_SET. */
int drm_fabric_user_fabric_new(enum drm_fabric_type type, u64 instance_id,
			       const char *name, u32 *fabric_id_out)
{
	struct drm_fabric_desc desc = {
		.type = type,
		.instance_id = instance_id,
		.name = name,
	};
	struct drm_fabric *fabric;

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	/* Userspace-supplied: reject without warning. */
	if (!drm_fabric_type_valid(type))
		return -EINVAL;

	/*
	 * A userspace-owned fabric outlives this call -- only FABRIC_DEL
	 * removes it -- so pin the module.
	 */
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	fabric = __drm_fabric_register(&desc, DRM_FABRIC_OWNER_USERSPACE);
	if (IS_ERR(fabric)) {
		module_put(THIS_MODULE);
		return PTR_ERR(fabric);
	}

	if (fabric_id_out)
		*fabric_id_out = fabric->id;

	return 0;
}

int drm_fabric_user_fabric_del(u32 fabric_id)
{
	struct drm_fabric *fabric;

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	scoped_guard(mutex, &drm_fabric_lock) {
		fabric = xa_load(&drm_fabric_xa, fabric_id);
		if (!fabric)
			return -ENOENT;
		/*
		 * Userspace-owned fabrics only: a provider's registration pointer
		 * would dangle.
		 */
		if (fabric->owner != DRM_FABRIC_OWNER_USERSPACE)
			return -EPERM;
		/* Symmetric with FABRIC_NEW: only an empty fabric may be removed. */
		if (drm_fabric_has_members(fabric))
			return -EBUSY;
		/* Notify while the userspace-owned fabric is still addressable by id. */
		drm_fabric_emit_fabric_delete(fabric, drm_fabric_base_seq_inc());
		xa_erase(&drm_fabric_xa, fabric->id);
	}

	/* Release the pin from FABRIC_NEW. */
	module_put(THIS_MODULE);
	drm_fabric_put(fabric);
	return 0;
}

/* Pins the fabric for use after the lock is dropped, or ERR_PTR(-ENOENT). */
static struct drm_fabric *drm_fabric_find_get_locked(u32 fabric_id)
{
	struct drm_fabric *fabric;

	fabric = drm_fabric_find_by_id(fabric_id);
	if (!fabric)
		return ERR_PTR(-ENOENT);

	return drm_fabric_get(fabric);
}

/*
 * Caller holds mutation_lock throughout. The target is resolved and pinned
 * before drm_fabric_lock is dropped for the sleeping callback.
 */
int drm_fabric_endpoint_set(struct drm_fabric_endpoint *ep,
			    const struct drm_fabric_endpoint_change *req)
{
	struct drm_fabric_endpoint_change change = *req;
	const struct drm_fabric_ops *ops = ep->ops;
	struct drm_fabric *new_fabric = NULL;
	int ret;

	if (!ops || !ops->endpoint_set)
		return -EOPNOTSUPP;

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	scoped_guard(mutex, &drm_fabric_lock) {
		/* No-op administrative request. */
		if ((change.valid & DRM_FABRIC_EP_CHANGE_ADMIN) &&
		    change.admin == ep->admin_state)
			change.valid &= ~DRM_FABRIC_EP_CHANGE_ADMIN;

		if (!change.valid)
			return 0;

		if (!(change.valid & DRM_FABRIC_EP_CHANGE_FABRIC))
			break;

		if (change.fabric_id == drm_fabric_endpoint_fabric_id(ep)) {
			change.valid &= ~DRM_FABRIC_EP_CHANGE_FABRIC;
			if (!change.valid)
				return 0;
			break;
		}

		/* Detach */
		if (!change.fabric_id)
			break;

		/* Attach: endpoint must be orphaned. */
		if (ep->fabric)
			return -EBUSY;

		new_fabric = drm_fabric_find_get_locked(change.fabric_id);
		if (IS_ERR(new_fabric))
			return PTR_ERR(new_fabric);

		/* Reject early if the target fabric already uses this id. */
		if (drm_fabric_ep_id_in_use(new_fabric, ep->fabric_ep_id)) {
			drm_fabric_put(new_fabric);
			return -EEXIST;
		}
	}

	ret = ops->endpoint_set(ep, &change, new_fabric);
	if (ret) {
		if (new_fabric)
			drm_fabric_put(new_fabric);
		return ret;
	}

	/*
	 * mutation_lock rules out a race; the WARN_ON_ONCE rechecks below are
	 * assertions only.
	 */
	scoped_guard(mutex, &drm_fabric_lock) {
		if (change.valid & DRM_FABRIC_EP_CHANGE_FABRIC) {
			if (new_fabric) {
				if (WARN_ON_ONCE(xa_load(&drm_fabric_xa, new_fabric->id) !=
						 new_fabric)) {
					drm_fabric_put(new_fabric);
					return -ENODEV;
				}
				if (WARN_ON_ONCE(drm_fabric_ep_id_in_use(new_fabric,
									 ep->fabric_ep_id))) {
					drm_fabric_put(new_fabric);
					return -EEXIST;
				}
				ep->fabric = new_fabric;
			} else {
				/* Reached only for an attached endpoint. */
				drm_fabric_put(ep->fabric);
				ep->fabric = NULL;
			}
		}

		if (change.valid & DRM_FABRIC_EP_CHANGE_ADMIN)
			ep->admin_state = change.admin;

		drm_fabric_emit_endpoint_change(ep, drm_fabric_base_seq_inc());
	}

	return 0;
}

/*
 * No revalidation is needed: the port stays pinned and admin_state has no
 * out-of-band writer.
 */
int drm_fabric_port_set_admin(struct drm_fabric_port *port,
			      enum drm_fabric_admin_state admin)
{
	const struct drm_fabric_ops *ops = port->endpoint->ops;
	int ret;

	if (!ops || !ops->port_set)
		return -EOPNOTSUPP;

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	/* No-op administrative request. */
	scoped_guard(mutex, &drm_fabric_lock)
		if (port->admin_state == admin)
			return 0;

	ret = ops->port_set(port, admin);
	if (ret)
		return ret;

	scoped_guard(mutex, &drm_fabric_lock) {
		port->admin_state = admin;
		drm_fabric_emit_port_change(port, drm_fabric_base_seq_inc());
	}

	return 0;
}

/*
 * No revalidation is needed: the port stays pinned and has_peer has no
 * out-of-band writer.
 */
int drm_fabric_port_peer_new(struct drm_fabric_port *port,
			     const struct drm_fabric_peer *peer)
{
	const struct drm_fabric_ops *ops = port->endpoint->ops;
	int ret;

	if (port->peer_mode != DRM_FABRIC_PEER_MODE_USERSPACE)
		return -EOPNOTSUPP;

	if (!ops || !ops->port_peer_new)
		return -EOPNOTSUPP;

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	scoped_guard(mutex, &drm_fabric_lock) {
		if (port->has_peer)
			return -EEXIST;
	}

	ret = ops->port_peer_new(port, peer);
	if (ret)
		return ret;

	scoped_guard(mutex, &drm_fabric_lock) {
		port->peer = *peer;
		port->has_peer = true;
		drm_fabric_emit_port_peer_create(port, peer,
						 drm_fabric_base_seq_inc());
	}

	return 0;
}

/* Same userspace-managed contract as drm_fabric_port_peer_new(). */
int drm_fabric_port_peer_del(struct drm_fabric_port *port)
{
	const struct drm_fabric_ops *ops = port->endpoint->ops;
	int ret;

	if (port->peer_mode != DRM_FABRIC_PEER_MODE_USERSPACE)
		return -EOPNOTSUPP;

	if (!ops || !ops->port_peer_del)
		return -EOPNOTSUPP;

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	scoped_guard(mutex, &drm_fabric_lock) {
		if (!port->has_peer)
			return -ENOENT;
	}

	ret = ops->port_peer_del(port);
	if (ret)
		return ret;

	scoped_guard(mutex, &drm_fabric_lock) {
		drm_fabric_emit_port_peer_delete(port, &port->peer,
						 drm_fabric_base_seq_inc());
		port->has_peer = false;
		memset(&port->peer, 0, sizeof(port->peer));
	}

	return 0;
}

static int __init drm_fabric_init(void)
{
	return drm_fabric_netlink_register();
}

static void __exit drm_fabric_exit(void)
{
	drm_fabric_netlink_unregister();

	WARN_ON(!xa_empty(&drm_fabric_xa));
	WARN_ON(!xa_empty(&drm_fabric_ep_xa));
	xa_destroy(&drm_fabric_xa);
	xa_destroy(&drm_fabric_ep_xa);
}

module_init(drm_fabric_init);
module_exit(drm_fabric_exit);

MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("DRM fabric infrastructure");
MODULE_LICENSE("Dual MIT/GPL");

#if IS_ENABLED(CONFIG_DRM_FABRIC_KUNIT_TEST)
#include "drm_fabric_test.c"
#endif
