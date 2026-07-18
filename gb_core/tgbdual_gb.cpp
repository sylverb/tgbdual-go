/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Copyright (C) 2001  Hii

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

//-------------------------------------------------
// GB その他エミュレーション部/外部とのインターフェース
// Interface with external / other unit emulation GB

#include "gb.h"
#include "tgbdual_sgb.h"
#include <stdlib.h>

gb::gb(renderer *ref,bool b_lcd,bool b_apu)
{
	m_renderer=ref;

	m_lcd=new lcd(this);
	m_rom=new rom();
	m_apu=new apu(this);// ROMより後に作られたし // I was made ​​later than the ROM
	m_mbc=new mbc(this);
	m_cpu=new cpu(this);
	m_sgb=new sgb(this);
	console_mode=GB_CONSOLE_DMG;
#if CHEAT_CODES == 1
	m_cheat=new cheat(this);
#endif
	target=NULL;

	m_renderer->reset();
	m_renderer->set_sound_renderer(b_apu?m_apu->get_renderer():NULL);

	reset();

	hook_ext=false;
	use_gba=false;
}

gb::~gb()
{
	m_renderer->set_sound_renderer(NULL);

	delete m_sgb;
	delete m_mbc;
	delete m_rom;
	delete m_apu;
	delete m_lcd;
	delete m_cpu;
}

void gb::reset()
{
	regs.SC=0;
	regs.DIV=0;
	regs.TIMA=0;
	regs.TMA=0;
	regs.TAC=0;
	regs.LCDC=0x91;
	regs.STAT=0;
	regs.SCY=0;
	regs.SCX=0;
	regs.LY=153;
	regs.LYC=0;
	regs.BGP=0xFC;
	regs.OBP1=0xFF;
	regs.OBP2=0xFF;
	regs.WY=0;
	regs.WX=0;
	regs.IF=0;
	regs.IE=0;

	memset(&c_regs,0,sizeof(c_regs));

	if (m_rom->get_loaded())
		m_rom->get_info()->gb_type=resolve_gb_type();

	m_sgb->reset();
	m_sgb->set_enabled(m_rom->get_loaded() && m_rom->get_info()->gb_type==2);
	m_cpu->reset();
	m_lcd->reset();
	/* lcd::reset clears sgb_color_active — re-apply after. */
	if (m_sgb->enabled())
		m_sgb->push_palettes();
	m_apu->reset();
	m_mbc->reset();

	now_frame=0;
	skip=skip_buf=0;
	re_render=0;
	stat_irq_line=false;
	mode3_tracking=false;
	mode3_clock=0;
}

void gb::update_stat_irq()
{
	/* STAT interrupt line = OR of all enabled sources that are currently active.
	 * IRQ fires only on rising edge (STAT blocking / Altered Space).
	 * While LCD is off, PPU STAT sources are inactive (gambatte-style). */
	if (!(regs.LCDC&0x80)){
		stat_irq_line=false;
		return;
	}
	byte st=regs.STAT;
	byte mode=st&0x03;
	bool line=false;
	if ((st&0x08)&&mode==0) line=true;           /* HBlank */
	if ((st&0x10)&&mode==1) line=true;           /* VBlank mode */
	if ((st&0x20)&&mode==2) line=true;           /* OAM search */
	if ((st&0x40)&&(st&0x04)) line=true;         /* LYC=LY */
	if (line&&!stat_irq_line)
		m_cpu->irq(INT_LCDC);
	stat_irq_line=line;
}

void gb::hook_extport(ext_hook *ext)
{
	hook_proc=*ext;
	hook_ext=true;
}

void gb::unhook_extport()
{
	hook_ext=false;
}

void gb::set_skip(int frame)
{
	skip_buf=frame;
}

void gb::set_console_mode(int mode)
{
	console_mode=mode;
}

int gb::resolve_gb_type() const
{
	if (!m_rom->get_loaded())
		return 1;
	const byte *rom=m_rom->get_rom();
	byte cgb=rom[0x143];
	bool cgb_cart=(cgb&0x80)!=0;
	bool cgb_only=(cgb&0xC0)==0xC0;

	switch (console_mode){
	case GB_CONSOLE_DMG:
		return 1;
	case GB_CONSOLE_CGB:
		if (cgb_cart)
			return use_gba?4:3;
		return 1;
	case GB_CONSOLE_SGB:
		if (!cgb_only)
			return 2;
		return cgb_cart?(use_gba?4:3):1;
	default:
		return 1;
	}
}

bool gb::load_rom(byte *buf,int size,byte *ram,int ram_size, bool persistent)
{
	if (m_rom->load_rom(buf,size,ram,ram_size, persistent))
   {
		reset();
		return true;
	}
   return false;
}

void gb::serialize(serializer &s)
{
	s_VAR(regs);
	s_VAR(c_regs);

	m_rom->serialize(s);
	m_cpu->serialize(s);
	m_mbc->serialize(s);
	m_lcd->serialize(s);
	m_apu->serialize(s);
	m_sgb->serialize(s);
}

size_t gb::get_state_size(void)
{
	size_t ret = 0;
	serializer s(&ret, serializer::COUNT);
	serialize(s);
	return ret;
}

void gb::save_state_mem(void *buf)
{
	serializer s(buf, serializer::SAVE_BUF);
	serialize(s);
}

void gb::restore_state_mem(void *buf)
{
	serializer s(buf, serializer::LOAD_BUF);
	serialize(s);
	/* Remap SGB colours through the renderer after load. */
	if (m_sgb && m_sgb->enabled() && m_sgb->has_palette())
		m_sgb->push_palettes();
}

void gb::refresh_pal()
{
	for (int i=0;i<64;i++)
		m_lcd->get_mapped_pal(i>>2)[i&3]=m_renderer->map_color(m_lcd->get_pal(i>>2)[i&3]);
}

void gb::run()
{
	if (m_rom->get_loaded()){
		if (regs.LCDC&0x80){ // LCDC 起動時 // Startup LCDC
			regs.LY=(regs.LY+1)%154;

			regs.STAT&=0xF8;
			if (regs.LYC==regs.LY)
				regs.STAT|=4;
			update_stat_irq();

			if (regs.LY==0){
				m_renderer->refresh();
				if (now_frame>=skip){
					m_renderer->render_screen((byte*)vframe,160,144,16);
					now_frame=0;
				}
				else
					now_frame++;
				m_lcd->clear_win_count();
				skip=skip_buf;
			}
			if (regs.LY>=144){ // VBlank 期間中 // During VBlank
				regs.STAT=(regs.STAT&0xFC)|1;
				update_stat_irq();
				/* HDMA continues during VBlank (one 16-byte chunk per line). */
				if (m_cpu->dma_executing)
					m_cpu->do_hdma_chunk();
				if (regs.LY==144){
					if (m_sgb)
						m_sgb->on_new_frame();
					/* Altered Space polls LY==144 with VBlank IE enabled; it must
					 * observe LY before the ISR runs (handler is >1 line long).
					 * One successful LDH+CP+JR ≈ 28 cycles — allow ~32.
					 * Daedalian Opus still gets IF early enough for HALT/D008.
					 * Full line remains 456 cycles (not the old 448). */
					m_cpu->exec(32);
					m_cpu->irq(INT_VBLANK);
					m_cpu->exec(456-32);
				}
				else if (regs.LY==153){
					m_cpu->exec(80);
					regs.LY=0;
					// 前のラインのかなり早目から0になるようだ。
					// It's pretty early to be 0 from the previous line.
					m_cpu->exec(456-80);
					regs.LY=153;
				}
				else
					m_cpu->exec(456);
			}
			else{ // VBlank 期間外 // Period outside VBlank
				regs.STAT=(regs.STAT&0xFC)|2;
				update_stat_irq();

				/* Track from mode 2 so LYC/STAT-timed BGP spam is caught.
				 * Keep mode 3/0 lengths fixed (80+169+207) — variable mode 3
				 * desyncs Prehistorik's per-line SCX wave timing. */
				mode3_clock=0;
				mode3_tracking=true;
				m_lcd->begin_mode3((now_frame>=skip)?(void*)vframe:NULL,regs.LY);
				m_cpu->exec(80); // state=2

				regs.STAT|=3;
				update_stat_irq(); /* mode 3: usually drops STAT line */
				m_cpu->exec(169); // state=3
				mode3_tracking=false;

				if (m_cpu->dma_executing){ // HBlank DMA
					m_cpu->do_hdma_chunk();

					if (now_frame>=skip){
						if (!m_lcd->end_mode3(vframe,regs.LY))
							m_lcd->render(vframe,regs.LY);
					}
					else
						m_lcd->end_mode3(NULL,regs.LY);

					regs.STAT&=0xfc;
					update_stat_irq();
					m_cpu->exec(207); // state=0
				}
				else{
						regs.STAT&=0xfc;
						if (now_frame>=skip){
							if (!m_lcd->end_mode3(vframe,regs.LY))
								m_lcd->render(vframe,regs.LY);
						}
						else
							m_lcd->end_mode3(NULL,regs.LY);
						update_stat_irq();
						m_cpu->exec(207); // state=0
				}
			}
		}
		else{ // LCDC 停止時 // LCDC is stopped
			regs.LY=0;
			re_render++;
			if (re_render>=154){
				word blank=m_lcd->get_blank_color();
				word *dst=vframe;
				for (int i=160*144;i>0;i--)
					*(dst++)=blank;
				m_renderer->refresh();
				if (now_frame>=skip){
					m_renderer->render_screen((byte*)vframe,160,144,16);
					now_frame=0;
				}
				else
					now_frame++;
				m_lcd->clear_win_count();
				re_render=0;
			}
			/* Mode 0 while LCD off, but freeze LYC coincidence (Mr. Do). */
			regs.STAT&=0xFC;
			m_cpu->exec(456);
		}
	}
}
