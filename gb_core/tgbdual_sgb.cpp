/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Super Game Boy HLE (detection + palettes + ATTR).
   Packet bitbang based on SameBoy / Pan Docs.
--------------------------------------------------*/

#include "gb.h"
#include "tgbdual_sgb.h"
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
	border_blank = 0;
	memset(ram_palettes, 0, sizeof(ram_palettes));
	memset(attribute_files, 0, sizeof(attribute_files));
	memset(attribute_map, 0, sizeof(attribute_map));
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
	if (border_blank != 0)
		return true;
	return false;
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
	/* Ignore malformed 0-length commands. */
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
	case SGB_PCT_TRN:
		/* Borders unsupported — freeze display so bitpatterns stay hidden. */
		vram_transfer_countdown = 3;
		transfer_dest = TRN_DISCARD;
		border_blank = 8;
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
	if (border_blank)
		border_blank--;
	if (vram_transfer_countdown == 0)
		return;
	if (--vram_transfer_countdown == 0)
		do_vram_transfer();
}

void sgb::do_vram_transfer()
{
	if (transfer_dest == TRN_DISCARD || transfer_dest == TRN_NONE)
		return;

	byte *vram = ref_gb->get_cpu()->get_vram();
	byte lcdc = ref_gb->get_regs()->LCDC;
	byte scx = ref_gb->get_regs()->SCX;
	byte scy = ref_gb->get_regs()->SCY;
	int map_base = (lcdc & 0x08) ? 0x1C00 : 0x1800;
	bool unsigned_tiles = (lcdc & 0x10) != 0;
	unsigned ntiles = (transfer_dest == TRN_ATTRIBUTES) ? 0xFE : 0x100;
	word tmp[0x100 * 8];
	word *out = tmp;

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
	s_VAR(border_blank);
}
