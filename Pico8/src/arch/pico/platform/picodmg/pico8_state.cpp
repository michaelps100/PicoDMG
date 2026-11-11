#include "pico8_state.hpp"
#include "p8_audio.h"
#include "emu/emulator.hpp"
#include "ff.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define P8_STATE_MAGIC 0x50385353u /* "P8SS" */

struct StateHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t memory_size;
    uint32_t palette_size;
    uint32_t audio_size;
    uint32_t lua_size;
};

static const char *s_savestate_lua = R"LUA(
local __p8_builtins = {}

function __p8_snapshot_builtins()
    __p8_builtins = {}
    for k, v in pairs(_G) do
        __p8_builtins[k] = true
    end
end

function __p8_save_state()
    local catalog = {}
    local visited_scan = {}
    
    local function scan_funcs(tbl, prefix, depth)
        if depth > 3 or visited_scan[tbl] then return end
        visited_scan[tbl] = true
        for k, v in pairs(tbl) do
            local path = (prefix == "") and tostring(k) or (prefix .. "." .. tostring(k))
            if type(v) == "function" then
                if not catalog[v] then
                    catalog[v] = path
                end
            elseif type(v) == "table" and depth < 3 then
                if tbl ~= _G or (not __p8_builtins[k] and k ~= "package" and k ~= "_G") then
                    scan_funcs(v, path, depth + 1)
                end
            end
        end
    end
    scan_funcs(_G, "", 0)

    local tbl_map = {}
    local tbl_list = {}
    local next_tbl_id = 1

    local function register_table(t)
        if tbl_map[t] then return tbl_map[t] end
        local id = next_tbl_id
        next_tbl_id = next_tbl_id + 1
        tbl_map[t] = id
        tbl_list[id] = t
        return id
    end

    local function discover_all_tables(root_t)
        if not root_t or tbl_map[root_t] then return end
        register_table(root_t)
        local queue = { root_t }
        local head = 1
        while head <= #queue do
            local t = queue[head]
            head = head + 1
            for k, v in pairs(t) do
                if type(k) == "table" and not tbl_map[k] then
                    register_table(k)
                    queue[#queue + 1] = k
                end
                if type(v) == "table" and not tbl_map[v] then
                    register_table(v)
                    queue[#queue + 1] = v
                end
            end
            local mt = getmetatable(t)
            if mt and type(mt) == "table" and not tbl_map[mt] then
                register_table(mt)
                queue[#queue + 1] = mt
            end
        end
    end

    local root_vars = {}
    for k, v in pairs(_G) do
        if not __p8_builtins[k] and type(k) == "string" and k:sub(1, 5) ~= "__p8_" then
            if type(v) == "table" then
                discover_all_tables(v)
                root_vars[#root_vars + 1] = { k, {"@t", tbl_map[v]} }
            elseif type(v) == "number" or type(v) == "boolean" or type(v) == "string" then
                root_vars[#root_vars + 1] = { k, v }
            end
        end
    end

    local function stringify(v)
        local tv = type(v)
        if tv == "number" then
            return tostring(v)
        elseif tv == "boolean" then
            return v and "true" or "false"
        elseif tv == "string" then
            return string.format("%q", v)
        elseif tv == "table" then
            if v[1] == "@t" then
                return string.format("{\"@t\",%d}", v[2])
            elseif v[1] == "@f" then
                return string.format("{\"@f\",%q}", v[2])
            end
        end
        return "nil"
    end

    local out = {"return{g={"}
    for i = 1, #root_vars do
        local rv = root_vars[i]
        out[#out + 1] = string.format("{%q,%s},", rv[1], stringify(rv[2]))
    end
    out[#out + 1] = "},t={"
    
    local metatables = {}
    for id = 1, #tbl_list do
        local t = tbl_list[id]
        local mt = getmetatable(t)
        if mt and tbl_map[mt] then
            metatables[#metatables + 1] = { id, tbl_map[mt] }
        end
        out[#out + 1] = "{"
        for k, v in pairs(t) do
            local key_repr = k
            if type(k) == "table" then
                key_repr = {"@t", tbl_map[k]}
            end
            local val_repr = nil
            if type(v) == "table" then
                val_repr = {"@t", tbl_map[v]}
            elseif type(v) == "function" then
                if catalog[v] then
                    val_repr = {"@f", catalog[v]}
                end
            elseif type(v) == "number" or type(v) == "boolean" or type(v) == "string" then
                val_repr = v
            end
            if val_repr ~= nil then
                out[#out + 1] = string.format("{%s,%s},", stringify(key_repr), stringify(val_repr))
            end
        end
        out[#out + 1] = "},"
    end
    out[#out + 1] = "},m={"
    for i = 1, #metatables do
        out[#out + 1] = string.format("{%d,%d},", metatables[i][1], metatables[i][2])
    end
    out[#out + 1] = "}}"

    return table.concat(out)
end

function __p8_load_state_from_data(data)
    if not data or not data.g or not data.t then return end

    local t_objs = {}
    for id = 1, #data.t do
        t_objs[id] = {}
    end

    local function resolve(v)
        if type(v) == "table" then
            if v[1] == "@t" then
                return t_objs[v[2]]
            elseif v[1] == "@f" then
                local path = v[2]
                local cur = _G
                for part in string.gmatch(path, "[^.]+") do
                    if cur then
                        local n = tonumber(part)
                        cur = cur[n or part]
                    end
                end
                return cur
            end
        end
        return v
    end

    local new_keys = {}
    for i = 1, #data.g do
        new_keys[data.g[i][1]] = true
    end
    for k, v in pairs(_G) do
        if not __p8_builtins[k] and type(k) == "string" and k:sub(1, 5) ~= "__p8_" and type(v) ~= "function" then
            if not new_keys[k] then
                _G[k] = nil
            end
        end
    end

    for id = 1, #data.t do
        local target = t_objs[id]
        local entries = data.t[id]
        for i = 1, #entries do
            local k = resolve(entries[i][1])
            local val = resolve(entries[i][2])
            if k ~= nil then
                target[k] = val
            end
        end
    end

    if data.m then
        for i = 1, #data.m do
            local pair = data.m[i]
            local id, mt_id = pair[1], pair[2]
            if t_objs[id] and t_objs[mt_id] then
                setmetatable(t_objs[id], t_objs[mt_id])
            end
        end
    end

    for i = 1, #data.g do
        local k = data.g[i][1]
        local val = resolve(data.g[i][2])
        _G[k] = val
    end

    collectgarbage("collect")
end
)LUA";

extern "C" void p8_state_init_lua(lua_State *L) {
    if (!L) return;
    if (luaL_dostring(L, s_savestate_lua) != 0) {
        printf("[Pico8 State] Failed to register savestate Lua script: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_getglobal(L, "__p8_snapshot_builtins");
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            printf("[Pico8 State] Failed to snapshot builtins: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

extern "C" bool pico8_internal_save_state(const char *path) {
    lua_State *L = emu::emulator.lua();
    const char *lua_str = "";
    size_t lua_len = 0;

    if (L) {
        lua_getglobal(L, "__p8_save_state");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 1, 0) == 0) {
                lua_str = lua_tolstring(L, -1, &lua_len);
            } else {
                printf("[Pico8 State] __p8_save_state error: %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }

    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        if (L && lua_len > 0) lua_pop(L, 1);
        return false;
    }

    size_t audio_sz = p8_audio_get_state_size();
    void *audio_buf = malloc(audio_sz);
    if (audio_buf) {
        p8_audio_get_state(audio_buf);
    } else {
        audio_sz = 0;
    }

    auto view = emu::emulator.memory().raw_view;
    auto pal = emu::emulator.palette();

    StateHeader hdr = {
        .magic = P8_STATE_MAGIC,
        .version = 2,
        .memory_size = (uint32_t)view.size(),
        .palette_size = (uint32_t)(pal.size() * sizeof(uint32_t)),
        .audio_size = (uint32_t)audio_sz,
        .lua_size = (uint32_t)lua_len
    };

    UINT bw = 0;
    bool ok = true;

    if (f_write(&fil, &hdr, sizeof(hdr), &bw) != FR_OK || bw != sizeof(hdr)) ok = false;
    if (ok && (f_write(&fil, view.data(), view.size(), &bw) != FR_OK || bw != view.size())) ok = false;
    if (ok && (f_write(&fil, pal.data(), hdr.palette_size, &bw) != FR_OK || bw != hdr.palette_size)) ok = false;
    if (ok && audio_sz > 0 && (f_write(&fil, audio_buf, audio_sz, &bw) != FR_OK || bw != audio_sz)) ok = false;
    if (ok && lua_len > 0 && (f_write(&fil, lua_str, lua_len, &bw) != FR_OK || bw != lua_len)) ok = false;

    if (audio_buf) free(audio_buf);
    if (L && lua_len > 0) lua_pop(L, 1);
    f_close(&fil);

    printf("[Pico8 State] Saved state to '%s' (RAM: %u, Pal: %u, Audio: %u, Lua: %u bytes, status: %s)\n",
           path, (unsigned)hdr.memory_size, (unsigned)hdr.palette_size, (unsigned)hdr.audio_size,
           (unsigned)hdr.lua_size, ok ? "OK" : "FAIL");

    return ok;
}

extern "C" bool pico8_internal_load_state(const char *path) {
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        return false;
    }

    StateHeader hdr;
    UINT br = 0;
    if (f_read(&fil, &hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr) || hdr.magic != P8_STATE_MAGIC) {
        f_close(&fil);
        return false;
    }

    auto view = emu::emulator.memory().raw_view;
    if (hdr.memory_size != view.size()) {
        f_close(&fil);
        return false;
    }

    if (f_read(&fil, view.data(), view.size(), &br) != FR_OK || br != view.size()) {
        f_close(&fil);
        return false;
    }

    if (hdr.version >= 2) {
        auto pal = emu::emulator.palette();
        if (hdr.palette_size == pal.size() * sizeof(uint32_t)) {
            f_read(&fil, pal.data(), hdr.palette_size, &br);
        }

        if (hdr.audio_size > 0) {
            void *audio_buf = malloc(hdr.audio_size);
            if (audio_buf) {
                if (f_read(&fil, audio_buf, hdr.audio_size, &br) == FR_OK && br == hdr.audio_size) {
                    p8_audio_set_state(audio_buf);
                }
                free(audio_buf);
            }
        }

        if (hdr.lua_size > 0) {
            char *lua_buf = (char *)malloc(hdr.lua_size + 1);
            if (lua_buf) {
                if (f_read(&fil, lua_buf, hdr.lua_size, &br) == FR_OK && br == hdr.lua_size) {
                    lua_buf[hdr.lua_size] = '\0';
                    lua_State *L = emu::emulator.lua();
                    if (L) {
                        if (luaL_loadbuffer(L, lua_buf, hdr.lua_size, "savestate") == 0) {
                            if (lua_pcall(L, 0, 1, 0) == 0) {
                                lua_getglobal(L, "__p8_load_state_from_data");
                                if (lua_isfunction(L, -1)) {
                                    lua_pushvalue(L, -2); /* push data table */
                                    if (lua_pcall(L, 1, 0, 0) != 0) {
                                        printf("[Pico8 State] __p8_load_state_from_data error: %s\n", lua_tostring(L, -1));
                                        lua_pop(L, 1);
                                    }
                                } else {
                                    lua_pop(L, 1);
                                }
                                lua_pop(L, 1); /* pop data table */
                            } else {
                                printf("[Pico8 State] savestate chunk exec error: %s\n", lua_tostring(L, -1));
                                lua_pop(L, 1);
                            }
                        } else {
                            printf("[Pico8 State] savestate chunk load error: %s\n", lua_tostring(L, -1));
                            lua_pop(L, 1);
                        }
                    }
                }
                free(lua_buf);
            }
        }
    }

    f_close(&fil);

    printf("[Pico8 State] Loaded state from '%s' (version: %u, Lua: %u bytes)\n",
           path, (unsigned)hdr.version, (unsigned)hdr.lua_size);

    return true;
}
