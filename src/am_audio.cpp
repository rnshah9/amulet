#include "amulet.h"

#define AM_AUDIO_NODE_FLAG_MARK           ((uint32_t)1)
#define AM_AUDIO_NODE_FLAG_CHILDREN_DIRTY ((uint32_t)2)

#define node_marked(node)           (node->flags & AM_AUDIO_NODE_FLAG_MARK)
#define mark_node(node)             node->flags |= AM_AUDIO_NODE_FLAG_MARK
#define unmark_node(node)           node->flags &= ~AM_AUDIO_NODE_FLAG_MARK

#define children_dirty(node)        (node->flags & AM_AUDIO_NODE_FLAG_CHILDREN_DIRTY)
#define set_children_dirty(node)    node->flags |= AM_AUDIO_NODE_FLAG_CHILDREN_DIRTY
#define clear_children_dirty(node)  node->flags &= ~AM_AUDIO_NODE_FLAG_CHILDREN_DIRTY

static am_audio_context audio_context;

// Audio Param

inline static float linear_incf(am_audio_param<float> param, int samples) {
    return (param.target_value - param.current_value) / (float)samples;
}

// Audio Node

am_audio_node::am_audio_node() {
    pending_children.owner = this;
    live_children.owner = this;
    flags = 0;
    recursion_limit = 0;
    last_sync = 0;
    last_render = 0;
}

void am_audio_node::sync_params() {
}

void am_audio_node::post_render(am_audio_context *context, int num_samples) {
}

void am_audio_node::render_audio(am_audio_context *context, am_audio_bus *bus) {
    render_children(context, bus);
}

void am_audio_node::render_children(am_audio_context *context, am_audio_bus *bus) {
    if (recursion_limit < 0) return;
    recursion_limit--;
    for (int i = 0; i < live_children.size; i++) {
        live_children.arr[i].child->render_audio(context, bus);
    }
    recursion_limit++;
}

// Gain Node

am_gain_node::am_gain_node() : gain(0) {
    gain.pending_value = 1;
}

void am_gain_node::sync_params() {
    gain.update_target();
}

void am_gain_node::post_render(am_audio_context *context, int num_samples) {
    gain.update_current();
}

void am_gain_node::render_audio(am_audio_context *context, am_audio_bus *bus) {
    render_children(context, bus);
    int num_channels = bus->num_channels;
    int num_samples = bus->num_samples;
    float gain_inc = linear_incf(gain, num_samples);
    float gain_val = gain.current_value;
    for (int i = 0; i < num_samples; i++) {
        for (int c = 0; c < num_channels; c++) {
            bus->channel_data[c][i] *= gain_val;
        }
        gain_val += gain_inc;
    }
}

// Oscillator Node

am_oscillator_node::am_oscillator_node() : phase(0), freq(440), waveform(AM_WAVEFORM_SINE) {
    offset = 0;
}

void am_oscillator_node::sync_params() {
    phase.update_target();
    freq.update_target();
}

void am_oscillator_node::post_render(am_audio_context *context, int num_samples) {
    phase.update_current();
    freq.update_current();
    offset += num_samples;
}

void am_oscillator_node::render_audio(am_audio_context *context, am_audio_bus *bus) {
    double t = (double)offset / (double)context->sample_rate;
    double dt = 1.0/(double)context->sample_rate;
    int num_channels = bus->num_channels;
    int num_samples = bus->num_samples;
    float phase_inc = linear_incf(phase, num_samples);
    float freq_inc = linear_incf(freq, num_samples);
    float phase_val = phase.current_value;
    float freq_val = freq.current_value;
    switch (waveform.current_value) {
        case AM_WAVEFORM_SINE: {
            for (int i = 0; i < num_samples; i++) {
                float val = sinf(AM_PI*2.0f*freq_val*(float)t+phase_val);
                for (int c = 0; c < num_channels; c++) {
                    bus->channel_data[c][i] = val;
                }
                phase_val += phase_inc;
                freq_val += freq_inc;
                t += dt;
            }
        }
        default: {}
    }
}

//-------------------------------------------------------------------------

static int search_uservalues(lua_State *L, am_audio_node *node) {
    if (node_marked(node)) return 0; // cycle
    node->pushuservalue(L); // push uservalue table of node
    lua_pushvalue(L, 2); // push field
    lua_rawget(L, -2); // lookup field in uservalue table
    if (!lua_isnil(L, -1)) {
        // found it
        lua_remove(L, -2); // remove uservalue table
        return 1;
    }
    lua_pop(L, 2); // pop nil, uservalue table
    if (node->pending_children.size != 1) return 0;
    mark_node(node);
    am_audio_node *child = node->pending_children.arr[0].child;
    child->push(L);
    lua_replace(L, 1); // child is now at index 1
    int r = search_uservalues(L, child);
    unmark_node(node);
    return r;
}

int am_audio_node_index(lua_State *L) {
    am_audio_node *node = (am_audio_node*)lua_touserdata(L, 1);
    am_default_index_func(L); // check metatable
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1); // pop nil
    return search_uservalues(L, node);
}

static int add_child(lua_State *L) {
    am_check_nargs(L, 2);
    am_audio_node *parent = am_get_userdata(L, am_audio_node, 1);
    am_audio_node *child = am_get_userdata(L, am_audio_node, 2);
    am_audio_node_child child_slot;
    child_slot.child = child;
    child_slot.ref = parent->ref(L, 2); // ref from parent to child
    parent->pending_children.push_back(L, child_slot);
    set_children_dirty(parent);
    lua_pushvalue(L, 1); // for chaining
    return 1;
}

static int remove_child(lua_State *L) {
    am_check_nargs(L, 2);
    am_audio_node *parent = am_get_userdata(L, am_audio_node, 1);
    am_audio_node *child = am_get_userdata(L, am_audio_node, 2);
    for (int i = 0; i < parent->pending_children.size; i++) {
        if (parent->pending_children.arr[i].child == child) {
            parent->unref(L, parent->pending_children.arr[i].ref);
            parent->pending_children.remove(i);
            set_children_dirty(parent);
            break;
        }
    }
    lua_pushvalue(L, 1); // for chaining
    return 1;
}

static int replace_child(lua_State *L) {
    am_check_nargs(L, 3);
    am_audio_node *parent = am_get_userdata(L, am_audio_node, 1);
    am_audio_node *old_child = am_get_userdata(L, am_audio_node, 2);
    am_audio_node *new_child = am_get_userdata(L, am_audio_node, 3);
    for (int i = 0; i < parent->pending_children.size; i++) {
        if (parent->pending_children.arr[i].child == old_child) {
            parent->reref(L, parent->pending_children.arr[i].ref, 3);
            parent->pending_children.arr[i].child = new_child;
            set_children_dirty(parent);
            break;
        }
    }
    lua_pushvalue(L, 1); // for chaining
    return 1;
}

static int remove_all_children(lua_State *L) {
    am_check_nargs(L, 1);
    am_audio_node *parent = am_get_userdata(L, am_audio_node, 1);
    for (int i = parent->pending_children.size-1; i >= 0; i--) {
        parent->unref(L, parent->pending_children.arr[i].ref);
    }
    parent->pending_children.clear();
    set_children_dirty(parent);
    lua_pushvalue(L, 1); // for chaining
    return 1;
}

void am_set_audio_node_child(lua_State *L, am_audio_node *parent) {
    if (lua_isnil(L, 1)) {
        return;
    }
    am_audio_node *child = am_get_userdata(L, am_audio_node, 1);
    am_audio_node_child child_slot;
    child_slot.child = child;
    child_slot.ref = parent->ref(L, 1); // ref from parent to child
    parent->pending_children.push_back(L, child_slot);
    set_children_dirty(parent);
}

static int child_pair_next(lua_State *L) {
    am_check_nargs(L, 2);
    am_audio_node *node = am_get_userdata(L, am_audio_node, 1);
    int i = luaL_checkinteger(L, 2);
    if (i >= 0 && i < node->pending_children.size) {
        lua_pushinteger(L, i+1);
        node->pending_children.arr[i].child->push(L);
        return 2;
    } else {
        lua_pushnil(L);
        return 1;
    }
}

static int child_pairs(lua_State *L) {
    lua_pushcclosure(L, child_pair_next, 0);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

static int get_child(lua_State *L) {
    am_check_nargs(L, 2);
    am_audio_node *node = am_get_userdata(L, am_audio_node, 1);
    int i = luaL_checkinteger(L, 2);
    if (i >= 1 && i <= node->pending_children.size) {
        node->pending_children.arr[i-1].child->push(L);
        return 1;
    } else {
        return 0;
    }
}

static inline void check_alias(lua_State *L) {
    am_default_index_func(L);
    if (!lua_isnil(L, -1)) goto error;
    lua_pop(L, 1);
    return;
    error:
    luaL_error(L,
        "alias '%s' is already used for something else",
        lua_tostring(L, 2));
}

static int alias(lua_State *L) {
    int nargs = am_check_nargs(L, 2);
    am_audio_node *node = am_get_userdata(L, am_audio_node, 1);
    node->pushuservalue(L);
    int userval_idx = am_absindex(L, -1);
    if (lua_istable(L, 2)) {
        // create multiple aliases - one for each key/value pair
        lua_pushvalue(L, 2); // push table, as we need position 2 for check_alias
        int tbl_idx = am_absindex(L, -1);
        lua_pushnil(L);
        while (lua_next(L, tbl_idx)) {
            lua_pushvalue(L, -2); // key
            lua_replace(L, 2); // check_alias expects key in position 2
            check_alias(L);
            lua_pushvalue(L, -2); // key
            lua_pushvalue(L, -2); // value
            lua_rawset(L, userval_idx); // uservalue[key] = value
            lua_pop(L, 1); // value
        }
        lua_pop(L, 1); // table
    } else if (lua_isstring(L, 2)) {
        check_alias(L);
        lua_pushvalue(L, 2);
        if (nargs > 2) {
            lua_pushvalue(L, 3);
        } else {
            lua_pushvalue(L, 1);
        }
        lua_rawset(L, userval_idx);
    } else {
        return luaL_error(L, "expecting a string or table at position 2");
    }
    lua_pop(L, 1); // uservalue
    lua_pushvalue(L, 1);
    return 1;
}

static int create_gain_node(lua_State *L) {
    am_check_nargs(L, 2);
    am_gain_node *node = am_new_userdata(L, am_gain_node);
    am_set_audio_node_child(L, node);
    node->gain.pending_value = luaL_checknumber(L, 2);
    return 1;
}

static void get_gain(lua_State *L, void *obj) {
    am_gain_node *node = (am_gain_node*)obj;
    lua_pushnumber(L, node->gain.pending_value);
}

static void set_gain(lua_State *L, void *obj) {
    am_gain_node *node = (am_gain_node*)obj;
    node->gain.pending_value = luaL_checknumber(L, 3);
}

static am_property gain_property = {get_gain, set_gain};

static void register_gain_node_mt(lua_State *L) {
    lua_newtable(L);
    lua_pushcclosure(L, am_audio_node_index, 0);
    lua_setfield(L, -2, "__index");

    am_register_property(L, "gain", &gain_property);

    lua_pushstring(L, "gain");
    lua_setfield(L, -2, "tname");

    am_register_metatable(L, MT_am_gain_node, MT_am_audio_node);
}

static int create_oscillator_node(lua_State *L) {
    int nargs = am_check_nargs(L, 1);
    am_oscillator_node *node = am_new_userdata(L, am_oscillator_node);
    node->freq.set_immediate(luaL_checknumber(L, 1));
    if (nargs > 1) {
        node->phase.set_immediate(luaL_checknumber(L, 2));
    }
    return 1;
}

static void get_phase(lua_State *L, void *obj) {
    am_oscillator_node *node = (am_oscillator_node*)obj;
    lua_pushnumber(L, node->phase.pending_value);
}

static void set_phase(lua_State *L, void *obj) {
    am_oscillator_node *node = (am_oscillator_node*)obj;
    node->phase.pending_value = luaL_checknumber(L, 3);
}

static am_property phase_property = {get_phase, set_phase};

static void get_freq(lua_State *L, void *obj) {
    am_oscillator_node *node = (am_oscillator_node*)obj;
    lua_pushnumber(L, node->freq.pending_value);
}

static void set_freq(lua_State *L, void *obj) {
    am_oscillator_node *node = (am_oscillator_node*)obj;
    node->freq.pending_value = luaL_checknumber(L, 3);
}

static am_property freq_property = {get_freq, set_freq};

static void register_oscillator_node_mt(lua_State *L) {
    lua_newtable(L);
    lua_pushcclosure(L, am_audio_node_index, 0);
    lua_setfield(L, -2, "__index");

    am_register_property(L, "phase", &phase_property);
    am_register_property(L, "freq", &freq_property);

    lua_pushstring(L, "oscillator");
    lua_setfield(L, -2, "tname");

    am_register_metatable(L, MT_am_oscillator_node, MT_am_audio_node);
}

static int make_root(lua_State *L) {
    am_check_nargs(L, 1);
    am_audio_node *node = am_get_userdata(L, am_audio_node, 1);
    audio_context.roots.push_back(node);
    lua_rawgeti(L, LUA_REGISTRYINDEX, AM_AUDIO_ROOTS_TABLE);
    lua_pushvalue(L, 1);
    luaL_ref(L, -2);
    lua_pop(L, 1); // roots table
    return 0;
}

static void register_audio_node_mt(lua_State *L) {
    lua_newtable(L);

    lua_pushcclosure(L, am_audio_node_index, 0);
    lua_setfield(L, -2, "__index");
    lua_pushcclosure(L, am_default_newindex_func, 0);
    lua_setfield(L, -2, "__newindex");

    lua_pushcclosure(L, child_pairs, 0);
    lua_setfield(L, -2, "children");
    lua_pushcclosure(L, get_child, 0);
    lua_setfield(L, -2, "child");

    lua_pushcclosure(L, alias, 0);
    lua_setfield(L, -2, "alias");

    lua_pushcclosure(L, add_child, 0);
    lua_setfield(L, -2, "add");
    lua_pushcclosure(L, remove_child, 0);
    lua_setfield(L, -2, "remove");
    lua_pushcclosure(L, replace_child, 0);
    lua_setfield(L, -2, "replace");
    lua_pushcclosure(L, remove_all_children, 0);
    lua_setfield(L, -2, "remove_all");
    
    lua_pushcclosure(L, create_gain_node, 0);
    lua_setfield(L, -2, "gain");

    lua_pushcclosure(L, make_root, 0);
    lua_setfield(L, -2, "make_root");

    lua_pushstring(L, "audio_node");
    lua_setfield(L, -2, "tname");

    am_register_metatable(L, MT_am_audio_node, 0);
}

void am_open_audio_module(lua_State *L) {
    luaL_Reg funcs[] = {
        {"oscillator", create_oscillator_node},
        {NULL, NULL}
    };
    am_open_module(L, AMULET_LUA_MODULE_NAME, funcs);
    register_audio_node_mt(L);
    register_gain_node_mt(L);
    register_oscillator_node_mt(L);

    lua_newtable(L);
    lua_rawseti(L, LUA_REGISTRYINDEX, AM_AUDIO_ROOTS_TABLE);
}

//-------------------------------------------------------------------------


void am_init_audio(int sample_rate) {
    audio_context.sample_rate = sample_rate;
    audio_context.sync_id = 0;
    audio_context.render_id = 0;
    audio_context.roots.clear();
}

static void do_post_render(am_audio_context *context, int num_samples, am_audio_node *node) {
    if (node->last_render >= context->render_id) return; // already processed
    node->last_render = context->render_id;
    node->post_render(context, num_samples);
    for (int i = 0; i < node->live_children.size; i++) {
        do_post_render(context, num_samples, node->live_children.arr[i].child);
    }
}

void am_fill_audio_bus(am_audio_bus *bus) {
    for (unsigned int i = 0; i < audio_context.roots.size(); i++) {
        am_audio_node *root = audio_context.roots[i];
        root->render_audio(&audio_context, bus);
    }
    audio_context.render_id++;
    for (unsigned int i = 0; i < audio_context.roots.size(); i++) {
        am_audio_node *root = audio_context.roots[i];
        do_post_render(&audio_context, bus->num_samples, root);
    }
}

static void sync_children_list(lua_State *L, am_audio_node *node) {
    if (children_dirty(node)) {
        // make sure live_children size <= pending_children size
        for (int i = node->live_children.size-1; i >= node->pending_children.size; i--) {
            node->unref(L, node->live_children.arr[i].ref);
            node->live_children.remove(i);
        }
        // replace live_chilren slots with pending_children slots
        for (int i = 0; i < node->live_children.size; i++) {
            node->live_children.arr[i].child = node->pending_children.arr[i].child;
            node->pending_children.arr[i].child->push(L);
            node->reref(L, node->live_children.arr[i].ref, -1);
            lua_pop(L, 1);
        }
        // append remaining pending_children to live_children
        for (int i = node->live_children.size; i < node->pending_children.size; i++) {
            am_audio_node_child slot;
            slot.child = node->pending_children.arr[i].child;
            slot.child->push(L);
            slot.ref = node->ref(L, -1);
            lua_pop(L, 1);
            node->live_children.push_back(L, slot);
        }
        clear_children_dirty(node);
    }
}

static void sync_audio_graph(lua_State *L, am_audio_context *context, am_audio_node *node) {
    if (node->last_sync >= context->sync_id) return; // already synced
    node->last_sync = context->sync_id;
    node->sync_params();
    sync_children_list(L, node);
    for (int i = 0; i < node->live_children.size; i++) {
        sync_audio_graph(L, context, node->live_children.arr[i].child);
    }
}

void am_sync_audio_graph(lua_State *L) {
    audio_context.sync_id++;
    for (unsigned int i = 0; i < audio_context.roots.size(); i++) {
        am_audio_node *root = audio_context.roots[i];
        sync_audio_graph(L, &audio_context, root);
    }
}
