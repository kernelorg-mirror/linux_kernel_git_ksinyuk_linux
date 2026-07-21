#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Endpoint selector ambiguity coverage for multiple endpoints sharing one parent
device in drm_fabric_sim.
"""

import errno
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L


def _ep(fab, ep_id):
    return fab.do("endpoint-get", {"endpoint-id": ep_id})["endpoint"]


def _gen(fab):
    return fab.do("port-get", {"endpoint-id": 2,
                               "port-index": 0}).get("topology-generation")


class Cfg:
    def __init__(self, fab, nl_error):
        self.fab = fab
        self.NlError = nl_error


def test_shared_parent_selector_ambiguity(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    ep0 = _ep(fab, 0)
    ep1 = _ep(fab, 1)
    shared_dev = ep0["dev-name"]
    bus_name = ep0["bus-name"]

    ksft.check(shared_dev == ep1.get("dev-name"),
               "shared-parent-precondition-dev-name",
               "ep0=%s ep1=%s" % (shared_dev, ep1.get("dev-name")))
    ksft.check(bus_name == ep1.get("bus-name"),
               "shared-parent-precondition-bus-name",
               "ep0=%s ep1=%s" % (bus_name, ep1.get("bus-name")))

    for name, req in (
            ("shared-parent-endpoint-get-dev-name-einval",
             {"dev-name": shared_dev}),
            ("shared-parent-endpoint-get-dev-bus-einval",
             {"dev-name": shared_dev, "bus-name": bus_name}),
    ):
        try:
            fab.do("endpoint-get", req)
            ksft.not_ok(name, "accepted")
        except NlError as exc:
            ksft.check(L.nl_errno(exc) == errno.EINVAL, name,
                       "errno=%d" % L.nl_errno(exc))

    got = _ep(fab, 0)
    ksft.check(got.get("endpoint-id") == 0,
               "shared-parent-endpoint-get-by-id",
               "endpoint=%s" % got)

    g0 = _gen(fab)
    try:
        fab.do("endpoint-set", {"dev-name": shared_dev, "admin-state": "down"})
        ksft.not_ok("shared-parent-endpoint-set-ambiguous-einval", "accepted")
    except NlError as exc:
        ksft.check(L.nl_errno(exc) == errno.EINVAL,
                   "shared-parent-endpoint-set-ambiguous-einval",
                   "errno=%d" % L.nl_errno(exc))

    ep0_after = _ep(fab, 0)
    ep1_after = _ep(fab, 1)
    g1 = _gen(fab)
    ksft.check(ep0_after.get("admin-state") == "up" and
               ep1_after.get("admin-state") == "up" and g1 == g0,
               "shared-parent-endpoint-set-no-modification",
               "ep0=%s ep1=%s g0=%s g1=%s" % (ep0_after, ep1_after, g0, g1))


CASES = (test_shared_parent_selector_ambiguity,)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, topology="mesh",
                     sim_args=["shared_parent_eps=2"]) as fab:
        cfg = Cfg(fab, NlError)
        L.run_cases(ksft, cfg, CASES)

    ksft.finish()


if __name__ == "__main__":
    main()
