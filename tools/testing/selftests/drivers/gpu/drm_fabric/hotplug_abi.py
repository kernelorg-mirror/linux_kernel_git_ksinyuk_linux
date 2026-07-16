#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Endpoint hotplug via fabricsim's debugfs lifecycle controls (add_endpoint/
del_endpoint, cf. netdevsim's new_port/del_port): CREATE/DELETE events
observed over the read-only query ABI and notifications; only the hotplug
stimulus uses the debugfs controls.

Usage: hotplug_abi.py [--no-load]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

EVT_DURATION = float(os.environ.get("EVT_DURATION", "3"))
# Subscription is synchronous (setsockopt); a brief settle suffices before
# triggering, after which wait_ntf() polls with a deadline.
EVT_SETTLE = float(os.environ.get("EVT_SETTLE", "0.2"))


def eps_by_name(fab):
    return {e["endpoint"]["name"]: e["endpoint"]
            for e in fab.dump("endpoint-get", {})}


def fabricsim_fid(fab):
    for f in fab.dump("fabric-get", {}):
        if f["fabric"]["name"] == "fabricsim":
            return f["fabric"]["fabric-id"]
    return None


def slot_of(name):
    return int(name.rsplit("ep", 1)[1])


def add_ep(fab, control, nports=None):
    """Add via debugfs; diff the name set (polling) to return the new endpoint."""
    before = set(eps_by_name(fab))
    L.dbg_write(control, nports if nports is not None else 1)
    new = L.wait_until(lambda: set(eps_by_name(fab)) - before)
    if len(new) != 1:
        return None
    return eps_by_name(fab)[next(iter(new))]


def del_ep(fab, slot, name):
    L.dbg_write("del_endpoint", slot)
    return L.wait_until(lambda: name not in eps_by_name(fab))


class Cfg:
    def __init__(self, fab, fid, nl_error):
        self.fab = fab
        self.fid = fid
        self.NlError = nl_error

def _gen(fab):
    """Current global topology-generation, read via a stable initial port."""
    return fab.do("port-get",
                  {"endpoint-id": 0, "port-index": 0}).get("topology-generation")


def test_provider_topology_lifecycle(ksft, cfg):
    """Provider grows and shrinks the topology within its fabric.

    Non-destructive: only the endpoints it adds are removed.
    """
    fab = cfg.fab

    # (a) provider-established initial adjacency (mesh) is observable read-only.
    p0 = fab.do("port-get", {"endpoint-id": 0, "port-index": 0})["port"]
    peer = p0.get("peer")
    ksft.check(peer is not None and peer.get("type") == "accel",
               "topology-initial-adjacency-visible", "peer=%s" % peer)

    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)

    n0 = len(eps_by_name(fab))
    g0 = _gen(fab)

    # (b) grow: add two endpoints; the first is asserted to announce a CREATE.
    ep_c = add_ep(fab, "add_endpoint", nports=2)
    c_evt = L.wait_ntf(ev, "endpoint-create-ntf", timeout=EVT_DURATION)
    ep_d = add_ep(fab, "add_endpoint", nports=2)

    if ep_c is None or ep_d is None:
        for e in (ep_c, ep_d):
            if e:
                del_ep(fab, slot_of(e["name"]), e["name"])
        for name in ("topology-grow-two-members", "topology-grow-create-ntf",
                     "topology-grow-advances-generation",
                     "topology-hotplug-is-member",
                     "topology-hotplug-endpoint-unlinked",
                     "topology-reads-do-not-advance-generation",
                     "topology-shrink-delete-ntf",
                     "topology-shrink-restores-baseline"):
            ksft.not_ok(name, "grow failed (c=%s d=%s)" % (ep_c, ep_d))
        return

    ksft.check(len(eps_by_name(fab)) == n0 + 2, "topology-grow-two-members",
               "n0=%d now=%d" % (n0, len(eps_by_name(fab))))
    ksft.check(c_evt is not None, "topology-grow-create-ntf")

    g_grown = _gen(fab)
    ksft.check(g0 is not None and g_grown is not None and g_grown > g0,
               "topology-grow-advances-generation",
               "g0=%s grown=%s" % (g0, g_grown))
    ksft.check(ep_c.get("fabric-id") == cfg.fid, "topology-hotplug-is-member",
               "fabric-id=%s" % ep_c.get("fabric-id"))

    # (c) a late arrival is not auto-wired: the provider links explicitly.
    pc = fab.do("port-get",
                {"endpoint-id": ep_c["endpoint-id"], "port-index": 0})["port"]
    ksft.check(pc.get("peer") is None, "topology-hotplug-endpoint-unlinked",
               "unexpected peer=%s" % pc.get("peer"))

    # (d) pure reads (dump + stats GET) never advance generation.
    g_pre_reads = _gen(fab)
    eps_by_name(fab)
    fab.do("port-stats-get", {"endpoint-id": ep_c["endpoint-id"],
                              "port-index": 0})
    g_post_reads = _gen(fab)
    ksft.check(g_post_reads == g_pre_reads,
               "topology-reads-do-not-advance-generation",
               "pre=%s post=%s" % (g_pre_reads, g_post_reads))

    # shrink back to baseline; assert one DELETE event and the restored count.
    ev2 = L.DrmFabric()
    ev2.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    del_ep(fab, slot_of(ep_d["name"]), ep_d["name"])
    d_evt = L.wait_ntf(ev2, "endpoint-delete-ntf", timeout=EVT_DURATION)
    del_ep(fab, slot_of(ep_c["name"]), ep_c["name"])
    ksft.check(d_evt is not None, "topology-shrink-delete-ntf")
    ksft.check(len(eps_by_name(fab)) == n0, "topology-shrink-restores-baseline",
               "n0=%d now=%d" % (n0, len(eps_by_name(fab))))



def test_hotplug_lifecycle(ksft, cfg):
    """Hotplug one endpoint and unplug it: CREATE, DELETE, membership."""
    fab = cfg.fab
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    n_before = len(eps_by_name(fab))
    new_ep = add_ep(fab, "add_endpoint", nports=2)
    add_evt = L.wait_ntf(ev, "endpoint-create-ntf", timeout=EVT_DURATION)

    ksft.check(new_ep is not None and len(eps_by_name(fab)) == n_before + 1,
               "hotplug-add-appears",
               "n_before=%d new=%s" % (n_before, new_ep))
    ksft.check(add_evt is not None, "hotplug-add-endpoint-create-ntf")
    if new_ep is None:
        ksft.not_ok("hotplug-add-is-member", "add_endpoint produced no endpoint")
        ksft.not_ok("hotplug-del-disappears", "add failed")
        ksft.not_ok("hotplug-del-endpoint-delete-ntf", "add failed")
        return

    name = new_ep["name"]
    ksft.check(new_ep.get("fabric-id") == cfg.fid, "hotplug-add-is-member",
               "fabric-id=%s" % new_ep.get("fabric-id"))

    try:
        ev = L.DrmFabric()
        ev.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        gone = del_ep(fab, slot_of(name), name)
        del_evt = L.wait_ntf(ev, "endpoint-delete-ntf", timeout=EVT_DURATION)
        ksft.check(gone, "hotplug-del-disappears")
        ksft.check(del_evt is not None, "hotplug-del-endpoint-delete-ntf")
    finally:
        if name in eps_by_name(fab):
            del_ep(fab, slot_of(name), name)


CASES = (
    test_provider_topology_lifecycle,
    test_hotplug_lifecycle,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, need_debugfs=True, need_control="add_endpoint") as fab:
        fid = fabricsim_fid(fab)
        if fid is None:
            ksft.skip_all("fabricsim fabric not present")

        L.run_cases(ksft, Cfg(fab, fid, NlError), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
