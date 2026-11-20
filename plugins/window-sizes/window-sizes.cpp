#include <wayfire/core.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/toplevel.hpp>
#include <wayfire/txn/transaction-manager.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/workarea.hpp>

#include <json-c/json.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
struct window_size_t
{
    int width = 0;
    int height = 0;
};

struct remembered_t
{
    window_size_t initial;
    window_size_t last;
};

struct open_view_t
{
    std::string app_id;
    window_size_t initial;
    window_size_t observed;
    std::optional<window_size_t> resized = std::nullopt;
    bool mapped = false;
    std::chrono::steady_clock::time_point mapped_at{};
};

bool valid(window_size_t size)
{
    return size.width >= 32 && size.height >= 32 &&
        size.width <= 8192 && size.height <= 8192;
}

window_size_t size_of(const wf::geometry_t& geometry)
{
    return {int(std::lround(geometry.width)), int(std::lround(geometry.height))};
}

bool same(window_size_t a, window_size_t b)
{
    return a.width == b.width && a.height == b.height;
}

double distance(window_size_t a, window_size_t b)
{
    if (!valid(a) || !valid(b))
    {
        return INFINITY;
    }

    return std::max(std::abs(std::log(double(a.width) / b.width)),
        std::abs(std::log(double(a.height) / b.height)));
}

std::filesystem::path state_path()
{
    const char *base = std::getenv("XDG_STATE_HOME");
    if (base && *base)
    {
        return std::filesystem::path(base) / "wayfire-window-sizes" / "sizes.json";
    }

    const char *home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/tmp") /
        ".local/state/wayfire-window-sizes/sizes.json";
}

class window_sizes_plugin_t : public wf::plugin_interface_t
{
    std::map<std::string, std::vector<remembered_t>> sizes;
    std::map<uint64_t, open_view_t> open_views;
    std::filesystem::path file = state_path();
    wf::view_matcher_t tile_by_default{"simple-tile/tile_by_default"};

    bool eligible(wayfire_toplevel_view view) const
    {
        if (!view || view->parent || view->get_app_id().empty())
        {
            return false;
        }

        const auto minimum = view->toplevel()->get_min_size();
        const auto maximum = view->toplevel()->get_max_size();
        return !(minimum.width > 0 && minimum.height > 0 &&
            minimum == maximum);
    }

    bool floating(wayfire_toplevel_view view) const
    {
        return !view->pending_fullscreen() && (view->pending_tiled_edges() == 0);
    }

    std::optional<window_size_t> matching_size(const std::string& app_id, window_size_t initial) const
    {
        auto it = sizes.find(app_id);
        if (it == sizes.end())
        {
            return {};
        }

        double best = std::log(1.45);
        std::optional<window_size_t> found;
        for (const auto& record : it->second)
        {
            const double score = std::min(distance(initial, record.initial),
                distance(initial, record.last));
            if (score < best)
            {
                best  = score;
                found = record.last;
            }
        }

        return found;
    }

    window_size_t fit_to_output(wayfire_toplevel_view view, window_size_t wanted) const
    {
        if (auto output = view->get_output())
        {
            const auto workarea = output->workarea->get_workarea();
            wanted.width  = std::min(wanted.width, std::max(32, int(workarea.width)));
            wanted.height = std::min(wanted.height, std::max(32, int(workarea.height)));
        }

        const auto minimum = view->toplevel()->get_min_size();
        const auto maximum = view->toplevel()->get_max_size();
        if (minimum.width > 0)
        {
            wanted.width = std::max(wanted.width, minimum.width);
        }

        if (minimum.height > 0)
        {
            wanted.height = std::max(wanted.height, minimum.height);
        }

        if (maximum.width > 0)
        {
            wanted.width = std::min(wanted.width, maximum.width);
        }

        if (maximum.height > 0)
        {
            wanted.height = std::min(wanted.height, maximum.height);
        }

        return wanted;
    }

    static std::optional<window_size_t> read_size(json_object *object, const char *prefix)
    {
        json_object *width = nullptr, *height = nullptr;
        if (!json_object_object_get_ex(object, (std::string(prefix) + "width").c_str(), &width) ||
            !json_object_object_get_ex(object, (std::string(prefix) + "height").c_str(), &height) ||
            !json_object_is_type(width, json_type_int) ||
            !json_object_is_type(height, json_type_int))
        {
            return {};
        }

        window_size_t result{json_object_get_int(width), json_object_get_int(height)};
        return valid(result) ? std::optional<window_size_t>{result} : std::nullopt;
    }

    void load()
    {
        json_object *root = json_object_from_file(file.c_str());
        if (!root)
        {
            return;
        }

        json_object *apps = nullptr;
        if (json_object_object_get_ex(root, "apps", &apps) &&
            json_object_is_type(apps, json_type_object))
        {
            json_object_object_foreach(apps, app_id, records)
            {
                if (!json_object_is_type(records, json_type_array))
                {
                    continue;
                }

                auto& list = sizes[app_id];
                for (std::size_t i = 0; i < std::min<std::size_t>(8,
                    json_object_array_length(records)); i++)
                {
                    auto entry = json_object_array_get_idx(records, i);
                    if (!entry || !json_object_is_type(entry, json_type_object))
                    {
                        continue;
                    }

                    auto initial = read_size(entry, "initial_");
                    auto last    = read_size(entry, "");
                    if (initial && last)
                    {
                        list.push_back({*initial, *last});
                    }
                }
            }
        }

        json_object_put(root);
    }

    void save() const
    {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        if (error)
        {
            return;
        }

        json_object *root = json_object_new_object();
        json_object *apps = json_object_new_object();
        json_object_object_add(root, "version", json_object_new_int(1));
        json_object_object_add(root, "apps", apps);
        for (const auto& [app_id, records] : sizes)
        {
            json_object *array = json_object_new_array();
            for (const auto& record : records)
            {
                json_object *entry = json_object_new_object();
                json_object_object_add(entry, "initial_width", json_object_new_int(record.initial.width));
                json_object_object_add(entry, "initial_height", json_object_new_int(record.initial.height));
                json_object_object_add(entry, "width", json_object_new_int(record.last.width));
                json_object_object_add(entry, "height", json_object_new_int(record.last.height));
                json_object_array_add(array, entry);
            }

            json_object_object_add(apps, app_id.c_str(), array);
        }

        std::string temporary = file.string() + ".XXXXXX";
        std::vector<char> name(temporary.begin(), temporary.end());
        name.push_back('\0');
        int fd = mkstemp(name.data());
        if (fd >= 0)
        {
            FILE *stream = fdopen(fd, "w");
            if (stream)
            {
                const char *contents = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
                const bool written = std::fputs(contents, stream) >= 0;
                const bool closed  = std::fclose(stream) == 0;
                if (written && closed)
                {
                    std::filesystem::rename(name.data(), file, error);
                    if (error)
                    {
                        std::filesystem::remove(name.data(), error);
                    }
                } else
                {
                    std::filesystem::remove(name.data(), error);
                }
            } else
            {
                close(fd);
                std::filesystem::remove(name.data(), error);
            }
        }

        json_object_put(root);
    }

    void remember(const open_view_t& open)
    {
        if (!open.resized || !valid(*open.resized) || !valid(open.initial))
        {
            return;
        }

        auto& records = sizes[open.app_id];
        auto match = std::find_if(records.begin(), records.end(), [&] (const auto& record)
        {
            return distance(open.initial, record.initial) < std::log(1.45);
        });
        if (match == records.end())
        {
            records.push_back({open.initial, *open.resized});
        } else
        {
            *match = {open.initial, *open.resized};
        }

        if (records.size() > 8)
        {
            records.erase(records.begin());
        }

        save();
    }

    wf::signal::connection_t<wf::txn::new_transaction_signal> on_new_tx =
        [=] (wf::txn::new_transaction_signal *event)
    {
        for (const auto& object : event->tx->get_objects())
        {
            auto toplevel = std::dynamic_pointer_cast<wf::toplevel_t>(object);
            if (!toplevel || toplevel->current().mapped || !toplevel->pending().mapped)
            {
                continue;
            }

            auto view = wf::toplevel_cast(wf::find_view_for_toplevel(toplevel));
            if (!eligible(view) || !floating(view) || tile_by_default.matches(view) ||
                open_views.count(view->get_id()))
            {
                continue;
            }

            const auto initial = size_of(toplevel->pending().geometry);
            if (!valid(initial))
            {
                continue;
            }

            open_view_t open{view->get_app_id(), initial, initial};
            if (auto restored = matching_size(open.app_id, initial))
            {
                auto wanted = fit_to_output(view, *restored);
                toplevel->pending().geometry.width  = wanted.width;
                toplevel->pending().geometry.height = wanted.height;
                if (auto output = view->get_output(); output &&
                    !view->has_property("startup-x") && !view->has_property("startup-y"))
                {
                    // Place runs earlier in the map transaction and centered
                    // the original dimensions. Center the restored dimensions.
                    const auto area = output->workarea->get_workarea();
                    const double scale = output->handle->scale;
                    toplevel->pending().geometry.x =
                        std::round((area.x + (area.width - wanted.width) / 2.0) * scale) / scale;
                    toplevel->pending().geometry.y =
                        std::round((area.y + (area.height - wanted.height) / 2.0) * scale) / scale;
                }

                open.observed = wanted;
            }

            open_views[view->get_id()] = std::move(open);
        }
    };

    void track_mapped(wayfire_view raw_view)
    {
        auto view = wf::toplevel_cast(raw_view);
        if (!eligible(view) || !floating(view))
        {
            return;
        }

        auto current = size_of(view->get_pending_geometry());
        auto& open = open_views[view->get_id()];
        if (open.app_id.empty())
        {
            open.app_id  = view->get_app_id();
            open.initial = current;
        }

        open.observed = current;
        open.mapped   = true;
        open.mapped_at = std::chrono::steady_clock::now();
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_mapped =
        [=] (wf::view_mapped_signal *event)
    {
        track_mapped(event->view);
    };

    wf::signal::connection_t<wf::view_geometry_changed_signal> on_geometry =
        [=] (wf::view_geometry_changed_signal *event)
    {
        auto view = event->view;
        auto it   = open_views.find(view->get_id());
        if (it == open_views.end() || !it->second.mapped || !floating(view))
        {
            return;
        }

        auto current = size_of(view->get_pending_geometry());
        if (valid(current) && !same(current, it->second.observed))
        {
            it->second.observed = current;
            // SSD margins and the first client configure can change geometry
            // immediately after mapping without a user resize.
            if (std::chrono::steady_clock::now() - it->second.mapped_at >=
                std::chrono::milliseconds(350))
            {
                it->second.resized = current;
            }
        }
    };

    wf::signal::connection_t<wf::view_app_id_changed_signal> on_app_id_changed =
        [=] (wf::view_app_id_changed_signal *event)
    {
        auto view = wf::toplevel_cast(event->view);
        if (!view || !eligible(view))
        {
            return;
        }

        auto it = open_views.find(view->get_id());
        if (it == open_views.end() && view->is_mapped())
        {
            track_mapped(view);
        } else if (it != open_views.end())
        {
            it->second.app_id = view->get_app_id();
        }
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_unmapped =
        [=] (wf::view_unmapped_signal *event)
    {
        auto it = open_views.find(event->view->get_id());
        if (it != open_views.end())
        {
            remember(it->second);
            open_views.erase(it);
        }
    };

  public:
    void init() override
    {
        load();
        wf::get_core().tx_manager->connect(&on_new_tx);
        wf::get_core().connect(&on_mapped);
        wf::get_core().connect(&on_geometry);
        wf::get_core().connect(&on_app_id_changed);
        wf::get_core().connect(&on_unmapped);

        // The plugin can be loaded while a session is running. Existing
        // floating windows should start recording their next resize too.
        for (auto view : wf::get_core().get_all_views())
        {
            if (view->is_mapped())
            {
                track_mapped(view);
                if (auto it = open_views.find(view->get_id()); it != open_views.end())
                {
                    it->second.mapped_at -= std::chrono::milliseconds(350);
                }
            }
        }
    }

    void fini() override
    {
        for (const auto& [_, open] : open_views)
        {
            remember(open);
        }

        open_views.clear();
    }
};
}

DECLARE_WAYFIRE_PLUGIN(window_sizes_plugin_t);
