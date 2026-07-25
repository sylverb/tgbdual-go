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
	regs.P1=0xCF; /* DMG/SGB boot value — required for SGB re-detection */
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
}

void gb::update_stat_irq()
{
	/* STAT interrupt line = OR of all enabled sources that are currently active.
	 * IRQ fires only on rising edge (STAT blocking / Altered Space / SF2).
	 * While LCD is off, PPU STAT sources are inactive (gambatte-style / Mr. Do). */
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

void gb::serialize(serializer &s, int version)
{
	s_VAR(regs);
	s_VAR(c_regs);
	if (version >= GB_SAVESTATE_V1)
		s_VAR(console_mode);

	m_rom->serialize(s);
	m_cpu->serialize(s);
	m_mbc->serialize(s, version);
	m_lcd->serialize(s, version);
	m_apu->serialize(s);
	/* SGB HLE blob (~29 KiB) only when running as SGB — omit for DMG/CGB. */
	if (version >= GB_SAVESTATE_V1 && console_mode == GB_CONSOLE_SGB)
		m_sgb->serialize(s);
}

size_t gb::get_state_size(int version)
{
	return get_state_size_for_type(version, m_rom->get_info()->gb_type);
}

size_t gb::get_state_size_for_type(int version, int gb_type)
{
	rom_info *info = m_rom->get_info();
	int old = info->gb_type;
	int old_mode = console_mode;
	info->gb_type = gb_type;
	/* COUNT must mirror serialize()'s SGB gate (console_mode == SGB). */
	if (version >= GB_SAVESTATE_V1) {
		if (gb_type == 2)
			console_mode = GB_CONSOLE_SGB;
		else if (gb_type >= 3)
			console_mode = GB_CONSOLE_CGB;
		else
			console_mode = GB_CONSOLE_DMG;
	}

	size_t ret = 0;
	serializer s(&ret, serializer::COUNT);
	serialize(s, version);

	info->gb_type = old;
	console_mode = old_mode;
	return ret;
}

void gb::save_state_mem(void *buf)
{
	serializer s(buf, serializer::SAVE_BUF);
	serialize(s, GB_SAVESTATE_V1);
}

void gb::restore_state_mem(void *buf)
{
	restore_state_mem(buf, GB_SAVESTATE_V1);
}

bool gb::restore_state_mem(void *buf, int version)
{
	if (version != GB_SAVESTATE_V0 && version != GB_SAVESTATE_V1)
		return false;

	serializer s(buf, serializer::LOAD_BUF);
	serialize(s, version);

	if (version == GB_SAVESTATE_V0) {
		/* v0 has no SGB blob — ensure HLE stays disabled. */
		if (m_sgb)
			m_sgb->set_enabled(false);

		/* console_mode was not in the file; derive it from restored gb_type. */
		int t = m_rom->get_info()->gb_type;
		if (t == 2)
			console_mode = GB_CONSOLE_SGB;
		else if (t >= 3)
			console_mode = GB_CONSOLE_CGB;
		else
			console_mode = GB_CONSOLE_DMG;
	} else if (console_mode != GB_CONSOLE_SGB) {
		/* v1 non-SGB saves omit the blob — drop any leftover HLE state. */
		if (m_sgb)
			m_sgb->set_enabled(false);
	} else if (m_sgb && m_sgb->enabled() && m_sgb->has_palette()) {
		m_sgb->push_palettes();
	}

	return true;
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
					if (m_sgb)
						m_sgb->on_new_frame();
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
				if (regs.LY==144){
					/* Street Fighter 2 / Altered Space: leave a short window so
					 * games can poll LY==144 before the VBlank ISR runs.
					 * Full line must stay 456 cycles (old path was 448). */
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

				/* Mid-scanline BGP tracking (Prehistorik Man). DMG/SGB only —
				 * CGB has no BGP mid-line effect we care about. Timing via
				 * cpu::total_clock snapshot (no per-op cost in exec). */
				bool track_bgp=m_rom->get_info()->gb_type<3;
				if (track_bgp)
					m_lcd->begin_mode3((now_frame>=skip)?(void*)vframe:NULL,regs.LY);

				m_cpu->exec(80); // state=2
				regs.STAT|=3;
				update_stat_irq(); /* mode 3: usually drops STAT line */
				m_cpu->exec(169); // state=3

				if (m_cpu->dma_executing){ // HBlank DMA
					if (m_cpu->b_dma_first){
						m_cpu->dma_dest_bank=m_cpu->vram_bank;
						if (m_cpu->dma_src<0x4000)
							m_cpu->dma_src_bank=m_rom->get_rom();
						else if (m_cpu->dma_src<0x8000)
							m_cpu->dma_src_bank=m_mbc->get_rom()-0x4000;
						else if (m_cpu->dma_src>=0xA000&&m_cpu->dma_src<0xC000)
							m_cpu->dma_src_bank=m_mbc->get_sram()-0xA000;
						else if (m_cpu->dma_src>=0xC000&&m_cpu->dma_src<0xD000)
							m_cpu->dma_src_bank=m_cpu->ram-0xC000;
						else if (m_cpu->dma_src>=0xD000&&m_cpu->dma_src<0xE000)
							m_cpu->dma_src_bank=m_cpu->ram_bank-0xD000;
						else m_cpu->dma_src_bank=NULL;
						m_cpu->b_dma_first=false;
					}
					memcpy(m_cpu->dma_dest_bank+(m_cpu->dma_dest&0x1ff0),m_cpu->dma_src_bank+m_cpu->dma_src,16);
//					fprintf(m_cpu->file,"%03d : dma exec %04X -> %04X rest %d\n",regs.LY,m_cpu->dma_src,m_cpu->dma_dest,m_cpu->dma_rest);

					m_cpu->dma_src+=16;
					m_cpu->dma_src&=0xfff0;
					m_cpu->dma_dest+=16;
					m_cpu->dma_dest&=0xfff0;
					m_cpu->dma_rest--;
					if (!m_cpu->dma_rest)
						m_cpu->dma_executing=false;

//					m_cpu->total_clock+=207*(m_cpu->speed?2:1);
//					m_cpu->sys_clock+=207*(m_cpu->speed?2:1);
//					m_cpu->div_clock+=207*(m_cpu->speed?2:1);
//					regs.STAT|=3;

					if (now_frame>=skip){
						if (!track_bgp||!m_lcd->end_mode3(vframe,regs.LY))
							m_lcd->render(vframe,regs.LY);
					}
					else if (track_bgp)
						m_lcd->end_mode3(NULL,regs.LY);

					regs.STAT&=0xfc;
					update_stat_irq();
					m_cpu->exec(207); // state=3
				}
				else{
/*					if (m_lcd->get_sprite_count()){
						if (m_lcd->get_sprite_count()>=10){
							m_cpu->exec(129);
							if ((regs.STAT&0x08))
								m_cpu->irq(INT_LCDC);
							regs.STAT&=0xfc;
							if (now_frame>=skip)
								m_lcd->render(vframe,regs.LY);
							m_cpu->exec(78); // state=0
						}
						else{
							m_cpu->exec(129*m_lcd->get_sprite_count()/10);
							if ((regs.STAT&0x08))
								m_cpu->irq(INT_LCDC);
							regs.STAT&=0xfc;
							if (now_frame>=skip)
								m_lcd->render(vframe,regs.LY);
							m_cpu->exec(207-(129*m_lcd->get_sprite_count()/10)); // state=0
						}
					}
					else{
*/						regs.STAT&=0xfc;
						if (now_frame>=skip){
							if (!track_bgp||!m_lcd->end_mode3(vframe,regs.LY))
								m_lcd->render(vframe,regs.LY);
						}
						else if (track_bgp)
							m_lcd->end_mode3(NULL,regs.LY);
						update_stat_irq();
						m_cpu->exec(207); // state=0
//					}
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
					if (m_sgb)
						m_sgb->on_new_frame();
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
