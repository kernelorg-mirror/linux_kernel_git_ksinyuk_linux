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
* Offer a flexible, future-proof interface that can be extended with new fabric
  types and attributes without breaking the uAPI.
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

Endpoint teardown removes the endpoint's owned half-edges without generating
a separate event for each port: the delete already describes the transition,
so removing an endpoint advances the topology generation once rather than
once per child port.

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
