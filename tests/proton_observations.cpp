#include "proton/bridge_events.h"
#include "proton/feature_state.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace
{

    using vkshade::proton::Evaluation;
    using vkshade::proton::FeatureSnapshot;
    using MonotonicClock = std::chrono::steady_clock;

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "proton observation contract failed: %s\n", message);
            std::abort();
        }
    }

    void require_empty_epoch(const FeatureSnapshot& state)
    {
        require(state.dlss == Evaluation::Never, "DLSS clears at an epoch boundary");
        require(state.dlssg == Evaluation::Never, "DLSS-G clears at an epoch boundary");
        require(state.dlssrr == Evaluation::Never, "DLSS-RR clears at an epoch boundary");
        require(state.resource_tags == 0, "resource tags clear at an epoch boundary");
        require(state.framegen_mode == -1, "frame generation mode clears at an epoch boundary");
    }

    void require_status_preserved(const FeatureSnapshot& state)
    {
        require(state.monitoring, "monitoring status survives an epoch boundary");
        require(!state.unavailable, "monitoring clears unavailable status");
        require(state.interposer_present, "interposer presence survives an epoch boundary");
        require(state.coverage_limited, "coverage limitation survives an epoch boundary");
    }

    void require_telemetry_preserves_observation(const FeatureSnapshot& before, const FeatureSnapshot& after, bool evaluations_must_match)
    {
        require(before.monitoring && !before.unavailable && before.interposer_present && before.coverage_limited,
                "telemetry fixture has populated monitor status");
        require(after.monitoring == before.monitoring, "telemetry preserves monitoring status");
        require(after.unavailable == before.unavailable, "telemetry preserves unavailable status");
        require(after.interposer_present == before.interposer_present, "telemetry preserves interposer status");
        require(after.coverage_limited == before.coverage_limited, "telemetry preserves coverage status");

        require(before.dlss != Evaluation::Never && before.dlssg != Evaluation::Never && before.dlssrr != Evaluation::Never,
                "telemetry fixture has populated feature evaluations");
        require(after.dlss != Evaluation::Never && after.dlssg != Evaluation::Never && after.dlssrr != Evaluation::Never,
                "telemetry preserves populated feature evaluations");
        if (evaluations_must_match)
        {
            require(after.dlss == before.dlss, "telemetry does not change DLSS evaluation age");
            require(after.dlssg == before.dlssg, "telemetry does not change DLSS-G evaluation age");
            require(after.dlssrr == before.dlssrr, "telemetry does not change DLSS-RR evaluation age");
        }

        require(before.resource_tags != 0 && after.resource_tags == before.resource_tags, "telemetry preserves populated resource tags");
        require(before.framegen_mode != -1 && after.framegen_mode == before.framegen_mode, "telemetry preserves configured frame generation mode");
    }

    void report_missed_freshness_sample(const char* phase)
    {
        std::fprintf(stderr, "Freshness sample missed: %s; retrying or marking the test skipped\n", phase);
    }

    void observe(uint32_t kind, uint64_t value, uint64_t generation)
    {
        vkshade_bridge_observe_event(kind, value, generation);
    }

    void check_concurrent_tag_union(uint64_t generation)
    {
        constexpr unsigned    worker_count = 16;
        std::atomic<unsigned> ready{0};
        std::atomic<bool>     start{false};
        std::thread           workers[worker_count];

        for (unsigned worker = 0; worker < worker_count; ++worker)
        {
            workers[worker] = std::thread([worker, generation, &ready, &start] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }

                for (unsigned group = 0; group < 4; ++group)
                {
                    const unsigned bit = worker + group * worker_count;
                    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, uint64_t{1} << bit, generation);
                }
            });
        }

        while (ready.load(std::memory_order_acquire) != worker_count)
        {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);

        for (auto& worker : workers)
        {
            worker.join();
        }

        require(vkshade::proton::feature_snapshot().resource_tags == UINT64_MAX,
                "completed concurrent tag callbacks retain the union of all 64 bits");
    }

} // namespace

int main()
{
    bool            freshness_evidence_complete = true;
    FeatureSnapshot state                       = vkshade::proton::feature_snapshot();
    require(!state.monitoring, "monitoring starts false");
    require(!state.unavailable, "unavailable starts false");
    require(!state.interposer_present, "interposer presence starts false");
    require(!state.coverage_limited, "coverage limitation starts false");
    require(state.tag_submission_calls == 0, "tag-submission call count starts at zero");
    require(state.last_tag_submission == 0, "last tag-submission payload starts at zero");
    require_empty_epoch(state);

    // Scoped events with generation zero cannot populate the initial epoch.
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 0);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, UINT64_MAX, 0);
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 2, 0);
    state = vkshade::proton::feature_snapshot();
    require_empty_epoch(state);

    constexpr uint32_t first_sdk_result     = 0x80000001U;
    constexpr uint32_t first_array_count    = 0x12345678U;
    constexpr uint64_t first_tag_submission = (uint64_t{first_sdk_result} << 32) | first_array_count;
    constexpr uint32_t last_sdk_result      = 0xFFFFFFFFU;
    constexpr uint32_t last_array_count     = 0;
    constexpr uint64_t last_tag_submission  = (uint64_t{last_sdk_result} << 32) | last_array_count;

    observe(VKSHADE_BRIDGE_TAG_SUBMISSION, first_tag_submission, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.tag_submission_calls == 1, "first telemetry call increments the call count");
    require(state.last_tag_submission == first_tag_submission, "first telemetry call records packed result and count");
    require_empty_epoch(state);
    require(!state.monitoring && !state.unavailable && !state.interposer_present && !state.coverage_limited,
            "tag-submission telemetry leaves lifecycle status unchanged");

    observe(VKSHADE_BRIDGE_TAG_SUBMISSION, last_tag_submission, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.tag_submission_calls == 2, "error-result telemetry call increments the count");
    require(state.last_tag_submission == last_tag_submission, "error-result telemetry updates the packed payload");
    require_empty_epoch(state);
    require(!state.monitoring && !state.unavailable && !state.interposer_present && !state.coverage_limited,
            "error-result telemetry leaves lifecycle status unchanged");

    observe(VKSHADE_BRIDGE_HOST_UNAVAILABLE, 0, 0);
    state = vkshade::proton::feature_snapshot();
    require(!state.monitoring, "host timeout does not mark monitoring ready");
    require(state.unavailable, "host timeout marks the initial monitor unavailable");

    observe(VKSHADE_BRIDGE_MONITORING, 0, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.monitoring, "monitoring event recovers from host timeout");
    require(!state.unavailable, "monitoring recovery clears host-timeout status");
    observe(VKSHADE_BRIDGE_HOST_UNAVAILABLE, 0, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.monitoring, "late host timeout does not overwrite reported monitoring");
    require(!state.unavailable, "late host timeout leaves a ready monitor available");

    observe(VKSHADE_BRIDGE_UNAVAILABLE, 0, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.unavailable, "companion unavailable event remains unconditional");
    require(!state.monitoring, "companion unavailable clears ready monitoring status");
    const bool monitoring_before_host_timeout = state.monitoring;
    observe(VKSHADE_BRIDGE_HOST_UNAVAILABLE, 0, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.monitoring == monitoring_before_host_timeout, "host timeout does not overwrite an observer-reported unavailable state");
    require(state.unavailable, "host timeout preserves observer-reported unavailable state");

    observe(VKSHADE_BRIDGE_MONITORING, 0, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.monitoring, "monitoring event marks the monitor ready");
    require(!state.unavailable, "monitoring event clears unavailable status");

    observe(VKSHADE_BRIDGE_INTERPOSER_PRESENT, 1, 0);
    observe(VKSHADE_BRIDGE_COVERAGE_LIMITED, 1, 0);
    state = vkshade::proton::feature_snapshot();
    require(state.interposer_present, "interposer-present event records library observation");
    require(state.coverage_limited, "coverage-limited event records limited coverage");

    // Generation 1 is the initial observation epoch.
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 1);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 1);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1001, 1);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 42, 1);

    constexpr uint64_t depth             = uint64_t{1} << 0;
    constexpr uint64_t motion            = uint64_t{1} << 1;
    constexpr uint64_t normals           = uint64_t{1} << 5;
    constexpr uint64_t normals_roughness = uint64_t{1} << 14;
    constexpr uint64_t hi_res_depth      = uint64_t{1} << 48;
    constexpr uint64_t linear_depth      = uint64_t{1} << 49;
    constexpr uint64_t arbitrary_tag     = uint64_t{1} << 63;
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, depth | hi_res_depth | arbitrary_tag, 1);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, motion | linear_depth, 1);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, normals | normals_roughness, 1);
    state = vkshade::proton::feature_snapshot();
    require(state.resource_tags == (depth | motion | normals | normals_roughness | hi_res_depth | linear_depth | arbitrary_tag),
            "resource tags union across callbacks and preserve high and arbitrary bits");

    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 4, 1);
    require(vkshade::proton::feature_snapshot().framegen_mode == -1, "invalid mode cannot replace the unknown mode");
    for (uint64_t mode = 0; mode <= 3; ++mode)
    {
        observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, mode, 1);
        require(vkshade::proton::feature_snapshot().framegen_mode == static_cast<int>(mode), "each configured frame generation mode is accepted");
    }
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, UINT64_MAX, 1);
    require(vkshade::proton::feature_snapshot().framegen_mode == 3, "invalid mode does not replace the last valid mode");

    // Each feature ID updates only its corresponding field in an empty epoch.
    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 2);
    state = vkshade::proton::feature_snapshot();
    require_empty_epoch(state);
    require_status_preserved(state);
    require(state.tag_submission_calls == 2, "tag-submission count persists through shutdown");
    require(state.last_tag_submission == last_tag_submission, "last tag-submission payload persists through shutdown");
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 2);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss != Evaluation::Never, "feature ID 0 records DLSS");
    require(state.dlssg == Evaluation::Never, "feature ID 0 leaves DLSS-G unchanged");
    require(state.dlssrr == Evaluation::Never, "feature ID 0 leaves DLSS-RR unchanged");
    require(state.resource_tags == 0, "feature ID 0 leaves resource tags unchanged");
    require(state.framegen_mode == -1, "feature ID 0 leaves frame generation mode unchanged");

    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 3);
    state = vkshade::proton::feature_snapshot();
    require_empty_epoch(state);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 3);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss == Evaluation::Never, "feature ID 1000 leaves DLSS unchanged");
    require(state.dlssg != Evaluation::Never, "feature ID 1000 records DLSS-G");
    require(state.dlssrr == Evaluation::Never, "feature ID 1000 leaves DLSS-RR unchanged");
    require(state.resource_tags == 0, "feature ID 1000 leaves resource tags unchanged");
    require(state.framegen_mode == -1, "feature ID 1000 leaves frame generation mode unchanged");

    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 4);
    state = vkshade::proton::feature_snapshot();
    require_empty_epoch(state);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1001, 4);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss == Evaluation::Never, "feature ID 1001 leaves DLSS unchanged");
    require(state.dlssg == Evaluation::Never, "feature ID 1001 leaves DLSS-G unchanged");
    require(state.dlssrr != Evaluation::Never, "feature ID 1001 records DLSS-RR");
    require(state.resource_tags == 0, "feature ID 1001 leaves resource tags unchanged");
    require(state.framegen_mode == -1, "feature ID 1001 leaves frame generation mode unchanged");

    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 5);
    state = vkshade::proton::feature_snapshot();
    require_empty_epoch(state);
    require_status_preserved(state);

    // Generation 5 holds all three evaluations for independent aging checks.
    auto generation5_started = MonotonicClock::now();
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 5);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 5);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1001, 5);
    auto generation5_recorded = MonotonicClock::now();
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, depth, 5);
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 3, 5);
    state = vkshade::proton::feature_snapshot();
    require(state.resource_tags == depth, "generation-5 retains its own resource tag");
    require(state.framegen_mode == 3, "generation-5 retains its own frame generation mode");

    constexpr uint32_t successful_sdk_result  = 0;
    constexpr uint32_t smaller_array_count    = 17;
    constexpr uint64_t smaller_tag_submission = (uint64_t{successful_sdk_result} << 32) | smaller_array_count;
    require(smaller_tag_submission < last_tag_submission, "later telemetry payload is numerically smaller");

    const FeatureSnapshot before_zero_result = vkshade::proton::feature_snapshot();
    observe(VKSHADE_BRIDGE_TAG_SUBMISSION, smaller_tag_submission, 0);
    state                                   = vkshade::proton::feature_snapshot();
    const bool zero_result_evaluations_safe = MonotonicClock::now() - generation5_started <= std::chrono::seconds(4);
    if (!zero_result_evaluations_safe)
    {
        freshness_evidence_complete = false;
        report_missed_freshness_sample("telemetry preservation of feature evaluations");
    }
    require(state.tag_submission_calls == 3, "zero-result telemetry call increments without filtering");
    require(state.last_tag_submission == smaller_tag_submission, "later smaller payload replaces larger last value");
    require_telemetry_preserves_observation(before_zero_result, state, zero_result_evaluations_safe);

    const FeatureSnapshot before_duplicate = vkshade::proton::feature_snapshot();
    observe(VKSHADE_BRIDGE_TAG_SUBMISSION, smaller_tag_submission, 0);
    state                                 = vkshade::proton::feature_snapshot();
    const bool duplicate_evaluations_safe = MonotonicClock::now() - generation5_started <= std::chrono::seconds(4);
    if (!duplicate_evaluations_safe)
    {
        freshness_evidence_complete = false;
        report_missed_freshness_sample("repeated telemetry preservation of feature evaluations");
    }
    require(state.tag_submission_calls == 4, "identical telemetry payload still increments the call count");
    require(state.last_tag_submission == smaller_tag_submission, "identical telemetry payload remains the latest value");
    require_telemetry_preserves_observation(before_duplicate, state, duplicate_evaluations_safe);

    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, motion, 5);
    state = vkshade::proton::feature_snapshot();
    require(state.resource_tags == (depth | motion), "generation-5 callbacks remain current after telemetry");
    require(state.framegen_mode == 3, "telemetry leaves the configured mode available to the current generation");

    bool               recent_interior_checked = false;
    constexpr unsigned freshness_attempt_limit = 3;
    for (unsigned attempt = 0; attempt < freshness_attempt_limit; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        const auto interior_sample_started      = MonotonicClock::now();
        state                                   = vkshade::proton::feature_snapshot();
        const auto interior_sample_finished     = MonotonicClock::now();
        const auto newest_event_age_lower_bound = interior_sample_started - generation5_recorded;
        const auto oldest_event_age_upper_bound = interior_sample_finished - generation5_started;
        if (newest_event_age_lower_bound >= std::chrono::seconds(2) && oldest_event_age_upper_bound <= std::chrono::seconds(4))
        {
            require(state.dlss == Evaluation::Recent, "DLSS remains Recent in the interior of its window");
            require(state.dlssg == Evaluation::Recent, "DLSS-G remains Recent in the interior of its window");
            require(state.dlssrr == Evaluation::Recent, "DLSS-RR remains Recent in the interior of its window");
            recent_interior_checked = true;
            break;
        }

        report_missed_freshness_sample("2-4 second Recent attempt");
        if (attempt + 1 < freshness_attempt_limit)
        {
            generation5_started = MonotonicClock::now();
            observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 5);
            observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 5);
            observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1001, 5);
            generation5_recorded = MonotonicClock::now();
        }
    }

    if (!recent_interior_checked)
    {
        freshness_evidence_complete = false;
        report_missed_freshness_sample("2-4 second Recent after three attempts");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    const auto older_sample_started = MonotonicClock::now();
    state                           = vkshade::proton::feature_snapshot();
    if (older_sample_started - generation5_recorded > std::chrono::seconds(5))
    {
        require(state.dlss == Evaluation::Earlier, "DLSS becomes Earlier beyond five seconds");
        require(state.dlssg == Evaluation::Earlier, "DLSS-G becomes Earlier beyond five seconds");
        require(state.dlssrr == Evaluation::Earlier, "DLSS-RR becomes Earlier beyond five seconds");
    }
    else
    {
        freshness_evidence_complete = false;
        report_missed_freshness_sample("beyond-five-second Earlier");
    }

    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 42, 5);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss == Evaluation::Earlier, "unknown feature ID does not refresh DLSS");
    require(state.dlssg == Evaluation::Earlier, "unknown feature ID does not refresh DLSS-G");
    require(state.dlssrr == Evaluation::Earlier, "unknown feature ID does not refresh DLSS-RR");

    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 1);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 1);
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1001, 1);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, depth | normals, 1);
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 0, 1);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss == Evaluation::Earlier, "older event cannot refresh current DLSS evaluation");
    require(state.dlssg == Evaluation::Earlier, "older event cannot refresh current DLSS-G evaluation");
    require(state.dlssrr == Evaluation::Earlier, "older event cannot refresh current DLSS-RR evaluation");
    require(state.resource_tags == (depth | motion), "older tag event cannot replace or extend current tags");
    require(state.framegen_mode == 3, "older mode event cannot replace the current mode");

    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 6);
    state = vkshade::proton::feature_snapshot();
    require_empty_epoch(state);
    require_status_preserved(state);
    require(state.tag_submission_calls == 4, "telemetry call count persists through populated-state shutdown");
    require(state.last_tag_submission == smaller_tag_submission, "smaller latest telemetry payload persists through populated-state shutdown");

    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 6);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, motion, 6);
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 1, 6);
    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 6);
    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 5);
    state = vkshade::proton::feature_snapshot();
    require(state.dlssg != Evaluation::Never, "same or older shutdown cannot clear current feature data");
    require(state.resource_tags == motion, "same or older shutdown cannot clear current tags");
    require(state.framegen_mode == 1, "same or older shutdown cannot clear current mode");

    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 0, 7);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, depth, 7);
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 3, 7);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss != Evaluation::Never, "generation-7 event advances the observation epoch");
    require(state.dlssg == Evaluation::Never, "generation-7 advance clears prior DLSS-G evaluation");
    require(state.dlssrr == Evaluation::Never, "generation-7 advance clears prior DLSS-RR evaluation");
    require(state.resource_tags == depth, "generation-7 retains its own resource tag");
    require(state.framegen_mode == 3, "generation-7 retains its own frame generation mode");
    observe(VKSHADE_BRIDGE_FEATURE_EVALUATED, 1000, 6);
    observe(VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN, depth | motion, 6);
    observe(VKSHADE_BRIDGE_FRAMEGEN_MODE, 2, 6);
    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 6);
    observe(VKSHADE_BRIDGE_SHUTDOWN, 0, 7);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss != Evaluation::Never, "stale shutdown leaves later-generation feature data intact");
    require(state.dlssg == Evaluation::Never, "stale callback cannot resurrect an older feature");
    require(state.resource_tags == depth, "stale callbacks and shutdown leave current tags intact");
    require(state.framegen_mode == 3, "stale callbacks and shutdown leave current mode intact");
    require_status_preserved(state);

    // Generation 8 begins empty, then concurrent callbacks must retain every
    // bit after all callers return.
    check_concurrent_tag_union(8);
    state = vkshade::proton::feature_snapshot();
    require(state.dlss == Evaluation::Never, "tag-only epoch has no DLSS evaluation");
    require(state.dlssg == Evaluation::Never, "tag-only epoch has no DLSS-G evaluation");
    require(state.dlssrr == Evaluation::Never, "tag-only epoch has no DLSS-RR evaluation");
    require(state.framegen_mode == -1, "tag-only epoch has unknown frame generation mode");
    require_status_preserved(state);

    if (!freshness_evidence_complete)
    {
        std::fprintf(stderr, "SKIP: freshness behavior lacked a valid monotonic observation window after bounded retries\n");
        return 77;
    }
    return 0;
}
