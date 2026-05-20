#include "goplayalong.h"

#include "process_reader.h"
#include "wstring_utils.h"

#include <format>
#include <stdexcept>
#include <utility>

namespace tnt {

static constexpr wchar_t PROCESS_NAME[] = L"Go PlayAlong 4.exe";
static constexpr wchar_t MODULE_NAME[]  = L"native.node";

static constexpr int MODULE_OFFSET = 0x12FD18;

// CheatEngine chain: *(native.node+0x12FD18) → *(+0x14) → *(+0x0) → +0xC8 = double (seconds)
// Leading 0x0 triggers the initial dereference at the base address before adding offsets.
static constexpr int POSITION_OFFSETS[]              = {0x0, 0x14, 0x0, 0xC8};
static constexpr int TIME_SELECTION_START_OFFSETS[]  = {0x0}; // TODO(windows)
static constexpr int TIME_SELECTION_END_OFFSETS[]    = {0x0}; // TODO(windows)
static constexpr int PLAY_RATE_OFFSETS[]             = {0x0}; // TODO(windows)
static constexpr int PLAY_STATE_OFFSETS[]            = {0x0}; // TODO(windows)
static constexpr int LOOP_STATE_OFFSETS[]            = {0x0}; // TODO(windows)
static constexpr int COUNT_IN_STATE_OFFSETS[]        = {0x0}; // TODO(windows) - may not exist in GoPlayAlong

static constexpr int PLAY_STATE_FLAG_BIT  = 8; // TODO(windows)
static constexpr int LOOP_STATE_FLAG_BIT  = 8; // TODO(windows)
static constexpr int COUNT_IN_FLAG_BIT    = 8; // TODO(windows)

struct GoPlayAlong::Impl final
{
    GoPlayAlongState ReadProcessMemory()
    {
        const ProcessReader reader(PROCESS_NAME, MODULE_NAME);

        const double raw_position = reader.ReadMemoryAddress<double>(
            MODULE_OFFSET, {POSITION_OFFSETS[0], POSITION_OFFSETS[1], POSITION_OFFSETS[2]});

        int raw_sel_start = reader.ReadMemoryAddress<int>(
            MODULE_OFFSET, {TIME_SELECTION_START_OFFSETS[0]});

        int raw_sel_end = reader.ReadMemoryAddress<int>(
            MODULE_OFFSET, {TIME_SELECTION_END_OFFSETS[0]});

        const float raw_play_rate = reader.ReadMemoryAddress<float>(
            MODULE_OFFSET, {PLAY_RATE_OFFSETS[0]});

        const DWORD raw_play_state = reader.ReadMemoryAddress<DWORD>(
            MODULE_OFFSET, {PLAY_STATE_OFFSETS[0]});

        const DWORD raw_loop_state = reader.ReadMemoryAddress<DWORD>(
            MODULE_OFFSET, {LOOP_STATE_OFFSETS[0]});

        const DWORD raw_count_in = reader.ReadMemoryAddress<DWORD>(
            MODULE_OFFSET, {COUNT_IN_STATE_OFFSETS[0]});

        if (raw_sel_start > raw_sel_end)
        {
            std::swap(raw_sel_start, raw_sel_end);
        }

        GoPlayAlongState state{};

        state.play_position                 = raw_position; // already in seconds
        state.time_selection_start_position = static_cast<double>(raw_sel_start); // TODO(windows): adjust units
        state.time_selection_end_position   = static_cast<double>(raw_sel_end);   // TODO(windows): adjust units
        state.play_rate                     = static_cast<double>(raw_play_rate);

        state.play_state     = raw_play_state  & (1U << PLAY_STATE_FLAG_BIT); // TODO(windows): adjust if plain bool
        state.loop_state     = raw_loop_state  & (1U << LOOP_STATE_FLAG_BIT); // TODO(windows): adjust if plain bool
        state.count_in_state = raw_count_in    & (1U << COUNT_IN_FLAG_BIT);   // TODO(windows): adjust if plain bool

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
