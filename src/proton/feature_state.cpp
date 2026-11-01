#include "feature_state.hpp"
#include "bridge_events.h"

#include <atomic>
#include <chrono>

namespace
{
    constexpr uint64_t max_generation = 0xffff;
    constexpr uint64_t max_time       = (uint64_t{1} << 48) - 1;
    constexpr uint64_t max_sequence   = (uint64_t{1} << 46) - 1;

    std::atomic<uint64_t> generation{1};
    enum class MonitorStatus
    {
        Initial,
        Monitoring,
        Unavailable
    };
    std::atomic<MonitorStatus> monitor_status{MonitorStatus::Initial};
    std::atomic<bool>          coverage_limited{false};
    std::atomic<bool>          interposer_present{false};
    std::atomic<uint64_t>      dlss_time{0};
    std::atomic<uint64_t>      dlssg_time{0};
    std::atomic<uint64_t>      dlssrr_time{0};
    std::atomic<uint64_t>      tag_generations[64]{};
    std::atomic<uint64_t>      tag_submission_calls{0};
    std::atomic<uint64_t>      last_tag_submission{0};
    std::atomic<uint64_t>      mode_sequence{0};
    std::atomic<uint64_t>      mode_record{0};
    std::atomic<bool>          mode_exhausted{false};

    void publish_max(std::atomic<uint64_t>& record, uint64_t value)
    {
        auto old = record.load();
        while (old < value && !record.compare_exchange_weak(old, value))
        {
        }
    }

    uint64_t now_ms()
    {
        const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (count < 0 || static_cast<uint64_t>(count) >= max_time)
        {
            coverage_limited.store(true);
            return 0;
        }
        return static_cast<uint64_t>(count) + 1;
    }

    vkshade::proton::Evaluation evaluation(uint64_t record, uint64_t epoch, uint64_t now)
    {
        if (epoch == 0 || record >> 48 != epoch)
            return vkshade::proton::Evaluation::Never;
        const auto timestamp = record & max_time;
        return now != 0 && (now < timestamp || now - timestamp <= 5000) ? vkshade::proton::Evaluation::Recent : vkshade::proton::Evaluation::Earlier;
    }

    bool promote_generation(uint64_t source_generation)
    {
        if (source_generation == 0)
            return false;
        auto old = generation.load();
        while (old != 0)
        {
            if (source_generation > max_generation)
            {
                if (!generation.compare_exchange_weak(old, 0))
                    continue;
                coverage_limited.store(true);
                return false;
            }
            if (old >= source_generation)
                return true;
            if (generation.compare_exchange_weak(old, source_generation))
                return true;
        }
        return false;
    }

    uint64_t next_mode_sequence()
    {
        auto old = mode_sequence.load();
        while (old < max_sequence)
        {
            if (mode_sequence.compare_exchange_weak(old, old + 1))
                return old + 1;
        }
        mode_exhausted.store(true);
        coverage_limited.store(true);
        return 0;
    }
} // namespace

extern "C" void VKSHADE_BRIDGE_ABI vkshade_bridge_observe_event(uint32_t kind, uint64_t value, uint64_t source_generation)
{
    if (kind == VKSHADE_BRIDGE_TAG_SUBMISSION)
    {
        last_tag_submission.store(value);
        tag_submission_calls.fetch_add(1);
        return;
    }
    if (kind == VKSHADE_BRIDGE_MONITORING)
    {
        monitor_status.store(MonitorStatus::Monitoring);
        return;
    }
    if (kind == VKSHADE_BRIDGE_UNAVAILABLE)
    {
        monitor_status.store(MonitorStatus::Unavailable);
        return;
    }
    if (kind == VKSHADE_BRIDGE_HOST_UNAVAILABLE)
    {
        auto expected = MonitorStatus::Initial;
        monitor_status.compare_exchange_strong(expected, MonitorStatus::Unavailable);
        return;
    }
    if (kind == VKSHADE_BRIDGE_COVERAGE_LIMITED)
    {
        coverage_limited.store(true);
        return;
    }
    if (kind == VKSHADE_BRIDGE_SHUTDOWN)
    {
        promote_generation(source_generation);
        return;
    }
    if (kind == VKSHADE_BRIDGE_INTERPOSER_PRESENT)
    {
        interposer_present.store(true);
        return;
    }

    if (kind != VKSHADE_BRIDGE_FEATURE_EVALUATED && kind != VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN && kind != VKSHADE_BRIDGE_FRAMEGEN_MODE)
        return;
    if (!promote_generation(source_generation))
        return;
    const auto epoch = source_generation;
    switch (kind)
    {
        case VKSHADE_BRIDGE_FEATURE_EVALUATED:
        {
            std::atomic<uint64_t>* record = nullptr;
            switch (value)
            {
                case 0: record = &dlss_time; break;
                case 1000: record = &dlssg_time; break;
                case 1001: record = &dlssrr_time; break;
                default: break;
            }
            if (record)
            {
                const auto timestamp = now_ms();
                if (timestamp != 0)
                    publish_max(*record, (epoch << 48) | timestamp);
            }
            break;
        }
        case VKSHADE_BRIDGE_RESOURCE_TAGS_SEEN:
            for (unsigned bit = 0; bit < 64; ++bit)
            {
                if (value & (uint64_t{1} << bit))
                    publish_max(tag_generations[bit], epoch);
            }
            break;
        case VKSHADE_BRIDGE_FRAMEGEN_MODE:
            if (value <= 3)
            {
                const auto sequence = next_mode_sequence();
                if (sequence != 0)
                    publish_max(mode_record, (epoch << 48) | (sequence << 2) | value);
            }
            break;
        default: break;
    }
}

namespace vkshade::proton
{
    FeatureSnapshot feature_snapshot()
    {
        FeatureSnapshot snapshot{};
        uint64_t        epoch;
        do
        {
            epoch                         = generation.load();
            const auto dlss               = dlss_time.load();
            const auto dlssg              = dlssg_time.load();
            const auto dlssrr             = dlssrr_time.load();
            const auto mode               = mode_record.load();
            const auto now                = now_ms();
            const auto status             = monitor_status.load();
            snapshot.monitoring           = status == MonitorStatus::Monitoring;
            snapshot.unavailable          = status == MonitorStatus::Unavailable;
            snapshot.interposer_present   = interposer_present.load();
            snapshot.coverage_limited     = coverage_limited.load();
            snapshot.tag_submission_calls = tag_submission_calls.load();
            snapshot.last_tag_submission  = last_tag_submission.load();
            snapshot.dlss                 = evaluation(dlss, epoch, now);
            snapshot.dlssg                = evaluation(dlssg, epoch, now);
            snapshot.dlssrr               = evaluation(dlssrr, epoch, now);
            snapshot.resource_tags        = 0;
            for (unsigned bit = 0; bit < 64; ++bit)
            {
                if (epoch != 0 && tag_generations[bit].load() == epoch)
                    snapshot.resource_tags |= uint64_t{1} << bit;
            }
            snapshot.framegen_mode = epoch != 0 && !mode_exhausted.load() && mode >> 48 == epoch ? static_cast<int>(mode & 3) : -1;
        } while (generation.load() != epoch);
        return snapshot;
    }
} // namespace vkshade::proton
