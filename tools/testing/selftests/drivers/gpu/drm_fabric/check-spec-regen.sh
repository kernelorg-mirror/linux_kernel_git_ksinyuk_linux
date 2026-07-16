#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 Intel Corporation
#
# Check that every generated drm_fabric netlink artifact matches its spec.
#
# Documentation/netlink/specs/drm_fabric.yaml is authoritative for three
# generated files that must not be hand-edited: the uAPI header and the
# kernel-side policy and operation tables. Stale kernel tables still build and
# pass every test here, because those tests exercise whatever family the tables
# describe. Reconciliation is directory-scoped, so an artifact generated outside
# those three directories is not visible. Host only; skips when the spec, a
# generated file or the ynl generator (python3 + PyYAML) is missing.

DIR="$(dirname "$(readlink -f "$0")")"

. "${DIR}"/../../../kselftest/ktap_helpers.sh

# Six levels up from this directory is the kernel tree root.
KDIR="${KDIR:-$(readlink -f "${DIR}/../../../../../..")}"

SPEC="Documentation/netlink/specs/drm_fabric.yaml"
GEN="tools/net/ynl/pyynl/ynl_gen_c.py"
[ -f "${KDIR}/${GEN}" ] || GEN="tools/net/ynl/ynl_gen_c.py"

FAB="drivers/gpu/drm/fabric"
UAPI_H="include/uapi/drm/drm_fabric.h"
NL_C="${FAB}/drm_fabric_nl.c"
NL_H="${FAB}/drm_fabric_nl.h"

ktap_print_header

if [ ! -f "${KDIR}/${SPEC}" ] || [ ! -f "${KDIR}/${GEN}" ] || \
   [ ! -f "${KDIR}/${UAPI_H}" ] || [ ! -f "${KDIR}/${NL_C}" ] || \
   [ ! -f "${KDIR}/${NL_H}" ]; then
	ktap_skip_all "drm_fabric spec, a generated file or the ynl generator is missing (set KDIR)"
	exit "${KSFT_SKIP}"
fi

if ! command -v python3 >/dev/null 2>&1 || ! python3 -c 'import yaml' 2>/dev/null; then
	ktap_skip_all "python3 with PyYAML is required"
	exit "${KSFT_SKIP}"
fi

# An unwritable tmpdir is an environment limit, not a mismatch: skip.
if ! tmp=$(mktemp -d 2>/dev/null); then
	ktap_skip_all "no writable temporary directory"
	exit "${KSFT_SKIP}"
fi
trap 'rm -rf "${tmp}"' EXIT

ktap_set_plan 4

# A generated file carries both a YNL-GEN banner and this spec's path.
for f in "${UAPI_H}" "${NL_C}" "${NL_H}"; do
	printf '%s\n' "${f}"
done | sort >"${tmp}/declared"

sed 's|/[^/]*$||' "${tmp}/declared" | sort -u >"${tmp}/dirs"

: >"${tmp}/found"
while read -r d; do
	for f in "${KDIR}/${d}"/*.c "${KDIR}/${d}"/*.h; do
		[ -f "${f}" ] || continue
		grep -q '^/\* YNL-GEN ' "${f}" || continue
		grep -qF -- "${SPEC}" "${f}" || continue
		printf '%s\n' "${f#"${KDIR}/"}"
	done
done <"${tmp}/dirs" | sort >"${tmp}/found"

if diff -u "${tmp}/declared" "${tmp}/found" >"${tmp}/diff"; then
	ktap_test_pass "generated artifacts in the tree are the ones checked here"
else
	sed 's/^/# /' "${tmp}/diff"
	ktap_print_msg "a file generated from ${SPEC} is not on this check's list"
	ktap_test_fail "generated artifacts in the tree are the ones checked here"
fi

# Run from KDIR with a relative spec path so banner and guard match.
check_generated()
{
	committed="$1"
	mode="$2"
	kind="$3"
	name="$4"
	# Includes derive from the basename; give each its own directory.
	out_dir="${tmp}/${mode}-${kind}"
	out="${out_dir}/$(basename "${committed}")"
	mkdir -p "${out_dir}"

	if ! ( cd "${KDIR}" && python3 "${GEN}" --mode "${mode}" --"${kind}" \
	       --spec "${SPEC}" -o "${out}" ) 2>"${tmp}/err"; then
		sed 's/^/# /' "${tmp}/err"
		ktap_test_fail "${name}"
		return
	fi

	if diff -u "${KDIR}/${committed}" "${out}" >"${tmp}/diff"; then
		ktap_test_pass "${name}"
	else
		sed 's/^/# /' "${tmp}/diff"
		ktap_print_msg "regenerate with: tools/net/ynl/ynl-regen.sh -f"
		ktap_test_fail "${name}"
	fi
}

check_generated "${UAPI_H}" uapi header \
	"drm_fabric uAPI header matches netlink spec"
check_generated "${NL_C}" kernel source \
	"drm_fabric netlink ops and policy match netlink spec"
check_generated "${NL_H}" kernel header \
	"drm_fabric netlink kernel header matches netlink spec"

ktap_finished
