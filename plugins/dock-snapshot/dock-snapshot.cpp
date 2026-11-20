#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include <drm_fourcc.h>
extern "C"
{
#include <wlr/render/wlr_texture.h>
}

#include <wayfire/core.hpp>
#include <wayfire/img.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/view.hpp>

#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>

namespace
{
std::mutex preview_mutex;
std::unordered_map<std::string, uint64_t> latest_preview;
std::atomic<uint64_t> preview_serial{0};

struct pixels_t
{
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
};

std::optional<pixels_t> read_view_pixels(wayfire_view view)
{
    if (!view || !view->is_mapped() || view->role != wf::VIEW_ROLE_TOPLEVEL ||
        !view->get_output())
    {
        return {};
    }

    pixels_t pixels;
    bool read = false;
    if (!wf::gles::run_in_context_if_gles([&]
    {
        wf::auxilliary_buffer_t snapshot;
        view->take_snapshot(snapshot);
        auto buffer = snapshot.get_renderbuffer().get_buffer();
        auto texture = buffer ? wlr_texture_from_buffer(wf::get_core().renderer, buffer) : nullptr;
        if (!texture)
        {
            return;
        }

        pixels.width = texture->width;
        pixels.height = texture->height;
        pixels.data.resize(size_t(pixels.width) * pixels.height * 4);
        wlr_texture_read_pixels_options options{};
        options.data = pixels.data.data();
        options.format = DRM_FORMAT_ABGR8888;
        options.stride = pixels.width * 4;
        read = wlr_texture_read_pixels(texture, &options);
        wlr_texture_destroy(texture);
    }) || !read)
    {
        return {};
    }

    return pixels;
}

std::optional<std::filesystem::path> preview_directory()
{
    const char *socket = std::getenv("WAYFIRE_SOCKET");
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    struct stat socket_stat{};
    if (!socket || !runtime || ::stat(socket, &socket_stat) != 0)
    {
        return {};
    }

    auto dir = std::filesystem::path(runtime) /
        ("wayfire-dock-previews-" + std::to_string(socket_stat.st_ino));
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
    {
        return {};
    }

    struct stat dir_stat{};
    if (::stat(dir.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode) ||
        dir_stat.st_uid != ::getuid() || (dir_stat.st_mode & 0077))
    {
        return {};
    }

    return dir;
}

std::optional<std::filesystem::path> capture_view(wayfire_view view, const char *kind)
{
    if (!view || !view->is_mapped() || view->role != wf::VIEW_ROLE_TOPLEVEL ||
        !view->get_output())
    {
        return {};
    }

    auto dir = preview_directory();
    if (!dir)
    {
        return {};
    }

    const auto name = std::string(kind) + "-" + std::to_string(view->get_id());
    const auto target = *dir / (name + ".png");
    const auto temporary = *dir / ("." + name + ".tmp-" + std::to_string(::getpid()) + ".png");
    bool rendered = wf::gles::run_in_context_if_gles([&]
    {
        wf::auxilliary_buffer_t snapshot;
        view->take_snapshot(snapshot);
        image_io::write_to_file(temporary.string(), snapshot.get_renderbuffer());
    });

    std::error_code error;
    if (!rendered || !std::filesystem::is_regular_file(temporary, error) ||
        std::filesystem::file_size(temporary, error) == 0 || error)
    {
        std::filesystem::remove(temporary, error);
        return {};
    }

    std::filesystem::rename(temporary, target, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        return {};
    }

    return target;
}

void queue_preview_write(pixels_t pixels, const std::filesystem::path& dir,
    const std::string& name, std::vector<std::future<void>>& pending_writes)
{
    const auto serial = ++preview_serial;
    const auto target = dir / (name + ".png");
    const auto temporary = dir / ("." + name + ".tmp-" +
        std::to_string(::getpid()) + "-" + std::to_string(serial) + ".png");
    {
        std::lock_guard lock(preview_mutex);
        latest_preview[name] = serial;
    }

    for (auto it = pending_writes.begin(); it != pending_writes.end();)
    {
        if (it->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            it = pending_writes.erase(it);
        } else
        {
            ++it;
        }
    }

    pending_writes.emplace_back(std::async(std::launch::async,
        [pixels = std::move(pixels), temporary, target, name, serial] () mutable
    {
        image_io::write_to_file(temporary.string(), pixels.data.data(),
            pixels.width, pixels.height, "png", false);
        std::lock_guard lock(preview_mutex);
        std::error_code error;
        auto current = latest_preview.find(name);
        if (current != latest_preview.end() && current->second == serial)
        {
            latest_preview.erase(current);
            if (std::filesystem::is_regular_file(temporary, error) &&
                std::filesystem::file_size(temporary, error) > 0 && !error)
            {
                std::filesystem::rename(temporary, target, error);
                if (!error)
                {
                    return;
                }
            }
        }

        std::filesystem::remove(temporary, error);
    }));
}

class dock_snapshot_output_t : public wf::per_output_plugin_instance_t
{
    std::vector<std::future<void>> pending_writes;

    wf::signal::connection_t<wf::view_minimize_request_signal> on_minimize = [=]
        (wf::view_minimize_request_signal *event)
    {
        if (event->state && event->view && event->view->get_output() == output)
        {
            auto dir = preview_directory();
            auto pixels = read_view_pixels(event->view);
            if (dir && pixels)
            {
                queue_preview_write(std::move(*pixels), *dir,
                    "view-" + std::to_string(event->view->get_id()), pending_writes);
            }
        }
    };

  public:
    void init() override
    {
        output->connect(&on_minimize);
    }

    void fini() override
    {
        on_minimize.disconnect();
        // The worker executes code from this module, so finish it before unload.
        pending_writes.clear();
    }
};

class dock_snapshot_t : public wf::plugin_interface_t,
    public wf::per_output_tracker_mixin_t<dock_snapshot_output_t>
{
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> repository;
    std::vector<std::future<void>> pending_live_writes;

    wf::ipc::method_callback capture = [=] (const wf::json_t& data)
    {
        auto view = wf::ipc::json_find_view_or_throw(data);
        auto path = capture_view(view, "live");
        if (!path)
        {
            return wf::ipc::json_error("Could not capture view");
        }

        auto result = wf::ipc::json_ok();
        result["path"] = path->string();
        return result;
    };

    wf::ipc::method_callback request = [=] (const wf::json_t& data)
    {
        auto view = wf::ipc::json_find_view_or_throw(data);
        auto dir = preview_directory();
        if (!dir)
        {
            return wf::ipc::json_error("Could not open preview directory");
        }

        const auto name = "live-" + std::to_string(view->get_id());
        const auto target = *dir / (name + ".png");
        const bool refresh = wf::ipc::json_get_bool(data, "refresh");
        bool pending;
        {
            std::lock_guard lock(preview_mutex);
            pending = latest_preview.count(name) != 0;
        }

        if (refresh && !pending)
        {
            auto pixels = read_view_pixels(view);
            if (!pixels)
            {
                return wf::ipc::json_error("Could not capture view");
            }

            queue_preview_write(std::move(*pixels), *dir, name, pending_live_writes);
            pending = true;
        }

        std::error_code error;
        const bool ready = std::filesystem::is_regular_file(target, error) &&
            std::filesystem::file_size(target, error) > 0 && !error;
        auto result = wf::ipc::json_ok();
        result["path"] = target.string();
        result["pending"] = pending;
        result["ready"] = ready;
        return result;
    };

    wf::signal::connection_t<wf::view_unmapped_signal> on_unmapped = [=]
        (wf::view_unmapped_signal *event)
    {
        auto dir = preview_directory();
        if (!dir || !event->view)
        {
            return;
        }

        std::lock_guard lock(preview_mutex);
        const auto view_id = std::to_string(event->view->get_id());
        latest_preview.erase("live-" + view_id);
        latest_preview.erase("view-" + view_id);
        std::error_code error;
        const auto id = view_id + ".png";
        std::filesystem::remove(*dir / ("live-" + id), error);
        std::filesystem::remove(*dir / ("view-" + id), error);
    };

  public:
    void init() override
    {
        repository->register_method("dock-preview/capture", capture);
        repository->register_method("dock-preview/request", request);
        wf::get_core().connect(&on_unmapped);
        init_output_tracking();
    }

    void fini() override
    {
        fini_output_tracking();
        on_unmapped.disconnect();
        repository->unregister_method("dock-preview/capture");
        repository->unregister_method("dock-preview/request");
        pending_live_writes.clear();
    }
};
}

DECLARE_WAYFIRE_PLUGIN(dock_snapshot_t);
