#include <memory>
#include <wayfire/util/log.hpp>
#include <wayfire/workarea.hpp>
#include "view-impl.hpp"
#include "wayfire/debug.hpp"
#include "wayfire/geometry.hpp"
#include "wayfire/output.hpp"
#include "wayfire/scene-render.hpp"
#include "wayfire/scene.hpp"
#include "wayfire/view.hpp"
#include "wayfire/view-transform.hpp"
#include "wayfire/toplevel-view.hpp"
#include "wayfire/workspace-set.hpp"
#include "wayfire/option-wrapper.hpp"

#include <glm/glm.hpp>
#include "wayfire/signal-definitions.hpp"
#include <wayfire/scene-operations.hpp>

void wf::view_interface_t::set_role(view_role_t new_role)
{
    role = new_role;
}

std::string wf::view_interface_t::to_string() const
{
    return "view-" + wf::object_base_t::to_string();
}

wayfire_view wf::view_interface_t::self()
{
    return wayfire_view(this);
}

/** Set the view's output. */
void wf::view_interface_t::set_output(wf::output_t *new_output)
{
    view_set_output_signal data;
    data.view   = self();
    data.output = get_output(); // the old output

    this->priv->output = new_output;

    this->emit(&data);
    if (new_output)
    {
        new_output->emit(&data);
    }

    wf::get_core().emit(&data);

    if (data.output && (data.output != new_output))
    {
        view_disappeared_signal data_disappeared;
        data_disappeared.view = self();
        data.output->emit(&data_disappeared);
        wf::scene::update(get_root_node(), scene::update_flag::REFOCUS);
    }
}

wf::output_t*wf::view_interface_t::get_output()
{
    return priv->output;
}

void wf::view_interface_t::ping()
{
    // Do nothing, specialized in the various shells
}

void wf::view_interface_t::close()
{
    /* no-op */
}

wf::geometry_t wf::view_interface_t::get_bounding_box()
{
    return get_transformed_node()->get_bounding_box();
}

bool wf::view_interface_t::is_focusable() const
{
    return true;
}

void wf::view_interface_t::damage()
{
    wf::scene::damage_node(get_surface_root_node(), get_surface_root_node()->get_bounding_box());
}

bool wf::view_interface_t::has_transformer()
{
    auto ch = get_transformed_node()->get_children();
    return !ch.empty() && ch.front() != get_surface_root_node();
}

void wf::view_interface_t::take_snapshot(wf::auxilliary_buffer_t& buffer)
{
    auto root_node = get_surface_root_node();
    const wf::geometry_t bbox = root_node->get_bounding_box();
    float scale = get_output()->handle->scale;
    buffer.allocate(wf::dimensions(bbox), scale,
        wf::buffer_allocation_hints_t{.hdr_linear = get_output() && get_output()->is_hdr()});

    wf::render_target_t target{buffer};
    target.geometry = bbox;
    target.scale    = scale;

    std::vector<scene::render_instance_uptr> instances;
    root_node->gen_render_instances(instances, [] (auto) {}, get_output());

    render_pass_params_t params;
    params.background_color = {0, 0, 0, 0};
    params.damage    = bbox;
    params.target    = target;
    params.instances = &instances;
    params.flags     = RPASS_CLEAR_BACKGROUND;
    render_pass_t::run(params);
}

wf::view_interface_t::view_interface_t()
{
    this->priv = std::make_unique<wf::view_interface_t::view_priv_impl>();
}

class sentinel_node_t : public wf::scene::node_t
{
  public:
    sentinel_node_t() : node_t(false)
    {}

    std::string stringify() const
    {
        return "sentinel node (unmapped contents)";
    }
};

void wf::view_interface_t::set_surface_root_node(scene::floating_inner_ptr surface_root_node)
{
    priv->dummy_node = std::make_shared<sentinel_node_t>();
    this->priv->surface_root_node = surface_root_node;

    // Plugins may want to add subsurfaces even before the view is mapped for the first time.
    // This is why we add a dummy node to track the position of the main surface in the order of surfaces
    // even before mapping the view for the first time.
    wf::scene::add_front(surface_root_node, priv->dummy_node);

    // Set up view content to scene.
    priv->transformed_node->set_children_list({surface_root_node});
}

static std::optional<wf::geometry_t> get_workspace_clip(wf::view_interface_t *view)
{
    static wf::option_wrapper_t<bool> isolate_workspaces{"core/isolate_workspaces"};
    if (!isolate_workspaces || !view || (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT))
    {
        return {};
    }

    auto toplevel = wf::toplevel_cast(wayfire_view{view});
    if (!toplevel || toplevel->sticky)
    {
        return {};
    }

    auto wset = toplevel->get_wset();
    if (!wset || !wset->get_attached_output())
    {
        return {};
    }

    auto geometry = wset->get_last_output_geometry();
    if (!geometry)
    {
        return {};
    }

    auto home = wset->get_view_home_workspace(toplevel);
    auto current = wset->get_current_workspace();
    geometry->x += (home.x - current.x) * geometry->width;
    geometry->y += (home.y - current.y) * geometry->height;
    return geometry;
}

class isolated_view_render_instance_t : public wf::scene::render_instance_t
{
    std::weak_ptr<wf::view_interface_t> view;
    std::vector<wf::scene::render_instance_uptr> children;
    wf::scene::damage_callback push_damage;

    wf::signal::connection_t<wf::scene::node_damage_signal> on_node_damage =
        [=] (wf::scene::node_damage_signal *ev)
    {
        push_damage(ev->region);
    };

    std::optional<wf::geometry_t> clip() const
    {
        auto ptr = view.lock();
        return get_workspace_clip(ptr.get());
    }

  public:
    isolated_view_render_instance_t(wf::view_interface_t *view, wf::scene::node_t *node,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) :
        view(view->weak_from_this()), push_damage(push_damage)
    {
        node->connect(&on_node_damage);
        auto forward_damage = [=] (wf::regionf_t damage)
        {
            if (auto bounds = clip())
            {
                damage &= *bounds;
            }

            this->push_damage(damage);
        };

        for (auto& child : node->get_children())
        {
            if (child->is_enabled())
            {
                child->gen_render_instances(children, forward_damage, shown_on);
            }
        }
    }

    void schedule_instructions(std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::regionf_t& damage) override
    {
        if (auto bounds = clip())
        {
            wf::regionf_t inside = damage & *bounds;
            for (auto& child : children)
            {
                child->schedule_instructions(instructions, target, inside);
            }

            damage ^= *bounds;
            damage |= inside & *bounds;
        } else
        {
            for (auto& child : children)
            {
                child->schedule_instructions(instructions, target, damage);
            }
        }
    }

    wf::scene::direct_scanout try_scanout(wf::output_t *output) override
    {
        if (auto bounds = clip(); bounds && (*bounds != output->get_relative_geometry()))
        {
            return wf::scene::direct_scanout::OCCLUSION;
        }

        return wf::scene::try_scanout_from_list(children, output);
    }

    void compute_visibility(wf::output_t *output, wf::regionf_t& visible) override
    {
        if (auto bounds = clip())
        {
            auto inside = visible & *bounds;
            wf::scene::compute_visibility_from_list(children, output, inside, {0, 0});
            visible ^= *bounds;
            visible |= inside & *bounds;
        } else
        {
            wf::scene::compute_visibility_from_list(children, output, visible, {0, 0});
        }
    }

    void presentation_feedback(wf::output_t *output) override
    {
        for (auto& child : children)
        {
            child->presentation_feedback(output);
        }
    }
};

class view_root_node_t : public wf::scene::floating_inner_node_t, public wf::view_node_tag_t
{
  public:
    view_root_node_t(wf::view_interface_t *_view) : floating_inner_node_t(false),
        view_node_tag_t(_view), view(_view->weak_from_this())
    {}

    std::optional<wf::scene::input_node_t> find_node_at(const wf::pointf_t& at) override
    {
        auto ptr = view.lock();
        if (auto bounds = get_workspace_clip(ptr.get()); bounds && !(*bounds & at))
        {
            return {};
        }

        return floating_inner_node_t::find_node_at(at);
    }

    wf::keyboard_focus_node_t keyboard_refocus(wf::output_t *output) override
    {
        auto ptr = view.lock();
        if (auto bounds = get_workspace_clip(ptr.get());
            bounds && !(*bounds & output->get_relative_geometry()))
        {
            return {};
        }

        return floating_inner_node_t::keyboard_refocus(output);
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *output) override
    {
        if (auto ptr = view.lock())
        {
            instances.push_back(std::make_unique<isolated_view_render_instance_t>(
                ptr.get(), this, push_damage, output));
        }
    }

    std::string stringify() const override
    {
        if (auto ptr = view.lock())
        {
            std::ostringstream out;
            out << ptr->self();
            return "view-root-node of " + out.str() + " " + stringify_flags();
        } else
        {
            return "inert view-root-node " + stringify_flags();
        }
    }

  private:
    std::weak_ptr<wf::view_interface_t> view;
};

void wf::view_interface_t::base_initialization()
{
    priv->root_node = std::make_shared<view_root_node_t>(this);
    priv->transformed_node = std::make_shared<scene::transform_manager_node_t>();
    priv->root_node->set_children_list({priv->transformed_node});
    priv->root_node->set_enabled(false);

    priv->pre_free = [=] (auto)
    {
        this->_clear_data();
        if (auto self = dynamic_cast<toplevel_view_interface_t*>(this))
        {
            auto children = self->children;
            for (auto ch : children)
            {
                ch->set_toplevel_parent(nullptr);
            }
        }
    };

    this->connect(&priv->pre_free);
}

wf::view_interface_t::~view_interface_t()
{
    wf::scene::remove_child(get_root_node());
}

const wf::scene::floating_inner_ptr& wf::view_interface_t::get_root_node() const
{
    return priv->root_node;
}

const std::shared_ptr<wf::scene::transform_manager_node_t>& wf::view_interface_t::get_transformed_node() const
{
    return priv->transformed_node;
}

const wf::scene::floating_inner_ptr& wf::view_interface_t::get_surface_root_node() const
{
    return priv->surface_root_node;
}

wayfire_view wf::node_to_view(wf::scene::node_t *node)
{
    while (node)
    {
        if (auto vnode = dynamic_cast<view_node_tag_t*>(node))
        {
            return vnode->get_view();
        }

        node = node->parent();
    }

    return nullptr;
}

wayfire_view wf::node_to_view(wf::scene::node_ptr node)
{
    return node_to_view(node.get());
}

wl_client*wf::view_interface_t::get_client()
{
    if (get_wlr_surface())
    {
        return wl_resource_get_client(get_wlr_surface()->resource);
    }

    return nullptr;
}

wlr_surface*wf::view_interface_t::get_wlr_surface()
{
    return priv->wsurface;
}
