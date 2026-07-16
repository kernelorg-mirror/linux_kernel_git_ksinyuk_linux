// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/*
 * KUnit tests for DRM Fabric object lifetime, identity, topology and
 * provider callback registration.
 */

#include <kunit/test.h>
#include <kunit/device.h>

#include <linux/device.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include <drm/drm_fabric.h>
#include <uapi/drm/drm_fabric.h>

#include "drm_fabric_internal.h"

static struct drm_fabric_port *fabrictest_port(struct drm_fabric_endpoint *ep,
					       u32 index)
{
	return drm_fabric_endpoint_port(ep, index);
}

static struct device *fabrictest_alloc_dev(struct kunit *test)
{
	struct device *dev = kunit_device_register(test, "drm_fabric_test");

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	return dev;
}

static void fabrictest_unregister_fabric(void *fab)
{
	drm_fabric_unregister(fab);
}

static void fabrictest_unregister_endpoint(void *ep)
{
	drm_fabric_endpoint_unregister(ep);
}

/* Production reads the generation under the lock; do the same here. */
static u32 fabrictest_seq_read(void)
{
	guard(mutex)(&drm_fabric_lock);

	return drm_fabric_base_seq;
}

static void fabrictest_seq_write(u32 val)
{
	guard(mutex)(&drm_fabric_lock);

	drm_fabric_base_seq = val;
}

static void fabrictest_restore_seq(void *saved)
{
	fabrictest_seq_write(*(u32 *)saved);
}

/* Restore on exit so a seeded value cannot leak into a later test. */
static void fabrictest_seed_seq(struct kunit *test, u32 val)
{
	u32 *saved = kunit_kzalloc(test, sizeof(*saved), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, saved);
	*saved = fabrictest_seq_read();
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_restore_seq, saved));

	fabrictest_seq_write(val);
}

static void drm_fabric_test_fabric_register(struct kunit *test)
{
	struct drm_fabric_desc desc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-fabric",
		.instance_id = 0xDEAD,
	};
	struct drm_fabric *fab;

	fab = drm_fabric_register(&desc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	KUNIT_EXPECT_EQ(test, fab->type, DRM_FABRIC_TYPE_SYNTHETIC);
	KUNIT_EXPECT_EQ(test, fab->instance_id, 0xDEADULL);
}

/*
 * The non-empty case warns because it is a provider teardown bug, so it is not
 * exercised here.
 */
static void drm_fabric_test_unregister_reports_removal(struct kunit *test)
{
	struct drm_fabric_desc desc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "unreg-ret",
	};
	struct drm_fabric *fab;

	fab = drm_fabric_register(&desc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_EXPECT_EQ(test, drm_fabric_unregister(fab), 0);
}

static void drm_fabric_test_endpoint_requires_fabric(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 4 };
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 1,
		.name = "no-fabric",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint *ep;

	ep = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_EXPECT_TRUE(test, IS_ERR(ep));
	if (IS_ERR(ep))
		KUNIT_EXPECT_EQ(test, PTR_ERR(ep), -EINVAL);
}

/* Registration must not walk a NULL port array. */
static void drm_fabric_test_endpoint_requires_port_array(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "no-port-array",
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 1,
		.name = "portless",
		.parent = fabrictest_dev,
		.ports = NULL,
		.num_ports = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_EXPECT_TRUE(test, IS_ERR(ep));
	if (IS_ERR(ep))
		KUNIT_EXPECT_EQ(test, PTR_ERR(ep), -EINVAL);
}

/* Reject a non-member pointer without dereferencing it. */
static void drm_fabric_test_unregister_rejects_non_member(struct kunit *test)
{
	struct drm_fabric *candidate;
	u32 before;

	candidate = kunit_kzalloc(test, sizeof(*candidate), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, candidate);

	before = fabrictest_seq_read();
	KUNIT_EXPECT_EQ(test, drm_fabric_unregister(candidate), -ENODEV);
	/* A rejected teardown emits nothing, so the generation cannot move. */
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), before);
}

/*
 * The check must compare the supplied pointer, not resolve the id it carries:
 * an id-keyed lookup would erase whichever live fabric owns that id.
 */
static void drm_fabric_test_unregister_rejects_same_id(struct kunit *test)
{
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "same-id-owner",
	};
	struct drm_fabric *fab, *twin;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	twin = kunit_kzalloc(test, sizeof(*twin), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, twin);
	twin->id = fab->id;

	KUNIT_EXPECT_EQ(test, drm_fabric_unregister(twin), -ENODEV);

	/* Returning 0 proves the impostor did not erase the id's owner. */
	kunit_remove_action(test, fabrictest_unregister_fabric, fab);
	KUNIT_EXPECT_EQ(test, drm_fabric_unregister(fab), 0);
}

/*
 * The endpoint registry is checked directly rather than through
 * drm_fabric_endpoint_unregister(), whose only report channel for an
 * unregistered pointer is a one-shot warning.
 */
static void drm_fabric_test_ep_membership_rejects_non_member(struct kunit *test)
{
	struct drm_fabric_endpoint *ep;

	ep = kunit_kzalloc(test, sizeof(*ep), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ep);

	scoped_guard(mutex, &drm_fabric_lock)
		KUNIT_EXPECT_FALSE(test, drm_fabric_ep_is_registered(ep));
}

/* Endpoint counterpart of the same-id case: membership is pointer identity. */
static void drm_fabric_test_ep_membership_rejects_same_id(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "ep-same-id-fab",
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 7,
		.name = "ep-same-id-owner",
		.parent = fabrictest_dev,
	};
	struct drm_fabric_endpoint *ep, *twin;
	struct drm_fabric *fab;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	twin = kunit_kzalloc(test, sizeof(*twin), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, twin);
	twin->id = ep->id;

	scoped_guard(mutex, &drm_fabric_lock) {
		KUNIT_EXPECT_FALSE(test, drm_fabric_ep_is_registered(twin));
		KUNIT_EXPECT_TRUE(test, drm_fabric_ep_is_registered(ep));
	}
}

/*
 * Collapses the register-vs-unregister race: unregister first, then attach to
 * the stale pointer.
 */
static void drm_fabric_test_endpoint_register_stale_fabric(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 4 };
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "stale-parent",
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 1,
		.name = "orphaned-by-unreg",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, drm_fabric_unregister(fab), 0);

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_EXPECT_TRUE(test, IS_ERR(ep));
	if (IS_ERR(ep))
		KUNIT_EXPECT_EQ(test, PTR_ERR(ep), -ENODEV);
}

static void drm_fabric_test_instance_id_unique(struct kunit *test)
{
	struct drm_fabric_desc a = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "iid-a",
		.instance_id = 0x1357,
	};
	struct drm_fabric_desc dup = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "iid-dup",
		.instance_id = 0x1357,
	};
	struct drm_fabric_desc zero_a = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "iid-z0", .instance_id = 0,
	};
	struct drm_fabric_desc zero_b = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "iid-z1", .instance_id = 0,
	};
	struct drm_fabric *fab, *fab2, *dupf;

	fab = drm_fabric_register(&a);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	dupf = drm_fabric_register(&dup);
	KUNIT_EXPECT_TRUE(test, IS_ERR(dupf));
	if (IS_ERR(dupf))
		KUNIT_EXPECT_EQ(test, PTR_ERR(dupf), -EEXIST);

	fab = drm_fabric_register(&zero_a);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));
	/* instance_id 0 is not special-cased. */
	fab2 = drm_fabric_register(&zero_b);
	KUNIT_EXPECT_TRUE(test, IS_ERR(fab2));
	if (IS_ERR(fab2))
		KUNIT_EXPECT_EQ(test, PTR_ERR(fab2), -EEXIST);
}

static void drm_fabric_test_endpoint_register(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-ep-fab",
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x42,
		.name = "ep0",
		.parent = fabrictest_dev,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab);
	KUNIT_EXPECT_EQ(test, ep->fabric_ep_id, 0x42ULL);
}

static void drm_fabric_test_port_register_peer(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-port-fab",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0,
		.max_lane_count = 4,
		.max_lane_signaling_rate_mbps = 200000,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x10,
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_peer peer = {
		.peer_id = 0x20,
		.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
		.port_index = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	port = fabrictest_port(ep, 0);
	KUNIT_ASSERT_NOT_NULL(test, port);

	KUNIT_EXPECT_EQ(test, port->index, 0);
	KUNIT_EXPECT_EQ(test, port->max_lane_count, 4);
	KUNIT_EXPECT_FALSE(test, port->has_peer);
	KUNIT_EXPECT_EQ(test, ep->num_ports, 1);

	KUNIT_EXPECT_EQ(test, drm_fabric_port_set_peer(port, &peer), 0);
	KUNIT_EXPECT_TRUE(test, port->has_peer);
	KUNIT_EXPECT_EQ(test, port->peer.peer_id, 0x20ULL);
	KUNIT_EXPECT_EQ(test, port->peer.port_index, 1);
}

/* Topology changes must invalidate an in-progress dump. */
static void drm_fabric_test_base_seq_advances(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-base-seq",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 2 };
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x40,
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_peer peer = {
		.peer_id = 0x41,
		.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
		.port_index = 0,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;
	u32 seq;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	port = fabrictest_port(ep, 0);
	KUNIT_ASSERT_NOT_NULL(test, port);

	seq = fabrictest_seq_read();
	KUNIT_EXPECT_EQ(test, drm_fabric_port_set_peer(port, &peer), 0);
	KUNIT_EXPECT_NE(test, fabrictest_seq_read(), seq);

	seq = fabrictest_seq_read();
	KUNIT_EXPECT_EQ(test, drm_fabric_port_unset_peer(port), 0);
	KUNIT_EXPECT_NE(test, fabrictest_seq_read(), seq);

	seq = fabrictest_seq_read();
	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_NE(test, fabrictest_seq_read(), seq);

	/* A no-op transition must not advance the generation. */
	seq = fabrictest_seq_read();
	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), seq);
}

/* The generation is a nonzero u32: wrapping must skip 0, not just increment. */
static void drm_fabric_test_generation_wrap_nonzero(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-gen-wrap",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 2 };
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x4A,
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	port = fabrictest_port(ep, 0);
	KUNIT_ASSERT_NOT_NULL(test, port);

	/* Make the next state change advance the generation once. */
	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_INACTIVE);

	fabrictest_seed_seq(test, U32_MAX);
	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_NE(test, fabrictest_seq_read(), 0);
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), 1);
}

static void drm_fabric_test_port_state_transition(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-state",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 2 };
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x30,
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	port = fabrictest_port(ep, 0);
	KUNIT_ASSERT_NOT_NULL(test, port);

	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_UNKNOWN);

	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_ACTIVE);

	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_DEGRADED);
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_DEGRADED);

	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_INACTIVE);
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_INACTIVE);
}

/* An invalid provider state must not be committed. */
static void drm_fabric_test_port_oper_state_rejects_invalid(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-state-invalid",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 2 };
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x32,
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	enum drm_fabric_port_state bad =
		(enum drm_fabric_port_state)(DRM_FABRIC_PORT_STATE_DEGRADED + 1);
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;
	u32 seq;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	port = fabrictest_port(ep, 0);
	KUNIT_ASSERT_NOT_NULL(test, port);

	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_ACTIVE);

	seq = fabrictest_seq_read();
	drm_fabric_port_set_oper(port, bad);
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), seq);

	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_INACTIVE);
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_INACTIVE);
}

static void drm_fabric_test_register_rejects_invalid_type(struct kunit *test)
{
	struct drm_fabric_desc above = {
		.type = (enum drm_fabric_type)(DRM_FABRIC_TYPE_SYNTHETIC + 1),
		.instance_id = 0x5a5a,
		.name = "test-type-above",
	};
	struct drm_fabric_desc zero = {
		.type = (enum drm_fabric_type)0,
		.instance_id = 0x5a5a,
		.name = "test-type-zero",
	};
	struct drm_fabric_desc good = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.instance_id = 0x5a5a,
		.name = "test-type-good",
	};
	struct drm_fabric *fab, *bad;
	u32 seq = fabrictest_seq_read();

	bad = drm_fabric_register(&above);
	KUNIT_EXPECT_TRUE(test, IS_ERR(bad));
	if (IS_ERR(bad))
		KUNIT_EXPECT_EQ(test, PTR_ERR(bad), -EINVAL);
	else
		drm_fabric_unregister(bad);

	bad = drm_fabric_register(&zero);
	KUNIT_EXPECT_TRUE(test, IS_ERR(bad));
	if (IS_ERR(bad))
		KUNIT_EXPECT_EQ(test, PTR_ERR(bad), -EINVAL);
	else
		drm_fabric_unregister(bad);

	/* Refused before publication, so the generation cannot have advanced. */
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), seq);

	/*
	 * Same instance id as both refusals: drm_fabric_has_instance() would
	 * answer -EEXIST here if either had left a registry entry behind.
	 */
	fab = drm_fabric_register(&good);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));
}

/* Verify reciprocity and far-side port indices for every K_4 edge. */
static void drm_fabric_test_mesh_kn_topology(struct kunit *test)
{
#define KN_EPS 4
#define KN_PORTS_PER_EP (KN_EPS - 1)
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "mesh-kn",
		.instance_id = 0x1002,
	};
	struct drm_fabric_port_desc pdescs[KN_PORTS_PER_EP];
	struct drm_fabric_endpoint_desc edesc;
	struct drm_fabric_peer peer;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *eps[KN_EPS];
	struct drm_fabric_port *ports[KN_EPS][KN_PORTS_PER_EP];
	int i, j, k, port_idx, peer_port;

	memset(pdescs, 0, sizeof(pdescs));
	for (j = 0; j < KN_PORTS_PER_EP; j++) {
		pdescs[j].index = j;
		pdescs[j].max_lane_count = 4;
		pdescs[j].max_lane_signaling_rate_mbps = 200000;
	}

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	for (i = 0; i < KN_EPS; i++) {
		memset(&edesc, 0, sizeof(edesc));
		edesc.fabric_ep_id = 0x100 + i;
		edesc.parent = fabrictest_dev;
		edesc.ports = pdescs;
		edesc.num_ports = KN_PORTS_PER_EP;

		eps[i] = drm_fabric_endpoint_register(fab, &edesc);
		KUNIT_ASSERT_FALSE(test, IS_ERR(eps[i]));
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test,
							  fabrictest_unregister_endpoint, eps[i]));

		for (j = 0; j < KN_PORTS_PER_EP; j++) {
			ports[i][j] = fabrictest_port(eps[i], j);
			KUNIT_ASSERT_NOT_NULL(test, ports[i][j]);
		}
	}

	/* Wire K_4: ep[i] port[j] -> ep[target] */
	for (i = 0; i < KN_EPS; i++) {
		port_idx = 0;
		for (j = 0; j < KN_EPS; j++) {
			if (i == j)
				continue;

			/* Compute peer's port index pointing back to us. */
			peer_port = 0;
			for (k = 0; k < KN_EPS; k++) {
				if (k == j)
					continue;
				if (k == i)
					break;
				peer_port++;
			}

			peer.peer_id = eps[j]->fabric_ep_id;
			peer.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL;
			peer.port_index = peer_port;
			drm_fabric_port_set_peer(ports[i][port_idx], &peer);
			port_idx++;
		}
	}

	for (i = 0; i < KN_EPS; i++) {
		for (j = 0; j < KN_PORTS_PER_EP; j++) {
			struct drm_fabric_port *p = ports[i][j];
			struct drm_fabric_port *pp;
			u64 peer_ep_id;
			u32 peer_pidx;
			int peer_ep_idx, pi;

			KUNIT_EXPECT_TRUE_MSG(test, p->has_peer,
					      "ep%d port%d has no peer", i, j);
			if (!p->has_peer)
				continue;

			peer_ep_id = p->peer.peer_id;
			peer_pidx = p->peer.port_index;

			peer_ep_idx = -1;
			for (pi = 0; pi < KN_EPS; pi++) {
				if (eps[pi]->fabric_ep_id == peer_ep_id) {
					peer_ep_idx = pi;
					break;
				}
			}
			KUNIT_EXPECT_GE_MSG(test, peer_ep_idx, 0,
					    "ep%d port%d peer EP not found", i, j);
			if (peer_ep_idx < 0)
				continue;
			KUNIT_EXPECT_LT(test, peer_pidx, (u32)KN_PORTS_PER_EP);
			if (peer_pidx >= (u32)KN_PORTS_PER_EP)
				continue;

			pp = ports[peer_ep_idx][peer_pidx];
			KUNIT_EXPECT_TRUE(test, pp->has_peer);
			KUNIT_EXPECT_EQ(test, pp->peer.peer_id, eps[i]->fabric_ep_id);
			KUNIT_EXPECT_EQ(test, pp->peer.port_index, (u32)j);
		}
	}
#undef KN_EPS
#undef KN_PORTS_PER_EP
}

static int fabrictest_stats_get(struct drm_fabric_port *port,
				struct drm_fabric_port_stats *stats)
{
	stats->read_bytes = 4096;
	stats->write_bytes = 2048;
	stats->link_down_count = 2;
	stats->retrain_count = 3;
	return 0;
}

static const struct drm_fabric_ops fabrictest_stats_ops = {
	.port_stats_get = fabrictest_stats_get,
};

/* This does not exercise netlink dispatch or error propagation. */
static void drm_fabric_test_port_stats_ops_registration(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-stats",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 4 };
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x5A,
		.parent = fabrictest_dev,
		.ops = &fabrictest_stats_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_desc edesc_noops = {
		.fabric_ep_id = 0x5B,
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_port_stats stats = {};
	struct drm_fabric_endpoint *ep, *ep_noops;
	struct drm_fabric_port *port;
	struct drm_fabric *fab;
	u32 gen;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	port = fabrictest_port(ep, 0);
	KUNIT_ASSERT_NOT_NULL(test, port);

	KUNIT_ASSERT_NOT_NULL(test, ep->ops);
	KUNIT_ASSERT_NOT_NULL(test, ep->ops->port_stats_get);

	/* A stats read is not a topology change: seq must not move. */
	gen = fabrictest_seq_read();
	KUNIT_EXPECT_EQ(test, ep->ops->port_stats_get(port, &stats), 0);
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), gen);
	KUNIT_EXPECT_EQ(test, stats.read_bytes, 4096ULL);
	KUNIT_EXPECT_EQ(test, stats.write_bytes, 2048ULL);
	KUNIT_EXPECT_EQ(test, stats.link_down_count, 2ULL);
	KUNIT_EXPECT_EQ(test, stats.retrain_count, 3ULL);

	ep_noops = drm_fabric_endpoint_register(fab, &edesc_noops);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_noops));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_noops));
	KUNIT_EXPECT_TRUE(test, !ep_noops->ops || !ep_noops->ops->port_stats_get);
}

/*
 * Unregistering an endpoint that has a peer link must clear only that
 * endpoint's own port record; it must not touch the still-registered far
 * side's peer record. (Contrast with drm_fabric_port_unset_peer(), which
 * clears a peer explicitly and is covered separately.)
 */
static void drm_fabric_test_local_unplug_keeps_edge(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-unplug",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 4 };
	struct drm_fabric_endpoint_desc eadesc = {
		.fabric_ep_id = 0xA0,
		.name = "unplug-a",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_desc ebdesc = {
		.fabric_ep_id = 0xB0,
		.name = "unplug-b",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_peer to_b = {
		.peer_id = 0xB0, .peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
	};
	struct drm_fabric_peer to_a = {
		.peer_id = 0xA0, .peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep_a, *ep_b;
	struct drm_fabric_port *pa, *pb;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep_a = drm_fabric_endpoint_register(fab, &eadesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_a));

	ep_b = drm_fabric_endpoint_register(fab, &ebdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_b));

	pa = fabrictest_port(ep_a, 0);
	pb = fabrictest_port(ep_b, 0);
	KUNIT_ASSERT_NOT_NULL(test, pa);
	KUNIT_ASSERT_NOT_NULL(test, pb);

	KUNIT_ASSERT_EQ(test, drm_fabric_port_set_peer(pa, &to_b), 0);
	KUNIT_ASSERT_EQ(test, drm_fabric_port_set_peer(pb, &to_a), 0);
	KUNIT_EXPECT_TRUE(test, pa->has_peer);

	/*
	 * Remove B without retracting its peer first, modelling abrupt provider
	 * teardown.
	 */
	kunit_release_action(test, fabrictest_unregister_endpoint, ep_b);

	/* The surviving half-edge must be byte-unchanged: no field mutated. */
	KUNIT_EXPECT_TRUE(test, pa->has_peer);
	KUNIT_EXPECT_MEMEQ(test, &pa->peer, &to_b, sizeof(pa->peer));
}

/*
 * A's peer record names a port index, not an object; registering and then
 * unregistering an unrelated third endpoint must not perturb it.
 */
static void drm_fabric_test_remote_peer_retained(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-remote",
	};
	struct drm_fabric_port_desc pdesc = { .index = 0, .max_lane_count = 4 };
	struct drm_fabric_endpoint_desc eadesc = {
		.fabric_ep_id = 0xA0,
		.name = "remote-a",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_desc ecdesc = {
		.fabric_ep_id = 0xC0,
		.name = "remote-c",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	/* 0xBEEF has no local endpoint object. */
	struct drm_fabric_peer remote = {
		.peer_id = 0xBEEF, .peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep_a, *ep_c;
	struct drm_fabric_port *pa;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep_a = drm_fabric_endpoint_register(fab, &eadesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_a));

	pa = fabrictest_port(ep_a, 0);
	KUNIT_ASSERT_NOT_NULL(test, pa);

	KUNIT_ASSERT_EQ(test, drm_fabric_port_set_peer(pa, &remote), 0);
	KUNIT_EXPECT_TRUE(test, pa->has_peer);

	ep_c = drm_fabric_endpoint_register(fab, &ecdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_c));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_c));

	kunit_release_action(test, fabrictest_unregister_endpoint, ep_c);

	KUNIT_EXPECT_TRUE(test, pa->has_peer);
	KUNIT_EXPECT_MEMEQ(test, &pa->peer, &remote, sizeof(pa->peer));
}

/*
 * Removing an endpoint with multiple peered ports must bump the topology
 * generation exactly once, not once per port torn down.
 */
static void drm_fabric_test_subtree_delete_single_bump(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-subtree",
	};
	struct drm_fabric_port_desc pdescs[3] = {
		{ .index = 0, .max_lane_count = 4 },
		{ .index = 1, .max_lane_count = 4 },
		{ .index = 2, .max_lane_count = 4 },
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0xD0,
		.name = "subtree-ep",
		.parent = fabrictest_dev,
		.ports = pdescs,
		.num_ports = 3,
	};
	struct drm_fabric_peer peer = {
		.peer_id = 0xD1, .peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	/* Two of the three ports carry a half-edge. */
	KUNIT_ASSERT_EQ(test, drm_fabric_port_set_peer(fabrictest_port(ep, 0), &peer), 0);
	KUNIT_ASSERT_EQ(test, drm_fabric_port_set_peer(fabrictest_port(ep, 1), &peer), 0);

	fabrictest_seed_seq(test, 100);
	kunit_release_action(test, fabrictest_unregister_endpoint, ep);
	KUNIT_EXPECT_EQ(test, fabrictest_seq_read(), 101);
}

static void drm_fabric_test_switch_topology(struct kunit *test)
{
#define SW_LEAVES 3
	/*
	 * Each leaf carries one half-edge to an opaque switch that is not a
	 * registered endpoint, so this asserts half-edge serialization, not any
	 * leaf -> switch -> leaf traversal.
	 */
	const u64 sw_id = 0x5000;
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-switch",
	};
	struct drm_fabric_port_desc leaf_pdesc = { .index = 0, .max_lane_count = 4 };
	struct drm_fabric_endpoint_desc edesc;
	struct drm_fabric_peer peer;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *leaves[SW_LEAVES];
	struct drm_fabric_port *leaf_ports[SW_LEAVES];
	int i;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	for (i = 0; i < SW_LEAVES; i++) {
		memset(&edesc, 0, sizeof(edesc));
		edesc.fabric_ep_id = 0x600 + i;
		edesc.parent = fabrictest_dev;
		edesc.ports = &leaf_pdesc;
		edesc.num_ports = 1;
		leaves[i] = drm_fabric_endpoint_register(fab, &edesc);
		KUNIT_ASSERT_FALSE(test, IS_ERR(leaves[i]));
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test,
							  fabrictest_unregister_endpoint,
							  leaves[i]));

		leaf_ports[i] = fabrictest_port(leaves[i], 0);
		KUNIT_ASSERT_NOT_NULL(test, leaf_ports[i]);

		peer.peer_id = sw_id;
		peer.peer_type = DRM_FABRIC_PEER_TYPE_SWITCH;
		peer.port_index = i;
		KUNIT_ASSERT_EQ(test,
				drm_fabric_port_set_peer(leaf_ports[i], &peer), 0);
	}

	/* Switch peer IDs do not share the endpoint identity namespace. */
	for (i = 0; i < SW_LEAVES; i++) {
		KUNIT_EXPECT_TRUE(test, leaf_ports[i]->has_peer);
		KUNIT_EXPECT_EQ(test, leaf_ports[i]->peer.peer_type,
				(u32)DRM_FABRIC_PEER_TYPE_SWITCH);
		KUNIT_EXPECT_EQ(test, leaf_ports[i]->peer.peer_id, sw_id);
		KUNIT_EXPECT_EQ(test, leaf_ports[i]->peer.port_index, (u32)i);
		KUNIT_EXPECT_NE(test, leaves[i]->fabric_ep_id, sw_id);
	}
#undef SW_LEAVES
}

enum fabrictest_shape { FT_SHAPE_LINEAR, FT_SHAPE_MESH };

struct fabrictest_topo_param {
	const char		*name;
	enum fabrictest_shape	shape;
	int			n_eps;
	int			ports_per_ep;
};

static const struct fabrictest_topo_param fabrictest_topo_params[] = {
	{ "linear-2", FT_SHAPE_LINEAR, 2, 1 },
	{ "linear-3", FT_SHAPE_LINEAR, 3, 2 },
	{ "linear-5", FT_SHAPE_LINEAR, 5, 2 },
	{ "linear-8", FT_SHAPE_LINEAR, 8, 2 },
	{ "mesh-2",   FT_SHAPE_MESH,   2, 1 },
	{ "mesh-3",   FT_SHAPE_MESH,   3, 2 },
	{ "mesh-4",   FT_SHAPE_MESH,   4, 3 },
	{ "mesh-6",   FT_SHAPE_MESH,   6, 5 },
};

static void fabrictest_topo_desc(const struct fabrictest_topo_param *p,
				 char *desc)
{
	strscpy(desc, p->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(fabrictest_topo, fabrictest_topo_params, fabrictest_topo_desc);

#define FT_MAX_EPS	8
#define FT_MAX_PORTS	16

/* Count peered ports to validate each generated topology's degree. */
static int fabrictest_peer_degree(struct drm_fabric_endpoint *ep, int n_ports)
{
	int j, deg = 0;

	for (j = 0; j < n_ports; j++) {
		struct drm_fabric_port *port = fabrictest_port(ep, j);

		if (port && port->has_peer)
			deg++;
	}
	return deg;
}

/*
 * Chain eps[0]-eps[1]-...-eps[n-1]; endpoints get one link, interior nodes
 * get two, each consuming the next free port.
 */
static void fabrictest_verify_linear_topology(struct kunit *test,
					      const struct fabrictest_topo_param *p,
					      struct drm_fabric_endpoint **eps)
{
	int cursor[FT_MAX_EPS] = {0};
	struct drm_fabric_peer peer;
	int i;

	for (i = 0; i < p->n_eps - 1; i++) {
		int ai = cursor[i]++;
		int bi = cursor[i + 1]++;

		peer = (struct drm_fabric_peer){
			.peer_id = eps[i + 1]->fabric_ep_id,
			.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
			.port_index = bi,
		};
		drm_fabric_port_set_peer(fabrictest_port(eps[i], ai), &peer);

		peer = (struct drm_fabric_peer){
			.peer_id = eps[i]->fabric_ep_id,
			.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
			.port_index = ai,
		};
		drm_fabric_port_set_peer(fabrictest_port(eps[i + 1], bi), &peer);
	}

	for (i = 0; i < p->n_eps; i++) {
		int want = (i == 0 || i == p->n_eps - 1) ? 1 : 2;

		KUNIT_EXPECT_EQ_MSG(test,
				    fabrictest_peer_degree(eps[i], p->ports_per_ep),
				    want, "linear ep%d degree", i);
	}
}

/*
 * Fully-connected K_N: every endpoint peers with every other one.
 */
static void fabrictest_verify_mesh_topology(struct kunit *test,
					    const struct fabrictest_topo_param *p,
					    struct drm_fabric_endpoint **eps)
{
	struct drm_fabric_peer peer;
	int i, j, k;

	for (i = 0; i < p->n_eps; i++) {
		int port_idx = 0;

		for (j = 0; j < p->n_eps; j++) {
			int peer_port = 0;

			if (i == j)
				continue;
			for (k = 0; k < p->n_eps; k++) {
				if (k == j)
					continue;
				if (k == i)
					break;
				peer_port++;
			}
			peer = (struct drm_fabric_peer){
				.peer_id = eps[j]->fabric_ep_id,
				.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
				.port_index = peer_port,
			};
			drm_fabric_port_set_peer(fabrictest_port(eps[i], port_idx),
						 &peer);
			port_idx++;
		}
	}

	for (i = 0; i < p->n_eps; i++)
		KUNIT_EXPECT_EQ_MSG(test,
				    fabrictest_peer_degree(eps[i], p->ports_per_ep),
				    p->n_eps - 1, "mesh ep%d degree", i);

	/* Spot-check reciprocity; exhaustive K_4 coverage is tested separately. */
	for (j = 0; j < p->n_eps - 1; j++) {
		struct drm_fabric_port *port = fabrictest_port(eps[0], j);
		struct drm_fabric_port *back;
		int pi, peer_ep = -1;

		KUNIT_ASSERT_NOT_NULL(test, port);
		KUNIT_ASSERT_TRUE(test, port->has_peer);

		for (pi = 0; pi < p->n_eps; pi++)
			if (eps[pi]->fabric_ep_id == port->peer.peer_id) {
				peer_ep = pi;
				break;
			}
		KUNIT_EXPECT_GE(test, peer_ep, 0);
		if (peer_ep < 0)
			continue;
		back = fabrictest_port(eps[peer_ep], port->peer.port_index);
		KUNIT_ASSERT_NOT_NULL(test, back);
		KUNIT_EXPECT_TRUE(test, back->has_peer);
		KUNIT_EXPECT_EQ(test, back->peer.peer_id,
				eps[0]->fabric_ep_id);
	}
}

/*
 * Parameterized over fabrictest_topo_params: linear chains and full meshes at
 * several sizes, checked generically via fabrictest_peer_degree().
 */
static void drm_fabric_test_topology_param(struct kunit *test)
{
	const struct fabrictest_topo_param *p = test->param_value;
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "test-topo",
	};
	struct drm_fabric_port_desc pdescs[FT_MAX_PORTS];
	struct drm_fabric_endpoint *eps[FT_MAX_EPS];
	struct drm_fabric_endpoint_desc edesc;
	struct drm_fabric *fab;
	int i, j;

	KUNIT_ASSERT_LE(test, p->n_eps, FT_MAX_EPS);
	KUNIT_ASSERT_LE(test, p->ports_per_ep, FT_MAX_PORTS);

	memset(pdescs, 0, sizeof(pdescs));
	for (j = 0; j < p->ports_per_ep; j++) {
		pdescs[j].index = j;
		pdescs[j].max_lane_count = 4;
		pdescs[j].max_lane_signaling_rate_mbps = 200000;
	}

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	for (i = 0; i < p->n_eps; i++) {
		memset(&edesc, 0, sizeof(edesc));
		edesc.fabric_ep_id = 0x100 + i;
		edesc.parent = fabrictest_dev;
		edesc.ports = pdescs;
		edesc.num_ports = p->ports_per_ep;

		eps[i] = drm_fabric_endpoint_register(fab, &edesc);
		KUNIT_ASSERT_FALSE(test, IS_ERR(eps[i]));
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test,
							  fabrictest_unregister_endpoint, eps[i]));
	}

	if (p->shape == FT_SHAPE_LINEAR)
		fabrictest_verify_linear_topology(test, p, eps);
	else
		fabrictest_verify_mesh_topology(test, p, eps);
}

static struct kunit_case drm_fabric_test_cases[] = {
	KUNIT_CASE(drm_fabric_test_fabric_register),
	KUNIT_CASE(drm_fabric_test_unregister_reports_removal),
	KUNIT_CASE(drm_fabric_test_instance_id_unique),
	KUNIT_CASE(drm_fabric_test_endpoint_register),
	KUNIT_CASE(drm_fabric_test_endpoint_requires_fabric),
	KUNIT_CASE(drm_fabric_test_endpoint_requires_port_array),
	KUNIT_CASE(drm_fabric_test_endpoint_register_stale_fabric),
	KUNIT_CASE(drm_fabric_test_unregister_rejects_non_member),
	KUNIT_CASE(drm_fabric_test_unregister_rejects_same_id),
	KUNIT_CASE(drm_fabric_test_ep_membership_rejects_non_member),
	KUNIT_CASE(drm_fabric_test_ep_membership_rejects_same_id),
	KUNIT_CASE(drm_fabric_test_port_register_peer),
	KUNIT_CASE(drm_fabric_test_base_seq_advances),
	KUNIT_CASE(drm_fabric_test_generation_wrap_nonzero),
	KUNIT_CASE(drm_fabric_test_port_state_transition),
	KUNIT_CASE(drm_fabric_test_port_oper_state_rejects_invalid),
	KUNIT_CASE(drm_fabric_test_register_rejects_invalid_type),
	KUNIT_CASE(drm_fabric_test_mesh_kn_topology),
	KUNIT_CASE(drm_fabric_test_port_stats_ops_registration),
	KUNIT_CASE(drm_fabric_test_local_unplug_keeps_edge),
	KUNIT_CASE(drm_fabric_test_remote_peer_retained),
	KUNIT_CASE(drm_fabric_test_subtree_delete_single_bump),
	KUNIT_CASE(drm_fabric_test_switch_topology),
	KUNIT_CASE_PARAM(drm_fabric_test_topology_param,
			 fabrictest_topo_gen_params),
	{}
};

static struct kunit_suite drm_fabric_test_suite = {
	.name = "drm_fabric",
	.test_cases = drm_fabric_test_cases,
};

kunit_test_suite(drm_fabric_test_suite);
