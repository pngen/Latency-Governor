# Security

Latency Governor enforces latency obligations; it does not run untrusted code.
This document describes the security-relevant properties and trust boundaries.

## Malformed input handling

Every parser — binary serialization, persistence records, and the framed TCP
protocol — is bounds-checked, canonical-byte-order, and rejects malformed input
cleanly rather than reading out of bounds.

* **Serialization** (`latency_governor/serialization.hpp`): fixed-width
  big-endian fields. `BinaryReader` verifies every read is within the
  remaining buffer; a truncated or oversized value fails the operation.
* **Enum validation**: raw enum values are range-checked against the
  registration count before being cast; an out-of-range value is rejected.
* **Persistence**: magic, format version, payload length, checksum, and
  trailing-data are all validated. Corruption, truncation, impossible lengths,
  unsupported versions, malformed enums, duplicate identities, inconsistent
  accounting, and impossible state transitions are rejected. Recovery never
  pretends success.

## Protocol bounds

* Framing magic, protocol version, message type, and payload length are all
  validated before a payload is read.
* `kMaxFrameSize` (16 MiB) bounds any single frame; a larger frame is
  rejected.
* Length and count fields read from the network are never trusted to allocate
  unbounded memory; decoded collections are capped.
* A single `recv` is never assumed to equal one message; complete
  send/receive loops are used.

## Persistence integrity

* State is persisted with an explicit versioned format and an FNV-1a checksum.
* Atomic write: temp file → flush → close → atomic rename. An incomplete write
  is never treated as durable.
* A corrupt or truncated file is rejected at load; it is not silently repaired
  or trusted.

## Stale authority rejection

Requests may not be mutated by stale completions, stale worker incarnations,
stale attempts, stale generations, duplicate completions, or mismatched
dispatches. The coordinator validates the authority envelope in strict order
(protocol, epoch, generation, attempt, dispatch, worker, worker boot id,
lifecycle eligibility) and rejects stale work with an explicit code.

## Replay protection semantics

After a worker process is lost and restarted, the coordinator rolls the epoch
and increments the generation of affected requests, invalidating all in-flight
authority. Any replayed pre-restart message is rejected.

## Resource exhaustion limits

All unbounded structures are capped: active requests, completed history,
workers, tenants, policy history, predictor histories, observation history,
event queues, frame size, string length, batch members, retries, explanation
depth, and persistence record sizes. Malicious or nonsensical inputs are
rejected cleanly, never by unbounded allocation.

## No raw pointer serialization, no code from network

* POD/raw-pointer data is never serialized directly; fields are encoded
  individually.
* No executable code is loaded from network or persistence inputs. Persisted
  state is data only.

## CUDA backend trust boundary

The CUDA backend executes bounded, deterministic kernels on the local GPU. It
treats the CUDA runtime as trustworthy infrastructure and confines its work to
device memory it allocates and frees within each call. It never loads kernels
from external input. Device memory is released on every execution; calibration
and correctness checks are performed on the host.

## Local deployment assumptions

The packaged coordinator/worker/ctl communicate over a local TCP loopback /
trusted network. Authentication and encryption are the operator's
responsibility; the protocol is not intended for untrusted networks. Latency
Governor assumes a trusted deployment boundary and does not itself provide
transport-level security.
