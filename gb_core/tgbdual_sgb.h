/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Super Game Boy HLE: joypad packet bitbang, MLT_REQ,
   palettes/ATTR, and border (CHR_TRN / PCT_TRN).
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

/* Full SGB frame including border (SNES). GB screen at (48,40). */
#define SGB_BORDER_WIDTH  256
#define SGB_BORDER_HEIGHT 224
#define SGB_GB_X 48
#define SGB_GB_Y 40

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
	 * (MASK_EN, VRAM TRN, or border fade-out). */
	bool screen_blanked() const;

	bool has_border() const { return border_ready; }

	/* Composite border + GB frame into lcd (RGB565), centered 1:1 on 320×240. */
	void blit_frame(word *lcd, int lcd_w, int lcd_h,
	                const word *gb_rgb, int gb_w, int gb_h) const;

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
		TRN_BORDER_LOW = 3,
		TRN_BORDER_HIGH = 4,
		TRN_BORDER_MAP = 5,
	};

	/* Visible border vs next border being transferred (SameBoy-style). */
	struct border_gfx {
		byte tiles[0x100 * 32];
		word map[32 * 32];
		word pal[16 * 4];
	};

	void command_ready();
	void apply_pal_command(int first, int second);
	void apply_pal_set();
	void load_attribute_file(unsigned file_index);
	void do_vram_transfer();
	void tick_border_animation();
	static word fade_rgb15(word color, byte fade);

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

	/*
	 * Border change fade (SameBoy / real SGB2): after PCT_TRN, countdown
	 * from 105. Values >64 keep the old border; 64..33 fade it out; at 32
	 * the pending border is committed; 31..1 fade the new one in.
	 */
	byte border_animation;
	border_gfx border;
	border_gfx pending_border;
	bool border_ready;
	/* True after the first pending→border swap at fade mid-point. */
	bool border_committed;
};

#endif
