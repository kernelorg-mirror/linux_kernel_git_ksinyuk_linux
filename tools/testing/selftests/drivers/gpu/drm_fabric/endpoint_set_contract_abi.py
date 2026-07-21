#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Contract coverage for ENDPOINT_SET membership/admin-state behavior over the
real drm-fabric Generic Netlink ABI.
"""

import errno
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

EVT_DURATION = float(os.environ.get("EVT_DURATION", "3"))
EVT_NEG_DURATION = float(os.environ.get("EVT_NEG_DURATION", "0.5"))
EVT_SETTLE = float(os.environ.get("EVT_SETTLE", "0.2"))

STABLE_EP = 3


def eps_by_name(fab):
    return {e["endpoint"]["name"]: e["endpoint"]
            for e in fab.dump("endpoint-get", {})}


def slot_of(name):
    return int(name.rsplit("ep", 1)[1])


def add_orphan(fab, nports=1):
    before = set(eps_by_name(fab))
    L.dbg_write("add_orphan", nports)
    new = L.wait_until(lambda: set(eps_by_name(fab)) - before)
    if len(new) != 1:
        return None
    return eps_by_name(fab)[next(iter(new))]


def del_ep(fab, slot, name):
    L.dbg_write("del_endpoint", slot)
    return L.wait_until(lambda: name not in eps_by_name(fab))


def fabricsim_fid(fab):
    for item in fab.dump("fabric-get", {}):
        fabric = item["fabric"]
        if fabric["name"] == "fabricsim":
            return fabric["fabric-id"]
    return None


def _ep(fab, ep_id):
    return fab.do("endpoint-get", {"endpoint-id": ep_id})["endpoint"]


def _gen(fab):
    return fab.do("port-get", {"endpoint-id": STABLE_EP,
                               "port-index": 0}).get("topology-generation")


def _new_fabric(fab, name, instance_id):
    return fab.do("fabric-new", {"type": "synthetic",
                                 "name": name,
                                 "instance-id": instance_id})["fabric-id"]


class Cfg:
    def __init__(self, fab, fid, nl_error):
        self.fab = fab
        self.fid = fid
        self.NlError = nl_error


def test_direct_reassignment_refused(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    orig = _ep(fab, 0)
    made_fid = None

    try:
        made_fid = _new_fabric(fab, "reassign-target", 0xD001)
        try:
            fab.do("endpoint-set", {"endpoint-id": 0, "fabric-id": made_fid})
            ksft.not_ok("endpoint-set-direct-reassign-ebusy", "accepted")
        except NlError as exc:
            ksft.check(L.nl_errno(exc) == errno.EBUSY,
                       "endpoint-set-direct-reassign-ebusy",
                       "errno=%d" % L.nl_errno(exc))

        now = _ep(fab, 0)
        ksft.check(now.get("fabric-id") == orig.get("fabric-id"),
                   "endpoint-set-direct-reassign-membership-unchanged",
                   "before=%s after=%s" % (orig.get("fabric-id"),
                                           now.get("fabric-id")))
    finally:
        if made_fid is not None:
            try:
                fab.do("fabric-del", {"fabric-id": made_fid})
            except NlError:
                pass


def test_detach_attach_keeps_fabric_ep_id(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    ep_id = 1
    before = _ep(fab, ep_id)
    orig_fid = before["fabric-id"]
    orig_ep_id = before["fabric-ep-id"]
    made_fid = None

    try:
        made_fid = _new_fabric(fab, "move-target", 0xD002)
        g0 = _gen(fab)
        fab.do("endpoint-set", {"endpoint-id": ep_id, "fabric-id": 0})
        g1 = _gen(fab)
        fab.do("endpoint-set", {"endpoint-id": ep_id, "fabric-id": made_fid})
        g2 = _gen(fab)
        now = _ep(fab, ep_id)

        ksft.check(now.get("fabric-id") == made_fid,
                   "endpoint-set-detach-attach-membership",
                   "fabric-id=%s" % now.get("fabric-id"))
        ksft.check(now.get("fabric-ep-id") == orig_ep_id,
                   "endpoint-set-detach-attach-fabric-ep-id",
                   "before=%s after=%s" % (orig_ep_id,
                                           now.get("fabric-ep-id")))
        ksft.check(g1 == g0 + 1 and g2 == g0 + 2,
                   "endpoint-set-detach-attach-two-generation-bumps",
                   "g0=%s g1=%s g2=%s" % (g0, g1, g2))
    except NlError as exc:
        for name in ("endpoint-set-detach-attach-membership",
                     "endpoint-set-detach-attach-fabric-ep-id",
                     "endpoint-set-detach-attach-two-generation-bumps"):
            ksft.not_ok(name, "errno=%d" % L.nl_errno(exc))
    finally:
        try:
            cur = _ep(fab, ep_id)
            if cur.get("fabric-id") not in (0, orig_fid):
                fab.do("endpoint-set", {"endpoint-id": ep_id, "fabric-id": 0})
                cur = _ep(fab, ep_id)
            if cur.get("fabric-id", 0) == 0:
                fab.do("endpoint-set", {"endpoint-id": ep_id,
                                        "fabric-id": orig_fid})
        except NlError:
            pass
        if made_fid is not None:
            try:
                fab.do("fabric-del", {"fabric-id": made_fid})
            except NlError:
                pass


def test_detach_then_failed_attach_leaves_orphan(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    ep_id = 2
    orig_fid = _ep(fab, ep_id)["fabric-id"]
    g0 = _gen(fab)

    try:
        fab.do("endpoint-set", {"endpoint-id": ep_id, "fabric-id": 0})
        g1 = _gen(fab)
        try:
            fab.do("endpoint-set", {"endpoint-id": ep_id,
                                    "fabric-id": 0xFFFFFFFF})
            ksft.not_ok("endpoint-set-failed-attach-enoent", "accepted")
        except NlError as exc:
            ksft.check(L.nl_errno(exc) == errno.ENOENT,
                       "endpoint-set-failed-attach-enoent",
                       "errno=%d" % L.nl_errno(exc))

        now = _ep(fab, ep_id)
        g2 = _gen(fab)
        ksft.check(now.get("fabric-id", 0) == 0,
                   "endpoint-set-failed-attach-keeps-orphan",
                   "fabric-id=%s" % now.get("fabric-id"))
        ksft.check(g1 == g0 + 1 and g2 == g1,
                   "endpoint-set-failed-attach-single-generation-bump",
                   "g0=%s g1=%s g2=%s" % (g0, g1, g2))
    except NlError as exc:
        for name in ("endpoint-set-failed-attach-enoent",
                     "endpoint-set-failed-attach-keeps-orphan",
                     "endpoint-set-failed-attach-single-generation-bump"):
            ksft.not_ok(name, "errno=%d" % L.nl_errno(exc))
    finally:
        try:
            if _ep(fab, ep_id).get("fabric-id", 0) == 0:
                fab.do("endpoint-set", {"endpoint-id": ep_id,
                                        "fabric-id": orig_fid})
        except NlError:
            pass


def test_combined_attach_enable_single_transition(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    orphan = add_orphan(fab, nports=1)
    made_fid = None

    if not orphan:
        for name in ("endpoint-set-combined-attach-enable-state",
                     "endpoint-set-combined-attach-enable-one-ntf",
                     "endpoint-set-combined-attach-enable-one-generation-bump"):
            ksft.not_ok(name, "add_orphan failed")
        return

    oid = orphan["endpoint-id"]
    slot = slot_of(orphan["name"])

    try:
        made_fid = _new_fabric(fab, "combined-target", 0xD003)
        ev = L.DrmFabric()
        ev.ntf_subscribe(L.MCAST_MONITOR)
        L.settle(EVT_SETTLE)
        g0 = _gen(fab)
        fab.do("endpoint-set", {"endpoint-id": oid,
                                "fabric-id": made_fid,
                                "admin-state": "up"})
        got = L.wait_ntf(
            ev, "endpoint-change-ntf", timeout=EVT_DURATION,
            match=lambda n: n["msg"]["endpoint"].get("endpoint-id") == oid)
        got2 = L.wait_ntf(
            ev, "endpoint-change-ntf", timeout=EVT_NEG_DURATION,
            match=lambda n: n["msg"]["endpoint"].get("endpoint-id") == oid)
        now = _ep(fab, oid)
        g1 = _gen(fab)

        ksft.check(now.get("fabric-id") == made_fid and
                   now.get("admin-state") == "up",
                   "endpoint-set-combined-attach-enable-state",
                   "endpoint=%s" % now)
        ksft.check(got is not None and got2 is None,
                   "endpoint-set-combined-attach-enable-one-ntf",
                   "first=%s second=%s" % (got, got2))
        ksft.check(g1 == g0 + 1,
                   "endpoint-set-combined-attach-enable-one-generation-bump",
                   "g0=%s g1=%s" % (g0, g1))
    except NlError as exc:
        for name in ("endpoint-set-combined-attach-enable-state",
                     "endpoint-set-combined-attach-enable-one-ntf",
                     "endpoint-set-combined-attach-enable-one-generation-bump"):
            ksft.not_ok(name, "errno=%d" % L.nl_errno(exc))
    finally:
        try:
            del_ep(fab, slot, orphan["name"])
        except OSError:
            pass
        if made_fid is not None:
            try:
                fab.do("fabric-del", {"fabric-id": made_fid})
            except NlError:
                pass


def test_unassigned_admin_up_and_detach_while_up(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    orphan = add_orphan(fab, nports=1)
    made_fid = None

    if not orphan:
        for name in ("endpoint-set-unassigned-admin-up",
                     "endpoint-set-unassigned-attach-keeps-admin-up",
                     "endpoint-set-detach-while-up-keeps-admin-up"):
            ksft.not_ok(name, "add_orphan failed")
        return

    oid = orphan["endpoint-id"]
    slot = slot_of(orphan["name"])

    try:
        made_fid = _new_fabric(fab, "detach-target", 0xD004)
        fab.do("endpoint-set", {"endpoint-id": oid, "admin-state": "up"})
        orphan_up = _ep(fab, oid)
        ksft.check(orphan_up.get("fabric-id", 0) == 0 and
                   orphan_up.get("admin-state") == "up",
                   "endpoint-set-unassigned-admin-up",
                   "endpoint=%s" % orphan_up)

        fab.do("endpoint-set", {"endpoint-id": oid, "fabric-id": made_fid})
        attached = _ep(fab, oid)
        ksft.check(attached.get("fabric-id") == made_fid and
                   attached.get("admin-state") == "up",
                   "endpoint-set-unassigned-attach-keeps-admin-up",
                   "endpoint=%s" % attached)

        fab.do("endpoint-set", {"endpoint-id": oid, "fabric-id": 0})
        detached = _ep(fab, oid)
        ksft.check(detached.get("fabric-id", 0) == 0 and
                   detached.get("admin-state") == "up",
                   "endpoint-set-detach-while-up-keeps-admin-up",
                   "endpoint=%s" % detached)
    except NlError as exc:
        for name in ("endpoint-set-unassigned-admin-up",
                     "endpoint-set-unassigned-attach-keeps-admin-up",
                     "endpoint-set-detach-while-up-keeps-admin-up"):
            ksft.not_ok(name, "errno=%d" % L.nl_errno(exc))
    finally:
        try:
            del_ep(fab, slot, orphan["name"])
        except OSError:
            pass
        if made_fid is not None:
            try:
                fab.do("fabric-del", {"fabric-id": made_fid})
            except NlError:
                pass


CASES = (
    test_direct_reassignment_refused,
    test_detach_attach_keeps_fabric_ep_id,
    test_detach_then_failed_attach_leaves_orphan,
    test_combined_attach_enable_single_transition,
    test_unassigned_admin_up_and_detach_while_up,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, topology="mesh", need_debugfs=True) as fab:
        fid = fabricsim_fid(fab)
        if fid is None:
            ksft.skip_all("could not find fabricsim fabric")
        cfg = Cfg(fab, fid, NlError)
        L.run_cases(ksft, cfg, CASES)

    ksft.finish()


if __name__ == "__main__":
    main()
