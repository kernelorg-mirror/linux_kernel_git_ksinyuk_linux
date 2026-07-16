#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""Exercise nested port-dump resume across endpoint removal.

A resumed dump must not apply the removed endpoint's saved port cursor
to its successor. Verify that each endpoint observed after removal starts
at port index 0 for both PORT_GET and PORT_STATS_GET.
"""

import os
import re
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L  # noqa: E402 - shared KTAP/module helpers

# --- Netlink / generic-netlink constants (cf. dump_intr_abi.py) -----------

NETLINK_GENERIC = 16
NLMSG_ERROR = 0x2
NLMSG_DONE = 0x3
NLM_F_REQUEST = 0x01
NLM_F_DUMP = 0x300
NLMSG_HDRLEN = 16
GENL_HDRLEN = 4
NLA_HDRLEN = 4
NLA_TYPE_MASK = 0x3FFF
CTRL_ID = 0x10
CTRL_CMD_GETFAMILY = 3
CTRL_ATTR_FAMILY_NAME = 2
CTRL_ATTR_FAMILY_ID = 1

SEQ = 2                 # request sequence; replies in the dump must echo it

# Multi-port endpoints so a batch boundary can land *inside* an endpoint (the
# only case that exercises a non-zero saved port cursor). 15 is near the sim's
# 16-port cap and rarely divides the per-batch port capacity evenly.
PORTS = int(os.environ.get("PORT_CURSOR_PORTS", "15"))
SCALE = int(os.environ.get("PORT_CURSOR_SCALE", "160"))


def _align4(n):
    return (n + 3) & ~3


def _nla(atype, payload):
    length = NLA_HDRLEN + len(payload)
    pad = b"\x00" * (_align4(length) - length)
    return struct.pack("=HH", length, atype) + payload + pad


def _msg(family_id, cmd, seq, flags, payload=b""):
    body = struct.pack("=BBH", cmd, 1, 0) + payload
    total = NLMSG_HDRLEN + len(body)
    return struct.pack("=IHHII", total, family_id, flags, seq, 0) + body


def _open():
    s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, NETLINK_GENERIC)
    s.bind((0, 0))
    s.settimeout(5)
    return s


def _resolve_family(sock, name):
    sock.send(_msg(CTRL_ID, CTRL_CMD_GETFAMILY, 1, NLM_F_REQUEST,
                   _nla(CTRL_ATTR_FAMILY_NAME, name + b"\x00")))
    data = sock.recv(8192)
    (_, mtype, _, _, _) = struct.unpack_from("=IHHII", data, 0)
    if mtype == NLMSG_ERROR:
        return None
    off = NLMSG_HDRLEN + GENL_HDRLEN
    while off + NLA_HDRLEN <= len(data):
        (alen, atype) = struct.unpack_from("=HH", data, off)
        if alen < NLA_HDRLEN:
            break
        if atype == CTRL_ATTR_FAMILY_ID and alen >= 6:
            return struct.unpack_from("=H", data, off + 4)[0]
        off += _align4(alen)
    return None


def _enum(name):
    """Parse `enum <name> { ... }` from the uAPI header into {member: value}.
    A value referencing another enumerator (the generated `MAX = (__MAX - 1)`
    sentinel) fails int() and is skipped rather than silently mis-numbering.
    """
    out = {}
    try:
        text = open(L.UAPI_HEADER).read()
    except OSError:
        return out
    m = re.search(r"enum\s+%s\s*\{(.*?)\}" % re.escape(name), text, re.S)
    if not m:
        return out
    n = 0
    for raw in re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S).split(","):
        item = raw.strip()
        if not item:
            continue
        if "=" in item:
            key, val = item.split("=", 1)
            key = key.strip()
            try:
                n = int(val.strip(), 0)
            except ValueError:
                continue        # references another enumerator; skip sentinel
        else:
            key = item
        out[key] = n
        n += 1
    return out


_HDR_PRESENT = os.path.isfile(L.UAPI_HEADER)
_CMD = _enum("drm_fabric_cmd")
_A = _enum("drm_fabric_a")
_PA = _enum("drm_fabric_a_port_attrs")
_PSA = _enum("drm_fabric_a_port_stats_attrs")

# (wanted name, parsed dict, fallback id) -- fallbacks match the current ABI and
# are only trusted when the header is absent (see main()).
_WANTED = (
    ("DRM_FABRIC_CMD_PORT_GET", _CMD, 3),
    ("DRM_FABRIC_CMD_PORT_STATS_GET", _CMD, 4),
    ("DRM_FABRIC_A_PORT", _A, 3),
    ("DRM_FABRIC_A_PORT_STATS", _A, 4),
    ("DRM_FABRIC_A_PORT_ATTRS_PORT_INDEX", _PA, 1),
    ("DRM_FABRIC_A_PORT_ATTRS_ENDPOINT_ID", _PA, 2),
    ("DRM_FABRIC_A_PORT_STATS_ATTRS_ENDPOINT_ID", _PSA, 2),
    ("DRM_FABRIC_A_PORT_STATS_ATTRS_PORT_INDEX", _PSA, 3),
)
CMD_PORT_GET = _CMD.get("DRM_FABRIC_CMD_PORT_GET", 3)
CMD_PORT_STATS_GET = _CMD.get("DRM_FABRIC_CMD_PORT_STATS_GET", 4)
A_PORT = _A.get("DRM_FABRIC_A_PORT", 3)
A_PORT_STATS = _A.get("DRM_FABRIC_A_PORT_STATS", 4)
PA_PORT_INDEX = _PA.get("DRM_FABRIC_A_PORT_ATTRS_PORT_INDEX", 1)
PA_ENDPOINT_ID = _PA.get("DRM_FABRIC_A_PORT_ATTRS_ENDPOINT_ID", 2)
PSA_ENDPOINT_ID = _PSA.get("DRM_FABRIC_A_PORT_STATS_ATTRS_ENDPOINT_ID", 2)
PSA_PORT_INDEX = _PSA.get("DRM_FABRIC_A_PORT_STATS_ATTRS_PORT_INDEX", 3)


def _walk(buf, base, end):
    """Yield (masked_type, payload) for well-formed nlattrs in buf[base:end]."""
    off = base
    while off + NLA_HDRLEN <= end:
        (alen, atype) = struct.unpack_from("=HH", buf, off)
        if alen < NLA_HDRLEN or off + alen > end:
            break                       # malformed: stop rather than over-read
        yield atype & NLA_TYPE_MASK, buf[off + NLA_HDRLEN:off + alen]
        off += _align4(alen)


def _parse_entry(payload, outer, ep_attr, idx_attr):
    """Extract (endpoint_id, port_index) from one dump reply payload. @outer
    is the top-level nest (A_PORT/A_PORT_STATS); @ep_attr/@idx_attr are the
    member ids inside it.
    """
    ep_id = port_idx = None
    for atype, data in _walk(payload, 0, len(payload)):
        if atype != outer:
            continue
        for btype, bdata in _walk(data, 0, len(data)):
            if btype == ep_attr and len(bdata) >= 4:
                ep_id = struct.unpack_from("=I", bdata, 0)[0]
            elif btype == idx_attr and len(bdata) >= 4:
                port_idx = struct.unpack_from("=I", bdata, 0)[0]
    return ep_id, port_idx


def _read_batch(sock, fam, seq, outer, ep_attr, idx_attr):
    """Read one dump datagram with strict structural validation.

    Returns (pairs, last_ep, done, err). Any malformed length, unexpected
    family/sequence, or unparseable reply sets err (reported, not silently
    dropped).
    """
    pairs, last_ep, done, err = [], None, False, False
    try:
        data = sock.recv(65536)
    except socket.timeout:
        return pairs, last_ep, True, True   # a stall mid-dump is a failure here
    off, end = 0, len(data)
    while off + NLMSG_HDRLEN <= end:
        (mlen, mtype, _, mseq, _) = struct.unpack_from("=IHHII", data, off)
        if mlen < NLMSG_HDRLEN or off + mlen > end:
            err = True
            break
        if mtype == NLMSG_DONE:
            done = True
        elif mtype == NLMSG_ERROR:
            err = True
        elif mtype == fam and mseq == seq:
            body = off + NLMSG_HDRLEN + GENL_HDRLEN
            ep_id, port_idx = _parse_entry(memoryview(data)[body:off + mlen],
                                           outer, ep_attr, idx_attr)
            if ep_id is not None and port_idx is not None:
                pairs.append((ep_id, port_idx))
                last_ep = ep_id
            else:
                err = True
        else:
            err = True
        off += _align4(mlen)
    return pairs, last_ep, done, err


class Cfg:
    def __init__(self, fam, fab):
        self.fam = fam
        self.fab = fab                  # YnlFamily handle (drm-fabric)
        self.ep_slot = {}               # endpoint-id -> fabricsim slot
        self.ep_ports = {}              # endpoint-id -> its own port count
        self.abort = False


def _ensure_population(cfg):
    """Top up to SCALE endpoints and rebuild the endpoint-id -> slot map.

    add_endpoint reuses freed slots, so the map is rebuilt each time.
    """
    have = len(cfg.fab.dump("endpoint-get", {}))
    for _ in range(SCALE - have):
        try:
            L.dbg_write("add_endpoint", PORTS)
        except OSError:
            break
    ep_slot = {}
    for e in cfg.fab.dump("endpoint-get", {}):
        ep = e["endpoint"]
        m = re.match(r"sim-ep(\d+)$", ep.get("name", ""))
        if m:
            ep_slot[ep["endpoint-id"]] = int(m.group(1))
    cfg.ep_slot = ep_slot

    # Per-endpoint port count, taken from each endpoint's own topology rather
    # than the global PORTS: the baseline population and runtime-added endpoints
    # can differ in width, and the "provably mid-dump" oracle below must compare
    # against the specific endpoint being suspended, not a module-wide setting.
    ep_ports = {}
    for p in cfg.fab.dump("port-get", {}):
        port = p["port"]
        ep_ports[port["endpoint-id"]] = ep_ports.get(port["endpoint-id"], 0) + 1
    cfg.ep_ports = ep_ports


def _reload():
    L.rmmod("drm_fabric_sim")
    if not L.module_loaded("drm_fabric"):
        if not L.insmod("drm-fabric.ko"):
            return False
    # A tiny baseline; the multi-port population is added below.
    if not L.insmod("drm-fabric-sim.ko", "num_endpoints=2", "ports_per_ep=2",
                    "topology=linear"):
        return False
    L.wait_until(lambda: L.module_loaded("drm_fabric_sim"))
    return True


def _restore_default():
    """Restore fabricsim's default shape so the next suite (sharing the loaded
    module) does not inherit this suite's small/churned population."""
    L.rmmod("drm_fabric_sim")
    if not L.module_loaded("drm_fabric"):
        L.insmod("drm-fabric.ko")
    L.insmod("drm-fabric-sim.ko")
    L.wait_until(lambda: L.module_loaded("drm_fabric_sim"))


def _dump_removal_keeps_leading_ports(ksft, cfg, cmd, outer, ep_attr, idx_attr,
                                      tag):
    """Assert every endpoint still starts at port 0 after a mid-endpoint
    removal. The stats variant also runs the provider callback off the
    topology lock during resume.
    """
    _ensure_population(cfg)
    s = _open()
    s.send(_msg(cfg.fam, cmd, SEQ, NLM_F_REQUEST | NLM_F_DUMP))

    seen = {}           # ep_id -> set(port_index) seen so far
    deleted = set()
    batches = exercised = 0
    done = err = False
    while not done:
        pairs, last_ep, done, e = _read_batch(s, cfg.fam, SEQ,
                                              outer, ep_attr, idx_attr)
        err = err or e
        if pairs:
            batches += 1
            for ep_id, port_idx in pairs:
                seen.setdefault(ep_id, set()).add(port_idx)
        # Delete only when provably mid-dump: the cumulative port set is a
        # strict, non-empty subset of that endpoint's own width, i.e. the
        # saved cursor is (last_ep, port_idx>0).
        ep_width = cfg.ep_ports.get(last_ep, PORTS)
        if (last_ep is not None and last_ep not in deleted
                and last_ep in cfg.ep_slot
                and 0 < len(seen.get(last_ep, ())) < ep_width):
            try:
                L.dbg_write("del_endpoint", cfg.ep_slot[last_ep])
                deleted.add(last_ep)
                exercised += 1
            except OSError:
                pass
        if batches > 10000:
            break
    s.close()

    if not ksft.check(batches >= 2 and not err,
                      "%s-dump-spans-multiple-batches" % tag,
                      "batches=%d err=%s (raise PORT_CURSOR_SCALE)"
                      % (batches, err)):
        cfg.abort = True
        return

    if not ksft.check(exercised >= 1,
                      "%s-dump-exercised-mid-endpoint-removal" % tag,
                      "no batch suspended mid-endpoint "
                      "(raise PORT_CURSOR_PORTS/PORT_CURSOR_SCALE)"):
        cfg.abort = True
        return

    # The invariant: no endpoint may appear missing its leading ports.
    bad = {ep: sorted(ports)[:3] for ep, ports in seen.items()
           if 0 not in ports}
    ksft.check(not bad, "%s-no-leading-ports-dropped" % tag,
               "endpoints missing port 0: %s"
               % ", ".join("ep%d=%s" % (e, p) for e, p in bad.items()))


def test_port_get_dump_removal_keeps_leading_ports(ksft, cfg):
    """Nested PORT_GET cursor survives mid-dump endpoint removal."""
    _dump_removal_keeps_leading_ports(ksft, cfg, CMD_PORT_GET, A_PORT,
                                      PA_ENDPOINT_ID, PA_PORT_INDEX, "port-get")


def test_port_stats_dump_removal_keeps_leading_ports(ksft, cfg):
    """Nested PORT_STATS_GET cursor + unlocked stats callback survive
    mid-dump endpoint removal."""
    _dump_removal_keeps_leading_ports(ksft, cfg, CMD_PORT_STATS_GET,
                                      A_PORT_STATS, PSA_ENDPOINT_ID,
                                      PSA_PORT_INDEX, "port-stats-get")


CASES = (
    test_port_get_dump_removal_keeps_leading_ports,
    test_port_stats_dump_removal_keeps_leading_ports,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()  # early SKIP if the YNL lib is missing

    if not L.is_root():
        ksft.skip_all("must run as root (genetlink + debugfs)")

    # Trust parsed ids only when they are complete; require every wanted enum
    # name when the header is present so a partial parse cannot mis-number.
    missing = [nm for nm, d, _ in _WANTED if nm not in d]
    if _HDR_PRESENT and missing:
        ksft.skip_all("uAPI header present but missing enum(s): %s"
                      % ", ".join(missing))
    if not _HDR_PRESENT:
        print("# port_cursor_abi: uAPI header absent, using id fallbacks")

    if not _reload():
        ksft.skip_all("could not load drm_fabric_sim")
    # Primary cleanup is try/finally below; on_teardown() is the backup for hard
    # exits, including the timeout killer's SIGTERM (which atexit would miss),
    # so this suite's churned population never leaks into the next one.
    L.on_teardown(_restore_default)

    if not L.debugfs_available():
        ksft.skip_all("fabricsim debugfs not present")

    try:
        fab = L.DrmFabric()
    except (OSError, NlError) as exc:
        ksft.skip_all("cannot open drm-fabric family: %s" % exc)

    s = _open()
    fam = _resolve_family(s, L.FAMILY.encode())
    s.close()
    if not fam:
        ksft.skip_all("could not resolve %s family id" % L.FAMILY)

    # Each case tops the multi-port population up to SCALE (so a dump spans many
    # batches) and rebuilds the endpoint-id -> slot map before it runs.
    try:
        L.run_cases(ksft, Cfg(fam, fab), CASES)
    finally:
        _restore_default()
    ksft.finish()


if __name__ == "__main__":
    main()
