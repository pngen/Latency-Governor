#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace latency_governor {

// ---------------------------------------------------------------------------
// Enum machinery.
//
// Each enum is declared through LG_ENUM_MAKE(Name, ListMacro) where ListMacro
// is an X-macro expanding to a sequence of X(Name, IDENT) calls. Enum values,
// their string names, their count, and their conversions are all generated from
// the same list so they can never drift.
// ---------------------------------------------------------------------------

#define LG_ENUM_IDENT(N, X) X,
#define LG_ENUM_ENTRY(N, X) std::pair<N, std::string_view>{N::X, #X},

#define LG_ENUM_MAKE(Name, ListMacro)                                             \
    enum class Name { ListMacro(Name, LG_ENUM_IDENT) };                            \
    namespace lg_enums {                                                           \
        inline constexpr auto Name##_entries = std::array{                         \
            ListMacro(Name, LG_ENUM_ENTRY)};                                       \
        inline constexpr std::size_t Name##_count = Name##_entries.size();         \
    }                                                                              \
    inline constexpr std::size_t Name##_count = lg_enums::Name##_count;            \
    template <> struct EnumTraits<Name> {                                          \
        using type = Name;                                                         \
        static constexpr auto& entries() noexcept { return lg_enums::Name##_entries; } \
    };

// Forward declaration of the enum trait.
template <typename E>
struct EnumTraits;

// --- SLO classes -----------------------------------------------------------
#define LG_SLO_CLASS_LIST(N, X) \
    X(N, REALTIME) X(N, INTERACTIVE) X(N, STANDARD) X(N, THROUGHPUT) X(N, BACKGROUND)
LG_ENUM_MAKE(SloClass, LG_SLO_CLASS_LIST)

// --- Risk state ------------------------------------------------------------
#define LG_RISK_STATE_LIST(N, X) \
    X(N, SAFE) X(N, WATCH) X(N, AT_RISK) X(N, CRITICAL) X(N, INFEASIBLE)
LG_ENUM_MAKE(RiskState, LG_RISK_STATE_LIST)

// --- Lifecycle state -------------------------------------------------------
#define LG_LIFECYCLE_LIST(N, X) \
    X(N, ADMITTED) X(N, QUEUED) X(N, BATCHING) X(N, DISPATCH) X(N, TRANSFER) \
    X(N, PREFILL) X(N, DECODE) X(N, SPECULATION) X(N, RETRY) X(N, RECOVERY) \
    X(N, COMPLETING) X(N, COMPLETED) X(N, FAILED) X(N, CANCELLED)
LG_ENUM_MAKE(LifecycleState, LG_LIFECYCLE_LIST)

// --- Budget phase ----------------------------------------------------------
#define LG_PHASE_LIST(N, X) \
    X(N, ADMISSION) X(N, QUEUEING) X(N, BATCH_WAIT) X(N, SCHEDULING) \
    X(N, TRANSFER) X(N, PREFILL) X(N, DECODE) X(N, SPEC_PROPOSAL) \
    X(N, SPEC_VERIFY) X(N, RETRY_BACKOFF) X(N, RECOVERY) X(N, COMPLETION) \
    X(N, UNCLASSIFIED)
LG_ENUM_MAKE(Phase, LG_PHASE_LIST)

// --- Observation type ------------------------------------------------------
#define LG_OBSERVATION_LIST(N, X) \
    X(N, QUEUE_ENTERED) X(N, QUEUE_EXITED) X(N, BATCH_WAIT_STARTED) \
    X(N, BATCH_SEALED) X(N, DISPATCH_STARTED) X(N, DISPATCH_COMPLETED) \
    X(N, TRANSFER_STARTED) X(N, TRANSFER_COMPLETED) X(N, PREFILL_STARTED) \
    X(N, PREFILL_CHUNK_COMPLETED) X(N, FIRST_TOKEN) X(N, DECODE_STEP) \
    X(N, SPECULATION_STARTED) X(N, VERIFICATION_COMPLETED) X(N, RETRY_SCHEDULED) \
    X(N, RETRY_STARTED) X(N, RECOVERY_STARTED) X(N, COMPLETION_COMMITTED) \
    X(N, CANCELLATION) X(N, FAILURE) X(N, RESOURCE_PRESSURE) X(N, PREDICTOR_UPDATE) \
    X(N, WORKER_LOST) X(N, WORKER_JOINED) X(N, ADMISSION_RESULT)
LG_ENUM_MAKE(ObservationType, LG_OBSERVATION_LIST)

// --- Intervention action ---------------------------------------------------
#define LG_INTERVENTION_LIST(N, X) \
    X(N, ADMIT) X(N, DEFER) X(N, REJECT) X(N, CONTINUE) X(N, EXPEDITE) \
    X(N, ESCALATE_PRIORITY) X(N, SEAL_BATCH_NOW) X(N, REDUCE_BATCH_WAIT) \
    X(N, BYPASS_BATCHING) X(N, PREFER_LOCAL_EXECUTION) X(N, PREFER_WARM_WORKER) \
    X(N, AVOID_TRANSFER) X(N, ALLOW_TRANSFER) X(N, CANCEL_RETRY) X(N, ALLOW_RETRY) \
    X(N, LIMIT_RETRY_BACKOFF) X(N, DISABLE_SPECULATION) X(N, REDUCE_SPECULATION_DEPTH) \
    X(N, PRESERVE_SPECULATION) X(N, YIELD_PREFILL) X(N, CONTINUE_PREFILL) \
    X(N, PROTECT_DECODE) X(N, FAIL_FAST) X(N, CANCEL) X(N, MARK_SLO_VIOLATION)
LG_ENUM_MAKE(InterventionAction, LG_INTERVENTION_LIST)

// --- Admission verdict -----------------------------------------------------
#define LG_ADMISSION_LIST(N, X) \
    X(N, FEASIBLE) X(N, FEASIBLE_WITH_RISK) X(N, DEFERABLE) X(N, INFEASIBLE)
LG_ENUM_MAKE(AdmissionVerdict, LG_ADMISSION_LIST)

// --- Confidence class ------------------------------------------------------
#define LG_CONFIDENCE_LIST(N, X) X(N, NONE) X(N, LOW) X(N, MEDIUM) X(N, HIGH)
LG_ENUM_MAKE(ConfidenceClass, LG_CONFIDENCE_LIST)

// --- Prediction source -----------------------------------------------------
#define LG_PREDICTION_SOURCE_LIST(N, X) \
    X(N, MEASURED) X(N, DERIVED) X(N, CONFIGURED) X(N, FALLBACK)
LG_ENUM_MAKE(PredictionSource, LG_PREDICTION_SOURCE_LIST)

// --- Device class ----------------------------------------------------------
#define LG_DEVICE_CLASS_LIST(N, X) \
    X(N, CPU) X(N, CUDA) X(N, HIP) X(N, LEVEL_ZERO) X(N, VULKAN) X(N, METAL) X(N, UNKNOWN)
LG_ENUM_MAKE(DeviceClass, LG_DEVICE_CLASS_LIST)

// --- CUDA workload kind ----------------------------------------------------
#define LG_CUDA_WORKLOAD_LIST(N, X) X(N, PREFILL) X(N, DECODE)
LG_ENUM_MAKE(CudaWorkloadKind, LG_CUDA_WORKLOAD_LIST)

// --- Rejection / stale-authority code --------------------------------------
#define LG_REJECTION_LIST(N, X) \
    X(N, NONE) X(N, EPOCH_MISMATCH) X(N, BOOT_MISMATCH) X(N, ATTEMPT_MISMATCH) \
    X(N, GENERATION_MISMATCH) X(N, DISPATCH_MISMATCH) X(N, WORKER_MISMATCH) X(N, REQUEST_NOT_FOUND) \
    X(N, STATE_INELIGIBLE) X(N, DUPLICATE) X(N, PROTOCOL_MISMATCH) \
    X(N, UNSUPPORTED_VERSION) X(N, MALFORMED) X(N, TRUNCATED) \
    X(N, UNKNOWN_MESSAGE) X(N, CORRUPT) X(N, INVALID_ID) X(N, OUT_OF_RANGE) \
    X(N, RATE_LIMITED) X(N, NOT_AUTHORIZED) X(N, BUDGET_EXHAUSTED) \
    X(N, BACKEND_UNAVAILABLE) X(N, REQUEST_CANCELLED)
LG_ENUM_MAKE(RejectionCode, LG_REJECTION_LIST)

// --- Intervention reason code ---------------------------------------------
#define LG_REASON_LIST(N, X) \
    X(N, DEADLINE_SLACK) X(N, HARD_DEADLINE_NEAR) X(N, HARD_DEADLINE_EXPIRED) \
    X(N, SOFT_TARGET_AT_RISK) X(N, PREDICTED_LATE) X(N, PREDICTED_INFEASIBLE) \
    X(N, BUDGET_EXHAUSTED) X(N, BATCH_WAIT_EXCEEDED) X(N, QUEUE_RESIDENCE_EXCEEDED) \
    X(N, TTFT_AT_RISK) X(N, INTER_TOKEN_AT_RISK) X(N, TRANSFER_COST_EXCEEDED) \
    X(N, RETRY_RESERVE_EXHAUSTED) X(N, SPECULATION_OVERHEAD) \
    X(N, SPECULATION_ACCEPTANCE_LOW) X(N, RESOURCE_PRESSURE_HIGH) \
    X(N, FAIRNESS_STARVATION) X(N, POLICY_OVERRIDE) X(N, PREDICTION_LOW_CONFIDENCE) \
    X(N, REQUEST_CANCELLED) X(N, COMPLETION_IMPOSSIBLE) X(N, CLASS_PROTECTION) \
    X(N, BACKEND_UNAVAILABLE) X(N, WORKER_UNAVAILABLE) X(N, NO_NEW_EVIDENCE)
LG_ENUM_MAKE(ReasonCode, LG_REASON_LIST)

// --- Protocol message type -------------------------------------------------
#define LG_MSG_TYPE_LIST(N, X) \
    X(N, HELLO) X(N, HELLO_ACK) X(N, REGISTER) X(N, REGISTER_ACK) \
    X(N, ADMIT) X(N, ADMIT_ACK) X(N, OBSERVATION) X(N, INTERVENTION) \
    X(N, COMPLETION) X(N, SNAPSHOT) X(N, EVALUATE) X(N, EVALUATE_ACK) \
    X(N, EXPLAIN) X(N, PING) X(N, PONG) X(N, ERROR) X(N, SHUTDOWN) X(N, OBSERVATION_ACK)
LG_ENUM_MAKE(MessageType, LG_MSG_TYPE_LIST)

// --- Admission policy -----------------------------------------------------
#define LG_ADMISSION_POLICY_LIST(N, X) \
    X(N, STRICT) X(N, DEFER) X(N, BEST_EFFORT)
LG_ENUM_MAKE(AdmissionPolicy, LG_ADMISSION_POLICY_LIST)

// --- Cancellation policy ---------------------------------------------------
#define LG_CANCELLATION_POLICY_LIST(N, X) \
    X(N, FAIL_FAST) X(N, ALLOW_COMPLETE)
LG_ENUM_MAKE(CancellationPolicy, LG_CANCELLATION_POLICY_LIST)

// --- Resource pressure signal ---------------------------------------------
#define LG_RESOURCE_SIGNAL_LIST(N, X) \
    X(N, MEMORY_PRESSURE) X(N, COMPUTE_PRESSURE) X(N, GPU_MEMORY_PRESSURE) \
    X(N, TRANSFER_BANDWIDTH) X(N, QUEUE_PRESSURE) X(N, WORKER_PRESSURE) X(N, UNKNOWN_PRESSURE)
LG_ENUM_MAKE(ResourceSignalType, LG_RESOURCE_SIGNAL_LIST)

// ---------------------------------------------------------------------------
// Generic conversions. Every enum above registered via LG_ENUM_MAKE is handled
// through EnumTraits<E>. to_string and enum_from_string are templates so they
// can be used with explicit type arguments without overload ambiguity.
// ---------------------------------------------------------------------------

[[nodiscard]] inline const char* to_string_unknown() noexcept { return "UNKNOWN"; }

template <typename E>
[[nodiscard]] const char* to_string(E v) noexcept {
    for (const auto& [e, s] : EnumTraits<E>::entries()) {
        if (e == v) return s.data();
    }
    return "UNKNOWN";
}

template <typename E>
[[nodiscard]] std::optional<E> enum_from_string(std::string_view s) noexcept {
    for (const auto& [e, nm] : EnumTraits<E>::entries()) {
        if (nm == s) return e;
    }
    return std::nullopt;
}

// Number of named values for a registered enum.
template <typename E>
[[nodiscard]] constexpr std::size_t enum_count() noexcept {
    return EnumTraits<E>::entries().size();
}

// Index of an enum value in its registration order. Returns the full count
// (out of range) for an unregistered/unknown value.
template <typename E>
[[nodiscard]] constexpr std::size_t enum_index(E v) noexcept {
    const auto& arr = EnumTraits<E>::entries();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].first == v) return i;
    }
    return arr.size();
}

} // namespace latency_governor