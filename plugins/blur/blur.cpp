#include <wayfire/per-output-plugin.hpp>
#include <memory>
#include <list>
#include <wayfire/config/types.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/matcher.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/bindings-repository.hpp>

#include "blur.hpp"
#include "wayfire/core.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/opengl.hpp"
#include "wayfire/region.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/signal-provider.hpp"
#include <list>

using blur_algorithm_provider =
    std::function<nonstd::observer_ptr<wf_blur_base>()>;

static int calculate_damage_padding(const wf::render_target_t& target, int blur_radius)
{
    float scale = target.scale;
    if (target.subbuffer)
    {
        const float subbox_scale_x = 1.0 * target.subbuffer->width / target.geometry.width;
        const float subbox_scale_y = 1.0 * target.subbuffer->height / target.geometry.height;
        scale *= std::max(subbox_scale_x, subbox_scale_y);
    }

    return std::ceil(blur_radius / scale);
}

namespace wf
{
namespace scene
{
class blur_node_t : public transformer_base_node_t
{
  public:
    blur_algorithm_provider provider;
    wf::option_wrapper_t<double> skip_small_translucency{"blur/skip_small_translucency"};
    blur_node_t(blur_algorithm_provider provider) : transformer_base_node_t(false)
    {
        this->provider = provider;
        skip_small_translucency.set_callback([this]
        {
            wf::scene::damage_node(this, get_bounding_box());
        });
    }

    std::string stringify() const override
    {
        return "blur";
    }

    void gen_render_instances(std::vector<render_instance_uptr>& instances,
        damage_callback push_damage, wf::output_t *shown_on) override;

    struct saved_pixels_t
    {
        struct tile_t
        {
            wf::auxilliary_buffer_t pixels;
            wlr_box box{};
        };

        wf::auxilliary_buffer_t pixels;
        std::vector<tile_t> tiles;
        size_t tile_count = 0;
        wf::region_t region;
        bool taken = false;
    };

    std::list<saved_pixels_t> saved_pixels;
    saved_pixels_t *acquire_saved_pixel_buffer()
    {
        auto it = std::find_if(saved_pixels.begin(), saved_pixels.end(),
            [] (const saved_pixels_t& buffer) { return !buffer.taken; });

        if (it != saved_pixels.end())
        {
            it->taken = true;
            return &(*it);
        }

        saved_pixels.emplace_back();
        saved_pixels.back().taken = true;
        return &saved_pixels.back();
    }

    void release_saved_pixel_buffer(saved_pixels_t *buffer)
    {
        buffer->taken = false;
    }
};

class blur_render_instance_t : public transformer_render_instance_t<blur_node_t>
{
    blur_node_t::saved_pixels_t *saved_pixels = nullptr;

  public:
    using transformer_render_instance_t::transformer_render_instance_t;
    bool is_fully_opaque(wf::regionf_t damage)
    {
        if (self->get_children().size() == 1)
        {
            if (auto opaque = dynamic_cast<opaque_region_node_t*>(self->get_children().front().get()))
            {
                auto opaque_region = opaque->get_opaque_region();
                if ((damage ^ opaque_region).empty())
                {
                    return true;
                }

                // Decide from the whole visible node, not this frame's damage.
                // Otherwise a corner-only repaint could alternate between blurred
                // and unblurred output as the damage region changes.
                if (self->skip_small_translucency <= 0.0)
                {
                    return false;
                }

                const wf::regionf_t visible{self->get_bounding_box()};
                const auto translucent = visible ^ opaque_region;
                auto area = [] (const wf::regionf_t& region)
                {
                    double result = 0;
                    for (const auto& box : region)
                    {
                        result += (box.x2 - box.x1) * (box.y2 - box.y1);
                    }

                    return result;
                };

                const double visible_area = area(visible);
                return visible_area > 0 &&
                    area(translucent) <= visible_area * double(self->skip_small_translucency);
            }
        }

        return false;
    }

    wf::regionf_t calculate_translucent_damage(const wf::render_target_t& target, wf::regionf_t damage)
    {
        if (self->get_children().size() == 1)
        {
            if (auto opaque = dynamic_cast<opaque_region_node_t*>(self->get_children().front().get()))
            {
                const int padding =
                    calculate_damage_padding(target, self->provider()->calculate_blur_radius());
                auto opaque_region = opaque->get_opaque_region();
                opaque_region.expand_edges(-padding);

                wf::regionf_t translucent_region = damage ^ opaque_region;
                return translucent_region;
            }
        }

        return damage;
    }

    void schedule_instructions(std::vector<render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::regionf_t& damage) override
    {
        auto bbox = self->get_bounding_box();
        auto padded_region = damage & bbox & target.geometry;
        if (padded_region.empty())
        {
            return;
        }

        const int padding = calculate_damage_padding(target, self->provider()->calculate_blur_radius());

        // In order to render a part of the blurred background, we need to sample
        // from area which is larger than the damaged area. However, the edges
        // of the expanded area suffer from the same problem (e.g. the blurred
        // background has artifacts). The solution to this is to expand the
        // damage and keep a copy of the pixels where we redraw, but wouldn't
        // have redrawn if not for blur. After that, we copy those old areas
        // back to the destination framebuffer, giving the illusion that they
        // were never damaged.
        if (is_fully_opaque(padded_region & target.geometry) && target.is_integer_scale())
        {
            // If there are no regions to blur, we can directly render them.
            // However, on non-integer scale, rendering with a transformer vs rendering without one may
            // align subsurfaces in a different way due to rounding.
            //
            // As a result, we can apply this optimization only when the target is integer scaled.
            for (auto& ch : this->children)
            {
                ch->schedule_instructions(instructions, target, damage);
            }

            return;
        }

        padded_region.expand_edges(padding);
        padded_region &= bbox;

        // Don't forget to keep expanded damage within the bounds of the render
        // target, otherwise we may be sampling from outside of it (undefined
        // contents).
        padded_region &= target.geometry;

        // Actual region which will be repainted by this render instance.
        wf::regionf_t we_repaint = padded_region;

        this->saved_pixels   = self->acquire_saved_pixel_buffer();
        saved_pixels->region =
            target.framebuffer_region_from_geometry_region(padded_region) ^
            target.framebuffer_region_from_geometry_region(damage);
        saved_pixels->tile_count = 0;

        // Nodes below should re-render the padded areas so that we can sample from them
        damage |= padded_region;

        // When damage already covers the blur padding, there are no old pixels to preserve.
        if (!saved_pixels->region.empty())
        {
            static wf::option_wrapper_t<int> max_buffer_size{"workarounds/max_buffer_size"};
            const int tile_limit = std::max(1, (int)max_buffer_size);
            const auto target_size = target.get_size();
            const bool needs_tiles = (target_size.width > tile_limit) ||
                (target_size.height > tile_limit);

            if (needs_tiles)
            {
                saved_pixels->pixels.free();
            } else
            {
                saved_pixels->tiles.clear();
                saved_pixels->pixels.allocate(target_size);
            }

            wf::gles::run_in_context_if_gles([&]
            {
                GLuint target_fb = wf::gles::ensure_render_buffer_fb_id(target);
                if (needs_tiles)
                {
                    for (const auto& rect : saved_pixels->region)
                    {
                        for (int y = rect.y1; y < rect.y2; y += tile_limit)
                        {
                            for (int x = rect.x1; x < rect.x2; x += tile_limit)
                            {
                                if (saved_pixels->tile_count == saved_pixels->tiles.size())
                                {
                                    saved_pixels->tiles.emplace_back();
                                }

                                auto& tile = saved_pixels->tiles[saved_pixels->tile_count++];
                                tile.box = {x, y, std::min(tile_limit, rect.x2 - x),
                                    std::min(tile_limit, rect.y2 - y)};
                                tile.pixels.allocate({tile.box.width, tile.box.height});
                                wf::gles::bind_render_buffer(tile.pixels.get_renderbuffer());
                                GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb));
                                GL_CALL(glBlitFramebuffer(
                                    x, y, x + tile.box.width, y + tile.box.height,
                                    0, 0, tile.box.width, tile.box.height,
                                    GL_COLOR_BUFFER_BIT, GL_NEAREST));
                            }
                        }
                    }
                } else
                {
                    wf::gles::bind_render_buffer(saved_pixels->pixels.get_renderbuffer());
                    GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb));

                    /* Copy pixels in padded_region from target_fb to saved_pixels. */
                    for (const auto& box : saved_pixels->region)
                    {
                        GL_CALL(glBlitFramebuffer(
                            box.x1, box.y1,
                            box.x2, box.y2,
                            box.x1, box.y1,
                            box.x2, box.y2,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR));
                    }
                }
            });
        }

        instructions.push_back(render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = we_repaint,
                });
    }

    void render(const wf::scene::render_instruction_t& data) override
    {
        auto bounding_box = self->get_bounding_box();
        data.pass->custom_gles_subpass([&]
        {
            wf::dimensionsf_t render_size;
            auto contents = get_texture(data.target.scale, &render_size);

            auto tex = wf::gles_texture_t{contents};
            if (!data.damage.empty())
            {
                auto translucent_damage = calculate_translucent_damage(data.target, data.damage);
                self->provider()->prepare_blur(data.target, translucent_damage);

                wf::geometry_t render_geometry = wf::construct_box(wf::origin(bounding_box), render_size);
                render_geometry = data.target.aligned_geometry_from_geometry_box(render_geometry);
                self->provider()->render(tex, render_geometry, data.damage, data.target, data.target);
            }

            GL_CALL(glDisable(GL_SCISSOR_TEST));

            if (!saved_pixels->region.empty())
            {
                wf::gles::bind_render_buffer(data.target);
                if (saved_pixels->tile_count > 0)
                {
                    for (size_t i = 0; i < saved_pixels->tile_count; i++)
                    {
                        auto& tile = saved_pixels->tiles[i];
                        GLuint saved_fb = wf::gles::ensure_render_buffer_fb_id(
                            tile.pixels.get_renderbuffer());
                        GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_fb));
                        GL_CALL(glBlitFramebuffer(
                            0, 0, tile.box.width, tile.box.height,
                            tile.box.x, tile.box.y,
                            tile.box.x + tile.box.width, tile.box.y + tile.box.height,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST));
                    }
                } else
                {
                    GLuint saved_fb = wf::gles::ensure_render_buffer_fb_id(
                        saved_pixels->pixels.get_renderbuffer());
                    GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_fb));

                    // Restore pixels outside the original damage after rendering
                    // the padded region needed by the blur.
                    for (const auto& box : saved_pixels->region)
                    {
                        GL_CALL(glBlitFramebuffer(
                            box.x1, box.y1,
                            box.x2, box.y2,
                            box.x1, box.y1,
                            box.x2, box.y2,
                            GL_COLOR_BUFFER_BIT, GL_LINEAR));
                    }
                }
            }

            /* Reset stuff */
            saved_pixels->region.clear();
            self->release_saved_pixel_buffer(saved_pixels);
            saved_pixels = NULL;
        });
    }

    direct_scanout try_scanout(wf::output_t *output) override
    {
        // Enable direct scanout if it is possible
        return scene::try_scanout_from_list(children, output);
    }
};

void blur_node_t::gen_render_instances(std::vector<render_instance_uptr>& instances,
    damage_callback push_damage, wf::output_t *shown_on)
{
    auto uptr =
        std::make_unique<blur_render_instance_t>(this, push_damage, shown_on);
    if (uptr->has_instances())
    {
        instances.push_back(std::move(uptr));
    }
}
}
}

class wayfire_blur : public wf::plugin_interface_t
{
    // Before doing a render pass, expand the damage by the blur radius.
    // This is needed, because when blurring, the pixels that changed
    // affect a larger area than the really damaged region, e.g. the region
    // that comes from client damage.
    wf::signal::connection_t<wf::render_pass_begin_signal>
    on_render_pass_begin = [=] (wf::render_pass_begin_signal *ev)
    {
        if (!provider)
        {
            return;
        }

        const int padding =
            calculate_damage_padding(ev->pass.get_target(), provider()->calculate_blur_radius());
        ev->damage.expand_edges(padding);
        ev->damage &= ev->pass.get_target().geometry;
    };

  public:
    blur_algorithm_provider provider;
    wf::button_callback button_toggle;
    wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped = [=] (wf::view_mapped_signal *ev)
    {
        if (blur_by_default.matches(ev->view))
        {
            add_transformer(ev->view);
        }
    };

    wf::view_matcher_t blur_by_default{"blur/blur_by_default"};
    wf::option_wrapper_t<std::string> method_opt{"blur/method"};
    wf::option_wrapper_t<wf::buttonbinding_t> toggle_button{"blur/toggle"};
    wf::config::option_base_t::updated_callback_t blur_method_changed;
    std::unique_ptr<wf_blur_base> blur_algorithm;

    void add_transformer(wayfire_view view)
    {
        auto tmanager = view->get_transformed_node();
        if (tmanager->get_transformer<wf::scene::blur_node_t>())
        {
            return;
        }

        auto provider = [=] ()
        {
            return blur_algorithm.get();
        };

        auto node = std::make_shared<wf::scene::blur_node_t>(provider);
        tmanager->add_transformer(node, wf::TRANSFORMER_BLUR);
    }

    void pop_transformer(wayfire_view view)
    {
        auto tmanager = view->get_transformed_node();
        tmanager->rem_transformer<wf::scene::blur_node_t>();
    }

    void remove_transformers()
    {
        for (auto& view : wf::get_core().get_all_views())
        {
            pop_transformer(view);
        }
    }

  public:
    void init() override
    {
        if (!wf::get_core().is_gles2())
        {
            const char *render_type =
                wf::get_core().is_vulkan() ? "vulkan" : (wf::get_core().is_pixman() ? "pixman" : "unknown");
            LOGE("blur: requires GLES2 support, but current renderer is ", render_type);
            return;
        }

        wf::get_core().connect(&on_render_pass_begin);
        blur_method_changed = [=] ()
        {
            blur_algorithm = create_blur_from_name(method_opt);
            wf::scene::damage_node(wf::get_core().scene(), wf::get_core().scene()->get_bounding_box());
        };

        /* Create initial blur algorithm */
        blur_method_changed();
        method_opt.set_callback(blur_method_changed);

        /* Toggles the blur state of the view the user clicked on */
        button_toggle = [=] (auto)
        {
            auto view = wf::get_core().get_cursor_focus_view();
            if (!view)
            {
                return false;
            }

            if (view->get_transformed_node()->get_transformer<wf::scene::blur_node_t>())
            {
                pop_transformer(view);
            } else
            {
                add_transformer(view);
            }

            return true;
        };

        wf::get_core().bindings->add_button(toggle_button, &button_toggle);
        provider = [=] () { return this->blur_algorithm.get(); };
        wf::get_core().connect(&on_view_mapped);

        for (auto& view : wf::get_core().get_all_views())
        {
            if (blur_by_default.matches(view))
            {
                add_transformer(view);
            }
        }
    }

    void fini() override
    {
        remove_transformers();
        wf::get_core().bindings->rem_binding(&button_toggle);

        /* Call blur algorithm destructor */
        blur_algorithm = nullptr;
    }
};

extern "C" __attribute__((visibility("default"))) wf::plugin_interface_t *newInstance()
{
    return new wayfire_blur;
}

extern "C" __attribute__((visibility("default"))) uint32_t getWayfireVersion()
{
    return WAYFIRE_API_ABI_VERSION;
}
