.. SPDX-License-Identifier: (GPL-2.0+ OR MIT)

===============================
DRM Fabric over Generic Netlink
===============================

Modern GPUs and dedicated AI accelerators are increasingly connected through
scale-up interconnect fabrics such as AMD xGMI and
`UALink <https://ualinkconsortium.org/specification/>`__.

DRM Fabric is a registry and dispatcher. It does not discover routes, compute
reachability, program switch forwarding, manage device memory, or provide a
data path; those remain with the vendor driver and the fabric controller.

Key Goals:

* Provide a standardized topology model for GPU and accelerator interconnects
  (xGMI, UALink and similar), enabling data-center discovery and monitoring.
* Support read-only enumeration, monitoring and state queries for
  provider-owned topology.
* Allow new attributes and fabric types to be added without reusing existing
  wire identifiers, so the uAPI extends without breaking existing consumers.
* Allow multiple endpoints and ports per provider, so drivers can model
  accelerator attachments, links and their peers.

.. contents::

Object model
============

DRM Fabric models interconnect topology with four object types::

    fabric
      `-- endpoint
            `-- port
                  `-- peer (optional value descriptor)

A *fabric* is a membership group of *endpoints*; each endpoint represents one
accelerator attachment and owns a fixed set of *ports*; and a *port* may carry a
*peer* describing the device directly adjacent at the far end of its link, which
is either another accelerator or a fabric switch.

Identity is layered. The core assigns kernel-local ``fabric-id`` and
``endpoint-id`` values that address live registry objects for the duration of
their registration. Providers supply an endpoint ``fabric-ep-id`` -- an
accelerator's identity within its fabric's identity domain. A peer instead
carries a type-qualified ``peer-id``: for ``peer-type = accel`` it is the
far-end accelerator's ``fabric-ep-id``, and for ``peer-type = switch`` it is an
opaque provider-defined switch identity that names no local object. Accelerator
and switch identities occupy separate namespaces selected by ``peer-type``, so
the same numeric value may name different objects under each type.

Fabric membership does not imply end-to-end reachability, and the topology is
not necessarily a tree. The registered shape reflects the direct adjacency the
provider reports: for example, it may be a full mesh with no root, a linear
chain, or a switch-based topology in which ports terminate at opaque switch
peers rather than locally registered endpoints.

A *peer* is a value descriptor, not a reference to a live kernel object: its
``peer-id`` may name a remote accelerator managed by another OS or an opaque
switch in another trust domain, and need not resolve in the local registry. The
core stores one directed half-edge and does not require the reverse half-edge to
exist, so removing an endpoint does not retract peer descriptors held by other
endpoints. ``peer-type = switch`` only describes the kind of far end; it does
not create a first-class switch object.

.. kernel-doc:: drivers/gpu/drm/fabric/drm_fabric.c
   :doc: DRM Fabric core

Peer semantics
--------------

A peer is an identity rather than a reference to a live object, and the
difference decides what the core reports. A ``peer-id`` that resolves in the
local registry today may stop resolving later because the endpoint it named
unregistered, and no event is emitted on the port still carrying it: a peer
is recorded on one local half-edge, and peer disappearance does not retract
that half-edge. A peer is therefore topology as last set, not proof of live
connectivity; liveness belongs to the fabric controller.

The core never retracts a half-edge on its own. Failing to resolve a peer
locally is not the same as the link going away -- the far end may be a switch,
an accelerator on another node, or a local endpoint that merely unregistered
-- so only the provider knows when a port's physical adjacency actually
changed, and only the provider retracts or replaces the descriptor.

``port-peer-delete-ntf`` reports an explicitly retracted half-edge; it is
not emitted when a peer merely becomes locally unresolvable, so its absence
is not evidence the far end is still reachable.

Endpoint teardown removes the endpoint's owned half-edges without
generating a separate event for each port: the delete already describes
the transition, so removing an endpoint advances the topology generation
once rather than once per child port.

Driver API
----------

.. kernel-doc:: include/drm/drm_fabric.h
   :internal:

.. kernel-doc:: drivers/gpu/drm/fabric/drm_fabric.c
   :export:

Design scope and boundaries
===========================

Vendor drivers retain hardware discovery, firmware interaction and the
load/store data path; DRM Fabric represents only the topology and
provider-reported state of DRM-managed accelerators, which is why it
belongs in DRM. The interface does not define MMU programming, switch
policy, key management, live migration, or any required user space daemon.

DRM Fabric does not define in-network collective operations or how an
endpoint or switch executes them. Such capabilities belong to the
interconnect implementation and its provider. Adding capability later is not
foreclosed: for an endpoint it is a new attribute, while
``peer-type = switch`` is a value descriptor rather than a registered object,
so there is nowhere to attach one today. ``peer-id`` is already an opaque
switch identity that need not resolve locally, so making one resolvable later
strengthens the contract rather than breaking it.

Object lifetime and locking
===========================

All registry and object state is protected by ``drm_fabric_lock``. A fabric and
its endpoints are created and torn down through the provider API; endpoint
unregister removes the endpoint from the registry and frees its fixed set of
ports. Fabric membership is tracked, so a provider must remove all member
endpoints before unregistering a provider-owned fabric: drm_fabric_unregister()
returns ``-EBUSY`` and leaves the fabric registered if any remain, so the
provider must retry after removing them rather than treat the fabric as gone.

Objects are reference counted and a port is pinned through its owning endpoint.
Endpoint unregister drops the registration reference and waits for outstanding
pins before freeing the ports; fabric membership holds a fabric reference.

Providers own object lifetime, so a provider must serialise endpoint
registration against unregistration of the containing fabric. The unregister
entry points compare the supplied pointer against the registry before
dereferencing it, so a stale or repeated teardown is rejected: fabric
unregistration returns ``-ENODEV``, and endpoint unregistration, having no
error return, warns and performs no teardown. Endpoint registration rejects a
departed parent the same way. These checks prove current address membership
only: they cannot tell an earlier incarnation from another object registered
later at the same address.

Generic Netlink family
======================

DRM Fabric is exposed through the ``drm-fabric`` Generic Netlink family.
Dump enumeration and asynchronous notifications fit this multi-object model
better than one-value-per-file sysfs. The family follows the YAML/ynl
discipline used by DRM RAS; devlink's device hierarchy does not represent a
fabric spanning multiple DRM devices.

YAML specification
------------------

The interface is described in a YAML specification
``Documentation/netlink/specs/drm_fabric.yaml``, which is the source of truth for
the wire format. It auto-generates the uAPI header
(``include/uapi/drm/drm_fabric.h``) and the kernel glue via
``tools/net/ynl/pyynl/ynl_gen_c.py``. Generated files must never be edited by
hand; regenerate them with ``tools/net/ynl/ynl-regen.sh`` after any spec change.

uAPI stability
--------------

The YAML specification is the contract. New attributes, commands and enum values
are added append-only; existing attribute numbers, command numbers and meanings
are never reused. Requests are strictly validated, so an unknown attribute in a
request is rejected; user space should ignore attributes it does not recognize
in replies and notifications. The family is versioned through
``DRM_FABRIC_FAMILY_VERSION``.

Kernel-local ``fabric-id`` and ``endpoint-id`` values identify live registry
objects and are not persistent hardware identities: they remain valid for the
lifetime of the registered object, but may disappear or be reused after the
object is unregistered. ``instance-id`` and ``fabric-ep-id`` carry
provider-defined identity, whose scope is described by the containing object.

Query operations
================

User space enumerates topology with four read-only commands, each supporting a
single lookup (``do``) and a bulk dump (``dump``):

* ``fabric-get`` -- enumerate fabrics (``do`` by ``fabric-id``, ``dump`` for all).
* ``endpoint-get`` -- enumerate endpoints, optionally filtered by ``fabric-id``,
  or resolve one by ``endpoint-id`` or backing ``dev-name``/``bus-name``.
* ``port-get`` -- enumerate ports, filtered by ``endpoint-id``. Ports may report
  the provider's maximum capability as ``max-lane-count`` and
  ``max-lane-signaling-rate-mbps`` (zero means unknown); these are maxima, not
  the currently negotiated width or rate.
* ``port-stats-get`` -- per-port statistics, filtered by ``endpoint-id``. A
  targeted request for a port whose provider does not implement
  ``port_stats_get`` returns ``-EOPNOTSUPP``. During a dump, ports without
  statistics support are omitted and enumeration continues with later ports;
  any other provider error ends the dump.

Notifications
-------------

Subscribe to the ``monitor`` multicast group to receive asynchronous change
notifications. Full-object notifications reuse the shape of their matching
``get`` reply and are declared with ``notify:``; peer-link notifications carry
a partial payload and are declared with ``event:``.

.. list-table::
   :header-rows: 1

   * - Notification
     - Trigger
     - Payload
   * - ``fabric-create-ntf``
     - a fabric is registered by a provider
     - reuses ``fabric-get``
   * - ``fabric-delete-ntf``
     - a provider-owned fabric is unregistered
     - reuses ``fabric-get``
   * - ``endpoint-create-ntf``
     - an endpoint is registered
     - reuses ``endpoint-get``
   * - ``endpoint-delete-ntf``
     - an endpoint is unregistered
     - reuses ``endpoint-get``
   * - ``port-change-ntf``
     - a port's operational state changes
     - reuses ``port-get``
   * - ``port-peer-create-ntf``
     - a port's peer descriptor is set by its provider
     - partial (``event:``)
   * - ``port-peer-delete-ntf``
     - a port's peer descriptor is explicitly unset by its provider; never
       emitted for an implicit half-edge loss (see `Peer semantics`_)
     - partial (``event:``)

Notifications are best-effort. A listener that detects loss, restarts, or
receives an interrupted dump must rebuild state with the query commands.

Topology generation and dump consistency
----------------------------------------

The core keeps a nonzero generation counter and advances it whenever topology or
exposed state changes. It is surfaced as the ``topology-generation`` attribute on
``fabric-get``, ``endpoint-get`` and ``port-get`` replies and on the topology
notifications. A provider-reported change advances the generation before its
notification is serialized, so an event carries the post-change value that a
later ``get``/``dump`` will also report.

``topology-generation`` is a change token, not a timestamp, liveness counter or
event count: a changed value means topology changed, but the delta between two
values has no defined meaning and the counter may wrap (it skips zero). Statistics
reads do not advance it.

The same value backs dump consistency. Every multipart dump samples it and calls
``genl_dump_check_consistent()``; if it changes between dump batches, Generic
Netlink marks the dump with ``NLM_F_DUMP_INTR``, meaning the snapshot may be torn.
User space must then discard the partial result and retry the complete dump. The
generation does not apply to statistics reads.

Only a dump that serialized at least one entry can carry ``NLM_F_DUMP_INTR``,
because Generic Netlink arms the consistency check on the first entry it emits.
A dump that yields no entries at all cannot report interruption, so user space
should treat an empty result as advisory and re-read ``topology-generation``
before concluding that the topology is empty.

Namespaces
----------

DRM Fabric objects describe host-global hardware and are not scoped per network
namespace. Query and dump operations (``*-get``) are therefore accepted only from
the initial network namespace; a request from any other network namespace fails
with ``-EPERM``. The Generic Netlink family is registered ``netnsok`` (so it
resolves in any network namespace and can return that policy error rather than
being invisible), but the operations themselves remain confined to ``init_net``.
Monitor notifications are likewise emitted only into ``init_net``, so a listener
that joins the multicast group from another network namespace never receives
them.

Examples
========

Query the topology with the in-tree YNL tool, pointing it at the spec:

.. code-block:: bash

    # List all fabrics
    ./tools/net/ynl/pyynl/cli.py \
        --spec Documentation/netlink/specs/drm_fabric.yaml \
        --dump fabric-get

Replies follow the shapes described above; a provider must be registered for
the topology to be non-empty.

List the endpoints of a fabric:

.. code-block:: bash

    ./tools/net/ynl/pyynl/cli.py \
        --spec Documentation/netlink/specs/drm_fabric.yaml \
        --dump endpoint-get --json '{"fabric-id": 1}'

Query a single port:

.. code-block:: bash

    ./tools/net/ynl/pyynl/cli.py \
        --spec Documentation/netlink/specs/drm_fabric.yaml \
        --do port-get --json '{"endpoint-id": 1, "port-index": 0}'

The family name on the wire is ``drm-fabric``.
