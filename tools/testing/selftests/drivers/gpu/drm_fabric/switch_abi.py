#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
fabricsim's "switch" shape links each leaf's first port to an opaque
switch that is not a registered endpoint: asserts half-edge serialization
and peer-id non-resolution, not leaf-switch-leaf reachability.

--no-load is ignored (needs a fresh insmod). Run as root.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L


def ports_of(fab, ep_id):
    return [p["port"] for p in fab.dump("port-get", {"endpoint-id": ep_id})]


def switch_peers(fab, sim_eps):
    """All TYPE=switch peers across the sim leaves: [(ep, peer), ...]."""
    out = []
    for e in sim_eps:
        for p in ports_of(fab, e["endpoint-id"]):
            peer = p.get("peer")
            if peer and peer.get("type") == "switch":
                out.append((e, peer))
    return out


class Cfg:
    def __init__(self, fab, sim_eps, sw_peers):
        self.fab = fab
        self.sim_eps = sim_eps
        self.sw_peers = sw_peers
        self.abort = False


def test_every_leaf_has_switch_peer(ksft, cfg):
    """Every leaf endpoint has a port with a TYPE=switch half-edge."""
    ksft.check(len(cfg.sw_peers) == len(cfg.sim_eps),
               "switch-every-leaf-has-switch-peer",
               "switch-peers=%d leaves=%d"
               % (len(cfg.sw_peers), len(cfg.sim_eps)))
    if not cfg.sw_peers:
        cfg.abort = True


def test_half_edge_fully_serialized(ksft, cfg):
    """Every switch half-edge carries all three peer fields."""
    complete = all(
        {"peer-id", "type", "port-index"} <= set(peer)
        for _, peer in cfg.sw_peers)
    ksft.check(complete, "switch-half-edge-fully-serialized",
               "a switch peer is missing peer-id/type/port-index")


def test_single_opaque_switch_id(ksft, cfg):
    """All leaves name one opaque switch id, each via a distinct switch port."""
    ids = {peer["peer-id"] for _, peer in cfg.sw_peers}
    ports = [peer["port-index"] for _, peer in cfg.sw_peers]
    ksft.check(len(ids) == 1, "switch-single-opaque-id",
               "switch peer-ids=%s" % sorted(ids))
    ksft.check(len(set(ports)) == len(ports), "switch-distinct-switch-ports",
               "switch-side port-indexes=%s" % sorted(ports))


def test_switch_id_does_not_resolve(ksft, cfg):
    """The opaque switch id is not a registered endpoint (local adjacency)."""
    ep_fepids = {e["fabric-ep-id"] for e in cfg.sim_eps}
    sw_ids = {peer["peer-id"] for _, peer in cfg.sw_peers}
    leaked = sw_ids & ep_fepids
    ksft.check(not leaked, "switch-id-does-not-resolve-to-endpoint",
               "switch id resolves to an endpoint fabric-ep-id: %s"
               % sorted(leaked))


CASES = (
    test_every_leaf_has_switch_peer,
    test_half_edge_fully_serialized,
    test_single_opaque_switch_id,
    test_switch_id_does_not_resolve,
)


def main():
    ksft = L.Ksft()

    # The switch shape is an insmod parameter, so fabricsim is always reloaded
    # with topology=switch (ignoring --no-load) and restored to default on exit.
    with L.fabricsim(ksft, topology="switch") as fab:
        eps = [e["endpoint"] for e in fab.dump("endpoint-get", {})]
        # Restrict to fabricsim's members (ignore anything a prior suite left).
        sim_eps = [e for e in eps if e["name"].startswith("sim-ep")]
        if len(sim_eps) < 2:
            ksft.skip_all("switch topology needs >= 2 endpoints, got %d"
                          % len(sim_eps))

        sw_peers = switch_peers(fab, sim_eps)
        L.run_cases(ksft, Cfg(fab, sim_eps, sw_peers), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
