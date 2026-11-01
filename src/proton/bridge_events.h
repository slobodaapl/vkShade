#ifndef VKSHADE_PROTON_BRIDGE_EVENTS_H
#define VKSHADE_PROTON_BRIDGE_EVENTS_H

#include <stdint.h>

enum vkshade_bridge_event_kind
{
    VKSHADE_BRIDGE_MONITORING         = 1,
    VKSHADE_BRIDGE_INTERPOSER_PRESENT = 2,
    VKSHADE_BRIDGE_SHUTDOWN           = 3,
    VKSHADE_BRIDGE_UNAVAILABLE        = 4,
    VKSHADE_BRIDGE_FEATURE_EVALUATED  = 5,
    VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN = 6,
    VKSHADE_BRIDGE_FRAMEGEN_MODE      = 7,
    VKSHADE_BRIDGE_COVERAGE_LIMITED   = 8,
    VKSHADE_BRIDGE_HOST_UNAVAILABLE   = 9,
    VKSHADE_BRIDGE_TAG_SUBMISSION     = 10,
};

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define VKSHADE_BRIDGE_ABI __attribute__((sysv_abi))
#else
#define VKSHADE_BRIDGE_ABI
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VKSHADE_BRIDGE_EXPORT __attribute__((visibility("default")))
#else
#define VKSHADE_BRIDGE_EXPORT
#endif

/* Feature, tag, and mode events carry the nonzero generation sampled on hook
 * entry. Shutdown carries the incremented generation. Generation zero is
 * reserved for host process-status events and tag submission telemetry. Tag
 * submission value packs the uint32 result in the high half and numTags in the
 * low half. */
typedef void(VKSHADE_BRIDGE_ABI* vkshade_bridge_event_callback)(uint32_t kind, uint64_t value, uint64_t source_generation);

#ifdef __cplusplus
extern "C"
{
#endif
    VKSHADE_BRIDGE_EXPORT void VKSHADE_BRIDGE_ABI vkshade_bridge_observe_event(uint32_t kind, uint64_t value, uint64_t source_generation);
#ifdef __cplusplus
}
#endif

#endif
