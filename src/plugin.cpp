#include "plugin.h"

#include "goplayalong.h"
#include "reaper.h"

#include <array>
#include <format>
#include <memory>
#include <stdexcept>

namespace tnt {

// REAPER runs the MainLoop ~30 times/second.
// A desync window of 9 frames is approximately 300ms.
static constexpr int DESYNC_WINDOW_SIZE = 9;

static constexpr double DESYNC_THRESHOLD = 0.3;                    // seconds
static constexpr double MINIMUM_TIME_STEP = 0.001;                 // seconds
static constexpr double MINIMUM_PLAY_RATE_STEP = 0.001;
static constexpr double GOPLAYALONG_CURSOR_JUMP_THRESHOLD = 0.1;   // seconds
static constexpr double LATENCY_COMPENSATION = 0.05;               // seconds
// GPA updates position at ~15 Hz; 5 consecutive non-advancing ticks ≈ 166 ms of no movement.
static constexpr int NOT_ADVANCING_STOP_THRESHOLD = 5;

struct Plugin::Impl final
{
    Impl(PluginState& plugin_state)
        : m_plugin_state(plugin_state)
    {}

    void MainLoop()
    {
        try
        {
            m_goplayalong_state = m_goplayalong.ReadProcessMemory();
        }
        catch (const std::runtime_error& error)
        {
            if (m_last_error != error.what())
            {
                m_reaper.ShowConsoleMessage(error.what());
                m_last_error = error.what();
            }
            return;
        }

        if (!m_last_error.empty())
        {
            m_reaper.ShowConsoleMessage("Successfully connected to GoPlayAlong process.\n");
            m_last_error = "";
        }

        if (m_goplayalong_state.play_state && ++m_debug_ticks >= 30)
        {
            m_debug_ticks = 0;
            m_reaper.ShowConsoleMessage(std::format("[SyncGPA] play_rate={:.4f} chain={}\n",
                m_goplayalong_state.play_rate, m_goplayalong_state.rate_chain_used));
        }

        if (m_goplayalong_state.play_state)
        {
            SyncLoopState();
            SyncTimeSelection();
            SyncPlayPosition();
            SyncPlayRate();
        }
        else if (ReaperStoppedOrPaused())
        {
            if (GoPlayAlongLoopStateChanged())
            {
                SyncLoopState();
            }

            if (GoPlayAlongTimeSelectionChanged() && m_goplayalong_state.time_selection_end_position > MINIMUM_PLAY_RATE_STEP)
            {
                SyncTimeSelection();
                SetPlayPosition(m_goplayalong_state.time_selection_start_position);
            }
            else if (GoPlayAlongCursorMoved())
            {
                SyncTimeSelection();
                SetPlayPosition(m_goplayalong_state.play_position);
            }

            if (GoPlayAlongPlayRateChanged())
            {
                SyncPlayRate();
            }
        }

        SyncPlayState();

        m_prev_goplayalong_state = m_goplayalong_state;
    }

private:
    void SyncLoopState()
    {
        if (m_goplayalong_state.loop_state && !(m_goplayalong_state.play_state && m_goplayalong_state.count_in_state))
        {
            m_reaper.SetRepeat(true);
        }
        else
        {
            m_reaper.SetRepeat(false);
        }
    }

    void SyncTimeSelection()
    {
        m_reaper.SetTimeSelection(
            m_goplayalong_state.time_selection_start_position,
            m_goplayalong_state.time_selection_end_position);
    }

    void SyncPlayPosition()
    {
        if (GoPlayAlongCursorMoved() && !CompareDoubles(m_reaper.GetPlayPosition(), m_goplayalong_state.play_position, DESYNC_THRESHOLD))
        {
            // Do not sync if REAPER is right at a loop boundary
            if (CompareDoubles(m_reaper.GetPlayPosition(), m_goplayalong_state.time_selection_start_position, DESYNC_THRESHOLD)
             || CompareDoubles(m_reaper.GetPlayPosition(), m_goplayalong_state.time_selection_end_position, DESYNC_THRESHOLD))
            {
                return;
            }

            // Follow intentional seeks in GoPlayAlong
            if (!CompareDoubles(m_prev_goplayalong_state.play_position, m_goplayalong_state.play_position, GOPLAYALONG_CURSOR_JUMP_THRESHOLD))
            {
                SetPlayPosition(m_goplayalong_state.play_position + LATENCY_COMPENSATION);
            }
            else if (Desync(DESYNC_THRESHOLD))
            {
                SetPlayPosition(m_goplayalong_state.play_position + LATENCY_COMPENSATION);
            }
        }
    }

    void SyncPlayRate()
    {
        if (m_goplayalong_state.play_rate > MINIMUM_PLAY_RATE_STEP)
        {
            if (!CompareDoubles(m_reaper.GetPlayRate(), m_goplayalong_state.play_rate, MINIMUM_PLAY_RATE_STEP))
            {
                EnablePreservePitch();
                m_reaper.SetPlayState(ReaperPlayState::PAUSED);
                m_reaper.SetPlayRate(m_goplayalong_state.play_rate);
            }
        }
    }

    void SyncPlayState()
    {
        const bool ps             = m_goplayalong_state.play_state;
        const bool pps            = m_prev_goplayalong_state.play_state;
        const bool advancing      = m_goplayalong_state.play_position > m_prev_goplayalong_state.play_position + MINIMUM_TIME_STEP;
        const bool reaper_stopped = ReaperStoppedOrPaused();

        if (!reaper_stopped)
            m_not_advancing_ticks = advancing ? 0 : m_not_advancing_ticks + 1;
        else
            m_not_advancing_ticks = 0;

        // Position-based stop: GPA position frozen for N ticks means GPA is paused/stopped.
        // This handles songs where play_state stays 1 even when GPA is not playing.
        if (!reaper_stopped && m_not_advancing_ticks >= NOT_ADVANCING_STOP_THRESHOLD)
        {
            m_not_advancing_ticks = 0;
            m_reaper.SetPlayState(ReaperPlayState::STOPPED);
            return;
        }

        if (ps)
        {
            if (m_goplayalong_state.count_in_state
             && (!GoPlayAlongCursorMoved() || (m_goplayalong_state.time_selection_start_position > MINIMUM_TIME_STEP
                 && m_prev_goplayalong_state.play_position < MINIMUM_TIME_STEP)))
            {
                if (!CompareDoubles(m_reaper.GetPlayPosition(), m_goplayalong_state.time_selection_start_position, MINIMUM_TIME_STEP)
                 && m_reaper.GetPlayPosition() < m_goplayalong_state.time_selection_end_position)
                {
                    return;
                }
                m_reaper.SetPlayState(ReaperPlayState::STOPPED);
            }
            else if (reaper_stopped && advancing)
            {
                if (m_goplayalong_state.time_selection_start_position > MINIMUM_TIME_STEP)
                {
                    SetPlayPosition(m_goplayalong_state.time_selection_start_position + LATENCY_COMPENSATION);
                }
                else
                {
                    SetPlayPosition(m_goplayalong_state.play_position + LATENCY_COMPENSATION);
                }
                m_reaper.SetPlayState(ReaperPlayState::PLAYING);
            }
        }
        else if (!reaper_stopped && pps)
        {
            // play_state explicitly 0: stop immediately (normal GPA behavior)
            m_reaper.SetPlayState(ReaperPlayState::STOPPED);
        }
    }

    bool Desync(const double threshold)
    {
        std::rotate(m_desync_window.rbegin(), m_desync_window.rbegin() + 1, m_desync_window.rend());
        m_desync_window[0] = fabs(m_reaper.GetPlayPosition() - m_goplayalong_state.play_position);

        for (const double value : m_desync_window)
        {
            if (value < threshold)
            {
                return false;
            }
        }
        return true;
    }

    void SetPlayPosition(const double time)
    {
        m_reaper.SetEditCursorPosition(time, false, true);
        m_desync_window.fill(0.0);
    }

    bool CompareDoubles(const double val1, const double val2, const double epsilon) const
    {
        return fabs(val1 - val2) < epsilon;
    }

    bool GoPlayAlongLoopStateChanged() const
    {
        return m_goplayalong_state.loop_state != m_prev_goplayalong_state.loop_state;
    }

    bool GoPlayAlongTimeSelectionChanged() const
    {
        return !CompareDoubles(m_goplayalong_state.time_selection_start_position, m_prev_goplayalong_state.time_selection_start_position, MINIMUM_TIME_STEP)
            || !CompareDoubles(m_goplayalong_state.time_selection_end_position, m_prev_goplayalong_state.time_selection_end_position, MINIMUM_TIME_STEP);
    }

    bool GoPlayAlongCursorMoved() const
    {
        return !CompareDoubles(m_goplayalong_state.play_position, m_prev_goplayalong_state.play_position, MINIMUM_TIME_STEP);
    }

    bool GoPlayAlongPlayRateChanged() const
    {
        return !CompareDoubles(m_goplayalong_state.play_rate, m_prev_goplayalong_state.play_rate, MINIMUM_PLAY_RATE_STEP);
    }

    bool ReaperStoppedOrPaused() const
    {
        switch (m_reaper.GetPlayState())
        {
        case ReaperPlayState::STOPPED:
        case ReaperPlayState::PAUSED:
            return true;
        case ReaperPlayState::PLAYING:
            return false;
        default:
            throw std::runtime_error("REAPER is in an invalid play state.\n");
        }
    }

    void EnablePreservePitch() const
    {
        if (!m_reaper.GetToggleCommandState(ReaperToggleCommand::PRESERVE_PITCH))
        {
            m_reaper.ToggleCommand(ReaperToggleCommand::PRESERVE_PITCH);
        }
    }

    PluginState& m_plugin_state;
    GoPlayAlong m_goplayalong;
    Reaper m_reaper;

    GoPlayAlongState m_prev_goplayalong_state;
    GoPlayAlongState m_goplayalong_state;

    std::array<double, DESYNC_WINDOW_SIZE> m_desync_window = {0.0};

    int m_not_advancing_ticks = 0;
    int m_debug_ticks = 0;

    std::string m_last_error;
};

Plugin::Plugin(PluginState& plugin_state)
    : m_impl(std::make_unique<Impl>(plugin_state))
{}

Plugin::~Plugin() = default;

void Plugin::MainLoop()
{
    m_impl->MainLoop();
}

}
