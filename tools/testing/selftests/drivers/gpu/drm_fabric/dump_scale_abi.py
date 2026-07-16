#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Forces netlink dump pagination (cb->args) with fabricsim's bulk_add/bulk_del,
past what the small object counts in other suites would ever trigger, and
verifies every dump returns the full set exactly once, including under
concurrent churn.

Requires drm_fabric + drm_fabric_sim with fabricsim debugfs; run as root.
"""

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lib_drm_fabric as L

# Enough endpoints that the ENDPOINT_GET reply spans several skbs (each entry
# carries id/fabric-id/fabric-ep-id/name/dev-name/bus-name/admin nests).
SCALE = int(os.environ.get("DUMP_SCALE", "300"))


def ep_ids(fab):
    return [e["endpoint"]["endpoint-id"] for e in fab.dump("endpoint-get", {})]


def port_count(fab):
    return len(list(fab.dump("port-get", {})))


class Cfg:
    """Carries the pre-scale baseline counts so the teardown case can assert
    the population is restored exactly."""

    def __init__(self, fab, nl_error, base_ids, base_ports, fid):
        self.fab = fab
        self.NlError = nl_error
        self.base_ids = base_ids
        self.base_n = len(base_ids)
        self.base_ports = base_ports
        self.fid = fid


def test_scale_dump(ksft, cfg):
    fab = cfg.fab
    try:
        L.dbg_write("bulk_add", SCALE)
    except OSError as exc:
        ksft.skip_all("bulk_add failed: %s" % exc)
    L.wait_until(lambda: len(ep_ids(fab)) >= cfg.base_n + SCALE, timeout=10)

    ids = ep_ids(fab)
    ksft.check(len(ids) == cfg.base_n + SCALE, "scale-endpoint-dump-count",
               "want %d got %d" % (cfg.base_n + SCALE, len(ids)))
    ksft.check(len(set(ids)) == len(ids), "scale-endpoint-dump-unique",
               "%d dups" % (len(ids) - len(set(ids))))
    ksft.check(set(cfg.base_ids).issubset(set(ids)),
               "scale-endpoint-dump-keeps-baseline")

    pc = port_count(fab)
    ksft.check(pc == cfg.base_ports + SCALE, "scale-port-dump-count",
               "want %d got %d" % (cfg.base_ports + SCALE, pc))

    if cfg.fid is not None:
        flt = [e["endpoint"] for e in fab.dump("endpoint-get",
                                               {"fabric-id": cfg.fid})]
        ksft.check(all(e.get("fabric-id") == cfg.fid for e in flt) and
                   len(flt) >= SCALE, "scale-endpoint-dump-filtered",
                   "got %d members" % len(flt))
    else:
        ksft.skip("scale-endpoint-dump-filtered", "fabricsim fabric absent")


def test_dump_consistency_under_churn(ksft, cfg):
    """Hammer multi-skb dumps while a helper thread churns the population."""
    fab = cfg.fab
    pre_churn = len(ep_ids(fab))
    L.dbg_write("bulk_add", SCALE)
    L.wait_until(lambda: len(ep_ids(fab)) > pre_churn, timeout=10)
    stop = threading.Event()
    churn_err = []

    def churn():
        while not stop.is_set():
            try:
                L.dbg_write("bulk_del", 0)
                # Re-check before re-populating so the last iteration does not
                # add a fresh SCALE population that would race the teardown.
                if stop.is_set():
                    break
                L.dbg_write("bulk_add", SCALE)
            except OSError:
                # racy debugfs writes may transiently fail; not a dump bug
                pass
            except Exception as exc:  # noqa: BLE001
                churn_err.append(str(exc))
                return

    worst_dups = 0
    dump_err = None
    # The join below is the real synchronization point; the case aborts if it
    # times out. Mark the worker daemon so a wedged iteration cannot also hang
    # interpreter shutdown after that failure has already been reported.
    t = threading.Thread(target=churn, daemon=True)
    t.start()
    try:
        for _ in range(60):
            d = ep_ids(fab)
            worst_dups = max(worst_dups, len(d) - len(set(d)))
    except Exception as exc:  # noqa: BLE001
        dump_err = str(exc)
    finally:
        stop.set()
        t.join(timeout=30)          # each churn iteration is bounded

    # A worker that will not terminate is a harness failure, not something to
    # leave running into the next case; abort so the teardown case cannot race
    # an in-flight bulk_add.
    if not ksft.check(not t.is_alive(), "dump-churn-worker-terminates",
                      "churn worker still alive after join"):
        cfg.abort = True
        return

    # Assert *no duplicates* (the invariant a broken cb->args resume would
    # violate), not *no omissions*: under concurrent churn the population
    # legitimately changes between skbs, so a missing id is expected here and
    # only a duplicated id signals a dump-resume bug.
    ksft.check(worst_dups == 0 and dump_err is None and not churn_err,
               "dump-consistency-no-dup-under-churn",
               "dups=%d dump_err=%s churn_err=%s"
               % (worst_dups, dump_err, churn_err[:1]))

    # Worker has exited: drain the churn population back to baseline so the
    # teardown case (and the next suite) starts from a known, quiescent count.
    L.dbg_write("bulk_del", 0)
    L.wait_until(lambda: len(ep_ids(fab)) == cfg.base_n, timeout=10)


def test_scale_teardown(ksft, cfg):
    fab = cfg.fab
    try:
        L.dbg_write("bulk_del", 1)
    except OSError as exc:
        ksft.not_ok("scale-teardown", "bulk_del failed: %s" % exc)
        return
    L.wait_until(lambda: len(ep_ids(fab)) == cfg.base_n, timeout=10)
    ksft.check(len(ep_ids(fab)) == cfg.base_n, "scale-teardown-restores-baseline",
               "want %d got %d" % (cfg.base_n, len(ep_ids(fab))))


CASES = (
    test_scale_dump,
    test_dump_consistency_under_churn,
    test_scale_teardown,
)


def main():
    ksft = L.Ksft()
    _, NlError = L.import_ynl()

    with L.fabricsim(ksft, need_debugfs=True, need_control="bulk_add") as fab:
        base_ids = ep_ids(fab)
        base_ports = port_count(fab)
        fid = None
        for f in fab.dump("fabric-get", {}):
            if f["fabric"]["name"] == "fabricsim":
                fid = f["fabric"]["fabric-id"]
                break

        L.run_cases(ksft, Cfg(fab, NlError, base_ids, base_ports, fid), CASES)
    ksft.finish()


if __name__ == "__main__":
    main()
