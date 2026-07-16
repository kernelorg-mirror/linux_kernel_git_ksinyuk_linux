#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Heterogeneous per-port stats: a dump skips a port whose provider returns
-EOPNOTSUPP and resumes; any other errno ends the dump. pyynl reassembles
multipart dumps transparently, so this talks raw Generic Netlink.

Needs drm_fabric + drm_fabric_sim (>= 3 ports on ep0), fabricsim debugfs
(per-port stats_errno, bulk_add/bulk_del), and root.
"""

import glob
import os
import re
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L  # noqa: E402 - shared KTAP/module helpers

EOPNOTSUPP = 95
EIO = 5
MID = 1  # not the first/last port, so skipping it proves resumption

# Runtime endpoints added for the skip case. ep0 is enumerated first, so its
# skipped port is necessarily behind a batch boundary from everything added
# here. Overridable where record size or NLMSG_GOODSIZE change the batch
# arithmetic enough to leave the dump single-batch.
RESUME_SCALE = int(os.environ.get("STATS_RESUME_SCALE", "300"))

# --- raw Generic Netlink (cf. nl_policy_probe.py, dump_intr_abi.py) -------

FAMILY_NAME = b"drm-fabric"

NETLINK_GENERIC = 16
NLMSG_ERROR = 0x2
NLMSG_DONE = 0x3
NLM_F_REQUEST = 0x01
NLM_F_DUMP = 0x300
NLMSG_HDRLEN = 16
GENL_HDRLEN = 4
CTRL_ID = 0x10
CTRL_CMD_GETFAMILY = 3
CTRL_ATTR_FAMILY_ID = 1
CTRL_ATTR_FAMILY_NAME = 2
NLA_TYPE_MASK = 0x3FFF  # strips NLA_F_NESTED / NLA_F_NET_BYTEORDER

# Small reads keep dump batches small, making resume easier to exercise.
BATCH_READ = 8192


def _align4(n):
    return (n + 3) & ~3


def _nla(attr_type, payload):
    length = 4 + len(payload)
    pad = b"\x00" * (_align4(length) - length)
    return struct.pack("=HH", length, attr_type) + payload + pad


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
    for atype, payload in _iter_attrs(data[NLMSG_HDRLEN + GENL_HDRLEN:]):
        if atype != CTRL_ATTR_FAMILY_ID:
            continue
        # CTRL_ATTR_FAMILY_ID is a u16; tolerate a u32 encoding too.
        if len(payload) >= 4:
            return struct.unpack_from("=I", payload)[0]
        if len(payload) >= 2:
            return struct.unpack_from("=H", payload)[0]
    return None


def _iter_attrs(blob):
    off = 0
    while off + 4 <= len(blob):
        (alen, atype) = struct.unpack_from("=HH", blob, off)
        if alen < 4:
            break
        yield atype & NLA_TYPE_MASK, blob[off + 4:off + alen]
        off += _align4(alen)


def _uapi_ids():
    """Stats-dump ids from the generated uAPI header.

    No fallback table: an unreadable header skips the suite rather than
    decoding under a stale guess.
    """
    want = ("DRM_FABRIC_CMD_PORT_STATS_GET",
            "DRM_FABRIC_A_PORT_STATS",
            "DRM_FABRIC_A_PORT_STATS_ATTRS_ENDPOINT_ID",
            "DRM_FABRIC_A_PORT_STATS_ATTRS_PORT_INDEX")
    try:
        text = open(L.UAPI_HEADER).read()
    except OSError:
        return None
    # Commands are an anonymous enum and attributes are named ones, so merge
    # every block flatly rather than parsing by enum name.
    syms = {}
    for body in re.findall(r"enum\s*(?:\w+\s*)?\{(.*?)\}", text, re.S):
        nxt = 0
        for raw in body.split(","):
            item = raw.split("/*")[0].strip()
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
            if name.isidentifier():
                syms[name] = nxt
            nxt += 1
    if not all(w in syms for w in want):
        return None
    return {"cmd": syms[want[0]], "nest": syms[want[1]],
            "ep": syms[want[2]], "port": syms[want[3]]}


def _record(body, ids):
    """(endpoint-id, port-index) carried by one stats entry, or None."""
    for atype, payload in _iter_attrs(body):
        if atype != ids["nest"]:
            continue
        ep = idx = None
        for natype, npayload in _iter_attrs(payload):
            if natype == ids["ep"] and len(npayload) >= 4:
                ep = struct.unpack_from("=I", npayload)[0]
            elif natype == ids["port"] and len(npayload) >= 4:
                idx = struct.unpack_from("=I", npayload)[0]
        if ep is not None and idx is not None:
            return (ep, idx)
    return None


class Dump:
    """Records in wire order, plus how the kernel delivered them."""

    def __init__(self, batches, records, done, error, timed_out):
        self.batches = batches
        self.records = records
        self.done = done
        self.error = error
        self.timed_out = timed_out

    @property
    def ok(self):
        return self.done and not self.error and not self.timed_out

    def __str__(self):
        return ("batches=%d records=%d done=%s error=%s timeout=%s"
                % (self.batches, len(self.records), self.done, self.error,
                   self.timed_out))


def _dump_stats(ids):
    """Run an unfiltered PORT_STATS_GET dump batch by batch."""
    sock = _open()
    try:
        sock.send(_msg(ids["fam"], ids["cmd"], 2, NLM_F_REQUEST | NLM_F_DUMP))
        batches, recs, done, err = 0, [], False, False
        while not done:
            try:
                data = sock.recv(BATCH_READ)
            except socket.timeout:
                return Dump(batches, recs, done, err, True)
            if not data:
                break
            batches += 1
            off = 0
            while off + NLMSG_HDRLEN <= len(data):
                (mlen, mtype, _, _, _) = struct.unpack_from("=IHHII", data, off)
                if mlen < NLMSG_HDRLEN:
                    break
                if mtype == NLMSG_DONE:
                    done = True
                elif mtype == NLMSG_ERROR:
                    err = True
                else:
                    rec = _record(data[off + NLMSG_HDRLEN + GENL_HDRLEN:
                                       off + mlen], ids)
                    if rec:
                        recs.append(rec)
                off += _align4(mlen)
            if batches > 10000:  # runaway guard
                break
        return Dump(batches, recs, done, err, False)
    finally:
        sock.close()


def _grow(ids):
    """Grow until the dump spans several batches.

    bulk_add is asynchronous, so read back until two dumps agree.
    """
    L.dbg_write("bulk_add", RESUME_SCALE)
    last, deadline = -1, time.monotonic() + 10.0
    while time.monotonic() < deadline:
        now = len(_dump_stats(ids).records)
        if now == last:
            return now
        last = now
        time.sleep(0.05)
    return last


def _ep0(fab):
    """endpoint-id of the first init endpoint (debugfs dir ep0, name sim-ep0)."""
    for e in fab.dump("endpoint-get", {}):
        ep = e["endpoint"]
        if ep.get("name") == "sim-ep0":
            return ep["endpoint-id"]
    return None


def _port_count(fab, ep_id):
    return len(fab.dump("port-get", {"endpoint-id": ep_id}))


def _knob(port, val):
    # fabricsim's per-port debugfs control: forces @port's next stats
    # callback to return -@val instead of real data.
    L.dbg_write("ep0/port%d/stats_errno" % port, val)


class Cfg:
    def __init__(self, fab, ep_id, nports, nl_error, ids):
        self.fab = fab
        self.ep_id = ep_id
        self.nports = nports
        self.NlError = nl_error
        self.ids = ids


def test_dump_skips_unsupported_port(ksft, cfg):
    """A mid-list -EOPNOTSUPP port is skipped across a real batch boundary.

    Expectation is the unknobbed dump minus exactly that port, so a lost
    neighbour, a re-emit after resume, or a duplicate all fail.
    """
    name = "stats-dump-skips-unsupported"
    ids = cfg.ids
    skipped = (cfg.ep_id, MID)
    try:
        _grow(ids)
        base = _dump_stats(ids)
        if not base.ok or not base.records:
            ksft.not_ok(name, "unknobbed dump unusable: %s" % base)
            return
        _knob(MID, EOPNOTSUPP)
        got = _dump_stats(ids)
        if not got.ok:
            ksft.not_ok(name, "dump did not complete: %s" % got)
            return
        lost = sorted(set(base.records) - set(got.records))
        extra = sorted(set(got.records) - set(base.records))
        dupes = len(got.records) != len(set(got.records))
        ksft.check(got.batches >= 2 and lost == [skipped] and not extra
                   and not dupes,
                   name,
                   "%s lost=%s extra=%s dupes=%s (want lost=[%s], >=2 "
                   "batches -- raise STATS_RESUME_SCALE if single-batch)"
                   % (got, lost, extra, dupes, skipped))
    finally:
        _knob(MID, 0)
        L.dbg_write("bulk_del", 0)


def test_targeted_unsupported_port_eopnotsupp(ksft, cfg):
    """A targeted request for the unsupported port still returns -EOPNOTSUPP."""
    fab, NlError = cfg.fab, cfg.NlError
    _knob(MID, EOPNOTSUPP)
    try:
        fab.do("port-stats-get", {"endpoint-id": cfg.ep_id, "port-index": MID})
        ksft.not_ok("stats-targeted-unsupported-eopnotsupp", "request accepted")
    except NlError as exc:
        e = L.nl_errno(exc)
        ksft.check(e == EOPNOTSUPP, "stats-targeted-unsupported-eopnotsupp",
                   "errno=%d (want EOPNOTSUPP=%d)" % (e, EOPNOTSUPP))
    finally:
        _knob(MID, 0)


def test_dump_aborts_on_real_error(ksft, cfg):
    """A non-capability provider error (EIO) ends the dump."""
    fab, NlError = cfg.fab, cfg.NlError
    _knob(MID, EIO)
    try:
        try:
            fab.dump("port-stats-get", {"endpoint-id": cfg.ep_id})
            ksft.not_ok("stats-dump-aborts-on-real-error",
                        "dump completed instead of aborting")
        except NlError as exc:
            e = L.nl_errno(exc)
            ksft.check(e == EIO, "stats-dump-aborts-on-real-error",
                       "errno=%d (want EIO=%d)" % (e, EIO))
    finally:
        _knob(MID, 0)


CASES = (
    test_dump_skips_unsupported_port,
    test_targeted_unsupported_port_eopnotsupp,
    test_dump_aborts_on_real_error,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, need_debugfs=True) as fab:
        if not L.family_has_op(fab, "port-stats-get"):
            ksft.skip_all("port-stats-get op absent")
        ep_id = _ep0(fab)
        if ep_id is None:
            ksft.skip_all("sim-ep0 endpoint not present")
        # The per-port stats_errno knob only exists on a recent simulator.
        if not glob.glob(os.path.join(L.DEBUGFS, "ep0", "port*", "stats_errno")):
            ksft.skip_all("fabricsim lacks per-port stats_errno knob (old module)")
        # The skip case needs a population big enough to span dump batches.
        if not os.path.exists(os.path.join(L.DEBUGFS, "bulk_add")):
            ksft.skip_all("fabricsim lacks bulk_add (cannot reach a resume "
                          "boundary)")
        nports = _port_count(fab, ep_id)
        if nports < 3:
            ksft.skip_all("need >= 3 ports on ep0 to place a mid-list skip "
                          "(have %d)" % nports)
        ids = _uapi_ids()
        if ids is None:
            ksft.skip_all("could not resolve stats uAPI ids from %s"
                          % L.UAPI_HEADER)
        sock = _open()
        try:
            fam = _resolve_family(sock, FAMILY_NAME)
        finally:
            sock.close()
        if fam is None:
            ksft.skip_all("drm-fabric generic netlink family not resolvable")
        ids["fam"] = fam
        L.run_cases(ksft, Cfg(fab, ep_id, nports, NlError, ids), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
