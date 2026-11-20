#include <wayfire/core.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wlr/types/wlr_xdg_shell.h>
#include <wayfire/unstable/wlr-view-events.hpp>
#include <wayfire/view.hpp>
#include <wayfire/window-manager.hpp>
#include <wlr/types/wlr_seat.h>
#include <string>

namespace
{
class tile_focus_t : public wf::plugin_interface_t
{
    // The panel sets this to "all" on tiled workspaces and "none" on floating ones.
    wf::view_matcher_t tiled_workspace{"simple-tile/tile_by_default"};

    wf::signal::connection_t<wf::view_pre_map_signal> on_view_pre_map =
        [this] (wf::view_pre_map_signal *event)
    {
        auto view = wf::toplevel_cast(event->view->self());
        if (!view || view->parent || !tiled_workspace.matches(view) ||
            view->get_property<std::string>("open-animation-type"))
        {
            return;
        }

        const auto min = view->toplevel()->get_min_size();
        const auto max = view->toplevel()->get_max_size();
        if ((min == max) && min.width > 0 && min.height > 0)
        {
            return;
        }

        // Zoom and Simple Tile's geometry crossfade compound during map.
        // Keep the layout transition and use the cheaper opening fade.
        view->set_property<std::string>("open-animation-type", "fade");
    };

    wf::signal::connection_t<wf::pointer_focus_changed_signal> on_pointer_focus =
        [this] (wf::pointer_focus_changed_signal *event)
    {
        auto& core = wf::get_core();
        auto view = wf::toplevel_cast(wf::node_to_view(event->new_focus));
        if (!view || !view->is_mapped() || view->parent || view->minimized ||
            !view->get_keyboard_focus_surface() || !tiled_workspace.matches(view) ||
            (view->get_output() != core.seat->get_active_output()) ||
            (core.seat->get_active_view() == view))
        {
            return;
        }

        // A drag must keep its original keyboard focus. Also leave keyboard
        // grabs, such as the window switcher and lock screen, alone.
        if (core.seat->seat->pointer_state.button_count ||
            (core.seat->get_active_node() &&
             !wf::node_to_view(core.seat->get_active_node())))
        {
            return;
        }

        core.default_wm->focus_request(view);
    };

  public:
    void init() override
    {
        wf::get_core().connect(&on_view_pre_map);
        wf::get_core().connect(&on_pointer_focus);
    }

    void fini() override
    {
        on_view_pre_map.disconnect();
        on_pointer_focus.disconnect();
    }
};
}

extern "C" __attribute__((visibility("default"))) wf::plugin_interface_t *newInstance()
{
    return new tile_focus_t;
}

extern "C" __attribute__((visibility("default"))) uint32_t getWayfireVersion()
{
    return WAYFIRE_API_ABI_VERSION;
}
