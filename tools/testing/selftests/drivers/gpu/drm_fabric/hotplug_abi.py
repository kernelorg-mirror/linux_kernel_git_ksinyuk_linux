#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Endpoint hotplug via fabricsim's debugfs lifecycle controls (cf. netdevsim's
new_port/del_port): CREATE/DELETE events and peer-unplug edge retention are
observed over the real ABI; ENDPOINT_SET/PORT_SET mutation is also issued over
the real (privileged) genetlink ABI -- only the hotplug stimulus itself uses
the test-only debugfs controls.

Usage: hotplug_abi.py [--no-load]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

EVT_DURATION = float(os.environ.get("EVT_DURATION", "3"))
# Window for asserting an event is ABSENT: a peer unplug emits (or suppresses)
# its notification synchronously during the del, so a short window proves
# non-arrival without burning the full positive EVT_DURATION.
EVT_NEG_DURATION = float(os.environ.get("EVT_NEG_DURATION", "0.5"))
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
    """Provider grows and shrinks the topology within its fabric: a
    read-only ABI view of the xGMI-shaped lifecycle where the provider owns
    membership/adjacency and userspace only observes. Asserts (a) initial
    adjacency visible, (b) hotplug CREATE/DELETE events each advance
    topology-generation, (c) a late arrival carries no peer (provider
    links explicitly, doesn't auto-wire), (d) pure reads never advance the
    generation. Non-destructive: only the two added endpoints are removed.
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
    """Hotplug one endpoint and unplug it, asserting the CREATE/DELETE
    events and membership. Self-contained: adds and deletes the same
    endpoint.
    """
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


def test_peer_unplug(ksft, cfg):
    """Link two members, delete one, assert the survivor's peer is intact."""
    fab, NlError = cfg.fab, cfg.NlError
    ep_a = add_ep(fab, "add_endpoint", nports=1)
    ep_b = add_ep(fab, "add_endpoint", nports=1)
    if not (ep_a and ep_b):
        ksft.not_ok("peer-unplug-link-established", "could not add two endpoints")
        ksft.not_ok("peer-unplug-survivor-peer-retained", "setup failed")
        ksft.not_ok("peer-unplug-no-port-change-ntf", "setup failed")
        # Tear down the half-built setup: an endpoint left behind here joins
        # the fabric every later case enumerates, turning one failed setup
        # into unrelated failures further down the suite.
        for ep in (ep_a, ep_b):
            if ep:
                del_ep(fab, slot_of(ep["name"]), ep["name"])
        return

    a_id, b_id = ep_a["endpoint-id"], ep_b["endpoint-id"]
    a_fepid, b_fepid = ep_a["fabric-ep-id"], ep_b["fabric-ep-id"]
    linked = True
    try:
        fab.do("port-peer-new", {"endpoint-id": a_id, "port-index": 0,
                                 "peer": {"peer-id": b_fepid,
                                          "type": "accel",
                                          "port-index": 0}})
        fab.do("port-peer-new", {"endpoint-id": b_id, "port-index": 0,
                                 "peer": {"peer-id": a_fepid,
                                          "type": "accel",
                                          "port-index": 0}})
    except NlError as exc:
        linked = False
        ksft.not_ok("peer-unplug-link-setup", "errno=%d" % exc.error)

    if linked:
        pa = fab.do("port-get", {"endpoint-id": a_id, "port-index": 0})["port"]
        ksft.check("peer" in pa, "peer-unplug-link-established")

        ev = L.DrmFabric()
        ev.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        del_ep(fab, slot_of(ep_b["name"]), ep_b["name"])
        pc = L.wait_ntf(
            ev, "port-change-ntf", timeout=EVT_NEG_DURATION,
            match=lambda n: n["msg"]["port"].get("endpoint-id") == a_id)
        pa2 = fab.do("port-get",
                     {"endpoint-id": a_id, "port-index": 0})["port"]
        ksft.check("peer" in pa2, "peer-unplug-survivor-peer-retained",
                   "peer=%s" % pa2.get("peer"))
        ksft.check(pc is None, "peer-unplug-no-port-change-ntf",
                   "unexpected port-change for a=%s" % (pc,))
    else:
        del_ep(fab, slot_of(ep_b["name"]), ep_b["name"])
    del_ep(fab, slot_of(ep_a["name"]), ep_a["name"])


def test_orphan_lifecycle(ksft, cfg):
    """Orphan attach -> admin up/down -> detach, plus a PORT_SET round-trip."""
    fab, NlError = cfg.fab, cfg.NlError
    orphan = add_ep(fab, "add_orphan", nports=1)
    if not orphan:
        ksft.not_ok("endpoint-set-orphan-created", "add_orphan failed")
        return

    o_id = orphan["endpoint-id"]
    ksft.check(orphan.get("fabric-id", 0) == 0, "endpoint-set-orphan-created",
               "fabric-id=%s" % orphan.get("fabric-id"))

    def ep_now():
        return fab.do("endpoint-get", {"endpoint-id": o_id})["endpoint"]

    try:
        fab.do("endpoint-set", {"endpoint-id": o_id, "fabric-id": cfg.fid})
        e = ep_now()
        ksft.check(e.get("fabric-id") == cfg.fid and
                   e.get("admin-state") == "down",
                   "endpoint-set-attach-keeps-admin-down",
                   "fabric=%s admin=%s" % (e.get("fabric-id"),
                                           e.get("admin-state")))

        fab.do("endpoint-set", {"endpoint-id": o_id, "admin-state": "up"})
        ksft.check(ep_now().get("admin-state") == "up",
                   "endpoint-set-admin-up")

        fab.do("endpoint-set", {"endpoint-id": o_id, "admin-state": "down"})
        fab.do("endpoint-set", {"endpoint-id": o_id, "fabric-id": 0})
        ksft.check(ep_now().get("fabric-id", 0) == 0,
                   "endpoint-set-detach-to-orphan")
    except NlError as exc:
        ksft.not_ok("endpoint-set-attach-keeps-admin-down",
                    "errno=%d" % exc.error)
        ksft.not_ok("endpoint-set-admin-up", "setup failed")
        ksft.not_ok("endpoint-set-detach-to-orphan", "setup failed")

    try:
        fab.do("port-set", {"endpoint-id": o_id, "port-index": 0,
                            "admin-state": "down"})
        d = fab.do("port-get",
                   {"endpoint-id": o_id, "port-index": 0})["port"]
        fab.do("port-set", {"endpoint-id": o_id, "port-index": 0,
                            "admin-state": "up"})
        u = fab.do("port-get",
                   {"endpoint-id": o_id, "port-index": 0})["port"]
        ksft.check(d.get("admin-state") == "down" and
                   u.get("admin-state") == "up",
                   "port-set-admin-round-trip",
                   "down=%s up=%s" % (d.get("admin-state"),
                                      u.get("admin-state")))
    except NlError as exc:
        ksft.not_ok("port-set-admin-round-trip", "errno=%d" % exc.error)

    del_ep(fab, slot_of(orphan["name"]), orphan["name"])


CASES = (
    test_provider_topology_lifecycle,
    test_hotplug_lifecycle,
    test_peer_unplug,
    test_orphan_lifecycle,
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
