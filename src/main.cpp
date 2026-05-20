#define REAPERAPI_IMPLEMENT

#include "plugin.h"

#include <WDL/wdltypes.h> // Must be included before reaper_plugin_functions
#include <reaper_plugin_functions.h>

using namespace tnt;

static PluginState g_plugin_state;
static Plugin g_plugin(g_plugin_state);

void MainLoop()
{
    g_plugin.MainLoop();
}

int ToggleActionCallback(int command)
{
    if (command != g_plugin_state.command_id)
    {
        return -1;
    }

    return g_plugin_state.action_state ? 1 : 0;
}

bool OnAction(KbdSectionInfo* sec, int command, int val, int valhw, int relmode, HWND hwnd)
{
    if (command != g_plugin_state.command_id)
    {
        return false;
    }

    g_plugin_state.action_state = !g_plugin_state.action_state;

    if (g_plugin_state.action_state)
    {
        plugin_register("timer", (void*)MainLoop);
    }
    else
    {
        plugin_register("-timer", (void*)MainLoop);
    }

    return true;
}

void Register()
{
    g_plugin_state.command_id = plugin_register("custom_action", &g_plugin_state.action);
    plugin_register("toggleaction", (void*)ToggleActionCallback);
    plugin_register("hookcommand2", (void*)OnAction);
}

void Unregister()
{
    plugin_register("-custom_action", &g_plugin_state.action);
    plugin_register("-toggleaction", (void*)ToggleActionCallback);
    plugin_register("-hookcommand2", (void*)OnAction);
}

extern "C"
{
    REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hinstance, reaper_plugin_info_t* rec)
    {
        g_plugin_state.hinstance = hinstance;

        if (rec != nullptr)
        {
            REAPERAPI_LoadAPI(rec->GetFunc);
            Register();
            return 1;
        }

        Unregister();
        return 0;
    }
}
