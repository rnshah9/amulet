#include "amulet.h"

void am_use_program_command::execute(am_render_state *rstate) {
    for (int i = 0; i < program->num_params; i++) {
        am_program_param *param = program->params[i];
        am_program_param **slot = &am_param_name_map[param->name];
        rstate->trail.trail(slot, sizeof(am_program_param*));
        *slot = param;
    }
    rstate->trail.trail(&rstate->active_program, sizeof(am_program*));
    rstate->active_program = program;
}

void am_draw_command::execute(am_render_state *rstate) {
    rstate->draw();
}

am_set_float_param_command::am_set_float_param_command(lua_State *L, int nargs, am_node *node) {
    if (nargs < 2 || !lua_isstring(L, 2)) {
        luaL_error(L, "expecting a string in position 2");
    }
    name = am_lookup_param_name(L, 2);
    if (nargs > 2) {
        value = lua_tonumber(L, 3);
    } else {
        value = 0.0f;
    }
}

void am_set_float_param_command::execute(am_render_state *rstate) {
    am_program_param *p = am_param_name_map[name];
    if (p != NULL) p->trailed_set_float(rstate, value);
}

void am_mul_float_param_command::execute(am_render_state *rstate) {
    am_program_param *p = am_param_name_map[name];
    if (p != NULL) p->trailed_mul_float(rstate, value);
}

void am_add_float_param_command::execute(am_render_state *rstate) {
    am_program_param *p = am_param_name_map[name];
    if (p != NULL) p->trailed_add_float(rstate, value);
}

void am_set_float_array_command::execute(am_render_state *rstate) {
    am_program_param *p = am_param_name_map[name];
    if (p == NULL) return;
    int available_bytes = vbo->size - offset - am_attribute_client_type_size(type);
    int max_draw_elements = 0;
    if (available_bytes > 0) {
        max_draw_elements = available_bytes / stride + 1;
    }
    p->trailed_set_float_array(rstate,
        vbo->buffer_id, type, normalized, stride, offset, max_draw_elements);
}