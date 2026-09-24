# Ultraviolent Agent Instructions

Ultraviolent is a clean-room virtual hardware platform for SGI-class systems.

Before making any substantive change, read these files in order:

1. `README.adoc`
2. `doc/VISION.adoc`
3. `doc/ARCHITECTURE.adoc`
4. `doc/ENGINEERING.adoc`
5. `doc/AGENTIC-BUILD.adoc`
6. `doc/ROADMAP.adoc`
7. `doc/STATUS.adoc`
8. Any subsystem-specific documentation relevant to the task

The current milestone and next task in `doc/STATUS.adoc` are authoritative.

## Hard rules

* Use C++23.
* Linux/x86-64 is the reference host.
* The reference interpreter is permanent.
* Guest-visible time comes from Ultraviolent virtual time, never host wall-clock execution duration.
* Machines own topology.
* Devices own behavior.
* The core owns execution mechanics.
* Backends own host integration.
* General CPU/core/device code must not depend on a concrete machine such as IP27.
* A guest-visible device and its host backend are separate abstractions.
* Do not introduce a universal `Device` hierarchy.
* Do not use pervasive `std::shared_ptr`.
* Prefer explicit ownership and narrow interfaces.
* Do not patch PROM, IRIX, or installation media.
* Do not special-case guest PCs or known boot instruction addresses.
* Do not implement behavior solely because it makes PROM or IRIX progress farther.
* Do not use non-public/proprietary SGI or IRIX source as an implementation reference.
* Do not add JIT, SMP, graphics, IP35, or unrelated hardware before the roadmap reaches those milestones.
* Do not copy the old prototype mechanically into this repository.

## Development formula

Every substantive change follows:

OBSERVE -> SPECIFY -> TEST -> IMPLEMENT -> COMPARE -> DOCUMENT -> COMMIT

Boot progress is evidence, not a specification.

Every implemented behavior should have a hardware, architecture, test, or reproducible observational reason.

## Scope discipline

Do only the current milestone unless a prerequisite requires otherwise.

If completing a task seems to require violating an architectural rule, stop and document the conflict rather than working around it.

Do not create abstractions speculatively. Add them when a real machine, device, or second implementation demonstrates the need.

## Required validation

Before considering work complete:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run the sanitizer preset when relevant:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Update `doc/STATUS.adoc` after a significant work session with:

* what changed;
* what was proven;
* what assumption was disproven;
* current best checkpoint;
* remaining blocker;
* exact next task.

The repository documentation, tests, and code must be sufficient for another agent to continue without access to prior chat history.

