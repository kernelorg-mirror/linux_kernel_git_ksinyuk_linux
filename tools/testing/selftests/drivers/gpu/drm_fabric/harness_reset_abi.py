#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Verify recovery after a SIGKILL-terminated test. reset_sim_or_fail() must
restore topology isolation at the next test's entry.

Requires root, YNL, drm_fabric, and drm_fabric_sim.
"""

import os
import signal
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L  # noqa: E402 - shared KTAP/module helpers

_HERE = os.path.dirname(os.path.abspath(__file__))

# Helper process: load the switch shape, then idle so the parent can kill it
# mid-life. It deliberately installs no cleanup -- SIGKILL would bypass it.
_CHILD = (
    "import sys, time\n"
    "sys.path.insert(0, %r)\n"
    "import lib_drm_fabric as L\n"
    "L.rmmod('drm_fabric_sim')\n"
    "if not L.module_loaded('drm_fabric'):\n"
    "    L.insmod('drm-fabric.ko')\n"
    "L.insmod('drm-fabric-sim.ko', 'topology=switch')\n"
    "time.sleep(120)\n"
) % _HERE


def _has_switch_peer(fab):
    """True if any leaf port carries a TYPE=switch half-edge (switch shape)."""
    for e in fab.dump("endpoint-get", {}):
        ep_id = e["endpoint"]["endpoint-id"]
        for p in fab.dump("port-get", {"endpoint-id": ep_id}):
            peer = p["port"].get("peer")
            if peer and peer.get("type") == "switch":
                return True
    return False


class Cfg:
    def __init__(self, nl_error):
        self.NlError = nl_error


def test_sigkill_topology_recovery(ksft, cfg):
    """A SIGKILL-leaked switch shape must not survive the next entry reset."""
    # 1. Bring up the switch shape in a helper and confirm it is observable.
    child = subprocess.Popen([sys.executable, "-c", _CHILD])
    try:
        if not L.wait_until(lambda: L.module_loaded("drm_fabric"), timeout=10.0):
            ksft.skip("harness-reset-sigkill-recovery",
                      "core module did not load")
            return
        fab = L.DrmFabric()
        loaded = L.wait_until(
            lambda: L.module_loaded("drm_fabric_sim") and _has_switch_peer(fab),
            timeout=10.0)
        if not loaded:
            child.send_signal(signal.SIGKILL)
            ksft.skip("harness-reset-sigkill-recovery",
                      "helper could not establish switch shape")
            return

        # 2. Terminate through the SIGKILL path: no cleanup runs, so the switch
        #    sim stays loaded exactly as a hard-timed-out test would leave it.
        child.send_signal(signal.SIGKILL)
        child.wait()
    finally:
        if child.poll() is None:
            child.send_signal(signal.SIGKILL)
            child.wait()

    stale = L.module_loaded("drm_fabric_sim")
    ksft.check(stale, "harness-reset-sigkill-leaves-stale-sim",
               "sim unexpectedly unloaded by the killed helper")

    # 3. The next test's entry reset must recover a known default shape.
    # Mid-case: a result was already emitted above, so a failed reset here
    # must become a not_ok(), not a skip_all() 0-plan (invalid once results
    # are on stdout). reset_sim_or_fail() already reported the failure, so
    # bail out rather than emitting a second, precondition-less check.
    if not L.reset_sim_or_fail(ksft, "harness-reset-sigkill-recovery-setup",
                                topology="mesh"):
        return
    fab = L.DrmFabric()
    ksft.check(not _has_switch_peer(fab),
               "harness-reset-sigkill-recovery",
               "switch half-edge survived reset_sim(mesh)")


CASES = (
    test_sigkill_topology_recovery,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()
    if not L.is_root():
        ksft.skip_all("must run as root (insmod + genetlink)")
    # This suite drives module load/unload itself rather than via fabricsim().
    try:
        L.run_cases(ksft, Cfg(NlError), CASES)
    finally:
        # Leave a sane default shape for whatever suite runs next.
        L.sim_restore_default()
    ksft.finish()


if __name__ == "__main__":
    main()
