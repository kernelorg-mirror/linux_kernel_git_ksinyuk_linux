#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Confinement is by init_net, not CAP_NET_ADMIN-in-userns: a child that
unshares into its own user+net namespace (or net-only, without
CONFIG_USER_NS) and regains root must still be refused, and specifically
refused *while holding CAP_NET_ADMIN* -- the complement of
cap_netadmin.py's unprivileged-in-init_net case. Verifies the child truly
left init_net and the family still resolves before trusting any -EPERM.

Requires drm_fabric + drm_fabric_sim; run as root. Skips without user
namespace support.
"""

import ctypes
import errno
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

CLONE_NEWUSER = 0x10000000
CLONE_NEWNET = 0x40000000
CAP_NET_ADMIN = 12


def _cap_effective():
    """CapEff bitmask of the calling thread, or None if unreadable."""
    try:
        with open("/proc/self/status", encoding="ascii") as f:
            for line in f:
                if line.startswith("CapEff:"):
                    return int(line.split()[1], 16)
    except OSError:
        pass
    return None


def _map_self(uid, gid):
    """Map @uid/@gid to 0 in the new user namespace. Ids must be read before
    unsharing: an unmapped namespace makes getuid() answer the overflow
    uid, and the kernel only accepts a self-map with the caller's real
    parent-side id.
    """
    try:
        # setgroups must be denied before gid_map is writable.
        with open("/proc/self/setgroups", "w", encoding="ascii") as f:
            f.write("deny")
        with open("/proc/self/uid_map", "w", encoding="ascii") as f:
            f.write("0 %d 1" % uid)
        with open("/proc/self/gid_map", "w", encoding="ascii") as f:
            f.write("0 %d 1" % gid)
    except OSError as exc:
        return "id map: %s" % exc
    return None


def _enter_namespaces():
    """Enter a non-initial network namespace; returns (mode, failure).
    Prefers "user+net" (models container root); falls back to "net" alone
    when CONFIG_USER_NS is absent, which is if anything the sharper case
    since the caller then keeps the initial CAP_NET_ADMIN, isolating the
    namespace check.
    """
    libc = ctypes.CDLL(None, use_errno=True)
    uid, gid = os.getuid(), os.getgid()

    if libc.unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0:
        fail = _map_self(uid, gid)
        return (None, fail) if fail else ("user+net", None)
    first = os.strerror(ctypes.get_errno())

    if libc.unshare(CLONE_NEWNET) == 0:
        return "net", None
    return None, ("user+net: %s; net: %s"
                  % (first, os.strerror(ctypes.get_errno())))


def _try(fab, NlError, fn):
    """Return 'ok' or the positive errno the ABI answered with."""
    try:
        fn(fab)
        return "ok"
    except NlError as exc:
        return L.nl_errno(exc)


def _child_probe(w):
    """Everything measured inside the new namespaces, reported as one JSON blob."""
    out = {"stage": "start"}
    try:
        mode, fail = _enter_namespaces()
        if fail:
            out = {"stage": "unshare", "detail": fail}
            raise SystemExit

        out = {
            "stage": "entered",
            "mode": mode,
            "ns_inode": os.stat("/proc/self/ns/net").st_ino,
            "cap_eff": _cap_effective(),
        }

        _, NlError = L.import_ynl()
        try:
            fab = L.DrmFabric()
        except Exception as exc:  # noqa: BLE001
            out["family"] = "error: %s" % exc
            raise SystemExit
        out["family"] = "ok"

        out["fabric_get"] = _try(fab, NlError,
                                 lambda f: f.do("fabric-get", {"fabric-id": 1}))
        out["fabric_get_dump"] = _try(fab, NlError,
                                      lambda f: list(f.dump("fabric-get", {})))
        out["fabric_new"] = _try(
            fab, NlError,
            lambda f: f.do("fabric-new", {
                "type": "synthetic", "name": "netns", "instance-id": 0x4E5}))
    except SystemExit:
        pass
    except Exception as exc:  # noqa: BLE001
        out["stage"] = "exception"
        out["detail"] = str(exc)
    os.write(w, json.dumps(out).encode())


_PROBE = None


def probe():
    """Run the namespaced child once and cache what it reported."""
    global _PROBE
    if _PROBE is not None:
        return _PROBE

    r, w = os.pipe()
    pid = os.fork()
    if pid == 0:  # child
        os.close(r)
        try:
            _child_probe(w)
        finally:
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

    try:
        _PROBE = json.loads(buf.decode())
    except ValueError:
        _PROBE = {"stage": "no-report"}
    return _PROBE


class Cfg:
    def __init__(self, fab, nl_error):
        self.fab = fab
        self.NlError = nl_error
        self.init_ns = os.stat("/proc/self/ns/net").st_ino


def _entered(ksft, cfg, name):
    """Common gate: report SKIP or FAIL when the child never got far enough."""
    p = probe()
    if p.get("stage") == "unshare":
        ksft.skip(name, "cannot create user+net namespace: %s"
                  % p.get("detail", "?"))
        return None
    if p.get("stage") != "entered":
        ksft.not_ok(name, "child did not reach the namespace: %s" % p)
        return None
    return p


def test_child_left_init_net(ksft, cfg):
    """Control: the child must really be in a different network namespace.
    Without it, a kernel lacking CONFIG_NET_NS could leave the child in
    init_net and every -EPERM below would be vacuous.
    """
    p = _entered(ksft, cfg, "netns-child-left-init-net")
    if p is None:
        return
    ksft.check(p["ns_inode"] != cfg.init_ns, "netns-child-left-init-net",
               "mode=%s child ns=%s parent ns=%s"
               % (p.get("mode"), p["ns_inode"], cfg.init_ns))


def test_child_holds_cap_net_admin(ksft, cfg):
    """Control: the child must hold CAP_NET_ADMIN, else the -EPERM
    assertions below would just be an ordinary unprivileged rejection,
    proving nothing about namespace confinement.
    """
    p = _entered(ksft, cfg, "netns-child-holds-cap-net-admin")
    if p is None:
        return
    cap = p.get("cap_eff")
    ksft.check(cap is not None and bool(cap & (1 << CAP_NET_ADMIN)),
               "netns-child-holds-cap-net-admin",
               "mode=%s CapEff=%s"
               % (p.get("mode"), "?" if cap is None else "0x%x" % cap))


def test_family_visible_in_child_netns(ksft, cfg):
    """Control: the family is netnsok and resolves in the new namespace,
    else the errnos below would be genetlink failing to find it, not the
    family refusing the caller.
    """
    p = _entered(ksft, cfg, "netns-family-resolves")
    if p is None:
        return
    ksft.check(p.get("family") == "ok", "netns-family-resolves",
               "family=%s" % p.get("family"))


def _expect_eperm(ksft, cfg, key, name):
    p = _entered(ksft, cfg, name)
    if p is None:
        return
    if p.get("family") != "ok":
        ksft.not_ok(name, "family did not resolve; errno is not meaningful")
        return
    got = p.get(key)
    ksft.check(got == errno.EPERM, name,
               "mode=%s result=%s (expected EPERM)" % (p.get("mode"), got))


def test_fabric_get_refused(ksft, cfg):
    """A read is refused too: confinement is not limited to mutation."""
    _expect_eperm(ksft, cfg, "fabric_get", "netns-fabric-get-eperm")


def test_fabric_get_dump_refused(ksft, cfg):
    """Dumps take the same check as doit handlers."""
    _expect_eperm(ksft, cfg, "fabric_get_dump", "netns-fabric-get-dump-eperm")


def test_fabric_new_refused(ksft, cfg):
    """Provisioning is refused despite the child holding CAP_NET_ADMIN."""
    _expect_eperm(ksft, cfg, "fabric_new", "netns-fabric-new-eperm")


def test_init_net_topology_unchanged(ksft, cfg):
    """The refused child must not have created anything in init_net."""
    fab, NlError = cfg.fab, cfg.NlError
    try:
        names = [f["fabric"].get("name") for f in fab.dump("fabric-get", {})]
    except NlError as exc:
        ksft.not_ok("netns-init-net-unchanged", "errno=%d" % L.nl_errno(exc))
        return
    ksft.check("netns" not in names, "netns-init-net-unchanged",
               "fabrics=%s" % names)


CASES = (
    test_child_left_init_net,
    test_child_holds_cap_net_admin,
    test_family_visible_in_child_netns,
    test_fabric_get_refused,
    test_fabric_get_dump_refused,
    test_fabric_new_refused,
    test_init_net_topology_unchanged,
)

# Only the provisioning case needs a mutation-capable build; confinement of
# reads and dumps is a query-only contract asserted on either build.
MUTATION_CASES = (
    test_fabric_new_refused,
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
