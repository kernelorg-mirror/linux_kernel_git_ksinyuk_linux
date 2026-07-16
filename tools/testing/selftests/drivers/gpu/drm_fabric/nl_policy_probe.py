#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Adversarial raw-netlink probes the YNL suites cannot reach: malformed
attrs (wrong type, unknown id, truncated nest, out-of-range enum, missing
required) must return a clean NLMSG_ERROR, never an oops; a liveness dump
confirms nothing wedged the family. Also introspects the family and emits
TAP.
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


# Symbols the uAPI defines on every build. Their absence means the header did
# not parse or is not drm_fabric's, which is distinct from a query-only build
# and must not be confused with one.
_REQUIRED_SYMS = ("DRM_FABRIC_CMD_FABRIC_GET", "DRM_FABRIC_CMD_PORT_GET",
                  "DRM_FABRIC_A_FABRIC_ID", "DRM_FABRIC_A_ENDPOINT_ID",
                  "DRM_FABRIC_A_PORT_INDEX", "DRM_FABRIC_A_PEER",
                  "DRM_FABRIC_A_PEER_ATTRS_PEER_ID",
                  "DRM_FABRIC_A_PEER_ATTRS_TYPE", "__DRM_FABRIC_A_MAX")

# Commands that exist only once topology provisioning is present. Whether the
# header defines them describes the build, which is what lets a command missing
# from the live family be reported as a failure instead of a skip.
_MUTATION_CMDS = ("DRM_FABRIC_CMD_FABRIC_NEW", "DRM_FABRIC_CMD_FABRIC_DEL",
                  "DRM_FABRIC_CMD_ENDPOINT_SET", "DRM_FABRIC_CMD_PORT_SET",
                  "DRM_FABRIC_CMD_PORT_PEER_NEW",
                  "DRM_FABRIC_CMD_PORT_PEER_DEL")
_MUTATION_SYMS = _MUTATION_CMDS + ("DRM_FABRIC_A_ADMIN_STATE",
                                  "DRM_FABRIC_A_TYPE",
                                  "DRM_FABRIC_A_INSTANCE_ID")


def _load_ids():
    """Resolve ids from the uAPI header, or report why we cannot; returns
    (ids, header path) or (None, reason). Deliberately no built-in fallback
    table: a stale entry wouldn't fail loudly, it would probe the wrong
    attribute and still report success.
    """
    hdr = _find_uapi_header()
    if not hdr:
        return None, ("drm_fabric uAPI header not found; set "
                      "UAPI_HEADER=/path/to/include/uapi/drm/drm_fabric.h")
    syms = _parse_all_enums(open(hdr).read())
    missing = [s for s in _REQUIRED_SYMS if s not in syms]
    if missing:
        return None, "%s does not define %s" % (hdr, ", ".join(missing))
    return syms, hdr


_IDS, _ID_SRC = _load_ids()


def _id(name):
    """Value of @name, or None when this build's header does not define it."""
    return _IDS.get(name) if _IDS else None


CMD_FABRIC_GET = _id("DRM_FABRIC_CMD_FABRIC_GET")
CMD_PORT_GET = _id("DRM_FABRIC_CMD_PORT_GET")
CMD_PORT_SET = _id("DRM_FABRIC_CMD_PORT_SET")
CMD_PORT_PEER_NEW = _id("DRM_FABRIC_CMD_PORT_PEER_NEW")
CMD_FABRIC_NEW = _id("DRM_FABRIC_CMD_FABRIC_NEW")

A_FABRIC_ID = _id("DRM_FABRIC_A_FABRIC_ID")
A_ENDPOINT_ID = _id("DRM_FABRIC_A_ENDPOINT_ID")
A_PORT_INDEX = _id("DRM_FABRIC_A_PORT_INDEX")
A_ADMIN_STATE = _id("DRM_FABRIC_A_ADMIN_STATE")
A_PEER = _id("DRM_FABRIC_A_PEER")
A_TYPE = _id("DRM_FABRIC_A_TYPE")
A_INSTANCE_ID = _id("DRM_FABRIC_A_INSTANCE_ID")

A_PEER_PEER_ID = _id("DRM_FABRIC_A_PEER_ATTRS_PEER_ID")
A_PEER_TYPE = _id("DRM_FABRIC_A_PEER_ATTRS_TYPE")

# One past the top-level attribute set's upper bound, so every command
# strict-rejects it: no per-command maxattr can exceed the set it indexes.
# __DRM_FABRIC_A_MAX is that value by construction, so this tracks the set as
# it grows instead of quietly aliasing a real attribute once it does.
A_UNKNOWN = _id("__DRM_FABRIC_A_MAX")

# What the build supports, as opposed to what the running family advertises.
BUILD_HAS_MUTATION = bool(_IDS) and all(s in _IDS for s in _MUTATION_SYMS)


# NLA builders

def _align4(n):
    return (n + 3) & ~3


def nla(attr_type, payload):
    length = 4 + len(payload)
    pad = b"\x00" * (_align4(length) - length)
    return struct.pack("=HH", length, attr_type) + payload + pad


def nla_nest(attr_type, payload):
    """Build a nest the way a real client does.

    Strict validation rejects an attribute the policy declares as a nest
    unless NLA_F_NESTED is set, before it ever recurses into the nested
    policy. Without the flag a probe aimed at a nested member only ever
    reaches the outer parse.
    """
    return nla(attr_type | NLA_F_NESTED, payload)


def nla_u32(attr_type, val):
    return nla(attr_type, struct.pack("=I", val & 0xFFFFFFFF))


def nla_u64(attr_type, val):
    return nla(attr_type, struct.pack("=Q", val & 0xFFFFFFFFFFFFFFFF))


def build_msg(family_id, cmd, seq, payload, flags=NLM_F_REQUEST | NLM_F_ACK):
    body = struct.pack("=BBH", cmd, 1, 0) + payload
    total = NLMSG_HDRLEN + len(body)
    nlh = struct.pack("=IHHII", total, family_id, flags, seq, 0)
    return nlh + body


# One counter for every request the suite sends, so each reply can be matched
# to the request that caused it and no two requests ever share a sequence.
_SEQ = [100]


def _next_seq():
    _SEQ[0] += 1
    return _SEQ[0]


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


def _getfamily(sock, name):
    """Send one CTRL_CMD_GETFAMILY and return the datagram that answers it.
    Only a reply matching our own sequence is accepted: an earlier request's
    queued ACK or late reply would otherwise look like a family that
    advertises nothing, silently disabling every introspection check.
    """
    seq = _next_seq()
    sock.send(build_msg(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, seq,
                        nla(CTRL_ATTR_FAMILY_NAME, name + b"\x00"),
                        flags=NLM_F_REQUEST))
    while True:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            return None
        (_, mtype, _, mseq, _) = struct.unpack_from("=IHHII", data, 0)
        if mseq != seq:
            continue
        if mtype == NLMSG_ERROR:
            return None
        return data


def resolve_family(sock, name):
    data = _getfamily(sock, name)
    if data is None:
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
    data = _getfamily(sock, name)
    if data is None:
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

def case_rejected(tap, name, sock, fid, cmd, payload, expect):
    """Pass iff the kernel rejected with one of @expect (positive errno
    values; the netlink error is negative, so we compare -e). The specific
    code matters: e.g. -EINVAL for a malformed attribute, not a generic
    failure.
    """
    sock.send(build_msg(fid, cmd, _next_seq(), payload))
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
        # Whether the running family advertises the mutation commands, from
        # live introspection in main(): True, False, or None when the
        # introspection itself failed. The three states are kept apart because
        # "this build has no mutation commands" is a skip while "this build has
        # them but the family does not offer them" is a failure.
        self.live_mutation = None


def test_malformed_requests(ksft, cfg):
    sock, fid = cfg.sock, cfg.fid
    # Malformed framing/attributes must fail validation with -EINVAL.
    EINVAL = {errno.EINVAL}
    # Out-of-range enums are caught by the generated NLA_POLICY range checks,
    # which report -ERANGE and nothing else. Accepting -EINVAL as well would
    # let a malformed probe that never reaches the range check pass silently.
    ERANGE = {errno.ERANGE}

    case_rejected(ksft, "wrong-type-short-u32", sock, fid, CMD_FABRIC_GET,
                  nla(A_FABRIC_ID, struct.pack("=H", 1)), EINVAL)

    case_rejected(ksft, "unknown-attribute-id", sock, fid, CMD_FABRIC_GET,
                  nla_u32(A_FABRIC_ID, 1) + nla_u32(A_UNKNOWN, 0), EINVAL)

    # Policy errors are unreachable when mutation commands are absent.
    if cfg.live_mutation:
        # Truncated nest: PEER header claims 64 bytes but carries 4. Rejected
        # while walking the attributes, before any policy runs.
        bad_nest = (struct.pack("=HH", 64, A_PEER | NLA_F_NESTED) +
                    b"\x00\x00\x00\x00")
        case_rejected(ksft, "truncated-nest", sock, fid, CMD_PORT_PEER_NEW,
                      nla_u32(A_ENDPOINT_ID, 0) + nla_u32(A_PORT_INDEX, 0) + bad_nest,
                      EINVAL)

        # Out-of-range enum: admin-state past DRM_FABRIC_ADMIN_UP.
        case_rejected(ksft, "enum-range-admin-state", sock, fid, CMD_PORT_SET,
                      nla_u32(A_ENDPOINT_ID, 0) + nla_u32(A_PORT_INDEX, 0) +
                      nla_u32(A_ADMIN_STATE, 0xFFFFFFFF), ERANGE)

        # Out-of-range enum: peer-type past DRM_FABRIC_PEER_SWITCH, inside a
        # nest, so this only reaches the nested policy as a well-formed nest.
        peer = nla_u64(A_PEER_PEER_ID, 0x1) + nla_u32(A_PEER_TYPE, 99)
        case_rejected(ksft, "enum-range-peer-type", sock, fid, CMD_PORT_PEER_NEW,
                      nla_u32(A_ENDPOINT_ID, 0) + nla_u32(A_PORT_INDEX, 0) +
                      nla_nest(A_PEER, peer), ERANGE)

        # Zero fabric-type, which the enum starts above and so never names.
        # The range check runs before the doit, so the refusal predates any
        # fabric the request could have created, which the next case asserts.
        before = fabric_count(sock, fid)
        case_rejected(ksft, "enum-range-fabric-type", sock, fid, CMD_FABRIC_NEW,
                      nla_u32(A_TYPE, 0) + nla_u64(A_INSTANCE_ID, 0xA5),
                      ERANGE)
        after = fabric_count(sock, fid)
        ksft.check(before is not None and after == before,
                   "enum-range-fabric-type-not-created",
                   "fabrics before=%s after=%s" % (before, after))
    else:
        # The case set stays the same either way -- the probes are reported
        # rather than silently omitted -- but only a query-only build earns a
        # skip. If this build defines the mutation commands and the family does
        # not offer them, the probes are unrunnable for a reason worth seeing.
        if cfg.live_mutation is None:
            report, why = ksft.not_ok, ("family introspection failed; cannot "
                                        "tell which commands are advertised")
        elif BUILD_HAS_MUTATION:
            report, why = ksft.not_ok, ("uAPI header defines the mutation "
                                        "commands but the family advertises "
                                        "none")
        else:
            report, why = ksft.skip, ("query-only build: uAPI header defines "
                                      "no mutation commands")
        for nm in ("truncated-nest", "enum-range-admin-state",
                   "enum-range-peer-type", "enum-range-fabric-type",
                   "enum-range-fabric-type-not-created"):
            report(nm, why)

    case_rejected(ksft, "missing-required-port-index", sock, fid, CMD_PORT_GET,
                  nla_u32(A_ENDPOINT_ID, 0), EINVAL)


def fabric_count(sock, fid):
    """Fabrics a dump reports, or None when the dump itself did not succeed.

    None is distinct from zero on purpose: a dump that errored says nothing
    about how many fabrics exist, and reporting it as zero would let a broken
    dump satisfy a claim that nothing was created.
    """
    sock.send(build_msg(fid, CMD_FABRIC_GET, _next_seq(), b"",
                        flags=NLM_F_REQUEST | NLM_F_DUMP))
    msgs = drain(sock)
    if not msgs or any(t == NLMSG_ERROR and e != 0 for (t, e) in msgs):
        return None
    return sum(1 for (t, _) in msgs if t not in (NLMSG_ERROR, NLMSG_DONE))


def test_liveness(ksft, cfg):
    """A dump that doesn't hang or error is not enough: it must also carry
    a well-formed, zero-status terminal NLMSG_DONE, or a wedge/regression in
    the dump's termination path would go unnoticed. An empty-but-valid dump
    (no data records, just a clean DONE) is still a pass.
    """
    sock, fid = cfg.sock, cfg.fid
    sock.send(build_msg(fid, CMD_FABRIC_GET, _next_seq(), b"",
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
    mutator_ids = [_id(n) for n in _MUTATION_CMDS if _id(n) is not None]
    getter_ids = [_id(n) for n in (
        "DRM_FABRIC_CMD_FABRIC_GET", "DRM_FABRIC_CMD_ENDPOINT_GET",
        "DRM_FABRIC_CMD_PORT_GET", "DRM_FABRIC_CMD_PORT_STATS_GET")
        if _id(n) is not None]

    info = get_family_info(cfg.sock, FAMILY_NAME)
    if not info:
        for nm in ("genl-family-version", "genl-mcast-monitor-present",
                   "genl-mutators-admin-perm", "genl-getters-not-admin-perm"):
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
    # The mutator admin-perm gate only applies once the mutation commands exist
    # at all; a query-only build registers no mutators to check. Gate on the
    # build rather than on the live family, so a build that should advertise
    # mutators but does not fails here instead of dropping the check.
    if BUILD_HAS_MUTATION:
        seen_mut = [c for c in mutator_ids if c in ops]
        bad_mut = [c for c in seen_mut if not (ops[c] & GENL_ADMIN_PERM)]
        if seen_mut and not bad_mut:
            ksft.ok("genl-mutators-admin-perm (%d cmds)" % len(seen_mut))
        else:
            ksft.not_ok("genl-mutators-admin-perm",
                        "seen=%s missing-perm=%s" % (seen_mut, bad_mut))

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

    # Every probe below is built from uAPI ids, so without them there is
    # nothing trustworthy to send.
    if _IDS is None:
        tap.skip_all(_ID_SRC)

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

    # Ask the live family which of the topology-mutation commands it actually
    # offers. Left as None when the introspection fails, so the probes gated on
    # it report that rather than treating an unanswered question as a no.
    cfg = Cfg(sock, fid)
    info = get_family_info(sock, FAMILY_NAME)
    if info is not None:
        cfg.live_mutation = any(_id(n) in info["ops"] for n in _MUTATION_CMDS
                                if _id(n) is not None)

    L.run_cases(tap, cfg, CASES)
    tap.finish()


if __name__ == "__main__":
    main()
