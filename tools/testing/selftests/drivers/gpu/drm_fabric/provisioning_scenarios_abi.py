#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
End-to-end provisioning lifecycles, tying the implementation to the
intended flows rather than the isolated mechanics covered elsewhere
(cap_netadmin/fault/fabric_abi): orchestrated startup and link
failure/recovery, each detailed on its own test.

Mutation via the real ABI; operational/telemetry state via fabricsim
debugfs. Needs drm_fabric + drm_fabric_sim (default mesh, 4 ports); root.

Usage: provisioning_scenarios_abi.py [--no-load]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

EVT_DURATION = float(os.environ.get("EVT_DURATION", "3"))
EVT_SETTLE = float(os.environ.get("EVT_SETTLE", "0.2"))

USER_PORT = 3


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


def add_orphan(fab, nports):
    """Register a provider orphan via debugfs; return the new endpoint dict."""
    before = set(eps_by_name(fab))
    L.dbg_write("add_orphan", nports)
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


def _gen(fab, ep_id, port_index):
    return fab.do("port-get", {"endpoint-id": ep_id,
                               "port-index": port_index}).get(
                                   "topology-generation")


def _port(fab, ep_id, port_index):
    return fab.do("port-get", {"endpoint-id": ep_id,
                               "port-index": port_index})["port"]


def _ep(fab, ep_id):
    return fab.do("endpoint-get", {"endpoint-id": ep_id})["endpoint"]


def test_orchestrated_startup(ksft, cfg):
    """Full orchestrated bring-up of a provider-supplied orphan: orphan ->
    create fabric -> attach -> endpoint admin up -> port admin up ->
    provider oper ACTIVE -> userspace installs a peer. Administrative
    intent (userspace) and operational state (provider) move
    independently.
    """
    fab, NlError = cfg.fab, cfg.NlError

    orphan = add_orphan(fab, nports=4)
    if not orphan:
        for name in ("startup-orphan-visible", "startup-fabric-created",
                     "startup-attach-membership",
                     "startup-attach-endpoint-change-ntf",
                     "startup-endpoint-admin-up",
                     "startup-oper-independent-of-admin",
                     "startup-port-admin-up",
                     "startup-provider-reports-oper-active",
                     "startup-oper-active-port-change-ntf",
                     "startup-userspace-peer-installed"):
            ksft.not_ok(name, "add_orphan failed")
        return

    o_id = orphan["endpoint-id"]
    slot = slot_of(orphan["name"])
    made_fabric = None

    ksft.check(orphan.get("fabric-id", 0) == 0 and
               orphan.get("admin-state") == "down", "startup-orphan-visible",
               "fabric-id=%s admin=%s" % (orphan.get("fabric-id"),
                                          orphan.get("admin-state")))
    try:
        rep = fab.do("fabric-new", {
            "type": "synthetic", "name": "startup", "instance-id": 0x57A})
        made_fabric = rep.get("fabric-id")
        ksft.check(made_fabric is not None, "startup-fabric-created",
                   "reply=%s" % rep)

        ev = L.DrmFabric()
        ev.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        fab.do("endpoint-set", {"endpoint-id": o_id, "fabric-id": made_fabric})
        attach_ntf = L.wait_ntf(
            ev, "endpoint-change-ntf", timeout=EVT_DURATION,
            match=lambda n: n["msg"]["endpoint"].get("endpoint-id") == o_id)
        e = _ep(fab, o_id)
        ksft.check(e.get("fabric-id") == made_fabric and
                   e.get("admin-state") == "down", "startup-attach-membership",
                   "fabric=%s admin=%s" % (e.get("fabric-id"),
                                           e.get("admin-state")))
        ksft.check(attach_ntf is not None,
                   "startup-attach-endpoint-change-ntf")

        fab.do("endpoint-set", {"endpoint-id": o_id, "admin-state": "up"})
        ksft.check(_ep(fab, o_id).get("admin-state") == "up",
                   "startup-endpoint-admin-up")

        # Bring a provider-managed port admin-up; operational state must not
        # follow automatically -- the provider owns it.
        pre = _port(fab, o_id, 0)
        fab.do("port-set", {"endpoint-id": o_id, "port-index": 0,
                            "admin-state": "up"})
        p = _port(fab, o_id, 0)
        ksft.check(p.get("admin-state") == "up", "startup-port-admin-up")
        ksft.check(pre.get("oper-state") != "active" and
                   p.get("oper-state") != "active",
                   "startup-oper-independent-of-admin",
                   "oper=%s" % p.get("oper-state"))

        evp = L.DrmFabric()
        evp.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        L.dbg_write("ep%d/port0/oper_state" % slot, "active")
        oper_ntf = L.wait_ntf(
            evp, "port-change-ntf", timeout=EVT_DURATION,
            match=lambda n: n["msg"]["port"].get("endpoint-id") == o_id)
        L.wait_until(lambda: _port(fab, o_id, 0).get("oper-state") == "active")
        ksft.check(_port(fab, o_id, 0).get("oper-state") == "active",
                   "startup-provider-reports-oper-active")
        ksft.check(oper_ntf is not None, "startup-oper-active-port-change-ntf")

        peer = {"peer-id": 0x2A, "type": "accel", "port-index": 0}
        fab.do("port-peer-new", {"endpoint-id": o_id, "port-index": USER_PORT,
                                 "peer": peer})
        pu = _port(fab, o_id, USER_PORT)
        ksft.check(pu.get("peer") is not None and
                   pu["peer"].get("peer-id") == 0x2A,
                   "startup-userspace-peer-installed",
                   "peer=%s" % pu.get("peer"))
    finally:
        for method, vals in (
                ("port-peer-del", {"endpoint-id": o_id,
                                   "port-index": USER_PORT}),
                ("port-set", {"endpoint-id": o_id, "port-index": 0,
                              "admin-state": "down"}),
                ("endpoint-set", {"endpoint-id": o_id, "admin-state": "down"}),
                ("endpoint-set", {"endpoint-id": o_id, "fabric-id": 0})):
            try:
                fab.do(method, vals)
            except NlError:
                pass
        if made_fabric is not None:
            try:
                fab.do("fabric-del", {"fabric-id": made_fabric})
            except NlError:
                pass
        del_ep(fab, slot, orphan["name"])


def test_link_failure_and_recovery(ksft, cfg):
    """A live link fails and recovers under provider control.

    Uses an initial mesh member (provider-managed port 0 with an established
    peer, userspace-managed port 3). Asserts administrative intent survives an
    operational failure, telemetry advances without touching topology-
    generation, operational transitions do advance it and emit port-change,
    an identical admin request is a no-op, and a userspace peer is replaced
    with strict delete-before-new ordering leaving no stale descriptor.
    """
    fab, NlError = cfg.fab, cfg.NlError
    EP, PP, UP = 0, 0, USER_PORT

    fab.do("port-set", {"endpoint-id": EP, "port-index": PP,
                        "admin-state": "up"})
    L.dbg_write("ep%d/port%d/oper_state" % (EP, PP), "active")
    L.wait_until(lambda: _port(fab, EP, PP).get("oper-state") == "active")
    try:
        base = fab.do("port-stats-get", {"endpoint-id": EP,
                                         "port-index": PP})["port-stats"]
        c0 = base.get("link-down-count", 0)
        g_active = _gen(fab, EP, PP)

        # Failure: the provider reports the link down.
        ev = L.DrmFabric()
        ev.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        L.dbg_write("ep%d/port%d/inject" % (EP, PP), "link_down")
        down_ntf = L.wait_ntf(
            ev, "port-change-ntf", timeout=EVT_DURATION,
            match=lambda n: n["msg"]["port"].get("endpoint-id") == EP)
        L.wait_until(lambda: _port(fab, EP, PP).get("oper-state") == "inactive")
        p_down = _port(fab, EP, PP)
        ksft.check(p_down.get("oper-state") == "inactive",
                   "linkfail-oper-inactive", "oper=%s" % p_down.get(
                       "oper-state"))
        ksft.check(p_down.get("admin-state") == "up",
                   "linkfail-admin-stays-up", "admin=%s" % p_down.get(
                       "admin-state"))
        ksft.check(down_ntf is not None, "linkfail-oper-change-port-change-ntf")
        g_down = _gen(fab, EP, PP)
        ksft.check(g_active is not None and g_down is not None and
                   g_down > g_active, "linkfail-oper-change-advances-generation",
                   "active=%s down=%s" % (g_active, g_down))

        # Telemetry advances; a stats read must not advance topology-generation.
        s = fab.do("port-stats-get", {"endpoint-id": EP,
                                      "port-index": PP})["port-stats"]
        ksft.check(s.get("link-down-count", 0) >= c0 + 1,
                   "linkfail-link-down-count-increases",
                   "c0=%d now=%s" % (c0, s.get("link-down-count")))
        ksft.check(_gen(fab, EP, PP) == g_down,
                   "linkfail-stats-read-no-generation-bump")

        # Recovery.
        ev2 = L.DrmFabric()
        ev2.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        L.dbg_write("ep%d/port%d/inject" % (EP, PP), "recover_to_active")
        up_ntf = L.wait_ntf(
            ev2, "port-change-ntf", timeout=EVT_DURATION,
            match=lambda n: n["msg"]["port"].get("endpoint-id") == EP)
        L.wait_until(lambda: _port(fab, EP, PP).get("oper-state") == "active")
        ksft.check(_port(fab, EP, PP).get("oper-state") == "active",
                   "linkfail-recovery-oper-active")
        g_recovered = _gen(fab, EP, PP)
        ksft.check(g_recovered > g_down,
                   "linkfail-recovery-advances-generation",
                   "down=%s recovered=%s" % (g_down, g_recovered))
        ksft.check(up_ntf is not None, "linkfail-recovery-port-change-ntf")

        # An identical admin request is a no-op: no generation change.
        g_pre_noop = _gen(fab, EP, PP)
        fab.do("port-set", {"endpoint-id": EP, "port-index": PP,
                            "admin-state": "up"})
        ksft.check(_gen(fab, EP, PP) == g_pre_noop,
                   "linkfail-idempotent-admin-noop")

        # Peer replacement on the userspace-managed port: X, then delete, then
        # Y -- strict delete-before-new ordering, no stale descriptor.
        peer_x = {"peer-id": 0x101, "type": "accel", "port-index": 0}
        peer_y = {"peer-id": 0x202, "type": "accel", "port-index": 0}
        fab.do("port-peer-new", {"endpoint-id": EP, "port-index": UP,
                                 "peer": peer_x})
        px = _port(fab, EP, UP).get("peer")
        fab.do("port-peer-del", {"endpoint-id": EP, "port-index": UP})
        pmid = _port(fab, EP, UP).get("peer")
        fab.do("port-peer-new", {"endpoint-id": EP, "port-index": UP,
                                 "peer": peer_y})
        py = _port(fab, EP, UP).get("peer")
        ksft.check(px is not None and px.get("peer-id") == 0x101,
                   "linkfail-peer-install-x", "peer=%s" % px)
        ksft.check(pmid is None, "linkfail-peer-del-clears", "peer=%s" % pmid)
        ksft.check(py is not None and py.get("peer-id") == 0x202,
                   "linkfail-peer-replace-y-no-stale", "peer=%s" % py)

        # Final query matches the reported stream: oper active + peer Y.
        pf0 = _port(fab, EP, PP)
        pfu = _port(fab, EP, UP)
        ksft.check(pf0.get("oper-state") == "active" and
                   (pfu.get("peer") or {}).get("peer-id") == 0x202,
                   "linkfail-final-query-matches",
                   "oper=%s peer=%s" % (pf0.get("oper-state"),
                                        pfu.get("peer")))
    finally:
        try:
            fab.do("port-peer-del", {"endpoint-id": EP, "port-index": UP})
        except NlError:
            pass
        L.dbg_write("ep%d/port%d/inject" % (EP, PP), "recover_to_active")


CASES = (
    test_orchestrated_startup,
    test_link_failure_and_recovery,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, need_debugfs=True, need_control="add_orphan") as fab:
        fid = fabricsim_fid(fab)
        if fid is None:
            ksft.skip_all("fabricsim fabric not present")
        if not L.family_has_op(fab, "fabric-new"):
            ksft.skip_all("mutation ABI absent (query-only build)")

        L.run_cases(ksft, Cfg(fab, fid, NlError), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
