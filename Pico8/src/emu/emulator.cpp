#include "emulator.hpp"
#include "devices/drawstatemisc.hpp"
#include "devices/image.hpp"
#include "devices/random.hpp"
#include "emu/bufferio.hpp"
#include "lgc.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <lauxlib.hpp>
#include <ldo.hpp>
#include <lua.hpp>
#include <lualib.hpp>

#include "y8header.hpp"
#include "y8std.hpp"
#include <devices/buttonstate.hpp>
#include <devices/clippingrectangle.hpp>
#include <devices/drawpalette.hpp>
#include <devices/screenpalette.hpp>
#include <hal/hal.hpp>
#include <p8/parser.hpp>
#include <p8/png_cart.hpp>

#if defined(PICO_BOARD) || defined(PICO_PLATFORM) || defined(PICO_RP2350) || defined(PICO_BUILD) || __has_include(<hardware/watchdog.h>)
#include <hardware/watchdog.h>
#define Y8_WATCHDOG_UPDATE() watchdog_update()
#else
#define Y8_WATCHDOG_UPDATE() do {} while (0)
#endif

extern "C" {
void mk_ili9225_text(char *s, uint16_t x, uint16_t y, uint16_t color, uint16_t bgcolor);
void mk_ili9225_fill(uint16_t color);
void pico8_show_error(const char *title, const char *msg);
void p8_state_init_lua(lua_State *L);
bool pico8_menu_get_turbo(void);
}

namespace emu {

Emulator::~Emulator() {
	if (_lua_ready) {
		lua_close(lua());
	}
}

void Emulator::init() {
	if (_lua_ready) {
		lua_close(lua());
		_lua_ready = false;
	}

	const auto default_palette = hal::get_default_palette();
	std::copy(default_palette.begin(), default_palette.end(), _palette.begin());

	lua_newstate(y8_lua_realloc, nullptr, &_lua_preallocated_state,
	             _memory.data());
	_lua_ready = true;

#ifdef Y8_EXPERIMENTAL_GENGC
	// printf("Buggy Lua 5.2 Generational GC enabled\n");
	luaC_changemode(lua(), KGC_GEN);
#else
	// Tune incremental GC for steady 30/60 fps without frame-drop spikes
	lua_gc(lua(), LUA_GCSETPAUSE, 100);
	lua_gc(lua(), LUA_GCSETSTEPMUL, 250);
#endif
	hal::load_rgb_palette(_palette);

	// Recreate global table with 128 preallocated hashmap records
	lua_createtable(lua(), 0, 128);
	lua_rawseti(lua(), LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

	bind_globals();

#if defined(PICO_BOARD) || defined(PICO_PLATFORM) || defined(PICO_RP2350) || defined(PICO_BUILD)
	p8_state_init_lua(lua());
#endif

	_memory.fill(0);
	device<devices::DrawPalette>.reset();
	device<devices::DrawStateMisc>.reset();
	device<devices::ScreenPalette>.reset();
	device<devices::ClippingRectangle>.reset();
	device<devices::Random>.set_seed(hal::get_unique_seed());
	_button_state = {};
}

extern "C" {
__attribute__((weak)) void p8_audio_sfx(int sfx_id, int channel, int offset, int length) { (void)sfx_id; (void)channel; (void)offset; (void)length; }
__attribute__((weak)) void p8_audio_music(int pattern, int fade_len, int channel_mask) { (void)pattern; (void)fade_len; (void)channel_mask; }
}

static int y8_sfx(lua_State *state) {
	int id = luaL_optinteger(state, 1, -1);
	int channel = luaL_optinteger(state, 2, -1);
	int offset = luaL_optinteger(state, 3, 0);
	int length = luaL_optinteger(state, 4, 32);
	p8_audio_sfx(id, channel, offset, length);
	return 0;
}

static int y8_music(lua_State *state) {
	int pattern = luaL_optinteger(state, 1, -1);
	int fade_len = luaL_optinteger(state, 2, 0);
	int channel_mask = luaL_optinteger(state, 3, 0);
	p8_audio_music(pattern, fade_len, channel_mask);
	return 0;
}

void Emulator::bind_globals() {
	luaL_openlibs(lua());

	for (const auto &binding : y8_std) {
		lua_register(lua(), binding.name, binding.callback);
	}

	for (const auto &binding : y8_numeric_globals) {
		lua_pushnumber(lua(), binding.value);
		lua_setglobal(lua(), reinterpret_cast<const char *>(binding.name));
	}

	lua_register(lua(), "sfx", y8_sfx);
	lua_register(lua(), "music", y8_music);

	/* PICO-8 coroutine aliases */
	lua_getglobal(lua(), "coroutine");
	if (lua_istable(lua(), -1)) {
		lua_getfield(lua(), -1, "create");  lua_setglobal(lua(), "cocreate");
		lua_getfield(lua(), -1, "resume");  lua_setglobal(lua(), "coresume");
		lua_getfield(lua(), -1, "status");  lua_setglobal(lua(), "costatus");
		lua_getfield(lua(), -1, "yield");   lua_setglobal(lua(), "yield");
	}
	lua_pop(lua(), 1);

	/* Ensure unpack is available globally */
	lua_getglobal(lua(), "table");
	if (lua_istable(lua(), -1)) {
		lua_getfield(lua(), -1, "unpack"); lua_setglobal(lua(), "unpack");
	}
	lua_pop(lua(), 1);
}

void Emulator::set_active_cart_path(std::string_view cart_path) {
	lua_pushlstring(lua(), cart_path.data(), cart_path.size());
	lua_setglobal(lua(), "__cart_name");
}

void Emulator::trigger_load_from_vm(std::string_view cart_path) {
	if (cart_path.size() >= _persistent_state.load_path_cstr.size() - 1) {
		panic("Path to load() is too long");
	}

	std::memcpy(_persistent_state.load_path_cstr.data(), cart_path.data(),
	            cart_path.size());
	_persistent_state.load_path_cstr[cart_path.size()] = '\0';

	throw EmulatorReset();
}

bool Emulator::load_from_path(std::string_view cart_path) {
	printf("Loading cart from \"%.*s\"\n", int(cart_path.size()),
	       cart_path.data());
	set_active_cart_path(cart_path);

	if (const auto last_dash_pos = cart_path.find_last_of("/\\");
	    last_dash_pos != cart_path.npos) {

		hal::fs_set_working_directory(cart_path.substr(0, last_dash_pos));
	}

	if (p8::is_png_cart(cart_path)) {
		return p8::load_png_cart(cart_path, *this);
	}

	hal::FileReaderContext reader;

	const hal::FileOpenStatus status =
		hal::fs_create_open_context(cart_path, reader);

	if (status != hal::FileOpenStatus::SUCCESS) {
		return false;
	}

	if (const auto parse_status = p8::parse(hal::fs_read_buffer, &reader, {},
	                                        &_lua_preallocated_state.g);
	    parse_status != p8::ParserStatus::OK) {
		printf("Load from '%.*s' failed with error code %d!\n",
		       int(cart_path.size()), cart_path.data(), int(parse_status));
		return false;
	}

	hal::fs_destroy_open_context(reader);

	return true;
}

bool Emulator::load_and_inject_header(Reader reader) {
	CartImporterReader state{.header_reader{app_header}, .cart_reader = reader};

	const int load_status = lua_load(
		lua(),
		[]([[maybe_unused]] lua_State *state, void *ud, size_t *sz) {
			return CartImporterReader::reader_callback(ud, sz);
		},
		&state, "/bios/header.lua", "t");

	if (load_status != 0) {
		const char *err = lua_tostring(lua(), -1);
		printf("[Pico8] Script load failed (%d): %s\n", load_status, err ? err : "(null)");
		if (err) {
			std::strncpy(g_last_panic_msg, err, sizeof(g_last_panic_msg) - 1);
			g_last_panic_msg[sizeof(g_last_panic_msg) - 1] = '\0';
		} else {
			snprintf(g_last_panic_msg, sizeof(g_last_panic_msg), "lua_load erro %d", load_status);
		}
		lua_pop(lua(), 1);
		return false;
	}
	return true;
}

void Emulator::exec(std::string_view buf) {
	const int load_status =
		luaL_loadbuffer(lua(), buf.data(), buf.size(), "repl");

	if (load_status != 0) {
		printf("Exec compilation failed: %s\n", lua_tostring(lua(), -1));
		panic(lua_tostring(lua(), -1));
		lua_pop(lua(), 1);
	}

	if (lua_pcall(lua(), 0, 0, 0) != 0) {
		printf("Exec failed: %s\n", lua_tostring(lua(), -1));
		panic(lua_tostring(lua(), -1));
		lua_pop(lua(), 1);
	}
}

void Emulator::handle_repl() {
#ifdef Y8_LUA_REPL
	// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
	std::array<char, 512> repl_buffer;
	const auto repl_input = hal::read_repl(repl_buffer);

	if (!repl_input.empty()) {
		exec(std::string_view{repl_input.data(), repl_input.size()});
	}
#endif
}

void Emulator::run_until_shutdown() {
	// This loop makes use of exceptions to handle loading and panics.
	//
	// This is kinda hacky for a few reasons, including:
	// - attempting to reduce stack space usage
	// - avoiding unbounded recursion like panic->repl->panic->etc
	// - proper exception handling for handle_repl and such

	for (;;) {
		try {
			run_once();
			break;
		} catch (const EmulatorReset &e) {
			throw;
		} catch (const EmulatorPanic &e) {
			goto panic_handler;
		} catch (const EmulatorExit &e) {
			break;
		}

	reset_handler: {
		lua_close(lua());
		init();
		try {
			load_from_path(_persistent_state.load_path_cstr.data());
		} catch (...) {
		}
		continue;
	}

	panic_handler: {
#if defined(PICO_BOARD) || defined(PICO_PLATFORM) || defined(PICO_RP2350) || defined(PICO_BUILD)
		extern char g_last_panic_msg[256];
		pico8_show_error("PICO-8 PANIC (ERRO):", g_last_panic_msg);
		break;
#else
		for (;;) {
			try {
				handle_repl();
			} catch (const EmulatorReset &e) {
				goto reset_handler;
			} catch (...) {
			}
			hal::update_button_state();
			flip();
		}
#endif
	}
	}
}

void Emulator::run_once() {
	hal::reset_timer();

	if (lua_pcall(lua(), 0, 0, 0) != 0) {
		printf("Script exec at load time failed: %s\n",
		       lua_tostring(lua(), -1));
		panic(lua_tostring(lua(), -1));
		lua_pop(lua(), 1);
	}

	_frame_target_time = 1'000'000u / get_fps_target();

	run_hook("_init");

	// if no hook ends up being executed in the loop; then assume we should be
	// exiting
	bool has_any_hook_defined;

	do {
		Y8_WATCHDOG_UPDATE();
		has_any_hook_defined = false;

		/// execute a hook and updates has_any_hook_defined for this iteration
		const auto run_loop_hook = [&](const char *name) {
			const auto hook_result = run_hook(name);
			has_any_hook_defined |= hook_result != HookResult::UNDEFINED;
			return hook_result;
		};

		handle_repl();

		// this _update behavior matches pico-8's: if _update60 is defined,
		// _update is ignored when both are unspecified, flipping will occur at
		// 30Hz regardless

		// we however don't clip the framerate to either 15/30/60. just push the
		// frames as early as we can.

		// TODO: need a mechanism to occasionally flip when draw is never called
		// (e.g. billion pale dots do that for a loading screen)

		_update_start_time = hal::measure_time_us();

		_button_state = hal::update_button_state();
		device<devices::ButtonState>.for_player(0) =
			_button_state.held_key_mask;

		if (run_loop_hook("_update60") == HookResult::UNDEFINED) {
			run_loop_hook("_update");
		}

		run_loop_hook("_draw");

		flip();
	} while (has_any_hook_defined);
}

void Emulator::flip() {
	Y8_WATCHDOG_UPDATE();

#if defined(PICO_BOARD) || defined(PICO_PLATFORM) || defined(PICO_RP2350) || defined(PICO_BUILD)
	const bool is_turbo = pico8_menu_get_turbo();
	static bool s_turbo_flip = false;
	/* In turbo mode, skip every second screen blit to maintain full 2x speed without SPI ceiling */
	const bool skip_blit = is_turbo && s_turbo_flip;
	s_turbo_flip = !s_turbo_flip;
#else
	const bool is_turbo = false;
	const bool skip_blit = false;
#endif

	if (!skip_blit) {
		hal::present_frame();
	}

	// printf("%f\n", double(taken_time) / 1000.0);

#ifdef Y8_EXPERIMENTAL_GENGC
	lua_gc(lua(), LUA_GCSTEP, 0);
#else
	lua_gc(lua(), LUA_GCSTEP, 100);
#endif

	hal::post_frame_hooks();

	const auto taken_time = hal::measure_time_us() - _frame_start_time;
	const auto target_time = is_turbo ? (_frame_target_time / 2) : _frame_target_time;

	if (taken_time < target_time) {
		hal::delay_time_us(target_time - taken_time);
	}

	_frame_start_time = hal::measure_time_us();
}

int Emulator::get_fps_target() const {
	// this function is const wrt the resulting state of the emulator
	auto *lua_state = const_cast<Emulator *>(this)->lua(); // NOLINT

	lua_getglobal(lua_state, "_update60");
	const bool is60 = lua_isfunction(lua_state, -1);
	lua_pop(lua_state, 1);

	return is60 ? 60 : 30;
}

Emulator::HookResult Emulator::run_hook(const char *name) {
	lua_getglobal(lua(), name);

	if (!lua_isfunction(lua(), -1)) {
		lua_pop(lua(), 1);
		return HookResult::UNDEFINED;
	}

	if (lua_pcall(lua(), 0, 0, 0) != 0) {
		printf("hook '%s' execution failed: %s\n", name,
		       lua_tostring(lua(), -1));
		panic(lua_tostring(lua(), -1));
		lua_pop(lua(), 1);
		// return HookResult::LUA_ERROR;
	}

	return HookResult::SUCCESS;
}

char g_last_panic_msg[256] = {0};

void Emulator::panic(const char *message) {
	printf("[Pico8] PANIC: %s\n", message ? message : "(null)");
	if (message) {
		std::strncpy(g_last_panic_msg, message, sizeof(g_last_panic_msg) - 1);
		g_last_panic_msg[sizeof(g_last_panic_msg) - 1] = '\0';
	}

#ifdef Y8_INFINITE_LOOP_EXIT
	throw EmulatorPanic();
#else
	exit(1);
#endif
}

Emulator emulator; // NOLINT

} // namespace emu