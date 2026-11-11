#include "emu/bufferio.hpp"
#include "pico/time.h"
#include <cstdio>
#include <hal/hal.hpp>
#include <hal/types.hpp>

#include "../main/bios.hpp"
#include <cmdthread.hpp>
#include <hardware/pwm.h>
#include <hardwarestate.hpp>
#include <pico/rand.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#if __has_include(<tusb.h>)
#include <tusb.h>
#endif

#include <platform/platform.hpp>

namespace hal {

namespace pico = arch::pico;

// NOTE: some of the implementations are deferred to the platform.

void present_frame() {
	arch::pico::platform::present_frame(nullptr);
}

void reset_timer() { pico::state.timer_start = get_absolute_time(); }

std::uint64_t measure_time_us() {
	const auto current_time = get_absolute_time();
	return absolute_time_diff_us(pico::state.timer_start, current_time);
}

#ifndef PICO_DEFAULT_LED_PIN
#ifdef GB_LED_GPIO
#define PICO_DEFAULT_LED_PIN GB_LED_GPIO
#else
#define PICO_DEFAULT_LED_PIN 25
#endif
#endif

void delay_time_us(std::uint64_t time) {
	sleep_us(time);
}

extern "C" __attribute__((weak)) void tud_task(void) {}

void post_frame_hooks() { tud_task(); }

std::span<char> read_repl(std::span<char> target_buffer) {
	(void)target_buffer;
	return {};
}

void fs_set_working_directory(std::string_view path) {
	// std::string_view is not a C string...
	std::array<char, 256> filename_c_buffer;

	if (path.size() + 1 > filename_c_buffer.size()) {
		return; // TODO: error status; or just exceptions...?
	}

	std::memcpy(filename_c_buffer.data(), path.data(), path.size());
	filename_c_buffer[path.size()] = '\0';

	f_chdir(filename_c_buffer.data());
}

FileOpenStatus fs_create_open_context(std::string_view path,
                                      FileReaderContext &ctx) {
	ctx.is_bios_read = false;

	if (path == "/bios/bios.p8") {
		ctx.is_bios_read = true;
		ctx.reader.bios_reader = emu::StringReader(bios_cartridge);
		return FileOpenStatus::SUCCESS;
	}

	// std::string_view is not a C string...
	std::array<char, 256> filename_c_buffer;

	if (path.size() + 1 > filename_c_buffer.size()) {
		return FileOpenStatus::FAIL;
	}

	std::memcpy(filename_c_buffer.data(), path.data(), path.size());
	filename_c_buffer[path.size()] = '\0';

	if (const auto open_status =
	        f_open(&ctx.reader.fs_reader, filename_c_buffer.data(), FA_READ);
	    open_status != FR_OK) {
		printf("[Pico8] f_open('%s') failed: %d\n", filename_c_buffer.data(), (int)open_status);
		return FileOpenStatus::FAIL;
	}

	return FileOpenStatus::SUCCESS;
}

void fs_destroy_open_context(FileReaderContext &ctx) {
	if (!ctx.is_bios_read) {
		f_close(&ctx.reader.fs_reader);
	}
}

const char *fs_read_buffer(void *context, std::size_t *size) {
	FileReaderContext &real_context =
		*static_cast<FileReaderContext *>(context);

	if (real_context.is_bios_read) {
		emu::StringReader &bios_reader = real_context.reader.bios_reader;
		return bios_reader.get_reader()(size);
	} else {
		UINT br = 0;
		if (f_read(&real_context.reader.fs_reader,
		           real_context.read_buffer.data(),
		           real_context.read_buffer.size(), &br) != FR_OK) {
			*size = 0;
			return nullptr;
		}

		*size = br;
		return *size != 0 ? real_context.read_buffer.data() : nullptr;
	}

	return nullptr;
}

// FIXME: consistency around string_view and const char*, add a wrapper for
// const char* if that exists?

DirectoryListStatus fs_list_directory(DirectoryListCallback *callback, void *ud,
                                      const char *path) {
	DIR dir;
	FRESULT res = f_opendir(&dir, path != nullptr ? path : ".");

	if (res != FR_OK) {
		return DirectoryListStatus::FAIL;
	}

	for (;;) {
		FILINFO info;
		res = f_readdir(&dir, &info);
		if (res != FR_OK || info.fname[0] == '\0') {
			break;
		}

		FileInfo out_info{.kind = (info.fattrib & AM_DIR) != 0
		                              ? FileInfo::Kind::DIRECTORY
		                              : FileInfo::Kind::FILE,
		                  .name = static_cast<const char *>(info.fname)};

		callback(ud, out_info);
	}

	return DirectoryListStatus::SUCCESS;
}

std::uint32_t get_unique_seed() { return std::uint32_t(get_rand_32()); }

} // namespace hal