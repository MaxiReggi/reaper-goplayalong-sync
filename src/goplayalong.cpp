#include "goplayalong.h"
#include "process_reader.h"

#include <chrono>
#include <deque>
#include <stdexcept>

namespace tnt {

static constexpr wchar_t PROCESS_NAME[] = L"Go PlayAlong 4.exe";
static constexpr wchar_t MODULE_NAME[]  = L"native.node";
static constexpr int     MODULE_OFFSET  = 0x12FD18;
// Pointer chain: native.node+0x12FD18 → [+0x14] → [+0x0] → +0xC8 = double (seconds)
static constexpr int POSITION_OFFSETS[] = {0x14, 0x0, 0xC8};

// Ticks of no position advance before declaring paused (~333ms at 30Hz)
static constexpr int    PAUSED_TICKS_THRESHOLD = 10;
// Width of the sliding window used to infer play rate
static constexpr double RATE_WINDOW_SEC        = 2.0;

struct GoPlayAlong::Impl
{
    struct RateSample { std::chrono::steady_clock::time_point time; double position; };

    double m_last_position = -1.0;
    int    m_paused_ticks  = 0;
    bool   m_is_playing    = false;
    double m_play_rate     = 1.0;
    std::deque<RateSample> m_rate_samples;
    std::chrono::steady_clock::time_point m_last_tick;

    GoPlayAlongState ReadProcessMemory()
    {
        const ProcessReader reader(PROCESS_NAME, MODULE_NAME);

        const double position = reader.ReadMemoryAddress<double>(
            MODULE_OFFSET, {POSITION_OFFSETS[0], POSITION_OFFSETS[1], POSITION_OFFSETS[2]});

        auto now = std::chrono::steady_clock::now();

        if (m_last_position >= 0.0)
        {
            const double delta = position - m_last_position;

            if (delta > 0.005 && delta < 0.5)
            {
                m_is_playing   = true;
                m_paused_ticks = 0;

                m_rate_samples.push_back({now, position});
                while (m_rate_samples.size() > 1)
                {
                    const double age = std::chrono::duration<double>(now - m_rate_samples.front().time).count();
                    if (age > RATE_WINDOW_SEC * 2.0) m_rate_samples.pop_front();
                    else break;
                }

                if (m_rate_samples.size() >= 4)
                {
                    const double window = std::chrono::duration<double>(
                        m_rate_samples.back().time - m_rate_samples.front().time).count();
                    if (window >= RATE_WINDOW_SEC)
                    {
                        const double rate = (m_rate_samples.back().position - m_rate_samples.front().position) / window;
                        if (rate > 0.1 && rate < 5.0)
                            m_play_rate = rate;
                    }
                }
            }
            else if (++m_paused_ticks >= PAUSED_TICKS_THRESHOLD)
            {
                m_is_playing = false;
                m_rate_samples.clear();
            }
        }

        m_last_position = position;
        m_last_tick     = now;

        GoPlayAlongState state{};
        state.play_position = position;
        state.play_state    = m_is_playing;
        state.play_rate     = m_play_rate;
        return state;
    }
};

GoPlayAlong::GoPlayAlong()  : m_impl(std::make_unique<Impl>()) {}
GoPlayAlong::~GoPlayAlong() = default;

GoPlayAlongState GoPlayAlong::ReadProcessMemory()
{
    return m_impl->ReadProcessMemory();
}

} // namespace tnt
