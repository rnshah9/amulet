#include "amulet.h"

am_render_state am_global_render_state;

void am_viewport_state::set(int x, int y, int w, int h) {
    am_viewport_state::x = x;
    am_viewport_state::y = y;
    am_viewport_state::w = w;
    am_viewport_state::h = h;
    dirty = true;
}

void am_viewport_state::update() {
    if (dirty) {
        am_set_viewport(x, y, w, h);
        dirty = false;
    }
}

void am_depth_test_state::set(bool enabled, am_depth_func func) {
    am_depth_test_state::enabled = enabled;
    am_depth_test_state::func = func;
    dirty = true;
}

void am_depth_test_state::restore(am_depth_test_state *old) {
    am_depth_test_state::enabled = old->enabled;
    am_depth_test_state::func = old->func;
    dirty = true;
}

void am_depth_test_state::update() {
    if (dirty) {
        am_set_depth_test_enabled(enabled);
        am_set_depth_func(func);
        dirty = false;
    }
}

void am_cull_face_state::set(bool enabled, am_face_winding winding, am_cull_face_side side) {
    am_cull_face_state::enabled = enabled;
    am_cull_face_state::winding = winding;
    am_cull_face_state::side = side;
    dirty = true;
}

void am_cull_face_state::restore(am_cull_face_state *old) {
    am_cull_face_state::enabled = old->enabled;
    am_cull_face_state::winding = old->winding;
    am_cull_face_state::side = old->side;
    dirty = true;
}

void am_cull_face_state::update() {
    if (dirty) {
        am_set_cull_face_enabled(enabled);
        if (enabled) {
            am_set_front_face_winding(winding);
            am_set_cull_face_side(side);
        }
        dirty = false;
    }
}

void am_render_state::update_state() {
    viewport_state.update();
    depth_test_state.update();
    cull_face_state.update();
    bind_active_program();
    bind_active_program_params();
}

void am_render_state::setup(am_framebuffer_id fb, bool clear, int w, int h, bool has_depthbuffer) {
    am_bind_framebuffer(fb);
    viewport_state.set(0, 0, w, h);
    depth_test_state.set(has_depthbuffer, AM_DEPTH_FUNC_LESS);
    if (clear) am_clear_framebuffer(true, true, true);
}

void am_render_state::draw_arrays(am_draw_mode mode, int first, int draw_array_count) {
    if (active_program == NULL) return;
    update_state();
    if (validate_active_program()) {
        if (max_draw_array_size == INT_MAX && draw_array_count == INT_MAX) {
            am_log1("%s", "WARNING: ignoring draw_arrays, "
                "because no attribute arrays have been bound"
                "and no count has been specified");
            draw_array_count = 0;
        }
        int count = max_draw_array_size - first;
        if (count > draw_array_count) count = draw_array_count;
        if (count > 0) {
            am_draw_arrays(mode, first, count);
        }
    }
}

void am_render_state::draw_elements(am_draw_mode mode, int first, int count,
    am_buffer_view *indices_view, am_element_index_type type)
{
    if (active_program == NULL) return;
    update_state();
    if (validate_active_program()) {
        if (indices_view->buffer->elembuf_id == 0) {
            indices_view->buffer->create_elembuf();
        }
        indices_view->buffer->update_if_dirty();
        indices_view->update_max_elem_if_required();
        if (max_draw_array_size == INT_MAX) {
            am_log1("%s", "WARNING: ignoring draw_elements, "
                "because no attribute arrays have been bound");
            count = 0;
        } else if ((int)indices_view->max_elem > max_draw_array_size) {
            am_log1("WARNING: ignoring draw_elements, "
                "because one of its indices (%d) is out of bounds",
                ((int)indices_view->max_elem + 1));
            count = 0;
        }
        if (count > (indices_view->size - first)) {
            count = (indices_view->size - first);
        }
        if (count > 0) {
            am_bind_buffer(AM_ELEMENT_ARRAY_BUFFER, indices_view->buffer->elembuf_id);
            am_draw_elements(mode, count, type, first * indices_view->stride);
        }
    }
}

bool am_render_state::validate_active_program() {
    if (am_conf_validate_shader_programs) {
        bool valid = am_validate_program(active_program->program_id);
        if (!valid) {
            char *log = am_get_program_info_log(active_program->program_id);
            am_log1("WARNING: shader program failed validation: %s", log);
            free(log);
        }
        return valid;
    } else {
        return true;
    }
}

void am_render_state::bind_active_program() {
    if (bound_program_id != active_program->program_id) {
        am_use_program(active_program->program_id);
        bound_program_id = active_program->program_id;
    }
}

void am_render_state::bind_active_program_params() {
    max_draw_array_size = INT_MAX;
    for (int i = 0; i < active_program->num_params; i++) {
        am_program_param *param = &active_program->params[i];
        param->bind(this);
    }
}

am_render_state::am_render_state() {
    viewport_state.x = 0;
    viewport_state.y = 0;
    viewport_state.w = 0;
    viewport_state.h = 0;
    viewport_state.dirty = true;

    depth_test_state.enabled = false;
    depth_test_state.func = AM_DEPTH_FUNC_LESS;
    viewport_state.dirty = true;

    max_draw_array_size = 0;

    bound_program_id = 0;
    active_program = NULL;

    next_free_texture_unit = 0;
}

am_render_state::~am_render_state() {
}

static void register_draw_arrays_node_mt(lua_State *L) {
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushstring(L, "draw_arrays");
    lua_setfield(L, -2, "tname");

    am_register_metatable(L, MT_am_draw_arrays_node, MT_am_scene_node);
}

void am_draw_arrays_node::render(am_render_state *rstate) {
    rstate->draw_arrays(mode, first, count);
}

static int create_draw_arrays_node(lua_State *L) {
    int nargs = am_check_nargs(L, 0);
    int first = 0;
    int count = INT_MAX;
    am_draw_mode mode = AM_DRAWMODE_TRIANGLES;
    if (nargs > 0) {
        mode = am_get_enum(L, am_draw_mode, 1);
    }
    if (nargs > 1) {
        first = luaL_checkinteger(L, 2) - 1;
        if (first < 0) {
            return luaL_error(L, "argument 2 must be positive");
        }
    }
    if (nargs > 2) {
        count = luaL_checkinteger(L, 3);
        if (count <= 0) {
            return luaL_error(L, "argument 3 must be positive");
        }
    }
    am_draw_arrays_node *node = am_new_userdata(L, am_draw_arrays_node);
    node->first = first;
    node->count = count;
    node->mode = mode;
    return 1;
}

am_draw_elements_node::am_draw_elements_node() {
    first = 0;
    count = INT_MAX;
    mode = AM_DRAWMODE_TRIANGLES;
    type = AM_ELEMENT_TYPE_USHORT;
    indices_view = NULL;
    view_ref = LUA_NOREF;
}

static void register_draw_elements_node_mt(lua_State *L) {
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");

    lua_pushstring(L, "draw_elements");
    lua_setfield(L, -2, "tname");

    am_register_metatable(L, MT_am_draw_elements_node, MT_am_scene_node);
}

void am_draw_elements_node::render(am_render_state *rstate) {
    rstate->draw_elements(mode, first, count, indices_view, type);
}

static int create_draw_elements_node(lua_State *L) {
    int nargs = am_check_nargs(L, 1);
    am_buffer_view *indices_view = am_get_userdata(L, am_buffer_view, 1);
    am_draw_elements_node *node = am_new_userdata(L, am_draw_elements_node);
    switch (indices_view->type) {
        case AM_VIEW_TYPE_USHORT_ELEM:
            if (indices_view->stride != 2) {
                return luaL_error(L, "ushort_elem array must have stride 2 when used with draw_elements");
            }
            node->type = AM_ELEMENT_TYPE_USHORT;
            break;
        case AM_VIEW_TYPE_UINT_ELEM:
            if (indices_view->stride != 4) {
                return luaL_error(L, "uint_elem array must have stride 4 when used with draw_elements");
            }
            node->type = AM_ELEMENT_TYPE_UINT;
            break;
        default:
            return luaL_error(L, "only ushort_elem and uint_elem views can be used as element arrays");
    }
    node->indices_view = indices_view;
    node->view_ref = node->ref(L, 1);
    if (nargs > 1) {
        node->mode = am_get_enum(L, am_draw_mode, 2);
    }
    if (nargs > 2) {
        node->first = luaL_checkinteger(L, 3) - 1;
        if (node->first < 0) {
            return luaL_error(L, "argument 3 must be positive");
        } else if (node->first >= indices_view->size) {
            return luaL_error(L, "argument 3 is out of bounds");
        }
    }
    if (nargs > 3) {
        node->count = luaL_checkinteger(L, 4);
        if (node->count <= 0) {
            return luaL_error(L, "argument 4 must be positive");
        }
    }
    return 1;
}

void am_open_renderer_module(lua_State *L) {
    luaL_Reg funcs[] = {
        {"draw_arrays", create_draw_arrays_node},
        {"draw_elements", create_draw_elements_node},
        {NULL, NULL}
    };
    am_open_module(L, AMULET_LUA_MODULE_NAME, funcs);

    am_enum_value draw_mode_enum[] = {
        {"points",          AM_DRAWMODE_POINTS},
        {"lines",           AM_DRAWMODE_LINES},
        {"line_strip",      AM_DRAWMODE_LINE_STRIP},
        {"line_loop",       AM_DRAWMODE_LINE_LOOP},
        {"triangles",       AM_DRAWMODE_TRIANGLES},
        {"triangle_strip",  AM_DRAWMODE_TRIANGLE_STRIP},
        {"triangle_fan",    AM_DRAWMODE_TRIANGLE_FAN},
        {NULL, 0}
    };
    am_register_enum(L, ENUM_am_draw_mode, draw_mode_enum);

    register_draw_arrays_node_mt(L);
    register_draw_elements_node_mt(L);
}
