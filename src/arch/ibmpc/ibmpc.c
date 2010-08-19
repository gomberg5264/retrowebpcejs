/*****************************************************************************
 * pce                                                                       *
 *****************************************************************************/

/*****************************************************************************
 * File name:   src/arch/ibmpc/ibmpc.c                                       *
 * Created:     1999-04-16 by Hampa Hug <hampa@hampa.ch>                     *
 * Copyright:   (C) 1999-2010 Hampa Hug <hampa@hampa.ch>                     *
 *****************************************************************************/

/*****************************************************************************
 * This program is free software. You can redistribute it and / or modify it *
 * under the terms of the GNU General Public License version 2 as  published *
 * by the Free Software Foundation.                                          *
 *                                                                           *
 * This program is distributed in the hope  that  it  will  be  useful,  but *
 * WITHOUT  ANY   WARRANTY,   without   even   the   implied   warranty   of *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU  General *
 * Public License for more details.                                          *
 *****************************************************************************/


#include "main.h"

#include <lib/iniram.h>
#include <lib/initerm.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <devices/video/mda.h>
#include <devices/video/hgc.h>
#include <devices/video/cga.h>
#include <devices/video/ega.h>
#include <devices/video/vga.h>
#include <devices/video/plantronics.h>
#include <devices/video/wy700.h>

#include <lib/inidsk.h>
#include <lib/iniram.h>
#include <lib/load.h>
#include <lib/string.h>


void pc_e86_hook (void *ext, unsigned char op1, unsigned char op2);


static
unsigned char pc_get_port8 (ibmpc_t *pc, unsigned long addr)
{
#ifdef DEBUG_PORTS
	pc_log_deb (pc, "get port 8 %04lX\n", addr);
#endif

	return (0xff);
}

static
unsigned short pc_get_port16 (ibmpc_t *pc, unsigned long addr)
{
#ifdef DEBUG_PORTS
	pc_log_deb (pc, "get port 16 %04lX\n", addr);
#endif

	return (0xffff);
}

static
void pc_set_port8 (ibmpc_t *pc, unsigned long addr, unsigned char val)
{
#ifdef DEBUG_PORTS
	pc_log_deb (pc, "set port 8 %04lX <- %02X\n", addr, val);
#endif

	switch (addr) {
	case 0x0081:
		pc->dma_page[2] = (unsigned long) (val & 0x0f) << 16;
		break;

	case 0x0082:
		pc->dma_page[3] = (unsigned long) (val & 0x0f) << 16;
		break;

	case 0x0083:
		pc->dma_page[1] = (unsigned long) (val & 0x0f) << 16;
		break;
	}
}

static
void pc_set_port16 (ibmpc_t *pc, unsigned long addr, unsigned short val)
{
#ifdef DEBUG_PORTS
	pc_log_deb (pc, "set port 16 %04lX <- %04X\n", addr, val);
#endif
}

static
unsigned char pc_ppi_get_port_a (ibmpc_t *pc)
{
	if (pc->ppi_port_b & 0x80) {
		return (pc->ppi_port_a[0]);
	}
	else {
		pc->ppi_port_a[1] = pc_kbd_get_key (&pc->kbd);

		return (pc->ppi_port_a[1]);
	}
}

static
unsigned char pc_ppi_get_port_c (ibmpc_t *pc)
{
	if (pc->cas != NULL) {
		if (pc_cas_get_inp (pc->cas)) {
			pc->ppi_port_c[0] |= 0x10;
			pc->ppi_port_c[1] |= 0x10;
		}
		else {
			pc->ppi_port_c[0] &= ~0x10;
			pc->ppi_port_c[1] &= ~0x10;
		}
	}

	if (pc->ppi_port_b & 0x04) {
		return (pc->ppi_port_c[0]);
	}
	else {
		return (pc->ppi_port_c[1]);
	}
}

static
void pc_ppi_set_port_b (ibmpc_t *pc, unsigned char val)
{
	unsigned char old;

	old = pc->ppi_port_b;
	pc->ppi_port_b = val;

	pc_kbd_set_clk (&pc->kbd, val & 0x40);
	pc_kbd_set_enable (&pc->kbd, (val & 0x80) == 0);

	e8253_set_gate (&pc->pit, 2, val & 0x01);

	if ((old ^ val) & 0x08) {
		/* cassette motor change */

		if (pc->cas != NULL) {
			pc_cas_set_motor (pc->cas, (val & 0x08) == 0);

			if (val & 0x08) {
				/* motor off: restore clock */
				pc->speed_current = pc->speed_saved;
			}
			else {
				/* motor on: set clock to 4.77 MHz */
				pc->speed_saved = pc->speed_current;
				pc->speed_current = 1;
			}
		}
	}
}

static
void pc_set_timer2_out (ibmpc_t *pc, unsigned char val)
{
	if (val) {
		pc->ppi_port_c[0] |= 0x20;
		pc->ppi_port_c[1] |= 0x20;
	}
	else {
		pc->ppi_port_c[0] &= ~0x20;
		pc->ppi_port_c[1] &= ~0x20;
	}

	if (pc->cas != NULL) {
		pc_cas_set_out (pc->cas, val);
	}
}

static
void pc_setup_mem (ibmpc_t *pc, ini_sct_t *ini)
{
	pc->mem = mem_new();

	ini_get_ram (pc->mem, ini, &pc->ram);
	ini_get_rom (pc->mem, ini);
}

static
void pc_setup_ports (ibmpc_t *pc, ini_sct_t *ini)
{
	pc->prt = mem_new();

	mem_set_fct (pc->prt, pc,
		pc_get_port8, pc_get_port16, NULL,
		pc_set_port8, pc_set_port16, NULL
	);
}

static
void pc_setup_nvram (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	const char    *fname;
	unsigned long base, size;

	pc->nvr = NULL;

	sct = ini_next_sct (ini, NULL, "nvram");
	if (sct == NULL) {
		return;
	}

	ini_get_string (sct, "file", &fname, NULL);
	ini_get_uint32 (sct, "base", &base, 0);
	ini_get_uint32 (sct, "size", &size, 65536);

	pce_log_tag (MSG_INF, "NVRAM:", "addr=0x%08lx size=%lu file=%s\n",
		base, size, (fname == NULL) ? "<>" : fname
	);

	pc->nvr = nvr_new (base, size);
	if (pc->nvr == NULL) {
		pce_log (MSG_ERR, "*** creating nvram failed\n");
		return;
	}

	nvr_set_endian (pc->nvr, 0);

	mem_add_blk (pc->mem, nvr_get_mem (pc->nvr), 0);

	if (fname != NULL) {
		if (nvr_set_fname (pc->nvr, fname)) {
			pce_log (MSG_ERR, "*** loading nvram failed (%s)\n", fname);
		}
	}
}

static
void pc_setup_cpu (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	const char    *model;
	unsigned      speed;

	sct = ini_next_sct (ini, NULL, "cpu");

	ini_get_string (sct, "model", &model, "8088");
	ini_get_uint16 (sct, "speed", &speed, 1);

	if (par_speed > 0) {
		speed = par_speed;
	}
	else if (speed == 0) {
		speed = 1;
	}

	if (par_cpu != NULL) {
		model = par_cpu;
	}

	pce_log_tag (MSG_INF, "CPU:", "model=%s speed=%uX\n",
		model, speed
	);

	pc->cpu = e86_new();

	if (pc_set_cpu_model (pc, model)) {
		pce_log (MSG_ERR, "*** unknown cpu model (%s)\n", model);
	}

	e86_set_mem (pc->cpu, pc->mem,
		(e86_get_uint8_f) &mem_get_uint8,
		(e86_set_uint8_f) &mem_set_uint8,
		(e86_get_uint16_f) &mem_get_uint16_le,
		(e86_set_uint16_f) &mem_set_uint16_le
	);

	e86_set_prt (pc->cpu, pc->prt,
		(e86_get_uint8_f) &mem_get_uint8,
		(e86_set_uint8_f) &mem_set_uint8,
		(e86_get_uint16_f) &mem_get_uint16_le,
		(e86_set_uint16_f) &mem_set_uint16_le
	);

	if (pc->ram != NULL) {
		e86_set_ram (pc->cpu, pc->ram->data, pc->ram->size);
	}
	else {
		e86_set_ram (pc->cpu, NULL, 0);
	}

	pc->cpu->op_ext = pc;
	pc->cpu->op_hook = &pc_e86_hook;

	pc->speed_current = speed;
	pc->speed_default = speed;
	pc->speed_saved = speed;
}

static
void pc_setup_dma (ibmpc_t *pc, ini_sct_t *ini)
{
	unsigned long addr;
	ini_sct_t     *sct;
	mem_blk_t     *blk;

	sct = ini_next_sct (ini, NULL, "dmac");

	ini_get_uint32 (sct, "address", &addr, 0);

	pce_log_tag (MSG_INF, "DMAC:", "addr=0x%08x size=0x%04x\n", addr, 16);

	pc->dma_page[0] = 0;
	pc->dma_page[1] = 0;
	pc->dma_page[2] = 0;
	pc->dma_page[3] = 0;

	e8237_init (&pc->dma);

	blk = mem_blk_new (addr, 16, 0);
	if (blk == NULL) {
		pce_log (MSG_ERR, "*** allocating DMAC failed\n");
		return;
	}

	mem_blk_set_fct (blk, &pc->dma,
		e8237_get_uint8, e8237_get_uint16, e8237_get_uint32,
		e8237_set_uint8, e8237_set_uint16, e8237_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);

	/* This is a hack. HLDA should be connected to the CPU core. Instead,
	 * this will keep it permanently at high. */
	e8237_set_hlda (&pc->dma, 1);
}

static
void pc_setup_pic (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	mem_blk_t     *blk;
	unsigned long addr;

	sct = ini_next_sct (ini, NULL, "pic");

	ini_get_uint32 (sct, "address", &addr, 0x0020);

	pce_log_tag (MSG_INF, "PIC:", "addr=0x%08x size=0x%04x\n", addr, 2);

	e8259_init (&pc->pic);

	blk = mem_blk_new (addr, 2, 0);
	if (blk == NULL) {
		return;
	}

	mem_blk_set_fct (blk, &pc->pic,
		e8259_get_uint8, e8259_get_uint16, e8259_get_uint32,
		e8259_set_uint8, e8259_set_uint16, e8259_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);

	e8259_set_int_fct (&pc->pic, pc->cpu, e86_irq);

	e86_set_inta_fct (pc->cpu, &pc->pic, e8259_inta);
}

static
void pc_setup_pit (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	mem_blk_t     *blk;
	unsigned long addr;

	sct = ini_next_sct (ini, NULL, "pit");

	ini_get_uint32 (sct, "address", &addr, 0x0040);

	pce_log_tag (MSG_INF, "PIT:", "addr=0x%08lx size=0x%04x\n",
		addr, 4
	);

	e8253_init (&pc->pit);

	blk = mem_blk_new (addr, 4, 0);
	if (blk == NULL) {
		return;
	}

	mem_blk_set_fct (blk, &pc->pit,
		e8253_get_uint8, e8253_get_uint16, e8253_get_uint32,
		e8253_set_uint8, e8253_set_uint16, e8253_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);

	e8253_set_gate (&pc->pit, 0, 1);
	e8253_set_gate (&pc->pit, 1, 1);
	e8253_set_gate (&pc->pit, 2, 1);

	e8253_set_out_fct (&pc->pit, 0, &pc->pic, e8259_set_irq0);
	e8253_set_out_fct (&pc->pit, 1, &pc->dma, e8237_set_dreq0);
	e8253_set_out_fct (&pc->pit, 2, pc, pc_set_timer2_out);
}

static
void pc_setup_ppi (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	mem_blk_t     *blk;
	unsigned long addr;
	unsigned long  ram;

	sct = ini_next_sct (ini, NULL, "ppi");

	if (pc->ram != NULL) {
		ram = mem_blk_get_size (pc->ram);
	}
	else {
		ram = 65536;
	}

	ram = (ram < 65536) ? 0 : ((ram - 65536) / 32768);

	ini_get_uint32 (sct, "address", &addr, 0x0060);

	pce_log_tag (MSG_INF, "PPI:", "addr=0x%08x size=0x%04x\n", addr, 4);

	e8255_init (&pc->ppi);

	pc->ppi.port[0].read_ext = pc;
	pc->ppi.port[0].read = (void *) pc_ppi_get_port_a;
	pc->ppi.port[1].write_ext = pc;
	pc->ppi.port[1].write = (void *) pc_ppi_set_port_b;
	pc->ppi.port[2].read_ext = pc;
	pc->ppi.port[2].read = (void *) pc_ppi_get_port_c;

	pc->ppi_port_a[0] = 0x30 | 0x0c;
	pc->ppi_port_a[1] = 0;
	pc->ppi_port_b = 0x08;
	pc->ppi_port_c[0] = (ram & 0x0f);
	pc->ppi_port_c[1] = (ram >> 4) & 0x01;

	blk = mem_blk_new (addr, 4, 0);
	if (blk == NULL) {
		return;
	}

	mem_blk_set_fct (blk, &pc->ppi,
		e8255_get_uint8, e8255_get_uint16, e8255_get_uint32,
		e8255_set_uint8, e8255_set_uint16, e8255_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);
}

static
void pc_setup_kbd (ibmpc_t *pc, ini_sct_t *ini)
{
	pc_kbd_init (&pc->kbd);

	pc_kbd_set_irq_fct (&pc->kbd, &pc->pic, e8259_set_irq1);
}

static
void pc_setup_cassette (ibmpc_t *pc, ini_sct_t *ini)
{
	const char    *fname;
	const char    *mode;
	unsigned long pos;
	int           enable, append, pcm, filter;
	ini_sct_t     *sct;

	pc->cas = NULL;

	sct = ini_next_sct (ini, NULL, "cassette");

	if (sct == NULL) {
		return;
	}

	ini_get_bool (sct, "enable", &enable, 1);

	if (enable == 0) {
		return;
	}

	ini_get_string (sct, "file", &fname, NULL);
	ini_get_string (sct, "mode", &mode, "load");
	ini_get_uint32 (sct, "position", &pos, 0);
	ini_get_bool (sct, "append", &append, 0);
	ini_get_bool (sct, "filter", &filter, 1);

	if (ini_get_bool (sct, "pcm", &pcm, 0)) {
		pcm = -1;
	}

	pce_log_tag (MSG_INF, "CASSETTE:",
		"file=%s mode=%s pcm=%d filter=%d pos=%lu append=%d\n",
		(fname != NULL) ? fname : "<none>",
		mode, pcm, filter, pos, append
	);

	pc->cas = pc_cas_new();

	if (pc->cas == NULL) {
		pce_log (MSG_ERR, "*** alloc failed\n");
		return;
	}

	if (pc_cas_set_fname (pc->cas, fname)) {
		pce_log (MSG_ERR, "*** opening file failed (%s)\n", fname);
	}

	if (strcmp (mode, "load") == 0) {
		pc_cas_set_mode (pc->cas, 0);
	}
	else if (strcmp (mode, "save") == 0) {
		pc_cas_set_mode (pc->cas, 1);
	}
	else {
		pce_log (MSG_ERR, "*** unknown cassette mode (%s)\n", mode);
	}

	if (append) {
		pc_cas_append (pc->cas);
	}
	else {
		pc_cas_set_position (pc->cas, pos);
	}

	if (pcm >= 0) {
		pc_cas_set_pcm (pc->cas, pcm);
	}

	pc_cas_set_filter (pc->cas, filter);
}

static
void pc_setup_terminal (ibmpc_t *pc, ini_sct_t *ini)
{
	pc->trm = ini_get_terminal (ini, par_terminal);

	if (pc->trm == NULL) {
		return;
	}

	trm_set_key_fct (pc->trm, &pc->kbd, pc_kbd_set_key);
	trm_set_msg_fct (pc->trm, pc, pc_set_msg);
}

static
int pc_setup_mda (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = mda_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x30;

	return (0);
}

static
int pc_setup_plantronics (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = pla_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x20;

	return (0);
}

static
int pc_setup_wy700 (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = wy700_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x20;

	return (0);
}

static
int pc_setup_hgc (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = hgc_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x30;

	return (0);
}

static
int pc_setup_cga (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = cga_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x20;

	return (0);
}

static
int pc_setup_ega (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = ega_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x00;

	return (0);
}

static
int pc_setup_vga (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = vga_new_ini (sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x00;

	return (0);
}

static
void pc_setup_video (ibmpc_t *pc, ini_sct_t *ini)
{
	const char *dev;
	ini_sct_t  *sct;

	pc->video = NULL;

	sct = ini_next_sct (ini, NULL, "video");
	ini_get_string (sct, "device", &dev, "cga");

	if (par_video != NULL) {
		while ((sct != NULL) && (strcmp (par_video, dev) != 0)) {
			sct = ini_next_sct (ini, sct, "video");
			ini_get_string (sct, "device", &dev, "cga");
		}

		if (sct == NULL) {
			dev = par_video;
		}
	}

	pce_log_tag (MSG_INF, "VIDEO:", "device=%s\n", dev);

	if (strcmp (dev, "vga") == 0) {
		pc_setup_vga (pc, sct);
	}
	else if (strcmp (dev, "ega") == 0) {
		pc_setup_ega (pc, sct);
	}
	else if (strcmp (dev, "cga") == 0) {
		pc_setup_cga (pc, sct);
	}
	else if (strcmp (dev, "hgc") == 0) {
		pc_setup_hgc (pc, sct);
	}
	else if (strcmp (dev, "mda") == 0) {
		pc_setup_mda (pc, sct);
	}
	else if (strcmp (dev, "plantronics") == 0) {
		pc_setup_plantronics (pc, sct);
	}
	else if ((strcmp (dev, "wyse") == 0) || (strcmp (dev, "wy700") == 0)) {
		pc_setup_wy700 (pc, sct);
	}
	else {
		pce_log (MSG_ERR, "*** unknown video device (%s)\n", dev);
	}

	if (pc->video == NULL) {
		pce_log (MSG_ERR, "*** setting up video device failed\n");
	}

	if (pc->video != NULL) {
		ini_get_ram (pc->mem, sct, &pc->ram);
		ini_get_rom (pc->mem, sct);
		pce_load_mem_ini (pc->mem, sct);

		if (pc->trm != NULL) {
			pce_video_set_terminal (pc->video, pc->trm);
		}
	}
}

static
void pc_setup_disks (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t *sct;
	disk_t    *dsk;

	pc->dsk = dsks_new();

	sct = NULL;
	while ((sct = ini_next_sct (ini, sct, "disk")) != NULL) {
		if (ini_get_disk (sct, &dsk)) {
			pce_log (MSG_ERR, "*** loading drive failed\n");
			continue;
		}

		if (dsk == NULL) {
			continue;
		}

		dsks_add_disk (pc->dsk, dsk);

		if (dsk_get_drive (dsk) < 0x80) {
			/* if floppy disk increase number of floppy disks in config word */
			if (pc->ppi_port_a[0] & 0x01) {
				pc->ppi_port_a[0] = (pc->ppi_port_a[0] + 0x40) & 0xff;
			}
			else {
				pc->ppi_port_a[0] |= 0x01;
			}

			pc->fd_cnt += 1;
		}
		else {
			pc->hd_cnt += 1;
		}
	}
}

static
void pc_setup_mouse (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	unsigned long addr;
	unsigned      irq;

	pc->mse = NULL;

	sct = ini_next_sct (ini, NULL, "mouse");

	if (sct == NULL) {
		return;
	}

	if (ini_get_uint32 (sct, "address", &addr, 0x03f8)) {
		ini_get_uint32 (sct, "io", &addr, 0x03f8);
	}
	ini_get_uint16 (sct, "irq", &irq, 4);

	pce_log_tag (MSG_INF, "MOUSE:", "addr=0x%04lx irq=%u\n", addr, irq);

	pc->mse = mse_new (addr, sct);

	e8250_set_irq_fct (&pc->mse->uart, &pc->pic, e8259_get_irq_f (&pc->pic, irq));

	mem_add_blk (pc->prt, mse_get_reg (pc->mse), 0);

	if (pc->trm != NULL) {
		trm_set_mouse_fct (pc->trm, pc->mse, mse_set);
	}
}

static
void pc_setup_parport (ibmpc_t *pc, ini_sct_t *ini)
{
	unsigned        i;
	unsigned long   addr;
	const char      *driver;
	ini_sct_t       *sct;
	static unsigned defbase[4] = { 0x378, 0x278, 0x3bc, 0x2bc };

	for (i = 0; i < 4; i++) {
		pc->parport[i] = NULL;
	}

	i = 0;
	sct = NULL;
	while ((sct = ini_next_sct (ini, sct, "parport")) != NULL) {
		if (i >= 4) {
			break;
		}

		ini_get_uint32 (sct, "address", &addr, defbase[i]);
		ini_get_string (sct, "driver", &driver, NULL);

		pce_log_tag (MSG_INF,
			"PARPORT:", "LPT%u addr=0x%04lx driver=%s\n",
			i + 1, addr, (driver == NULL) ? "<none>" : driver
		);

		pc->parport[i] = parport_new (addr);

		if (pc->parport[i] == NULL) {
			pce_log (MSG_ERR,
				"*** parport %u setup failed\n", i + 1
			);
		}
		else {
			if (driver != NULL) {
				if (parport_set_driver (pc->parport[i], driver)) {
					pce_log (MSG_ERR,
						"*** can't open driver (%s)\n",
						driver
					);
				}

			}

			mem_add_blk (pc->prt, parport_get_reg (pc->parport[i]), 0);

			i += 1;
		}
	}
}

static
void pc_setup_serport (ibmpc_t *pc, ini_sct_t *ini)
{
	unsigned      i;
	unsigned long addr;
	unsigned      irq;
	unsigned      multi, multi_read, multi_write;
	const char    *driver;
	const char    *log;
	const char    *chip;
	ini_sct_t     *sct;

	static unsigned long defbase[4] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
	static unsigned      defirq[4] = { 4, 3, 4, 3 };

	for (i = 0; i < 4; i++) {
		pc->serport[i] = NULL;
	}

	i = 0;
	sct = NULL;
	while ((sct = ini_next_sct (ini, sct, "serial")) != NULL) {
		if (i >= 4) {
			break;
		}

		if (ini_get_uint32 (sct, "address", &addr, defbase[i])) {
			ini_get_uint32 (sct, "io", &addr, defbase[i]);
		}

		ini_get_uint16 (sct, "irq", &irq, defirq[i]);
		ini_get_string (sct, "uart", &chip, "8250");
		ini_get_uint16 (sct, "multichar", &multi, 1);
		ini_get_uint16 (sct, "multichar_read", &multi_read, multi);
		ini_get_uint16 (sct, "multichar_write", &multi_write, multi);
		ini_get_string (sct, "driver", &driver, NULL);
		ini_get_string (sct, "log", &log, NULL);

		pce_log_tag (MSG_INF,
			"SERPORT:", "COM%u addr=0x%04lx irq=%u multichar=r=%u/w=%u uart=%s\n",
			i + 1, addr, irq, multi_read, multi_write, chip
		);

		if (driver != NULL) {
			pce_log_tag (MSG_INF,
				"SERPORT:", "COM%u driver=%s\n",
				i + 1, driver
			);
		}

		pc->serport[i] = ser_new (addr, 0);

		if (pc->serport[i] == NULL) {
			pce_log (MSG_ERR, "*** serial port setup failed [%04X/%u -> %s]\n",
				addr, irq, (driver == NULL) ? "<none>" : driver
			);
		}
		else {
			if (driver != NULL) {
				if (ser_set_driver (pc->serport[i], driver)) {
					pce_log (MSG_ERR,
						"*** can't open driver (%s)\n",
						driver
					);
				}

			}

			if (log != NULL) {
				if (ser_set_log (pc->serport[i], log)) {
					pce_log (MSG_ERR,
						"*** can't open log file (%s)\n",
						log
					);
				}
			}

			if (e8250_set_chip_str (&pc->serport[i]->uart, chip)) {
				pce_log (MSG_ERR, "*** unknown UART chip (%s)\n", chip);
			}

			e8250_set_multichar (&pc->serport[i]->uart, multi_read, multi_write);

			e8250_set_irq_fct (&pc->serport[i]->uart,
				&pc->pic, e8259_get_irq_f (&pc->pic, irq)
			);

			mem_add_blk (pc->prt, ser_get_reg (pc->serport[i]), 0);

			i += 1;
		}
	}
}

static
void pc_setup_ems (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t *sct;
	mem_blk_t *mem;

	pc->ems = NULL;

	sct = ini_next_sct (ini, NULL, "ems");
	if (sct == NULL) {
		return;
	}

	pc->ems = ems_new (sct);
	if (pc->ems == NULL) {
		return;
	}

	mem = ems_get_mem (pc->ems);
	if (mem != NULL) {
		mem_add_blk (pc->mem, mem, 0);
	}
}

static
void pc_setup_xms (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t *sct;
	mem_blk_t *mem;

	pc->xms = NULL;

	sct = ini_next_sct (ini, NULL, "xms");
	if (sct == NULL) {
		return;
	}

	pc->xms = xms_new (sct);
	if (pc->xms == NULL) {
		return;
	}

	mem = xms_get_umb_mem (pc->xms);
	if (mem != NULL) {
		mem_add_blk (pc->mem, mem, 0);
	}

	mem = xms_get_hma_mem (pc->xms);
	if (mem != NULL) {
		mem_add_blk (pc->mem, mem, 0);
	}
}

ibmpc_t *pc_new (ini_sct_t *ini)
{
	ibmpc_t *pc;

	pc = malloc (sizeof (ibmpc_t));
	if (pc == NULL) {
		return (NULL);
	}

	pc->cfg = ini;

	ini_get_uint16 (ini, "boot", &pc->bootdrive, 128);

	ini_get_bool (ini, "rtc", &pc->support_rtc, 1);

	pc->fd_cnt = 0;
	pc->hd_cnt = 0;

	pc->brk = 0;
	pc->pause = 0;

	bps_init (&pc->bps);

	pc_setup_mem (pc, ini);
	pc_setup_ports (pc, ini);

	pc_setup_nvram (pc, ini);
	pc_setup_cpu (pc, ini);
	pc_setup_dma (pc, ini);
	pc_setup_pic (pc, ini);
	pc_setup_pit (pc, ini);
	pc_setup_ppi (pc, ini);
	pc_setup_kbd (pc, ini);
	pc_setup_cassette (pc, ini);

	pc_setup_terminal (pc, ini);

	pc_setup_video (pc, ini);

	if (pc->trm != NULL) {
		trm_open (pc->trm, 640, 480);
		trm_set_msg_trm (pc->trm, "term.title", "pce-ibmpc");
	}

	pc_setup_disks (pc, ini);
	pc_setup_mouse (pc, ini);
	pc_setup_serport (pc, ini);
	pc_setup_parport (pc, ini);
	pc_setup_ems (pc, ini);
	pc_setup_xms (pc, ini);

	pce_load_mem_ini (pc->mem, ini);

	pc_clock_reset (pc);

	return (pc);
}

void pc_del_xms (ibmpc_t *pc)
{
	xms_del (pc->xms);
	pc->xms = NULL;
}

void pc_del_ems (ibmpc_t *pc)
{
	ems_del (pc->ems);
	pc->ems = NULL;
}

void pc_del_mouse (ibmpc_t *pc)
{
	if (pc->mse != NULL) {
		mse_del (pc->mse);
		pc->mse = NULL;
	}
}

void pc_del_parport (ibmpc_t *pc)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		if (pc->parport[i] != NULL) {
			parport_del (pc->parport[i]);
		}
	}
}

void pc_del_serport (ibmpc_t *pc)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		if (pc->serport[i] != NULL) {
			ser_del (pc->serport[i]);
		}
	}
}

void pc_del (ibmpc_t *pc)
{
	if (pc == NULL) {
		return;
	}

	bps_free (&pc->bps);

	pc_del_xms (pc);
	pc_del_ems (pc);
	pc_del_parport (pc);
	pc_del_serport (pc);
	pc_del_mouse (pc);

	dsks_del (pc->dsk);

	pce_video_del (pc->video);

	trm_del (pc->trm);

	pc_cas_del (pc->cas);
	e8237_free (&pc->dma);
	e8255_free (&pc->ppi);
	e8253_free (&pc->pit);
	e8259_free (&pc->pic);
	e86_del (pc->cpu);

	nvr_del (pc->nvr);
	mem_del (pc->mem);
	mem_del (pc->prt);

	ini_sct_del (pc->cfg);

	free (pc);
}

void pc_log_deb (ibmpc_t *pc, const char *msg, ...)
{
	va_list va;

	if (pc == NULL) {
		pc = par_pc;
	}

	pce_log (MSG_DEB, "[%04X:%04X] ",
		e86_get_cs (pc->cpu), e86_get_ip (pc->cpu)
	);

	va_start (va, msg);
	pce_log_va (MSG_DEB, msg, va);
	va_end (va);
}

int pc_set_serport_driver (ibmpc_t *pc, unsigned port, const char *driver)
{
	if ((port >= 4) || (pc->serport[port] == NULL)) {
		return (1);
	}

	if (ser_set_driver (pc->serport[port], driver)) {
		return (1);
	}

	return (0);
}

int pc_set_serport_file (ibmpc_t *pc, unsigned port, const char *fname)
{
	int  r;
	char *driver;

	driver = str_cat_alloc ("stdio:file=", fname);

	r = pc_set_serport_driver (pc, port, driver);

	free (driver);

	return (r);
}

int pc_set_parport_driver (ibmpc_t *pc, unsigned port, const char *driver)
{
	if ((port >= 4) || (pc->parport[port] == NULL)) {
		return (1);
	}

	if (parport_set_driver (pc->parport[port], driver)) {
		return (1);
	}

	return (0);
}

int pc_set_parport_file (ibmpc_t *pc, unsigned port, const char *fname)
{
	int  r;
	char *driver;

	driver = str_cat_alloc ("stdio:file=", fname);

	r = pc_set_parport_driver (pc, port, driver);

	free (driver);

	return (r);
}

/*
 * Get the segment address of the PCE ROM extension
 */
unsigned pc_get_pcex_seg (ibmpc_t *pc)
{
	unsigned i;
	unsigned seg;
	unsigned v1, v2;

	for (i = 0; i < 16; i++) {
		seg = 0xf000 + i * 0x0100;

		v1 = e86_get_mem16 (pc->cpu, seg, 0x0004);
		v2 = e86_get_mem16 (pc->cpu, seg, 0x0006);

		if ((v1 == 0x4350) && (v2 == 0x5845)) {
			return (seg);
		}
	}

	return (0);
}

void pc_patch_bios (ibmpc_t *pc)
{
	unsigned seg;

	if (e86_get_mem8 (pc->cpu, 0xf000, 0xfff0) != 0xea) {
		return;
	}

	if (e86_get_mem16 (pc->cpu, 0xf000, 0xfff3) != 0xf000) {
		return;
	}

	seg = pc_get_pcex_seg (pc);

	if (seg == 0) {
		return;
	}

	pc_log_deb (pc, "patching the bios (0x%04x)\n", seg);

	mem_set_uint8_rw (pc->mem, 0xffff1, 0x0c);
	mem_set_uint8_rw (pc->mem, 0xffff2, 0x00);
	mem_set_uint8_rw (pc->mem, 0xffff3, seg & 0xff);
	mem_set_uint8_rw (pc->mem, 0xffff4, (seg >> 8) & 0xff);
}

void pc_reset (ibmpc_t *pc)
{
	pc_log_deb (pc, "reset pc\n");

	if (par_patch_bios) {
		pc_patch_bios (pc);
	}

	e86_reset (pc->cpu);

	e8237_reset (&pc->dma);
	e8253_reset (&pc->pit);
	e8259_reset (&pc->pic);

	pc_kbd_reset (&pc->kbd);

	if (pc->xms != NULL) {
		xms_reset (pc->xms);
	}

	if (pc->ems != NULL) {
		ems_reset (pc->ems);
	}
}

/*
 * Get the 1.19 MHz system clock
 */
unsigned long pc_get_clock2 (ibmpc_t *pc)
{
	return (pc->clock2);
}


void pc_clock_reset (ibmpc_t *pc)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		pc->clk_div[i] = 0;
	}

	pc->sync_clock2_sim = 0;
	pc->sync_clock2_real = 0;

	pce_get_interval_us (&pc->sync_interval);

	pc->clock1 = 0;
	pc->clock2 = 0;
}

void pc_clock_discontinuity (ibmpc_t *pc)
{
	pc->sync_clock2_real = pc->sync_clock2_sim;
	pce_get_interval_us (&pc->sync_interval);
}

/*
 * Synchronize the system clock with real time
 */
static
void pc_clock_delay (ibmpc_t *pc)
{
	unsigned long vclk;
	unsigned long rclk;
	unsigned long us;

	vclk = pc->sync_clock2_sim;

	rclk = pce_get_interval_us (&pc->sync_interval);
	rclk = (PCE_IBMPC_CLK2 * (unsigned long long) rclk) / 1000000;
	rclk += pc->sync_clock2_real;

	if (vclk < rclk) {
		pc->sync_clock2_sim = 0;
		pc->sync_clock2_real = rclk - vclk;
		return;
	}

	vclk -= rclk;

	pc->sync_clock2_sim = vclk;
	pc->sync_clock2_real = 0;

	us = (1000000 * (unsigned long long) vclk) / PCE_IBMPC_CLK2;

	if (us > 10000) {
		pce_usleep (us);
	}
}

void pc_clock (ibmpc_t *pc, unsigned long cnt)
{
	unsigned      i;
	unsigned long spd;
	unsigned long clk;

	e86_clock (pc->cpu, cnt);

	pc->clock1 += cnt;

	spd = 4 * pc->speed_current;

	if (pc->clock1 < spd) {
		return;
	}

	pc->clock1 -= spd;

	pc->sync_clock2_sim += 1;

	pc->clock2 += 1;

	pce_video_clock0 (pc->video, 1, 1);

	e8253_clock (&pc->pit, 1);

	pc->clk_div[0] += 1;

	if (pc->clk_div[0] >= 8) {
		clk = pc->clk_div[0] & ~7UL;
		pc->clk_div[1] += clk;
		pc->clk_div[0] &= 7;

		pc_kbd_clock (&pc->kbd, clk);

		if (pc->cas != NULL) {
			pc_cas_clock (pc->cas, clk);
		}

		e8237_clock (&pc->dma, 1);
		e8259_clock (&pc->pic);

		pce_video_clock1 (pc->video, 0);

		for (i = 0; i < 4; i++) {
			if (pc->serport[i] != NULL) {
				e8250_clock (&pc->serport[i]->uart, clk);
			}
		}

		if (pc->clk_div[1] >= 1024) {
			clk = pc->clk_div[1] & ~1023UL;
			pc->clk_div[1] &= 1023;

			if (pc->trm != NULL) {
				trm_check (pc->trm);
			}

			for (i = 0; i < 4; i++) {
				if (pc->serport[i] != NULL) {
					ser_clock (pc->serport[i], clk);
				}
			}

			if (pc->mse != NULL) {
				mse_clock (pc->mse, clk);
			}

			pc_clock_delay (pc);
		}
	}
}

int pc_set_cpu_model (ibmpc_t *pc, const char *str)
{
	if (strcmp (str, "8086") == 0) {
		e86_enable_86 (pc->cpu);
	}
	else if (strcmp (str, "8088") == 0) {
		e86_enable_86 (pc->cpu);
		e86_set_pq_size (pc->cpu, 4);
		e86_set_options (pc->cpu, E86_CPU_8BIT, 1);
	}
	else if (strcmp (str, "v30") == 0) {
		e86_enable_v30 (pc->cpu);
	}
	else if (strcmp (str, "v20") == 0) {
		e86_enable_v30 (pc->cpu);
		e86_set_pq_size (pc->cpu, 4);
		e86_set_options (pc->cpu, E86_CPU_8BIT, 1);
	}
	else if (strcmp (str, "8086+") == 0) {
		e86_enable_186 (pc->cpu);
		e86_set_options (pc->cpu, E86_CPU_MASK_SHIFT, 0);
		e86_set_options (pc->cpu, E86_CPU_PUSH_FIRST, 0);
	}
	else if (strcmp (str, "8088+") == 0) {
		e86_enable_186 (pc->cpu);
		e86_set_pq_size (pc->cpu, 4);
		e86_set_options (pc->cpu, E86_CPU_MASK_SHIFT, 0);
		e86_set_options (pc->cpu, E86_CPU_PUSH_FIRST, 0);
		e86_set_options (pc->cpu, E86_CPU_8BIT, 1);
	}
	else if (strcmp (str, "80186") == 0) {
		e86_enable_186 (pc->cpu);
	}
	else if (strcmp (str, "80188") == 0) {
		e86_enable_186 (pc->cpu);
		e86_set_pq_size (pc->cpu, 4);
		e86_set_options (pc->cpu, E86_CPU_8BIT, 1);
	}
	else if (strcmp (str, "80286") == 0) {
		e86_enable_286 (pc->cpu);
	}
	else {
		return (1);
	}

	return (0);
}

void pc_set_speed (ibmpc_t *pc, unsigned factor)
{
	if (factor == 0) {
		pc->speed_current = pc->speed_default;
	}
	else {
		pc->speed_current = factor;
	}

	pce_log_tag (MSG_INF, "CPU:", "setting speed to %uX\n", pc->speed_current);
}

void pc_set_bootdrive (ibmpc_t *pc, unsigned drv)
{
	pc->bootdrive = drv;
}

unsigned pc_get_bootdrive (ibmpc_t *pc)
{
	return (pc->bootdrive);
}
