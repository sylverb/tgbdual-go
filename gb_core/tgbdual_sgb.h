/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Super Game Boy HLE: joypad packet bitbang, MLT_REQ
   detection, and palette commands (no borders).
--------------------------------------------------*/
#ifndef TGBDUAL_SGB_H
#define TGBDUAL_SGB_H

#include "gb_types.h"

class gb;
class serializer;

/* Console preference from the UI / settings. */
enum {
	GB_CONSOLE_DMG = 1,
	GB_CONSOLE_CGB = 2,
	GB_CONSOLE_SGB = 3,
};

class sgb
{
public:
	sgb(gb *ref);
	void reset();
	void set_enabled(bool on);
	bool enabled() const { return active; }

	void joyp_write(byte value);
	byte multipad_nibble() const;
	void on_new_frame();

	bool has_palette() const { return palette_set; }
	void push_palettes();
	const word *palette(int idx) const { return pal[idx & 3]; }

	/* 20×18 tile attribute map (palette 0–3 per BG tile). */
	const byte *attr_map() const { return attribute_map; }

	/* MASK_EN: 0=off, 1=freeze, 2=black, 3=color 0. */
	byte mask_mode() const { return mask; }

	/* True while live GB LCD must not update the framebuffer
	 * (MASK_EN freeze/black/color0, or border TRN holdoff). */
	bool screen_blanked() const;

	void serialize(serializer &s);

private:
	enum {
		MASK_OFF = 0,
		MASK_FREEZE = 1,
		MASK_BLACK = 2,
		MASK_COLOR0 = 3,
	};
	enum {
		TRN_NONE = 0,
		TRN_PALETTES = 1,
		TRN_ATTRIBUTES = 2,
		TRN_DISCARD = 3, /* CHR_TRN / PCT_TRN — timing only, no borders */
	};

	void command_ready();
	void apply_pal_command(int first, int second);
	void apply_pal_set();
	void load_attribute_file(unsigned file_index);
	void do_vram_transfer();

	gb *ref_gb;
	bool active;

	byte command[16 * 7];
	word command_bit;
	bool ready_for_pulse;
	bool ready_for_write;
	bool ready_for_stop;

	byte player_count;
	byte current_player;

	word pal[4][4];
	bool palette_set;

	word ram_palettes[512 * 4];
	byte attribute_files[0xFD2];
	byte attribute_map[20 * 18];
	byte vram_transfer_countdown;
	byte transfer_dest;
	byte mask;
	/* Extra freeze frames after border TRNs (pattern still in VRAM). */
	byte border_blank;
};

#endif
