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

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/sched.h>
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

static struct drm_fabric *fabrictest_find_fabric(u32 id)
{
	struct drm_fabric *fab;

	mutex_lock(&drm_fabric_lock);
	fab = drm_fabric_find_by_id(id);
	mutex_unlock(&drm_fabric_lock);

	return fab;
}

/*
 * The FD-01 pin/unpin pair is only observable when drm_fabric is a loadable
 * module; built-in, try_module_get() is a stub. Check this before trusting
 * fabrictest_module_refcount().
 */
static bool fabrictest_module_refcount_observable(void)
{
	return IS_ENABLED(CONFIG_MODULE_UNLOAD) && IS_MODULE(CONFIG_DRM_FABRIC);
}

static int fabrictest_module_refcount(void)
{
#if defined(CONFIG_MODULE_UNLOAD) && IS_MODULE(CONFIG_DRM_FABRIC)
	return module_refcount(THIS_MODULE);
#else
	return 0;
#endif
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

/*
 * A NULL fabric registers an orphan endpoint, which reports fabric-id 0
 * and can be unregistered.
 */
static void drm_fabric_test_endpoint_orphan_register(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 1,
		.name = "orphan",
		.parent = fabrictest_dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint *ep;

	ep = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_EXPECT_EQ(test, drm_fabric_endpoint_fabric_id(ep), 0);

	drm_fabric_endpoint_unregister(ep);
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
 * kunit_device_register() always attaches its devices to the shared "kunit"
 * bus, so the case of identical dev-name across two real bus types -- where
 * a bus-name argument selects one endpoint and rejects the other -- requires a
 * custom test bus and is not covered here.
 */
static void drm_fabric_test_endpoint_find_by_dev_name_contract(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "find-by-dev-name",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.name = "selector",
		.parent = dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint *ep_a, *ep_b, *found;
	struct drm_fabric *fab;
	const char *devname, *busname;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	devname = dev_name(dev);
	busname = dev_bus_name(dev);

	edesc.fabric_ep_id = 0x501;
	ep_a = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_a));

	scoped_guard(mutex, &drm_fabric_lock) {
		found = drm_fabric_endpoint_find_by_dev_name(devname, NULL);
		KUNIT_EXPECT_PTR_EQ(test, found, ep_a);

		found = drm_fabric_endpoint_find_by_dev_name(devname, busname);
		KUNIT_EXPECT_PTR_EQ(test, found, ep_a);

		/* Non-matching bus filters the endpoint out. */
		found = drm_fabric_endpoint_find_by_dev_name(devname, "not-kunit");
		KUNIT_EXPECT_NULL(test, found);
	}

	edesc.fabric_ep_id = 0x502;
	ep_b = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_b));

	scoped_guard(mutex, &drm_fabric_lock) {
		found = drm_fabric_endpoint_find_by_dev_name(devname, NULL);
		KUNIT_EXPECT_TRUE(test, IS_ERR(found));
		if (IS_ERR(found))
			KUNIT_EXPECT_EQ(test, PTR_ERR(found), -EINVAL);

		found = drm_fabric_endpoint_find_by_dev_name(devname, busname);
		KUNIT_EXPECT_TRUE(test, IS_ERR(found));
		if (IS_ERR(found))
			KUNIT_EXPECT_EQ(test, PTR_ERR(found), -EINVAL);

		KUNIT_EXPECT_NULL(test,
				  drm_fabric_endpoint_find_by_dev_name("missing-device", NULL));
	}
}

/*
 * Collapses the register-vs-unregister race: unregister first, then attach to
 * the stale pointer.
 */
static void drm_fabric_test_endpoint_register_stale_fabric(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
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

static void drm_fabric_test_port_find_variants(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "port-find",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x120,
		.parent = dev,
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

	scoped_guard(mutex, &drm_fabric_lock) {
		KUNIT_EXPECT_PTR_EQ(test, drm_fabric_port_find(ep->id, 0), port);
		KUNIT_EXPECT_NULL(test, drm_fabric_port_find(ep->id, 1));
		KUNIT_EXPECT_NULL(test, drm_fabric_port_find(0x7fffffff, 0));
	}
}

static void drm_fabric_test_port_find_get_pin_balance(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "port-find-get",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x121,
		.parent = dev,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port, *pinned;
	int refs;

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

	refs = refcount_read(&ep->refs);
	pinned = drm_fabric_port_find_get(ep->id, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(pinned));
	KUNIT_EXPECT_PTR_EQ(test, pinned, port);
	KUNIT_EXPECT_EQ(test, refcount_read(&ep->refs), refs + 1);

	drm_fabric_port_put(pinned);
	KUNIT_EXPECT_EQ(test, refcount_read(&ep->refs), refs);

	/* _find_get() reports a miss as ERR_PTR(-ENOENT); _find() returns NULL. */
	KUNIT_EXPECT_EQ(test, PTR_ERR(drm_fabric_port_find_get(ep->id, 1)), -ENOENT);
	KUNIT_EXPECT_EQ(test, PTR_ERR(drm_fabric_port_find_get(0x7fffffff, 0)), -ENOENT);
}

/* Topology changes must invalidate an in-progress dump. */
static void drm_fabric_test_base_seq_advances(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-base-seq",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 2,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 2,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 2,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 2,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
		pdescs[j].peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER;
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

/* Trivial provider that accepts every mutation so the core commits it. */
static int fabrictest_mut_endpoint_set(struct drm_fabric_endpoint *ep,
				       const struct drm_fabric_endpoint_change *change,
				       struct drm_fabric *fabric)
{
	return 0;
}

static int fabrictest_mut_port_set(struct drm_fabric_port *port,
				   enum drm_fabric_admin_state admin)
{
	return 0;
}

static int fabrictest_mut_port_peer_new(struct drm_fabric_port *port,
					const struct drm_fabric_peer *peer)
{
	return 0;
}

static int fabrictest_mut_port_peer_del(struct drm_fabric_port *port)
{
	return 0;
}

static const struct drm_fabric_ops fabrictest_mut_ops = {
	.endpoint_set	= fabrictest_mut_endpoint_set,
	.port_set	= fabrictest_mut_port_set,
	.port_peer_new	= fabrictest_mut_port_peer_new,
	.port_peer_del	= fabrictest_mut_port_peer_del,
};

/*
 * Provider that rejects every mutation: the core calls it before committing, so
 * a failure must leave state, generation and notifications untouched.
 */
static int fabrictest_fail_endpoint_set(struct drm_fabric_endpoint *ep,
					const struct drm_fabric_endpoint_change *change,
					struct drm_fabric *fabric)
{
	return -EIO;
}

static int fabrictest_fail_port_set(struct drm_fabric_port *port,
				    enum drm_fabric_admin_state admin)
{
	return -EIO;
}

static int fabrictest_fail_port_peer_new(struct drm_fabric_port *port,
					 const struct drm_fabric_peer *peer)
{
	return -EIO;
}

static const struct drm_fabric_ops fabrictest_fail_ops = {
	.endpoint_set	= fabrictest_fail_endpoint_set,
	.port_set	= fabrictest_fail_port_set,
	.port_peer_new	= fabrictest_fail_port_peer_new,
};

/*
 * Internal mutators assert drm_fabric_mutation_lock is held, matching the
 * netlink pre/post_doit contract; wrap each with the lock here.
 */
static int fabrictest_ep_set_locked(struct drm_fabric_endpoint *ep,
				    const struct drm_fabric_endpoint_change *change)
{
	int ret;

	mutex_lock(&drm_fabric_mutation_lock);
	ret = drm_fabric_endpoint_set(ep, change);
	mutex_unlock(&drm_fabric_mutation_lock);
	return ret;
}

static int fabrictest_port_admin_locked(struct drm_fabric_port *port,
					enum drm_fabric_admin_state admin)
{
	int ret;

	mutex_lock(&drm_fabric_mutation_lock);
	ret = drm_fabric_port_set_admin(port, admin);
	mutex_unlock(&drm_fabric_mutation_lock);
	return ret;
}

static int fabrictest_port_peer_new_locked(struct drm_fabric_port *port,
					   const struct drm_fabric_peer *peer)
{
	int ret;

	mutex_lock(&drm_fabric_mutation_lock);
	ret = drm_fabric_port_peer_new(port, peer);
	mutex_unlock(&drm_fabric_mutation_lock);
	return ret;
}

static int fabrictest_port_peer_del_locked(struct drm_fabric_port *port)
{
	int ret;

	mutex_lock(&drm_fabric_mutation_lock);
	ret = drm_fabric_port_peer_del(port);
	mutex_unlock(&drm_fabric_mutation_lock);
	return ret;
}

static int fabrictest_user_fabric_new_locked(enum drm_fabric_type type,
					     u64 instance_id, const char *name,
					     u32 *fabric_id_out)
{
	int ret;

	mutex_lock(&drm_fabric_mutation_lock);
	ret = drm_fabric_user_fabric_new(type, instance_id, name, fabric_id_out);
	mutex_unlock(&drm_fabric_mutation_lock);
	return ret;
}

static int fabrictest_user_fabric_del_locked(u32 fabric_id)
{
	int ret;

	mutex_lock(&drm_fabric_mutation_lock);
	ret = drm_fabric_user_fabric_del(fabric_id);
	mutex_unlock(&drm_fabric_mutation_lock);
	return ret;
}

static void drm_fabric_test_failed_mutation_no_commit(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "test-failmut",
	};
	/* USERSPACE peer_mode so PORT_PEER_NEW reaches the provider below. */
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_USERSPACE,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x66, .parent = dev, .ops = &fabrictest_fail_ops,
		.ports = &pdesc, .num_ports = 1,
	};
	struct drm_fabric_peer peer = {
		.peer_id = 0x67, .peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
	};
	enum drm_fabric_admin_state admin0;
	struct drm_fabric_endpoint *ep;
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
	admin0 = port->admin_state;
	gen = drm_fabric_base_seq;

	KUNIT_EXPECT_EQ(test,
			fabrictest_port_admin_locked(port, DRM_FABRIC_ADMIN_STATE_UP), -EIO);
	KUNIT_EXPECT_EQ(test, port->admin_state, admin0);
	KUNIT_EXPECT_EQ(test, drm_fabric_base_seq, gen);

	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_new_locked(port, &peer), -EIO);
	KUNIT_EXPECT_FALSE(test, port->has_peer);
	KUNIT_EXPECT_EQ(test, drm_fabric_base_seq, gen);
}

static void drm_fabric_test_orphan_attach_detach(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-attach",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x55,
		.name = "orphan-ep",
		.parent = fabrictest_dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	u32 seq;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	KUNIT_EXPECT_NULL(test, ep->fabric);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_DOWN);

	/*
	 * Membership and admin state are independent in the core; provider
	 * policy may reject combinations such as admin-up on an orphan.
	 */
	seq = drm_fabric_base_seq;
	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_ADMIN, .admin = DRM_FABRIC_ADMIN_STATE_UP,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
	KUNIT_EXPECT_NE(test, drm_fabric_base_seq, seq);

	/* Attaching changes membership only; admin_state is untouched. */
	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = fab->id,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_UP);

	/* Detach while admin is UP is accepted: membership clears, admin is kept. */
	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = 0,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_NULL(test, ep->fabric);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_UP);

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_ADMIN, .admin = DRM_FABRIC_ADMIN_STATE_DOWN,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_DOWN);

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = 0,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_NULL(test, ep->fabric);
}

static void drm_fabric_test_endpoint_set_direct_reassign_busy(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc_a = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-reassign-a",
		.instance_id = 0x3101,
	};
	struct drm_fabric_desc fdesc_b = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-reassign-b",
		.instance_id = 0x3102,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x56,
		.name = "reassign-ep",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab_a, *fab_b;
	struct drm_fabric_endpoint *ep;

	fab_a = drm_fabric_register(&fdesc_a);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab_a));

	fab_b = drm_fabric_register(&fdesc_b);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab_b));

	ep = drm_fabric_endpoint_register(fab_a, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		.fabric_id = fab_b->id,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), -EBUSY);
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab_a);
}

static void drm_fabric_test_endpoint_set_detach_attach_keeps_ep_id(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc_a = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-move-a",
		.instance_id = 0x3201,
	};
	struct drm_fabric_desc fdesc_b = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-move-b",
		.instance_id = 0x3202,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x57,
		.name = "move-ep",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab_a, *fab_b;
	struct drm_fabric_endpoint *ep;
	u64 fabric_ep_id;

	fab_a = drm_fabric_register(&fdesc_a);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab_a));

	fab_b = drm_fabric_register(&fdesc_b);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab_b));

	ep = drm_fabric_endpoint_register(fab_a, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	fabric_ep_id = ep->fabric_ep_id;

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		.fabric_id = 0,
	};
	KUNIT_ASSERT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_NULL(test, ep->fabric);

	change.fabric_id = fab_b->id;
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab_b);
	KUNIT_EXPECT_EQ(test, ep->fabric_ep_id, fabric_ep_id);
}

static void drm_fabric_test_endpoint_register_admin_defaults(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-admin-defaults",
		.instance_id = 0x3301,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.name = "admin-defaults",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *member, *orphan;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	edesc.fabric_ep_id = 0x58;
	edesc.name = "member-default";
	member = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(member));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, member));

	edesc.fabric_ep_id = 0x59;
	edesc.name = "orphan-default";
	orphan = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan));

	KUNIT_EXPECT_EQ(test, member->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
	KUNIT_EXPECT_EQ(test, orphan->admin_state, DRM_FABRIC_ADMIN_STATE_DOWN);
}

static void drm_fabric_test_endpoint_attach_preserves_admin_state(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-admin-attach",
		.instance_id = 0x3401,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.name = "admin-attach",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *member, *orphan;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	edesc.fabric_ep_id = 0x5a;
	edesc.name = "member-attach";
	member = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(member));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, member));

	edesc.fabric_ep_id = 0x5b;
	edesc.name = "orphan-attach";
	orphan = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan));

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		.fabric_id = fab->id,
	};
	KUNIT_ASSERT_EQ(test, fabrictest_ep_set_locked(orphan, &change), 0);
	KUNIT_EXPECT_PTR_EQ(test, orphan->fabric, fab);
	KUNIT_EXPECT_EQ(test, orphan->admin_state, DRM_FABRIC_ADMIN_STATE_DOWN);
	KUNIT_EXPECT_EQ(test, member->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
}

static void drm_fabric_test_endpoint_set_attach_enable_single_transition(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-attach-enable",
		.instance_id = 0x3501,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x5c,
		.name = "attach-enable",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	u32 seq_before, seq_after;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	seq_before = fabrictest_seq_read();
	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC | DRM_FABRIC_EP_CHANGE_ADMIN,
		.fabric_id = fab->id,
		.admin = DRM_FABRIC_ADMIN_STATE_UP,
	};
	KUNIT_ASSERT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	seq_after = fabrictest_seq_read();

	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
	KUNIT_EXPECT_EQ(test, seq_after - seq_before, 1U);
}

static void drm_fabric_test_endpoint_detach_while_admin_up(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-detach-up",
		.instance_id = 0x3601,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x5d,
		.name = "detach-up",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC | DRM_FABRIC_EP_CHANGE_ADMIN,
		.fabric_id = fab->id,
		.admin = DRM_FABRIC_ADMIN_STATE_UP,
	};
	KUNIT_ASSERT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		.fabric_id = 0,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_NULL(test, ep->fabric);
	KUNIT_EXPECT_EQ(test, ep->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
}

/*
 * A combined membership+admin request must commit neither field when the
 * provider rejects it, mirroring drm_fabric_test_failed_mutation_no_commit()
 * for the two-field endpoint_set() path.
 */
static void drm_fabric_test_endpoint_set_combined_change_fails_no_commit(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-combined-fail",
		.instance_id = 0x3801,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x70,
		.name = "combined-fail",
		.parent = dev,
		.ops = &fabrictest_fail_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric *orig_fabric;
	enum drm_fabric_admin_state orig_admin;
	u32 gen;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	/* Registered as a member: registration default is admin state UP. */
	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	orig_fabric = ep->fabric;
	orig_admin = ep->admin_state;
	gen = drm_fabric_base_seq;

	/* Combined detach + admin-down request; the provider rejects it. */
	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC | DRM_FABRIC_EP_CHANGE_ADMIN,
		.fabric_id = 0,
		.admin = DRM_FABRIC_ADMIN_STATE_DOWN,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), -EIO);

	/* Neither field commits, and the generation does not advance. */
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, orig_fabric);
	KUNIT_EXPECT_EQ(test, ep->admin_state, orig_admin);
	KUNIT_EXPECT_EQ(test, drm_fabric_base_seq, gen);
}

static void drm_fabric_test_endpoint_set_unassigned_ep_id_namespace(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc_a = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-epid-a",
		.instance_id = 0x3701,
	};
	struct drm_fabric_desc fdesc_b = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-epid-b",
		.instance_id = 0x3702,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x60,
		.name = "orphan-epid",
		.parent = dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change attach;
	struct drm_fabric *fab_a, *fab_b;
	struct drm_fabric_endpoint *orphan_a, *orphan_b;

	fab_a = drm_fabric_register(&fdesc_a);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab_a));

	fab_b = drm_fabric_register(&fdesc_b);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab_b));

	orphan_a = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan_a));

	edesc.name = "orphan-epid-b";
	orphan_b = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan_b));

	attach = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		.fabric_id = fab_a->id,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(orphan_a, &attach), 0);
	KUNIT_EXPECT_PTR_EQ(test, orphan_a->fabric, fab_a);

	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(orphan_b, &attach), -EEXIST);
	KUNIT_EXPECT_NULL(test, orphan_b->fabric);

	attach.fabric_id = fab_b->id;
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(orphan_b, &attach), 0);
	KUNIT_EXPECT_PTR_EQ(test, orphan_b->fabric, fab_b);
}

/*
 * fabric_ep_id must be unique among a fabric's members (peer descriptors
 * resolve against it); orphan ids do not resolve and may collide.
 */
static void drm_fabric_test_ep_id_unique(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "test-epid",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.name = "epid", .parent = dev, .ops = &fabrictest_mut_ops,
		.ports = &pdesc, .num_ports = 1,
	};
	struct drm_fabric_endpoint_change attach;
	struct drm_fabric_endpoint *ep_a, *ep_dup, *orphan_a, *orphan_b, *orphan_c;
	struct drm_fabric *fab;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	edesc.fabric_ep_id = 0x42;
	ep_a = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_a));

	/* A second member reusing that id is rejected. */
	edesc.fabric_ep_id = 0x42;
	ep_dup = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_EXPECT_TRUE(test, IS_ERR(ep_dup));
	KUNIT_EXPECT_EQ(test, PTR_ERR(ep_dup), -EEXIST);

	edesc.fabric_ep_id = 0x43;
	ep_dup = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep_dup));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep_dup));

	/* Orphans do not resolve peers, so two may share an id. */
	edesc.fabric_ep_id = 0x42;
	orphan_a = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan_a));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan_a));

	edesc.fabric_ep_id = 0x42;
	orphan_b = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan_b));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan_b));

	/* Attaching an orphan whose id collides with a member is rejected. */
	attach = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = fab->id,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(orphan_a, &attach), -EEXIST);
	KUNIT_EXPECT_NULL(test, orphan_a->fabric);

	edesc.fabric_ep_id = 0x44;
	orphan_c = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(orphan_c));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, orphan_c));

	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(orphan_c, &attach), 0);
	KUNIT_EXPECT_PTR_EQ(test, orphan_c->fabric, fab);
}

static void drm_fabric_test_port_admin_peer(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-portadmin",
	};
	/* userspace-managed so this test can drive the PORT_PEER_NEW path. */
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_USERSPACE,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x77,
		.parent = fabrictest_dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_peer peer = {
		.peer_id = 0x88,
		.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
		.port_index = 2,
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

	seq = drm_fabric_base_seq;
	KUNIT_EXPECT_EQ(test, fabrictest_port_admin_locked(port, DRM_FABRIC_ADMIN_STATE_UP), 0);
	KUNIT_EXPECT_EQ(test, port->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
	KUNIT_EXPECT_NE(test, drm_fabric_base_seq, seq);

	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);

	seq = drm_fabric_base_seq;
	KUNIT_EXPECT_EQ(test, fabrictest_port_admin_locked(port, DRM_FABRIC_ADMIN_STATE_DOWN), 0);
	KUNIT_EXPECT_EQ(test, port->admin_state, DRM_FABRIC_ADMIN_STATE_DOWN);
	KUNIT_EXPECT_NE(test, drm_fabric_base_seq, seq);
	/* Admin and operational state are independent; admin-down preserves oper. */
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_ACTIVE);

	seq = drm_fabric_base_seq;
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_new_locked(port, &peer), 0);
	KUNIT_EXPECT_TRUE(test, port->has_peer);
	KUNIT_EXPECT_EQ(test, port->peer.peer_id, 0x88ULL);
	KUNIT_EXPECT_NE(test, drm_fabric_base_seq, seq);

	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_new_locked(port, &peer), -EEXIST);

	seq = drm_fabric_base_seq;
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_del_locked(port), 0);
	KUNIT_EXPECT_FALSE(test, port->has_peer);
	KUNIT_EXPECT_NE(test, drm_fabric_base_seq, seq);

	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_del_locked(port), -ENOENT);
}

/*
 * PROVIDER ports take peers only from drm_fabric_port_set_peer() (kernel side);
 * USERSPACE ports take peers only through the locked PEER_NEW/DEL mutators.
 * Each rejects the other's path with -EOPNOTSUPP.
 */
static void drm_fabric_test_peer_mode(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-peermode",
	};
	struct drm_fabric_port_desc pdescs[2] = {
		{ .index = 0, .max_lane_count = 4,
		  .peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER },
		{ .index = 1, .max_lane_count = 4,
		  .peer_mode = DRM_FABRIC_PEER_MODE_USERSPACE },
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x91,
		.parent = fabrictest_dev,
		.ops = &fabrictest_mut_ops,
		.ports = pdescs,
		.num_ports = 2,
	};
	struct drm_fabric_peer peer = {
		.peer_id = 0xA1,
		.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL,
		.port_index = 1,
	};
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *pport, *uport;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	pport = fabrictest_port(ep, 0);
	uport = fabrictest_port(ep, 1);
	KUNIT_ASSERT_NOT_NULL(test, pport);
	KUNIT_ASSERT_NOT_NULL(test, uport);

	/* Provider-managed port: the provider programs it; the user path is refused. */
	KUNIT_EXPECT_EQ(test, drm_fabric_port_set_peer(pport, &peer), 0);
	KUNIT_EXPECT_TRUE(test, pport->has_peer);
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_new_locked(pport, &peer), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_del_locked(pport), -EOPNOTSUPP);
	/* The refused user calls leave the provider's peer intact. */
	KUNIT_EXPECT_TRUE(test, pport->has_peer);
	/* Same-source duplicate/absent errors are preserved. */
	KUNIT_EXPECT_EQ(test, drm_fabric_port_set_peer(pport, &peer), -EEXIST);
	KUNIT_EXPECT_EQ(test, drm_fabric_port_unset_peer(pport), 0);
	KUNIT_EXPECT_EQ(test, drm_fabric_port_unset_peer(pport), -ENOENT);

	/* Userspace-managed port: the user path programs it; the provider is refused. */
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_new_locked(uport, &peer), 0);
	KUNIT_EXPECT_TRUE(test, uport->has_peer);
	KUNIT_EXPECT_EQ(test, drm_fabric_port_set_peer(uport, &peer), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, drm_fabric_port_unset_peer(uport), -EOPNOTSUPP);
	/* The refused provider calls leave the userspace peer intact. */
	KUNIT_EXPECT_TRUE(test, uport->has_peer);
	KUNIT_EXPECT_EQ(test, uport->peer.peer_id, 0xA1ULL);
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_new_locked(uport, &peer), -EEXIST);
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_del_locked(uport), 0);
	KUNIT_EXPECT_EQ(test, fabrictest_port_peer_del_locked(uport), -ENOENT);
}

/*
 * Model A reports operational state from the provisioning callback.
 * drm_fabric_lock must be dropped across the callback to avoid recursion.
 */
struct fabrictest_model_a_ctx {
	unsigned int	calls;
	bool		mutation_lock_held;
	bool		fabric_lock_held;
};

static int fabrictest_model_a_port_set(struct drm_fabric_port *port,
				       enum drm_fabric_admin_state admin)
{
	struct fabrictest_model_a_ctx *ctx = port->endpoint->priv;

	ctx->calls++;
#ifdef CONFIG_LOCKDEP
	ctx->mutation_lock_held = lockdep_is_held(&drm_fabric_mutation_lock);
	ctx->fabric_lock_held = lockdep_is_held(&drm_fabric_lock);
#endif

	lockdep_assert_held(&drm_fabric_mutation_lock);
	lockdep_assert_not_held(&drm_fabric_lock);

	/* Takes drm_fabric_lock: a core that had not dropped it would deadlock here. */
	drm_fabric_port_set_oper(port, DRM_FABRIC_PORT_STATE_ACTIVE);
	return 0;
}

static const struct drm_fabric_ops fabrictest_model_a_ops = {
	.port_set = fabrictest_model_a_port_set,
};

static void drm_fabric_test_model_a_oper_report(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "test-modela",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_USERSPACE,
	};
	struct fabrictest_model_a_ctx ctx = {};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x5a,
		.parent = dev,
		.ops = &fabrictest_model_a_ops,
		.priv = &ctx,
		.ports = &pdesc,
		.num_ports = 1,
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

	KUNIT_ASSERT_NE(test, port->admin_state, DRM_FABRIC_ADMIN_STATE_UP);
	KUNIT_ASSERT_NE(test, port->oper_state, DRM_FABRIC_PORT_STATE_ACTIVE);

	seq = drm_fabric_base_seq;

	KUNIT_EXPECT_EQ(test,
			fabrictest_port_admin_locked(port, DRM_FABRIC_ADMIN_STATE_UP), 0);

	KUNIT_EXPECT_EQ(test, ctx.calls, 1u);
	if (IS_ENABLED(CONFIG_LOCKDEP)) {
		KUNIT_EXPECT_TRUE(test, ctx.mutation_lock_held);
		KUNIT_EXPECT_FALSE(test, ctx.fabric_lock_held);
	}

	/*
	 * The synchronous oper report committed inside the callback and the
	 * administrative state committed after it, with no recursive deadlock.
	 */
	KUNIT_EXPECT_EQ(test, port->oper_state, DRM_FABRIC_PORT_STATE_ACTIVE);
	KUNIT_EXPECT_EQ(test, port->admin_state, DRM_FABRIC_ADMIN_STATE_UP);

	KUNIT_EXPECT_NE(test, drm_fabric_base_seq, seq);

	/*
	 * Repeating the same admin state is a no-op: no second provider call,
	 * no second seq bump.
	 */
	seq = drm_fabric_base_seq;
	KUNIT_EXPECT_EQ(test,
			fabrictest_port_admin_locked(port, DRM_FABRIC_ADMIN_STATE_UP), 0);
	KUNIT_EXPECT_EQ(test, ctx.calls, 1u);
	KUNIT_EXPECT_EQ(test, drm_fabric_base_seq, seq);
}

struct fabrictest_unreg_race {
	struct drm_fabric_endpoint *ep;
	struct completion started;
	struct completion finished;
};

static int fabrictest_unreg_thread(void *arg)
{
	struct fabrictest_unreg_race *r = arg;

	complete(&r->started);
	drm_fabric_endpoint_unregister(r->ep);
	complete(&r->finished);

	/* Stay alive until the test reaps us, so kthread_stop() is valid. */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	return 0;
}

/*
 * drm_fabric_endpoint_unregister() must take mutation_lock itself, so it cannot
 * race a concurrent mutator: it blocks until the lock is free.
 */
static void drm_fabric_test_unregister_serializes_mutation(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-unreg",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x99,
		.parent = fabrictest_dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct fabrictest_unreg_race r;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct task_struct *task;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	/* The worker owns this endpoint's unregister, so no kunit teardown action. */
	ep = drm_fabric_endpoint_register(fab, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));

	r.ep = ep;
	init_completion(&r.started);
	init_completion(&r.finished);

	mutex_lock(&drm_fabric_mutation_lock);

	task = kthread_run(fabrictest_unreg_thread, &r, "fabrtest-unreg");
	if (IS_ERR(task)) {
		/* A fatal assertion would skip cleanup; unwind before failing. */
		mutex_unlock(&drm_fabric_mutation_lock);
		drm_fabric_endpoint_unregister(ep);
		KUNIT_FAIL(test, "kthread_run failed: %pe", task);
		return;
	}

	KUNIT_EXPECT_GT(test,
			wait_for_completion_timeout(&r.started, msecs_to_jiffies(1000)),
			0);
	msleep(50);

	/* Racer entered unregister() but is still stuck waiting for the lock. */
	KUNIT_EXPECT_FALSE(test, try_wait_for_completion(&r.finished));

	/* Release => unregister proceeds and must finish promptly. */
	mutex_unlock(&drm_fabric_mutation_lock);
	KUNIT_EXPECT_GT(test,
			wait_for_completion_timeout(&r.finished, msecs_to_jiffies(5000)),
			0);

	kthread_stop(task);
}

static void fabrictest_stop_thread(void *t)
{
	kthread_stop(t);
}

/*
 * Attach and endpoint registration compete for one fabric_ep_id.
 * mutation_lock makes registration wait, then fail with -EEXIST.
 */
struct fabrictest_l4 {
	struct completion cb_entered;
	struct completion cb_release;
	struct completion attach_done;
	struct completion reg_done;
	int attach_ret;
	struct drm_fabric_endpoint *reg_ep;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *orphan;
	struct device *dev;
	u64 ep_id;
};

/*
 * Stalls inside the provider callback (mutation_lock held) so a second thread
 * can be started and observed to block on the same fabric_ep_id.
 */
static int fabrictest_l4_endpoint_set(struct drm_fabric_endpoint *ep,
				      const struct drm_fabric_endpoint_change *change,
				      struct drm_fabric *fabric)
{
	struct fabrictest_l4 *l4 = ep->priv;

	complete(&l4->cb_entered);
	/* Bounded so a test abort can never wedge teardown on this thread. */
	wait_for_completion_timeout(&l4->cb_release, msecs_to_jiffies(10000));
	return 0;
}

static const struct drm_fabric_ops fabrictest_l4_ops = {
	.endpoint_set = fabrictest_l4_endpoint_set,
};

static int fabrictest_l4_attach_thread(void *arg)
{
	struct fabrictest_l4 *l4 = arg;
	struct drm_fabric_endpoint_change attach = {
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		.fabric_id = l4->fab->id,
	};

	l4->attach_ret = fabrictest_ep_set_locked(l4->orphan, &attach);
	complete(&l4->attach_done);

	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	return 0;
}

static int fabrictest_l4_register_thread(void *arg)
{
	struct fabrictest_l4 *l4 = arg;
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = l4->ep_id,
		.name = "l4-b",
		.parent = l4->dev,
		.ops = &fabrictest_l4_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};

	l4->reg_ep = drm_fabric_endpoint_register(l4->fab, &edesc);
	complete(&l4->reg_done);

	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	return 0;
}

static void drm_fabric_test_attach_register_collision(struct kunit *test)
{
	struct device *dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC, .name = "test-l4",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc;
	struct fabrictest_l4 *l4;
	struct task_struct *t1, *t2;
	unsigned long timeout;

	l4 = kunit_kzalloc(test, sizeof(*l4), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, l4);
	init_completion(&l4->cb_entered);
	init_completion(&l4->cb_release);
	init_completion(&l4->attach_done);
	init_completion(&l4->reg_done);
	l4->dev = dev;
	l4->ep_id = 0x4242;
	l4->reg_ep = ERR_PTR(-EINPROGRESS);

	l4->fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(l4->fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, l4->fab));

	/* Orphan A with fabric_ep_id X and a blocking endpoint_set callback. */
	edesc = (struct drm_fabric_endpoint_desc){
		.fabric_ep_id = l4->ep_id,
		.name = "l4-a",
		.parent = dev,
		.ops = &fabrictest_l4_ops,
		.priv = l4,
		.ports = &pdesc,
		.num_ports = 1,
	};
	l4->orphan = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(l4->orphan));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test,
						  fabrictest_unregister_endpoint,
						  l4->orphan));

	/*
	 * T1 attaches A -> F and blocks inside the provider callback while it
	 * holds drm_fabric_mutation_lock.
	 */
	t1 = kthread_run(fabrictest_l4_attach_thread, l4, "fabrtest-l4-a");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t1));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_stop_thread, t1));
	KUNIT_ASSERT_GT(test,
			wait_for_completion_timeout(&l4->cb_entered, msecs_to_jiffies(5000)),
			0);

	/* T2 races to register B with the same id directly into F. */
	t2 = kthread_run(fabrictest_l4_register_thread, l4, "fabrtest-l4-b");
	KUNIT_ASSERT_FALSE(test, IS_ERR(t2));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_stop_thread, t2));

	/* Register thread is queued behind the stalled attach, not finished. */
	msleep(50);
	KUNIT_EXPECT_FALSE(test, try_wait_for_completion(&l4->reg_done));

	/* Release A's callback: the attach commits and claims X. */
	complete(&l4->cb_release);
	KUNIT_EXPECT_GT(test,
			wait_for_completion_timeout(&l4->attach_done, msecs_to_jiffies(5000)),
			0);
	KUNIT_EXPECT_EQ(test, l4->attach_ret, 0);
	KUNIT_EXPECT_PTR_EQ(test, l4->orphan->fabric, l4->fab);

	/* B then proceeds and must fail: X is now owned by A. */
	timeout = wait_for_completion_timeout(&l4->reg_done, msecs_to_jiffies(5000));
	KUNIT_EXPECT_GT(test, timeout, 0);
	if (!timeout)
		return;
	if (!IS_ERR(l4->reg_ep)) {
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test,
							  fabrictest_unregister_endpoint,
							  l4->reg_ep));
		KUNIT_FAIL(test, "racing registration unexpectedly succeeded");
		return;
	}

	KUNIT_EXPECT_EQ(test, PTR_ERR(l4->reg_ep), -EEXIST);
}

static int fabrictest_stats_get(struct drm_fabric_port *port,
				struct drm_fabric_port_stats *stats)
{
	/* The statistics callback may sleep and runs without either fabric lock. */
	lockdep_assert_not_held(&drm_fabric_lock);
	lockdep_assert_not_held(&drm_fabric_mutation_lock);

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
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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

static void drm_fabric_test_user_fabric_new_del(struct kunit *test)
{
	bool refcount_observable = fabrictest_module_refcount_observable();
	int baseline = refcount_observable ? fabrictest_module_refcount() : 0;
	struct drm_fabric *fab;
	u32 fid = 0;
	int ret;

	ret = fabrictest_user_fabric_new_locked(DRM_FABRIC_TYPE_SYNTHETIC, 0x1234, "vpod0", &fid);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_NE(test, fid, 0);
	if (ret || !fid)
		return;

	fab = fabrictest_find_fabric(fid);
	KUNIT_EXPECT_NOT_NULL(test, fab);
	if (fab)
		KUNIT_EXPECT_EQ(test, fab->type, DRM_FABRIC_TYPE_SYNTHETIC);

	/* FD-01: publishing a userspace fabric must pin the module. */
	if (refcount_observable)
		KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline + 1);

	KUNIT_EXPECT_EQ(test, fabrictest_user_fabric_del_locked(fid), 0);

	fab = fabrictest_find_fabric(fid);
	KUNIT_EXPECT_NULL(test, fab);

	/* FD-01: removing it must release that pin again. */
	if (refcount_observable)
		KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline);
}

/*
 * Keyed by id, not pointer: after deletion this returns -ENOENT instead of
 * touching freed memory.
 */
static void fabrictest_user_fabric_del(void *p)
{
	fabrictest_user_fabric_del_locked(*(u32 *)p);
}

/*
 * FD-01: a rejected FABRIC_NEW must not publish a fabric or leak a module
 * reference. Invalid type is refused before try_module_get(); a duplicate
 * (type, instance_id) is refused after it, so only that path tests module_put().
 */
static void drm_fabric_test_user_fabric_new_reject_no_module_ref(struct kunit *test)
{
	bool refcount_observable = fabrictest_module_refcount_observable();
	int baseline = refcount_observable ? fabrictest_module_refcount() : 0;
	u32 *fid = kunit_kzalloc(test, sizeof(*fid), GFP_KERNEL);
	u32 dup_fid = 0;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, fid);

	ret = fabrictest_user_fabric_new_locked((enum drm_fabric_type)0, 0xa1a1,
						"test-new-invalid", NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	if (refcount_observable)
		KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline);

	ret = fabrictest_user_fabric_new_locked(DRM_FABRIC_TYPE_SYNTHETIC, 0xa2a2,
						"test-new-dup", fid);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_user_fabric_del, fid));

	ret = fabrictest_user_fabric_new_locked(DRM_FABRIC_TYPE_SYNTHETIC, 0xa2a2,
						"test-new-dup2", &dup_fid);
	KUNIT_EXPECT_EQ(test, ret, -EEXIST);
	KUNIT_EXPECT_EQ(test, dup_fid, 0);
	KUNIT_EXPECT_NOT_NULL(test, fabrictest_find_fabric(*fid));

	/* Only the first, successful registration should still be pinning us. */
	if (refcount_observable)
		KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline + 1);
}

/*
 * Sweep of the endpoint_set()/user_fabric_del() error paths: no-op change,
 * nonexistent fabric, already-attached, and provider-vs-user ownership.
 */
static void drm_fabric_test_reject_paths(struct kunit *test)
{
	bool refcount_observable = fabrictest_module_refcount_observable();
	int baseline = refcount_observable ? fabrictest_module_refcount() : 0;
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	/* Distinct instance_ids: this case needs two live fabrics, not a
	 * uniqueness collision (which (type, instance_id) equality would now
	 * trigger -- including for instance_id 0).
	 */
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-reject",
		.instance_id = 0x2001,
	};
	struct drm_fabric_desc fdesc2 = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-reject2",
		.instance_id = 0x2002,
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct drm_fabric_endpoint_desc edesc = {
		.fabric_ep_id = 0x99,
		.name = "reject-ep",
		.parent = fabrictest_dev,
		.ops = &fabrictest_mut_ops,
		.ports = &pdesc,
		.num_ports = 1,
	};
	struct drm_fabric_endpoint_change change;
	struct drm_fabric *fab, *fab2;
	struct drm_fabric_endpoint *ep;

	fab = drm_fabric_register(&fdesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab));

	fab2 = drm_fabric_register(&fdesc2);
	KUNIT_ASSERT_FALSE(test, IS_ERR(fab2));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_fabric, fab2));

	ep = drm_fabric_endpoint_register(NULL, &edesc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(ep));
	KUNIT_ASSERT_EQ(test, 0,
			kunit_add_action_or_reset(test, fabrictest_unregister_endpoint, ep));

	/* An empty change request is a core no-op; netlink maps no-attrs to -EINVAL. */
	change = (struct drm_fabric_endpoint_change){ .valid = 0 };
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = 0x7fffffff,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), -ENOENT);

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = fab->id,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), 0);
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab);

	change = (struct drm_fabric_endpoint_change){
		.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = fab2->id,
	};
	KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &change), -EBUSY);
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab);

	/* Full FABRIC_DEL ownership/emptiness matrix. */

	/* Unknown id: not found, before any ownership or emptiness check. */
	KUNIT_EXPECT_EQ(test, fabrictest_user_fabric_del_locked(0x7fffffff), -ENOENT);

	/*
	 * Provider-owned fabrics are refused with -EPERM whether empty (fab2) or
	 * non-empty (fab holds @ep): a provider keeps sole ownership of its
	 * fabric's lifetime, and -EPERM is checked before the -EBUSY emptiness
	 * test.
	 */
	KUNIT_EXPECT_EQ(test, fabrictest_user_fabric_del_locked(fab->id), -EPERM);
	KUNIT_EXPECT_EQ(test, fabrictest_user_fabric_del_locked(fab2->id), -EPERM);

	/*
	 * FD-01: provider-owned fabrics never took a module reference, and a
	 * rejected delete must not touch either the object or a reference.
	 */
	KUNIT_EXPECT_PTR_EQ(test, fabrictest_find_fabric(fab->id), fab);
	KUNIT_EXPECT_PTR_EQ(test, fabrictest_find_fabric(fab2->id), fab2);
	if (refcount_observable)
		KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline);

	/*
	 * Userspace-owned fabrics: non-empty is -EBUSY, empty is deletable.
	 * Reuse @ep (moved out of @fab) to make the userspace fabric non-empty.
	 */
	{
		struct drm_fabric_endpoint_change detach = {
			.valid = DRM_FABRIC_EP_CHANGE_FABRIC, .fabric_id = 0,
		};
		struct drm_fabric_endpoint_change attach = {
			.valid = DRM_FABRIC_EP_CHANGE_FABRIC,
		};
		u32 *uid = kunit_kzalloc(test, sizeof(*uid), GFP_KERNEL);
		int ret;

		KUNIT_ASSERT_NOT_NULL(test, uid);
		ret = fabrictest_user_fabric_new_locked(DRM_FABRIC_TYPE_SYNTHETIC,
							0x2003, "test-user-del",
							uid);
		KUNIT_ASSERT_EQ(test, ret, 0);
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test,
							  fabrictest_user_fabric_del,
							  uid));
		if (refcount_observable)
			KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline + 1);

		KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &detach), 0);
		attach.fabric_id = *uid;
		KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &attach), 0);

		KUNIT_EXPECT_EQ(test, fabrictest_user_fabric_del_locked(*uid), -EBUSY);

		/*
		 * FD-01: the -EBUSY rejection must leave the fabric resolvable
		 * and its module reference held, exactly as before the attempt.
		 */
		KUNIT_EXPECT_NOT_NULL(test, fabrictest_find_fabric(*uid));
		if (refcount_observable)
			KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline + 1);

		KUNIT_EXPECT_EQ(test, fabrictest_ep_set_locked(ep, &detach), 0);
		KUNIT_EXPECT_EQ(test, fabrictest_user_fabric_del_locked(*uid), 0);

		/* FD-01: a successful delete drops both the object and the pin. */
		KUNIT_EXPECT_NULL(test, fabrictest_find_fabric(*uid));
		if (refcount_observable)
			KUNIT_EXPECT_EQ(test, fabrictest_module_refcount(), baseline);
	}
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
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
		{ .index = 0, .max_lane_count = 4, .peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER },
		{ .index = 1, .max_lane_count = 4, .peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER },
		{ .index = 2, .max_lane_count = 4, .peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER },
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

#define FABRICTEST_CONC_THREADS	4
#define FABRICTEST_CONC_ITERS	200

struct fabrictest_conc_ctx {
	/* Sampled *inside* the provider hook (under the core lock). */
	atomic_t	in_flight;
	atomic_t	max_in_flight;
	atomic_t	calls;
	/* Sampled *around* the core mutation call (incl. lock wait). */
	atomic_t	contenders;
	atomic_t	max_contenders;
	atomic_t	started;
	atomic_t	done;
	int		nthreads;
};

/* Lock-free running maximum; cmpxchg retries until the value only grows. */
static void fabrictest_bump_max(atomic_t *max, int cur)
{
	int old = atomic_read(max);

	while (cur > old)
		old = atomic_cmpxchg(max, old, cur);
}

/* Record concurrent callback entry, then sleep to widen the overlap window. */
static void fabrictest_conc_enter(struct fabrictest_conc_ctx *ctx)
{
	fabrictest_bump_max(&ctx->max_in_flight,
			    atomic_inc_return(&ctx->in_flight));
	atomic_inc(&ctx->calls);
	usleep_range(20, 60);
	atomic_dec(&ctx->in_flight);
}

static int fabrictest_mock_port_set(struct drm_fabric_port *port,
				    enum drm_fabric_admin_state admin)
{
	fabrictest_conc_enter(port->endpoint->priv);
	return 0;
}

static int fabrictest_mock_endpoint_set(struct drm_fabric_endpoint *ep,
					const struct drm_fabric_endpoint_change *change,
					struct drm_fabric *fabric)
{
	fabrictest_conc_enter(ep->priv);
	return 0;
}

static const struct drm_fabric_ops fabrictest_conc_ops = {
	.port_set = fabrictest_mock_port_set,
	.endpoint_set = fabrictest_mock_endpoint_set,
};

struct fabrictest_worker {
	struct drm_fabric_endpoint	*ep;
	struct drm_fabric_port		*port;
	struct fabrictest_conc_ctx	*ctx;
	int				kind;	/* 0: PORT_SET, 1: ENDPOINT_SET */
	int				iters;
};

/*
 * All worker state lives in one kunit-managed allocation so the kthreads never
 * dereference the test function's stack.  Combined with the per-thread stop
 * action below, an assert-abort during spawn can still reap every worker before
 * its backing memory (and the endpoint it touches) is torn down.
 */
struct fabrictest_conc_harness {
	struct fabrictest_conc_ctx	ctx;
	struct fabrictest_worker	workers[FABRICTEST_CONC_THREADS];
	struct task_struct		*threads[FABRICTEST_CONC_THREADS];
};

static int fabrictest_mutator(void *arg)
{
	struct fabrictest_worker *w = arg;
	struct fabrictest_conc_ctx *ctx = w->ctx;
	unsigned long deadline;
	int i;

	/*
	 * Barrier: don't start hammering until every worker is up, so the
	 * contention window is as wide as possible.
	 */
	atomic_inc(&ctx->started);
	deadline = jiffies + msecs_to_jiffies(1000);
	while (atomic_read(&ctx->started) < ctx->nthreads &&
	       time_before(jiffies, deadline))
		cond_resched();

	for (i = 0; i < w->iters; i++) {
		enum drm_fabric_admin_state admin =
			(i & 1) ? DRM_FABRIC_ADMIN_STATE_UP : DRM_FABRIC_ADMIN_STATE_DOWN;

		/*
		 * Count threads in/awaiting the mutator (the locked wrapper
		 * blocks on drm_fabric_mutation_lock if another worker holds it),
		 * so the test can prove real contention happened rather than
		 * passing vacuously.
		 */
		fabrictest_bump_max(&ctx->max_contenders,
				    atomic_inc_return(&ctx->contenders));
		if (w->kind == 0) {
			fabrictest_port_admin_locked(w->port, admin);
		} else {
			struct drm_fabric_endpoint_change change = {
				.valid = DRM_FABRIC_EP_CHANGE_ADMIN,
				.admin = admin,
			};

			fabrictest_ep_set_locked(w->ep, &change);
		}
		atomic_dec(&ctx->contenders);
		cond_resched();
	}

	atomic_inc(&ctx->done);

	/* Idle until the test reaps us so the threadfn never exits early. */
	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(2));

	return 0;
}

/*
 * FABRICTEST_CONC_THREADS racers alternate port-admin and endpoint-admin
 * mutators; mutation_lock must serialize them into the provider hook.
 */
static void drm_fabric_test_concurrent_mutation(struct kunit *test)
{
	struct device *fabrictest_dev = fabrictest_alloc_dev(test);
	struct drm_fabric_desc fdesc = {
		.type = DRM_FABRIC_TYPE_SYNTHETIC,
		.name = "test-conc",
	};
	struct drm_fabric_port_desc pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
	struct fabrictest_conc_harness *h;
	struct drm_fabric_endpoint_desc edesc;
	struct drm_fabric *fab;
	struct drm_fabric_endpoint *ep;
	struct drm_fabric_port *port;
	unsigned long deadline;
	int i;

	h = kunit_kzalloc(test, sizeof(*h), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, h);
	h->ctx.nthreads = FABRICTEST_CONC_THREADS;

	edesc = (struct drm_fabric_endpoint_desc){
		.fabric_ep_id = 0xC0,
		.name = "conc-ep",
		.parent = fabrictest_dev,
		.ops = &fabrictest_conc_ops,
		.priv = &h->ctx,
		.ports = &pdesc,
		.num_ports = 1,
	};

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

	for (i = 0; i < FABRICTEST_CONC_THREADS; i++) {
		h->workers[i] = (struct fabrictest_worker){
			.ep = ep,
			.port = port,
			.ctx = &h->ctx,
			.kind = i & 1,
			.iters = FABRICTEST_CONC_ITERS,
		};
		h->threads[i] = kthread_run(fabrictest_mutator, &h->workers[i],
					    "fabrtest-conc/%d", i);
		KUNIT_ASSERT_FALSE(test, IS_ERR(h->threads[i]));
		/* Reap this worker if a later assertion aborts the test. */
		KUNIT_ASSERT_EQ(test, 0,
				kunit_add_action_or_reset(test,
							  fabrictest_stop_thread,
							  h->threads[i]));
	}

	deadline = jiffies + msecs_to_jiffies(10000);
	while (atomic_read(&h->ctx.done) < FABRICTEST_CONC_THREADS &&
	       time_before(jiffies, deadline))
		schedule_timeout_interruptible(msecs_to_jiffies(20));

	/* Correctness: no deadlock / lost wakeup, every worker completed. */
	KUNIT_EXPECT_EQ(test, atomic_read(&h->ctx.done), FABRICTEST_CONC_THREADS);
	KUNIT_EXPECT_GT(test, atomic_read(&h->ctx.calls), 0);

	/* Correctness: the object model is consistent after the storm. */
	KUNIT_EXPECT_PTR_EQ(test, ep->fabric, fab);
	KUNIT_EXPECT_LE(test, (int)port->admin_state, (int)DRM_FABRIC_ADMIN_STATE_UP);
	KUNIT_EXPECT_LE(test, (int)ep->admin_state, (int)DRM_FABRIC_ADMIN_STATE_UP);

	/*
	 * Prove real contention occurred while the provider callback stayed
	 * serialized.
	 */
	KUNIT_EXPECT_GE_MSG(test, atomic_read(&h->ctx.max_contenders), 2,
			    "workers never contended; concurrency not exercised");

	/*
	 * Provider callbacks must not overlap. max_contenders >= 2 makes this
	 * assertion non-vacuous.
	 */
	KUNIT_EXPECT_EQ_MSG(test, atomic_read(&h->ctx.max_in_flight), 1,
			    "provider hooks overlapped; mutations did not serialise");
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
	struct drm_fabric_port_desc leaf_pdesc = {
		.index = 0, .max_lane_count = 4,
		.peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER,
	};
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
		pdescs[j].peer_mode = DRM_FABRIC_PEER_MODE_PROVIDER;
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
	KUNIT_CASE(drm_fabric_test_endpoint_orphan_register),
	KUNIT_CASE(drm_fabric_test_endpoint_requires_port_array),
	KUNIT_CASE(drm_fabric_test_endpoint_register_stale_fabric),
	KUNIT_CASE(drm_fabric_test_unregister_rejects_non_member),
	KUNIT_CASE(drm_fabric_test_unregister_rejects_same_id),
	KUNIT_CASE(drm_fabric_test_ep_membership_rejects_non_member),
	KUNIT_CASE(drm_fabric_test_ep_membership_rejects_same_id),
	KUNIT_CASE(drm_fabric_test_endpoint_find_by_dev_name_contract),
	KUNIT_CASE(drm_fabric_test_port_register_peer),
	KUNIT_CASE(drm_fabric_test_port_find_variants),
	KUNIT_CASE(drm_fabric_test_port_find_get_pin_balance),
	KUNIT_CASE(drm_fabric_test_base_seq_advances),
	KUNIT_CASE(drm_fabric_test_generation_wrap_nonzero),
	KUNIT_CASE(drm_fabric_test_port_state_transition),
	KUNIT_CASE(drm_fabric_test_port_oper_state_rejects_invalid),
	KUNIT_CASE(drm_fabric_test_register_rejects_invalid_type),
	KUNIT_CASE(drm_fabric_test_mesh_kn_topology),
	KUNIT_CASE(drm_fabric_test_orphan_attach_detach),
	KUNIT_CASE(drm_fabric_test_endpoint_set_direct_reassign_busy),
	KUNIT_CASE(drm_fabric_test_endpoint_set_detach_attach_keeps_ep_id),
	KUNIT_CASE(drm_fabric_test_endpoint_register_admin_defaults),
	KUNIT_CASE(drm_fabric_test_endpoint_attach_preserves_admin_state),
	KUNIT_CASE(drm_fabric_test_endpoint_set_attach_enable_single_transition),
	KUNIT_CASE(drm_fabric_test_endpoint_detach_while_admin_up),
	KUNIT_CASE(drm_fabric_test_endpoint_set_combined_change_fails_no_commit),
	KUNIT_CASE(drm_fabric_test_endpoint_set_unassigned_ep_id_namespace),
	KUNIT_CASE(drm_fabric_test_ep_id_unique),
	KUNIT_CASE(drm_fabric_test_failed_mutation_no_commit),
	KUNIT_CASE(drm_fabric_test_port_admin_peer),
	KUNIT_CASE(drm_fabric_test_peer_mode),
	KUNIT_CASE(drm_fabric_test_model_a_oper_report),
	KUNIT_CASE_SLOW(drm_fabric_test_unregister_serializes_mutation),
	KUNIT_CASE_SLOW(drm_fabric_test_attach_register_collision),
	KUNIT_CASE(drm_fabric_test_port_stats_ops_registration),
	KUNIT_CASE(drm_fabric_test_user_fabric_new_del),
	KUNIT_CASE(drm_fabric_test_user_fabric_new_reject_no_module_ref),
	KUNIT_CASE(drm_fabric_test_reject_paths),
	KUNIT_CASE(drm_fabric_test_local_unplug_keeps_edge),
	KUNIT_CASE(drm_fabric_test_remote_peer_retained),
	KUNIT_CASE(drm_fabric_test_subtree_delete_single_bump),
	KUNIT_CASE_SLOW(drm_fabric_test_concurrent_mutation),
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
