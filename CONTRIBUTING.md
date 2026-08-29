# Contributing

Thank you for contributing to Latency Governor. This is a serious systems
runtime; contributions should preserve its architectural boundary and its
engineering guarantees.

## Core principles

* **Latency governance, not monitoring.** Latency Governor owns the control
  boundary where measured and predicted latency becomes an enforceable runtime
  obligation. Do not add instrumented dashboards or generic metrics libraries.
* **Deterministic behavior.** Equal inputs must produce equal decisions. No
  nondeterminism in policy, risk, intervention, or accounting logic.
* **Explicit authority.** No request state may be mutated by stale authority of
  any kind (epoch, boot id, attempt, generation, dispatch, lifecycle).
* **Bounded resources.** Everything potentially unbounded must have a
  configured maximum.
* **Vendor neutrality.** The core is backend-neutral; CUDA is a concrete
  validation backend.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Enable the CUDA backend (NVIDIA RTX 5090, compute capability 12.0, CUDA 13.x):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLATENCY_GOVERNOR_ENABLE_CUDA=ON \
    -DCMAKE_CUDA_COMPILER="<cuda>/bin/nvcc.exe"
```

## Coding standards

* C++20.
* No warnings under `/W4 /WX` on MSVC (the primary closure target) and
  `-Wall -Wextra -Wpedantic -Werror` elsewhere. Fix causes; never broadly
  suppress warnings.
* No ABI-dependent raw struct serialization; encode fields individually with
  canonical byte order.
* Monotonic clock only for all runtime decisions; wall-clock only for
  telemetry labels.
* Backend work, networking, persistence I/O, and CUDA synchronization must not
  run under the governor's internal lock.
* Prefer immutable snapshots / narrow critical sections.

## Tests

Add or extend tests under `tests/`. Cover correctness and, where appropriate,
property/adversarial, concurrency, persistence, protocol, multiprocess, and
CUDA paths. Use deterministic seeds for randomized tests and print the seed.
No test may rely on a hidden timeout escape hatch.

## Pull requests

* Explain the architectural boundary your change touches.
* Include regression tests for any defect you fix.
* Report measured validation results (do not invent numbers).
* Keep the repository stand-alone; do not depend on unreleased sibling
  repositories.
