#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Full drm-fabric genetlink ABI coverage via YNL against fabricsim: every
command (do + dump) and event, asserted on decoded reply dicts so checks
are immune to CLI text changes.

Usage: fabric_abi.py [--no-load]   (--no-load: modules already loaded)
"""

import errno
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

EVT_DURATION = float(os.environ.get("EVT_DURATION", "3"))
# Multicast subscription is synchronous (setsockopt), so a brief settle before
# triggering is enough; wait_ntf() then polls with a deadline for the arrival.
EVT_SETTLE = float(os.environ.get("EVT_SETTLE", "0.2"))


def _unload_providers():
    L.rmmod("drm_fabric_sim")
    L.rmmod("drm_fabric")


def fabric_names(fab):
    return [e["fabric"]["name"] for e in fab.dump("fabric-get", {})]


def ep_list(fab, filt=None):
    return [e["endpoint"] for e in fab.dump("endpoint-get", filt or {})]


def port_list(fab, ep_id):
    return [p["port"] for p in fab.dump("port-get", {"endpoint-id": ep_id})]


def peers_in(ports):
    return [p["peer"]["peer-id"] for p in ports if "peer" in p]


def _reload_sim(shape):
    L.rmmod("drm_fabric_sim")
    L.insmod("drm-fabric-sim.ko", "topology=%s" % shape)
    L.wait_until(lambda: L.module_loaded("drm_fabric_sim"))


class Cfg:
    def __init__(self, fab, fid, dfs, no_load, nl_error):
        self.fab = fab
        self.fid = fid
        self.dfs = dfs
        self.no_load = no_load
        self.NlError = nl_error


def test_fabric_get(ksft, cfg):
    fab = cfg.fab
    ksft.check("fabricsim" in fabric_names(fab), "fabric-get-dump")
    f = fab.do("fabric-get", {"fabric-id": cfg.fid})
    ksft.check(f["fabric"]["name"] == "fabricsim", "fabric-get-do")


def test_endpoint_get_dump(ksft, cfg):
    fab = cfg.fab
    eps = ep_list(fab)
    ksft.check(len(eps) == 4, "endpoint-get-dump", "got %d" % len(eps))
    epf = ep_list(fab, {"fabric-id": cfg.fid})
    ksft.check(len(epf) == 4, "endpoint-get-dump-filtered", "got %d" % len(epf))


def test_endpoint_get_do(ksft, cfg):
    fab = cfg.fab
    e0 = fab.do("endpoint-get", {"endpoint-id": 0})
    ksft.check(e0["endpoint"]["name"] == "sim-ep0", "endpoint-get-do")
    e = fab.do("endpoint-get", {"dev-name": "fabricsim.0"})
    ksft.check(e["endpoint"]["name"] == "sim-ep0", "endpoint-get-do-by-dev-name")
    e = fab.do("endpoint-get", {"dev-name": "fabricsim.1", "bus-name": "platform"})
    ksft.check(e["endpoint"]["name"] == "sim-ep1",
               "endpoint-get-do-by-dev-name-bus")
    e = fab.do("endpoint-get", {"endpoint-id": 0, "dev-name": "fabricsim.0"})
    ksft.check(e["endpoint"]["name"] == "sim-ep0",
               "endpoint-get-do-id-and-dev-name-agree")


def test_endpoint_get_do_errors(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("endpoint-get", {"endpoint-id": 0, "dev-name": "fabricsim.1"})
        ksft.not_ok("endpoint-get-do-id-dev-name-conflict-einval", "accepted")
    except NlError as exc:
        ksft.check(L.nl_errno(exc) == 22,  # EINVAL
                   "endpoint-get-do-id-dev-name-conflict-einval",
                   "errno=%d" % L.nl_errno(exc))
    try:
        fab.do("endpoint-get", {"bus-name": "platform"})
        ksft.not_ok("endpoint-get-do-bus-name-only-einval", "accepted")
    except NlError as exc:
        ksft.check(L.nl_errno(exc) == 22, "endpoint-get-do-bus-name-only-einval",
                   "errno=%d" % L.nl_errno(exc))
    try:
        fab.do("endpoint-get", {"dev-name": "nope.99"})
        ksft.not_ok("endpoint-get-do-dev-name-enoent", "accepted")
    except NlError as exc:
        ksft.check(L.nl_errno(exc) == 2, "endpoint-get-do-dev-name-enoent",
                   "errno=%d" % L.nl_errno(exc))


def test_port_get(ksft, cfg):
    fab = cfg.fab
    p0 = port_list(fab, 0)
    ksft.check(len(p0) == 4, "port-get-dump", "got %d" % len(p0))
    port = fab.do("port-get", {"endpoint-id": 0, "port-index": 0})["port"]
    ksft.check(port["oper-state"] == "active" and "peer" in port, "port-get-do")
    ksft.check(port.get("peer", {}).get("peer-id") == 257,
               "port-get-peer-data")


def test_port_stats(ksft, cfg):
    fab = cfg.fab
    st = fab.do("port-stats-get", {"endpoint-id": 0, "port-index": 0})["port-stats"]
    ksft.check(st.get("read-bytes", -1) == 0 and st.get("write-bytes", -1) == 0,
               "port-stats-get-do")
    stats = fab.dump("port-stats-get", {"endpoint-id": 0})
    ksft.check(len(stats) == 4, "port-stats-get-dump", "got %d" % len(stats))


def test_activity_stats(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("activity-stats-increment", "debugfs not available")
        return
    fab = cfg.fab
    L.dbg_write("ep0/port0/read_rate", 1024)
    L.dbg_write("ep0/port0/write_rate", 512)
    L.dbg_write("ep0/port0/activity_enable", 1)

    def _stats():
        return fab.do("port-stats-get",
                      {"endpoint-id": 0, "port-index": 0})["port-stats"]

    L.wait_until(lambda: _stats().get("read-bytes", 0) > 0 and
                 _stats().get("write-bytes", 0) > 0)
    L.dbg_write("ep0/port0/activity_enable", 0)
    s = _stats()
    ksft.check(s.get("read-bytes", 0) > 0 and s.get("write-bytes", 0) > 0,
               "activity-stats-increment",
               "read=%s write=%s" % (s.get("read-bytes"), s.get("write-bytes")))


def test_inject_link_down(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("inject-link-down-oper-inactive", "debugfs not available")
        return
    fab = cfg.fab
    L.dbg_write("ep1/port0/inject", "link_down")
    p = fab.do("port-get", {"endpoint-id": 1, "port-index": 0})["port"]
    ksft.check(p["oper-state"] == "inactive", "inject-link-down-oper-inactive",
               "oper=%s" % p["oper-state"])
    L.dbg_write("ep1/port0/inject", "recover_to_active")


def test_oper_state_degraded(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("debugfs-oper-state-degraded", "debugfs not available")
        return
    fab = cfg.fab
    L.dbg_write("ep2/port0/oper_state", "degraded")
    p = fab.do("port-get", {"endpoint-id": 2, "port-index": 0})["port"]
    ksft.check(p["oper-state"] == "degraded", "debugfs-oper-state-degraded",
               "oper=%s" % p["oper-state"])
    L.dbg_write("ep2/port0/oper_state", "active")


def test_topology_kn(ksft, cfg):
    fab = cfg.fab
    peers = set(peers_in(port_list(fab, 0)))
    ksft.check(len(peers) == 3, "topology-kn-distinct-peers",
               "distinct peers=%d" % len(peers))
    p = fab.do("port-get", {"endpoint-id": 1, "port-index": 0})["port"]
    ksft.check(p.get("peer", {}).get("peer-id") == 256,
               "topology-kn-bidirectional")
    p = fab.do("port-get", {"endpoint-id": 0, "port-index": 3})["port"]
    ksft.check("peer" not in p, "port-no-peer")


def test_counters_stop(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("counters-stop-after-disable", "debugfs not available")
        return
    fab = cfg.fab

    def _rb():
        return fab.do("port-stats-get",
                      {"endpoint-id": 0, "port-index": 0})["port-stats"]["read-bytes"]

    r1 = _rb()
    # Proving a *non-event* (counters must NOT advance after disable) needs a
    # real wait; poll a bounded window and assert the value never moved.
    moved = L.wait_until(lambda: _rb() != r1, timeout=0.5)
    ksft.check(not moved, "counters-stop-after-disable",
               "read-bytes moved from %s to %s" % (r1, _rb()))


def test_port_state_cycle(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("port-state-inject-cycle", "debugfs not available")
        return
    fab = cfg.fab
    L.dbg_write("ep2/port1/oper_state", "active")
    L.dbg_write("ep2/port1/inject", "degrade")
    s1 = fab.do("port-get", {"endpoint-id": 2, "port-index": 1})["port"]["oper-state"]
    L.dbg_write("ep2/port1/inject", "link_down")
    s2 = fab.do("port-get", {"endpoint-id": 2, "port-index": 1})["port"]["oper-state"]
    L.dbg_write("ep2/port1/inject", "recover_to_active")
    s3 = fab.do("port-get", {"endpoint-id": 2, "port-index": 1})["port"]["oper-state"]
    ksft.check(s1 == "degraded" and s2 == "inactive" and s3 == "active",
               "port-state-inject-cycle", "%s,%s,%s" % (s1, s2, s3))


def test_port_change_ntf(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("port-change-ntf-notification", "debugfs not available")
        return
    fab = cfg.fab
    L.dbg_write("ep1/port1/oper_state", "active")
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    L.dbg_write("ep1/port1/oper_state", "degraded")
    got = L.wait_ntf(ev, "port-change-ntf", timeout=EVT_DURATION,
                     match=lambda n: n["msg"]["port"].get("oper-state") == "degraded")
    ksft.check(got is not None, "port-change-ntf-notification")
    # The event carries the post-change topology-generation (nonzero); a
    # subsequent GET reports a generation that is >= the event's.
    if got is not None:
        egen = got["msg"].get("topology-generation")
        ggen = fab.do("port-get",
                      {"endpoint-id": 1, "port-index": 1}).get("topology-generation")
        ksft.check(egen is not None and egen != 0 and
                   ggen is not None and ggen >= egen,
                   "port-change-ntf-topology-generation",
                   "event=%s get=%s" % (egen, ggen))
    L.dbg_write("ep1/port1/oper_state", "active")


def test_endpoint_change_ntf(ksft, cfg):
    # ENDPOINT_CHANGE_NTF is emitted by an attribute change (endpoint-set), not
    # by unregister -- removing a provider emits ENDPOINT_DELETE_NTF instead.
    # Toggle a live endpoint's admin state to provoke the change event, then
    # restore the original state so later cases are unaffected.
    fab, NlError = cfg.fab, cfg.NlError
    ep = fab.do("endpoint-get", {"endpoint-id": 0})["endpoint"]
    cur = ep.get("admin-state")
    target = "down" if cur == "up" else "up"
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    try:
        fab.do("endpoint-set", {"endpoint-id": 0, "admin-state": target})
    except NlError as exc:
        ksft.not_ok("endpoint-change-ntf-notification",
                    "endpoint-set errno=%d" % exc.error)
        return
    got = L.wait_ntf(ev, "endpoint-change-ntf", timeout=EVT_DURATION,
                     match=lambda n: n["msg"]["endpoint"].get("endpoint-id") == 0)
    try:
        fab.do("endpoint-set", {"endpoint-id": 0, "admin-state": cur})
    except NlError:
        pass
    ksft.check(got is not None, "endpoint-change-ntf-notification")


def test_linear_topology(ksft, cfg):
    """Reload the sim into the linear topology and assert the chain shape.
    Restores the default mesh K_4 on the way out (even on failure), so
    this cannot cascade into later cases that assume the default topology.
    """
    if cfg.no_load:
        ksft.skip("linear-topology-chain", "skipped with --no-load")
        ksft.skip("linear-topology-adjacent-peers", "skipped with --no-load")
        ksft.skip("linear-topology-end-no-extra-peer", "skipped with --no-load")
        ksft.skip("linear-topology-debugfs-works",
                  "skipped with --no-load or no debugfs")
        return
    fab = cfg.fab
    _reload_sim("linear")
    try:
        n0 = len(peers_in(port_list(fab, 0)))
        n1 = len(peers_in(port_list(fab, 1)))
        n3 = len(peers_in(port_list(fab, 3)))
        ksft.check(n0 == 1 and n1 == 2 and n3 == 1, "linear-topology-chain",
                   "ep0=%d ep1=%d ep3=%d" % (n0, n1, n3))
        ps = peers_in(port_list(fab, 0))
        ksft.check(ps and ps[0] == 257, "linear-topology-adjacent-peers",
                   "ep0 peers=%s" % ps)
        p = fab.do("port-get", {"endpoint-id": 0, "port-index": 1})["port"]
        ksft.check("peer" not in p, "linear-topology-end-no-extra-peer")
        if cfg.dfs:
            L.dbg_write("ep0/port0/oper_state", "degraded")
            p = fab.do("port-get", {"endpoint-id": 0, "port-index": 0})["port"]
            ksft.check(p["oper-state"] == "degraded",
                       "linear-topology-debugfs-works")
            L.dbg_write("ep0/port0/oper_state", "active")
        else:
            ksft.skip("linear-topology-debugfs-works",
                      "skipped with --no-load or no debugfs")
    finally:
        # Always return to the default mesh K_4 shape for the cases that follow.
        _reload_sim("mesh")


def test_reload_mesh(ksft, cfg):
    """Defensively re-establish the default mesh K_N topology (idempotent)
    and assert it, guaranteeing the precondition for the RAS/NTF cases
    that follow even if an earlier reload failed.
    """
    if cfg.no_load:
        ksft.skip("reload-mesh-topology-restored", "skipped with --no-load")
        return
    fab = cfg.fab
    _reload_sim("mesh")
    n0 = len(peers_in(port_list(fab, 0)))
    ksft.check(n0 == 3, "reload-mesh-topology-restored", "ep0 peers=%d" % n0)


def test_port_change_ntf_full(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("port-change-ntf-full-port-nest", "debugfs not available")
        return
    L.dbg_write("ep0/port0/oper_state", "active")
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    L.dbg_write("ep0/port0/oper_state", "degraded")
    got = L.wait_ntf(
        ev, "port-change-ntf", timeout=EVT_DURATION,
        match=lambda n: "endpoint-id" in n["msg"]["port"] and
        "port-index" in n["msg"]["port"])
    ksft.check(got is not None, "port-change-ntf-full-port-nest")
    L.dbg_write("ep0/port0/oper_state", "active")


def test_link_down_exact_count(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("inject-link-down-exact-count", "debugfs not available")
        return
    fab = cfg.fab
    base = fab.do("port-stats-get",
                  {"endpoint-id": 3, "port-index": 1})["port-stats"]
    c0 = base.get("link-down-count", 0)
    for _ in range(3):
        L.dbg_write("ep3/port1/inject", "link_down")
    s = fab.do("port-stats-get",
               {"endpoint-id": 3, "port-index": 1})["port-stats"]
    ksft.check(s.get("link-down-count", 0) == c0 + 3,
               "inject-link-down-exact-count",
               "expected %d got %s" % (c0 + 3, s.get("link-down-count")))
    L.dbg_write("ep3/port1/inject", "recover_to_active")


def test_stats_survive_mutation(ksft, cfg):
    if not cfg.dfs:
        ksft.skip("stats-counters-survive-mutation", "debugfs not available")
        return
    fab, NlError = cfg.fab, cfg.NlError
    L.dbg_write("ep2/port0/inject", "link_down")
    L.dbg_write("ep2/port0/inject", "link_down")
    pre = fab.do("port-stats-get",
                 {"endpoint-id": 2, "port-index": 0})["port-stats"]
    cpre = pre.get("link-down-count", 0)
    survived = True
    try:
        fab.do("port-set", {"endpoint-id": 2, "port-index": 0,
                            "admin-state": "down"})
        fab.do("port-set", {"endpoint-id": 2, "port-index": 0,
                            "admin-state": "up"})
        # Drive a down/detach/attach/up sequence so the port stats assertion
        # below runs against the same endpoint membership and admin state it
        # started with.
        fab.do("endpoint-set", {"endpoint-id": 2, "admin-state": "down"})
        fab.do("endpoint-set", {"endpoint-id": 2, "fabric-id": 0})
        fab.do("endpoint-set", {"endpoint-id": 2, "fabric-id": cfg.fid})
        fab.do("endpoint-set", {"endpoint-id": 2, "admin-state": "up"})
    except NlError as exc:
        # The sim does not ordinarily reject this mutation-only sequence (no
        # fault injection is armed here), so an unexpected failure here is a
        # real ABI regression, not an environmental limitation.
        survived = None
        ksft.not_ok("stats-counters-survive-mutation",
                    "mutation errno=%d" % L.nl_errno(exc))
    if survived is not None:
        post = fab.do("port-stats-get",
                      {"endpoint-id": 2, "port-index": 0})["port-stats"]
        ksft.check(post.get("link-down-count", 0) == cpre,
                   "stats-counters-survive-mutation",
                   "pre=%d post=%s" % (cpre, post.get("link-down-count")))
    # Restore everything the sequence above can have changed, not just the
    # port: a failure part-way through leaves the endpoint detached or
    # admin-down, and skipping with that state still in place would silently
    # change the topology every later case enumerates. Restore the same
    # sequence explicitly so later cases see the baseline state again.
    for cmd, req in (("endpoint-set", {"endpoint-id": 2, "admin-state": "down"}),
                     ("endpoint-set", {"endpoint-id": 2, "fabric-id": cfg.fid}),
                     ("endpoint-set", {"endpoint-id": 2, "admin-state": "up"}),
                     ("port-set", {"endpoint-id": 2, "port-index": 0,
                                   "admin-state": "up"})):
        try:
            fab.do(cmd, req)
        except NlError:
            pass
    try:
        L.dbg_write("ep2/port0/inject", "recover_to_active")
    except OSError:
        pass
    # Assert the restore actually took: a silent failure here is exactly what
    # would make a later, unrelated case fail instead of this one.
    back = fab.do("endpoint-get", {"endpoint-id": 2})["endpoint"]
    ksft.check(back.get("fabric-id") == cfg.fid and
               back.get("admin-state") == "up",
               "stats-mutation-endpoint-restored",
               "fabric-id=%s admin-state=%s"
               % (back.get("fabric-id"), back.get("admin-state")))


def test_fabric_new_duplicate(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    params = {"type": "synthetic", "name": "iid-uniq", "instance-id": 0x9999}

    def fabric_cleanup(fabric_id):
        def drop():
            """Delete the fabric unless explicit cleanup already did."""
            try:
                fab.do("fabric-del", {"fabric-id": fabric_id})
            except NlError as exc:
                if L.nl_errno(exc) != errno.ENOENT:
                    raise

        return drop

    try:
        fabric_id = fab.do("fabric-new",
                           params)["fabric-id"]
    except NlError as exc:
        ksft.not_ok("fabric-new-duplicate-instance-id-eexist",
                    "setup fabric-new errno=%d" % L.nl_errno(exc))
        return

    L.on_teardown(fabric_cleanup(fabric_id))

    dup = dict(params, name="iid-dup")
    try:
        duplicate = fab.do("fabric-new", dup)
    except NlError as exc:
        ksft.check(L.nl_errno(exc) == errno.EEXIST,
                   "fabric-new-duplicate-instance-id-eexist",
                   "errno=%d" % L.nl_errno(exc))
    else:
        # Arm cleanup before reporting: an accepted duplicate is a second
        # live fabric that drop() above cannot reach.
        dup_id = duplicate.get("fabric-id")
        if dup_id is not None:
            L.on_teardown(fabric_cleanup(dup_id))
        ksft.not_ok("fabric-new-duplicate-instance-id-eexist",
                    "duplicate instance-id accepted")

    try:
        fab.do("fabric-del", {"fabric-id": fabric_id})
        ksft.ok("fabric-new-duplicate-cleanup-del")
    except NlError as exc:
        ksft.not_ok("fabric-new-duplicate-cleanup-del",
                    "errno=%d" % L.nl_errno(exc))


# Ordered scenario: each case builds on the topology/state left by the prior
# one (e.g. the linear reload precedes its assertions, and the mesh reload
# restores K_N for the stats cases). Keep this list in order.
CASES = (
    test_fabric_get,
    test_endpoint_get_dump,
    test_endpoint_get_do,
    test_endpoint_get_do_errors,
    test_port_get,
    test_port_stats,
    test_activity_stats,
    test_inject_link_down,
    test_oper_state_degraded,
    test_topology_kn,
    test_counters_stop,
    test_port_state_cycle,
    test_port_change_ntf,
    test_endpoint_change_ntf,
    test_linear_topology,
    test_reload_mesh,
    test_port_change_ntf_full,
    test_link_down_exact_count,
    test_stats_survive_mutation,
    test_fabric_new_duplicate,
)

# Cases that exercise the topology-mutation uAPI.  On a query-only build the
# family has no mutation ops, so these are filtered out.
MUTATION_CASES = (
    test_endpoint_change_ntf,
    test_stats_survive_mutation,
    test_fabric_new_duplicate,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    if not L.is_root():
        ksft.skip_all("must run as root (genetlink + debugfs + insmod)")

    no_load = "--no-load" in sys.argv[1:]

    if not no_load:
        L.rmmod("drm_fabric_sim")
        L.rmmod("drm_fabric")
        # Arm teardown before loading so a partial load is unwound, and so the
        # topology reshapes this suite performs are restored even if the timeout
        # killer sends SIGTERM (which a bare atexit would miss).
        L.on_teardown(_unload_providers)
        if not L.insmod("drm-fabric.ko") or not L.insmod("drm-fabric-sim.ko"):
            ksft.skip_all("could not load drm_fabric + drm_fabric_sim modules")
        L.wait_until(lambda: L.module_loaded("drm_fabric_sim"))

    if not L.module_loaded("drm_fabric"):
        ksft.skip_all("drm_fabric not loaded")
    if not L.module_loaded("drm_fabric_sim"):
        ksft.skip_all("drm_fabric_sim not loaded")

    try:
        fab = L.DrmFabric()
    except (OSError, NlError) as exc:
        ksft.skip_all("cannot open drm-fabric family: %s" % exc)

    # fabric-id 0 is the reserved orphan sentinel; discover the live id.
    fabrics = fab.dump("fabric-get", {})
    fid = fabrics[0]["fabric"]["fabric-id"] if fabrics else 1

    cfg = Cfg(fab, fid, L.debugfs_available(), no_load, NlError)
    L.run_cases(ksft, cfg, L.select_cases(fab, CASES, MUTATION_CASES))
    ksft.finish()


if __name__ == "__main__":
    main()
