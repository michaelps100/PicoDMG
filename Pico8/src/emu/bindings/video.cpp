#include "video.hpp"
#include "hal/hal.hpp"

#include <cassert>
#include <concepts>
#include <lauxlib.hpp>
#include <lua.hpp>

#include <devices/buttonstate.hpp>
#include <devices/clippingrectangle.hpp>
#include <devices/drawpalette.hpp>
#include <devices/drawstatemisc.hpp>
#include <devices/image.hpp>
#include <devices/map.hpp>
#include <devices/screenpalette.hpp>
#include <devices/spriteflags.hpp>
#include <emu/emulator.hpp>
#include <util/point.hpp>
#include <video/defaultfont.hpp>

using util::Point;

namespace emu::bindings {

namespace detail {

// TODO: the raw_color naming is confusing:
// - one refers to having the fill pattern color in the upper nibble
// (set_pixel_with_pattern)
// - one refers to having the transparency info (set_pixel_with_alpha) (UPDATE:
// now to finish renaming to pen_color) it is currently past 1am and my brain
// infodumps better than it chooses names

inline void set_pixel(Point p, std::uint8_t pen_color) {
	const auto resolved_color =
		device<devices::DrawPalette>.get_color(pen_color & 0x0F);
	device<devices::Framebuffer>.set_pixel(p.x, p.y, resolved_color);
}

inline void draw_hline_solid(int x0, int x1, int y, std::uint8_t resolved_color) {
	const auto clip = device<devices::ClippingRectangle>;
	if (y < (int)clip.y_begin() || y >= (int)clip.y_end()) return;
	if (x0 > x1) std::swap(x0, x1);
	x0 = std::max(x0, (int)clip.x_begin());
	x1 = std::min(x1, (int)clip.x_end() - 1);
	if (x0 > x1) return;

	uint8_t *row = device<devices::Framebuffer>.raw_view.data() + (y * 64);
	const uint8_t c = resolved_color & 0x0Fu;
	const uint8_t pair = (uint8_t)((c << 4) | c);

	int x = x0;
	if (x & 1) {
		uint8_t &b = row[x >> 1];
		b = (b & 0x0Fu) | (uint8_t)(c << 4);
		x++;
	}

	const int full_bytes = (x1 - x + 1) / 2;
	if (full_bytes > 0) {
		std::memset(&row[x >> 1], pair, (size_t)full_bytes);
		x += full_bytes * 2;
	}

	if (x <= x1) {
		uint8_t &b = row[x >> 1];
		b = (b & 0xF0u) | c;
	}
}

inline void set_pixel_with_pattern(Point p, std::uint8_t raw_color) {
	auto draw_state = device<devices::DrawStateMisc>;
	const uint16_t pat = draw_state.fill_pattern();

	if (__builtin_expect(pat == 0 && !draw_state.fill_zero_is_transparent(), 1)) {
		set_pixel(p, raw_color & 0x0Fu);
		return;
	}

	const auto bit_index = (p.x & 0b11) | ((p.y & 0b11) << 2);
	if ((pat >> bit_index) & 0b1) {
		set_pixel(p, raw_color >> 4);
	} else if (!draw_state.fill_zero_is_transparent()) {
		set_pixel(p, raw_color & 0x0Fu);
	}
}

inline void draw_line(Point a, Point b, std::uint8_t raw_color) {
	const auto clip = device<devices::ClippingRectangle>;
	auto draw_state = device<devices::DrawStateMisc>;
	const bool is_solid = (draw_state.fill_pattern() == 0 && !draw_state.fill_zero_is_transparent());
	const uint8_t resolved_color = is_solid ?
		device<devices::DrawPalette>.get_color(raw_color & 0x0Fu) : 0;

	const bool steep = std::abs(b.y - a.y) > std::abs(b.x - a.x);

	if (steep) {
		a = a.yx();
		b = b.yx();
	}

	if (a.x > b.x) {
		std::swap(a, b);
	}

	const auto dx = b.x - a.x;
	const auto dy = std::abs(b.y - a.y);

	auto error = dx / 2;
	const auto y_step = (a.y < b.y) ? 1 : -1;

	int y = a.y;
	for (int x = a.x; x <= b.x; ++x) {
		Point plot_point(x, y);
		if (steep) {
			plot_point = plot_point.yx();
		}

		if (clip.contains(plot_point)) {
			if (is_solid) {
				device<devices::Framebuffer>.set_pixel(plot_point.x, plot_point.y, resolved_color);
			} else {
				detail::set_pixel_with_pattern(plot_point, raw_color);
			}
		}

		error -= dy;

		if (error < 0) {
			y += y_step;
			error += dx;
		}
	}
}

inline Point worldspace_to_screenspace(Point p) {
	const auto draw_misc = device<devices::DrawStateMisc>;
	return p.with_offset(-draw_misc.camera_x(), -draw_misc.camera_y());
}

inline Point screenspace_to_worldspace(Point p) {
	const auto draw_misc = device<devices::DrawStateMisc>;
	return p.with_offset(draw_misc.camera_x(), draw_misc.camera_y());
}

inline Point sprite_index_to_position(int sprite_index) {
	constexpr int sprites_per_row = 128 / 8;
	return {(sprite_index % sprites_per_row) * 8,
	        (sprite_index / sprites_per_row) * 8};
}

// aggressive inlining because there can be big opportunities for code
// simplification when some parameters are known
[[gnu::always_inline, gnu::flatten]]
inline void draw_sprite(Point sprite_origin, int sprite_width,
                        int sprite_height, Point target_origin,
                        int target_width, int target_height,
                        bool x_flip = false, bool y_flip = false) {
	auto sprite = device<devices::Spritesheet>;
	auto palette = device<devices::DrawPalette>;
	auto clip = device<devices::ClippingRectangle>;
	auto fb = device<devices::Framebuffer>;

	// convert from negative target size to positive target size, but with
	// flipped sprite coords
	if (target_width < 0) {
		x_flip = !x_flip;
		target_width = -target_width;
		target_origin.x -= target_width;
	}

	if (target_height < 0) {
		y_flip = !y_flip;
		target_height = -target_height;
		target_origin.y -= target_height;
	}

	if (target_width <= 0 || target_height <= 0 || sprite_width <= 0 || sprite_height <= 0) {
		return;
	}

	// compute screenspace coordinates (possibly out-of-bounds, incl. negative)
	const Point target_tl = target_origin;

	// compute screenspace clipped coordinates
	const Point clipped_tl = target_origin.max(clip.top_left());
	const Point clipped_br =
		target_origin.with_offset(target_width, target_height)
			.min(clip.bottom_right());

	if (clipped_tl.x >= clipped_br.x || clipped_tl.y >= clipped_br.y) {
		return;
	}

	// step value added to the sprite coordinate for each real pixel
	// this will be flipped later when using x_ or y_ flip
	LuaFix16 x_spr_step = 1, y_spr_step = 1;

	// only bother computing the divisions if doing a stretched draw
	if (sprite_width != target_width) {
		x_spr_step = LuaFix16(sprite_width) / LuaFix16(target_width);
	}

	if (sprite_height != target_height) {
		y_spr_step = LuaFix16(sprite_height) / LuaFix16(target_height);
	}

	// x/y_spr_start aren't sprite_origin, because we need to precalculate by
	// how many sprite pixels the clipping would have shifted
	LuaFix16 x_spr_start, y_spr_start; // NOLINT: initialized in all branches

	if (!x_flip) {
		x_spr_start = LuaFix16(sprite_origin.x) +
		              LuaFix16(clipped_tl.x - target_tl.x) * x_spr_step;
	} else {
		LuaFix16 step = x_spr_step;
		x_spr_step = -step;
		LuaFix16 x_spr_at_target_tl = LuaFix16(sprite_origin.x) +
		                              LuaFix16(target_width - 1) * step;
		x_spr_start = x_spr_at_target_tl +
		              LuaFix16(clipped_tl.x - target_tl.x) * x_spr_step;
	}

	if (!y_flip) {
		y_spr_start = LuaFix16(sprite_origin.y) +
		              LuaFix16(clipped_tl.y - target_tl.y) * y_spr_step;
	} else {
		LuaFix16 step = y_spr_step;
		y_spr_step = -step;
		LuaFix16 y_spr_at_target_tl = LuaFix16(sprite_origin.y) +
		                              LuaFix16(target_height - 1) * step;
		y_spr_start = y_spr_at_target_tl +
		              LuaFix16(clipped_tl.y - target_tl.y) * y_spr_step;
	}

	// Pre-cache palette lookup to eliminate 2 MMIO reads per pixel in inner loop
	uint8_t pal_resolved[16];
	bool pal_transparent[16];
	for (int i = 0; i < 16; ++i) {
		pal_resolved[i] = palette.get_color((uint8_t)i);
		pal_transparent[i] = palette.is_transparent((uint8_t)i);
	}

	auto sprite_y = y_spr_start;
	for (int y = clipped_tl.y; y < clipped_br.y; ++y) {
		auto sprite_x = x_spr_start;
		for (int x = clipped_tl.x; x < clipped_br.x; ++x) {
			const u8 sprite_color =
				sprite.get_pixel(int(sprite_x), int(sprite_y)) & 0x0Fu;

			if (!pal_transparent[sprite_color]) {
				fb.set_pixel(x, y, pal_resolved[sprite_color]);
			}

			sprite_x += x_spr_step;
		}
		sprite_y += y_spr_step;
	}
}

[[gnu::flatten]]
inline void draw_circle(Point origin, int radius,
                        std::invocable<Point> auto plot_point,
                        std::invocable<float, float, float> auto plot_hline) {
	// i don't know how this works but it should
	int f = 1 - radius;
	int ddf_x = 0;
	int ddf_y = -2 * radius;

	int x = 0;
	int y = radius;

	plot_point({origin.x, origin.y - radius});
	plot_point({origin.x, origin.y + radius});
	plot_hline(origin.x - radius, origin.x + radius, origin.y);

	while (x < y) {
		if (f >= 0) {
			--y;
			ddf_y += 2;
			f += ddf_y;
		}
		++x;

		ddf_x += 2;
		f += ddf_x + 1;

		plot_hline(origin.x - x, origin.x + x, origin.y + y);
		plot_hline(origin.x - x, origin.x + x, origin.y - y);
		plot_hline(origin.x - y, origin.x + y, origin.y + x);
		plot_hline(origin.x - y, origin.x + y, origin.y - x);
	}
}

template <typename PlotPixel>
inline void draw_ellipse_rect(int x0, int y0, int x1, int y1, PlotPixel &&plot) {
	int a = std::abs(x1 - x0);
	int b = std::abs(y1 - y0);
	int b1 = b & 1;

	if (x0 > x1) {
		std::swap(x0, x1);
	}
	if (y0 > y1) {
		std::swap(y0, y1);
	}

	if (a == 0 && b == 0) {
		plot(x0, y0);
		return;
	}
	if (a == 0) {
		for (int y = y0; y <= y1; ++y) {
			plot(x0, y);
		}
		return;
	}
	if (b == 0) {
		for (int x = x0; x <= x1; ++x) {
			plot(x, y0);
		}
		return;
	}

	int64_t a_step = 8LL * a * a;
	int64_t b_step = 8LL * b * b;
	int64_t dx = 4LL * (1 - a) * b * b;
	int64_t dy = 4LL * (b1 + 1) * a * a;
	int64_t err = dx + dy;

	y0 += (b + 1) / 2;
	y1 = y0 - b1;

	while (x0 <= x1) {
		plot(x1, y0);
		plot(x0, y0);
		plot(x0, y1);
		plot(x1, y1);
		int64_t e2 = 2 * err;
		if (e2 <= dy) {
			y0++;
			y1--;
			dy += a_step;
			err += dy;
		}
		if (e2 >= dx || 2 * err > dy) {
			x0++;
			x1--;
			dx += b_step;
			err += dx;
		}
	}

	while (y0 - y1 < b) {
		plot(x0 - 1, y0);
		plot(x1 + 1, y0++);
		plot(x0 - 1, y1);
		plot(x1 + 1, y1--);
	}
}

// returns the glyph width in pixels
inline int draw_glyph(Point origin, std::uint8_t glyph,
                      const std::uint8_t *font,
                      int font_w_low, int font_w_high, int font_h,
                      std::uint8_t pen_color) {
	const auto clip = device<devices::ClippingRectangle>;

	const auto glyph_offset = std::size_t(glyph) * 8;

	int glyph_width = (glyph >= 128) ? font_w_high : font_w_low;
	int glyph_height = font_h;

	for (int local_y = 0; local_y < glyph_height; ++local_y) {
		for (int local_x = 0; local_x < glyph_width; ++local_x) {
			const Point p = origin.with_offset(local_x, local_y);

			const bool pixel_set =
				(font[glyph_offset + local_y] >> local_x) & 0b1;

			if (pixel_set && clip.contains(p)) {
				set_pixel(p, pen_color);
			}
		}
	}

	return glyph_width;
}

// returns the new mouse cursor
inline Point draw_text(Point origin, std::string_view text,
                       const std::uint8_t *font,
                       int font_w_low, int font_w_high, int font_h,
                       std::uint8_t pen_color, bool terminal) {
	Point current_position = origin;

	int glyph_height = font_h;

	for (const char c : text) {
		int glyph_width = 0;
		uint8_t uc = static_cast<uint8_t>(c);

		// P8SCII handling
		switch (uc) {
		case 0: // '\0' => terminate printing
			goto terminate_printing;
		case 10: // '\n' => newline
			current_position.x = origin.x;
			current_position.y += glyph_height + 1;
			break;
		case 14: // '\14' => switch font defined at 0x5600
			break;
		case 15: // '\15' => switch font to default
			break;
		default: {
			if (uc < 32 && uc != 0) {
				// control characters not rendered as glyphs
				break;
			}
			glyph_width = draw_glyph(current_position, uc, font, font_w_low, font_w_high, font_h, pen_color);
			break;
		}
		}

		if (terminal) {
			if (current_position.x > 127 - glyph_width) {
				current_position.x = origin.x;
				current_position.y += glyph_height + 1;
			} else {
				current_position.x += glyph_width;
			}
		} else {
			current_position.x += glyph_width;
		}
	}

terminate_printing:
	return current_position;
}

enum class Palette {
	PEN_PALETTE = 0,
	SCREEN_PALETTE = 1,
	SCREEN_SECRET_PALETTE = 2
};

inline void set_palette_color(int source_color,
                              int target_color, Palette palette) {
	if (source_color < 0 || source_color > 15) {
		return;
	}

	std::uint8_t raw_target = 0;
	if (target_color < 0) {
		// PICO-8: -1..-16 map to secret colors 143..128 (0x8F..0x80)
		raw_target = (std::uint8_t)(128 + (16 + (target_color % 16)) % 16);
	} else if (target_color >= 16 && target_color <= 31) {
		// 16..31 map to secret colors 128..143 (0x80..0x8F)
		raw_target = (std::uint8_t)((target_color - 16) | 0x80);
	} else {
		raw_target = (std::uint8_t)target_color;
	}

	switch (palette) {
	case Palette::PEN_PALETTE:
		device<devices::DrawPalette>.set_color(source_color, raw_target);
		break;
	case Palette::SCREEN_PALETTE:
		device<devices::ScreenPalette>.set_raw_color(source_color,
		                                             raw_target);
		break;
	default:
		break;
	}
}

} // namespace detail

int y8_camera(lua_State *state) {
	const std::int16_t x = lua_tointeger(state, 1);
	const std::int16_t y = lua_tointeger(state, 2);

	device<devices::DrawStateMisc>.set_camera_position(Point(x, y));

	return 0;
}

int y8_color(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto &pen_color = device<devices::DrawStateMisc>.raw_pen_color();
	const auto old_pen_color = pen_color;

	if (argument_count >= 1) {
		pen_color = lua_tounsigned(state, 1);
	} else {
		pen_color = 6;
	}

	lua_pushinteger(state, old_pen_color);

	return 1;
}

int y8_pset(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const Point world_point(lua_tointeger(state, 1), lua_tointeger(state, 2));
	const Point p = detail::worldspace_to_screenspace(world_point);

	auto &color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 3) {
		color = lua_tounsigned(state, 3);
	}

	if (device<devices::ClippingRectangle>.contains(p)) {
		detail::set_pixel(p, color);
	}

	return 0;
}

int y8_pget(lua_State *state) {
	const Point world_point(lua_tointeger(state, 1), lua_tointeger(state, 2));
	const Point p = detail::worldspace_to_screenspace(world_point);

	std::uint8_t pixel = 0;
	if (device<devices::ClippingRectangle>.contains(p)) {
		pixel = device<devices::Framebuffer>.get_pixel(p.x, p.y);
	}

	lua_pushunsigned(state, pixel);

	return 1;
}

int y8_sset(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const Point p(lua_tointeger(state, 1), lua_tointeger(state, 2));
	auto &color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 3) {
		color = lua_tounsigned(state, 3);
	}

	if (p.x >= 0 && p.y >= 0 && p.x < 128 && p.y < 128) {
		device<devices::Spritesheet>.set_pixel(p.x, p.y, color);
	}

	return 0;
}

int y8_sget(lua_State *state) {
	const Point p(lua_tointeger(state, 1), lua_tointeger(state, 2));

	if (p.x >= 0 && p.y >= 0 && p.x < 128 && p.y < 128) {
		lua_pushunsigned(state,
		                 device<devices::Spritesheet>.get_pixel(p.x, p.y));
	} else {
		lua_pushunsigned(state, 0);
	}

	return 1;
}

int y8_fget(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const auto sprite_flags = device<devices::SpriteFlags>;

	// FIXME: how does this value wrap, too?
	const auto sprite_id = lua_tounsigned(state, 1);

	if (argument_count >= 2) {
		// FIXME: is this value wrapping %16 in pico-8?
		const auto flag_index = lua_tounsigned(state, 2);

		lua_pushboolean(state, sprite_flags.get_flag(sprite_id, flag_index));
		return 1;
	}

	lua_pushunsigned(state, sprite_flags.flags_for(sprite_id));
	return 1;
}

int y8_fset(lua_State *state) {
	const auto argument_count = lua_gettop(state);
	const auto sprite_flags = device<devices::SpriteFlags>;
	const auto sprite_id = lua_tounsigned(state, 1);

	if (argument_count >= 3) {
		const auto flag_index = lua_tounsigned(state, 2);
		bool val = false;
		if (lua_isboolean(state, 3)) {
			val = lua_toboolean(state, 3);
		} else {
			val = lua_tounsigned(state, 3) != 0;
		}
		sprite_flags.set_flag(sprite_id, flag_index, val);
		return 0;
	} else if (argument_count >= 2) {
		const auto val = lua_tounsigned(state, 2);
		sprite_flags.flags_for(sprite_id) = static_cast<std::uint8_t>(val);
		return 0;
	}
	return 0;
}

int y8_cls(lua_State *state) {
	std::uint8_t palette_entry = 0;

	const auto argument_count = lua_gettop(state);

	if (argument_count >= 1) {
		palette_entry = lua_tounsigned(state, 1);
	}

	device<devices::DrawStateMisc>.set_text_point({0, 0});
	device<devices::Framebuffer>.clear(palette_entry);

	return 0;
}

int y8_line(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const auto draw_misc = device<devices::DrawStateMisc>;

	auto &raw_color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 4) {
		if (argument_count >= 5) {
			raw_color = lua_tounsigned(state, 5);
		}

		const Point world_a(lua_tointeger(state, 1), lua_tointeger(state, 2));
		const Point world_b(lua_tointeger(state, 3), lua_tointeger(state, 4));

		detail::draw_line(detail::worldspace_to_screenspace(world_a),
		                  detail::worldspace_to_screenspace(world_b),
		                  raw_color);

		draw_misc.set_line_endpoint(world_b);
	} else if (argument_count >= 2) {
		if (argument_count >= 3) {
			raw_color = lua_tounsigned(state, 3);
		}

		const Point world_a(lua_tointeger(state, 1), lua_tointeger(state, 2));
		const Point world_b(draw_misc.line_endpoint_x(),
		                    draw_misc.line_endpoint_y());

		detail::draw_line(detail::worldspace_to_screenspace(world_a),
		                  detail::worldspace_to_screenspace(world_b),
		                  raw_color);

		draw_misc.set_line_endpoint(world_a);
	} else {
		if (argument_count >= 1) {
			raw_color = lua_tounsigned(state, 1);
		}

		draw_misc.set_line_endpoint_valid(false);
	}

	return 0;
}

int y8_circ(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const Point world_origin(lua_tointeger(state, 1), lua_tointeger(state, 2));
	const Point screen_origin = detail::worldspace_to_screenspace(world_origin);

	const auto clip = device<devices::ClippingRectangle>;

	int radius = 4;
	std::uint8_t raw_color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 3) {
		radius = lua_tointeger(state, 3);
	}

	if (argument_count >= 4) {
		raw_color = lua_tounsigned(state, 4);
	}

	const auto plot_point = [&](Point p) {
		if (clip.contains(p)) {
			detail::set_pixel_with_pattern(p, raw_color);
		}
	};

	detail::draw_circle(
		screen_origin, radius, [&](Point p) { plot_point(p); },
		[](int, int, int) {});

	return 0;
}

int y8_circfill(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const Point world_origin(lua_tointeger(state, 1), lua_tointeger(state, 2));
	const Point screen_origin = detail::worldspace_to_screenspace(world_origin);

	const auto clip = device<devices::ClippingRectangle>;

	int radius = 4;

	std::uint8_t raw_color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 3) {
		radius = lua_tointeger(state, 3);
	}

	// TODO: adjust radius depending on if 0x5f36 is set

	if (argument_count >= 4) {
		raw_color = lua_tounsigned(state, 4);
	}

	auto draw_state = device<devices::DrawStateMisc>;
	if (__builtin_expect(draw_state.fill_pattern() == 0 && !draw_state.fill_zero_is_transparent(), 1)) {
		const uint8_t resolved_color =
			device<devices::DrawPalette>.get_color(raw_color & 0x0Fu);

		detail::draw_circle(
			screen_origin, radius,
			[&](Point p) {
				if (clip.contains(p)) {
					device<devices::Framebuffer>.set_pixel(p.x, p.y, resolved_color);
				}
			},
			[&](float x1, float x2, float y) {
				detail::draw_hline_solid((int)x1, (int)x2, (int)y, resolved_color);
			});
	} else {
		const auto plot_point = [&](Point p) {
			if (clip.contains(p)) {
				detail::set_pixel_with_pattern(p, raw_color);
			}
		};

		detail::draw_circle(
			screen_origin, radius, [&](Point p) { plot_point(p); },
			[&](float x1, float x2, float y) {
				const int x_min = std::max(std::min((int)x1, (int)x2), int(clip.x_begin()));
				const int x_max = std::min(std::max((int)x1, (int)x2), int(clip.x_end() - 1));

				for (int x = x_min; x <= x_max; ++x) {
					detail::set_pixel_with_pattern({x, (int)y}, raw_color);
				}
			});
	}

	return 0;
}

int y8_rect(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto world_x0 = lua_tointeger(state, 1);
	auto world_y0 = lua_tointeger(state, 2);
	auto world_x1 = lua_tointeger(state, 3);
	auto world_y1 = lua_tointeger(state, 4);

	std::uint8_t raw_color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 5) {
		raw_color = lua_tounsigned(state, 5);
	}

	Point p0 = detail::worldspace_to_screenspace(Point(world_x0, world_y0));
	Point p1 = detail::worldspace_to_screenspace(Point(world_x1, world_y1));

	detail::draw_line(Point(p0.x, p0.y), Point(p1.x, p0.y), raw_color);
	detail::draw_line(Point(p1.x, p0.y), Point(p1.x, p1.y), raw_color);
	detail::draw_line(Point(p1.x, p1.y), Point(p0.x, p1.y), raw_color);
	detail::draw_line(Point(p0.x, p1.y), Point(p0.x, p0.y), raw_color);

	return 0;
}

int y8_rectfill(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto world_x0 = lua_tointeger(state, 1);
	auto world_y0 = lua_tointeger(state, 2);
	auto world_x1 = lua_tointeger(state, 3);
	auto world_y1 = lua_tointeger(state, 4);

	int min_x = std::min(world_x0, world_x1);
	int max_x = std::max(world_x0, world_x1);
	int min_y = std::min(world_y0, world_y1);
	int max_y = std::max(world_y0, world_y1);

	Point top_left =
		detail::worldspace_to_screenspace(Point(min_x, min_y));
	Point bottom_right =
		detail::worldspace_to_screenspace(Point(max_x, max_y));

	auto clip = device<devices::ClippingRectangle>;

	auto &raw_color = device<devices::DrawStateMisc>.raw_pen_color();

	if (argument_count >= 5) {
		raw_color = lua_tounsigned(state, 5);
	}

	top_left.x = std::max(top_left.x, int(clip.x_begin()));
	top_left.y = std::max(top_left.y, int(clip.y_begin()));
	bottom_right.x = std::min(bottom_right.x, int(clip.x_end() - 1));
	bottom_right.y = std::min(bottom_right.y, int(clip.y_end() - 1));

	if (top_left.x > bottom_right.x || top_left.y > bottom_right.y) {
		return 0;
	}

	auto draw_state = device<devices::DrawStateMisc>;
	if (__builtin_expect(draw_state.fill_pattern() == 0 && !draw_state.fill_zero_is_transparent(), 1)) {
		const uint8_t resolved_color =
			device<devices::DrawPalette>.get_color(raw_color & 0x0Fu);
		for (int y = top_left.y; y <= bottom_right.y; ++y) {
			detail::draw_hline_solid(top_left.x, bottom_right.x, y, resolved_color);
		}
	} else {
		for (int y = top_left.y; y <= bottom_right.y; ++y) {
			for (int x = top_left.x; x <= bottom_right.x; ++x) {
				detail::set_pixel_with_pattern(Point(x, y), raw_color);
			}
		}
	}

	return 0;
}

int y8_oval(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto world_x0 = lua_tointeger(state, 1);
	auto world_y0 = lua_tointeger(state, 2);
	auto world_x1 = lua_tointeger(state, 3);
	auto world_y1 = lua_tointeger(state, 4);

	auto &raw_color = device<devices::DrawStateMisc>.raw_pen_color();
	if (argument_count >= 5) {
		raw_color = lua_tounsigned(state, 5);
	}

	Point p0 = detail::worldspace_to_screenspace(Point(world_x0, world_y0));
	Point p1 = detail::worldspace_to_screenspace(Point(world_x1, world_y1));

	int x0 = p0.x;
	int y0 = p0.y;
	int x1 = p1.x;
	int y1 = p1.y;

	if (x0 > x1) std::swap(x0, x1);
	if (y0 > y1) std::swap(y0, y1);

	const auto clip = device<devices::ClippingRectangle>;
	if (x1 < (int)clip.x_begin() || x0 >= (int)clip.x_end() ||
	    y1 < (int)clip.y_begin() || y0 >= (int)clip.y_end()) {
		return 0;
	}

	const auto plot_point = [&](int x, int y) {
		if (clip.contains(Point(x, y))) {
			detail::set_pixel_with_pattern(Point(x, y), raw_color);
		}
	};

	detail::draw_ellipse_rect(x0, y0, x1, y1, plot_point);
	return 0;
}

int y8_ovalfill(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto world_x0 = lua_tointeger(state, 1);
	auto world_y0 = lua_tointeger(state, 2);
	auto world_x1 = lua_tointeger(state, 3);
	auto world_y1 = lua_tointeger(state, 4);

	auto &raw_color = device<devices::DrawStateMisc>.raw_pen_color();
	if (argument_count >= 5) {
		raw_color = lua_tounsigned(state, 5);
	}

	Point p0 = detail::worldspace_to_screenspace(Point(world_x0, world_y0));
	Point p1 = detail::worldspace_to_screenspace(Point(world_x1, world_y1));

	int x0 = p0.x;
	int y0 = p0.y;
	int x1 = p1.x;
	int y1 = p1.y;

	if (x0 > x1) std::swap(x0, x1);
	if (y0 > y1) std::swap(y0, y1);

	const auto clip = device<devices::ClippingRectangle>;
	if (x1 < (int)clip.x_begin() || x0 >= (int)clip.x_end() ||
	    y1 < (int)clip.y_begin() || y0 >= (int)clip.y_end()) {
		return 0;
	}

	int scan_y0 = std::max(y0, (int)clip.y_begin());
	int scan_y1 = std::min(y1, (int)clip.y_end() - 1);
	if (scan_y0 > scan_y1) {
		return 0;
	}

	int16_t span_min[128];
	int16_t span_max[128];
	for (int y = scan_y0; y <= scan_y1; ++y) {
		span_min[y] = 32767;
		span_max[y] = -32768;
	}

	const auto mark_span = [&](int x, int y) {
		if (y >= scan_y0 && y <= scan_y1) {
			if (x < span_min[y]) span_min[y] = (int16_t)x;
			if (x > span_max[y]) span_max[y] = (int16_t)x;
		}
	};

	detail::draw_ellipse_rect(x0, y0, x1, y1, mark_span);

	const int clip_x0 = (int)clip.x_begin();
	const int clip_x1 = (int)clip.x_end() - 1;

	auto draw_state = device<devices::DrawStateMisc>;
	if (__builtin_expect(draw_state.fill_pattern() == 0 && !draw_state.fill_zero_is_transparent(), 1)) {
		const uint8_t resolved_color =
			device<devices::DrawPalette>.get_color(raw_color & 0x0Fu);
		for (int y = scan_y0; y <= scan_y1; ++y) {
			if (span_min[y] <= span_max[y]) {
				int sx0 = std::max((int)span_min[y], clip_x0);
				int sx1 = std::min((int)span_max[y], clip_x1);
				if (sx0 <= sx1) {
					detail::draw_hline_solid(sx0, sx1, y, resolved_color);
				}
			}
		}
	} else {
		for (int y = scan_y0; y <= scan_y1; ++y) {
			if (span_min[y] <= span_max[y]) {
				int sx0 = std::max((int)span_min[y], clip_x0);
				int sx1 = std::min((int)span_max[y], clip_x1);
				for (int x = sx0; x <= sx1; ++x) {
					detail::set_pixel_with_pattern(Point(x, y), raw_color);
				}
			}
		}
	}

	return 0;
}

int y8_spr(lua_State *state) {
	// negative width does not flip the sprite, it just draws nothing

	const auto argument_count = lua_gettop(state);

	const auto sprite_index = lua_tounsigned(state, 1);

	const int world_origin_x = lua_tointeger(state, 2);
	const int world_origin_y = lua_tointeger(state, 3);
	const auto unclipped_origin = detail::worldspace_to_screenspace(
		Point(world_origin_x, world_origin_y));

	std::uint8_t width = 8, height = 8;
	bool x_flip = false, y_flip = false;

	// when providing only 4 arguments, pico-8 does not produce an error, but
	// the sprite is not visible we assume no game rely on this behavior (why
	// would any)

	if (argument_count >= 5) {
		width = std::uint8_t(luaL_checknumber(state, 4) * 8);
		height = std::uint8_t(luaL_checknumber(state, 5) * 8);
	}

	if (argument_count >= 6) {
		x_flip = lua_toboolean(state, 6);
	}

	if (argument_count >= 7) {
		y_flip = lua_toboolean(state, 7);
	}

	detail::draw_sprite(detail::sprite_index_to_position(sprite_index), width,
	                    height, unclipped_origin, width, height, x_flip,
	                    y_flip);

	return 0;
}

int y8_sspr(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const int sprite_origin_x = lua_tounsigned(state, 1);
	const int sprite_origin_y = lua_tounsigned(state, 2);
	const int sprite_width = lua_tounsigned(state, 3);
	const int sprite_height = lua_tounsigned(state, 4);
	const int world_origin_x = lua_tointeger(state, 5);
	const int world_origin_y = lua_tointeger(state, 6);
	int target_width = sprite_width;
	int target_height = sprite_height;

	const auto unclipped_origin = detail::worldspace_to_screenspace(
		Point(world_origin_x, world_origin_y));

	bool x_flip = false, y_flip = false;

	// when providing only 4 arguments, pico-8 does not produce an error, but
	// the sprite is not visible we assume no game rely on this behavior (why
	// would any)

	if (argument_count >= 7) {
		target_width = lua_tointeger(state, 7);
	}

	if (argument_count >= 8) {
		target_height = lua_tointeger(state, 8);
	}

	if (argument_count >= 9) {
		x_flip = lua_toboolean(state, 9);
	}

	if (argument_count >= 10) {
		y_flip = lua_toboolean(state, 10);
	}

	// printf("spr (%d, %d) size (%d, %d) orig (%d, %d) target (%d, %d)\n",
	//        sprite_origin_x, sprite_origin_y, sprite_width, sprite_height,
	//        world_origin_x, world_origin_y, target_width, target_height);

	detail::draw_sprite(Point(sprite_origin_x, sprite_origin_y), sprite_width,
	                    sprite_height, unclipped_origin, target_width,
	                    target_height, x_flip, y_flip);

	return 0;
}

int y8_pal(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	// TODO: double check that no args reset both palettes
	if (argument_count == 0) {
		device<devices::DrawPalette>.reset();
		device<devices::ScreenPalette>.reset();
		return 0;
	}

	std::uint8_t target_palette = 0;

	// first parameter is table? => pal(tbl, [target_palette])
	if (lua_istable(state, 1)) {
		if (argument_count >= 2) {
			target_palette = lua_tounsigned(state, 2);
		}

		lua_pushnil(state);
		while (lua_next(state, 1) != 0) {
			if (lua_isnumber(state, -2) && lua_isnumber(state, -1)) {
				int src = lua_tointeger(state, -2);
				int dst = lua_tointeger(state, -1);
				detail::set_palette_color(src, dst,
				                          detail::Palette(target_palette));
			}
			lua_pop(state, 1);
		}

		return 0;
	}

	const int source_color = lua_tointeger(state, 1);
	const int target_color = lua_tointeger(state, 2);

	if (argument_count >= 3) {
		target_palette = lua_tounsigned(state, 3);
	}

	detail::set_palette_color(source_color, target_color,
	                          detail::Palette(target_palette));

	return 0;
}

int y8_palt(lua_State *state) {
	const auto draw_palette = device<devices::DrawPalette>;

	const auto argument_count = lua_gettop(state);

	// case: palt(col, [transparent])
	if (argument_count >= 1) {
		const auto color = lua_tounsigned(state, 1) % 16;
		const auto is_transparent = lua_toboolean(state, 2);
		draw_palette.set_transparent(color, is_transparent);
	}
	// case: palt()
	else {
		draw_palette.set_transparent(0, true);
		for (std::size_t i = 1; i < 16; ++i) {
			draw_palette.set_transparent(i, false);
		}
		return 0;
	}

	return 0;
}

int y8_fillp(lua_State *state) {
	const auto draw_state_misc = device<devices::DrawStateMisc>;

	const auto argument_count = lua_gettop(state);

	if (argument_count >= 1) {
		LuaFix16 val = luaL_checknumber(state, 1);
		uint32_t raw = static_cast<uint32_t>(val.value);
		uint16_t pattern = static_cast<uint16_t>(raw >> 16);
		uint16_t frac = static_cast<uint16_t>(raw & 0xFFFF);

		uint8_t flags = 0;
		if (frac & (1u << 15)) flags |= (1u << 0);
		if (frac & (1u << 14)) flags |= (1u << 1);
		if (frac & (1u << 13)) flags |= (1u << 2);
		if (frac & (1u << 12)) flags |= (1u << 3);

		draw_state_misc.fill_pattern() = pattern;
		draw_state_misc.get_byte(0x33) = flags;
	} else {
		draw_state_misc.fill_pattern() = 0;
		draw_state_misc.get_byte(0x33) = 0;
	}

	return 0;
}

int y8_clip(lua_State *state) {
	const auto argument_count = lua_gettop(state);
	const auto clip = device<devices::ClippingRectangle>;

	lua_pushinteger(state, clip.x_begin());
	lua_pushinteger(state, clip.y_begin());
	lua_pushinteger(state, std::max(0, int(clip.x_end()) - int(clip.x_begin())));
	lua_pushinteger(state, std::max(0, int(clip.y_end()) - int(clip.y_begin())));

	if (argument_count >= 4) {
		int x0 = lua_tointeger(state, 1);
		int y0 = lua_tointeger(state, 2);
		int w  = lua_tointeger(state, 3);
		int h  = lua_tointeger(state, 4);

		if (w <= 0 || h <= 0) {
			clip.x_begin() = 0;
			clip.y_begin() = 0;
			clip.x_end() = 0;
			clip.y_end() = 0;
			return 4;
		}

		int x1 = x0 + w;
		int y1 = y0 + h;

		x0 = std::clamp(x0, 0, 128);
		y0 = std::clamp(y0, 0, 128);
		x1 = std::clamp(x1, 0, 128);
		y1 = std::clamp(y1, 0, 128);

		if (x1 < x0) x1 = x0;
		if (y1 < y0) y1 = y0;

		clip.x_begin() = static_cast<std::uint8_t>(x0);
		clip.y_begin() = static_cast<std::uint8_t>(y0);
		clip.x_end()   = static_cast<std::uint8_t>(x1);
		clip.y_end()   = static_cast<std::uint8_t>(y1);
	} else {
		clip.x_begin() = 0;
		clip.y_begin() = 0;
		clip.x_end() = 128;
		clip.y_end() = 128;
	}

	return 4;
}

int y8_mset(lua_State *state) {
	const auto x = lua_tointeger(state, 1);
	const auto y = lua_tointeger(state, 2);
	const std::uint8_t tile = lua_tounsigned(state, 3) % 256;

	const auto map = device<devices::Map>;

	if (map.in_bounds(x, y)) {
		map.tile(x, y) = tile;
	}

	return 0;
}

int y8_mget(lua_State *state) {
	const auto x = lua_tointeger(state, 1);
	const auto y = lua_tointeger(state, 2);

	const auto map = device<devices::Map>;

	if (map.in_bounds(x, y)) {
		lua_pushunsigned(state, map.tile(x, y));
	} else {
		lua_pushunsigned(state, 0);
	}

	return 1;
}

int y8_flip([[maybe_unused]] lua_State *state) {
	const auto btn_state = hal::update_button_state();
	emu::emulator.set_button_state(btn_state);
	device<devices::ButtonState>.for_player(0) = btn_state.held_key_mask;
	emu::emulator.flip();
	return 0;
}

int y8_map(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const auto sprite_flags = device<devices::SpriteFlags>;

	const auto tile_x_raw_origin = lua_tointeger(state, 1);
	const auto tile_y_raw_origin = lua_tointeger(state, 2);

	const Point world_origin(lua_tointeger(state, 3), lua_tointeger(state, 4));
	const Point screen_origin = detail::worldspace_to_screenspace(world_origin);

	int tile_width = 128;
	int tile_height = 64;

	unsigned layer_mask = 0;

	if (argument_count >= 7) {
		layer_mask = lua_tounsigned(state, 7);
	}

	if (argument_count >= 5) {
		tile_width = lua_tointeger(state, 5);
		tile_height = lua_tointeger(state, 6);

		if (tile_width < 0 || tile_height < 0) {
			return 0;
		}
	}

	const auto tile_x_origin = std::max(tile_x_raw_origin, 0);
	const auto tile_y_origin = std::max(tile_y_raw_origin, 0);
	const auto tile_x_end = std::min(tile_x_raw_origin + tile_width, 128);
	const auto tile_y_end = std::min(tile_y_raw_origin + tile_height, 64);

	const auto map = device<devices::Map>;

	for (int tile_y = tile_y_origin; tile_y < tile_y_end; ++tile_y) {
		for (int tile_x = tile_x_origin; tile_x < tile_x_end; ++tile_x) {
			const auto screen_x_offset =
				screen_origin.x + (tile_x - tile_x_raw_origin) * 8;
			const auto screen_y_offset =
				screen_origin.y + (tile_y - tile_y_raw_origin) * 8;

			const auto tile = map.tile(tile_x, tile_y);

			// is it the transparency tile?
			if (tile == 0) {
				continue;
			}

			// is it the layer we want to render?
			if ((sprite_flags.flags_for(tile) & layer_mask) != layer_mask) {
				continue;
			}

			detail::draw_sprite(detail::sprite_index_to_position(tile), 8, 8,
			                    Point(screen_x_offset, screen_y_offset), 8, 8);
		}
	}

	return 0;
}

int y8_cursor(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	const auto draw_misc = device<devices::DrawStateMisc>;

	lua_pushinteger(state, draw_misc.text_x());
	lua_pushinteger(state, draw_misc.text_y());
	lua_pushinteger(state, draw_misc.raw_pen_color());

	draw_misc.text_x() = lua_tointeger(state, 1);
	draw_misc.text_y() = lua_tointeger(state, 2);

	if (argument_count >= 3) {
		draw_misc.raw_pen_color() = lua_tointeger(state, 3);
	}

	return 3;
}

int y8_print(lua_State *state) {
	const auto draw_misc = device<devices::DrawStateMisc>;

	const auto argument_count = lua_gettop(state);

	// case: print()
	if (argument_count == 0) {
		return 0;
	}

	size_t str_len = 0;
	const char *str = luaL_tolstring(state, 1, &str_len);
	if (str == nullptr) {
		return 0;
	}

	auto &raw_color = device<devices::DrawStateMisc>.raw_pen_color();

	Point world_point;
	bool terminal_scrolling;

	// case: print(text, x, y, [col])
	if (argument_count >= 3) {
		if (argument_count >= 4) {
			raw_color = lua_tounsigned(state, 4);
		}

		world_point = Point(lua_tointeger(state, 2), lua_tointeger(state, 3));
		terminal_scrolling = false;
	}
	// case: print(text, [col])
	else {
		if (argument_count >= 2) {
			raw_color = lua_tounsigned(state, 2);
		}

		world_point = draw_misc.get_text_point();
		terminal_scrolling = true;
	}

	const uint8_t *font_ptr = video::pico8_builtin_font.data();
	int w_low = 4, w_high = 8, h = 6;

	// Check if custom font is enabled (bit 7 of 0x5F58)
	if ((emulator.memory().get_byte(0x5F58) & 0x80) != 0) {
		font_ptr = emulator.memory().raw_view.data() + 0x5600;
		uint8_t cfg_w_low = emulator.memory().get_byte(0x5600);
		uint8_t cfg_w_high = emulator.memory().get_byte(0x5601);
		uint8_t cfg_h = emulator.memory().get_byte(0x5602);
		if (cfg_w_low > 0) w_low = cfg_w_low;
		if (cfg_w_high > 0) w_high = cfg_w_high;
		if (cfg_h > 0) h = cfg_h;
	}

	const Point screen_point(detail::worldspace_to_screenspace(world_point));
	const Point updated_point =
		detail::screenspace_to_worldspace(detail::draw_text(
			screen_point, std::string_view{str, str_len}, font_ptr, w_low, w_high, h, raw_color, terminal_scrolling));
	draw_misc.set_text_point(updated_point);

	lua_pop(state, 1); // pop string pushed by luaL_tolstring
	return 0;
}

int y8_rgbpal(lua_State *state) {
	const auto argument_count = lua_gettop(state);

	auto &palette = emu::emulator.palette();

	// case: rgbpal(entry, [r], [g], [b])
	if (argument_count >= 1) {
		const unsigned entry = lua_tounsigned(state, 1) % 32;

		const auto old_value = palette[entry];

		if (argument_count >= 4) {
			const unsigned r = lua_tounsigned(state, 2) % 256;
			const unsigned g = lua_tounsigned(state, 3) % 256;
			const unsigned b = lua_tounsigned(state, 4) % 256;

			palette[entry] = (r << 16) | (g << 8) | b;

			hal::load_rgb_palette(palette);
		}

		lua_pushunsigned(state, (old_value >> 16) & 0xFF);
		lua_pushunsigned(state, (old_value >> 8) & 0xFF);
		lua_pushunsigned(state, old_value & 0xFF);

		return 3;
	}

	return 0;
}

} // namespace emu::bindings