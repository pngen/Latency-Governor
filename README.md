# Latency Governor

**Latency Governor** is an open-source, vendor-neutral runtime for enforcing explicit latency SLOs across heterogeneous AI serving infrastructure.

The core systems question is:

> How should resource and execution decisions be governed against explicit latency obligations across admission, queues, batching, prefill, decode, speculation, memory, transfers, retries, and distributed execution?

## What this is

Latency Governor owns the **control boundary** where measured and predicted latency becomes an **enforceable runtime obligation**. It is not a latency dashboard, profiler, tracing tool, generic metrics library, queue implementation, or another inference scheduler.

The boundary is fundamental. An inference scheduler decides what work should run next and where. A batch fabric decides which compatible requests should execute together. A prefill fabric governs prompt processing; a decode fabric governs token generation; a speculation fabric governs commit authority; a transfer fabric governs how bytes move; a memory system determines when scarcity becomes dangerous. **Latency Governor decides whether those actions remain compatible with explicit latency obligations, what intervention is required when they are not, and how scarce latency budget should be allocated across the end-to-end request path.**

It is not merely monitoring latency; it is governing resource decisions against explicit latency obligations.

## Architecture

The runtime is a C++20 systems library organized around a vendor-neutral core with a concrete CUDA validation backend.

* **Requests and contracts.** Every admitted request carries an explicit `LatencyContract` rather than an informal priority: target latency, soft and hard deadlines, per-phase budgets, TTFT and inter-token targets, queueing/dispatch budgets, transfer allowance, retry allowance, speculation overhead, tolerances, admission/cancellation policy, and fairness weight.
* **Latency budget accounting.** `PhaseBudgets` attributes monotonic elapsed time into admission, queueing, batch-wait, scheduling, transfer, prefill, decode, speculation, retry/backoff, recovery, completion, and unclassified overhead, with deterministic roll-forward/donation that preserves total-budget invariants. Overspend in one phase becomes visible to later phases; it never silently resets.
* **Prediction.** `Predictor` provides bounded, explicit predictions with uncertainty intervals, confidence class, evidence count, predictor generation, and provenance (measured/derived/configured/fallback). Cold start is explicit; no invented precision.
* **Risk and feasibility.** `evaluate_risk` classifies each request as SAFE/WATCH/AT_RISK/CRITICAL/INFEASIBLE from explicit components (remaining budget, predicted remaining, uncertainty, deadline reached, TTFT/inter-token/transfer/retry/speculation risk, resource pressure). It is deterministic and exposes every contributing factor — an operator can always ask *why* a request is at risk.
* **Governance actions.** The intervention planner returns typed, explainable actions such as ADMIT, DEFER, SEAL_BATCH_NOW, BYPASS_BATCHING, EXPEDITE, PREFER_LOCAL_EXECUTION, CANCEL_RETRY, LIMIT_RETRY_BACKOFF, DISABLE_SPECULATION, REDUCE_SPECULATION_DEPTH, YIELD_PREFILL, PROTECT_DECODE, FAIL_FAST, CANCEL, and MARK_SLO_VIOLATION, each with a structured reason code and supporting evidence.
* **Policy engine.** Versioned, validated, serializable, deterministic, atomically replaceable policies. A request admitted under one generation retains knowledge of that generation; live replacement never reinterprets historical decisions.
* **Fairness.** Bounded multi-tenant fairness (service consumed, interventions received, deadline pressure, starvation age, weight, active count, class) with deterministic arbitration and stable tie-breaking, so a lower-latency class is protected without starving background traffic indefinitely.
* **Authority.** Strong typed IDs, explicit generations and epochs, and strict-order authority validation. A request may not be mutated by a stale completion, stale worker incarnation, stale attempt, stale generation, duplicate completion, or mismatched dispatch.
* **Persistence / recovery.** Versioned binary format, checksummed, with atomic temp→flush→close→rename writes. Recovery rejects corruption, truncation, impossible lengths, unsupported versions, malformed enums, duplicate identities, inconsistent accounting, and impossible transitions.
* **Protocol.** A real framed binary TCP protocol (magic, version, message type, bounded payload, canonical byte order) with complete send/recv loops and malformed/truncated/unknown/unsupported rejection.
* **Distributed.** Real coordinator + worker OS processes over framed TCP with worker registration (WorkerId, WorkerBootId, capabilities, device, protocol version, generation) and epoch-based restart fencing.

### Conceptual adjacency

Latency Governor does **not** reimplement its sibling runtimes. It *governs* them. A scheduler, batch fabric, prefill/decode runtimes, speculation fabric, transfer engine, or memory system can query it or submit latency observations through a clean control API. The boundary is: those runtimes decide *how* to work; Latency Governor decides whether their actions remain compatible with explicit latency obligations. This project stands on its own and does not depend on any sibling repository.

## Latency contracts and SLO classes

Classes such as REALTIME, INTERACTIVE, STANDARD, THROUGHPUT, and BACKGROUND are built-in defaults over a generic policy representation. A contract distinguishes target latency, soft deadline, hard deadline, latency budget, consumed budget, remaining budget, predicted lateness, and actual violation — never collapsed into a single timestamp.

## CUDA hardware proof

Latency Governor ships a real CUDA backend (CudaBackend) validated on an NVIDIA RTX 5090 (compute capability 12.0) with CUDA 13.x. It performs real `cudaMalloc`, H2D copies, kernel launches, synchronization, D2H copyback, verification, and free across two materially different workload classes (prefill-like bulk and decode-like iterative). Real monotonic measurements from accelerator execution feed the predictor and governor, and a governed test demonstrates a request crossing from SAFE to AT_RISK and a deterministic intervention. Numerical output is verified and device/runtime accounting closes cleanly.

## SLO class / phase budget / prediction / risk / intervention model

*(see the public headers under `include/latency_governor/` and the documentation comments for the precise fields.)*

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

With the CUDA validation backend (RTX 5090, compute capability 12.0, sm_120):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DLATENCY_GOVERNOR_ENABLE_CUDA=ON \
    -DCMAKE_CUDA_COMPILER="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1/bin/nvcc.exe"
```

## Install and downstream use

```
cmake --install build --prefix <prefix>
```

Then consume from a separate project:

```
find_package(LatencyGovernor CONFIG REQUIRED)
target_link_libraries(app PRIVATE LatencyGovernor::latency_governor)
```

## Tests

```
ctest --test-dir build
```

## Examples and benchmarks

```
# examples
./build/examples/example_01_contract
# benchmarks measure completed work, not enqueue/submit latency
./build/benchmarks/bench_core
```

## Validation

This table is populated only from actually observed results (see the closure
procedure in the repository notes). No placeholder success numbers are treated
as measured.

| Validation | Result |
| --- | --- |
| CTest targets / cases | 7 test targets, 100% pass in both Release and Debug |
| Randomized / property operations | test_property: 6,320 checks, seed printed and deterministic |
| Concurrency validation | 8 writer threads x 1,500 admitt/observe + 2 reader threads; passes (deadlock/race free) |
| Adversarial checks | corruption, truncation, bad magic, unsupported version, trailing data, malformed enums/lengths, stale authority, duplicate ids/replays/wrong generation/attempt/epoch |
| Protocol checks | framing round-trip, malformed/truncated/unknown/unsupported rejection; full send/recv loops |
| Persistence / recovery checks | save/load round-trip preserving identity/generation/epoch; rejects corruption, truncation, bad magic, unsupported version, trailing data |
| Multiprocess runs | real coordinator + 2 real worker OS processes; worker killed and restarted with new WorkerBootId; stale epoch/gen/boot/attempt/dispatch replayed over real TCP and rejected; fresh work succeeds; accounting returns to zero (passed 3x, deterministic) |
| CUDA checks (RTX 5090) | real cudaMalloc/H2D/kernel launch/sync/D2H/verify/free on RTX 5090 (cc 12.0, sm_120); measured durations feed the predictor; governed request crosses to AT_RISK/CRITICAL and emits a deterministic intervention; numerical output verified; device/accounting closes to zero |
| Examples building/running | 14 examples build and run |
| Warning status (/W4 /WX) | zero warnings in Release and Debug |
| Downstream find_package result | independent consumer compiles against the installed package via find_package(LatencyGovernor CONFIG REQUIRED) and runs (exit 0) |
| Benchmark results | bench_core admission ~9.8M ops/s, observation ingest ~7.3M ops/s, risk ~6.2M ops/s, intervention ~1.3M ops/s, predictor update ~92M/s, snapshot ~52k/s, explain ~480k/s; bench_scale admit ~2M ops/s at 1k/10k/100k; bench_threads 16 threads ~505k obs ops/s; bench_protocol ~11.9M round-trips/s; bench_cuda 400 completed kernels ~2.2k/s feeding the governor |

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.