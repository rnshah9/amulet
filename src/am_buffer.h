enum am_buffer_view_type {
    AM_BUF_ELEM_TYPE_FLOAT,
    AM_BUF_ELEM_TYPE_FLOAT_VEC2,
    AM_BUF_ELEM_TYPE_FLOAT_VEC3,
    AM_BUF_ELEM_TYPE_FLOAT_VEC4,
    AM_BUF_ELEM_TYPE_BYTE,
    AM_BUF_ELEM_TYPE_BYTE_VEC2,
    AM_BUF_ELEM_TYPE_BYTE_VEC3,
    AM_BUF_ELEM_TYPE_BYTE_VEC4,
    AM_BUF_ELEM_TYPE_UNSIGNED_BYTE,
    AM_BUF_ELEM_TYPE_UNSIGNED_BYTE_VEC2,
    AM_BUF_ELEM_TYPE_UNSIGNED_BYTE_VEC3,
    AM_BUF_ELEM_TYPE_UNSIGNED_BYTE_VEC4,
    AM_BUF_ELEM_TYPE_SHORT,
    AM_BUF_ELEM_TYPE_SHORT_VEC2,
    AM_BUF_ELEM_TYPE_SHORT_VEC3,
    AM_BUF_ELEM_TYPE_SHORT_VEC4,
    AM_BUF_ELEM_TYPE_UNSIGNED_SHORT,
    AM_BUF_ELEM_TYPE_UNSIGNED_SHORT_VEC2,
    AM_BUF_ELEM_TYPE_UNSIGNED_SHORT_VEC3,
    AM_BUF_ELEM_TYPE_UNSIGNED_SHORT_VEC4,
};

struct am_buffer {
    int                 size;  // in bytes
    char                data[];
};

struct am_buffer_view {
    am_buffer           *buffer;
    int                 offset; // in bytes
    int                 stride; // in bytes
    int                 size;   // number of elements
    am_buffer_view_type type;
    bool                normalized;
};

void am_open_buffer_module(lua_State *L);