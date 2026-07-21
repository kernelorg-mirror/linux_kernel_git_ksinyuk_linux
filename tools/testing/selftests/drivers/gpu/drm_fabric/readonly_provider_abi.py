#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Read-only provider-mode coverage for drm_fabric_sim.
"""

import errno
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


def slot_of(name):
    return int(name.rsplit("ep", 1)[1])


def add_ep(fab, nports=1):
    before = set(eps_by_name(fab))
    L.dbg_write("add_endpoint", nports)
    new = L.wait_until(lambda: set(eps_by_name(fab)) - before)
    if len(new) != 1:
        return None
    return eps_by_name(fab)[next(iter(new))]


def del_ep(fab, slot, name):
    L.dbg_write("del_endpoint", slot)
    return L.wait_until(lambda: name not in eps_by_name(fab))


def _gen(fab):
    return fab.do("port-get", {"endpoint-id": 0,
                               "port-index": 0}).get("topology-generation")


class Cfg:
    def __init__(self, fab, nl_error):
        self.fab = fab
        self.NlError = nl_error


def test_readonly_queries_and_mutations(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    made_fid = None

    ksft.check(len(fab.dump("fabric-get", {})) >= 1, "readonly-fabric-get")
    ksft.check(len(fab.dump("endpoint-get", {})) >= 1, "readonly-endpoint-get")
    ksft.check("port" in fab.do("port-get", {"endpoint-id": 0, "port-index": 0}),
               "readonly-port-get")
    ksft.check("port-stats" in fab.do("port-stats-get",
                                      {"endpoint-id": 0, "port-index": 0}),
               "readonly-port-stats-get")

    for name, op, req in (
            ("readonly-endpoint-set-membership-eopnotsupp",
             "endpoint-set", {"endpoint-id": 0, "fabric-id": 0}),
            ("readonly-endpoint-set-admin-eopnotsupp",
             "endpoint-set", {"endpoint-id": 0, "admin-state": "down"}),
            ("readonly-port-set-eopnotsupp",
             "port-set", {"endpoint-id": 0, "port-index": 0,
                          "admin-state": "down"}),
            ("readonly-port-peer-new-eopnotsupp",
             "port-peer-new", {"endpoint-id": 0, "port-index": USER_PORT,
                               "peer": {"peer-id": 0x22, "type": "accel",
                                        "port-index": 0}}),
            ("readonly-port-peer-del-eopnotsupp",
             "port-peer-del", {"endpoint-id": 0, "port-index": USER_PORT}),
    ):
        try:
            fab.do(op, req)
            ksft.not_ok(name, "accepted")
        except NlError as exc:
            ksft.check(L.nl_errno(exc) == errno.EOPNOTSUPP, name,
                       "errno=%d" % L.nl_errno(exc))

    try:
        rep = fab.do("fabric-new", {"type": "synthetic",
                                    "name": "readonly-empty",
                                    "instance-id": 0xE001})
        made_fid = rep.get("fabric-id")
        ksft.check(made_fid is not None, "readonly-fabric-new",
                   "reply=%s" % rep)
        fab.do("fabric-del", {"fabric-id": made_fid})
        ksft.ok("readonly-fabric-del")
        made_fid = None
    except NlError as exc:
        ksft.not_ok("readonly-fabric-new", "errno=%d" % L.nl_errno(exc))
        ksft.not_ok("readonly-fabric-del", "errno=%d" % L.nl_errno(exc))
    finally:
        if made_fid is not None:
            try:
                fab.do("fabric-del", {"fabric-id": made_fid})
            except NlError:
                pass


def test_readonly_provider_changes_visible(ksft, cfg):
    fab = cfg.fab
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    g0 = _gen(fab)
    ep = add_ep(fab, nports=1)

    if not ep:
        ksft.not_ok("readonly-provider-change-ntf", "add_endpoint failed")
        ksft.not_ok("readonly-provider-change-generation", "add_endpoint failed")
        return

    try:
        got = L.wait_ntf(
            ev, "endpoint-create-ntf", timeout=EVT_DURATION,
            match=lambda n: n["msg"]["endpoint"].get("endpoint-id") ==
            ep["endpoint-id"])
        g1 = _gen(fab)
        ksft.check(got is not None, "readonly-provider-change-ntf")
        ksft.check(g1 == g0 + 1, "readonly-provider-change-generation",
                   "g0=%s g1=%s" % (g0, g1))
    finally:
        del_ep(fab, slot_of(ep["name"]), ep["name"])


CASES = (
    test_readonly_queries_and_mutations,
    test_readonly_provider_changes_visible,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, topology="mesh", need_debugfs=True,
                     sim_args=["readonly=1"]) as fab:
        cfg = Cfg(fab, NlError)
        L.run_cases(ksft, cfg, CASES)

    ksft.finish()


if __name__ == "__main__":
    main()
