#include "reaper.h"

#include <WDL/wdltypes.h> // Must be included before reaper_plugin_functions
#include <reaper_plugin_functions.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace tnt {

static constexpr int PRESERVE_PITCH_COMMAND = 40671;

struct Reaper::Impl final
{
    double GetOutputLatency() const
    {
        return ::GetOutputLatency();
    }

    double GetPlayPosition() const
    {
        return ::GetPlayPosition();
    }

    double GetPlayRate() const
    {
        return ::Master_GetPlayRate(nullptr);
    }

    ReaperPlayState GetPlayState() const
    {
        const int play_state = ::GetPlayState();
        if (play_state & 2)
        {
            return ReaperPlayState::PAUSED;
        }
        else if (play_state & 1)
        {
            return ReaperPlayState::PLAYING;
        }
        else
        {
            return ReaperPlayState::STOPPED;
        }
    }

    bool GetToggleCommandState(const ReaperToggleCommand& command) const
    {
        switch (command)
        {
        case ReaperToggleCommand::PRESERVE_PITCH:
            return ::GetToggleCommandState(PRESERVE_PITCH_COMMAND) != 0;
        default:
            throw std::runtime_error("GetToggleCommandState: command not found.\n");
        }
    }

    void SetEditCursorPosition(const double time, const bool move_view, const bool seek_play) const
    {
        ::SetEditCurPos(time, move_view, seek_play);
    }

    void SetPlayRate(const double play_rate) const
    {
        ::CSurf_OnPlayRateChange(play_rate);
    }

    void SetPlayState(const ReaperPlayState& play_state) const
    {
        switch (play_state)
        {
        case ReaperPlayState::STOPPED:
            ::CSurf_OnStop();
            break;
        case ReaperPlayState::PLAYING:
            ::CSurf_OnPlay();
            break;
        case ReaperPlayState::PAUSED:
            ::CSurf_OnPause();
            break;
        default:
            throw std::runtime_error("SetPlayState: invalid play state.\n");
        }
    }

    void SetRepeat(const bool repeat) const
    {
        ::GetSetRepeat(repeat ? 1 : 0);
    }

    void SetTimeSelection(const double start_time, const double end_time) const
    {
        ::GetSet_LoopTimeRange(true, false, const_cast<double*>(&start_time), const_cast<double*>(&end_time), false);
    }

    void ShowConsoleMessage(const std::string& message) const
    {
        ::ShowConsoleMsg(message.c_str());
    }

    void ToggleCommand(const ReaperToggleCommand& command) const
    {
        switch (command)
        {
        case ReaperToggleCommand::PRESERVE_PITCH:
            ::Main_OnCommand(PRESERVE_PITCH_COMMAND, 0);
            break;
        default:
            throw std::runtime_error("ToggleCommand: command not found.\n");
        }
    }
};

Reaper::Reaper()
    : m_impl(std::make_unique<Impl>())
{}

Reaper::~Reaper() = default;

double Reaper::GetOutputLatency() const { return m_impl->GetOutputLatency(); }
double Reaper::GetPlayPosition() const { return m_impl->GetPlayPosition(); }
double Reaper::GetPlayRate() const { return m_impl->GetPlayRate(); }
ReaperPlayState Reaper::GetPlayState() const { return m_impl->GetPlayState(); }

bool Reaper::GetToggleCommandState(const ReaperToggleCommand& command) const
{
    return m_impl->GetToggleCommandState(command);
}

void Reaper::SetEditCursorPosition(const double time, const bool move_view, const bool seek_play) const
{
    m_impl->SetEditCursorPosition(time, move_view, seek_play);
}

void Reaper::SetPlayRate(const double play_rate) const { m_impl->SetPlayRate(play_rate); }
void Reaper::SetPlayState(const ReaperPlayState& play_state) const { m_impl->SetPlayState(play_state); }
void Reaper::SetRepeat(const bool repeat) const { m_impl->SetRepeat(repeat); }

void Reaper::SetTimeSelection(const double start_time, const double end_time) const
{
    m_impl->SetTimeSelection(start_time, end_time);
}

void Reaper::ShowConsoleMessage(const std::string& message) const { m_impl->ShowConsoleMessage(message); }

void Reaper::ToggleCommand(const ReaperToggleCommand& command) const { m_impl->ToggleCommand(command); }

}
