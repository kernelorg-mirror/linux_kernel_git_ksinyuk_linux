#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Provider fault injection via fabricsim's fail_* debugfs toggles (cf.
netdevsim's should_fail): a failed mutation must surface the provider's
exact errno through genetlink, leave core state untouched, emit no change
notification, and succeed once the fault is cleared.

Requires drm_fabric + drm_fabric_sim with fabricsim debugfs; run as root.
"""

import errno
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

# Budget for proving a notification did *not* arrive. Short by design: the
# failing command has already returned before the wait starts, so a success
# notification would have been queued by then.
EVT_NEG_DURATION = float(os.environ.get("EVT_NEG_DURATION", "0.5"))
EVT_SETTLE = float(os.environ.get("EVT_SETTLE", "0.2"))


def eps_by_name(fab):
    return {e["endpoint"]["name"]: e["endpoint"]
            for e in fab.dump("endpoint-get", {})}


def slot_of(name):
    return int(name.rsplit("ep", 1)[1])


def add_via(fab, control, nports=1):
    """Drive a debugfs hotplug-add; return the new endpoint dict (or None)."""
    before = set(eps_by_name(fab))
    L.dbg_write(control, nports)
    new = L.wait_until(lambda: set(eps_by_name(fab)) - before)
    return eps_by_name(fab)[next(iter(new))] if len(new) == 1 else None


def del_via(fab, name, slot):
    L.dbg_write("del_endpoint", slot)
    L.wait_until(lambda: name not in eps_by_name(fab))


def fabricsim_fid(fab):
    for f in fab.dump("fabric-get", {}):
        if f["fabric"]["name"] == "fabricsim":
            return f["fabric"]["fabric-id"]
    return None


def set_fault(name, on):
    L.dbg_write(name, "Y" if on else "N")


def set_fail_errno(code):
    L.dbg_write("fail_errno", int(code))


def has_fail_errno():
    return os.path.exists(os.path.join(L.DEBUGFS, "fail_errno"))


class Cfg:
    def __init__(self, fab, nl_error, fid, orphan):
        self.fab = fab
        self.NlError = nl_error
        self.fid = fid
        self.orphan = orphan
        self.oid = orphan["endpoint-id"]
        self.oslot = slot_of(orphan["name"])


def test_endpoint_set_fault(ksft, cfg):
    """A failed ENDPOINT_SET returns -ENOMEM and rolls back; clearing succeeds."""
    fab, NlError = cfg.fab, cfg.NlError
    orphan, oid, fid = cfg.orphan, cfg.oid, cfg.fid

    ksft.check(orphan.get("fabric-id", 0) == 0, "fault-orphan-precondition",
               "fabric-id=%s" % orphan.get("fabric-id"))

    # A failed mutation must not emit a success notification.
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)

    set_fault("fail_mutation", True)
    try:
        got = None
        try:
            fab.do("endpoint-set", {"endpoint-id": oid, "fabric-id": fid})
        except NlError as exc:
            got = exc.error
        ksft.check(got == errno.ENOMEM, "fault-endpoint-set-returns-enomem",
                   "errno=%s" % got)

        ec = L.wait_ntf(ev, "endpoint-change-ntf", timeout=EVT_NEG_DURATION,
                        match=lambda n: n["msg"]["endpoint"].get("endpoint-id") == oid)
        ksft.check(ec is None, "fault-endpoint-set-emits-no-ntf",
                   "unexpected endpoint-change for %s: %s" % (oid, ec))

        now = eps_by_name(fab).get(orphan["name"], {})
        ksft.check(now.get("fabric-id", 0) == 0, "fault-endpoint-set-failure-atomicity",
                   "fabric-id=%s (expected still-orphan)" % now.get("fabric-id"))
    finally:
        set_fault("fail_mutation", False)
    ok = True
    try:
        fab.do("endpoint-set", {"endpoint-id": oid, "fabric-id": fid})
    except NlError as exc:
        ok = False
        ksft.not_ok("fault-cleared-endpoint-set-ok", "errno=%d" % exc.error)
    if ok:
        attached = eps_by_name(fab).get(orphan["name"], {})
        ksft.check(attached.get("fabric-id") == fid,
                   "fault-cleared-endpoint-set-ok",
                   "fabric-id=%s" % attached.get("fabric-id"))
        ec2 = L.wait_ntf(ev, "endpoint-change-ntf", timeout=EVT_NEG_DURATION,
                        match=lambda n: n["msg"]["endpoint"].get("endpoint-id") == oid)
        ksft.check(ec2 is not None, "fault-cleared-endpoint-set-emits-ntf",
                   "expected endpoint-change for %s, got none" % oid)

    try:
        fab.do("endpoint-set", {"endpoint-id": oid, "fabric-id": 0})
    except NlError:
        pass
    del_via(fab, orphan["name"], cfg.oslot)


def test_port_peer_new_fault(ksft, cfg):
    """A failed PORT_PEER_NEW returns -ENOMEM and leaves no peer behind."""
    fab, NlError = cfg.fab, cfg.NlError
    ep_a = add_via(fab, "add_endpoint", nports=1)
    if ep_a is None:
        ksft.not_ok("fault-port-peer-new-returns-enomem", "add ep failed")
        ksft.not_ok("fault-port-peer-new-failure-atomicity", "add ep failed")
        return
    a_id = ep_a["endpoint-id"]
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    set_fault("fail_mutation", True)
    try:
        got = None
        try:
            fab.do("port-peer-new",
                   {"endpoint-id": a_id, "port-index": 0,
                    "peer": {"peer-id": 0xBEEF, "type": "accel",
                             "port-index": 0}})
        except NlError as exc:
            got = exc.error
        ksft.check(got == errno.ENOMEM, "fault-port-peer-new-returns-enomem",
                   "errno=%s" % got)
        pc = L.wait_ntf(ev, "port-change-ntf", timeout=EVT_NEG_DURATION,
                        match=lambda n: n["msg"]["port"].get("endpoint-id") == a_id)
        ksft.check(pc is None, "fault-port-peer-new-emits-no-ntf",
                   "unexpected port-change for %s: %s" % (a_id, pc))
    finally:
        set_fault("fail_mutation", False)
    pa = fab.do("port-get", {"endpoint-id": a_id, "port-index": 0})["port"]
    ksft.check("peer" not in pa, "fault-port-peer-new-failure-atomicity",
               "unexpected peer=%s" % pa.get("peer"))
    del_via(fab, ep_a["name"], slot_of(ep_a["name"]))


def test_register_fault(ksft, cfg):
    """A failed provider create surfaces -ENOMEM and leaks no endpoint."""
    fab = cfg.fab
    n_before = len(eps_by_name(fab))
    set_fault("fail_register", True)
    try:
        reg_errno = None
        try:
            L.dbg_write("add_endpoint", 1)
        except OSError as exc:
            reg_errno = exc.errno
        # Confirming a non-event needs a bounded wait: poll a short window for a
        # late endpoint after the synchronous -ENOMEM.
        grew = L.wait_until(lambda: len(eps_by_name(fab)) != n_before, timeout=0.3)
        ksft.check(reg_errno == errno.ENOMEM, "fault-register-returns-enomem",
                   "errno=%s" % reg_errno)
        ksft.check(not grew and len(eps_by_name(fab)) == n_before,
                   "fault-register-no-leak",
                   "count changed %d -> %d" % (n_before, len(eps_by_name(fab))))
    finally:
        set_fault("fail_register", False)
    created = add_via(fab, "add_endpoint", nports=1)
    ksft.check(created is not None, "fault-cleared-register-ok")
    if created is not None:
        del_via(fab, created["name"], slot_of(created["name"]))


def test_errno_round_trip(ksft, cfg):
    """A selectable provider errno propagates verbatim (not flattened to ENOMEM)."""
    fab, NlError = cfg.fab, cfg.NlError
    if not has_fail_errno():
        ksft.skip("fault-errno-round-trip", "fail_errno knob absent (old module)")
        return
    ep = add_via(fab, "add_endpoint", nports=1)
    if ep is None:
        ksft.not_ok("fault-errno-round-trip", "add ep failed")
        return
    a_id = ep["endpoint-id"]
    # EBUSY is not the -ENOMEM the other cases use nor a code genl raises itself,
    # so seeing it come back means the provider's errno was preserved verbatim.
    set_fail_errno(errno.EBUSY)
    set_fault("fail_mutation", True)
    try:
        got = None
        try:
            fab.do("port-peer-new",
                   {"endpoint-id": a_id, "port-index": 0,
                    "peer": {"peer-id": 0xBEEF, "type": "accel",
                             "port-index": 0}})
        except NlError as exc:
            got = exc.error
    finally:
        set_fault("fail_mutation", False)
        set_fail_errno(errno.ENOMEM)        # restore the default for later cases
    ksft.check(got == errno.EBUSY, "fault-errno-round-trip",
               "expected EBUSY(%d), got %s" % (errno.EBUSY, got))
    del_via(fab, ep["name"], slot_of(ep["name"]))


def test_port_peer_del_fault(ksft, cfg):
    """A failed PORT_PEER_DEL surfaces the errno and keeps the peer (failure atomicity)."""
    fab, NlError = cfg.fab, cfg.NlError
    ep = add_via(fab, "add_endpoint", nports=1)
    if ep is None:
        ksft.not_ok("fault-port-peer-del-returns-errno", "add ep failed")
        ksft.not_ok("fault-port-peer-del-retained", "add ep failed")
        return
    a_id = ep["endpoint-id"]
    fab.do("port-peer-new",
           {"endpoint-id": a_id, "port-index": 0,
            "peer": {"peer-id": 0xBEEF, "type": "accel", "port-index": 0}})
    # Subscribe after the successful add, so any event seen below belongs to the
    # failing delete rather than the setup.
    ev = L.DrmFabric()
    ev.ntf_subscribe(L.MCAST_MONITOR)
    L.settle(EVT_SETTLE)
    set_fault("fail_mutation", True)
    try:
        got = None
        try:
            fab.do("port-peer-del", {"endpoint-id": a_id, "port-index": 0})
        except NlError as exc:
            got = exc.error
        ksft.check(got == errno.ENOMEM, "fault-port-peer-del-returns-errno",
                   "errno=%s" % got)
        pd = L.wait_ntf(ev, "port-change-ntf", timeout=EVT_NEG_DURATION,
                        match=lambda n: n["msg"]["port"].get("endpoint-id") == a_id)
        ksft.check(pd is None, "fault-port-peer-del-emits-no-ntf",
                   "unexpected port-change for %s: %s" % (a_id, pd))
        pa = fab.do("port-get", {"endpoint-id": a_id, "port-index": 0})["port"]
        ksft.check("peer" in pa, "fault-port-peer-del-retained",
                   "peer unexpectedly removed after failed delete")
    finally:
        set_fault("fail_mutation", False)
    try:
        fab.do("port-peer-del", {"endpoint-id": a_id, "port-index": 0})
    except NlError as exc:
        ksft.not_ok("fault-port-peer-del-cleared-ok", "errno=%d" % exc.error)
    else:
        pa = fab.do("port-get", {"endpoint-id": a_id, "port-index": 0})["port"]
        ksft.check("peer" not in pa, "fault-port-peer-del-cleared-ok",
                   "peer still present after clear")
    del_via(fab, ep["name"], slot_of(ep["name"]))


CASES = (
    test_endpoint_set_fault,
    test_port_peer_new_fault,
    test_register_fault,
    test_errno_round_trip,
    test_port_peer_del_fault,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, need_debugfs=True, need_control="fail_mutation") as fab:
        fid = fabricsim_fid(fab)
        if fid is None:
            ksft.skip_all("fabricsim fabric not present")

        orphan = add_via(fab, "add_orphan", nports=1)
        if orphan is None:
            ksft.skip_all("could not create orphan endpoint")

        L.run_cases(ksft, Cfg(fab, NlError, fid, orphan), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
