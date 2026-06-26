#include "goplayalong.h"

#include "process_reader.h"
#include "wstring_utils.h"

#include <cmath>
#include <format>
#include <stdexcept>
#include <utility>

namespace tnt {

static constexpr wchar_t PROCESS_NAME[] = L"Go PlayAlong 4.exe";
static constexpr wchar_t MODULE_NAME[]  = L"native.node";

// *(native.node+0x12FD18) → *(+0x14) → *(+0x0) → +0xC8 = double (seconds)
static constexpr int POSITION_MODULE_OFFSET = 0x12FD18;
static constexpr int POSITION_OFFSETS[]     = {0x0, 0x14, 0x0, 0xC8};

// *(native.node+0x12FD28) → +0x44 = DWORD, 0=stopped 1=playing
static constexpr int PLAY_STATE_MODULE_OFFSET = 0x12FD28;
static constexpr int PLAY_STATE_OFFSETS[]     = {0x0, 0x44};

// *(native.node+0x12FD28) + 0x20 = double (loop start, seconds)
// *(native.node+0x12FD28) + 0x28 = double (loop end, seconds)
// These are stored directly in the module global struct — no pointer dereference.
// loop_state is inferred: active when time_selection_end > 0.
static constexpr int TIME_SELECTION_START_OFFSETS[] = {0x20};
static constexpr int TIME_SELECTION_END_OFFSETS[]   = {0x28};

// Primary:    *(native.node+0x12FD18) → *(+0x0) → *(+0x14) → *(+0x4) → *(+0xD8) → *(+0xC)  → +0x2C (catalog/downloaded files)
// Fallback 1: *(native.node+0x12FD18) → *(+0x14) → *(+0x4) → *(+0xD8) → *(+0x0) → +0x2C          (user-created .gp files)
// Fallback 2: *(native.node+0x12FD18) → *(+0x14) → *(+0x4) → *(+0x1C) → *(+0xD8) → *(+0x0) → +0x2C
static constexpr int PLAY_RATE_MODULE_OFFSET           = 0x12FD18;
static constexpr int PLAY_RATE_OFFSETS[]               = {0x0, 0x14, 0x4, 0xD8, 0xC, 0x2C};
static constexpr int PLAY_RATE_FALLBACK_1_OFFSETS[]    = {0x14, 0x4, 0xD8, 0x0, 0x2C};
static constexpr int PLAY_RATE_FALLBACK_2_OFFSETS[]    = {0x14, 0x4, 0x1C, 0xD8, 0x0, 0x2C};
static constexpr float PLAY_RATE_VALID[]              = {0.5f, 0.6000000238f, 0.6999999881f, 0.8000000119f, 0.8999999762f, 1.0f};
static constexpr float PLAY_RATE_EPSILON              = 0.0001f;

static bool IsValidPlayRate(const float rate)
{
    for (const float valid : PLAY_RATE_VALID)
        if (std::fabs(rate - valid) < PLAY_RATE_EPSILON)
            return true;
    return false;
}

// count_in_state: not present in GoPlayAlong, always false

struct GoPlayAlong::Impl final
{
    GoPlayAlongState ReadProcessMemory()
    {
        const ProcessReader reader(PROCESS_NAME, MODULE_NAME);

        const double raw_position = reader.ReadMemoryAddress<double>(
            POSITION_MODULE_OFFSET, {POSITION_OFFSETS[0], POSITION_OFFSETS[1], POSITION_OFFSETS[2], POSITION_OFFSETS[3]});

        const DWORD raw_play_state = reader.ReadMemoryAddress<DWORD>(
            PLAY_STATE_MODULE_OFFSET, {PLAY_STATE_OFFSETS[0], PLAY_STATE_OFFSETS[1]});

        float raw_play_rate = 1.0f;
        int rate_chain_used = 0;
        try
        {
            const float rate = reader.ReadMemoryAddress<float>(
                PLAY_RATE_MODULE_OFFSET, {PLAY_RATE_FALLBACK_1_OFFSETS[0], PLAY_RATE_FALLBACK_1_OFFSETS[1], PLAY_RATE_FALLBACK_1_OFFSETS[2], PLAY_RATE_FALLBACK_1_OFFSETS[3], PLAY_RATE_FALLBACK_1_OFFSETS[4]});
            if (IsValidPlayRate(rate))
                { raw_play_rate = rate; rate_chain_used = 1; }
            else
                throw std::runtime_error("out of range");
        }
        catch (...)
        {
            try
            {
                const float rate = reader.ReadMemoryAddress<float>(
                    PLAY_RATE_MODULE_OFFSET, {PLAY_RATE_FALLBACK_2_OFFSETS[0], PLAY_RATE_FALLBACK_2_OFFSETS[1], PLAY_RATE_FALLBACK_2_OFFSETS[2], PLAY_RATE_FALLBACK_2_OFFSETS[3], PLAY_RATE_FALLBACK_2_OFFSETS[4], PLAY_RATE_FALLBACK_2_OFFSETS[5]});
                if (IsValidPlayRate(rate))
                    { raw_play_rate = rate; rate_chain_used = 2; }
                else
                    throw std::runtime_error("out of range");
            }
            catch (...)
            {
                try
                {
                    const float rate = reader.ReadMemoryAddress<float>(
                        PLAY_RATE_MODULE_OFFSET, {PLAY_RATE_OFFSETS[0], PLAY_RATE_OFFSETS[1], PLAY_RATE_OFFSETS[2], PLAY_RATE_OFFSETS[3], PLAY_RATE_OFFSETS[4], PLAY_RATE_OFFSETS[5]});
                    if (IsValidPlayRate(rate))
                        { raw_play_rate = rate; rate_chain_used = 3; }
                }
                catch (...) {}
            }
        }


        GoPlayAlongState state{};

        state.play_position    = raw_position;
        state.play_state       = raw_play_state != 0;
        state.play_rate        = static_cast<double>(raw_play_rate);
        state.rate_chain_used  = rate_chain_used;

        const double raw_sel_start = reader.ReadMemoryAddress<double>(
            PLAY_STATE_MODULE_OFFSET, {TIME_SELECTION_START_OFFSETS[0]});
        const double raw_sel_end = reader.ReadMemoryAddress<double>(
            PLAY_STATE_MODULE_OFFSET, {TIME_SELECTION_END_OFFSETS[0]});

        state.time_selection_start_position = raw_sel_start;
        state.time_selection_end_position   = raw_sel_end;
        if (state.time_selection_start_position > state.time_selection_end_position)
            std::swap(state.time_selection_start_position, state.time_selection_end_position);
        state.loop_state = state.time_selection_end_position > 0.0;

        return state;
    }
};

GoPlayAlong::GoPlayAlong()
    : m_impl(std::make_unique<Impl>())
{}

GoPlayAlong::~GoPlayAlong() = default;

GoPlayAlongState GoPlayAlong::ReadProcessMemory()
{
    return m_impl->ReadProcessMemory();
}

}
