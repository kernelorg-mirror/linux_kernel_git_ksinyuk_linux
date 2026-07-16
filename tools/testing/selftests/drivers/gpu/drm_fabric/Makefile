# SPDX-License-Identifier: GPL-2.0
#
# kselftests for the drm-fabric Generic Netlink ABI.
#
# Each TEST_PROGS entry is invoked separately by the kselftest harness; suites
# self-load modules when needed (check-spec-regen.sh is host-only).
#
# Run:
#   make -C tools/testing/selftests TARGETS=drivers/gpu/drm_fabric run_tests

TEST_PROGS := \
	check-spec-regen.sh \
	fabric_abi.py \
	nl_policy_probe.py \
	dump_intr_abi.py \
	port_cursor_abi.py \
	port_stats_cap_abi.py \
	hotplug_abi.py \
	dump_scale_abi.py \
	switch_abi.py \
	fault_abi.py \
	harness_reset_abi.py

TEST_FILES := lib_drm_fabric.py

include ../../../lib.mk

# A TEST_PROGS entry that loses its executable bit still runs, because the
# kselftest runner falls back to the shebang, but only after a warning that is
# easy to miss in a long run. Catch it at build time instead.
all: check-test-progs-mode

check-test-progs-mode:
	@for prog in $(TEST_PROGS); do \
		test -x "$$prog" || { \
			echo "$$prog: in TEST_PROGS but not executable" >&2; \
			exit 1; \
		}; \
	done

.PHONY: check-test-progs-mode
