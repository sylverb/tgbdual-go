/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Super Game Boy HLE (palettes, ATTR, border).
   Packet bitbang / transfers based on SameBoy / Pan Docs.
--------------------------------------------------*/

#include "gb.h"
#include "tgbdual_sgb.h"
#include "serializer.h"
#include <string.h>

enum {
	SGB_PAL01    = 0x00,
	SGB_PAL23    = 0x01,
	SGB_PAL03    = 0x02,
	SGB_PAL12    = 0x03,
	SGB_PAL_SET  = 0x0A,
	SGB_PAL_TRN  = 0x0B,
	SGB_MLT_REQ  = 0x11,
	SGB_CHR_TRN  = 0x13,
	SGB_PCT_TRN  = 0x14,
	SGB_ATTR_TRN = 0x15,
	SGB_ATTR_SET = 0x16,
	SGB_MASK_EN  = 0x17,
};

#define SGB_PACKET_BITS (16 * 8)

/* Measured on SGB2 (SameBoy); may be off by ±2. */
#define SGB_BORDER_FADE_FRAMES 105

sgb::sgb(gb *ref)
{
	ref_gb = ref;
	active = false;
	reset();
}

void sgb::reset()
{
	memset(command, 0, sizeof(command));
	command_bit = 0;
	ready_for_pulse = true;
	ready_for_write = false;
	ready_for_stop = false;
	player_count = 1;
	current_player = 0;
	palette_set = false;
	vram_transfer_countdown = 0;
	transfer_dest = TRN_NONE;
	mask = MASK_OFF;
	border_animation = 0;
	border_ready = false;
	border_committed = false;
	memset(ram_palettes, 0, sizeof(ram_palettes));
	memset(attribute_files, 0, sizeof(attribute_files));
	memset(attribute_map, 0, sizeof(attribute_map));
	memset(&border, 0, sizeof(border));
	memset(&pending_border, 0, sizeof(pending_border));
	static const word def[4] = { 0x7FFF, 0x56B5, 0x294A, 0x0000 };
	for (int p = 0; p < 4; p++)
		for (int c = 0; c < 4; c++)
			pal[p][c] = def[c];
}

void sgb::push_palettes()
{
	if (active)
		ref_gb->get_lcd()->apply_sgb_palettes(pal);
}

void sgb::set_enabled(bool on)
{
	active = on;
	if (!on)
		reset();
	else
		push_palettes();
}

bool sgb::screen_blanked() const
{
	if (!active)
		return false;
	if (mask != MASK_OFF)
		return true;
	if (vram_transfer_countdown != 0)
		return true;
	/* Freeze GB LCD during border fade-out (keep last good frame). */
	if (border_animation > 32)
		return true;
	return false;
}

word sgb::fade_rgb15(word color, byte fade)
{
	/* Saturating subtract per channel (SameBoy convert_rgb15_with_fade). */
	byte r = (byte)((color) & 0x1F);
	byte g = (byte)((color >> 5) & 0x1F);
	byte b = (byte)((color >> 10) & 0x1F);
	byte rd = (byte)(r - fade);
	byte gd = (byte)(g - fade);
	byte bd = (byte)(b - fade);
	if (rd >= 0x20) rd = 0;
	if (gd >= 0x20) gd = 0;
	if (bd >= 0x20) bd = 0;
	return (word)(rd | (gd << 5) | (bd << 10));
}

void sgb::tick_border_animation()
{
	if (border_animation == 0)
		return;
	border_animation--;
	if (border_animation == 32) {
		memcpy(&border, &pending_border, sizeof(border));
		border_committed = true;
	}
}

byte sgb::multipad_nibble() const
{
	if (!active || player_count <= 1)
		return 0x0F;
	return (byte)(0x0F - (current_player & 3));
}

void sgb::apply_pal_command(int first, int second)
{
	word shared = (word)(command[1] | (command[2] << 8));
	pal[0][0] = pal[1][0] = pal[2][0] = pal[3][0] = shared;

	for (int i = 0; i < 3; i++) {
		word c = (word)(command[3 + i * 2] | (command[4 + i * 2] << 8));
		pal[first][i + 1] = c;
	}
	for (int i = 0; i < 3; i++) {
		word c = (word)(command[9 + i * 2] | (command[10 + i * 2] << 8));
		pal[second][i + 1] = c;
	}
	palette_set = true;
	push_palettes();
}

void sgb::load_attribute_file(unsigned file_index)
{
	if (file_index > 0x2C)
		return;
	byte *output = attribute_map;
	for (unsigned i = 0; i < 90; i++) {
		byte b = attribute_files[file_index * 90 + i];
		for (int j = 0; j < 4; j++) {
			*(output++) = (byte)(b >> 6);
			b = (byte)(b << 2);
		}
	}
}

void sgb::apply_pal_set()
{
	for (int i = 0; i < 4; i++) {
		unsigned id = (unsigned)(command[1 + i * 2] |
		                         ((command[2 + i * 2] & 1) << 8));
		if (id > 511)
			id = 511;
		memcpy(pal[i], &ram_palettes[id * 4], sizeof(pal[i]));
	}
	pal[1][0] = pal[2][0] = pal[3][0] = pal[0][0];
	palette_set = true;
	push_palettes();
	if (command[9] & 0x80)
		load_attribute_file(command[9] & 0x3F);
	if (command[9] & 0x40)
		mask = MASK_OFF;
}

void sgb::command_ready()
{
	byte header = command[0];
	if ((header & 7) == 0)
		return;

	switch (header >> 3) {
	case SGB_PAL01:
		apply_pal_command(0, 1);
		break;
	case SGB_PAL23:
		apply_pal_command(2, 3);
		break;
	case SGB_PAL03:
		apply_pal_command(0, 3);
		break;
	case SGB_PAL12:
		apply_pal_command(1, 2);
		break;
	case SGB_PAL_SET:
		apply_pal_set();
		break;
	case SGB_PAL_TRN:
		vram_transfer_countdown = 3;
		transfer_dest = TRN_PALETTES;
		break;
	case SGB_MLT_REQ:
		player_count = (byte)((command[1] & 3) + 1);
		if (player_count == 3)
			player_count = 4;
		current_player = (byte)(current_player & (player_count - 1));
		break;
	case SGB_CHR_TRN:
		vram_transfer_countdown = 3;
		transfer_dest = (command[1] & 1) ? TRN_BORDER_HIGH : TRN_BORDER_LOW;
		break;
	case SGB_PCT_TRN:
		vram_transfer_countdown = 3;
		transfer_dest = TRN_BORDER_MAP;
		break;
	case SGB_ATTR_TRN:
		vram_transfer_countdown = 3;
		transfer_dest = TRN_ATTRIBUTES;
		break;
	case SGB_ATTR_SET:
		load_attribute_file(command[1] & 0x3F);
		if (command[1] & 0x40)
			mask = MASK_OFF;
		break;
	case SGB_MASK_EN:
		mask = (byte)(command[1] & 3);
		break;
	default:
		break;
	}
}

void sgb::on_new_frame()
{
	if (!active)
		return;
	if (vram_transfer_countdown != 0) {
		if (--vram_transfer_countdown == 0)
			do_vram_transfer();
	}
	tick_border_animation();
}

void sgb::do_vram_transfer()
{
	if (transfer_dest == TRN_NONE)
		return;

	byte *vram = ref_gb->get_cpu()->get_vram();
	byte lcdc = ref_gb->get_regs()->LCDC;
	byte scx = ref_gb->get_regs()->SCX;
	byte scy = ref_gb->get_regs()->SCY;
	int map_base = (lcdc & 0x08) ? 0x1C00 : 0x1800;
	bool unsigned_tiles = (lcdc & 0x10) != 0;

	unsigned ntiles = 0x100;
	word *dest = NULL;
	if (transfer_dest == TRN_ATTRIBUTES)
		ntiles = 0xFE;
	else if (transfer_dest == TRN_BORDER_MAP)
		ntiles = 0x88;
	else if (transfer_dest == TRN_BORDER_LOW)
		dest = (word *)pending_border.tiles;
	else if (transfer_dest == TRN_BORDER_HIGH)
		dest = (word *)pending_border.tiles + 0x800;

	word tmp[0x100 * 8];
	word *out = dest ? dest : tmp;

	for (unsigned tile = 0; tile < ntiles; tile++) {
		unsigned tile_x = (tile % 20) * 8;
		unsigned tile_y = (tile / 20) * 8;
		int map_x = ((int)scx + (int)tile_x) >> 3;
		int map_y = ((int)scy + (int)tile_y) >> 3;
		byte tile_id = vram[map_base + ((map_y & 31) * 32) + (map_x & 31)];
		byte *td;
		if (unsigned_tiles)
			td = vram + (unsigned)tile_id * 16;
		else
			td = vram + 0x1000 + (int)(signed char)tile_id * 16;
		for (int row = 0; row < 8; row++)
			*out++ = (word)(td[row * 2] | (td[row * 2 + 1] << 8));
	}

	if (transfer_dest == TRN_PALETTES)
		memcpy(ram_palettes, tmp, 0x1000);
	else if (transfer_dest == TRN_ATTRIBUTES)
		memcpy(attribute_files, tmp, sizeof(attribute_files));
	else if (transfer_dest == TRN_BORDER_MAP) {
		/* raw_data[0x440]: map[32*32] then palette[16*4] */
		memcpy(pending_border.map, tmp, sizeof(pending_border.map));
		memcpy(pending_border.pal, tmp + 32 * 32, sizeof(pending_border.pal));
		/* Start fade-out of the visible border, then swap at mid-fade. */
		border_animation = SGB_BORDER_FADE_FRAMES;
		border_ready = true;
	}
}

void sgb::blit_frame(word *lcd, int lcd_w, int lcd_h,
                     const word *gb_rgb, int gb_w, int gb_h) const
{
	/* Centered 1:1 SGB frame on the LCD. Render tile-by-tile: the first
	 * implementation decoded every border pixel and called virtual
	 * map_color() for it, which was much too expensive on STM32. */
	int ox = (lcd_w - SGB_BORDER_WIDTH) / 2;
	int oy = (lcd_h - SGB_BORDER_HEIGHT) / 2;
	if (ox < 0) ox = 0;
	if (oy < 0) oy = 0;

	word color0 = ref_gb->get_renderer()->map_color(pal[0][0]);
	word black = ref_gb->get_renderer()->map_color(0);

	/* Fade amount matches SameBoy after its per-frame decrement. */
	byte fade = 0;
	byte anim = border_animation;
	if (anim != 0 && anim <= 64) {
		if (anim > 32)
			fade = (byte)(64 - anim);
		else
			fade = anim;
	}

	word mapped_border_pal[16 * 4];
	for (unsigned i = 0; i < 16 * 4; i++) {
		word c = border.pal[i];
		if (fade)
			c = fade_rgb15(c, fade);
		mapped_border_pal[i] = ref_gb->get_renderer()->map_color(c);
	}

	/* Skip border tiles until the first PCT_TRN is committed, and during the
	 * initial fade-out of an empty border (color0 defaults to white). */
	bool draw_border = border_committed ||
	                   (border_animation > 0 && border_animation < 32);

	/* Margins outside the 256×224 SGB image. Cheap — only ~32+8 px strip. */
	for (int y = 0; y < oy; y++) {
		word *row = lcd + y * lcd_w;
		for (int x = 0; x < lcd_w; x++)
			row[x] = black;
	}
	for (int y = oy + SGB_BORDER_HEIGHT; y < lcd_h; y++) {
		word *row = lcd + y * lcd_w;
		for (int x = 0; x < lcd_w; x++)
			row[x] = black;
	}
	for (int y = oy; y < oy + SGB_BORDER_HEIGHT && y < lcd_h; y++) {
		word *row = lcd + y * lcd_w;
		for (int x = 0; x < ox; x++)
			row[x] = black;
		for (int x = ox + SGB_BORDER_WIDTH; x < lcd_w; x++)
			row[x] = black;
	}

	/* Before a border exists, fill the decorative ring black (not white).
	 * Once tiles are drawn they overwrite every pixel — no full clear. */
	if (!draw_border) {
		for (int y = 0; y < SGB_BORDER_HEIGHT && (oy + y) < lcd_h; y++) {
			/* Skip the GB window — filled below. */
			int ty = y / 8;
			word *row = lcd + (oy + y) * lcd_w + ox;
			if (ty >= 5 && ty < 23) {
				for (int x = 0; x < SGB_GB_X; x++)
					row[x] = black;
				for (int x = SGB_GB_X + gb_w; x < SGB_BORDER_WIDTH; x++)
					row[x] = black;
			} else {
				for (int x = 0; x < SGB_BORDER_WIDTH; x++)
					row[x] = black;
			}
		}
	} else {
	for (unsigned tile_y = 0; tile_y < 28; tile_y++) {
		for (unsigned tile_x = 0; tile_x < 32; tile_x++) {
			if (tile_x >= 6 && tile_x < 26 &&
			    tile_y >= 5 && tile_y < 23)
				continue;
			word tile = border.map[tile_x + tile_y * 32];
			byte flip_x = (tile & 0x4000) ? 0 : 7;
			byte flip_y = (tile & 0x8000) ? 7 : 0;
			byte pal_i = (byte)((tile >> 10) & 3);
			unsigned tile_base = (unsigned)(tile & 0xFF) * 32;

			for (unsigned y = 0; y < 8; y++) {
				word *dst = lcd + (oy + (int)tile_y * 8 + (int)y) * lcd_w +
				            ox + (int)tile_x * 8;
				if (tile & 0x300) {
					for (unsigned x = 0; x < 8; x++)
						dst[x] = color0;
					continue;
				}
				unsigned base = tile_base + (unsigned)(y ^ flip_y) * 2;
				for (unsigned x = 0; x < 8; x++) {
					byte bit = (byte)(1 << (x ^ flip_x));
					byte color = (byte)(((border.tiles[base] & bit) ? 1 : 0) |
					                    ((border.tiles[base + 1] & bit) ? 2 : 0) |
					                    ((border.tiles[base + 16] & bit) ? 4 : 0) |
					                    ((border.tiles[base + 17] & bit) ? 8 : 0));
					dst[x] = color ? mapped_border_pal[color + pal_i * 16]
					               : color0;
				}
			}
		}
	}
	}

	/* GB window. lcd::render freezes tgb_buffer while screen_blanked(), so
	 * gb_rgb already holds the last good frame during MASK/TRN/fade-out. */
	int gbx = ox + SGB_GB_X;
	int gby = oy + SGB_GB_Y;

	if (mask == MASK_BLACK) {
		for (int y = 0; y < gb_h; y++) {
			word *row = lcd + (gby + y) * lcd_w + gbx;
			for (int x = 0; x < gb_w; x++)
				row[x] = black;
		}
	} else if (mask == MASK_COLOR0) {
		for (int y = 0; y < gb_h; y++) {
			word *row = lcd + (gby + y) * lcd_w + gbx;
			for (int x = 0; x < gb_w; x++)
				row[x] = color0;
		}
	} else {
		for (int y = 0; y < gb_h; y++)
			memcpy(lcd + (gby + y) * lcd_w + gbx,
			       gb_rgb + y * gb_w, (size_t)gb_w * sizeof(word));
	}
}

void sgb::joyp_write(byte value)
{
	if (!active)
		return;

	word packets = (word)((command[0] & 7) ? (command[0] & 7) : 1);
	word command_size = (word)(packets * SGB_PACKET_BITS);

	byte old = ref_gb->get_regs()->P1;
	if ((value & 0x20) != 0 && (old & 0x20) == 0) {
		if ((player_count & 1) == 0) {
			current_player++;
			current_player &= (byte)(player_count - 1);
		}
	}

	switch ((value >> 4) & 3) {
	case 3:
		ready_for_pulse = true;
		break;

	case 2:
		if (!ready_for_pulse || !ready_for_write)
			return;
		if (ready_for_stop) {
			if (command_bit == command_size) {
				command_ready();
				command_bit = 0;
				memset(command, 0, sizeof(command));
			}
			ready_for_pulse = false;
			ready_for_write = false;
			ready_for_stop = false;
		} else {
			if (command_bit < (word)(sizeof(command) * 8)) {
				command_bit++;
				ready_for_pulse = false;
				if ((command_bit & (SGB_PACKET_BITS - 1)) == 0)
					ready_for_stop = true;
			}
		}
		break;

	case 1:
		if (!ready_for_pulse || !ready_for_write)
			return;
		if (ready_for_stop) {
			ready_for_pulse = false;
			ready_for_write = false;
			command_bit = 0;
			memset(command, 0, sizeof(command));
		} else {
			if (command_bit < (word)(sizeof(command) * 8)) {
				command[command_bit / 8] |= (byte)(1 << (command_bit & 7));
				command_bit++;
				ready_for_pulse = false;
				if ((command_bit & (SGB_PACKET_BITS - 1)) == 0)
					ready_for_stop = true;
			}
		}
		break;

	case 0:
		if (!ready_for_pulse)
			return;
		ready_for_write = true;
		ready_for_pulse = false;
		if ((command_bit & (SGB_PACKET_BITS - 1)) != 0 ||
		    command_bit == 0 ||
		    ready_for_stop) {
			command_bit = 0;
			memset(command, 0, sizeof(command));
			ready_for_stop = false;
		}
		break;
	}
}

void sgb::serialize(serializer &s)
{
	s_VAR(active);
	s_ARRAY(command);
	s_VAR(command_bit);
	s_VAR(ready_for_pulse);
	s_VAR(ready_for_write);
	s_VAR(ready_for_stop);
	s_VAR(player_count);
	s_VAR(current_player);
	s_ARRAY(pal);
	s_VAR(palette_set);
	s_ARRAY(ram_palettes);
	s_ARRAY(attribute_files);
	s_ARRAY(attribute_map);
	s_VAR(vram_transfer_countdown);
	s_VAR(transfer_dest);
	s_VAR(mask);
	s_VAR(border_animation);
	s_ARRAY(border.tiles);
	s_ARRAY(border.map);
	s_ARRAY(border.pal);
	s_ARRAY(pending_border.tiles);
	s_ARRAY(pending_border.map);
	s_ARRAY(pending_border.pal);
	s_VAR(border_ready);
	s_VAR(border_committed);
}
