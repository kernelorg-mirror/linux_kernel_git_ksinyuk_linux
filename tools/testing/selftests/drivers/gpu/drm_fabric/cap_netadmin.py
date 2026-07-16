#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
CAP_NET_ADMIN enforcement on the mutation commands: an unprivileged child
(forked, uid dropped before the socket opens) is refused with -EPERM; also
covers the -EINVAL/-ENOENT/-EEXIST rejection paths.

Requires drm_fabric + drm_fabric_sim loaded; run as root.
"""

import errno
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

UNPRIV_UID = int(os.environ.get("UNPRIV_UID", "65534"))


def run_unpriv(method, vals):
    """Run a single `do` under an unprivileged uid in a child process.
    Returns (ok, err): ok True on success, err the positive errno on
    NlError. Result crosses via a JSON line over a pipe.
    """
    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:  # child
        os.close(r)
        result = {"kind": "exc", "val": "setup"}
        try:
            try:
                os.setgroups([])
            except OSError:
                pass
            os.setresgid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID)
            os.setresuid(UNPRIV_UID, UNPRIV_UID, UNPRIV_UID)
            _, NlError = L.import_ynl()
            fam = L.DrmFabric()
            try:
                fam.do(method, vals)
                result = {"kind": "ok", "val": None}
            except NlError as exc:
                result = {"kind": "err", "val": exc.error}
        except Exception as exc:  # noqa: BLE001
            result = {"kind": "exc", "val": str(exc)}
        os.write(w, json.dumps(result).encode())
        os.close(w)
        os._exit(0)

    os.close(w)
    buf = b""
    while True:
        chunk = os.read(r, 4096)
        if not chunk:
            break
        buf += chunk
    os.close(r)
    os.waitpid(pid, 0)
    result = json.loads(buf.decode())
    return (result["kind"] == "ok",
            result["val"] if result["kind"] == "err" else None)


class Cfg:
    def __init__(self, fab, nl_error):
        self.fab = fab
        self.NlError = nl_error


def test_cap_fabric_new_privileged(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    new_fid = None
    try:
        rep = fab.do("fabric-new",
                     {"type": "synthetic",
                      "name": "captest",
                      "instance-id": 0xCA9})
        new_fid = rep.get("fabric-id")
        ksft.check(new_fid is not None, "cap-fabric-new-privileged",
                   "reply=%s" % rep)
    except NlError as exc:
        ksft.not_ok("cap-fabric-new-privileged", "errno=%d" % exc.error)
    if new_fid is not None:
        try:
            fab.do("fabric-del", {"fabric-id": new_fid})
        except NlError:
            pass


def test_cap_fabric_new_unprivileged(ksft, cfg):
    ok, err = run_unpriv("fabric-new",
                         {"type": "synthetic",
                          "name": "nope",
                          "instance-id": 0x4E0})
    ksft.check(not ok and err == errno.EPERM, "cap-fabric-new-unprivileged-eperm",
               "ok=%s errno=%s" % (ok, err))


def test_cap_port_set_unprivileged(ksft, cfg):
    ok, err = run_unpriv("port-set",
                         {"endpoint-id": 0, "port-index": 0, "admin-state": "down"})
    ksft.check(not ok and err == errno.EPERM, "cap-port-set-unprivileged-eperm",
               "ok=%s errno=%s" % (ok, err))


def test_cap_port_set_privileged(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    ok_priv = True
    detail = ""
    try:
        fab.do("port-set", {"endpoint-id": 0, "port-index": 0, "admin-state": "down"})
    except NlError as exc:
        ok_priv = False
        detail = "errno=%d" % exc.error
    try:
        fab.do("port-set", {"endpoint-id": 0, "port-index": 0, "admin-state": "up"})
    except NlError:
        pass
    ksft.check(ok_priv, "cap-port-set-privileged-ok", detail)


def test_cap_fabric_get_unprivileged(ksft, cfg):
    ok, err = run_unpriv("fabric-get", {"fabric-id": 1})
    ksft.check(ok, "cap-fabric-get-unprivileged-ok", "errno=%s" % err)


def test_reject_fabric_del_unknown(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("fabric-del", {"fabric-id": 4294967295})
        ksft.not_ok("reject-fabric-del-unknown-enoent", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.ENOENT, "reject-fabric-del-unknown-enoent",
                   "errno=%d" % exc.error)


def test_reject_fabric_new_no_type(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("fabric-new", {"name": "no-type"})
        ksft.not_ok("reject-fabric-new-no-type-einval", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.EINVAL, "reject-fabric-new-no-type-einval",
                   "errno=%d" % exc.error)


# A zero fabric type has no ynl symbolic name; the raw probe is in
# nl_policy_probe.py.

USER_PORT = 3


def test_reject_port_peer_new_provider_managed(ksft, cfg):
    """PORT_PEER_NEW on a provider-managed port is refused with -EOPNOTSUPP."""
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("port-peer-new",
               {"endpoint-id": 0, "port-index": 0,
                "peer": {"peer-id": 258, "type": "accel", "port-index": 0}})
        ksft.not_ok("reject-port-peer-new-provider-managed-eopnotsupp", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.EOPNOTSUPP,
                   "reject-port-peer-new-provider-managed-eopnotsupp",
                   "errno=%d" % exc.error)


def test_userspace_peer_roundtrip(ksft, cfg):
    """A userspace-managed port takes PORT_PEER_NEW, rejects a duplicate with
    -EEXIST, and clears with PORT_PEER_DEL."""
    fab, NlError = cfg.fab, cfg.NlError
    peer = {"peer-id": 258, "type": "accel", "port-index": 0}
    try:
        fab.do("port-peer-new",
               {"endpoint-id": 0, "port-index": USER_PORT, "peer": peer})
    except NlError as exc:
        ksft.not_ok("userspace-peer-new-ok", "errno=%d" % exc.error)
        return
    ksft.ok("userspace-peer-new-ok")
    try:
        fab.do("port-peer-new",
               {"endpoint-id": 0, "port-index": USER_PORT, "peer": peer})
        ksft.not_ok("userspace-peer-new-dup-eexist", "accepted duplicate")
    except NlError as exc:
        ksft.check(exc.error == errno.EEXIST, "userspace-peer-new-dup-eexist",
                   "errno=%d" % exc.error)
    # Clear it again so the reject suite leaves the port unlinked.
    try:
        fab.do("port-peer-del", {"endpoint-id": 0, "port-index": USER_PORT})
        ksft.ok("userspace-peer-del-ok")
    except NlError as exc:
        ksft.not_ok("userspace-peer-del-ok", "errno=%d" % exc.error)


def _reject_incomplete_peer(ksft, cfg, peer, name):
    """A port-peer-new with an incomplete peer must be refused with -EINVAL.
    Targets the userspace-managed port, so the rejection is unambiguously
    peer-attribute validation, not the provider/userspace mode check.
    """
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("port-peer-new",
               {"endpoint-id": 0, "port-index": USER_PORT, "peer": peer})
        ksft.not_ok(name, "accepted incomplete peer")
    except NlError as exc:
        ksft.check(exc.error == errno.EINVAL, name, "errno=%d" % exc.error)


def test_reject_port_peer_new_no_type(ksft, cfg):
    """A peer without a type is rejected (no valid peer type is 0)."""
    _reject_incomplete_peer(ksft, cfg, {"peer-id": 258, "port-index": 0},
                            "reject-port-peer-new-no-type-einval")


def test_reject_port_peer_new_no_port_index(ksft, cfg):
    """A peer without a port-index is rejected (0 would be a valid index)."""
    _reject_incomplete_peer(ksft, cfg, {"peer-id": 258, "type": "accel"},
                            "reject-port-peer-new-no-port-index-einval")


def test_reject_port_peer_new_no_peer_id(ksft, cfg):
    """A peer without a peer-id is rejected."""
    _reject_incomplete_peer(ksft, cfg, {"type": "accel", "port-index": 0},
                            "reject-port-peer-new-no-peer-id-einval")


def test_reject_fabric_del_provider(ksft, cfg):
    """FABRIC_DEL refuses a provider-owned fabric with -EPERM."""
    fab, NlError = cfg.fab, cfg.NlError
    prov = [f["fabric"] for f in fab.dump("fabric-get", {})
            if f["fabric"].get("name") == "fabricsim"]
    if not prov:
        ksft.skip("reject-fabric-del-provider-eperm", "no fabricsim fabric")
        return
    try:
        fab.do("fabric-del", {"fabric-id": prov[0]["fabric-id"]})
        ksft.not_ok("reject-fabric-del-provider-eperm", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.EPERM, "reject-fabric-del-provider-eperm",
                   "errno=%d" % exc.error)


def test_reject_port_peer_del_unlinked(ksft, cfg):
    """PORT_PEER_DEL on the unlinked userspace-managed port is -ENOENT."""
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("port-peer-del", {"endpoint-id": 0, "port-index": USER_PORT})
        ksft.not_ok("reject-port-peer-del-unlinked-enoent", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.ENOENT, "reject-port-peer-del-unlinked-enoent",
                   "errno=%d" % exc.error)


def test_reject_port_peer_del_provider_managed(ksft, cfg):
    """PORT_PEER_DEL on a provider-managed port is refused with -EOPNOTSUPP."""
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("port-peer-del", {"endpoint-id": 0, "port-index": 0})
        ksft.not_ok("reject-port-peer-del-provider-managed-eopnotsupp", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.EOPNOTSUPP,
                   "reject-port-peer-del-provider-managed-eopnotsupp",
                   "errno=%d" % exc.error)


def test_reject_endpoint_set_empty(ksft, cfg):
    fab, NlError = cfg.fab, cfg.NlError
    try:
        fab.do("endpoint-set", {"endpoint-id": 0})
        ksft.not_ok("reject-endpoint-set-empty-einval", "accepted")
    except NlError as exc:
        ksft.check(exc.error == errno.EINVAL, "reject-endpoint-set-empty-einval",
                   "errno=%d" % exc.error)


CASES = (
    test_cap_fabric_new_privileged,
    test_cap_fabric_new_unprivileged,
    test_cap_port_set_unprivileged,
    test_cap_port_set_privileged,
    test_cap_fabric_get_unprivileged,
    test_reject_fabric_del_unknown,
    test_reject_fabric_del_provider,
    test_reject_fabric_new_no_type,
    test_reject_port_peer_new_provider_managed,
    test_userspace_peer_roundtrip,
    test_reject_port_peer_new_no_type,
    test_reject_port_peer_new_no_port_index,
    test_reject_port_peer_new_no_peer_id,
    test_reject_port_peer_del_unlinked,
    test_reject_port_peer_del_provider_managed,
    test_reject_endpoint_set_empty,
)

MUTATION_CASES = tuple(
    case for case in CASES
    if case is not test_cap_fabric_get_unprivileged
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft) as fab:
        L.run_cases(ksft, Cfg(fab, NlError),
                    L.select_cases(fab, CASES, MUTATION_CASES))
    ksft.finish()


if __name__ == "__main__":
    main()
