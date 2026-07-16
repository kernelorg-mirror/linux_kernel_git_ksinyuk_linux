# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
"""
Shared helpers for the drm-fabric selftests (cf.
tools/testing/selftests/net/lib/py): YNL family binding to drm_fabric.yaml,
path/module discovery, the DrmFabric wrapper, and a KTAP emitter (Ksft)
over kselftest/ksft.py.
"""

import atexit
import contextlib
import os
import signal
import subprocess
import sys
import time

# Path discovery.
# This file lives at tools/testing/selftests/drivers/gpu/drm_fabric/, six
# directory levels below the kernel tree root, which ROOT resolves to.
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", "..", "..", ".."))

SPEC = os.environ.get("SPEC") or os.path.join(
    ROOT, "Documentation", "netlink", "specs", "drm_fabric.yaml")
UAPI_HEADER = os.environ.get("UAPI_HEADER") or os.path.join(
    ROOT, "include", "uapi", "drm", "drm_fabric.h")

# Modules: the QEMU harness sets FABRIC_DIR=/modules; otherwise build output
# lives next to the source.
FABRIC_DIR = os.environ.get("FABRIC_DIR") or os.path.join(
    ROOT, "drivers", "gpu", "drm", "fabric")

DEBUGFS = "/sys/kernel/debug/drm_fabric_sim"

FAMILY = "drm-fabric"
MCAST_MONITOR = "monitor"


def _ynl_dir():
    env = os.environ.get("YNL_DIR")
    if env:
        return env
    return os.path.join(ROOT, "tools", "net", "ynl")


def import_ynl():
    """Import YnlFamily/NlError from the in-tree YNL library, or SKIP."""
    ynl_dir = _ynl_dir()
    if ynl_dir not in sys.path:
        sys.path.insert(0, ynl_dir)
    try:
        from pyynl.lib import YnlFamily, NlError
    except ModuleNotFoundError as exc:
        print("1..0 # SKIP cannot import YNL library from %s (%s)"
              % (ynl_dir, exc))
        sys.exit(4)
    return YnlFamily, NlError


def _ksft_dir_candidates():
    """Where the kernel's generic kselftest/ksft.py may live."""
    yield os.path.normpath(os.path.join(HERE, "..", "..", "..", "kselftest"))
    yield os.path.join(ROOT, "tools", "testing", "selftests", "kselftest")
    ynl_root = os.path.normpath(os.path.join(_ynl_dir(), "..", "..", ".."))
    yield os.path.join(ynl_root, "tools", "testing", "selftests", "kselftest")


_ksft_mod = None


def import_ksft():
    """Import the kernel's kselftest/ksft.py, or None if unavailable (inline
    TAP fallback below)."""
    global _ksft_mod
    if _ksft_mod is not None:
        return _ksft_mod or None
    for cand in _ksft_dir_candidates():
        if os.path.isfile(os.path.join(cand, "ksft.py")):
            if cand not in sys.path:
                sys.path.insert(0, cand)
            import ksft as _k
            _ksft_mod = _k
            return _k
    _ksft_mod = False   # cache "looked, not found"
    return None


# KTAP emitter

class Ksft:
    """KTAP emitter delegating to the kernel's kselftest/ksft.py.

    Thin ergonomic adapter (check/ok/skip/finish) over the in-tree primitives
    (``test_result_*``/``set_plan``/``finished``) so the suites do not reinvent
    TAP. Falls back to inline printing only when ksft.py is not importable.
    """

    def __init__(self):
        self._k = import_ksft()
        self.cnt = 0
        self.fail = 0
        self._started = False
        # Timing. Suite wall starts at construction so it also covers the
        # module load / family setup main() does before the first result.
        # Per-case and per-step deltas accrue as results are emitted; the
        # digest is printed by finish() as KTAP "# time:" diagnostics.
        self._t0 = time.monotonic()
        self._last = None                # time of the previous result
        self._cur = None                 # [name, t_start, cnt_at_start]
        self._cases = []                 # [(name, dur_s, steps), ...]
        self._slow_step = ("", 0.0)      # (name, longest inter-result gap)

    def _tick(self, name):
        """Record the wall gap since the previous result (a "step")."""
        now = time.monotonic()
        if self._last is None:
            self._last = now             # first result: no prior to measure
            return
        delta = now - self._last
        self._last = now
        if delta > self._slow_step[1]:
            self._slow_step = (name, delta)

    def case_begin(self, name):
        self._cur = [name, time.monotonic(), self.cnt]

    def case_end(self):
        if self._cur is None:
            return
        name, t_start, cnt0 = self._cur
        self._cases.append((name, time.monotonic() - t_start, self.cnt - cnt0))
        self._cur = None

    def start(self):
        if self._started:
            return
        self._started = True
        if self._k:
            self._k.print_header()
        else:
            print("TAP version 13")

    def skip_all(self, reason):
        # A 0-plan skip is the whole result (standard KTAP, both backends),
        # which is only valid before any other result has been printed. A
        # mid-case call is a bug in the caller, not a runtime condition to
        # report as KTAP -- surface it loudly instead of emitting a plan
        # that contradicts results already on stdout.
        if self.cnt != 0:
            raise RuntimeError(
                "skip_all() called after %d result(s) already emitted "
                "(reason=%r); mid-case failures must use not_ok() (e.g. "
                "via reset_sim_or_fail()), not skip_all()" % (self.cnt, reason))
        print("1..0 # SKIP %s" % reason)
        sys.exit(4)

    def ok(self, name):
        self.start()
        self._tick(name)
        self.cnt += 1
        if self._k:
            self._k.test_result_pass(name)
        else:
            print("ok %d %s" % (self.cnt, name))

    def not_ok(self, name, detail=""):
        self.start()
        self._tick(name)
        self.cnt += 1
        self.fail += 1
        if self._k:
            if detail:
                self._k.print_msg(detail)
            self._k.test_result_fail(name)
        else:
            print("not ok %d %s" % (self.cnt, name))
            if detail:
                print("  # %s" % detail)

    def skip(self, name, reason=""):
        self.start()
        self._tick(name)
        self.cnt += 1
        if self._k:
            if reason:
                self._k.print_msg("%s: %s" % (name, reason))
            self._k.test_result_skip(name)
        else:
            print("ok %d %s # SKIP %s" % (self.cnt, name, reason))

    def check(self, cond, name, detail=""):
        if cond:
            self.ok(name)
        else:
            self.not_ok(name, detail)
        return bool(cond)

    def _emit_timing(self):
        """Print per-case and per-suite wall-clock as "# time:" KTAP diagnostics
        (ignored by TAP parsers). Per-case lines are gated behind
        FABRIC_TIMING; the one-line suite summary is always emitted.
        """
        wall = time.monotonic() - self._t0
        suite = os.path.basename(sys.argv[0]) or "suite"
        if os.environ.get("FABRIC_TIMING"):
            for name, dur, steps in self._cases:
                print("# time: case=%s wall=%.3fs steps=%d" % (name, dur, steps))
        slow = max(self._cases, default=("-", 0.0, 0), key=lambda c: c[1])
        print("# time: suite=%s wall=%.3fs cases=%d steps=%d "
              "slowest-case=%s(%.3fs) slowest-step=%s(%.3fs)"
              % (suite, wall, len(self._cases), self.cnt,
                 slow[0], slow[1], self._slow_step[0], self._slow_step[1]))

    def finish(self):
        self.start()
        self._emit_timing()
        if self._k:
            self._k.set_plan(self.cnt)
            self._k.finished()       # prints totals + exits 0/1 by pass+skip
        else:
            print("1..%d" % self.cnt)
            print("")
            print("# %d/%d passed, %d failed"
                  % (self.cnt - self.fail, self.cnt, self.fail))
            sys.exit(1 if self.fail else 0)


# Case dispatch

def run_cases(ksft, cfg, cases):
    """Dispatch each case, isolating an exception to its own result."""
    for fn in cases:
        ksft.case_begin(fn.__name__)
        try:
            fn(ksft, cfg)
        except Exception as exc:  # noqa: BLE001 - isolate one case's failure
            ksft.not_ok(fn.__name__, "unhandled exception: %r" % exc)
        finally:
            ksft.case_end()
        if getattr(cfg, "abort", False):
            break


# YNL wrapper

def DrmFabric(**kwargs):
    """Construct a YnlFamily bound to the drm_fabric spec (schema off)."""
    YnlFamily, _ = import_ynl()
    if not os.path.isfile(SPEC):
        Ksft().skip_all("drm_fabric.yaml not found at %s" % SPEC)
    # schema='' skips slow jsonschema validation, matching the net selftests.
    return YnlFamily(SPEC, schema="", **kwargs)


def nl_errno(exc):
    """Positive errno carried by a netlink exception."""
    return getattr(exc, "error", 0)


def family_has_op(fab, name):
    """True if the loaded family advertises operation @name: a query-only
    build has none of the topology-mutation ops (fabric-new, fabric-del,
    endpoint-set, port-set, port-peer-new/del), so cases exercising them are
    filtered rather than raising KeyError.
    """
    return name in getattr(fab, "ops", {})


def select_cases(fab, cases, mutation_cases, probe="fabric-new"):
    """Return @cases, dropping @mutation_cases when @probe (a representative
    mutation op) is absent from the family.
    """
    if family_has_op(fab, probe):
        return tuple(cases)
    drop = set(mutation_cases)
    return tuple(c for c in cases if c not in drop)


# System helpers (kselftest runs as root)

def is_root():
    return os.geteuid() == 0


def module_loaded(name):
    return os.path.isdir("/sys/module/%s" % name)


def insmod(ko, *args):
    path = ko if os.path.isabs(ko) else os.path.join(FABRIC_DIR, ko)
    return subprocess.call(["insmod", path, *args],
                           stderr=subprocess.DEVNULL) == 0


def rmmod(name):
    subprocess.call(["rmmod", name], stderr=subprocess.DEVNULL)


def debugfs_available():
    return os.path.isdir(DEBUGFS)


def dbg_write(rel, val):
    with open(os.path.join(DEBUGFS, rel), "w") as fh:
        fh.write(str(val))


def settle(seconds=0.2):
    time.sleep(seconds)


def wait_until(predicate, timeout=3.0, interval=0.02):
    """Poll @predicate until truthy or @timeout elapses; return the last value."""
    deadline = time.monotonic() + timeout
    val = predicate()
    while not val and time.monotonic() < deadline:
        time.sleep(interval)
        val = predicate()
    return val


def wait_ntf(ev, want_name, timeout=3.0, match=None):
    """First notification named @want_name within @timeout, else None."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        for ntf in ev.poll_ntf(duration=min(remaining, 0.25)):
            if ntf["name"] != want_name:
                continue
            if match is None or match(ntf):
                return ntf


# Teardown that survives the timeout killer

_teardowns = []
_teardown_armed = False


def _run_teardowns():
    """Run registered teardowns once, most-recent first, swallowing errors."""
    while _teardowns:
        fn = _teardowns.pop()
        try:
            fn()
        except Exception:  # noqa: BLE001 - teardown must not mask the exit
            pass


def _sig_teardown(signum, _frame):
    _run_teardowns()
    # Restore the default disposition and re-raise so the exit status still
    # reflects the signal (the kselftest runner treats it as a failure/timeout).
    signal.signal(signum, signal.SIG_DFL)
    os.kill(os.getpid(), signum)


def on_teardown(fn):
    """Register @fn for normal exit and SIGTERM/SIGINT.

    atexit alone misses the timeout runner's SIGTERM. SIGKILL cannot be
    caught, so a killed predecessor is recovered at the next test's entry.
    """
    global _teardown_armed
    if not _teardown_armed:
        atexit.register(_run_teardowns)
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                signal.signal(sig, _sig_teardown)
            except (ValueError, OSError):
                pass   # not on the main thread; atexit still covers clean exit
        _teardown_armed = True
    _teardowns.append(fn)


# Suite fixture

def _providers_unload():
    rmmod("drm_fabric_sim")
    rmmod("drm_fabric")


def sim_restore_default():
    # Drop the shape the suite loaded and put the default topology back.
    rmmod("drm_fabric_sim")
    if module_loaded("drm_fabric"):
        insmod("drm-fabric-sim.ko")
        wait_until(lambda: module_loaded("drm_fabric_sim"))


def _insmod_fabricsim(topology=None, sim_args=None):
    args = list(sim_args or ())
    if topology is not None:
        args.insert(0, "topology=%s" % topology)
    return insmod("drm-fabric-sim.ko", *args)


def _reset_sim_steps(topology, sim_args=None):
    """Drop whatever a previous test left loaded, then load @topology.

    A test killed with SIGKILL runs no Python cleanup, so isolation is
    re-established here, at the next test's entry. Returns (ok, reason) so
    each caller can pick its own KTAP path.
    """
    if not is_root():
        return False, "must run as root (insmod)"
    rmmod("drm_fabric_sim")
    if not module_loaded("drm_fabric") and not insmod("drm-fabric.ko"):
        return False, "could not load drm_fabric"
    if not _insmod_fabricsim(topology, sim_args):
        return False, "could not load drm_fabric_sim topology=%s args=%s" % (
            topology, list(sim_args or ()))
    if not wait_until(lambda: module_loaded("drm_fabric_sim")):
        return False, "drm_fabric_sim did not appear after reset"
    return True, ""


def reset_sim(ksft, topology="mesh", sim_args=None):
    """Load @topology, or skip the suite.

    Setup-time only: skip_all() is invalid once a result has been emitted.
    """
    ok, reason = _reset_sim_steps(topology, sim_args)
    if not ok:
        ksft.skip_all(reason)


def reset_sim_or_fail(ksft, name, topology="mesh", sim_args=None):
    """Load @topology, or fail the current case as @name.

    On False the caller must bail out; the not_ok() already stands.
    """
    ok, reason = _reset_sim_steps(topology, sim_args)
    if not ok:
        ksft.not_ok(name, reason)
        return False
    return True


@contextlib.contextmanager
def fabricsim(ksft, topology=None, need_debugfs=False, need_control=None,
              open_family=True, sim_args=None):
    """Bring the providers up, yield a bound family, arrange teardown.

    A missing precondition skips the suite. @topology reloads the sim even
    under --no-load and restores the default on exit. @open_family=False
    yields None for suites opening their own socket.
    """
    _, NlError = import_ynl()

    if not is_root():
        ksft.skip_all("must run as root (genetlink + debugfs + insmod)")

    if topology is not None:
        # Entry reset: dropping any sim left by a previous (possibly
        # SIGKILL-terminated) test before loading this shape is what makes a
        # topology-changing suite start from a known state. See reset_sim().
        rmmod("drm_fabric_sim")
        loaded_core = False
        if not module_loaded("drm_fabric"):
            if not insmod("drm-fabric.ko"):
                ksft.skip_all("could not load drm_fabric")
            loaded_core = True
        # Arm teardown before loading the sim so a failed sim load (or any
        # later skip) still restores the default shape and unwinds a core we
        # loaded here, rather than leaking it into the next suite.
        on_teardown(_providers_unload if loaded_core else sim_restore_default)
        if not _insmod_fabricsim(topology, sim_args):
            ksft.skip_all("could not load drm_fabric_sim topology=%s args=%s"
                          % (topology, list(sim_args or ())))
        wait_until(lambda: module_loaded("drm_fabric_sim"))
    elif "--no-load" not in sys.argv[1:] and not module_loaded("drm_fabric"):
        rmmod("drm_fabric_sim")
        rmmod("drm_fabric")
        # Arm teardown before loading so a partial load (core up, sim load
        # failed) is unwound instead of leaking a module into the next suite.
        on_teardown(_providers_unload)
        if not insmod("drm-fabric.ko") or not _insmod_fabricsim(None, sim_args):
            ksft.skip_all("could not load drm_fabric + drm_fabric_sim modules")
        wait_until(lambda: module_loaded("drm_fabric_sim"))
    else:
        # Running against providers somebody else loaded (--no-load, or a
        # previous suite that restored the sim but kept the core). There is no
        # module to unwind, but the suite can still add endpoints and peers,
        # and without a teardown that state would leak into the next suite and
        # survive the timeout killer's SIGTERM. Restore the default shape.
        on_teardown(sim_restore_default)

    if not module_loaded("drm_fabric_sim"):
        ksft.skip_all("drm_fabric_sim not loaded")
    if need_debugfs and not debugfs_available():
        ksft.skip_all("fabricsim debugfs not available (runtime controls)")
    if need_control and not os.path.exists(os.path.join(DEBUGFS, need_control)):
        ksft.skip_all("fabricsim lacks '%s' control (old module)" % need_control)

    if not open_family:
        yield None
        return

    try:
        fab = DrmFabric()
    except (OSError, NlError) as exc:
        ksft.skip_all("cannot open drm-fabric family: %s" % exc)
    yield fab
