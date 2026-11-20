/*
 * MIT License
 *
 * Copyright (c) 2023 Scott Moreau (Wayfire keycolor renderer template)
 * Copyright (c) 2026 Desktop Settings contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/inotify.h>
#include <unistd.h>

#include <json-c/json.h>
#include <glm/vec4.hpp>

#include <wayfire/core.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/option-wrapper.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/workarea.hpp>

namespace
{
const std::string transformer_name = "rounded-windows";
const std::string program_name = "rounded-windows-program";

std::string palette_directory()
{
    if (const char *xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    {
        return std::string(xdg) + "/desktop-settings/appearance/colors";
    }

    if (const char *home = getenv("HOME"); home && *home)
    {
        return std::string(home) + "/.config/desktop-settings/appearance/colors";
    }

    return {};
}

bool parse_hex_color(const std::string& text, wf::color_t& color)
{
    if (text.size() != 7 || text[0] != '#')
    {
        return false;
    }

    try
    {
        for (size_t i = 1; i < text.size(); ++i)
        {
            if (!std::isxdigit(static_cast<unsigned char>(text[i])))
            {
                return false;
            }
        }

        color = {std::stoi(text.substr(1, 2), nullptr, 16) / 255.0,
            std::stoi(text.substr(3, 2), nullptr, 16) / 255.0,
            std::stoi(text.substr(5, 2), nullptr, 16) / 255.0, 1.0};
        return true;
    } catch (...)
    {
        return false;
    }
}

wf::color_t palette_accent()
{
    wf::color_t fallback{0.0, 0.568627, 1.0, 1.0};
    const std::string directory = palette_directory();
    if (directory.empty())
    {
        return fallback;
    }

    json_object *root = json_object_from_file((directory + "/current.json").c_str());
    if (!root)
    {
        return fallback;
    }

    json_object *colors = nullptr;
    json_object *accent = nullptr;
    if (json_object_object_get_ex(root, "colors", &colors) &&
        json_object_object_get_ex(colors, "accent", &accent) &&
        json_object_is_type(accent, json_type_string))
    {
        parse_hex_color(json_object_get_string(accent), fallback);
    }

    json_object_put(root);
    return fallback;
}

bool should_square_corners(wayfire_view view)
{
    auto toplevel = wf::toplevel_cast(view);
    if (!toplevel)
    {
        return false;
    }

    const auto& state = toplevel->toplevel()->current();
    if (state.fullscreen)
    {
        return true;
    }

    // Tiled views have all tiled edges too, but gaps expose their corners.
    // Only a window covering the whole work area should have square corners.
    if (state.tiled_edges == wf::TILED_EDGES_ALL && view->get_output())
    {
        const auto workarea = view->get_output()->workarea->get_workarea();
        const auto geometry = toplevel->get_geometry();
        return geometry.width >= workarea.width &&
            geometry.height >= workarea.height;
    }

    return false;
}

const char *vertex_shader = R"(
#version 100
attribute highp vec2 position;
attribute highp vec2 texcoord;
varying highp vec2 uvpos;
uniform mat4 mvp;
void main() {
    gl_Position = mvp * vec4(position, 0.0, 1.0);
    uvpos = texcoord;
}
)";

const char *fragment_shader = R"(
#version 100
@builtin_ext@
@builtin@
precision highp float;
varying highp vec2 uvpos;
uniform highp vec2 size;
uniform highp vec2 content_size;
uniform highp vec2 content_offset;
uniform highp vec2 border_size;
uniform highp vec2 border_offset;
uniform highp float radius;
uniform highp float border_radius;
uniform highp float border_width;
uniform lowp vec4 border_color;
void main() {
    vec2 content_pos = uvpos * size - content_offset;
    vec4 pixel = vec4(0.0);
    if (content_pos.x >= 0.0 && content_pos.y >= 0.0 &&
        content_pos.x <= content_size.x && content_pos.y <= content_size.y) {
        vec2 window_pos = uvpos * size - border_offset;
        // The cached image can include client-side shadows outside the xdg
        // window geometry. Clip the image to the same box as the outer border.
        if (window_pos.x >= 0.0 && window_pos.y >= 0.0 &&
            window_pos.x <= border_size.x && window_pos.y <= border_size.y) {
            pixel = get_pixel(content_pos / content_size);
            vec2 edge = min(window_pos, border_size - window_pos);
            if (edge.x < radius && edge.y < radius) {
                float distance_to_center = length(edge - vec2(radius));
                pixel *= 1.0 - smoothstep(radius - 1.0, radius + 0.5,
                    distance_to_center);
            }
        }
    }
    if (border_width > 0.0) {
        vec2 border_pos = uvpos * size - border_offset;
        vec2 half_size = border_size * 0.5;
        vec2 q = abs(border_pos - half_size) - (half_size - vec2(border_radius));
        float distance_to_edge = length(max(q, vec2(0.0))) +
            min(max(q.x, q.y), 0.0) - border_radius;
        float border_coverage = smoothstep(-0.75, 0.25, distance_to_edge) *
            (1.0 - smoothstep(border_width - 0.75,
                border_width + 0.25, distance_to_edge));
        pixel = mix(pixel, vec4(border_color.rgb, 1.0),
            border_coverage * border_color.a);
    }
    gl_FragColor = pixel;
}
)";

class renderer_data_t : public wf::custom_data_t
{
  public:
    OpenGL::program_t program;
    wf::color_t border_color{0.0, 0.568627, 1.0, 1.0};
};

class rounded_transformer_t : public wf::scene::view_2d_transformer_t,
    public wf::scene::opaque_region_node_t
{
    wayfire_view view;
    wf::option_wrapper_t<int> radius{"panel-decor/corner_radius"};
    wf::option_wrapper_t<int> border_width{"rounded-windows/active_border_width"};
    wf::signal::connection_t<wf::view_activated_state_signal> on_activated = [this] (auto)
    {
        damage_border_change();
    };
    wf::signal::connection_t<wf::view_fullscreen_signal> on_fullscreen = [this] (auto)
    {
        damage_border_change();
    };
    wf::signal::connection_t<wf::view_tiled_signal> on_tiled = [this] (auto)
    {
        damage_border_change();
    };

    wf::geometry_t padded_border_geometry() const
    {
        const auto box = border_geometry();
        const double padding = std::max(0, int(border_width));
        return {box.x - padding, box.y - padding,
            box.width + 2 * padding, box.height + 2 * padding};
    }

    void damage_border_change()
    {
        // Activation changes the visible extent of the outside border. Damage
        // both extents and refresh visibility when the bounding box shrinks.
        wf::scene::damage_node(view->get_root_node(), padded_border_geometry());
        wf::scene::update(shared_from_this(), wf::scene::update_flag::GEOMETRY);
    }

    class render_instance_t : public wf::scene::transformer_render_instance_t<wf::scene::transformer_base_node_t>
    {
        rounded_transformer_t *owner;
        wayfire_view view;

      public:
        render_instance_t(rounded_transformer_t *owner,
            wf::scene::damage_callback push_damage, wayfire_view view) :
            wf::scene::transformer_render_instance_t<wf::scene::transformer_base_node_t>(
                owner, push_damage, view->get_output()),
            owner(owner), view(view)
        {}

        void render(const wf::scene::render_instruction_t& data) override
        {
            if (data.damage.empty())
            {
                return;
            }

            auto visual = owner->get_children_bounding_box();
            if (visual.width <= 0 || visual.height <= 0)
            {
                return;
            }

            auto fb_geometry = data.target.framebuffer_box_from_geometry_box(data.target.geometry);
            auto content_box = data.target.framebuffer_box_from_geometry_box(visual);
            auto border_box = data.target.framebuffer_box_from_geometry_box(owner->border_geometry());
            auto box = data.target.framebuffer_box_from_geometry_box(owner->get_bounding_box());
            const int content_offset_x = content_box.x - box.x;
            const int content_offset_y = box.height - (content_box.y - box.y) - content_box.height;
            const int border_offset_x = border_box.x - box.x;
            const int border_offset_y = box.height - (border_box.y - box.y) - border_box.height;
            box.x -= fb_geometry.x;
            box.y -= fb_geometry.y;

            auto shader_data = wf::get_core().get_data<renderer_data_t>(program_name);
            if (!shader_data)
            {
                return;
            }

            float effective_radius = std::clamp(float(owner->radius * data.target.scale),
                0.0f, float(std::min(border_box.width, border_box.height)) / 2.0f);
            if (should_square_corners(view))
            {
                effective_radius = 0;
            }
            const auto toplevel = wf::toplevel_cast(view);
            const float effective_border_width = toplevel && toplevel->activated &&
                !should_square_corners(view) ?
                std::clamp(float(owner->border_width * data.target.scale),
                    0.0f, float(std::min(border_box.width, border_box.height)) / 2.0f) : 0.0f;

            static const float vertices[] = {
                -1.0f, -1.0f, 1.0f, -1.0f,
                1.0f, 1.0f, -1.0f, 1.0f
            };
            static const float texcoords[] = {
                0.0f, 0.0f, 1.0f, 0.0f,
                1.0f, 1.0f, 0.0f, 1.0f
            };

            // Translucent client shadows can change without their outer
            // pixels being included in the surface damage. Repaint only
            // the non-opaque portion of the cached view before sampling it.
            wf::regionf_t translucent{visual};
            translucent &= owner->border_geometry();
            const auto& children = owner->get_children();
            if (children.size() == 1)
            {
                if (auto opaque = dynamic_cast<wf::scene::opaque_region_node_t*>(children.front().get()))
                {
                    translucent ^= opaque->get_opaque_region();
                }
            }

            owner->cached_damage |= translucent;
            // The child pass can disable blending when it draws opaque
            // surfaces. Enter the GLES subpass after it finishes so the
            // rounded mask is composited over the background.
            auto contents = get_texture(data.target.scale);
            data.pass->custom_gles_subpass([&]
            {
                auto texture = wf::gles_texture_t{contents};
                shader_data->program.use(texture.type);
                shader_data->program.uniform2f("size", box.width, box.height);
                shader_data->program.uniform2f("content_size", content_box.width, content_box.height);
                shader_data->program.uniform2f("content_offset", content_offset_x, content_offset_y);
                shader_data->program.uniform2f("border_size", border_box.width, border_box.height);
                shader_data->program.uniform2f("border_offset", border_offset_x, border_offset_y);
                shader_data->program.uniform1f("radius", effective_radius);
                shader_data->program.uniform1f("border_radius", effective_radius);
                shader_data->program.uniform1f("border_width", effective_border_width);
                const auto& color = shader_data->border_color;
                shader_data->program.uniform4f("border_color",
                    glm::vec4(color.r, color.g, color.b, color.a));
                shader_data->program.attrib_pointer("position", 2, 0, vertices);
                shader_data->program.attrib_pointer("texcoord", 2, 0, texcoords);
                shader_data->program.uniformMatrix4f("mvp", wf::gles::output_transform(data.target));
                GL_CALL(glActiveTexture(GL_TEXTURE0));
                shader_data->program.set_active_texture(texture);
                wf::gles::bind_render_buffer(data.target);
                GL_CALL(glViewport(box.x, box.y, box.width, box.height));
                wf::gles::for_each_scissor_rect(data.target, data.damage, [&]
                {
                    GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
                });
                GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
                shader_data->program.deactivate();
            });
        }
    };

  public:
    rounded_transformer_t(wayfire_view view) : wf::scene::view_2d_transformer_t(view), view(view)
    {
        view->connect(&on_activated);
        view->connect(&on_fullscreen);
        view->connect(&on_tiled);
    }

    wf::geometry_t border_geometry() const
    {
        if (auto toplevel = wf::toplevel_cast(view))
        {
            const auto geometry = toplevel->get_geometry();
            if (geometry.width > 0 && geometry.height > 0)
            {
                return geometry;
            }
        }
        return get_children_bounding_box();
    }

    wf::geometry_t get_bounding_box() override
    {
        // Client shadows outside the window geometry have already been clipped
        // by the shader. Exposing them (or an invisible inactive border) here
        // lets outer effects such as backdrop blur redraw a transparent halo.
        const auto toplevel = wf::toplevel_cast(view);
        if (toplevel && toplevel->activated && !should_square_corners(view))
        {
            return padded_border_geometry();
        }

        return border_geometry();
    }

    wf::regionf_t get_opaque_region() const override
    {
        const auto& children = get_children();
        if (children.size() != 1)
        {
            return {};
        }

        auto child = dynamic_cast<wf::scene::opaque_region_node_t*>(children.front().get());
        if (!child)
        {
            return {};
        }

        auto box = border_geometry();
        auto region = child->get_opaque_region() & box;
        double corner = std::clamp(double(radius), 0.0, std::min(box.width, box.height) / 2.0);
        if (should_square_corners(view))
        {
            corner = 0;
        }

        if (corner > 0)
        {
            // The shader only changes alpha inside the corner squares. Their
            // complete removal is conservative because most corner pixels stay opaque.
            region ^= wf::geometry_t{box.x, box.y, corner, corner};
            region ^= wf::geometry_t{box.x + box.width - corner, box.y, corner, corner};
            region ^= wf::geometry_t{box.x, box.y + box.height - corner, corner, corner};
            region ^= wf::geometry_t{box.x + box.width - corner,
                box.y + box.height - corner, corner, corner};
        }

        return region;
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *) override
    {
        instances.push_back(std::make_unique<render_instance_t>(this, push_damage, view));
    }
};

class rounded_windows_t : public wf::plugin_interface_t
{
    wf::option_wrapper_t<int> radius{"panel-decor/corner_radius"};
    wf::option_wrapper_t<int> border_width{"rounded-windows/active_border_width"};
    wf::option_wrapper_t<std::string> border_color{"rounded-windows/active_border_color"};
    int palette_fd = -1;
    int palette_watch_fd = -1;
    wl_event_source *palette_source = nullptr;

    void reload_border_color()
    {
        if (auto data = wf::get_core().get_data<renderer_data_t>(program_name))
        {
            const std::string selected = border_color;
            auto color = selected == "auto" ? palette_accent() : wf::color_t{};
            if (selected != "auto" && !parse_hex_color(selected, color))
            {
                color = palette_accent();
            }
            data->border_color = color;
            for (auto view : wf::get_core().get_all_views())
            {
                if (view)
                {
                    if (auto transformer = view->get_transformed_node()
                        ->get_transformer<rounded_transformer_t>(transformer_name))
                    {
                        wf::scene::damage_node(view->get_root_node(), transformer->get_bounding_box());
                    }
                }
            }
        }
    }

    static int on_palette_event(int fd, uint32_t, void *user_data)
    {
        auto self = static_cast<rounded_windows_t*>(user_data);
        char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
        const ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length <= 0)
        {
            return 0;
        }

        for (ssize_t offset = 0; offset + ssize_t(sizeof(inotify_event)) <= length;)
        {
            const auto event = reinterpret_cast<const inotify_event*>(buffer + offset);
            const ssize_t event_size = sizeof(inotify_event) + event->len;
            if (offset + event_size > length)
            {
                break;
            }

            if (event->wd == self->palette_watch_fd && event->len &&
                std::strcmp(event->name, "current.json") == 0 &&
                (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)))
            {
                self->reload_border_color();
            }
            offset += event_size;
        }
        return 0;
    }

    void watch_palette()
    {
        const auto directory = palette_directory();
        if (directory.empty())
        {
            return;
        }

        palette_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (palette_fd < 0)
        {
            return;
        }
        palette_watch_fd = inotify_add_watch(palette_fd, directory.c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (palette_watch_fd < 0)
        {
            close(palette_fd);
            palette_fd = -1;
            return;
        }

        auto event_loop = wl_display_get_event_loop(wf::get_core().display);
        palette_source = wl_event_loop_add_fd(event_loop, palette_fd,
            WL_EVENT_READABLE, on_palette_event, this);
    }

    void stop_watching_palette()
    {
        if (palette_source)
        {
            wl_event_source_remove(palette_source);
            palette_source = nullptr;
        }
        if (palette_watch_fd >= 0)
        {
            inotify_rm_watch(palette_fd, palette_watch_fd);
            palette_watch_fd = -1;
        }
        if (palette_fd >= 0)
        {
            close(palette_fd);
            palette_fd = -1;
        }
    }

    void update_view(wayfire_view view)
    {
        if (!view || !view->is_mapped() || view->role != wf::VIEW_ROLE_TOPLEVEL)
        {
            return;
        }

        auto node = view->get_transformed_node();
        if (auto previous = node->get_transformer<rounded_transformer_t>(transformer_name))
        {
            // The option already contains its new value here. Include the
            // previous outside ring when a user reduces the configured width.
            auto damage = previous->get_bounding_box();
            damage.x -= 12;
            damage.y -= 12;
            damage.width += 24;
            damage.height += 24;
            wf::scene::damage_node(view->get_root_node(), damage);
        }
        if ((radius > 0 || border_width > 0) && !node->get_transformer(transformer_name))
        {
            auto transformer = std::make_shared<rounded_transformer_t>(view);
            node->add_transformer(transformer, wf::TRANSFORMER_2D, transformer_name);
        } else if (radius == 0 && border_width == 0 && node->get_transformer(transformer_name))
        {
            node->rem_transformer(transformer_name);
        }

        if (auto current = node->get_transformer<rounded_transformer_t>(transformer_name))
        {
            wf::scene::damage_node(view->get_root_node(), current->get_bounding_box());
        } else
        {
            view->damage();
        }
    }

    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *event)
    {
        update_view(event->view);
    };

  public:
    void init() override
    {
        if (!wf::get_core().is_gles2())
        {
            LOGE("rounded-windows requires the OpenGL ES renderer");
            return;
        }

        auto data = std::make_unique<renderer_data_t>();
        wf::gles::run_in_context([&]
        {
            data->program.compile(vertex_shader, fragment_shader);
        });
        wf::get_core().store_data(std::move(data), program_name);
        wf::get_core().connect(&on_view_mapped);
        reload_border_color();
        watch_palette();
        radius.set_callback([=] ()
        {
            for (auto view : wf::get_core().get_all_views())
            {
                update_view(view);
            }
        });
        border_width.set_callback([=] ()
        {
            for (auto view : wf::get_core().get_all_views())
            {
                update_view(view);
            }
        });
        border_color.set_callback([=] ()
        {
            reload_border_color();
        });
        for (auto view : wf::get_core().get_all_views())
        {
            update_view(view);
        }
        // A previous clipping geometry can leave pixels in backdrop effects'
        // cached output outside the new bounds. Repaint the scene once when
        // this plugin is hot-reloaded so the background reaches the corners.
        wf::scene::damage_node(wf::get_core().scene(),
            wf::get_core().scene()->get_bounding_box());
    }

    void fini() override
    {
        stop_watching_palette();
        for (auto view : wf::get_core().get_all_views())
        {
            if (view && view->get_transformed_node()->get_transformer(transformer_name))
            {
                view->get_transformed_node()->rem_transformer(transformer_name);
                view->damage();
            }
        }
        wf::scene::damage_node(wf::get_core().scene(),
            wf::get_core().scene()->get_bounding_box());
        auto data = wf::get_core().get_data<renderer_data_t>(program_name);
        if (data)
        {
            wf::gles::run_in_context_if_gles([&]
            {
                data->program.free_resources();
            });
            wf::get_core().erase_data(program_name);
        }
    }
};
}

extern "C" __attribute__((visibility("default"))) wf::plugin_interface_t *newInstance()
{
    return new rounded_windows_t;
}

extern "C" __attribute__((visibility("default"))) uint32_t getWayfireVersion()
{
    return WAYFIRE_API_ABI_VERSION;
}
