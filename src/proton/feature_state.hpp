#pragma once

#include <cstdint>

namespace vkshade::proton
{

    enum class Evaluation
    {
        Never,
        Recent,
        Earlier
    };

    struct FeatureSnapshot
    {
        bool       monitoring;
        bool       unavailable;
        bool       interposer_present;
        bool       coverage_limited;
        Evaluation dlss;
        Evaluation dlssg;
        Evaluation dlssrr;
        uint64_t   resource_tags;
        uint64_t   tag_submission_calls;
        uint64_t   last_tag_submission;
        // -1 means no valid mode has been observed.
        int framegen_mode;
    };

    // Evaluations are recent for five seconds after the successful call.
    FeatureSnapshot feature_snapshot();

} // namespace vkshade::proton
