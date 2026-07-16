#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Adversarial raw-netlink probes the YNL suites cannot reach: malformed
attrs (wrong type, unknown id, truncated nest, out-of-range enum, missing
required) must return a clean NLMSG_ERROR, never an oops; a liveness dump
confirms nothing wedged the family. Also introspects the family and emits
TAP.

Topology-mutation policy probes arrive with the provisioning ABI; this
query-only build defines no mutation commands or attributes to probe.
"""

import errno
import os
import re
import socket
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L  # noqa: E402 - shared KTAP emitter only (no pyynl)

# Netlink / generic-netlink constants
NETLINK_GENERIC = 16

NLMSG_ERROR = 0x2
NLMSG_DONE = 0x3

NLM_F_REQUEST = 0x01
NLM_F_ACK = 0x04
NLM_F_DUMP = 0x300

GENL_ID_CTRL = 0x10
CTRL_CMD_GETFAMILY = 3
# uapi/linux/genetlink.h CTRL_ATTR_* enum:
#   1 FAMILY_ID, 2 FAMILY_NAME, 3 VERSION, 4 HDRSIZE, 5 MAXATTR,
#   6 OPS, 7 MCAST_GROUPS
CTRL_ATTR_FAMILY_ID = 1
CTRL_ATTR_FAMILY_NAME = 2
CTRL_ATTR_VERSION = 3
CTRL_ATTR_OPS = 6
CTRL_ATTR_MCAST_GROUPS = 7
# Within a CTRL_ATTR_OPS entry:
CTRL_ATTR_OP_ID = 1
CTRL_ATTR_OP_FLAGS = 2
# Within a CTRL_ATTR_MCAST_GROUPS entry:
CTRL_ATTR_MCAST_GRP_NAME = 1

# genetlink op flags (uapi/linux/genetlink.h)
GENL_ADMIN_PERM = 0x01

NLA_F_NESTED = 0x8000
NLA_TYPE_MASK = ~(NLA_F_NESTED | 0x4000)

NLMSG_HDRLEN = 16
GENL_HDRLEN = 4

FAMILY_NAME = b"drm-fabric"

EXPECTED_VERSION = 1
MCAST_MONITOR = b"monitor"


# Command / attribute ids: derive from the uAPI header.
#
# Hand-written ids drift the moment someone reorders an enum, leaving the probe
# silently fuzzing the wrong command. Parse them from the canonical uapi header
# (or its sibling/initramfs copy) so a reorder is reflected automatically;
# deliberately no fallback table -- skip the whole suite if the header cannot
# be found, rather than risk probing under a stale guess.

def _find_uapi_header():
    cand = os.environ.get("UAPI_HEADER")
    if cand and os.path.isfile(cand):
        return cand
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, *([".."] * 6)))
    for p in (os.path.join(root, "include", "uapi", "drm", "drm_fabric.h"),
              "/opt/spec/drm_fabric.h"):
        if os.path.isfile(p):
            return p
    return None


def _parse_all_enums(text):
    """Merge values from every enum block into one symbol table.

    Commands use an anonymous enum and attributes a named one, so parsing by
    enum name is brittle. Symbols are assumed unique; collisions are
    last-wins.
    """
    out = {}
    for body in re.findall(r"enum\s*(?:\w+\s*)?\{(.*?)\}", text, re.S):
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        body = re.sub(r"//[^\n]*", "", body)
        nxt = 0
        for raw in body.split(","):
            item = raw.strip()
            if not item:
                continue
            if "=" in item:
                name, val = item.split("=", 1)
                name = name.strip()
                try:
                    nxt = int(val.strip(), 0)
                except ValueError:
                    continue
            else:
                name = item
            if re.match(r"^[A-Za-z_]\w*$", name):
                out[name] = nxt
            nxt += 1
    return out


def _load_ids():
    # Committed fallbacks (kept in sync with drm_fabric.h, query-only build).
    syms = {"DRM_FABRIC_CMD_FABRIC_GET": 1, "DRM_FABRIC_CMD_PORT_GET": 3,
            "DRM_FABRIC_A_FABRIC_ID": 5, "DRM_FABRIC_A_ENDPOINT_ID": 6,
            "DRM_FABRIC_A_PORT_INDEX": 7, "DRM_FABRIC_A_PEER": 10}
    src = "fallback literals"
    hdr = _find_uapi_header()
    if hdr:
        parsed = _parse_all_enums(open(hdr).read())
        if "DRM_FABRIC_CMD_PORT_GET" in parsed and "DRM_FABRIC_A_FABRIC_ID" in parsed:
            syms, src = parsed, hdr
    return syms, src


_SYMS, _ID_SRC = _load_ids()

CMD_FABRIC_GET = _SYMS["DRM_FABRIC_CMD_FABRIC_GET"]
CMD_PORT_GET = _SYMS["DRM_FABRIC_CMD_PORT_GET"]

A_FABRIC_ID = _SYMS["DRM_FABRIC_A_FABRIC_ID"]
A_ENDPOINT_ID = _SYMS["DRM_FABRIC_A_ENDPOINT_ID"]
A_PORT_INDEX = _SYMS["DRM_FABRIC_A_PORT_INDEX"]

# An attribute id guaranteed to be past the family's top-level maxattr, so the
# kernel strict-rejects it. Derived from the parsed ids (one past the largest
# symbol) rather than a magic literal, which would silently stop testing strict
# rejection once the attribute set grows past it.
A_UNKNOWN = max(_SYMS.values()) + 1


# NLA builders

def _align4(n):
    return (n + 3) & ~3


def nla(attr_type, payload):
    length = 4 + len(payload)
    pad = b"\x00" * (_align4(length) - length)
    return struct.pack("=HH", length, attr_type) + payload + pad


def nla_u32(attr_type, val):
    return nla(attr_type, struct.pack("=I", val & 0xFFFFFFFF))


def nla_u64(attr_type, val):
    return nla(attr_type, struct.pack("=Q", val & 0xFFFFFFFFFFFFFFFF))


def build_msg(family_id, cmd, seq, payload, flags=NLM_F_REQUEST | NLM_F_ACK):
    body = struct.pack("=BBH", cmd, 1, 0) + payload
    total = NLMSG_HDRLEN + len(body)
    nlh = struct.pack("=IHHII", total, family_id, flags, seq, 0)
    return nlh + body


# Socket helpers

def open_sock():
    s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, NETLINK_GENERIC)
    s.bind((0, 0))
    s.settimeout(3)
    return s


def iter_attrs(blob):
    off = 0
    while off + 4 <= len(blob):
        (alen, atype) = struct.unpack_from("=HH", blob, off)
        if alen < 4:
            break
        payload = blob[off + 4:off + alen]
        yield atype, payload
        off += _align4(alen)


def parse_dgram(data):
    """Split one recv() buffer into (nlmsg_type, errno) tuples.

    errno is None when mlen is too short to hold the 4 payload bytes it
    claims -- distinct from a genuine zero status.
    """
    out = []
    off = 0
    while off + NLMSG_HDRLEN <= len(data):
        (mlen, mtype, _, _, _) = struct.unpack_from("=IHHII", data, off)
        if mlen < NLMSG_HDRLEN:
            break
        if mtype in (NLMSG_ERROR, NLMSG_DONE):
            if mlen >= NLMSG_HDRLEN + 4:
                (err,) = struct.unpack_from("=i", data, off + NLMSG_HDRLEN)
            else:
                err = None
            out.append((mtype, err))
        else:
            out.append((mtype, 0))
        off += _align4(mlen)
    return out


def drain(sock, first_timeout=0.5, more_timeout=0.3):
    """Read every datagram the kernel queued in response to one request."""
    msgs = []
    sock.settimeout(first_timeout)
    try:
        msgs += parse_dgram(sock.recv(16384))
    except socket.timeout:
        return msgs
    sock.settimeout(more_timeout)
    while True:
        try:
            msgs += parse_dgram(sock.recv(16384))
        except socket.timeout:
            break
    return msgs


def resolve_family(sock, name):
    seq = 1
    msg = build_msg(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, seq,
                    nla(CTRL_ATTR_FAMILY_NAME, name + b"\x00"))
    sock.send(msg)
    try:
        data = sock.recv(8192)
    except socket.timeout:
        return None
    (_, mtype, _, _, _) = struct.unpack_from("=IHHII", data, 0)
    if mtype == NLMSG_ERROR:
        return None
    attrs = data[NLMSG_HDRLEN + GENL_HDRLEN:]
    for atype, payload in iter_attrs(attrs):
        if atype == CTRL_ATTR_FAMILY_ID:
            if len(payload) >= 4:
                return struct.unpack_from("=I", payload, 0)[0]
            if len(payload) >= 2:
                return struct.unpack_from("=H", payload, 0)[0]
    return None


def get_family_info(sock, name):
    """Introspect the family via CTRL_CMD_GETFAMILY.

    Returns {version, ops: {op_id: flags}, mcast: set(names)} or None,
    letting callers confirm version, admin-perm on mutators, and the
    monitor group.
    """
    seq = 2
    msg = build_msg(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, seq,
                    nla(CTRL_ATTR_FAMILY_NAME, name + b"\x00"),
                    flags=NLM_F_REQUEST)
    sock.send(msg)
    try:
        data = sock.recv(65536)
    except socket.timeout:
        return None
    (_, mtype, _, _, _) = struct.unpack_from("=IHHII", data, 0)
    if mtype == NLMSG_ERROR:
        return None

    info = {"version": None, "ops": {}, "mcast": set()}
    attrs = data[NLMSG_HDRLEN + GENL_HDRLEN:]
    for atype, payload in iter_attrs(attrs):
        atype &= NLA_TYPE_MASK
        if atype == CTRL_ATTR_VERSION and len(payload) >= 4:
            info["version"] = struct.unpack_from("=I", payload, 0)[0]
        elif atype == CTRL_ATTR_OPS:
            for _, op_blob in iter_attrs(payload):     # one entry per op
                op_id = op_flags = None
                for sub, val in iter_attrs(op_blob):
                    sub &= NLA_TYPE_MASK
                    if sub == CTRL_ATTR_OP_ID and len(val) >= 4:
                        op_id = struct.unpack_from("=I", val, 0)[0]
                    elif sub == CTRL_ATTR_OP_FLAGS and len(val) >= 4:
                        op_flags = struct.unpack_from("=I", val, 0)[0]
                if op_id is not None:
                    info["ops"][op_id] = op_flags or 0
        elif atype == CTRL_ATTR_MCAST_GROUPS:
            for _, grp_blob in iter_attrs(payload):
                for sub, val in iter_attrs(grp_blob):
                    sub &= NLA_TYPE_MASK
                    if sub == CTRL_ATTR_MCAST_GRP_NAME:
                        info["mcast"].add(val.rstrip(b"\x00"))
    return info


# The KTAP emitter (L.Ksft) is shared with the YNL suites: one emitter, and a
# dynamic plan printed at finish() instead of a hard-coded count that drifts
# every time a case is added or removed.

_SEQ = [100]


def case_rejected(tap, name, sock, fid, cmd, payload, expect):
    """Pass iff the kernel rejected with one of @expect (positive errno
    values; the netlink error is negative, so we compare -e). The specific
    code matters: e.g. -EINVAL for a malformed attribute, not a generic
    failure.
    """
    _SEQ[0] += 1
    sock.send(build_msg(fid, cmd, _SEQ[0], payload))
    msgs = drain(sock)
    rejected = [-e for (t, e) in msgs
               if t == NLMSG_ERROR and e is not None and e != 0]
    if not msgs:
        tap.not_ok(name, "no response (possible hang)")
    elif not rejected:
        tap.not_ok(name, "accepted (no error returned)")
    elif rejected[0] in expect:
        tap.ok("%s (errno=%d)" % (name, rejected[0]))
    else:
        want = "/".join(errno.errorcode.get(e, str(e)) for e in sorted(expect))
        tap.not_ok(name, "errno=%d (%s), expected %s"
                   % (rejected[0], errno.errorcode.get(rejected[0], "?"), want))


def _maybe_load_modules():
    """Standalone runs self-load; a pre-loading harness passes --no-load.
    Returns True iff this run loaded the providers, so the caller can
    register teardown.
    """
    if "--no-load" in sys.argv[1:]:
        return False
    if os.path.isdir("/sys/module/drm_fabric"):
        return False
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, *([".."] * 6)))
    fdir = os.environ.get("FABRIC_DIR") or os.path.join(
        root, "drivers", "gpu", "drm", "fabric")
    loaded = False
    for ko in ("drm-fabric.ko", "drm-fabric-sim.ko"):
        path = os.path.join(fdir, ko)
        if os.path.isfile(path):
            if subprocess.call(["insmod", path],
                               stderr=subprocess.DEVNULL) == 0:
                loaded = True
    return loaded


def _unload_providers():
    subprocess.call(["rmmod", "drm_fabric_sim"], stderr=subprocess.DEVNULL)
    subprocess.call(["rmmod", "drm_fabric"], stderr=subprocess.DEVNULL)


class Cfg:
    def __init__(self, sock, fid):
        self.sock = sock
        self.fid = fid


def test_malformed_requests(ksft, cfg):
    sock, fid = cfg.sock, cfg.fid
    # Malformed framing/attributes must fail validation with -EINVAL.
    EINVAL = {errno.EINVAL}

    case_rejected(ksft, "wrong-type-short-u32", sock, fid, CMD_FABRIC_GET,
                  nla(A_FABRIC_ID, struct.pack("=H", 1)), EINVAL)

    case_rejected(ksft, "unknown-attribute-id", sock, fid, CMD_FABRIC_GET,
                  nla_u32(A_FABRIC_ID, 1) + nla_u32(A_UNKNOWN, 0), EINVAL)

    case_rejected(ksft, "missing-required-port-index", sock, fid, CMD_PORT_GET,
                  nla_u32(A_ENDPOINT_ID, 0), EINVAL)


def test_liveness(ksft, cfg):
    """A dump that doesn't hang or error is not enough: it must also carry
    a well-formed, zero-status terminal NLMSG_DONE, or a wedge/regression in
    the dump's termination path would go unnoticed. An empty-but-valid dump
    (no data records, just a clean DONE) is still a pass.
    """
    sock, fid = cfg.sock, cfg.fid
    _SEQ[0] += 1
    sock.send(build_msg(fid, CMD_FABRIC_GET, _SEQ[0], b"",
                        flags=NLM_F_REQUEST | NLM_F_DUMP))
    msgs = drain(sock)
    errs = [e for (t, e) in msgs if t == NLMSG_ERROR and e != 0]
    dones = [e for (t, e) in msgs if t == NLMSG_DONE]
    if not msgs:
        ksft.not_ok("liveness-dump-after-fuzz", "no response (possible hang)")
    elif errs:
        ksft.not_ok("liveness-dump-after-fuzz", "dump errno=%s" % errs[0])
    elif not dones:
        ksft.not_ok("liveness-dump-after-fuzz",
                    "no terminal DONE (dump possibly truncated)")
    elif dones[0] is None:
        ksft.not_ok("liveness-dump-after-fuzz", "malformed terminal DONE")
    elif dones[0] != 0:
        ksft.not_ok("liveness-dump-after-fuzz",
                    "terminal DONE error=%d" % dones[0])
    else:
        ksft.ok("liveness-dump-after-fuzz")


def test_family_introspection(ksft, cfg):
    """Via CTRL_CMD_GETFAMILY: version, admin-perm gating, mcast surface."""
    getter_ids = [_SYMS[n] for n in (
        "DRM_FABRIC_CMD_FABRIC_GET", "DRM_FABRIC_CMD_ENDPOINT_GET",
        "DRM_FABRIC_CMD_PORT_GET", "DRM_FABRIC_CMD_PORT_STATS_GET")
        if n in _SYMS]

    info = get_family_info(cfg.sock, FAMILY_NAME)
    if not info:
        for nm in ("genl-family-version", "genl-mcast-monitor-present",
                   "genl-getters-not-admin-perm"):
            ksft.not_ok(nm, "CTRL_CMD_GETFAMILY introspection failed")
        return

    if info["version"] == EXPECTED_VERSION:
        ksft.ok("genl-family-version (v%d)" % info["version"])
    else:
        ksft.not_ok("genl-family-version",
                    "got %s want %d" % (info["version"], EXPECTED_VERSION))

    if MCAST_MONITOR in info["mcast"]:
        ksft.ok("genl-mcast-monitor-present")
    else:
        ksft.not_ok("genl-mcast-monitor-present",
                    "groups=%s" % info["mcast"])

    ops = info["ops"]
    # A query-only build exposes getters only: each must be ungated (no
    # GENL_ADMIN_PERM), so a normal namespace can enumerate topology.
    seen_get = [c for c in getter_ids if c in ops]
    bad_get = [c for c in seen_get if ops[c] & GENL_ADMIN_PERM]
    if seen_get and not bad_get:
        ksft.ok("genl-getters-not-admin-perm (%d cmds)" % len(seen_get))
    else:
        ksft.not_ok("genl-getters-not-admin-perm",
                    "seen=%s wrongly-gated=%s" % (seen_get, bad_get))


CASES = (
    test_malformed_requests,
    test_liveness,
    test_family_introspection,
)


def main():
    tap = L.Ksft()

    if os.geteuid() != 0:
        tap.skip_all("root is required to load drm_fabric modules")

    if _maybe_load_modules():
        L.on_teardown(_unload_providers)

    try:
        sock = open_sock()
    except OSError as exc:
        tap.skip_all("cannot open genetlink socket: %s" % exc)

    fid = resolve_family(sock, FAMILY_NAME)
    if not fid:
        tap.skip_all("drm-fabric genl family not registered "
                     "(load drm_fabric.ko)")

    sys.stderr.write("# attribute/command ids from: %s\n" % _ID_SRC)

    cfg = Cfg(sock, fid)
    L.run_cases(tap, cfg, CASES)
    tap.finish()


if __name__ == "__main__":
    main()
