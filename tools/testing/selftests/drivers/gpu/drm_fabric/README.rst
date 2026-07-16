.. SPDX-License-Identifier: GPL-2.0

====================
drm_fabric selftests
====================

These selftests exercise the ``drm-fabric`` query uAPI against
``drm_fabric_sim`` using the in-tree YNL library. KUnit covers the core object
model.

Tree layout
-----------

``Documentation/netlink/specs/drm_fabric.yaml``
  Netlink specification and source of truth for generated artifacts.

``include/uapi/drm/drm_fabric.h``
  Generated uAPI header; checked by ``check-spec-regen.sh``.

``drivers/gpu/drm/fabric/drm_fabric_nl.[ch]``
  Generated kernel policy and operation tables; checked by
  ``check-spec-regen.sh``.

Suites
------

``check-spec-regen.sh``
  Regenerates each artifact from the YAML spec and asserts an exact match.

``fabric_abi.py``
  Queries, events and topology.

``nl_policy_probe.py``
  Raw Generic Netlink policy probes and family introspection.

``dump_intr_abi.py``
  ``NLM_F_DUMP_INTR`` on a generation bump mid-dump, and on ``NLMSG_DONE``
  when the bump lands after the last entry.

``port_cursor_abi.py``
  Nested port-dump cursor (``cb->args``) across endpoints under removal.

``port_stats_cap_abi.py``
  Heterogeneous per-port stats: mid-list ``-EOPNOTSUPP`` skipped, other errno
  ends the dump.

``hotplug_abi.py``
  Endpoint hotplug: CREATE/DELETE notifications.

``dump_scale_abi.py``
  Dump resume under many endpoints (``bulk_add``).

``switch_abi.py``
  Opaque switch peers whose identifiers do not resolve to an endpoint
  (``topology=switch``).

``fault_abi.py``
  Provider fault injection: errno propagation and no leaked endpoint
  (``fail_*``).

``harness_reset_abi.py``
  Recovery after a SIGKILL-terminated predecessor.

``lib_drm_fabric.py``
  Shared helpers.

Expected skips
--------------

A SKIP means a required precondition was unavailable.

Environment
  ``check-spec-regen.sh`` needs PyYAML and writable temporary storage.

Per case
  A case skips when a required control, parameter or family capability is
  unavailable.

Whole suite
  A program skips when it cannot establish its initial topology.

Timing
  The two ``dump_intr_abi.py`` boundary cases may skip if the concurrent
  topology change misses the required dump boundary.

KUnit
-----

Keep the source tree free of ``.config`` and use an object directory:

.. code-block:: sh

   export KBUILD_OUTPUT="$PWD/.kunit/dev-kernel"

.. code-block:: sh

   ./tools/testing/kunit/kunit.py run \
       --kunitconfig drivers/gpu/drm/fabric/.kunitconfig 'drm_fabric*'

Debug configuration:

.. code-block:: sh

   ./tools/testing/kunit/kunit.py run --arch x86_64 \
       --kunitconfig drivers/gpu/drm/fabric/.kunitconfig.debug \
       --timeout 900 --qemu_args '-m 2048' 'drm_fabric*'

KASAN, UBSAN, kmemleak, lockdep or atomic-sleep reports fail the run.

Netlink ABI
-----------

Needs root and a booted kernel carrying the modules. See ``config`` for the
Kconfig fragment; the runner applies the 300-second timeout from ``settings``.

.. code-block:: sh

   sudo make -C tools/testing/selftests TARGETS=drivers/gpu/drm_fabric run_tests

virtme-ng
---------

Build out-of-tree, boot with ``vng`` and run the same target in the guest:

.. code-block:: sh

   O=.kunit/vng-drm-fabric
   vng --kconfig \
       --config tools/testing/selftests/drivers/gpu/drm_fabric/config "O=$O"
   make -j"$(nproc)" "O=$O" LOCALVERSION=-virtme
   vng --run "$O" --user root -- \
       env FABRIC_DIR="$PWD/$O/drivers/gpu/drm/fabric" \
       make -C tools/testing/selftests TARGETS=drivers/gpu/drm_fabric run_tests

Dependencies (Debian/Ubuntu): ``python3``, ``python3-yaml``,
``qemu-system-x86``, ``virtme-ng`` (``pip install --user virtme-ng``).