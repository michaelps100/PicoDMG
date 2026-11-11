#pragma once

#include "lstate.hpp"
#include <array>
#include <lua.hpp>
#include <span>
#include <string_view>

#include <emu/bufferio.hpp>
#include <emu/mmio.hpp>
#include <hal/hal.hpp>
#include <video/palette.hpp>

namespace emu {

struct EmulatorPersistentState {
	std::array<char, 128> load_path_cstr{};
	std::array<char, 128> breadcrumb_cstr{};
};

class Emulator {
	public:
	constexpr Emulator() = default;
	~Emulator();
	Emulator(const Emulator &) = delete;
	Emulator &operator=(const Emulator &) = delete;
	Emulator(Emulator &&) = delete;
	Emulator &operator=(Emulator &&) = delete;

	void init();
	void bind_globals();

	void set_active_cart_path(std::string_view cart_path);

	void set_breadcrumb(std::string_view bc) {
		std::size_t len = std::min(bc.size(), _persistent_state.breadcrumb_cstr.size() - 1);
		std::memcpy(_persistent_state.breadcrumb_cstr.data(), bc.data(), len);
		_persistent_state.breadcrumb_cstr[len] = '\0';
	}

	void clear_breadcrumb() {
		_persistent_state.breadcrumb_cstr[0] = '\0';
	}

	std::string_view get_breadcrumb() const {
		return {_persistent_state.breadcrumb_cstr.data()};
	}

	bool has_pending_load_cart() const {
		return _persistent_state.load_path_cstr[0] != '\0';
	}

	std::string_view get_pending_load_cart() const {
		return {_persistent_state.load_path_cstr.data()};
	}

	void clear_pending_load_cart() {
		_persistent_state.load_path_cstr[0] = '\0';
	}

	void trigger_load_from_vm(std::string_view cart_path);
	bool load_from_path(std::string_view cart_path);
	bool load_and_inject_header(Reader reader);
	void exec(std::string_view buf);

	void handle_repl();

	void run_until_shutdown();
	void run_once();
	void flip();

	int get_fps_target() const;

	enum class HookResult { SUCCESS, UNDEFINED, LUA_ERROR };

	HookResult run_hook(const char *name);

	[[noreturn]] void panic(const char *message);

	constexpr Memory memory() { return Memory{std::span(_memory)}; }

	auto &palette() { return _palette; }

	std::uint64_t get_update_start_time() { return _update_start_time; }
	std::uint64_t get_frame_start_time() { return _frame_start_time; }
	std::uint64_t get_frame_target_time() { return _frame_target_time; }

	// TODO: implement repetition logic
	// TODO: probably shouldn't use hal:: structs here
	hal::ButtonState get_button_state() { return _button_state; }
	void set_button_state(hal::ButtonState s) { _button_state = s; }

	lua_State *lua() { return &_lua_preallocated_state.l.l; }

	private:
	EmulatorPersistentState _persistent_state;
	std::array<std::uint8_t, 65536> _memory;
	std::array<std::uint32_t, 32> _palette;
	LG _lua_preallocated_state;
	bool _lua_ready = false;

	hal::ButtonState _button_state;

	std::uint64_t _update_start_time = 0;
	std::uint64_t _frame_start_time = 0;
	std::uint64_t _frame_target_time = 0;
};

// NOLINTBEGIN
extern Emulator emulator;
extern char g_last_panic_msg[256];

template <class Device, std::uint16_t map_address = Device::default_map_address>
inline constexpr auto device = emulator.memory().device<Device>(map_address);
// NOLINTEND

} // namespace emu
