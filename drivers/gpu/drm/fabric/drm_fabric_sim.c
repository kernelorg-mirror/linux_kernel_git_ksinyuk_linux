// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>

#include <drm/drm_fabric.h>
#include <uapi/drm/drm_fabric.h>

/**
 * DOC: fabricsim
 *
 * In-kernel synthetic drm_fabric provider for review and CI. It registers a
 * linear, mesh or switch topology and drives the selftests through debugfs:
 * synthetic counters, port-state transitions, endpoint hotplug, a bulk
 * population for dump-resume testing, and fault injection. The debugfs hooks
 * are not uAPI; userspace observes the resulting state through the Generic
 * Netlink ABI.
 */

static char *topology = "mesh";
module_param(topology, charp, 0444);
MODULE_PARM_DESC(topology, "Topology shape: linear, mesh, switch (default: mesh)");

static int num_endpoints = 4;
module_param(num_endpoints, int, 0444);
MODULE_PARM_DESC(num_endpoints, "Number of endpoints (2-8, default: 4)");

static int ports_per_ep = 4;
module_param(ports_per_ep, int, 0444);
MODULE_PARM_DESC(ports_per_ep, "Ports per endpoint (1-16, default: 4)");

struct fabricsim_port_priv {
	struct drm_fabric_port *port;

	/* Bumped lockless from the timer and debugfs; untorn on 32-bit. */
	atomic64_t read_bytes;
	atomic64_t write_bytes;
	atomic64_t link_down_count;
	atomic64_t retrain_count;

	/* Blocks enable while disable drains a pending timer re-arm. */
	struct mutex activity_lock;
	bool activity_enabled;
	struct timer_list activity_timer;
	u32 read_rate;		/* bytes per timer tick (FABRICSIM_TICK_MS) */
	u32 write_rate;		/* bytes per timer tick (FABRICSIM_TICK_MS) */

	/*
	 * Test-only: returns -stats_errno, with values above MAX_ERRNO mapped
	 * to -EIO.
	 */
	u32 stats_errno;
};

struct fabricsim_ep_priv {
	struct drm_fabric_endpoint *ep;
	struct platform_device *pdev;	/* backing device for dev-name/bus-name */
	struct fabricsim_port_priv *ports;
	int num_ports;
	int slot;
	bool runtime;
	struct dentry *dbg_dir;
};

#define FABRICSIM_MAX_EPS 512
/* Initial generated topology only; runtime hotplug uses FABRICSIM_MAX_EPS. */
#define FABRICSIM_MAX_INIT_EPS 8

static struct drm_fabric *fabricsim_fabric;
static struct fabricsim_ep_priv *fabricsim_slots[FABRICSIM_MAX_EPS];
static int fabricsim_init_eps;
static bool fabricsim_exiting;		/* gate runtime controls during teardown */
/*
 * Nests outside drm_fabric_lock and is never taken from the provider ops or the
 * debugfs port handlers, so the two cannot invert.
 */
static DEFINE_MUTEX(fabricsim_lock);
static struct dentry *fabricsim_debugfs_root;

/* Test-only fault injection (debugfs). Sticky until cleared. */
static bool fabricsim_fail_register;
static u32 fabricsim_fail_errno = ENOMEM;

/*
 * Negative errno an armed fault returns; zero or an out-of-range value
 * gives -ENOMEM.
 */
static int fabricsim_injected_errno(void)
{
	u32 e = fabricsim_fail_errno;

	if (e == 0 || e > MAX_ERRNO)
		return -ENOMEM;
	return -(int)e;
}

static int fabricsim_port_stats_get(struct drm_fabric_port *port,
				    struct drm_fabric_port_stats *stats)
{
	struct fabricsim_ep_priv *ep_priv = port->endpoint->priv;
	struct fabricsim_port_priv *pp;

	/*
	 * The endpoint is pinned by the core; missing private state is a
	 * provider bug.
	 */
	if (!ep_priv)
		return -ENOENT;

	/*
	 * Port i has index i. Index rather than search for a matching ->port,
	 * which a dump racing registration would not yet see.
	 */
	if (port->index >= (u32)ep_priv->num_ports)
		return -ENOENT;

	pp = &ep_priv->ports[port->index];

	/* Dumps skip -EOPNOTSUPP; any other injected error aborts the dump. */
	if (pp->stats_errno) {
		u32 e = pp->stats_errno;

		return e <= MAX_ERRNO ? -(int)e : -EIO;
	}

	stats->read_bytes = atomic64_read(&pp->read_bytes);
	stats->write_bytes = atomic64_read(&pp->write_bytes);
	stats->link_down_count = atomic64_read(&pp->link_down_count);
	stats->retrain_count = atomic64_read(&pp->retrain_count);

	return 0;
}

static const struct drm_fabric_ops fabricsim_ops = {
	.port_stats_get		= fabricsim_port_stats_get,
};

#define FABRICSIM_TICK_MS 100

static void fabricsim_activity_tick(struct timer_list *t)
{
	struct fabricsim_port_priv *pp =
		container_of(t, struct fabricsim_port_priv, activity_timer);

	/*
	 * Paired with WRITE_ONCE() in the enable/disable path, so a stopped
	 * timer stays stopped.
	 */
	if (!READ_ONCE(pp->activity_enabled))
		return;

	atomic64_add(READ_ONCE(pp->read_rate), &pp->read_bytes);
	atomic64_add(READ_ONCE(pp->write_rate), &pp->write_bytes);

	mod_timer(&pp->activity_timer,
		  jiffies + msecs_to_jiffies(FABRICSIM_TICK_MS));
}

static ssize_t fabricsim_activity_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	struct fabricsim_port_priv *pp = file->private_data;
	char kbuf[8];
	int val;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (kstrtoint(kbuf, 10, &val))
		return -EINVAL;

	/*
	 * Held across the whole transition, or a concurrent enable could re-arm
	 * mid-drain.
	 */
	scoped_guard(mutex, &pp->activity_lock) {
		if (val && !pp->activity_enabled) {
			WRITE_ONCE(pp->activity_enabled, true);
			mod_timer(&pp->activity_timer,
				  jiffies + msecs_to_jiffies(FABRICSIM_TICK_MS));
		} else if (!val && pp->activity_enabled) {
			WRITE_ONCE(pp->activity_enabled, false);
			/*
			 * Not timer_shutdown_sync(): a later enable re-arms.
			 * A tick past the enabled check can re-arm after one
			 * timer_delete_sync(), so loop until a drain is clean.
			 */
			while (timer_delete_sync(&pp->activity_timer))
				;
		}
	}

	return count;
}

static ssize_t fabricsim_activity_read(struct file *file,
				       char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct fabricsim_port_priv *pp = file->private_data;
	char kbuf[4];
	int len;

	len = scnprintf(kbuf, sizeof(kbuf), "%d\n",
			READ_ONCE(pp->activity_enabled) ? 1 : 0);
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

/* Per-port debugfs files use fabricsim_port_priv as file->private_data. */
static const struct file_operations fabricsim_activity_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= fabricsim_activity_read,
	.write	= fabricsim_activity_write,
};

static ssize_t fabricsim_inject_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	struct fabricsim_port_priv *pp = file->private_data;
	char kbuf[32];

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (count > 0 && kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	if (strcmp(kbuf, "link_down") == 0) {
		atomic64_inc(&pp->link_down_count);
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_INACTIVE);
	} else if (strcmp(kbuf, "degrade") == 0) {
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_DEGRADED);
	} else if (strcmp(kbuf, "recover_to_active") == 0) {
		atomic64_inc(&pp->retrain_count);
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_ACTIVE);
	} else {
		return -EINVAL;
	}

	return count;
}

static const struct file_operations fabricsim_inject_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= fabricsim_inject_write,
};

static ssize_t fabricsim_oper_state_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	struct fabricsim_port_priv *pp = file->private_data;
	char kbuf[16];

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (count > 0 && kbuf[count - 1] == '\n')
		kbuf[count - 1] = '\0';

	if (strcmp(kbuf, "unknown") == 0)
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_UNKNOWN);
	else if (strcmp(kbuf, "inactive") == 0)
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_INACTIVE);
	else if (strcmp(kbuf, "active") == 0)
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_ACTIVE);
	else if (strcmp(kbuf, "degraded") == 0)
		drm_fabric_port_set_oper(pp->port, DRM_FABRIC_PORT_STATE_DEGRADED);
	else
		return -EINVAL;

	return count;
}

static ssize_t fabricsim_oper_state_read(struct file *file,
					 char __user *buf,
					 size_t count, loff_t *ppos)
{
	struct fabricsim_port_priv *pp = file->private_data;
	const char *state_str;
	char kbuf[16];
	int len;

	switch (pp->port->oper_state) {
	case DRM_FABRIC_PORT_STATE_INACTIVE:
		state_str = "inactive";
		break;
	case DRM_FABRIC_PORT_STATE_ACTIVE:
		state_str = "active";
		break;
	case DRM_FABRIC_PORT_STATE_DEGRADED:
		state_str = "degraded";
		break;
	default:
		state_str = "unknown";
		break;
	}

	len = scnprintf(kbuf, sizeof(kbuf), "%s\n", state_str);
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations fabricsim_oper_state_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= fabricsim_oper_state_read,
	.write	= fabricsim_oper_state_write,
};

static void fabricsim_link_linear(void)
{
	int i;
	int port_cursor[FABRICSIM_MAX_INIT_EPS] = {0};
	struct drm_fabric_peer peer;

	/* Linear chain: ep[0]<->ep[1]<->ep[2]<->...<->ep[N-1] */
	for (i = 0; i < fabricsim_init_eps - 1; i++) {
		int pa_idx = port_cursor[i]++;
		int pb_idx = port_cursor[i + 1]++;
		struct drm_fabric_endpoint *ep_a = fabricsim_slots[i]->ep;
		struct drm_fabric_endpoint *ep_b = fabricsim_slots[i + 1]->ep;
		struct drm_fabric_port *pa, *pb;

		/*
		 * Interior nodes consume two ports; stop rather than walk off
		 * an endpoint's port array if it was sized too small.
		 */
		if (pa_idx >= fabricsim_slots[i]->num_ports ||
		    pb_idx >= fabricsim_slots[i + 1]->num_ports)
			break;

		pa = fabricsim_slots[i]->ports[pa_idx].port;
		pb = fabricsim_slots[i + 1]->ports[pb_idx].port;

		/* Peers are directed half-edges, so install both directions. */
		peer.peer_id = ep_b->fabric_ep_id;
		peer.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL;
		peer.port_index = pb->index;
		drm_fabric_port_set_peer(pa, &peer);

		peer.peer_id = ep_a->fabric_ep_id;
		peer.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL;
		peer.port_index = pa->index;
		drm_fabric_port_set_peer(pb, &peer);
	}
}

static void fabricsim_link_mesh(void)
{
	int i, j, k, port_idx, peer_port_idx;
	struct drm_fabric_peer peer;

	/* Fully-connected K_N: port j on ep[i] reaches ep[j], shifted past i. */
	for (i = 0; i < fabricsim_init_eps; i++) {
		port_idx = 0;
		for (j = 0; j < fabricsim_init_eps; j++) {
			if (i == j)
				continue;

			if (port_idx >= fabricsim_slots[i]->num_ports)
				break;

			/*
			 * On endpoint j, i uses the slot obtained by skipping j
			 * in endpoint order.
			 */
			peer_port_idx = 0;
			for (k = 0; k < fabricsim_init_eps; k++) {
				if (k == j)
					continue;
				if (k == i)
					break;
				peer_port_idx++;
			}

			peer.peer_id = fabricsim_slots[j]->ep->fabric_ep_id;
			peer.peer_type = DRM_FABRIC_PEER_TYPE_ACCEL;
			peer.port_index = peer_port_idx;
			drm_fabric_port_set_peer(fabricsim_slots[i]->ports[port_idx].port,
						 &peer);

			port_idx++;
		}
	}
}

/*
 * Opaque switch peer-id, deliberately outside the leaf range (0x100 + slot)
 * so it never resolves in the endpoint registry.
 */
#define FABRICSIM_SWITCH_FABRIC_EP_ID 0x5000ULL

static void fabricsim_link_switch(void)
{
	struct drm_fabric_peer peer;
	int i;

	for (i = 0; i < fabricsim_init_eps; i++) {
		struct drm_fabric_port *leaf_port =
			fabricsim_slots[i]->ports[0].port;

		if (!leaf_port)
			continue;

		/* One directed half-edge from the leaf to an opaque switch. */
		peer.peer_id = FABRICSIM_SWITCH_FABRIC_EP_ID;
		peer.peer_type = DRM_FABRIC_PEER_TYPE_SWITCH;
		peer.port_index = i; /* distinct switch-side port per leaf */
		drm_fabric_port_set_peer(leaf_port, &peer);
	}
}

static void fabricsim_ep_debugfs_create(struct fabricsim_ep_priv *ep_priv)
{
	struct dentry *port_dir;
	char name[32];
	int j;

	if (IS_ERR_OR_NULL(fabricsim_debugfs_root))
		return;

	snprintf(name, sizeof(name), "ep%d", ep_priv->slot);
	ep_priv->dbg_dir = debugfs_create_dir(name, fabricsim_debugfs_root);
	if (IS_ERR_OR_NULL(ep_priv->dbg_dir)) {
		ep_priv->dbg_dir = NULL;
		return;
	}

	for (j = 0; j < ep_priv->num_ports; j++) {
		struct fabricsim_port_priv *pp = &ep_priv->ports[j];

		snprintf(name, sizeof(name), "port%d", j);
		port_dir = debugfs_create_dir(name, ep_priv->dbg_dir);

		debugfs_create_file("activity_enable", 0644,
				    port_dir, pp, &fabricsim_activity_fops);
		debugfs_create_file("inject", 0200,
				    port_dir, pp, &fabricsim_inject_fops);
		debugfs_create_file("oper_state", 0644,
				    port_dir, pp, &fabricsim_oper_state_fops);
		debugfs_create_u32("read_rate", 0644, port_dir, &pp->read_rate);
		debugfs_create_u32("write_rate", 0644, port_dir, &pp->write_rate);
		debugfs_create_u32("stats_errno", 0644, port_dir, &pp->stats_errno);
	}
}

/*
 * Create one endpoint at @slot with @nports ports, registered as a member of
 * the synthetic fabric.  Returns the new ep_priv or an ERR_PTR.  Caller holds
 * fabricsim_lock.
 */
static struct fabricsim_ep_priv *fabricsim_make_ep(int slot, int nports)
{
	struct drm_fabric_endpoint_desc edesc = {};
	struct drm_fabric_port_desc pdescs[16];
	struct fabricsim_ep_priv *ep_priv;
	struct platform_device *pdev;
	char ep_name[32];
	int j, ret;

	lockdep_assert_held(&fabricsim_lock);

	if (nports < 1)
		nports = 1;
	if (nports > 16)
		nports = 16;

	/* Refuse before any allocation, so there is nothing to roll back. */
	if (fabricsim_fail_register)
		return ERR_PTR(fabricsim_injected_errno());

	ep_priv = kzalloc_obj(*ep_priv, GFP_KERNEL);
	if (!ep_priv)
		return ERR_PTR(-ENOMEM);

	ep_priv->slot = slot;
	ep_priv->num_ports = nports;

	pdev = platform_device_register_simple("fabricsim", slot, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_free;
	}
	ep_priv->pdev = pdev;

	for (j = 0; j < nports; j++) {
		pdescs[j].index = j;
		pdescs[j].max_lane_count = 4;
		pdescs[j].max_lane_signaling_rate_mbps = 200000; /* 200 Gbps/lane */
	}

	snprintf(ep_name, sizeof(ep_name), "sim-ep%d", slot);
	edesc.fabric_ep_id = 0x100 + slot;
	edesc.name = ep_name;
	edesc.parent = &pdev->dev;
	edesc.ops = &fabricsim_ops;
	edesc.priv = ep_priv;
	edesc.ports = pdescs;
	edesc.num_ports = nports;

	/*
	 * Fill the port array before registering: a racing PORT_STATS_GET can
	 * hit any port once published.
	 */
	ep_priv->ports = kcalloc(nports, sizeof(struct fabricsim_port_priv),
				 GFP_KERNEL);
	if (!ep_priv->ports) {
		ret = -ENOMEM;
		goto err_pdev;
	}

	for (j = 0; j < nports; j++) {
		struct fabricsim_port_priv *pp = &ep_priv->ports[j];

		pp->read_rate = 1024;
		pp->write_rate = 512;
		mutex_init(&pp->activity_lock);
		timer_setup(&pp->activity_timer, fabricsim_activity_tick, 0);
	}

	ep_priv->ep = drm_fabric_endpoint_register(fabricsim_fabric, &edesc);
	if (IS_ERR(ep_priv->ep)) {
		ret = PTR_ERR(ep_priv->ep);
		goto err_ports;
	}

	/*
	 * Fill the simulator port pointers before topology wiring and before
	 * the debugfs nodes make them externally reachable.
	 */
	for (j = 0; j < nports; j++)
		ep_priv->ports[j].port = drm_fabric_endpoint_port(ep_priv->ep, j);

	fabricsim_ep_debugfs_create(ep_priv);

	return ep_priv;

err_ports:
	for (j = 0; j < nports; j++)
		mutex_destroy(&ep_priv->ports[j].activity_lock);
	kfree(ep_priv->ports);
err_pdev:
	platform_device_unregister(ep_priv->pdev);
err_free:
	kfree(ep_priv);
	return ERR_PTR(ret);
}

/* Peers on other endpoints are left untouched. Caller holds fabricsim_lock. */
static void fabricsim_destroy_ep(struct fabricsim_ep_priv *ep_priv)
{
	int j;

	lockdep_assert_held(&fabricsim_lock);

	debugfs_remove_recursive(ep_priv->dbg_dir);

	for (j = 0; j < ep_priv->num_ports; j++) {
		struct fabricsim_port_priv *pp = &ep_priv->ports[j];

		/*
		 * debugfs_remove_recursive() drained writers before the
		 * activity timer is shut down.
		 */
		scoped_guard(mutex, &pp->activity_lock) {
			WRITE_ONCE(pp->activity_enabled, false);
			timer_shutdown_sync(&pp->activity_timer);
		}
		mutex_destroy(&pp->activity_lock);
	}

	drm_fabric_endpoint_unregister(ep_priv->ep);
	kfree(ep_priv->ports);
	platform_device_unregister(ep_priv->pdev);
	kfree(ep_priv);
}

static int fabricsim_add_endpoint(int nports)
{
	struct fabricsim_ep_priv *ep_priv;
	int slot, ret;

	mutex_lock(&fabricsim_lock);
	if (fabricsim_exiting) {
		mutex_unlock(&fabricsim_lock);
		return -ENODEV;
	}

	for (slot = 0; slot < FABRICSIM_MAX_EPS; slot++)
		if (!fabricsim_slots[slot])
			break;
	if (slot == FABRICSIM_MAX_EPS) {
		mutex_unlock(&fabricsim_lock);
		return -ENOSPC;
	}

	ep_priv = fabricsim_make_ep(slot, nports);
	if (IS_ERR(ep_priv)) {
		ret = PTR_ERR(ep_priv);
		mutex_unlock(&fabricsim_lock);
		return ret;
	}
	ep_priv->runtime = true;
	fabricsim_slots[slot] = ep_priv;
	mutex_unlock(&fabricsim_lock);

	return slot;
}

/* fabricsim_lock stays held across teardown, so the slot remains reserved. */
static int fabricsim_del_endpoint(int slot)
{
	struct fabricsim_ep_priv *ep_priv;

	if (slot < 0 || slot >= FABRICSIM_MAX_EPS)
		return -EINVAL;

	mutex_lock(&fabricsim_lock);
	if (fabricsim_exiting) {
		mutex_unlock(&fabricsim_lock);
		return -ENODEV;
	}
	ep_priv = fabricsim_slots[slot];
	if (!ep_priv) {
		mutex_unlock(&fabricsim_lock);
		return -ENOENT;
	}
	fabricsim_destroy_ep(ep_priv);
	fabricsim_slots[slot] = NULL;
	mutex_unlock(&fabricsim_lock);

	return 0;
}

/*
 * @n single-port member endpoints for the dump-scale selftest: population, not
 * topology. Returns the count added, or a negative errno only if none were.
 */
static int fabricsim_bulk_add(int n)
{
	int added = 0;
	int ret;

	if (n <= 0)
		return -EINVAL;

	while (added < n) {
		ret = fabricsim_add_endpoint(1);
		if (ret < 0)
			return added ? added : ret;
		added++;
	}
	return added;
}

/*
 * Ownership comes from ->runtime, not the slot index: slots are reused, so a
 * runtime endpoint can sit below the init population.
 */
static int fabricsim_bulk_del(void)
{
	int slot;

	for (slot = 0; slot < FABRICSIM_MAX_EPS; slot++) {
		mutex_lock(&fabricsim_lock);
		if (fabricsim_exiting) {
			mutex_unlock(&fabricsim_lock);
			return -ENODEV;
		}
		if (fabricsim_slots[slot] && fabricsim_slots[slot]->runtime) {
			fabricsim_destroy_ep(fabricsim_slots[slot]);
			fabricsim_slots[slot] = NULL;
		}
		mutex_unlock(&fabricsim_lock);
	}
	return 0;
}

static int fabricsim_parse_int(const char __user *buf, size_t count, int dflt)
{
	char kbuf[16];
	int val;

	if (count == 0 || count >= sizeof(kbuf))
		return dflt;
	if (copy_from_user(kbuf, buf, count))
		return dflt;
	kbuf[count] = '\0';
	if (kstrtoint(strim(kbuf), 10, &val))
		return dflt;
	return val;
}

static ssize_t fabricsim_add_ep_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	int nports = fabricsim_parse_int(buf, count, ports_per_ep);
	int ret = fabricsim_add_endpoint(nports);

	return ret < 0 ? ret : count;
}

static ssize_t fabricsim_del_ep_write(struct file *file, const char __user *buf,
				      size_t count, loff_t *ppos)
{
	int slot = fabricsim_parse_int(buf, count, -1);
	int ret = fabricsim_del_endpoint(slot);

	return ret < 0 ? ret : count;
}

static const struct file_operations fabricsim_add_ep_fops = {
	.owner	= THIS_MODULE,
	.write	= fabricsim_add_ep_write,
};

static const struct file_operations fabricsim_del_ep_fops = {
	.owner	= THIS_MODULE,
	.write	= fabricsim_del_ep_write,
};

static ssize_t fabricsim_bulk_add_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	int n = fabricsim_parse_int(buf, count, 0);
	int ret = fabricsim_bulk_add(n);

	return ret < 0 ? ret : count;
}

static ssize_t fabricsim_bulk_del_write(struct file *file,
					const char __user *buf,
					size_t count, loff_t *ppos)
{
	int ret = fabricsim_bulk_del();

	return ret < 0 ? ret : count;
}

static const struct file_operations fabricsim_bulk_add_fops = {
	.owner	= THIS_MODULE,
	.write	= fabricsim_bulk_add_write,
};

static const struct file_operations fabricsim_bulk_del_fops = {
	.owner	= THIS_MODULE,
	.write	= fabricsim_bulk_del_write,
};

static ssize_t fabricsim_fail_errno_write(struct file *file,
					  const char __user *buf,
					  size_t count, loff_t *ppos)
{
	u32 val;
	int ret;

	ret = kstrtou32_from_user(buf, count, 0, &val);
	if (ret)
		return ret;
	/* Zero stays the documented "clear to -ENOMEM" sentinel. */
	if (val > MAX_ERRNO)
		return -EINVAL;
	fabricsim_fail_errno = val;
	return count;
}

static ssize_t fabricsim_fail_errno_read(struct file *file, char __user *buf,
					 size_t count, loff_t *ppos)
{
	char kbuf[16];
	int len;

	len = scnprintf(kbuf, sizeof(kbuf), "%u\n", fabricsim_fail_errno);
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static const struct file_operations fabricsim_fail_errno_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= fabricsim_fail_errno_read,
	.write	= fabricsim_fail_errno_write,
};

/*
 * Validate and derive the module parameters before anything is registered.
 * This runs before allocation, so it needs no unwind path.
 */
static int __init fabricsim_setup_params(void)
{
	/*
	 * Reject an unrecognised topology rather than falling back to mesh, so
	 * a typo cannot fake a shape.
	 */
	if (strcmp(topology, "mesh") && strcmp(topology, "linear") &&
	    strcmp(topology, "switch")) {
		pr_err("fabricsim: unknown topology \"%s\" (use mesh, linear or switch)\n",
		       topology);
		return -EINVAL;
	}

	if (num_endpoints < 2)
		num_endpoints = 2;
	if (num_endpoints > FABRICSIM_MAX_INIT_EPS)
		num_endpoints = FABRICSIM_MAX_INIT_EPS;
	if (ports_per_ep < 1)
		ports_per_ep = 1;
	if (ports_per_ep > 16)
		ports_per_ep = 16;

	/*
	 * A mesh gives every endpoint (N-1) peers, so the busiest endpoint needs
	 * at least (N-1) ports. The switch shape only needs one port per leaf
	 * (a single half-edge to the opaque switch), so it is not bumped here.
	 */
	if (strcmp(topology, "mesh") == 0 && ports_per_ep < num_endpoints - 1)
		ports_per_ep = num_endpoints - 1;

	/*
	 * A linear chain gives every interior node two neighbours, so it needs
	 * at least two ports; bump a too-small request rather than index past
	 * the endpoint's port array.
	 */
	if (strcmp(topology, "linear") == 0 && num_endpoints > 2 &&
	    ports_per_ep < 2)
		ports_per_ep = 2;

	fabricsim_init_eps = num_endpoints;

	return 0;
}

static int __init fabricsim_init(void)
{
	struct drm_fabric_desc fdesc;
	int i, j, ret;

	ret = fabricsim_setup_params();
	if (ret)
		return ret;

	fdesc.type = DRM_FABRIC_TYPE_SYNTHETIC;
	fdesc.name = "fabricsim";
	fdesc.instance_id = 0x8086CAFE;

	fabricsim_fabric = drm_fabric_register(&fdesc);
	if (IS_ERR(fabricsim_fabric))
		return PTR_ERR(fabricsim_fabric);

	/* Root must exist before the per-endpoint debugfs subtrees. */
	fabricsim_debugfs_root = debugfs_create_dir("drm_fabric_sim", NULL);
	if (IS_ERR(fabricsim_debugfs_root))
		fabricsim_debugfs_root = NULL;

	mutex_lock(&fabricsim_lock);
	for (i = 0; i < fabricsim_init_eps; i++) {
		struct fabricsim_ep_priv *ep_priv =
			fabricsim_make_ep(i, ports_per_ep);

		if (IS_ERR(ep_priv)) {
			ret = PTR_ERR(ep_priv);
			mutex_unlock(&fabricsim_lock);
			goto err_eps;
		}
		fabricsim_slots[i] = ep_priv;
	}
	mutex_unlock(&fabricsim_lock);

	if (strcmp(topology, "linear") == 0)
		fabricsim_link_linear();
	else if (strcmp(topology, "switch") == 0)
		fabricsim_link_switch();
	else
		fabricsim_link_mesh();

	/* A linked port starts ACTIVE; an unlinked one stays as registered. */
	for (i = 0; i < fabricsim_init_eps; i++) {
		for (j = 0; j < fabricsim_slots[i]->num_ports; j++) {
			struct fabricsim_port_priv *pp = &fabricsim_slots[i]->ports[j];

			if (pp->port && pp->port->has_peer)
				drm_fabric_port_set_oper(pp->port,
							 DRM_FABRIC_PORT_STATE_ACTIVE);
		}
	}

	/* Root-level runtime lifecycle controls (test-only, not uAPI). */
	if (fabricsim_debugfs_root) {
		debugfs_create_file("add_endpoint", 0200, fabricsim_debugfs_root,
				    NULL, &fabricsim_add_ep_fops);
		debugfs_create_file("del_endpoint", 0200, fabricsim_debugfs_root,
				    NULL, &fabricsim_del_ep_fops);

		debugfs_create_file("bulk_add", 0200, fabricsim_debugfs_root,
				    NULL, &fabricsim_bulk_add_fops);
		debugfs_create_file("bulk_del", 0200, fabricsim_debugfs_root,
				    NULL, &fabricsim_bulk_del_fops);

		debugfs_create_bool("fail_register", 0644,
				    fabricsim_debugfs_root,
				    &fabricsim_fail_register);
		debugfs_create_file("fail_errno", 0644,
				    fabricsim_debugfs_root,
				    NULL, &fabricsim_fail_errno_fops);
	}

	pr_info("fabricsim: registered %s topology with %d endpoints, %d ports/ep\n",
		topology, fabricsim_init_eps, ports_per_ep);

	return 0;

err_eps:
	mutex_lock(&fabricsim_lock);
	for (i = FABRICSIM_MAX_EPS - 1; i >= 0; i--) {
		if (fabricsim_slots[i]) {
			fabricsim_destroy_ep(fabricsim_slots[i]);
			fabricsim_slots[i] = NULL;
		}
	}
	mutex_unlock(&fabricsim_lock);
	debugfs_remove_recursive(fabricsim_debugfs_root);
	WARN_ON(drm_fabric_unregister(fabricsim_fabric));
	return ret;
}

static void __exit fabricsim_exit(void)
{
	int i;

	/*
	 * fabricsim_exiting gates add/del first and the root subtree is removed
	 * last, so no handler can race this teardown.
	 */
	mutex_lock(&fabricsim_lock);
	fabricsim_exiting = true;
	for (i = FABRICSIM_MAX_EPS - 1; i >= 0; i--) {
		if (fabricsim_slots[i]) {
			fabricsim_destroy_ep(fabricsim_slots[i]);
			fabricsim_slots[i] = NULL;
		}
	}
	mutex_unlock(&fabricsim_lock);

	debugfs_remove_recursive(fabricsim_debugfs_root);
	WARN_ON(drm_fabric_unregister(fabricsim_fabric));

	pr_info("fabricsim: unloaded\n");
}

module_init(fabricsim_init);
module_exit(fabricsim_exit);

MODULE_AUTHOR("Intel Corporation");
MODULE_AUTHOR("Konstantin Sinyuk <ksinyuk@kernel.org>");
MODULE_DESCRIPTION("DRM Fabric fabricsim synthetic driver");
MODULE_LICENSE("Dual MIT/GPL");
