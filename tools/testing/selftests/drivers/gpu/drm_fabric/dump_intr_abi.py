#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Tests NLM_F_DUMP_INTR when a topology change lands mid-dump or after the
last entry. pyynl decodes attributes but never surfaces nlmsg_flags, so
this talks raw Generic Netlink.

Needs drm_fabric + drm_fabric_sim, fabricsim debugfs (bulk_add), and root.
"""

import glob
import os
import re
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L  # noqa: E402 - shared KTAP/module helpers (no pyynl)

# --- Netlink / generic-netlink constants (cf. nl_policy_probe.py) ---------

NETLINK_GENERIC = 16
NLMSG_ERROR = 0x2
NLMSG_DONE = 0x3
NLM_F_REQUEST = 0x01
NLM_F_MULTI = 0x02
NLM_F_DUMP = 0x300
NLM_F_DUMP_INTR = 0x10
NLMSG_HDRLEN = 16
GENL_HDRLEN = 4
CTRL_ID = 0x10
CTRL_CMD_GETFAMILY = 3
CTRL_ATTR_FAMILY_NAME = 2
CTRL_ATTR_FAMILY_ID = 1

# Population large enough that ENDPOINT_GET spans several dump skbs (so there is
# a between-batch window to mutate). Overridable for slow/fast machines.
SCALE = int(os.environ.get("DUMP_INTR_SCALE", "600"))


def _align4(n):
    return (n + 3) & ~3


def _nla(atype, payload):
    length = 4 + len(payload)
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
    while off + 4 <= len(data):
        (alen, atype) = struct.unpack_from("=HH", data, off)
        if alen < 4:
            break
        if atype == CTRL_ATTR_FAMILY_ID:
            # CTRL_ATTR_FAMILY_ID is a u16; tolerate a u32 encoding too.
            if alen >= 8:
                return struct.unpack_from("=I", data, off + 4)[0]
            if alen >= 6:
                return struct.unpack_from("=H", data, off + 4)[0]
        off += _align4(alen)
    return None


def _cmd_id(wanted, fallback):
    """Resolve @wanted from the generated uAPI drm_fabric_cmd enum."""
    try:
        text = open(L.UAPI_HEADER).read()
        m = re.search(r"enum\s+drm_fabric_cmd\s*\{(.*?)\}", text, re.S)
        if m:
            n = 0
            for raw in re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S).split(","):
                item = raw.strip()
                if not item:
                    continue
                if "=" in item:
                    name, val = item.split("=", 1)
                    name, n = name.strip(), int(val.strip(), 0)
                else:
                    name = item
                if name == wanted:
                    return n
                n += 1
    except (OSError, ValueError):
        pass
    return fallback


def _read_msgs(sock):
    """Read one dump datagram as (msgs, timed_out): (nlmsg_type, nlmsg_flags)
    pairs. A timeout is reported explicitly, never conflated with a real
    NLMSG_DONE.
    """
    try:
        data = sock.recv(65536)
    except socket.timeout:
        return [], True
    msgs, off = [], 0
    while off + NLMSG_HDRLEN <= len(data):
        (mlen, mtype, mflags, _, _) = struct.unpack_from("=IHHII", data, off)
        if mlen < NLMSG_HDRLEN:
            break
        msgs.append((mtype, mflags))
        off += _align4(mlen)
    return msgs, False


def _read_batch(sock):
    """Read one dump datagram as (list_of_flags, saw_done, saw_error, timed_out)."""
    msgs, timed_out = _read_msgs(sock)
    if timed_out:
        return [], False, False, True
    return ([f for (_, f) in msgs],
            any(t == NLMSG_DONE for (t, _) in msgs),
            any(t == NLMSG_ERROR for (t, _) in msgs),
            False)


def _dump(sock, fam, cmd, mutate_after_first=None):
    """Run an ENDPOINT_GET dump batch-by-batch; returns (batches, intr_seen,
    err_seen, saw_done, timed_out). @mutate_after_first, if given, runs
    once between the first and second batch.
    """
    sock.send(_msg(fam, cmd, 2, NLM_F_REQUEST | NLM_F_DUMP))
    batches, intr, err, done, timed_out, mutated = 0, False, False, False, False, False
    while not done:
        flags, done, e, to = _read_batch(sock)
        if to:
            timed_out = True
            break
        if not flags:
            break
        batches += 1
        err = err or e
        if any(f & NLM_F_DUMP_INTR for f in flags):
            intr = True
        if mutate_after_first and not mutated:
            mutate_after_first()
            mutated = True
        if batches > 10000:            # runaway guard
            break
    return batches, intr, err, done, timed_out


class Cfg:
    def __init__(self, fam, cmd, stats_cmd):
        self.fam = fam
        self.cmd = cmd
        self.stats_cmd = stats_cmd


def test_multi_skb_dump_available(ksft, cfg):
    """Precondition: the population makes ENDPOINT_GET span >1 dump batch."""
    s = _open()
    batches, _, err, done, to = _dump(s, cfg.fam, cfg.cmd)
    s.close()
    ok = ksft.check(batches >= 2 and not err and done and not to,
                    "dump-spans-multiple-batches",
                    "batches=%d err=%s done=%s timeout=%s (raise DUMP_INTR_SCALE)"
                    % (batches, err, done, to))
    if not ok:
        cfg.abort = True   # the INTR cases below are meaningless single-batch


def test_no_intr_when_quiescent(ksft, cfg):
    """A quiescent dump must complete with a real NLMSG_DONE (not a socket
    timeout) and no NLM_F_DUMP_INTR; a stalled dump is a failure, not a
    silent pass.
    """
    s = _open()
    batches, intr, err, done, to = _dump(s, cfg.fam, cfg.cmd)
    s.close()
    ksft.check(done and not to and not intr and not err, "quiescent-dump-no-intr",
               "done=%s timeout=%s intr=%s err=%s batches=%d"
               % (done, to, intr, err, batches))


def _find_oper_state():
    """A fabricsim per-port oper_state debugfs knob, if any (baseline ports)."""
    m = glob.glob(os.path.join(L.DEBUGFS, "*", "*", "oper_state"))
    return m[0] if m else None


def test_intr_on_mutation_mid_dump(ksft, cfg):
    """A base_seq bump during a multi-skb dump must raise NLM_F_DUMP_INTR.

    oper_state writes are synchronous, unlike bulk_add, so the generation
    changes before netlink's one-skb-ahead prefill snapshots it.
    """
    oper = _find_oper_state()
    if not oper:
        ksft.skip("mutation-mid-dump-sets-intr", "no fabricsim oper_state knob")
        return
    rel = os.path.relpath(oper, L.DEBUGFS)
    states = ("active", "degraded")

    s = _open()
    s.send(_msg(cfg.fam, cfg.cmd, 3, NLM_F_REQUEST | NLM_F_DUMP))
    intr = err = timed_out = False
    batches = i = 0
    done = False
    while not done:
        flags, done, e, to = _read_batch(s)
        if to:
            timed_out = True
            break
        if not flags:
            break
        batches += 1
        err = err or e
        if any(f & NLM_F_DUMP_INTR for f in flags):
            intr = True
        # Synchronous generation bump between batches (state must change to
        # take effect, so alternate the two values).
        try:
            L.dbg_write(rel, states[i % 2])
            i += 1
        except OSError:
            pass
        if batches > 10000:
            break
    s.close()

    # The dump must both observe the interruption and still terminate cleanly
    # (a real NLMSG_DONE, not a stall).
    ksft.check(intr and done and not timed_out and not err,
               "mutation-mid-dump-sets-intr",
               "intr=%s done=%s timeout=%s err=%s batches=%d"
               % (intr, done, timed_out, err, batches))


def test_intr_on_post_exhaustion_mutation(ksft, cfg):
    """A topology change after the final entry must still be reported on
    NLMSG_DONE.

    A dump handler that samples the generation only once it has a record in
    hand leaves a hole: a batch that finds the cursor already exhausted emits
    nothing, so it never samples, and NLMSG_DONE goes out carrying the
    generation from the previous batch.
    """
    s = _open()
    s.send(_msg(cfg.fam, cfg.stats_cmd, 4, NLM_F_REQUEST | NLM_F_DUMP))

    name = "post-exhaustion-mutation-sets-intr"
    try:
        msgs, timed_out = _read_msgs(s)
        entries = sum(1 for (t, _) in msgs if t == cfg.fam)
        if timed_out or not entries or any(t == NLMSG_DONE for (t, _) in msgs):
            ksft.skip(name, "PORT_STATS_GET did not park mid-dump "
                            "(entries=%d timeout=%s)" % (entries, timed_out))
            return

        # Retire everything ahead of the cursor while the dump is parked. The
        # dump only advances when we read, so settling here cannot let it run
        # past the mutation.
        try:
            L.dbg_write("bulk_del", 0)
        except OSError as exc:
            ksft.skip(name, "bulk_del failed: %s" % exc)
            return
        L.settle(0.3)

        # Netlink prefills one skb ahead, so the next read still delivers
        # entries serialized before the mutation. Those carry the old
        # generation and leave the pending inconsistency untouched.
        done_flags, entry_intr, err = None, False, False
        for _ in range(10000):
            msgs, timed_out = _read_msgs(s)
            if timed_out or not msgs:
                break
            for (mtype, mflags) in msgs:
                if mtype == cfg.fam:
                    entry_intr = entry_intr or bool(mflags & NLM_F_DUMP_INTR)
                elif mtype == NLMSG_ERROR:
                    err = True
                elif mtype == NLMSG_DONE:
                    done_flags = mflags
            if done_flags is not None:
                break

        # An entry that already carried the flag means the interruption was
        # reported mid-dump and the consistency check reset with it, so
        # NLMSG_DONE need not repeat it. That is the mid-dump path, covered by
        # the case above, and it cannot stand in for this one.
        if entry_intr:
            ksft.skip(name, "interruption reported on an entry; the "
                            "exhaustion path was not isolated")
            return

        ksft.check(done_flags is not None and
                   bool(done_flags & NLM_F_DUMP_INTR) and not err, name,
                   "done_flags=%s err=%s"
                   % ("none" if done_flags is None else hex(done_flags), err))
    finally:
        s.close()
        # Restore the population for whatever runs next.
        try:
            L.dbg_write("bulk_add", SCALE)
        except OSError:
            pass
        L.settle(0.3)


CASES = (
    test_multi_skb_dump_available,
    test_no_intr_when_quiescent,
    test_intr_on_mutation_mid_dump,
    test_intr_on_post_exhaustion_mutation,
)


def main():
    ksft = L.Ksft()

    with L.fabricsim(ksft, need_debugfs=True, need_control="bulk_add",
                     open_family=False):
        sock = _open()
        fam = _resolve_family(sock, L.FAMILY.encode())
        sock.close()
        if not fam:
            ksft.skip_all("cannot resolve drm-fabric genl family")

        # Grow the population so the dump pages across several skbs.
        try:
            L.dbg_write("bulk_add", SCALE)
        except OSError as exc:
            ksft.skip_all("bulk_add failed: %s" % exc)
        L.settle(0.3)

        cfg = Cfg(fam,
                  _cmd_id("DRM_FABRIC_CMD_ENDPOINT_GET", 2),
                  _cmd_id("DRM_FABRIC_CMD_PORT_STATS_GET", 4))
        L.run_cases(ksft, cfg, CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
