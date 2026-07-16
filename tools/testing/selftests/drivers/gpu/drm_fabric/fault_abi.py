#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Provider fault injection via fabricsim's fail_register debugfs toggle (cf.
netdevsim's should_fail): a failed provider-driven endpoint create must
surface the provider's errno and leak no endpoint, succeeding once the
fault is cleared.

Requires drm_fabric + drm_fabric_sim with fabricsim debugfs; run as root.
"""

import errno
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L


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


class Cfg:
    def __init__(self, fab, fid):
        self.fab = fab
        self.fid = fid


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


CASES = (
    test_register_fault,
)


def main():
    ksft = L.Ksft()

    with L.fabricsim(ksft, need_debugfs=True, need_control="fail_register") as fab:
        fid = fabricsim_fid(fab)
        if fid is None:
            ksft.skip_all("fabricsim fabric not present")

        L.run_cases(ksft, Cfg(fab, fid), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
