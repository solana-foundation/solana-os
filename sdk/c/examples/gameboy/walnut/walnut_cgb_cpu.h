/* Solana OS SDK: the second part of walnut_cgb.h (../readme.md), from its line
 * 3184 on, unchanged. Include it right after walnut_cgb.h. */
#ifndef WALNUT_GB_CPU_H
#define WALNUT_GB_CPU_H
#ifndef WALNUT_GB_HEADER_ONLY
static inline void __gb_step_cpu(struct gb_s *gb)
{
	uint16_t oppair;
	uint8_t opcode;
	uint_fast16_t inst_cycles;
	static const uint8_t op_cycles[0x100] =
	{
		/* *INDENT-OFF* */
		/*0 1 2  3  4  5  6  7  8  9  A  B  C  D  E  F	*/
		4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,	/* 0x00 */
		4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,	/* 0x10 */
		8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,	/* 0x20 */
		8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,	/* 0x30 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x40 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x50 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x60 */
		8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4, /* 0x70 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x80 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x90 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0xA0 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0xB0 */
		8,12,12,16,12,16, 8,16, 8,16,12, 8,12,24, 8,16,	/* 0xC0 */
		8,12,12, 0,12,16, 8,16, 8,16,12, 0,12, 0, 8,16,	/* 0xD0 */
		12,12,8, 0, 0,16, 8,16,16, 4,16, 0, 0, 0, 8,16,	/* 0xE0 */
		12,12,8, 4, 0,16, 8,16,12, 8,16, 4, 0, 0, 8,16	/* 0xF0 */
		/* *INDENT-ON* */
	};
	static const uint_fast16_t TAC_CYCLES[4] = {1024, 16, 64, 256};

	/* Handle interrupts */
	/* If gb_halt is positive, then an interrupt must have occurred by the
	 * time we reach here, because on HALT, we jump to the next interrupt
	 * immediately. */
	while(gb->gb_halt || (gb->gb_ime &&
			gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & ANY_INTR))
	{
		gb->gb_halt = false;

		if(!gb->gb_ime)
			break;

		/* Disable interrupts */
		gb->gb_ime = false;

		/* Push Program Counter */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);

		/* Call interrupt handler if required. */
		if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & VBLANK_INTR)
		{
			gb->cpu_reg.pc.reg = VBLANK_INTR_ADDR;
			gb->hram_io[IO_IF] ^= VBLANK_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & LCDC_INTR)
		{
			gb->cpu_reg.pc.reg = LCDC_INTR_ADDR;
			gb->hram_io[IO_IF] ^= LCDC_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & TIMER_INTR)
		{
			gb->cpu_reg.pc.reg = TIMER_INTR_ADDR;
			gb->hram_io[IO_IF] ^= TIMER_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & SERIAL_INTR)
		{
			gb->cpu_reg.pc.reg = SERIAL_INTR_ADDR;
			gb->hram_io[IO_IF] ^= SERIAL_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & CONTROL_INTR)
		{
			gb->cpu_reg.pc.reg = CONTROL_INTR_ADDR;
			gb->hram_io[IO_IF] ^= CONTROL_INTR;
		}

		break;
	}

	/* Obtain opcode */
	
	oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
	opcode = (uint8_t)oppair; // auto-truncate
#if (WALNUT_GB_SAFE_DUALFETCH_DMA || WALNUT_GB_SAFE_DUALFETCH_MBC)
  gb->prefetch_invalid=false;
#endif
	inst_cycles = op_cycles[opcode];

	/* Execute opcode */
	switch(opcode)
	{
	case 0x00: /* NOP */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x01: /* LD BC, imm */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH
    gb->cpu_reg.bc.bytes.c = (uint8_t)(oppair >> 8); // C was already partially loaded in oppair
    oppair = __gb_read16(gb, gb->cpu_reg.pc.reg + 1); // Read 16-bit immediate starting from PC+1    
    gb->cpu_reg.bc.bytes.b = (uint8_t)(oppair);// Store lower byte of immediate into B
    gb->cpu_reg.pc.reg += 2;// Increment PC by 2 to skip over the 16-bit immediate
    // Prefetch next opcode from upper byte of read16
    opcode = (uint8_t)(oppair >> 8);
#else
    // Original 8-bit fetch path
    gb->cpu_reg.bc.bytes.c = (uint8_t)(oppair >> 8);
    gb->cpu_reg.pc.reg++;
    gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.pc.reg++);
    opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#endif
		break;
	case 0x02: /* LD (BC), A */
		__gb_write(gb, gb->cpu_reg.bc.reg, gb->cpu_reg.a);	  
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x03: /* INC BC */
		gb->cpu_reg.bc.reg++;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x04: /* INC B */
		WGB_INSTR_INC_R8(gb->cpu_reg.bc.bytes.b);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x05: /* DEC B */
		WGB_INSTR_DEC_R8(gb->cpu_reg.bc.bytes.b);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x06: /* LD B, imm */
		gb->cpu_reg.bc.bytes.b = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	case 0x07: /* RLCA */
		gb->cpu_reg.a = (gb->cpu_reg.a << 1) | (gb->cpu_reg.a >> 7);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = (gb->cpu_reg.a & 0x01);
		opcode = (uint8_t)(oppair >> 8);
		break;
	case 0x08: /* LD (imm), SP */
	{		
#if WALNUT_GB_16_BIT_OPS_DUALFETCH
		uint8_t l = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
    oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
		uint8_t h = oppair;
		uint16_t temp = WALNUT_GB_U8_TO_U16(h, l);
		__gb_write16(gb,temp,gb->cpu_reg.sp.reg);
    // gb->cpu_reg.pc.reg+=2;
		opcode = oppair >> 8;
#else
    uint8_t l = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t temp = WALNUT_GB_U8_TO_U16(h, l);
    __gb_write(gb, temp++, gb->cpu_reg.sp.bytes.p);
    __gb_write(gb, temp, gb->cpu_reg.sp.bytes.s);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#endif
    break;
	}

	case 0x09: /* ADD HL, BC */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.bc.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			(temp ^ gb->cpu_reg.hl.reg ^ gb->cpu_reg.bc.reg) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFFFF0000) ? 1 : 0;
		gb->cpu_reg.hl.reg = (temp & 0x0000FFFF);
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x0A: /* LD A, (BC) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.bc.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x0B: /* DEC BC */
		gb->cpu_reg.bc.reg--;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x0C: /* INC C */
		WGB_INSTR_INC_R8(gb->cpu_reg.bc.bytes.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x0D: /* DEC C */
		WGB_INSTR_DEC_R8(gb->cpu_reg.bc.bytes.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x0E: /* LD C, imm */
		gb->cpu_reg.bc.bytes.c = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x0F: /* RRCA */
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = gb->cpu_reg.a & 0x01;
		gb->cpu_reg.a = (gb->cpu_reg.a >> 1) | (gb->cpu_reg.a << 7);
		opcode = (uint8_t)(oppair >> 8);
		break;
	case 0x10: /* STOP */
		//gb->gb_halt = true;
#if WALNUT_FULL_GBC_SUPPORT
		if(gb->cgb.cgbMode & gb->cgb.doubleSpeedPrep)
		{
			gb->cgb.doubleSpeedPrep = 0;
			gb->cgb.doubleSpeed ^= 1;
		}
#endif
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x11: /* LD DE, imm */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH
    gb->cpu_reg.de.bytes.e = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
		oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.de.bytes.d = oppair;
		opcode = oppair >> 8;
#else
    gb->cpu_reg.de.bytes.e = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
    gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.pc.reg++);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#endif
		break;
	case 0x12: /* LD (DE), A */
		__gb_write(gb, gb->cpu_reg.de.reg, gb->cpu_reg.a);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x13: /* INC DE */
		gb->cpu_reg.de.reg++;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x14: /* INC D */
		WGB_INSTR_INC_R8(gb->cpu_reg.de.bytes.d);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x15: /* DEC D */
		WGB_INSTR_DEC_R8(gb->cpu_reg.de.bytes.d);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x16: /* LD D, imm */
		gb->cpu_reg.de.bytes.d = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x17: /* RLA */
	{
		uint8_t temp = gb->cpu_reg.a;
		gb->cpu_reg.a = (gb->cpu_reg.a << 1) | gb->cpu_reg.f.f_bits.c;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = (temp >> 7) & 0x01;
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x18: /* JR imm */
	{
		int8_t temp = (int8_t) (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
		gb->cpu_reg.pc.reg += temp;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}
	case 0x19: /* ADD HL, DE */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.de.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			(temp ^ gb->cpu_reg.hl.reg ^ gb->cpu_reg.de.reg) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFFFF0000) ? 1 : 0;
		gb->cpu_reg.hl.reg = (temp & 0x0000FFFF);
		opcode = (uint8_t)(oppair >> 8);
		break;
	}
	case 0x1A: /* LD A, (DE) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.de.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x1B: /* DEC DE */
		gb->cpu_reg.de.reg--;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x1C: /* INC E */
		WGB_INSTR_INC_R8(gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x1D: /* DEC E */
		WGB_INSTR_DEC_R8(gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x1E: /* LD E, imm */
		gb->cpu_reg.de.bytes.e =  (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x1F: /* RRA */
	{
		uint8_t temp = gb->cpu_reg.a;
		gb->cpu_reg.a = gb->cpu_reg.a >> 1 | (gb->cpu_reg.f.f_bits.c << 7);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = temp & 0x1;
		opcode = (uint8_t)(oppair >> 8);
		break;		
	}

	case 0x20: /* JR NZ, imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
			int8_t temp = (int8_t) (oppair >> 8);
	    gb->cpu_reg.pc.reg++;
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x21: /* LD HL, imm */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH
		gb->cpu_reg.hl.bytes.l =(uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.hl.bytes.h = oppair;
		opcode = oppair >> 8;
#else
    gb->cpu_reg.hl.bytes.l =(uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
    gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.pc.reg++);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#endif
		break;

	case 0x22: /* LDI (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		gb->cpu_reg.hl.reg++;
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x23: /* INC HL */
		gb->cpu_reg.hl.reg++;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x24: /* INC H */
		WGB_INSTR_INC_R8(gb->cpu_reg.hl.bytes.h);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x25: /* DEC H */
		WGB_INSTR_DEC_R8(gb->cpu_reg.hl.bytes.h);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x26: /* LD H, imm */
		gb->cpu_reg.hl.bytes.h =(uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x27: /* DAA */
	{
		/* The following is from SameBoy. MIT License. */
		int16_t a = gb->cpu_reg.a;

		if(gb->cpu_reg.f.f_bits.n)
		{
			if(gb->cpu_reg.f.f_bits.h)
				a = (a - 0x06) & 0xFF;

			if(gb->cpu_reg.f.f_bits.c)
				a -= 0x60;
		}
		else
		{
			if(gb->cpu_reg.f.f_bits.h || (a & 0x0F) > 9)
				a += 0x06;

			if(gb->cpu_reg.f.f_bits.c || a > 0x9F)
				a += 0x60;
		}

		if((a & 0x100) == 0x100)
			gb->cpu_reg.f.f_bits.c = 1;

		gb->cpu_reg.a = (uint8_t)a;
		gb->cpu_reg.f.f_bits.z = (gb->cpu_reg.a == 0);
		gb->cpu_reg.f.f_bits.h = 0;
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x28: /* JR Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
			int8_t temp = (int8_t) (oppair >> 8);
	    gb->cpu_reg.pc.reg++;
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x29: /* ADD HL, HL */
	{
		gb->cpu_reg.f.f_bits.c = (gb->cpu_reg.hl.reg & 0x8000) > 0;
		gb->cpu_reg.hl.reg <<= 1;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = (gb->cpu_reg.hl.reg & 0x1000) > 0;
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x2A: /* LD A, (HL+) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg++);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x2B: /* DEC HL */
		gb->cpu_reg.hl.reg--;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x2C: /* INC L */
		WGB_INSTR_INC_R8(gb->cpu_reg.hl.bytes.l);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x2D: /* DEC L */
		WGB_INSTR_DEC_R8(gb->cpu_reg.hl.bytes.l);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x2E: /* LD L, imm */
		gb->cpu_reg.hl.bytes.l = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	
	case 0x2F: /* CPL */
		gb->cpu_reg.a = ~gb->cpu_reg.a;
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h = 1;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x30: /* JR NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
			int8_t temp = (int8_t) (oppair >> 8);
	    gb->cpu_reg.pc.reg++;
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;
    opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x31: /* LD SP, imm */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH
		gb->cpu_reg.sp.bytes.p = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg ++;
    oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.sp.bytes.s = oppair; // auto-truncated
		opcode = oppair >> 8; 
#else
    gb->cpu_reg.sp.bytes.p = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
    gb->cpu_reg.sp.bytes.s = __gb_read(gb, gb->cpu_reg.pc.reg++);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); 
#endif  
		break;

	case 0x32: /* LD (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		gb->cpu_reg.hl.reg--;
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x33: /* INC SP */
		gb->cpu_reg.sp.reg++;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x34: /* INC (HL) */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.hl.reg);
		WGB_INSTR_INC_R8(temp);
		__gb_write(gb, gb->cpu_reg.hl.reg, temp);
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x35: /* DEC (HL) */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.hl.reg);
		WGB_INSTR_DEC_R8(temp);
		__gb_write(gb, gb->cpu_reg.hl.reg, temp);
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x36: /* LD (HL), imm */
		__gb_write(gb, gb->cpu_reg.hl.reg,(uint8_t)(oppair >> 8));
		gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x37: /* SCF */
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = 0;
		gb->cpu_reg.f.f_bits.c = 1;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x38: /* JR C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
			int8_t temp = (int8_t)(oppair >> 8);
	    gb->cpu_reg.pc.reg++;
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x39: /* ADD HL, SP */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.sp.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			((gb->cpu_reg.hl.reg & 0xFFF) + (gb->cpu_reg.sp.reg & 0xFFF)) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = temp & 0x10000 ? 1 : 0;
		gb->cpu_reg.hl.reg = (uint16_t)temp;
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0x3A: /* LD A, (HL) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg--);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x3B: /* DEC SP */
		gb->cpu_reg.sp.reg--;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x3C: /* INC A */
		WGB_INSTR_INC_R8(gb->cpu_reg.a);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x3D: /* DEC A */
		WGB_INSTR_DEC_R8(gb->cpu_reg.a);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x3E: /* LD A, imm */
		gb->cpu_reg.a = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0x3F: /* CCF */
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = 0;
		gb->cpu_reg.f.f_bits.c = ~gb->cpu_reg.f.f_bits.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x40: /* LD B, B */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x41: /* LD B, C */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.bc.bytes.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x42: /* LD B, D */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.de.bytes.d;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x43: /* LD B, E */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.de.bytes.e;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x44: /* LD B, H */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.hl.bytes.h;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x45: /* LD B, L */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.hl.bytes.l;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x46: /* LD B, (HL) */
		gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x47: /* LD B, A */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.a;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x48: /* LD C, B */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.bc.bytes.b;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x49: /* LD C, C */
	  opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x4A: /* LD C, D */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.de.bytes.d;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x4B: /* LD C, E */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.de.bytes.e;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x4C: /* LD C, H */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.hl.bytes.h;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x4D: /* LD C, L */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.hl.bytes.l;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x4E: /* LD C, (HL) */
		gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x4F: /* LD C, A */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.a;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x50: /* LD D, B */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.bc.bytes.b;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x51: /* LD D, C */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.bc.bytes.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x52: /* LD D, D */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x53: /* LD D, E */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.de.bytes.e;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x54: /* LD D, H */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.hl.bytes.h;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x55: /* LD D, L */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.hl.bytes.l;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x56: /* LD D, (HL) */
		gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x57: /* LD D, A */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.a;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x58: /* LD E, B */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.bc.bytes.b;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x59: /* LD E, C */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.bc.bytes.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x5A: /* LD E, D */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.de.bytes.d;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x5B: /* LD E, E */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x5C: /* LD E, H */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.hl.bytes.h;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x5D: /* LD E, L */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.hl.bytes.l;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x5E: /* LD E, (HL) */
		gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x5F: /* LD E, A */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.a;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x60: /* LD H, B */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.bc.bytes.b;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x61: /* LD H, C */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.bc.bytes.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x62: /* LD H, D */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.de.bytes.d;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x63: /* LD H, E */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.de.bytes.e;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x64: /* LD H, H */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x65: /* LD H, L */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.hl.bytes.l;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x66: /* LD H, (HL) */
		gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x67: /* LD H, A */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.a;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x68: /* LD L, B */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.bc.bytes.b;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x69: /* LD L, C */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.bc.bytes.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x6A: /* LD L, D */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.de.bytes.d;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x6B: /* LD L, E */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.de.bytes.e;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x6C: /* LD L, H */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.hl.bytes.h;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x6D: /* LD L, L */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x6E: /* LD L, (HL) */
		gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x6F: /* LD L, A */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.a;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x70: /* LD (HL), B */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.bc.bytes.b);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif	
		break;

	case 0x71: /* LD (HL), C */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.bc.bytes.c);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif		
		break;

	case 0x72: /* LD (HL), D */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.de.bytes.d);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x73: /* LD (HL), E */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x74: /* LD (HL), H */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.hl.bytes.h);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x75: /* LD (HL), L */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.hl.bytes.l);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif	
		break;

	case 0x76: /* HALT */
	{
		int_fast16_t halt_cycles = INT_FAST16_MAX;

		/* TODO: Emulate HALT bug? */
		gb->gb_halt = true;

		if(gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
		{
			int serial_cycles = SERIAL_CYCLES -
				gb->counter.serial_count;

			if(serial_cycles < halt_cycles)
				halt_cycles = serial_cycles;
		}

		if(gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK)
		{
			int tac_cycles = TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK] -
				gb->counter.tima_count;

			if(tac_cycles < halt_cycles)
				halt_cycles = tac_cycles;
		}

		if((gb->hram_io[IO_LCDC] & LCDC_ENABLE))
		{
			int lcd_cycles;

			/* If LCD is in HBlank, calculate the number of cycles
			 * until the end of HBlank and the start of mode 2 or
			 * mode 1. */
			if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_HBLANK)
			{
				lcd_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
			}
			else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_OAM_SCAN)
			{
				lcd_cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - gb->counter.lcd_count;
			}
			else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_LCD_DRAW)
			{
				lcd_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
			}
			else
			{
				/* VBlank */
				lcd_cycles = LCD_LINE_CYCLES - gb->counter.lcd_count;
			}

			if(lcd_cycles < halt_cycles)
				halt_cycles = lcd_cycles;
		}

		/* Some halt cycles may already be very high, so make sure we
		 * don't underflow here. */
		if(halt_cycles <= 0)
			halt_cycles = 4;

		inst_cycles = (uint_fast16_t)halt_cycles;

#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // possibly changes mbc behaviour
#else
    opcode = (uint8_t)(oppair >> 8);
#endif
		break;
	}
	

	case 0x77: /* LD (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // GTA SAFE TEST
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0x78: /* LD A, B */
		gb->cpu_reg.a = gb->cpu_reg.bc.bytes.b;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x79: /* LD A, C */
		gb->cpu_reg.a = gb->cpu_reg.bc.bytes.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x7A: /* LD A, D */
		gb->cpu_reg.a = gb->cpu_reg.de.bytes.d;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x7B: /* LD A, E */
		gb->cpu_reg.a = gb->cpu_reg.de.bytes.e;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x7C: /* LD A, H */
		gb->cpu_reg.a = gb->cpu_reg.hl.bytes.h;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x7D: /* LD A, L */
		gb->cpu_reg.a = gb->cpu_reg.hl.bytes.l;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x7E: /* LD A, (HL) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x7F: /* LD A, A */
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x80: /* ADD A, B */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.b, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x81: /* ADD A, C */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.c, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x82: /* ADD A, D */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.d, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x83: /* ADD A, E */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.e, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x84: /* ADD A, H */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.h, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x85: /* ADD A, L */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.l, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x86: /* ADD A, (HL) */
		WGB_INSTR_ADC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x87: /* ADD A, A */
		WGB_INSTR_ADC_R8(gb->cpu_reg.a, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x88: /* ADC A, B */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.b, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x89: /* ADC A, C */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.c, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x8A: /* ADC A, D */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.d, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x8B: /* ADC A, E */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.e, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x8C: /* ADC A, H */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.h, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x8D: /* ADC A, L */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.l, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x8E: /* ADC A, (HL) */
		WGB_INSTR_ADC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x8F: /* ADC A, A */
		WGB_INSTR_ADC_R8(gb->cpu_reg.a, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x90: /* SUB B */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.b, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x91: /* SUB C */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.c, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x92: /* SUB D */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.d, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x93: /* SUB E */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.e, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x94: /* SUB H */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.h, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x95: /* SUB L */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.l, 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x96: /* SUB (HL) */
		WGB_INSTR_SBC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), 0);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x97: /* SUB A */
		gb->cpu_reg.a = 0;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 1;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x98: /* SBC A, B */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.b, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x99: /* SBC A, C */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.c, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x9A: /* SBC A, D */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.d, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x9B: /* SBC A, E */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.e, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x9C: /* SBC A, H */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.h, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x9D: /* SBC A, L */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.l, gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x9E: /* SBC A, (HL) */
		WGB_INSTR_SBC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), gb->cpu_reg.f.f_bits.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0x9F: /* SBC A, A */
		gb->cpu_reg.a = gb->cpu_reg.f.f_bits.c ? 0xFF : 0x00;
		gb->cpu_reg.f.f_bits.z = !gb->cpu_reg.f.f_bits.c;
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h = gb->cpu_reg.f.f_bits.c;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA0: /* AND B */
		WGB_INSTR_AND_R8(gb->cpu_reg.bc.bytes.b);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA1: /* AND C */
		WGB_INSTR_AND_R8(gb->cpu_reg.bc.bytes.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA2: /* AND D */
		WGB_INSTR_AND_R8(gb->cpu_reg.de.bytes.d);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA3: /* AND E */
		WGB_INSTR_AND_R8(gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA4: /* AND H */
		WGB_INSTR_AND_R8(gb->cpu_reg.hl.bytes.h);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA5: /* AND L */
		WGB_INSTR_AND_R8(gb->cpu_reg.hl.bytes.l);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA6: /* AND (HL) */
		WGB_INSTR_AND_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA7: /* AND A */
		WGB_INSTR_AND_R8(gb->cpu_reg.a);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA8: /* XOR B */
		WGB_INSTR_XOR_R8(gb->cpu_reg.bc.bytes.b);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xA9: /* XOR C */
		WGB_INSTR_XOR_R8(gb->cpu_reg.bc.bytes.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xAA: /* XOR D */
		WGB_INSTR_XOR_R8(gb->cpu_reg.de.bytes.d);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xAB: /* XOR E */
		WGB_INSTR_XOR_R8(gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xAC: /* XOR H */
		WGB_INSTR_XOR_R8(gb->cpu_reg.hl.bytes.h);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xAD: /* XOR L */
		WGB_INSTR_XOR_R8(gb->cpu_reg.hl.bytes.l);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xAE: /* XOR (HL) */
		WGB_INSTR_XOR_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xAF: /* XOR A */
		WGB_INSTR_XOR_R8(gb->cpu_reg.a);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB0: /* OR B */
		WGB_INSTR_OR_R8(gb->cpu_reg.bc.bytes.b);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB1: /* OR C */
		WGB_INSTR_OR_R8(gb->cpu_reg.bc.bytes.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB2: /* OR D */
		WGB_INSTR_OR_R8(gb->cpu_reg.de.bytes.d);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB3: /* OR E */
		WGB_INSTR_OR_R8(gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB4: /* OR H */
		WGB_INSTR_OR_R8(gb->cpu_reg.hl.bytes.h);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB5: /* OR L */
		WGB_INSTR_OR_R8(gb->cpu_reg.hl.bytes.l);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB6: /* OR (HL) */
		WGB_INSTR_OR_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB7: /* OR A */
		WGB_INSTR_OR_R8(gb->cpu_reg.a);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB8: /* CP B */
		WGB_INSTR_CP_R8(gb->cpu_reg.bc.bytes.b);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xB9: /* CP C */
		WGB_INSTR_CP_R8(gb->cpu_reg.bc.bytes.c);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xBA: /* CP D */
		WGB_INSTR_CP_R8(gb->cpu_reg.de.bytes.d);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xBB: /* CP E */
		WGB_INSTR_CP_R8(gb->cpu_reg.de.bytes.e);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xBC: /* CP H */
		WGB_INSTR_CP_R8(gb->cpu_reg.hl.bytes.h);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xBD: /* CP L */
		WGB_INSTR_CP_R8(gb->cpu_reg.hl.bytes.l);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xBE: /* CP (HL) */
		WGB_INSTR_CP_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xBF: /* CP A */
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 1;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xC0: /* RET NZ */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED3
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2;
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
        inst_cycles += 12;
		}
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xC1: /* POP BC */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED3
    gb->cpu_reg.bc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xC2: /* JP NZ, imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{		// best without 16-bit read when used in first half of dual fetch chain
        uint8_t c = (uint8_t)(oppair >> 8);
				gb->cpu_reg.pc.reg++;
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xC3: /* JP imm */
		{ // best without 16-bit read when used in first half of dual fetch chain
    uint8_t c = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
    uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.pc.bytes.c = c;
    gb->cpu_reg.pc.bytes.p = p;
		}
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xC4: /* CALL NZ imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED3
        uint8_t c, p;
        c = (uint8_t)(oppair >> 8);        // upper byte of immediate
        gb->cpu_reg.pc.reg++;              // advance to low byte
        p = __gb_read(gb, gb->cpu_reg.pc.reg++); // read low byte
        // Push current PC onto stack
        gb->cpu_reg.sp.reg -= 2;          // reserve space for 2 bytes
        __gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg); 
        // Load immediate address into PC
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#else
			uint8_t c, p;
			c = (uint8_t)(oppair >> 8);
	    gb->cpu_reg.pc.reg++;
			p = __gb_read(gb, gb->cpu_reg.pc.reg++);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
			gb->cpu_reg.pc.bytes.c = c;
			gb->cpu_reg.pc.bytes.p = p;
#endif        
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xC5: /* PUSH BC */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED3
		gb->cpu_reg.sp.reg-=2;
		__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.bc.reg);
#else
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.bc.bytes.b);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.bc.bytes.c);
#endif
		opcode = (uint8_t)(oppair >> 8); 
		break;

	case 0xC6: /* ADD A, imm */
	{
		uint8_t val = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
		WGB_INSTR_ADC_R8(val, 0);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xC7: /* RST 0x0000 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0000;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xC8: /* RET Z */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED2
        // Optional 16-bit path
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2;
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
			inst_cycles += 12;
			opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		}
		else
		  opcode=(uint8_t)(oppair >> 8);
		break;

	case 0xC9: /* RET */
	{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED2
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xCA: /* JP Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
        uint8_t c = (uint8_t)(oppair >> 8);
				gb->cpu_reg.pc.reg++;
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xCB: /* CB INST */
		inst_cycles = __gb_execute_cb(gb);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); // can cb change pc??? * revise
		//opcode = (uint8_t)(oppair >> 8); // things stopped working for megaman, rtype dx, etc with chaining when I made this change unless cgabmaactive was checked and opcode reloaded with gb_read
		break;

	case 0xCC: /* CALL Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
			uint8_t c = (uint8_t)(oppair >> 8);
			gb->cpu_reg.pc.reg++;
			uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
			gb->cpu_reg.pc.bytes.c = c;
			gb->cpu_reg.pc.bytes.p = p;
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xCD: /* CALL imm */
	{
		uint8_t c = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.bytes.c = c;
		gb->cpu_reg.pc.bytes.p = p;
	}
	opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
	break;

	case 0xCE: /* ADC A, imm */
	{
		uint8_t val = (uint8_t)(oppair >> 8);
	  gb->cpu_reg.pc.reg++;
		WGB_INSTR_ADC_R8(val, gb->cpu_reg.f.f_bits.c);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xCF: /* RST 0x0008 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0008;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xD0: /* RET NC */
    if (!gb->cpu_reg.f.f_bits.c)
    {
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
        inst_cycles += 12;
    }
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xD1: /* POP DE */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
    gb->cpu_reg.de.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		opcode = (uint8_t)(oppair >> 8); 
		break;

	case 0xD2: /* JP NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xD4: /* CALL NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
        uint8_t c =(uint8_t)(oppair >> 8);
				gb->cpu_reg.pc.reg++;
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
				gb->cpu_reg.sp.reg-=2;
				__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);
#else
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
#endif
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
				inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xD5: /* PUSH DE */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
		gb->cpu_reg.sp.reg-=2;
		__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.de.reg);
#else
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.de.bytes.d);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.de.bytes.e);
#endif
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xD6: /* SUB imm */
	{
		uint8_t val =(uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		uint16_t temp = gb->cpu_reg.a - val;
		gb->cpu_reg.f.f_bits.z = ((temp & 0xFF) == 0x00);
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h =
			(gb->cpu_reg.a ^ val ^ temp) & 0x10 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFF00) ? 1 : 0;
		gb->cpu_reg.a = (temp & 0xFF);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xD7: /* RST 0x0010 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0010;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xD8: /* RET C */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
			inst_cycles += 12;
			opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		}
		else
			opcode = (uint8_t)(oppair >> 8);

		break;

	case 0xD9: /* RETI */
	{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
    gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		gb->gb_ime = true;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
	}
	break;

	case 0xDA: /* JP C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
        uint8_t c = (uint8_t)(oppair >> 8);
				gb->cpu_reg.pc.reg++;
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
				inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); 
		break;

	case 0xDC: /* CALL C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
        uint8_t c = (uint8_t)(oppair >> 8);
				gb->cpu_reg.pc.reg++;
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
				gb->cpu_reg.sp.reg-=2;
				__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);
#else
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
#endif
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
				inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg); 
		break;

	case 0xDE: /* SBC A, imm */
	{
		uint8_t val = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		WGB_INSTR_SBC_R8(val, gb->cpu_reg.f.f_bits.c);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}
	case 0xDF: /* RST 0x0018 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0018;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xE0: /* LD (0xFF00+imm), A */
		__gb_write(gb, 0xFF00 | (uint8_t)(oppair >> 8),
			   gb->cpu_reg.a);
		gb->cpu_reg.pc.reg++;
	  opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xE1: /* POP HL */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED_DISABLED 
    gb->cpu_reg.hl.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2; // advance SP past the popped value
#else
    gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xE2: /* LD (C), A */
		__gb_write(gb, 0xFF00 | gb->cpu_reg.bc.bytes.c, gb->cpu_reg.a);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES_DISABLED
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#else
		opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0xE5: /* PUSH HL */
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
		gb->cpu_reg.sp.reg-=2;
		__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.hl.reg);
#else
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.hl.bytes.h);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.hl.bytes.l);
#endif
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xE6: /* AND imm */
	{
		uint8_t temp = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		WGB_INSTR_AND_R8(temp);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xE7: /* RST 0x0020 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0020;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xE8: /* ADD SP, imm */
	{
		int8_t offset = (int8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.h = ((gb->cpu_reg.sp.reg & 0xF) + (offset & 0xF) > 0xF) ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = ((gb->cpu_reg.sp.reg & 0xFF) + (offset & 0xFF) > 0xFF);
		gb->cpu_reg.sp.reg += offset;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xE9: /* JP (HL) */
		gb->cpu_reg.pc.reg = gb->cpu_reg.hl.reg;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xEA: /* LD (imm), A */
	{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
    uint8_t l = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = oppair; // autotruncate
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
		__gb_write(gb, addr, gb->cpu_reg.a);
		opcode = oppair >> 8;	
#else
    uint8_t l = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
		__gb_write(gb, addr, gb->cpu_reg.a);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);		
#endif
		break;
	}

	case 0xEE: /* XOR imm */
		WGB_INSTR_XOR_R8((uint8_t)(oppair >> 8));
		gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xEF: /* RST 0x0028 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0028;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xF0: /* LD A, (0xFF00+imm) */
		gb->cpu_reg.a =
			__gb_read(gb, 0xFF00 | (uint8_t)(oppair >> 8));
			gb->cpu_reg.pc.reg++;
			opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xF1: /* POP AF */
	{
		uint8_t temp_8 = __gb_read(gb, gb->cpu_reg.sp.reg++);
		gb->cpu_reg.f.f_bits.z = (temp_8 >> 7) & 1;
		gb->cpu_reg.f.f_bits.n = (temp_8 >> 6) & 1;
		gb->cpu_reg.f.f_bits.h = (temp_8 >> 5) & 1;
		gb->cpu_reg.f.f_bits.c = (temp_8 >> 4) & 1;
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.sp.reg++);
		opcode = (uint8_t)(oppair >> 8);
		break;
	}

	case 0xF2: /* LD A, (C) */
		gb->cpu_reg.a = __gb_read(gb, 0xFF00 | gb->cpu_reg.bc.bytes.c);
#if WALNUT_GB_SAFE_DUALFETCH_OPCODES
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#else
	opcode = (uint8_t)(oppair >> 8);
#endif
		break;

	case 0xF3: /* DI */
		gb->gb_ime = false;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xF5: /* PUSH AF */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.a);
		__gb_write(gb, --gb->cpu_reg.sp.reg,
			   gb->cpu_reg.f.f_bits.z << 7 | gb->cpu_reg.f.f_bits.n << 6 |
			   gb->cpu_reg.f.f_bits.h << 5 | gb->cpu_reg.f.f_bits.c << 4);
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xF6: /* OR imm */
		WGB_INSTR_OR_R8((uint8_t)(oppair >> 8));
		gb->cpu_reg.pc.reg++;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xF7: /* PUSH AF */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0030;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	case 0xF8: /* LD HL, SP+/-imm */
	{
		/* Taken from SameBoy, which is released under MIT Licence. */
		int8_t offset = (int8_t) (oppair >> 8);
		gb->cpu_reg.pc.reg++;
		gb->cpu_reg.hl.reg = gb->cpu_reg.sp.reg + offset;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.h = ((gb->cpu_reg.sp.reg & 0xF) + (offset & 0xF) > 0xF) ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = ((gb->cpu_reg.sp.reg & 0xFF) + (offset & 0xFF) > 0xFF) ? 1 : 0;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xF9: /* LD SP, HL */
		gb->cpu_reg.sp.reg = gb->cpu_reg.hl.reg;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xFA: /* LD A, (imm) */
	{
#if WALNUT_GB_16_BIT_OPS_DUALFETCH_DISABLED
    uint8_t l = (uint8_t)(oppair >> 8); // we dont increment pc because a interrupt might change it an invalidate this byte, we increment if no interrupt between 1st and 2nd opcode handlers
		gb->cpu_reg.pc.reg++;
		oppair = __gb_read16(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = oppair; // auto-truncate
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
		gb->cpu_reg.a = __gb_read(gb, addr);
		opcode = oppair >> 8;
#else
    uint8_t l = (uint8_t)(oppair >> 8); // we dont increment pc because a interrupt might change it an invalidate this byte, we increment if no interrupt between 1st and 2nd opcode handlers
		gb->cpu_reg.pc.reg++;
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
		gb->cpu_reg.a = __gb_read(gb, addr);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
#endif
		break;
	}

	case 0xFB: /* EI */
		gb->gb_ime = true;
		opcode = (uint8_t)(oppair >> 8);
		break;

	case 0xFE: /* CP imm */
	{
		uint8_t val = (uint8_t)(oppair >> 8);
		gb->cpu_reg.pc.reg++;
		WGB_INSTR_CP_R8(val);
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;
	}

	case 0xFF: /* RST 0x0038 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0038;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);
		break;

	default:
		/* Return address where invalid opcode that was read. */
		(gb->gb_error)(gb, GB_INVALID_OPCODE, gb->cpu_reg.pc.reg - 1);
		WGB_UNREACHABLE();
		return;
	}

	do
	{
		/* DIV register timing */
		gb->counter.div_count += inst_cycles;
		while(gb->counter.div_count >= DIV_CYCLES)
		{
			gb->hram_io[IO_DIV]++;
			gb->counter.div_count -= DIV_CYCLES;
		}

		/* Check for RTC tick. */
		if(gb->mbc == 3 && (gb->rtc_real.reg.high & 0x40) == 0)
		{
			gb->counter.rtc_count += inst_cycles;
			while(WGB_UNLIKELY(gb->counter.rtc_count >= RTC_CYCLES))
			{
				gb->counter.rtc_count -= RTC_CYCLES;

				/* Detect invalid rollover. */
				if(WGB_UNLIKELY(gb->rtc_real.reg.sec == 63))
				{
					gb->rtc_real.reg.sec = 0;
					continue;
				}

				if(++gb->rtc_real.reg.sec != 60)
					continue;

				gb->rtc_real.reg.sec = 0;
				if(gb->rtc_real.reg.min == 63)
				{
					gb->rtc_real.reg.min = 0;
					continue;
				}
				if(++gb->rtc_real.reg.min != 60)
					continue;

				gb->rtc_real.reg.min = 0;
				if(gb->rtc_real.reg.hour == 31)
				{
					gb->rtc_real.reg.hour = 0;
					continue;
				}
				if(++gb->rtc_real.reg.hour != 24)
					continue;

				gb->rtc_real.reg.hour = 0;
				if(++gb->rtc_real.reg.yday != 0)
					continue;

				if(gb->rtc_real.reg.high & 1)  /* Bit 8 of days*/
					gb->rtc_real.reg.high |= 0x80; /* Overflow bit */

				gb->rtc_real.reg.high ^= 1;
			}
		}

		/* Check serial transmission. */
		if(gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
		{
			unsigned int serial_cycles = SERIAL_CYCLES_1KB;

			/* If new transfer, call TX function. */
			if(gb->counter.serial_count == 0 &&
				gb->gb_serial_tx != NULL)
				(gb->gb_serial_tx)(gb, gb->hram_io[IO_SB]);

#if WALNUT_FULL_GBC_SUPPORT
			if(gb->hram_io[IO_SC] & 0x3)
				serial_cycles = SERIAL_CYCLES_32KB;
#endif

			gb->counter.serial_count += inst_cycles;

			/* If it's time to receive byte, call RX function. */
			if(gb->counter.serial_count >= serial_cycles)
			{
				/* If RX can be done, do it. */
				/* If RX failed, do not change SB if using external
				 * clock, or set to 0xFF if using internal clock. */
				uint8_t rx;

				if(gb->gb_serial_rx != NULL &&
					(gb->gb_serial_rx(gb, &rx) ==
						GB_SERIAL_RX_SUCCESS))
				{
					gb->hram_io[IO_SB] = rx;

					/* Inform game of serial TX/RX completion. */
					gb->hram_io[IO_SC] &= 0x01;
					gb->hram_io[IO_IF] |= SERIAL_INTR;
				}
				else if(gb->hram_io[IO_SC] & SERIAL_SC_CLOCK_SRC)
				{
					/* If using internal clock, and console is not
					 * attached to any external peripheral, shifted
					 * bits are replaced with logic 1. */
					gb->hram_io[IO_SB] = 0xFF;

					/* Inform game of serial TX/RX completion. */
					gb->hram_io[IO_SC] &= 0x01;
					gb->hram_io[IO_IF] |= SERIAL_INTR;
				}
				else
				{
					/* If using external clock, and console is not
					 * attached to any external peripheral, bits are
					 * not shifted, so SB is not modified. */
				}

				gb->counter.serial_count = 0;
			}
		}

		/* TIMA register timing */
		/* TODO: Change tac_enable to struct of TAC timer control bits. */
		if(gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK)
		{
			gb->counter.tima_count += inst_cycles;

			while(gb->counter.tima_count >=
				TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK])
			{
				gb->counter.tima_count -=
					TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK];

				if(++gb->hram_io[IO_TIMA] == 0)
				{
					gb->hram_io[IO_IF] |= TIMER_INTR;
					/* On overflow, set TMA to TIMA. */
					gb->hram_io[IO_TIMA] = gb->hram_io[IO_TMA];
				}
			}
		}

		/* If LCD is off, don't update LCD state or increase the LCD
		 * ticks. Instead, keep track of the amount of time that is
		 * being passed. */
		if(!(gb->hram_io[IO_LCDC] & LCDC_ENABLE))
		{
			gb->counter.lcd_off_count += inst_cycles;
			if(gb->counter.lcd_off_count >= LCD_FRAME_CYCLES)
			{
				gb->counter.lcd_off_count -= LCD_FRAME_CYCLES;
				gb->gb_frame = true;
			}
			continue;
		}

		/* LCD Timing */
#if WALNUT_FULL_GBC_SUPPORT
        if (inst_cycles > 1) {
            gb->counter.lcd_count += (inst_cycles >> gb->cgb.doubleSpeed);
        } else {
#endif
		gb->counter.lcd_count += inst_cycles;
#if WALNUT_FULL_GBC_SUPPORT
	}
#endif

		/* New Scanline. HBlank -> VBlank or OAM Scan */
		if(gb->counter.lcd_count >= LCD_LINE_CYCLES)
		{
			gb->counter.lcd_count -= LCD_LINE_CYCLES;

			/* Next line */
			gb->hram_io[IO_LY] = gb->hram_io[IO_LY] + 1;
			if (gb->hram_io[IO_LY] == LCD_VERT_LINES)
				gb->hram_io[IO_LY] = 0;

			/* LYC Update */
			if(gb->hram_io[IO_LY] == gb->hram_io[IO_LYC])
			{
				gb->hram_io[IO_STAT] |= STAT_LYC_COINC;

				if(gb->hram_io[IO_STAT] & STAT_LYC_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;
			}
			else
				gb->hram_io[IO_STAT] &= 0xFB;

			/* Check if LCD should be in Mode 1 (VBLANK) state */
			if(gb->hram_io[IO_LY] == LCD_HEIGHT)
			{
				gb->hram_io[IO_STAT] =
					(gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_VBLANK;
				gb->gb_frame = true;
				gb->hram_io[IO_IF] |= VBLANK_INTR;
				gb->lcd_blank = false;

				if(gb->hram_io[IO_STAT] & STAT_MODE_1_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;

#if ENABLE_LCD
				/* If frame skip is activated, check if we need to draw
				 * the frame or skip it. */
				if(gb->direct.frame_skip)
				{
					gb->display.frame_skip_count =
						!gb->display.frame_skip_count;
				}

				/* If interlaced is activated, change which lines get
				 * updated. Also, only update lines on frames that are
				 * actually drawn when frame skip is enabled. */
				if(gb->direct.interlace &&
						(!gb->direct.frame_skip ||
						 gb->display.frame_skip_count))
				{
					gb->display.interlace_count =
						!gb->display.interlace_count;
				}
#endif
                                /* If halted forever, then return on VBLANK. */
                                if(gb->gb_halt && !gb->hram_io[IO_IE])
					break;
			}
			/* Start of normal Line (not in VBLANK) */
			else if(gb->hram_io[IO_LY] < LCD_HEIGHT)
			{
				if(gb->hram_io[IO_LY] == 0)
				{
					/* Clear Screen */
					gb->display.WY = gb->hram_io[IO_WY];
					gb->display.window_clear = 0;
				}

				/* OAM Search occurs at the start of the line. */
				gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_OAM_SCAN;
				gb->counter.lcd_count = 0;

#if WALNUT_FULL_GBC_SUPPORT
				//DMA GBC
				if(gb->cgb.cgbMode && !gb->cgb.dmaActive && gb->cgb.dmaMode)
				{
#if WALNUT_GB_32BIT_DMA
					// Optimized 16-bit path
					for (uint8_t i = 0; i < 0x10; i += 4)
					{
							uint32_t val = __gb_read32(gb, (gb->cgb.dmaSource & 0xFFF0) + i);
							__gb_write32(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// 8-bit logic if there is some cause to fall back
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 1, val >> 8);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 2, val >> 16);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 3, val >> 24);
					}
#elif WALNUT_GB_16BIT_DMA
					// Optimized 16-bit path
					for (uint8_t i = 0; i < 0x10; i += 2)
					{
							uint16_t val = __gb_read16(gb, (gb->cgb.dmaSource & 0xFFF0) + i);
							__gb_write16(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// 8-bit logic if there is some cause to fall back
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val & 0xFF);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 1, val >> 8);
					}
#else
			    // Original 8-bit path
					for (uint8_t i = 0; i < 0x10; i++)
					{
						__gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i,
										__gb_read(gb, (gb->cgb.dmaSource & 0xFFF0) + i));
					}
#endif

					gb->cgb.dmaSource += 0x10;
					gb->cgb.dmaDest += 0x10;
					if(!(--gb->cgb.dmaSize)) {gb->cgb.dmaActive = 1;
					}
#if WALNUT_GB_SAFE_DUALFETCH_DMA
					gb->prefetch_invalid=true;
#endif
				}
#endif
				if(gb->hram_io[IO_STAT] & STAT_MODE_2_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;

				/* If halted immediately jump to next LCD mode.
				 * From OAM Search to LCD Draw. */
				//if(gb->counter.lcd_count < LCD_MODE2_OAM_SCAN_END)
				//	inst_cycles = LCD_MODE2_OAM_SCAN_END - gb->counter.lcd_count;
				inst_cycles = LCD_MODE2_OAM_SCAN_DURATION;
			}
		}
		/* Go from Mode 3 (LCD Draw) to Mode 0 (HBLANK). */
		else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_LCD_DRAW &&
				gb->counter.lcd_count >= LCD_MODE3_LCD_DRAW_END)
		{
			gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_HBLANK;

			if(gb->hram_io[IO_STAT] & STAT_MODE_0_INTR)
				gb->hram_io[IO_IF] |= LCDC_INTR;

			/* If halted immediately, jump from OAM Scan to LCD Draw. */
			if (gb->counter.lcd_count < LCD_MODE0_HBLANK_MAX_DRUATION)
				inst_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
		}
		/* Go from Mode 2 (OAM Scan) to Mode 3 (LCD Draw). */
		else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_OAM_SCAN &&
				gb->counter.lcd_count >= LCD_MODE2_OAM_SCAN_END)
		{
			gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_LCD_DRAW;
#if ENABLE_LCD
			if(!gb->lcd_blank)
				__gb_draw_line(gb);
#endif
			/* If halted immediately jump to next LCD mode. */
			if (gb->counter.lcd_count < LCD_MODE3_LCD_DRAW_MIN_DURATION)
				inst_cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - gb->counter.lcd_count;
		}
	} while(gb->gb_halt && (gb->hram_io[IO_IF] & gb->hram_io[IO_IE]) == 0);
	/* If halted, loop until an interrupt occurs. */

	// **** 2nd instruction processing ****
#if (WALNUT_GB_SAFE_DUALFETCH_DMA || WALNUT_GB_SAFE_DUALFETCH_MBC)
	if (gb->prefetch_invalid)
	{
		gb->prefetch_invalid=false;
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);// opcode was invalidated so we reload
	}
#endif
	/* Handle interrupts */
	/* If gb_halt is positive, then an interrupt must have occurred by the
	 * time we reach here, because on HALT, we jump to the next interrupt
	 * immediately. We also load opcode here because it may be replacing the previously loaded one from the dual fetch logic's first pass */
	while(gb->gb_halt || (gb->gb_ime &&
			gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & ANY_INTR))
	{
		gb->gb_halt = false;

		if(!gb->gb_ime)
			break;

		/* Disable interrupts */
		gb->gb_ime = false;

		/* Push Program Counter */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);

		/* Call interrupt handler if required. */
		if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & VBLANK_INTR)
		{
			gb->cpu_reg.pc.reg = VBLANK_INTR_ADDR;
			gb->hram_io[IO_IF] ^= VBLANK_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & LCDC_INTR)
		{
			gb->cpu_reg.pc.reg = LCDC_INTR_ADDR;
			gb->hram_io[IO_IF] ^= LCDC_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & TIMER_INTR)
		{
			gb->cpu_reg.pc.reg = TIMER_INTR_ADDR;
			gb->hram_io[IO_IF] ^= TIMER_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & SERIAL_INTR)
		{
			gb->cpu_reg.pc.reg = SERIAL_INTR_ADDR;
			gb->hram_io[IO_IF] ^= SERIAL_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & CONTROL_INTR)
		{
			gb->cpu_reg.pc.reg = CONTROL_INTR_ADDR;			
			gb->hram_io[IO_IF] ^= CONTROL_INTR;
		}
		
		opcode = __gb_read(gb, gb->cpu_reg.pc.reg);

		break;
	}
	// at this point opcode always has the next instruction but pc hasn't yet been incremented so we increment here collectively for all cases
	// that can lead us to this point.
	gb->cpu_reg.pc.reg++;
	inst_cycles = op_cycles[opcode];
	/* Execute opcode */
	switch(opcode)
	{
	case 0x00: /* NOP */
		break;

	case 0x01: /* LD BC, imm */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.bc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;
	case 0x02: /* LD (BC), A */
		__gb_write(gb, gb->cpu_reg.bc.reg, gb->cpu_reg.a);
		break;

	case 0x03: /* INC BC */
		gb->cpu_reg.bc.reg++;
		break;

	case 0x04: /* INC B */
		WGB_INSTR_INC_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0x05: /* DEC B */
		WGB_INSTR_DEC_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0x06: /* LD B, imm */
		gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x07: /* RLCA */
		gb->cpu_reg.a = (gb->cpu_reg.a << 1) | (gb->cpu_reg.a >> 7);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = (gb->cpu_reg.a & 0x01);
		break;

	case 0x08: /* LD (imm), SP */
	{
#if WALNUT_GB_16_BIT_OPS
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
    __gb_write16(gb, addr, gb->cpu_reg.sp.reg);
#else
    uint8_t l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
    __gb_write(gb, addr++, gb->cpu_reg.sp.bytes.p);
    __gb_write(gb, addr, gb->cpu_reg.sp.bytes.s);
#endif
    break;
	}

	case 0x09: /* ADD HL, BC */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.bc.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			(temp ^ gb->cpu_reg.hl.reg ^ gb->cpu_reg.bc.reg) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFFFF0000) ? 1 : 0;
		gb->cpu_reg.hl.reg = (temp & 0x0000FFFF);
		break;
	}

	case 0x0A: /* LD A, (BC) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.bc.reg);
		break;

	case 0x0B: /* DEC BC */
		gb->cpu_reg.bc.reg--;
		break;

	case 0x0C: /* INC C */
		WGB_INSTR_INC_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0x0D: /* DEC C */
		WGB_INSTR_DEC_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0x0E: /* LD C, imm */
		gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x0F: /* RRCA */
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = gb->cpu_reg.a & 0x01;
		gb->cpu_reg.a = (gb->cpu_reg.a >> 1) | (gb->cpu_reg.a << 7);
		break;

	case 0x10: /* STOP */
		//gb->gb_halt = true;
#if WALNUT_FULL_GBC_SUPPORT
		if(gb->cgb.cgbMode & gb->cgb.doubleSpeedPrep)
		{
			gb->cgb.doubleSpeedPrep = 0;
			gb->cgb.doubleSpeed ^= 1;
		}
#endif
		break;

	case 0x11: /* LD DE, imm */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.de.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;

	case 0x12: /* LD (DE), A */
		__gb_write(gb, gb->cpu_reg.de.reg, gb->cpu_reg.a);
		break;

	case 0x13: /* INC DE */
		gb->cpu_reg.de.reg++;
		break;

	case 0x14: /* INC D */
		WGB_INSTR_INC_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0x15: /* DEC D */
		WGB_INSTR_DEC_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0x16: /* LD D, imm */
		gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x17: /* RLA */
	{
		uint8_t temp = gb->cpu_reg.a;
		gb->cpu_reg.a = (gb->cpu_reg.a << 1) | gb->cpu_reg.f.f_bits.c;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = (temp >> 7) & 0x01;
		break;
	}

	case 0x18: /* JR imm */
	{
		int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.pc.reg += temp;
		break;
	}

	case 0x19: /* ADD HL, DE */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.de.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			(temp ^ gb->cpu_reg.hl.reg ^ gb->cpu_reg.de.reg) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFFFF0000) ? 1 : 0;
		gb->cpu_reg.hl.reg = (temp & 0x0000FFFF);
		break;
	}

	case 0x1A: /* LD A, (DE) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.de.reg);
		break;

	case 0x1B: /* DEC DE */
		gb->cpu_reg.de.reg--;
		break;

	case 0x1C: /* INC E */
		WGB_INSTR_INC_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0x1D: /* DEC E */
		WGB_INSTR_DEC_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0x1E: /* LD E, imm */
		gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x1F: /* RRA */
	{
		uint8_t temp = gb->cpu_reg.a;
		gb->cpu_reg.a = gb->cpu_reg.a >> 1 | (gb->cpu_reg.f.f_bits.c << 7);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = temp & 0x1;
		break;
	}

	case 0x20: /* JR NZ, imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x21: /* LD HL, imm */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.hl.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;

	case 0x22: /* LDI (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		gb->cpu_reg.hl.reg++;
		break;

	case 0x23: /* INC HL */
		gb->cpu_reg.hl.reg++;
		break;

	case 0x24: /* INC H */
		WGB_INSTR_INC_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0x25: /* DEC H */
		WGB_INSTR_DEC_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0x26: /* LD H, imm */
		gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x27: /* DAA */
	{
		/* The following is from SameBoy. MIT License. */
		int16_t a = gb->cpu_reg.a;

		if(gb->cpu_reg.f.f_bits.n)
		{
			if(gb->cpu_reg.f.f_bits.h)
				a = (a - 0x06) & 0xFF;

			if(gb->cpu_reg.f.f_bits.c)
				a -= 0x60;
		}
		else
		{
			if(gb->cpu_reg.f.f_bits.h || (a & 0x0F) > 9)
				a += 0x06;

			if(gb->cpu_reg.f.f_bits.c || a > 0x9F)
				a += 0x60;
		}

		if((a & 0x100) == 0x100)
			gb->cpu_reg.f.f_bits.c = 1;

		gb->cpu_reg.a = (uint8_t)a;
		gb->cpu_reg.f.f_bits.z = (gb->cpu_reg.a == 0);
		gb->cpu_reg.f.f_bits.h = 0;

		break;
	}

	case 0x28: /* JR Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x29: /* ADD HL, HL */
	{
		gb->cpu_reg.f.f_bits.c = (gb->cpu_reg.hl.reg & 0x8000) > 0;
		gb->cpu_reg.hl.reg <<= 1;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = (gb->cpu_reg.hl.reg & 0x1000) > 0;
		break;
	}

	case 0x2A: /* LD A, (HL+) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg++);
		break;

	case 0x2B: /* DEC HL */
		gb->cpu_reg.hl.reg--;
		break;

	case 0x2C: /* INC L */
		WGB_INSTR_INC_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0x2D: /* DEC L */
		WGB_INSTR_DEC_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0x2E: /* LD L, imm */
		gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x2F: /* CPL */
		gb->cpu_reg.a = ~gb->cpu_reg.a;
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h = 1;
		break;

	case 0x30: /* JR NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x31: /* LD SP, imm */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.sp.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.sp.bytes.p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.sp.bytes.s = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;

	case 0x32: /* LD (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		gb->cpu_reg.hl.reg--;
		break;

	case 0x33: /* INC SP */
		gb->cpu_reg.sp.reg++;
		break;

	case 0x34: /* INC (HL) */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.hl.reg);
		WGB_INSTR_INC_R8(temp);
		__gb_write(gb, gb->cpu_reg.hl.reg, temp);
		break;
	}

	case 0x35: /* DEC (HL) */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.hl.reg);
		WGB_INSTR_DEC_R8(temp);
		__gb_write(gb, gb->cpu_reg.hl.reg, temp);
		break;
	}

	case 0x36: /* LD (HL), imm */
		__gb_write(gb, gb->cpu_reg.hl.reg, __gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0x37: /* SCF */
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = 0;
		gb->cpu_reg.f.f_bits.c = 1;
		break;

	case 0x38: /* JR C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x39: /* ADD HL, SP */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.sp.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			((gb->cpu_reg.hl.reg & 0xFFF) + (gb->cpu_reg.sp.reg & 0xFFF)) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = temp & 0x10000 ? 1 : 0;
		gb->cpu_reg.hl.reg = (uint16_t)temp;
		break;
	}

	case 0x3A: /* LD A, (HL) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg--);
		break;

	case 0x3B: /* DEC SP */
		gb->cpu_reg.sp.reg--;
		break;

	case 0x3C: /* INC A */
		WGB_INSTR_INC_R8(gb->cpu_reg.a);
		break;

	case 0x3D: /* DEC A */
		WGB_INSTR_DEC_R8(gb->cpu_reg.a);
		break;

	case 0x3E: /* LD A, imm */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x3F: /* CCF */
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = 0;
		gb->cpu_reg.f.f_bits.c = ~gb->cpu_reg.f.f_bits.c;
		break;

	case 0x40: /* LD B, B */
		break;

	case 0x41: /* LD B, C */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x42: /* LD B, D */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.de.bytes.d;
		break;

	case 0x43: /* LD B, E */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.de.bytes.e;
		break;

	case 0x44: /* LD B, H */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x45: /* LD B, L */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x46: /* LD B, (HL) */
		gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x47: /* LD B, A */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.a;
		break;

	case 0x48: /* LD C, B */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x49: /* LD C, C */
		break;

	case 0x4A: /* LD C, D */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.de.bytes.d;
		break;

	case 0x4B: /* LD C, E */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.de.bytes.e;
		break;

	case 0x4C: /* LD C, H */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x4D: /* LD C, L */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x4E: /* LD C, (HL) */
		gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x4F: /* LD C, A */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.a;
		break;

	case 0x50: /* LD D, B */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x51: /* LD D, C */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x52: /* LD D, D */
		break;

	case 0x53: /* LD D, E */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.de.bytes.e;
		break;

	case 0x54: /* LD D, H */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x55: /* LD D, L */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x56: /* LD D, (HL) */
		gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x57: /* LD D, A */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.a;
		break;

	case 0x58: /* LD E, B */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x59: /* LD E, C */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x5A: /* LD E, D */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.de.bytes.d;
		break;

	case 0x5B: /* LD E, E */
		break;

	case 0x5C: /* LD E, H */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x5D: /* LD E, L */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x5E: /* LD E, (HL) */
		gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x5F: /* LD E, A */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.a;
		break;

	case 0x60: /* LD H, B */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x61: /* LD H, C */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x62: /* LD H, D */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.de.bytes.d;
		break;

	case 0x63: /* LD H, E */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.de.bytes.e;
		break;

	case 0x64: /* LD H, H */
		break;

	case 0x65: /* LD H, L */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x66: /* LD H, (HL) */
		gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x67: /* LD H, A */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.a;
		break;

	case 0x68: /* LD L, B */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x69: /* LD L, C */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x6A: /* LD L, D */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.de.bytes.d;
		break;

	case 0x6B: /* LD L, E */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.de.bytes.e;
		break;

	case 0x6C: /* LD L, H */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x6D: /* LD L, L */
		break;

	case 0x6E: /* LD L, (HL) */
		gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x6F: /* LD L, A */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.a;
		break;

	case 0x70: /* LD (HL), B */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.bc.bytes.b);
		break;

	case 0x71: /* LD (HL), C */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.bc.bytes.c);
		break;

	case 0x72: /* LD (HL), D */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.de.bytes.d);
		break;

	case 0x73: /* LD (HL), E */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.de.bytes.e);
		break;

	case 0x74: /* LD (HL), H */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.hl.bytes.h);
		break;

	case 0x75: /* LD (HL), L */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.hl.bytes.l);
		break;

	case 0x76: /* HALT */
	{
		int_fast16_t halt_cycles = INT_FAST16_MAX;

		/* TODO: Emulate HALT bug? */
		gb->gb_halt = true;

		if(gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
		{
			int serial_cycles = SERIAL_CYCLES -
				gb->counter.serial_count;

			if(serial_cycles < halt_cycles)
				halt_cycles = serial_cycles;
		}

		if(gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK)
		{
			int tac_cycles = TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK] -
				gb->counter.tima_count;

			if(tac_cycles < halt_cycles)
				halt_cycles = tac_cycles;
		}

		if((gb->hram_io[IO_LCDC] & LCDC_ENABLE))
		{
			int lcd_cycles;

			/* If LCD is in HBlank, calculate the number of cycles
			 * until the end of HBlank and the start of mode 2 or
			 * mode 1. */
			if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_HBLANK)
			{
				lcd_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
			}
			else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_OAM_SCAN)
			{
				lcd_cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - gb->counter.lcd_count;
			}
			else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_LCD_DRAW)
			{
				lcd_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
			}
			else
			{
				/* VBlank */
				lcd_cycles = LCD_LINE_CYCLES - gb->counter.lcd_count;
			}

			if(lcd_cycles < halt_cycles)
				halt_cycles = lcd_cycles;
		}

		/* Some halt cycles may already be very high, so make sure we
		 * don't underflow here. */
		if(halt_cycles <= 0)
			halt_cycles = 4;

		inst_cycles = (uint_fast16_t)halt_cycles;
		break;
	}

	case 0x77: /* LD (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		break;

	case 0x78: /* LD A, B */
		gb->cpu_reg.a = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x79: /* LD A, C */
		gb->cpu_reg.a = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x7A: /* LD A, D */
		gb->cpu_reg.a = gb->cpu_reg.de.bytes.d;
		break;

	case 0x7B: /* LD A, E */
		gb->cpu_reg.a = gb->cpu_reg.de.bytes.e;
		break;

	case 0x7C: /* LD A, H */
		gb->cpu_reg.a = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x7D: /* LD A, L */
		gb->cpu_reg.a = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x7E: /* LD A, (HL) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x7F: /* LD A, A */
		break;

	case 0x80: /* ADD A, B */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.b, 0);
		break;

	case 0x81: /* ADD A, C */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.c, 0);
		break;

	case 0x82: /* ADD A, D */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.d, 0);
		break;

	case 0x83: /* ADD A, E */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.e, 0);
		break;

	case 0x84: /* ADD A, H */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.h, 0);
		break;

	case 0x85: /* ADD A, L */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.l, 0);
		break;

	case 0x86: /* ADD A, (HL) */
		WGB_INSTR_ADC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), 0);
		break;

	case 0x87: /* ADD A, A */
		WGB_INSTR_ADC_R8(gb->cpu_reg.a, 0);
		break;

	case 0x88: /* ADC A, B */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.b, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x89: /* ADC A, C */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.c, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8A: /* ADC A, D */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.d, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8B: /* ADC A, E */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.e, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8C: /* ADC A, H */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.h, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8D: /* ADC A, L */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.l, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8E: /* ADC A, (HL) */
		WGB_INSTR_ADC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8F: /* ADC A, A */
		WGB_INSTR_ADC_R8(gb->cpu_reg.a, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x90: /* SUB B */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.b, 0);
		break;

	case 0x91: /* SUB C */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.c, 0);
		break;

	case 0x92: /* SUB D */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.d, 0);
		break;

	case 0x93: /* SUB E */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.e, 0);
		break;

	case 0x94: /* SUB H */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.h, 0);
		break;

	case 0x95: /* SUB L */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.l, 0);
		break;

	case 0x96: /* SUB (HL) */
		WGB_INSTR_SBC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), 0);
		break;

	case 0x97: /* SUB A */
		gb->cpu_reg.a = 0;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 1;
		break;

	case 0x98: /* SBC A, B */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.b, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x99: /* SBC A, C */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.c, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9A: /* SBC A, D */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.d, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9B: /* SBC A, E */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.e, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9C: /* SBC A, H */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.h, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9D: /* SBC A, L */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.l, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9E: /* SBC A, (HL) */
		WGB_INSTR_SBC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9F: /* SBC A, A */
		gb->cpu_reg.a = gb->cpu_reg.f.f_bits.c ? 0xFF : 0x00;
		gb->cpu_reg.f.f_bits.z = !gb->cpu_reg.f.f_bits.c;
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h = gb->cpu_reg.f.f_bits.c;
		break;

	case 0xA0: /* AND B */
		WGB_INSTR_AND_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xA1: /* AND C */
		WGB_INSTR_AND_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xA2: /* AND D */
		WGB_INSTR_AND_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xA3: /* AND E */
		WGB_INSTR_AND_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xA4: /* AND H */
		WGB_INSTR_AND_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xA5: /* AND L */
		WGB_INSTR_AND_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xA6: /* AND (HL) */
		WGB_INSTR_AND_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xA7: /* AND A */
		WGB_INSTR_AND_R8(gb->cpu_reg.a);
		break;

	case 0xA8: /* XOR B */
		WGB_INSTR_XOR_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xA9: /* XOR C */
		WGB_INSTR_XOR_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xAA: /* XOR D */
		WGB_INSTR_XOR_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xAB: /* XOR E */
		WGB_INSTR_XOR_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xAC: /* XOR H */
		WGB_INSTR_XOR_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xAD: /* XOR L */
		WGB_INSTR_XOR_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xAE: /* XOR (HL) */
		WGB_INSTR_XOR_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xAF: /* XOR A */
		WGB_INSTR_XOR_R8(gb->cpu_reg.a);
		break;

	case 0xB0: /* OR B */
		WGB_INSTR_OR_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xB1: /* OR C */
		WGB_INSTR_OR_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xB2: /* OR D */
		WGB_INSTR_OR_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xB3: /* OR E */
		WGB_INSTR_OR_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xB4: /* OR H */
		WGB_INSTR_OR_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xB5: /* OR L */
		WGB_INSTR_OR_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xB6: /* OR (HL) */
		WGB_INSTR_OR_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xB7: /* OR A */
		WGB_INSTR_OR_R8(gb->cpu_reg.a);
		break;

	case 0xB8: /* CP B */
		WGB_INSTR_CP_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xB9: /* CP C */
		WGB_INSTR_CP_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xBA: /* CP D */
		WGB_INSTR_CP_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xBB: /* CP E */
		WGB_INSTR_CP_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xBC: /* CP H */
		WGB_INSTR_CP_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xBD: /* CP L */
		WGB_INSTR_CP_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xBE: /* CP (HL) */
		WGB_INSTR_CP_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xBF: /* CP A */
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 1;
		break;

	case 0xC0: /* RET NZ */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2;
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
        inst_cycles += 12;
		}

		break;

	case 0xC1: /* POP BC */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.bc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;

	case 0xC2: /* JP NZ, imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xC3: /* JP imm */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
#else
{
    uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.pc.bytes.c = c;
    gb->cpu_reg.pc.bytes.p = p;
}
#endif
		break;

	case 0xC4: /* CALL NZ imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS
			uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
			gb->cpu_reg.pc.reg += 2;
      gb->cpu_reg.sp.reg -= 2;
      __gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);
			gb->cpu_reg.pc.reg = addr;
#else
			uint8_t c, p;
			c = __gb_read(gb, gb->cpu_reg.pc.reg++);
			p = __gb_read(gb, gb->cpu_reg.pc.reg++);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
			gb->cpu_reg.pc.bytes.c = c;
			gb->cpu_reg.pc.bytes.p = p;
#endif        
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xC5: /* PUSH BC */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.bc.bytes.b);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.bc.bytes.c);
		break;

	case 0xC6: /* ADD A, imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_ADC_R8(val, 0);
		break;
	}

	case 0xC7: /* RST 0x0000 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0000;
		break;

	case 0xC8: /* RET Z */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS
        // Optional 16-bit path if stack is aligned
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2;
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
			inst_cycles += 12;
		}
		break;

	case 0xC9: /* RET */
	{
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;
	}

	case 0xCA: /* JP Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg); // no need to advance pc because of jump
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xCB: /* CB INST */
		inst_cycles = __gb_execute_cb(gb);
		break;

	case 0xCC: /* CALL Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_OPS
        uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance past immediate

        // push old PC
				gb->cpu_reg.sp.reg-=2;
        __gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);

        gb->cpu_reg.pc.reg = addr;
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xCD: /* CALL imm */
#if WALNUT_GB_16_BIT_OPS
{
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2; // advance past immediate
		gb->cpu_reg.sp.reg-=2;
		__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg = addr;
}
#else
{
    uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
    __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
    gb->cpu_reg.pc.bytes.c = c;
    gb->cpu_reg.pc.bytes.p = p;
}
#endif
	break;

	case 0xCE: /* ADC A, imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_ADC_R8(val, gb->cpu_reg.f.f_bits.c);
		break;
	}

	case 0xCF: /* RST 0x0008 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0008;
		break;

	case 0xD0: /* RET NC */

    if (!gb->cpu_reg.f.f_bits.c)
    {
#if WALNUT_GB_16_BIT_OPS
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
        inst_cycles += 12;
    }

		break;

	case 0xD1: /* POP DE */
#if WALNUT_GB_16_BIT_OPS
    gb->cpu_reg.de.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;

	case 0xD2: /* JP NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_OPS
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg); // jump so no need to advance pc
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xD4: /* CALL NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_OPS
        uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past immediate
				gb->cpu_reg.sp.reg-=2;
        __gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg = addr;
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xD5: /* PUSH DE */
	// this was revised but working backwards from this 16-bit op to find the one thats broken
#if WALNUT_GB_16_BIT_OPS
		gb->cpu_reg.sp.reg-=2;
		__gb_write16(gb,gb->cpu_reg.sp.reg,gb->cpu_reg.de.reg);
#else
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.de.bytes.d);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.de.bytes.e);
#endif
		break;

	case 0xD6: /* SUB imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		uint16_t temp = gb->cpu_reg.a - val;
		gb->cpu_reg.f.f_bits.z = ((temp & 0xFF) == 0x00);
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h =
			(gb->cpu_reg.a ^ val ^ temp) & 0x10 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFF00) ? 1 : 0;
		gb->cpu_reg.a = (temp & 0xFF);
		break;
	}

	case 0xD7: /* RST 0x0010 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0010;
		break;

	case 0xD8: /* RET C */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
			inst_cycles += 12;
		}

		break;

	case 0xD9: /* RETI */
	{
#if WALNUT_GB_16_BIT
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
    gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		gb->gb_ime = true;
	}
	break;

	case 0xDA: /* JP C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past the immediate word
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xDC: /* CALL C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT
        uint16_t target = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past the immediate word
        // push current PC onto stack
        gb->cpu_reg.sp.reg -= 2;
				__gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg)
        gb->cpu_reg.pc.reg = target;
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xDE: /* SBC A, imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_SBC_R8(val, gb->cpu_reg.f.f_bits.c);
		break;
	}

	case 0xDF: /* RST 0x0018 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0018;
		break;

	case 0xE0: /* LD (0xFF00+imm), A */
		__gb_write(gb, 0xFF00 | __gb_read(gb, gb->cpu_reg.pc.reg++),
			   gb->cpu_reg.a);
		break;

	case 0xE1: /* POP HL */
#if WALNUT_GB_16_BIT
    gb->cpu_reg.hl.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2; // advance SP past the popped value
#else
    gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;

	case 0xE2: /* LD (C), A */
		__gb_write(gb, 0xFF00 | gb->cpu_reg.bc.bytes.c, gb->cpu_reg.a);
		break;

	case 0xE5: /* PUSH HL */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.hl.bytes.h);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.hl.bytes.l);
		break;

	case 0xE6: /* AND imm */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_AND_R8(temp);
		break;
	}

	case 0xE7: /* RST 0x0020 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0020;
		break;

	case 0xE8: /* ADD SP, imm */
	{
		int8_t offset = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.h = ((gb->cpu_reg.sp.reg & 0xF) + (offset & 0xF) > 0xF) ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = ((gb->cpu_reg.sp.reg & 0xFF) + (offset & 0xFF) > 0xFF);
		gb->cpu_reg.sp.reg += offset;
		break;
	}

	case 0xE9: /* JP (HL) */
		gb->cpu_reg.pc.reg = gb->cpu_reg.hl.reg;
		break;

	case 0xEA: /* LD (imm), A */
	{
#if WALNUT_GB_16_BIT
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2; // advance past the immediate
#else
    uint8_t l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
#endif
		__gb_write(gb, addr, gb->cpu_reg.a);
		break;
	}

	case 0xEE: /* XOR imm */
		WGB_INSTR_XOR_R8(__gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0xEF: /* RST 0x0028 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0028;
		break;

	case 0xF0: /* LD A, (0xFF00+imm) */
		gb->cpu_reg.a =
			__gb_read(gb, 0xFF00 | __gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0xF1: /* POP AF */
	{
		uint8_t temp_8 = __gb_read(gb, gb->cpu_reg.sp.reg++);
		gb->cpu_reg.f.f_bits.z = (temp_8 >> 7) & 1;
		gb->cpu_reg.f.f_bits.n = (temp_8 >> 6) & 1;
		gb->cpu_reg.f.f_bits.h = (temp_8 >> 5) & 1;
		gb->cpu_reg.f.f_bits.c = (temp_8 >> 4) & 1;
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.sp.reg++);
		break;
	}

	case 0xF2: /* LD A, (C) */
		gb->cpu_reg.a = __gb_read(gb, 0xFF00 | gb->cpu_reg.bc.bytes.c);
		break;

	case 0xF3: /* DI */
		gb->gb_ime = false;
		break;

	case 0xF5: /* PUSH AF */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.a);
		__gb_write(gb, --gb->cpu_reg.sp.reg,
			   gb->cpu_reg.f.f_bits.z << 7 | gb->cpu_reg.f.f_bits.n << 6 |
			   gb->cpu_reg.f.f_bits.h << 5 | gb->cpu_reg.f.f_bits.c << 4);
		break;

	case 0xF6: /* OR imm */
		WGB_INSTR_OR_R8(__gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0xF7: /* PUSH AF */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0030;
		break;

	case 0xF8: /* LD HL, SP+/-imm */
	{
		/* Taken from SameBoy, which is released under MIT Licence. */
		int8_t offset = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.hl.reg = gb->cpu_reg.sp.reg + offset;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.h = ((gb->cpu_reg.sp.reg & 0xF) + (offset & 0xF) > 0xF) ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = ((gb->cpu_reg.sp.reg & 0xFF) + (offset & 0xFF) > 0xFF) ? 1 : 0;
		break;
	}

	case 0xF9: /* LD SP, HL */
		gb->cpu_reg.sp.reg = gb->cpu_reg.hl.reg;
		break;

	case 0xFA: /* LD A, (imm) */
	{
#if WALNUT_GB_16_BIT
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2; // advance past the immediate
#else
    uint8_t l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
#endif
		gb->cpu_reg.a = __gb_read(gb, addr);
		break;
	}

	case 0xFB: /* EI */
		gb->gb_ime = true;
		break;

	case 0xFE: /* CP imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_CP_R8(val);
		break;
	}

	case 0xFF: /* RST 0x0038 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0038;
		break;

	default:
		/* Return address where invalid opcode that was read. */
		(gb->gb_error)(gb, GB_INVALID_OPCODE, gb->cpu_reg.pc.reg - 1);
		WGB_UNREACHABLE();
	}

	// *** post opcode handling ***

	do
	{
		/* DIV register timing */
		gb->counter.div_count += inst_cycles;
		while(gb->counter.div_count >= DIV_CYCLES)
		{
			gb->hram_io[IO_DIV]++;
			gb->counter.div_count -= DIV_CYCLES;
		}

		/* Check for RTC tick. */
		if(gb->mbc == 3 && (gb->rtc_real.reg.high & 0x40) == 0)
		{
			gb->counter.rtc_count += inst_cycles;
			while(WGB_UNLIKELY(gb->counter.rtc_count >= RTC_CYCLES))
			{
				gb->counter.rtc_count -= RTC_CYCLES;

				/* Detect invalid rollover. */
				if(WGB_UNLIKELY(gb->rtc_real.reg.sec == 63))
				{
					gb->rtc_real.reg.sec = 0;
					continue;
				}

				if(++gb->rtc_real.reg.sec != 60)
					continue;

				gb->rtc_real.reg.sec = 0;
				if(gb->rtc_real.reg.min == 63)
				{
					gb->rtc_real.reg.min = 0;
					continue;
				}
				if(++gb->rtc_real.reg.min != 60)
					continue;

				gb->rtc_real.reg.min = 0;
				if(gb->rtc_real.reg.hour == 31)
				{
					gb->rtc_real.reg.hour = 0;
					continue;
				}
				if(++gb->rtc_real.reg.hour != 24)
					continue;

				gb->rtc_real.reg.hour = 0;
				if(++gb->rtc_real.reg.yday != 0)
					continue;

				if(gb->rtc_real.reg.high & 1)  /* Bit 8 of days*/
					gb->rtc_real.reg.high |= 0x80; /* Overflow bit */

				gb->rtc_real.reg.high ^= 1;
			}
		}

		/* Check serial transmission. */
		if(gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
		{
			unsigned int serial_cycles = SERIAL_CYCLES_1KB;

			/* If new transfer, call TX function. */
			if(gb->counter.serial_count == 0 &&
				gb->gb_serial_tx != NULL)
				(gb->gb_serial_tx)(gb, gb->hram_io[IO_SB]);

#if WALNUT_FULL_GBC_SUPPORT
			if(gb->hram_io[IO_SC] & 0x3)
				serial_cycles = SERIAL_CYCLES_32KB;
#endif

			gb->counter.serial_count += inst_cycles;

			/* If it's time to receive byte, call RX function. */
			if(gb->counter.serial_count >= serial_cycles)
			{
				/* If RX can be done, do it. */
				/* If RX failed, do not change SB if using external
				 * clock, or set to 0xFF if using internal clock. */
				uint8_t rx;

				if(gb->gb_serial_rx != NULL &&
					(gb->gb_serial_rx(gb, &rx) ==
						GB_SERIAL_RX_SUCCESS))
				{
					gb->hram_io[IO_SB] = rx;

					/* Inform game of serial TX/RX completion. */
					gb->hram_io[IO_SC] &= 0x01;
					gb->hram_io[IO_IF] |= SERIAL_INTR;
				}
				else if(gb->hram_io[IO_SC] & SERIAL_SC_CLOCK_SRC)
				{
					/* If using internal clock, and console is not
					 * attached to any external peripheral, shifted
					 * bits are replaced with logic 1. */
					gb->hram_io[IO_SB] = 0xFF;

					/* Inform game of serial TX/RX completion. */
					gb->hram_io[IO_SC] &= 0x01;
					gb->hram_io[IO_IF] |= SERIAL_INTR;
				}
				else
				{
					/* If using external clock, and console is not
					 * attached to any external peripheral, bits are
					 * not shifted, so SB is not modified. */
				}

				gb->counter.serial_count = 0;
			}
		}

		/* TIMA register timing */
		/* TODO: Change tac_enable to struct of TAC timer control bits. */
		if(gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK)
		{
			gb->counter.tima_count += inst_cycles;

			while(gb->counter.tima_count >=
				TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK])
			{
				gb->counter.tima_count -=
					TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK];

				if(++gb->hram_io[IO_TIMA] == 0)
				{
					gb->hram_io[IO_IF] |= TIMER_INTR;
					/* On overflow, set TMA to TIMA. */
					gb->hram_io[IO_TIMA] = gb->hram_io[IO_TMA];
				}
			}
		}

		/* If LCD is off, don't update LCD state or increase the LCD
		 * ticks. Instead, keep track of the amount of time that is
		 * being passed. */
		if(!(gb->hram_io[IO_LCDC] & LCDC_ENABLE))
		{
			gb->counter.lcd_off_count += inst_cycles;
			if(gb->counter.lcd_off_count >= LCD_FRAME_CYCLES)
			{
				gb->counter.lcd_off_count -= LCD_FRAME_CYCLES;
				gb->gb_frame = true;
			}
			continue;
		}

		/* LCD Timing */
#if WALNUT_FULL_GBC_SUPPORT
        if (inst_cycles > 1) {
            gb->counter.lcd_count += (inst_cycles >> gb->cgb.doubleSpeed);
        } else {
#endif
		gb->counter.lcd_count += inst_cycles;
#if WALNUT_FULL_GBC_SUPPORT
	}
#endif

		/* New Scanline. HBlank -> VBlank or OAM Scan */
		if(gb->counter.lcd_count >= LCD_LINE_CYCLES)
		{
			gb->counter.lcd_count -= LCD_LINE_CYCLES;

			/* Next line */
			gb->hram_io[IO_LY] = gb->hram_io[IO_LY] + 1;
			if (gb->hram_io[IO_LY] == LCD_VERT_LINES)
				gb->hram_io[IO_LY] = 0;

			/* LYC Update */
			if(gb->hram_io[IO_LY] == gb->hram_io[IO_LYC])
			{
				gb->hram_io[IO_STAT] |= STAT_LYC_COINC;

				if(gb->hram_io[IO_STAT] & STAT_LYC_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;
			}
			else
				gb->hram_io[IO_STAT] &= 0xFB;

			/* Check if LCD should be in Mode 1 (VBLANK) state */
			if(gb->hram_io[IO_LY] == LCD_HEIGHT)
			{
				gb->hram_io[IO_STAT] =
					(gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_VBLANK;
				gb->gb_frame = true;
				gb->hram_io[IO_IF] |= VBLANK_INTR;
				gb->lcd_blank = false;

				if(gb->hram_io[IO_STAT] & STAT_MODE_1_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;

#if ENABLE_LCD
				/* If frame skip is activated, check if we need to draw
				 * the frame or skip it. */
				if(gb->direct.frame_skip)
				{
					gb->display.frame_skip_count =
						!gb->display.frame_skip_count;
				}

				/* If interlaced is activated, change which lines get
				 * updated. Also, only update lines on frames that are
				 * actually drawn when frame skip is enabled. */
				if(gb->direct.interlace &&
						(!gb->direct.frame_skip ||
						 gb->display.frame_skip_count))
				{
					gb->display.interlace_count =
						!gb->display.interlace_count;
				}
#endif
                                /* If halted forever, then return on VBLANK. */
                                if(gb->gb_halt && !gb->hram_io[IO_IE])
					break;
			}
			/* Start of normal Line (not in VBLANK) */
			else if(gb->hram_io[IO_LY] < LCD_HEIGHT)
			{
				if(gb->hram_io[IO_LY] == 0)
				{
					/* Clear Screen */
					gb->display.WY = gb->hram_io[IO_WY];
					gb->display.window_clear = 0;
				}

				/* OAM Search occurs at the start of the line. */
				gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_OAM_SCAN;
				gb->counter.lcd_count = 0;


#if WALNUT_FULL_GBC_SUPPORT
				//DMA GBC
				if(gb->cgb.cgbMode && !gb->cgb.dmaActive && gb->cgb.dmaMode)
				{
#if WALNUT_GB_32BIT_DMA
					// Optimized 16-bit path
					for (uint8_t i = 0; i < 0x10; i += 4)
					{
							uint32_t val = __gb_read32(gb, (gb->cgb.dmaSource & 0xFFF0) + i);
							__gb_write32(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// 8-bit logic if there is some cause to fall back
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 1, val >> 8);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 2, val >> 16);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 3, val >> 24);
					}
#elif WALNUT_GB_16BIT_DMA
					// Optimized 16-bit path
					for (uint8_t i = 0; i < 0x10; i += 2)
					{
							uint16_t val = __gb_read16(gb, (gb->cgb.dmaSource & 0xFFF0) + i);
							__gb_write16(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// 8-bit logic if there is some cause to fall back
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val & 0xFF);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 1, val >> 8);
					}
#else
			    // Original 8-bit path
					for (uint8_t i = 0; i < 0x10; i++)
					{
						__gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i,
										__gb_read(gb, (gb->cgb.dmaSource & 0xFFF0) + i));
					}
#endif

					gb->cgb.dmaSource += 0x10;
					gb->cgb.dmaDest += 0x10;
					if(!(--gb->cgb.dmaSize)) {gb->cgb.dmaActive = 1;
					}
				}
#endif
				if(gb->hram_io[IO_STAT] & STAT_MODE_2_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;

				/* If halted immediately jump to next LCD mode.
				 * From OAM Search to LCD Draw. */
				//if(gb->counter.lcd_count < LCD_MODE2_OAM_SCAN_END)
				//	inst_cycles = LCD_MODE2_OAM_SCAN_END - gb->counter.lcd_count;
				inst_cycles = LCD_MODE2_OAM_SCAN_DURATION;
			}
		}
		/* Go from Mode 3 (LCD Draw) to Mode 0 (HBLANK). */
		else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_LCD_DRAW &&
				gb->counter.lcd_count >= LCD_MODE3_LCD_DRAW_END)
		{
			gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_HBLANK;

			if(gb->hram_io[IO_STAT] & STAT_MODE_0_INTR)
				gb->hram_io[IO_IF] |= LCDC_INTR;

			/* If halted immediately, jump from OAM Scan to LCD Draw. */
			if (gb->counter.lcd_count < LCD_MODE0_HBLANK_MAX_DRUATION)
				inst_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
		}
		/* Go from Mode 2 (OAM Scan) to Mode 3 (LCD Draw). */
		else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_OAM_SCAN &&
				gb->counter.lcd_count >= LCD_MODE2_OAM_SCAN_END)
		{
			gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_LCD_DRAW;
#if ENABLE_LCD
			if(!gb->lcd_blank)
				__gb_draw_line(gb);
#endif
			/* If halted immediately jump to next LCD mode. */
			if (gb->counter.lcd_count < LCD_MODE3_LCD_DRAW_MIN_DURATION)
				inst_cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - gb->counter.lcd_count;
		}
	} while(gb->gb_halt && (gb->hram_io[IO_IF] & gb->hram_io[IO_IE]) == 0);
}

void gb_run_frame(struct gb_s *gb)
{
	gb->gb_frame = false;
#if (WALNUT_GB_SAFE_DUALFETCH_DMA || WALNUT_GB_SAFE_DUALFETCH_MBC)
	gb->prefetch_invalid=false; // this is toggled internally, only needs to be set once - 60 times a second is no harm though
#endif
	while(!gb->gb_frame)
	{
		__gb_step_cpu_x(gb);
	}
}

void gb_run_frame_dualfetch(struct gb_s *gb)
{
	gb->gb_frame = false;
#if (WALNUT_GB_SAFE_DUALFETCH_DMA || WALNUT_GB_SAFE_DUALFETCH_MBC)
	gb->prefetch_invalid=false; // this is toggled internally, only needs to be set once - 60 times a second is no harm though
#endif
	while(!gb->gb_frame)
	{
			__gb_step_cpu(gb);
	}
}

int gb_get_save_size_s(struct gb_s *gb, size_t *ram_size)
{
	const uint_fast16_t ram_size_location = 0x0149;
	const uint_fast32_t ram_sizes[] =
	{
		/* 0,  2KiB,   8KiB,  32KiB,  128KiB,   64KiB */
		0x00, 0x800, 0x2000, 0x8000, 0x20000, 0x10000
	};
	uint8_t ram_size_code = gb->gb_rom_read(gb, ram_size_location);

	/* MBC2 always has 512 half-bytes of cart RAM.
	 * This assumes that only the lower nibble of each byte is used; the
	 * nibbles are not packed. */
	if(gb->mbc == 2)
	{
		*ram_size = 0x200;
		return 0;
	}

	/* Return -1 on invalid or unsupported RAM size. */
	if(ram_size_code >= WALNUT_GB_ARRAYSIZE(ram_sizes))
		return -1;

	*ram_size = ram_sizes[ram_size_code];
	return 0;
}

WGB_DEPRECATED("Does not return error code. Use gb_get_save_size_s instead.")
uint_fast32_t gb_get_save_size(struct gb_s *gb)
{
	const uint_fast16_t ram_size_location = 0x0149;
	const uint_fast32_t ram_sizes[] =
	{
		/* 0,  2KiB,   8KiB,  32KiB,  128KiB,   64KiB */
		0x00, 0x800, 0x2000, 0x8000, 0x20000, 0x10000
	};
	uint8_t ram_size_code = gb->gb_rom_read(gb, ram_size_location);

	/* MBC2 always has 512 half-bytes of cart RAM.
	 * This assumes that only the lower nibble of each byte is used; the
	 * nibbles are not packed. */
	if(gb->mbc == 2)
		return 0x200;

	/* Return 0 on invalid or unsupported RAM size. */
	if(ram_size_code >= WALNUT_GB_ARRAYSIZE(ram_sizes))
		return 0;

	return ram_sizes[ram_size_code];
}

void gb_init_serial(struct gb_s *gb,
		    void (*gb_serial_tx)(struct gb_s*, const uint8_t),
		    enum gb_serial_rx_ret_e (*gb_serial_rx)(struct gb_s*,
			    uint8_t*))
{
	gb->gb_serial_tx = gb_serial_tx;
	gb->gb_serial_rx = gb_serial_rx;
}

uint8_t gb_colour_hash(struct gb_s *gb)
{
#define ROM_TITLE_START_ADDR	0x0134
#define ROM_TITLE_END_ADDR	0x0143

	uint8_t x = 0;
	uint16_t i;

	for(i = ROM_TITLE_START_ADDR; i <= ROM_TITLE_END_ADDR; i++)
		x += gb->gb_rom_read(gb, i);

	return x;
}

/**
 * Resets the context, and initialises startup values for a DMG console.
 */
void gb_reset(struct gb_s *gb)
{
	gb->gb_halt = false;
	gb->gb_ime = true;

	/* Initialise MBC values. */
	gb->selected_rom_bank = 1;
	gb->cart_ram_bank = 0;
	gb->enable_cart_ram = 0;
	gb->cart_mode_select = 0;

	/* Use values as though the boot ROM was already executed. */
	if(gb->gb_bootrom_read == NULL)
	{
		uint8_t hdr_chk;
		hdr_chk = gb->gb_rom_read(gb, ROM_HEADER_CHECKSUM_LOC) != 0;

		gb->cpu_reg.a = 0x01;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = hdr_chk;
		gb->cpu_reg.f.f_bits.c = hdr_chk;
		gb->cpu_reg.bc.reg = 0x0013;
		gb->cpu_reg.de.reg = 0x00D8;
		gb->cpu_reg.hl.reg = 0x014D;
		gb->cpu_reg.sp.reg = 0xFFFE;
		gb->cpu_reg.pc.reg = 0x0100;

		gb->hram_io[IO_DIV ] = 0xAB;
		gb->hram_io[IO_LCDC] = 0x91;
		gb->hram_io[IO_STAT] = 0x85;
		gb->hram_io[IO_BOOT] = 0x01;

		__gb_write(gb, 0xFF26, 0xF1);
#if WALNUT_FULL_GBC_SUPPORT
		if(gb->cgb.cgbMode)
		{
			gb->cpu_reg.a = 0x11;
			gb->cpu_reg.f.f_bits.z = 1;
			gb->cpu_reg.f.f_bits.n = 0;
			gb->cpu_reg.f.f_bits.h = hdr_chk;
			gb->cpu_reg.f.f_bits.c = hdr_chk;
			gb->cpu_reg.bc.reg = 0x0000;
			gb->cpu_reg.de.reg = 0x0008;
			gb->cpu_reg.hl.reg = 0x007C;
			gb->hram_io[IO_DIV] = 0xFF;
		}
#endif

		memset(gb->vram, 0x00, VRAM_SIZE);
	}
	else
	{
		/* Set value as though the console was just switched on.
		 * CPU registers are uninitialised. */
		gb->cpu_reg.pc.reg = 0x0000;
		gb->hram_io[IO_DIV ] = 0x00;
		gb->hram_io[IO_LCDC] = 0x00;
		gb->hram_io[IO_STAT] = 0x84;
		gb->hram_io[IO_BOOT] = 0x00;
	}

	gb->counter.lcd_count = 0;
	gb->counter.div_count = 0;
	gb->counter.tima_count = 0;
	gb->counter.serial_count = 0;
	gb->counter.rtc_count = 0;
	gb->counter.lcd_off_count = 0;

	gb->direct.joypad = 0xFF;
	gb->hram_io[IO_JOYP] = 0xCF;
	gb->hram_io[IO_SB  ] = 0x00;
	gb->hram_io[IO_SC  ] = 0x7E;
#if WALNUT_FULL_GBC_SUPPORT
	if(gb->cgb.cgbMode) gb->hram_io[IO_SC] = 0x7F;
#endif
	/* DIV */
	gb->hram_io[IO_TIMA] = 0x00;
	gb->hram_io[IO_TMA ] = 0x00;
	gb->hram_io[IO_TAC ] = 0xF8;
	gb->hram_io[IO_IF  ] = 0xE1;

	/* LCDC */
	/* STAT */
	gb->hram_io[IO_SCY ] = 0x00;
	gb->hram_io[IO_SCX ] = 0x00;
	gb->hram_io[IO_LY  ] = 0x00;
	gb->hram_io[IO_LYC ] = 0x00;
	__gb_write(gb, 0xFF47, 0xFC); // BGP
	__gb_write(gb, 0xFF48, 0xFF); // OBJP0
	__gb_write(gb, 0xFF49, 0xFF); // OBJP1
	gb->hram_io[IO_WY] = 0x00;
	gb->hram_io[IO_WX] = 0x00;
	gb->hram_io[IO_IE] = 0x00;
	gb->hram_io[IO_IF] = 0xE1;
#if WALNUT_FULL_GBC_SUPPORT
	/* Initialize some CGB registers */
	gb->cgb.doubleSpeed = 0;
	gb->cgb.doubleSpeedPrep = 0;
	gb->cgb.wramBank = 1;
	gb->cgb.wramBankOffset = WRAM_0_ADDR;
	gb->cgb.vramBank = 0;
	gb->cgb.vramBankOffset = VRAM_ADDR;
	for (int i = 0; i < 0x20; i++)
	{
		gb->cgb.OAMPalette[(i << 1)] = gb->cgb.BGPalette[(i << 1)] = 0x7F;
		gb->cgb.OAMPalette[(i << 1) + 1] = gb->cgb.BGPalette[(i << 1) + 1] = 0xFF;
	}
	gb->cgb.OAMPaletteID = 0;
	gb->cgb.BGPaletteID = 0;
	gb->cgb.OAMPaletteInc = 0;
	gb->cgb.BGPaletteInc = 0;
	gb->cgb.dmaActive = 1;  // Not active
	gb->cgb.dmaMode = 0;
	gb->cgb.dmaSize = 0;
	gb->cgb.dmaSource = 0;
	gb->cgb.dmaDest = 0;
#endif
}

enum gb_init_error_e gb_init(struct gb_s *gb,
			     uint8_t (*gb_rom_read)(struct gb_s*, const uint_fast32_t),
					 uint16_t (*gb_rom_read16)(struct gb_s*, const uint_fast32_t),
					 uint32_t (*gb_rom_read32)(struct gb_s*, const uint_fast32_t),
			     uint8_t (*gb_cart_ram_read)(struct gb_s*, const uint_fast32_t),
			     void (*gb_cart_ram_write)(struct gb_s*, const uint_fast32_t, const uint8_t),
			     void (*gb_error)(struct gb_s*, const enum gb_error_e, const uint16_t),
			     void *priv)
{
#if WALNUT_FULL_GBC_SUPPORT
	const uint16_t cgb_flag = 0x0143;
#endif
	const uint16_t mbc_location = 0x0147;
	const uint16_t bank_count_location = 0x0148;
	const uint16_t ram_size_location = 0x0149;
	/**
	 * Table for cartridge type (MBC). -1 if invalid.
	 * TODO: MMM01 is untested.
	 * TODO: MBC6 is untested.
	 * TODO: MBC7 is unsupported.
	 * TODO: POCKET CAMERA is unsupported.
	 * TODO: BANDAI TAMA5 is unsupported.
	 * TODO: HuC3 is unsupported.
	 * TODO: HuC1 is unsupported.
	 **/
	const int8_t cart_mbc[] =
	{
		0, 1, 1, 1, -1, 2, 2, -1, 0, 0, -1, 0, 0, 0, -1, 3,
		3, 3, 3, 3, -1, -1, -1, -1, -1, 5, 5, 5, 5, 5, 5, -1
	};
	/* Whether cart has RAM. */
	const uint8_t cart_ram[] =
	{
		0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0,
		1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0
	};
	/* How large the ROM is in banks of 16 KiB. */
	const uint16_t num_rom_banks_mask[] =
	{
		2, 4, 8, 16, 32, 64, 128, 256, 512
	};
	/* How large the cart RAM is in banks of 8 KiB. Code $01 is unused, but
	 * some early homebrew ROMs supposedly may use this value. */
	const uint8_t num_ram_banks[] = { 0, 1, 1, 4, 16, 8 };

	gb->gb_rom_read = gb_rom_read;
	gb->gb_rom_read_16bit = gb_rom_read16;
	gb->gb_rom_read_32bit = gb_rom_read32;
	gb->gb_cart_ram_read = gb_cart_ram_read;
	gb->gb_cart_ram_write = gb_cart_ram_write;
	gb->gb_error = gb_error;
	gb->direct.priv = priv;

	/* Initialise serial transfer function to NULL. If the front-end does
	 * not provide serial support, Walnut-GB will emulate no cable connected
	 * automatically. */
	gb->gb_serial_tx = NULL;
	gb->gb_serial_rx = NULL;

	gb->gb_bootrom_read = NULL;

	/* Check valid ROM using checksum value. */
	{
		uint8_t x = 0;
		uint16_t i;

		for(i = 0x0134; i <= 0x014C; i++)
			x = x - gb->gb_rom_read(gb, i) - 1;

		if(x != gb->gb_rom_read(gb, ROM_HEADER_CHECKSUM_LOC))
			return GB_INIT_INVALID_CHECKSUM;
	}

	/* Check if cartridge type is supported, and set MBC type. */
	{
#if WALNUT_FULL_GBC_SUPPORT
		gb->cgb.cgbMode = (gb_rom_read(gb, cgb_flag) & 0x80) >> 7;
#endif
		const uint8_t mbc_value = gb->gb_rom_read(gb, mbc_location);

		if(mbc_value > sizeof(cart_mbc) - 1 ||
				(gb->mbc = cart_mbc[mbc_value]) == -1)
			return GB_INIT_CARTRIDGE_UNSUPPORTED;
	}

	gb->num_rom_banks_mask = num_rom_banks_mask[gb_rom_read(gb, bank_count_location)] - 1;
	gb->cart_ram = cart_ram[gb_rom_read(gb, mbc_location)];
	gb->num_ram_banks = num_ram_banks[gb_rom_read(gb, ram_size_location)];

	/* If the ROM says that it support RAM, but has 0 RAM banks, then
	 * disable RAM reads from the cartridge. */
	if(gb->cart_ram == 0 || gb->num_ram_banks == 0)
	{
		gb->cart_ram = 0;
		gb->num_ram_banks = 0;
	}

	/* If MBC3 and number of ROM or RAM banks are larger than 128 or 8,
	 * respectively, then select MBC3O mode. */
	if(gb->mbc == 3)
		gb->cart_is_mbc3O = gb->num_rom_banks_mask > 128 || gb->num_ram_banks > 4;

	/* Note that MBC2 will appear to have no RAM banks, but it actually
	 * always has 512 half-bytes of RAM. Hence, gb->num_ram_banks must be
	 * ignored for MBC2. */

	gb->lcd_blank = false;
	gb->display.lcd_draw_line = NULL;

	gb_reset(gb);

	return GB_INIT_NO_ERROR;
}

const char* gb_get_rom_name(struct gb_s* gb, char *title_str)
{
	uint_fast16_t title_loc = 0x134;
	/* End of title may be 0x13E for newer games. */
	const uint_fast16_t title_end = 0x143;
	const char* title_start = title_str;

	for(; title_loc <= title_end; title_loc++)
	{
		const char title_char = gb->gb_rom_read(gb, title_loc);

		if(title_char >= ' ' && title_char <= '_')
		{
			*title_str = title_char;
			title_str++;
		}
		else
			break;
	}

	*title_str = '\0';
	return title_start;
}

#if ENABLE_LCD
void gb_init_lcd(struct gb_s *gb,
		void (*lcd_draw_line)(struct gb_s *gb,
			const uint8_t *pixels,
			const uint_fast8_t line))
{
	gb->display.lcd_draw_line = lcd_draw_line;

	gb->direct.interlace = false;
	gb->display.interlace_count = false;
	gb->direct.frame_skip = false;
	gb->display.frame_skip_count = false;

	gb->display.window_clear = 0;
	gb->display.WY = 0;

	return;
}
#endif

void gb_set_bootrom(struct gb_s *gb,
		 uint8_t (*gb_bootrom_read)(struct gb_s*, const uint_fast16_t))
{
	gb->gb_bootrom_read = gb_bootrom_read;
}

/**
 * Deprecated. Will be removed in the next major version.
 */
WGB_DEPRECATED("RTC is now ticked internally; this function has no effect")
void gb_tick_rtc(struct gb_s *gb)
{
	(void) gb;
	return;
}

void gb_set_rtc(struct gb_s *gb, const struct tm * const time)
{
	gb->rtc_real.bytes[0] = time->tm_sec;
	gb->rtc_real.bytes[1] = time->tm_min;
	gb->rtc_real.bytes[2] = time->tm_hour;
	gb->rtc_real.bytes[3] = time->tm_yday & 0xFF; /* Low 8 bits of day counter. */
	gb->rtc_real.bytes[4] = time->tm_yday >> 8; /* High 1 bit of day counter. */
}
#endif // WALNUT_GB_HEADER_ONLY

/** Function prototypes: Required functions **/
/**
 * Initialises the emulator context to a known state. Call this before calling
 * any other walnut-gb function.
 * To reset the emulator, you can call gb_reset() instead.
 *
 * \param gb	Allocated emulator context. Must not be NULL.
 * \param gb_rom_read Pointer to function that reads ROM data. ROM banking is
 * 		already handled by Walnut-GB. Must not be NULL.
 * \param gb_rom_read16 Pointer to function that reads ROM data in 16-bit chunks. ROM banking is
 *      already handled by Walnut-GB. Must not be NULL.
  * \param gb_rom_read32 pointer to function that reads ROM data in 32-bit chunks. ROM banking is
 *      already handled by Walnut-GB. Must not be NULL.
 * \param gb_cart_ram_read Pointer to function that reads Cart RAM. Must not be
 * 		NULL.
 * \param gb_cart_ram_write Pointer to function to writes to Cart RAM. Must not
 * 		be NULL.
 * \param gb_error Pointer to function that is called when an unrecoverable
 *		error occurs. Must not be NULL. Returning from this
 *		function is undefined and will result in SIGABRT.
 * \param priv	Private data that is stored within the emulator context. Set to
 * 		NULL if unused.
 * \returns	0 on success or an enum that describes the error.
 */
enum gb_init_error_e gb_init(struct gb_s* gb,
	uint8_t(*gb_rom_read)(struct gb_s*, const uint_fast32_t),
	uint16_t(*gb_rom_read16)(struct gb_s*, const uint_fast32_t),
	uint32_t(*gb_rom_read32)(struct gb_s*, const uint_fast32_t),
	uint8_t(*gb_cart_ram_read)(struct gb_s*, const uint_fast32_t),
	void (*gb_cart_ram_write)(struct gb_s*, const uint_fast32_t, const uint8_t),
	void (*gb_error)(struct gb_s*, const enum gb_error_e, const uint16_t),
	void* priv);
/**
 * Executes the emulator and runs for the duration of time equal to one frame.
 *
 * \param	An initialised emulator context. Must not be NULL.
 */
void gb_run_frame(struct gb_s *gb);

/**
 * Internal function used to step the CPU. Used mainly for testing.
 * Use gb_run_frame() instead.
 *
 * \param	An initialised emulator context. Must not be NULL.
 */
static inline void __gb_step_cpu(struct gb_s *gb);

/** Function prototypes: Optional Functions **/
/**
 * Reset the emulator, like turning the Game Boy off and on again.
 * This function can be called at any time.
 *
 * \param	An initialised emulator context. Must not be NULL.
 */
void gb_reset(struct gb_s *gb);

/**
 * Initialises the display context of the emulator. Only available when
 * ENABLE_LCD is defined to a non-zero value.
 * The pixel data sent to lcd_draw_line comes with both shade and layer data.
 * The first two least significant bits are the shade data (black, dark, light,
 * white). Bits 4 and 5 are layer data (OBJ0, OBJ1, BG), which can be used to
 * add more colours to the game in the same way that the Game Boy Color does to
 * older Game Boy games.
 * This function can be called at any time.
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \param lcd_draw_line Pointer to function that draws the 2-bit pixel data on the line
 *		"line". Must not be NULL.
 */
#if ENABLE_LCD
void gb_init_lcd(struct gb_s *gb,
		void (*lcd_draw_line)(struct gb_s *gb,
			const uint8_t *pixels,
			const uint_fast8_t line));
#endif

/**
 * Initialises the serial connection of the emulator. This function is optional,
 * and if not called, the emulator will assume that no link cable is connected
 * to the game.
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \param gb_serial_tx Pointer to function that transmits a byte of data over
 *		the serial connection. Must not be NULL.
 * \param gb_serial_rx Pointer to function that receives a byte of data over the
 *		serial connection. If no byte is received,
 *		return GB_SERIAL_RX_NO_CONNECTION. Must not be NULL.
 */
void gb_init_serial(struct gb_s *gb,
		    void (*gb_serial_tx)(struct gb_s*, const uint8_t),
		    enum gb_serial_rx_ret_e (*gb_serial_rx)(struct gb_s*,
			    uint8_t*));

/**
 * Obtains the save size of the game (size of the Cart RAM). Required by the
 * frontend to allocate enough memory for the Cart RAM.
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \param ram_size Pointer to size_t variable that will be set to the size of
 *		the Cart RAM in bytes. Must not be NULL.
 *		If the Cart RAM is not battery backed, this will be set to 0.
 *		If the Cart RAM size is invalid or unknown, this will not be
 *		set.
 * \returns	0 on success, or -1 if the RAM size is invalid or unknown.
 */
int gb_get_save_size_s(struct gb_s *gb, size_t *ram_size);

/**
 * Deprecated. Use gb_get_save_size_s() instead.
 * Obtains the save size of the game (size of the Cart RAM). Required by the
 * frontend to allocate enough memory for the Cart RAM.
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \returns	Size of the Cart RAM in bytes. 0 if Cartridge has not battery
 *		backed RAM.
 *		0 is also returned on invalid or unknown RAM size.
 */
uint_fast32_t gb_get_save_size(struct gb_s *gb);

/**
 * Calculates and returns a hash of the game header in the same way the Game
 * Boy Color does for colourising old Game Boy games. The frontend can use this
 * hash to automatically set a colour palette.
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \returns	Hash of the game header.
 */
uint8_t gb_colour_hash(struct gb_s *gb);

/**
 * Returns the title of ROM.
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \param title_str Allocated string at least 16 characters.
 * \returns	Pointer to start of string, null terminated.
 */
const char* gb_get_rom_name(struct gb_s* gb, char *title_str);

/**
 * Deprecated. Will be removed in the next major version.
 * RTC is ticked internally and this function has no effect.
 */
void gb_tick_rtc(struct gb_s *gb);

/**
 * Set initial values in RTC.
 * Should be called after gb_init().
 *
 * \param gb	An initialised emulator context. Must not be NULL.
 * \param time	Time structure with date and time.
 */
void gb_set_rtc(struct gb_s *gb, const struct tm * const time);

/**
 * Use boot ROM on reset. gb_reset() must be called for this to take affect.
 * \param gb 	An initialised emulator context. Must not be NULL.
 * \param gb_bootrom_read Function pointer to read boot ROM binary.
 */
void gb_set_bootrom(struct gb_s *gb,
	uint8_t (*gb_bootrom_read)(struct gb_s*, const uint_fast16_t));

/* Steps the cpu by one instruction, this is the original Peanut-GB dispatch method here for compatibility, or for burning cycles inefficiently if doing preemptive execution */
void __gb_step_cpu_x(struct gb_s *gb)
{
	uint8_t opcode;
	uint_fast16_t inst_cycles;
	static const uint8_t op_cycles[0x100] =
	{
		/* *INDENT-OFF* */
		/*0 1 2  3  4  5  6  7  8  9  A  B  C  D  E  F	*/
		4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,	/* 0x00 */
		4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,	/* 0x10 */
		8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,	/* 0x20 */
		8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,	/* 0x30 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x40 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x50 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x60 */
		8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4, /* 0x70 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x80 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0x90 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0xA0 */
		4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,	/* 0xB0 */
		8,12,12,16,12,16, 8,16, 8,16,12, 8,12,24, 8,16,	/* 0xC0 */
		8,12,12, 0,12,16, 8,16, 8,16,12, 0,12, 0, 8,16,	/* 0xD0 */
		12,12,8, 0, 0,16, 8,16,16, 4,16, 0, 0, 0, 8,16,	/* 0xE0 */
		12,12,8, 4, 0,16, 8,16,12, 8,16, 4, 0, 0, 8,16	/* 0xF0 */
		/* *INDENT-ON* */
	};
	static const uint_fast16_t TAC_CYCLES[4] = {1024, 16, 64, 256};

	/* Handle interrupts */
	/* If gb_halt is positive, then an interrupt must have occurred by the
	 * time we reach here, because on HALT, we jump to the next interrupt
	 * immediately. */
	while(gb->gb_halt || (gb->gb_ime &&
			gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & ANY_INTR))
	{
		gb->gb_halt = false;

		if(!gb->gb_ime)
			break;

		/* Disable interrupts */
		gb->gb_ime = false;

		/* Push Program Counter */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);

		/* Call interrupt handler if required. */
		if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & VBLANK_INTR)
		{
			gb->cpu_reg.pc.reg = VBLANK_INTR_ADDR;
			gb->hram_io[IO_IF] ^= VBLANK_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & LCDC_INTR)
		{
			gb->cpu_reg.pc.reg = LCDC_INTR_ADDR;
			gb->hram_io[IO_IF] ^= LCDC_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & TIMER_INTR)
		{
			gb->cpu_reg.pc.reg = TIMER_INTR_ADDR;
			gb->hram_io[IO_IF] ^= TIMER_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & SERIAL_INTR)
		{
			gb->cpu_reg.pc.reg = SERIAL_INTR_ADDR;
			gb->hram_io[IO_IF] ^= SERIAL_INTR;
		}
		else if(gb->hram_io[IO_IF] & gb->hram_io[IO_IE] & CONTROL_INTR)
		{
			gb->cpu_reg.pc.reg = CONTROL_INTR_ADDR;
			gb->hram_io[IO_IF] ^= CONTROL_INTR;
		}

		break;
	}

	/* Obtain opcode */
	opcode = __gb_read(gb, gb->cpu_reg.pc.reg++);
	inst_cycles = op_cycles[opcode];

	/* Execute opcode */
	switch(opcode)
	{
	case 0x00: /* NOP */
		break;

	case 0x01: /* LD BC, imm */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.bc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;
	case 0x02: /* LD (BC), A */
		__gb_write(gb, gb->cpu_reg.bc.reg, gb->cpu_reg.a);
		break;

	case 0x03: /* INC BC */
		gb->cpu_reg.bc.reg++;
		break;

	case 0x04: /* INC B */
		WGB_INSTR_INC_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0x05: /* DEC B */
		WGB_INSTR_DEC_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0x06: /* LD B, imm */
		gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x07: /* RLCA */
		gb->cpu_reg.a = (gb->cpu_reg.a << 1) | (gb->cpu_reg.a >> 7);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = (gb->cpu_reg.a & 0x01);
		break;

	case 0x08: /* LD (imm), SP */
	{		
#if WALNUT_GB_16_BIT_DISABLED
    uint16_t temp = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    uint8_t l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t temp = WALNUT_GB_U8_TO_U16(h, l);
#endif
    __gb_write(gb, temp++, gb->cpu_reg.sp.bytes.p);
    __gb_write(gb, temp, gb->cpu_reg.sp.bytes.s);
    break;
	}

	case 0x09: /* ADD HL, BC */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.bc.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			(temp ^ gb->cpu_reg.hl.reg ^ gb->cpu_reg.bc.reg) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFFFF0000) ? 1 : 0;
		gb->cpu_reg.hl.reg = (temp & 0x0000FFFF);
		break;
	}

	case 0x0A: /* LD A, (BC) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.bc.reg);
		break;

	case 0x0B: /* DEC BC */
		gb->cpu_reg.bc.reg--;
		break;

	case 0x0C: /* INC C */
		WGB_INSTR_INC_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0x0D: /* DEC C */
		WGB_INSTR_DEC_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0x0E: /* LD C, imm */
		gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x0F: /* RRCA */
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = gb->cpu_reg.a & 0x01;
		gb->cpu_reg.a = (gb->cpu_reg.a >> 1) | (gb->cpu_reg.a << 7);
		break;

	case 0x10: /* STOP */
		//gb->gb_halt = true;
#if WALNUT_FULL_GBC_SUPPORT
		if(gb->cgb.cgbMode & gb->cgb.doubleSpeedPrep)
		{
			gb->cgb.doubleSpeedPrep = 0;
			gb->cgb.doubleSpeed ^= 1;
		}
#endif
		break;

	case 0x11: /* LD DE, imm */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.de.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;

	case 0x12: /* LD (DE), A */
		__gb_write(gb, gb->cpu_reg.de.reg, gb->cpu_reg.a);
		break;

	case 0x13: /* INC DE */
		gb->cpu_reg.de.reg++;
		break;

	case 0x14: /* INC D */
		WGB_INSTR_INC_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0x15: /* DEC D */
		WGB_INSTR_DEC_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0x16: /* LD D, imm */
		gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x17: /* RLA */
	{
		uint8_t temp = gb->cpu_reg.a;
		gb->cpu_reg.a = (gb->cpu_reg.a << 1) | gb->cpu_reg.f.f_bits.c;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = (temp >> 7) & 0x01;
		break;
	}

	case 0x18: /* JR imm */
	{
		int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.pc.reg += temp;
		break;
	}

	case 0x19: /* ADD HL, DE */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.de.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			(temp ^ gb->cpu_reg.hl.reg ^ gb->cpu_reg.de.reg) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFFFF0000) ? 1 : 0;
		gb->cpu_reg.hl.reg = (temp & 0x0000FFFF);
		break;
	}

	case 0x1A: /* LD A, (DE) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.de.reg);
		break;

	case 0x1B: /* DEC DE */
		gb->cpu_reg.de.reg--;
		break;

	case 0x1C: /* INC E */
		WGB_INSTR_INC_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0x1D: /* DEC E */
		WGB_INSTR_DEC_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0x1E: /* LD E, imm */
		gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x1F: /* RRA */
	{
		uint8_t temp = gb->cpu_reg.a;
		gb->cpu_reg.a = gb->cpu_reg.a >> 1 | (gb->cpu_reg.f.f_bits.c << 7);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.c = temp & 0x1;
		break;
	}

	case 0x20: /* JR NZ, imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x21: /* LD HL, imm */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.hl.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;

	case 0x22: /* LDI (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		gb->cpu_reg.hl.reg++;
		break;

	case 0x23: /* INC HL */
		gb->cpu_reg.hl.reg++;
		break;

	case 0x24: /* INC H */
		WGB_INSTR_INC_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0x25: /* DEC H */
		WGB_INSTR_DEC_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0x26: /* LD H, imm */
		gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x27: /* DAA */
	{
		/* The following is from SameBoy. MIT License. */
		int16_t a = gb->cpu_reg.a;

		if(gb->cpu_reg.f.f_bits.n)
		{
			if(gb->cpu_reg.f.f_bits.h)
				a = (a - 0x06) & 0xFF;

			if(gb->cpu_reg.f.f_bits.c)
				a -= 0x60;
		}
		else
		{
			if(gb->cpu_reg.f.f_bits.h || (a & 0x0F) > 9)
				a += 0x06;

			if(gb->cpu_reg.f.f_bits.c || a > 0x9F)
				a += 0x60;
		}

		if((a & 0x100) == 0x100)
			gb->cpu_reg.f.f_bits.c = 1;

		gb->cpu_reg.a = (uint8_t)a;
		gb->cpu_reg.f.f_bits.z = (gb->cpu_reg.a == 0);
		gb->cpu_reg.f.f_bits.h = 0;

		break;
	}

	case 0x28: /* JR Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x29: /* ADD HL, HL */
	{
		gb->cpu_reg.f.f_bits.c = (gb->cpu_reg.hl.reg & 0x8000) > 0;
		gb->cpu_reg.hl.reg <<= 1;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = (gb->cpu_reg.hl.reg & 0x1000) > 0;
		break;
	}

	case 0x2A: /* LD A, (HL+) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg++);
		break;

	case 0x2B: /* DEC HL */
		gb->cpu_reg.hl.reg--;
		break;

	case 0x2C: /* INC L */
		WGB_INSTR_INC_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0x2D: /* DEC L */
		WGB_INSTR_DEC_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0x2E: /* LD L, imm */
		gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x2F: /* CPL */
		gb->cpu_reg.a = ~gb->cpu_reg.a;
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h = 1;
		break;

	case 0x30: /* JR NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x31: /* LD SP, imm */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.sp.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2;
#else
    gb->cpu_reg.sp.bytes.p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.sp.bytes.s = __gb_read(gb, gb->cpu_reg.pc.reg++);
#endif
		break;

	case 0x32: /* LD (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		gb->cpu_reg.hl.reg--;
		break;

	case 0x33: /* INC SP */
		gb->cpu_reg.sp.reg++;
		break;

	case 0x34: /* INC (HL) */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.hl.reg);
		WGB_INSTR_INC_R8(temp);
		__gb_write(gb, gb->cpu_reg.hl.reg, temp);
		break;
	}

	case 0x35: /* DEC (HL) */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.hl.reg);
		WGB_INSTR_DEC_R8(temp);
		__gb_write(gb, gb->cpu_reg.hl.reg, temp);
		break;
	}

	case 0x36: /* LD (HL), imm */
		__gb_write(gb, gb->cpu_reg.hl.reg, __gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0x37: /* SCF */
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = 0;
		gb->cpu_reg.f.f_bits.c = 1;
		break;

	case 0x38: /* JR C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
			int8_t temp = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
			gb->cpu_reg.pc.reg += temp;
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg++;

		break;

	case 0x39: /* ADD HL, SP */
	{
		uint_fast32_t temp = gb->cpu_reg.hl.reg + gb->cpu_reg.sp.reg;
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h =
			((gb->cpu_reg.hl.reg & 0xFFF) + (gb->cpu_reg.sp.reg & 0xFFF)) & 0x1000 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = temp & 0x10000 ? 1 : 0;
		gb->cpu_reg.hl.reg = (uint16_t)temp;
		break;
	}

	case 0x3A: /* LD A, (HL) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg--);
		break;

	case 0x3B: /* DEC SP */
		gb->cpu_reg.sp.reg--;
		break;

	case 0x3C: /* INC A */
		WGB_INSTR_INC_R8(gb->cpu_reg.a);
		break;

	case 0x3D: /* DEC A */
		WGB_INSTR_DEC_R8(gb->cpu_reg.a);
		break;

	case 0x3E: /* LD A, imm */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.pc.reg++);
		break;

	case 0x3F: /* CCF */
		gb->cpu_reg.f.f_bits.n = 0;
		gb->cpu_reg.f.f_bits.h = 0;
		gb->cpu_reg.f.f_bits.c = ~gb->cpu_reg.f.f_bits.c;
		break;

	case 0x40: /* LD B, B */
		break;

	case 0x41: /* LD B, C */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x42: /* LD B, D */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.de.bytes.d;
		break;

	case 0x43: /* LD B, E */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.de.bytes.e;
		break;

	case 0x44: /* LD B, H */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x45: /* LD B, L */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x46: /* LD B, (HL) */
		gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x47: /* LD B, A */
		gb->cpu_reg.bc.bytes.b = gb->cpu_reg.a;
		break;

	case 0x48: /* LD C, B */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x49: /* LD C, C */
		break;

	case 0x4A: /* LD C, D */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.de.bytes.d;
		break;

	case 0x4B: /* LD C, E */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.de.bytes.e;
		break;

	case 0x4C: /* LD C, H */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x4D: /* LD C, L */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x4E: /* LD C, (HL) */
		gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x4F: /* LD C, A */
		gb->cpu_reg.bc.bytes.c = gb->cpu_reg.a;
		break;

	case 0x50: /* LD D, B */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x51: /* LD D, C */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x52: /* LD D, D */
		break;

	case 0x53: /* LD D, E */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.de.bytes.e;
		break;

	case 0x54: /* LD D, H */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x55: /* LD D, L */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x56: /* LD D, (HL) */
		gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x57: /* LD D, A */
		gb->cpu_reg.de.bytes.d = gb->cpu_reg.a;
		break;

	case 0x58: /* LD E, B */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x59: /* LD E, C */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x5A: /* LD E, D */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.de.bytes.d;
		break;

	case 0x5B: /* LD E, E */
		break;

	case 0x5C: /* LD E, H */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x5D: /* LD E, L */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x5E: /* LD E, (HL) */
		gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x5F: /* LD E, A */
		gb->cpu_reg.de.bytes.e = gb->cpu_reg.a;
		break;

	case 0x60: /* LD H, B */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x61: /* LD H, C */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x62: /* LD H, D */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.de.bytes.d;
		break;

	case 0x63: /* LD H, E */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.de.bytes.e;
		break;

	case 0x64: /* LD H, H */
		break;

	case 0x65: /* LD H, L */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x66: /* LD H, (HL) */
		gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x67: /* LD H, A */
		gb->cpu_reg.hl.bytes.h = gb->cpu_reg.a;
		break;

	case 0x68: /* LD L, B */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x69: /* LD L, C */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x6A: /* LD L, D */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.de.bytes.d;
		break;

	case 0x6B: /* LD L, E */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.de.bytes.e;
		break;

	case 0x6C: /* LD L, H */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x6D: /* LD L, L */
		break;

	case 0x6E: /* LD L, (HL) */
		gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x6F: /* LD L, A */
		gb->cpu_reg.hl.bytes.l = gb->cpu_reg.a;
		break;

	case 0x70: /* LD (HL), B */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.bc.bytes.b);
		break;

	case 0x71: /* LD (HL), C */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.bc.bytes.c);
		break;

	case 0x72: /* LD (HL), D */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.de.bytes.d);
		break;

	case 0x73: /* LD (HL), E */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.de.bytes.e);
		break;

	case 0x74: /* LD (HL), H */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.hl.bytes.h);
		break;

	case 0x75: /* LD (HL), L */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.hl.bytes.l);
		break;

	case 0x76: /* HALT */
	{
		int_fast16_t halt_cycles = INT_FAST16_MAX;

		/* TODO: Emulate HALT bug? */
		gb->gb_halt = true;

		if(gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
		{
			int serial_cycles = SERIAL_CYCLES -
				gb->counter.serial_count;

			if(serial_cycles < halt_cycles)
				halt_cycles = serial_cycles;
		}

		if(gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK)
		{
			int tac_cycles = TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK] -
				gb->counter.tima_count;

			if(tac_cycles < halt_cycles)
				halt_cycles = tac_cycles;
		}

		if((gb->hram_io[IO_LCDC] & LCDC_ENABLE))
		{
			int lcd_cycles;

			/* If LCD is in HBlank, calculate the number of cycles
			 * until the end of HBlank and the start of mode 2 or
			 * mode 1. */
			if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_HBLANK)
			{
				lcd_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
			}
			else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_OAM_SCAN)
			{
				lcd_cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - gb->counter.lcd_count;
			}
			else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_LCD_DRAW)
			{
				lcd_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
			}
			else
			{
				/* VBlank */
				lcd_cycles = LCD_LINE_CYCLES - gb->counter.lcd_count;
			}

			if(lcd_cycles < halt_cycles)
				halt_cycles = lcd_cycles;
		}

		/* Some halt cycles may already be very high, so make sure we
		 * don't underflow here. */
		if(halt_cycles <= 0)
			halt_cycles = 4;

		inst_cycles = (uint_fast16_t)halt_cycles;
		break;
	}

	case 0x77: /* LD (HL), A */
		__gb_write(gb, gb->cpu_reg.hl.reg, gb->cpu_reg.a);
		break;

	case 0x78: /* LD A, B */
		gb->cpu_reg.a = gb->cpu_reg.bc.bytes.b;
		break;

	case 0x79: /* LD A, C */
		gb->cpu_reg.a = gb->cpu_reg.bc.bytes.c;
		break;

	case 0x7A: /* LD A, D */
		gb->cpu_reg.a = gb->cpu_reg.de.bytes.d;
		break;

	case 0x7B: /* LD A, E */
		gb->cpu_reg.a = gb->cpu_reg.de.bytes.e;
		break;

	case 0x7C: /* LD A, H */
		gb->cpu_reg.a = gb->cpu_reg.hl.bytes.h;
		break;

	case 0x7D: /* LD A, L */
		gb->cpu_reg.a = gb->cpu_reg.hl.bytes.l;
		break;

	case 0x7E: /* LD A, (HL) */
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.hl.reg);
		break;

	case 0x7F: /* LD A, A */
		break;

	case 0x80: /* ADD A, B */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.b, 0);
		break;

	case 0x81: /* ADD A, C */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.c, 0);
		break;

	case 0x82: /* ADD A, D */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.d, 0);
		break;

	case 0x83: /* ADD A, E */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.e, 0);
		break;

	case 0x84: /* ADD A, H */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.h, 0);
		break;

	case 0x85: /* ADD A, L */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.l, 0);
		break;

	case 0x86: /* ADD A, (HL) */
		WGB_INSTR_ADC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), 0);
		break;

	case 0x87: /* ADD A, A */
		WGB_INSTR_ADC_R8(gb->cpu_reg.a, 0);
		break;

	case 0x88: /* ADC A, B */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.b, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x89: /* ADC A, C */
		WGB_INSTR_ADC_R8(gb->cpu_reg.bc.bytes.c, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8A: /* ADC A, D */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.d, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8B: /* ADC A, E */
		WGB_INSTR_ADC_R8(gb->cpu_reg.de.bytes.e, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8C: /* ADC A, H */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.h, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8D: /* ADC A, L */
		WGB_INSTR_ADC_R8(gb->cpu_reg.hl.bytes.l, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8E: /* ADC A, (HL) */
		WGB_INSTR_ADC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), gb->cpu_reg.f.f_bits.c);
		break;

	case 0x8F: /* ADC A, A */
		WGB_INSTR_ADC_R8(gb->cpu_reg.a, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x90: /* SUB B */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.b, 0);
		break;

	case 0x91: /* SUB C */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.c, 0);
		break;

	case 0x92: /* SUB D */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.d, 0);
		break;

	case 0x93: /* SUB E */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.e, 0);
		break;

	case 0x94: /* SUB H */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.h, 0);
		break;

	case 0x95: /* SUB L */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.l, 0);
		break;

	case 0x96: /* SUB (HL) */
		WGB_INSTR_SBC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), 0);
		break;

	case 0x97: /* SUB A */
		gb->cpu_reg.a = 0;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 1;
		break;

	case 0x98: /* SBC A, B */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.b, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x99: /* SBC A, C */
		WGB_INSTR_SBC_R8(gb->cpu_reg.bc.bytes.c, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9A: /* SBC A, D */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.d, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9B: /* SBC A, E */
		WGB_INSTR_SBC_R8(gb->cpu_reg.de.bytes.e, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9C: /* SBC A, H */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.h, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9D: /* SBC A, L */
		WGB_INSTR_SBC_R8(gb->cpu_reg.hl.bytes.l, gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9E: /* SBC A, (HL) */
		WGB_INSTR_SBC_R8(__gb_read(gb, gb->cpu_reg.hl.reg), gb->cpu_reg.f.f_bits.c);
		break;

	case 0x9F: /* SBC A, A */
		gb->cpu_reg.a = gb->cpu_reg.f.f_bits.c ? 0xFF : 0x00;
		gb->cpu_reg.f.f_bits.z = !gb->cpu_reg.f.f_bits.c;
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h = gb->cpu_reg.f.f_bits.c;
		break;

	case 0xA0: /* AND B */
		WGB_INSTR_AND_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xA1: /* AND C */
		WGB_INSTR_AND_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xA2: /* AND D */
		WGB_INSTR_AND_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xA3: /* AND E */
		WGB_INSTR_AND_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xA4: /* AND H */
		WGB_INSTR_AND_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xA5: /* AND L */
		WGB_INSTR_AND_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xA6: /* AND (HL) */
		WGB_INSTR_AND_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xA7: /* AND A */
		WGB_INSTR_AND_R8(gb->cpu_reg.a);
		break;

	case 0xA8: /* XOR B */
		WGB_INSTR_XOR_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xA9: /* XOR C */
		WGB_INSTR_XOR_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xAA: /* XOR D */
		WGB_INSTR_XOR_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xAB: /* XOR E */
		WGB_INSTR_XOR_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xAC: /* XOR H */
		WGB_INSTR_XOR_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xAD: /* XOR L */
		WGB_INSTR_XOR_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xAE: /* XOR (HL) */
		WGB_INSTR_XOR_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xAF: /* XOR A */
		WGB_INSTR_XOR_R8(gb->cpu_reg.a);
		break;

	case 0xB0: /* OR B */
		WGB_INSTR_OR_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xB1: /* OR C */
		WGB_INSTR_OR_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xB2: /* OR D */
		WGB_INSTR_OR_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xB3: /* OR E */
		WGB_INSTR_OR_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xB4: /* OR H */
		WGB_INSTR_OR_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xB5: /* OR L */
		WGB_INSTR_OR_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xB6: /* OR (HL) */
		WGB_INSTR_OR_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xB7: /* OR A */
		WGB_INSTR_OR_R8(gb->cpu_reg.a);
		break;

	case 0xB8: /* CP B */
		WGB_INSTR_CP_R8(gb->cpu_reg.bc.bytes.b);
		break;

	case 0xB9: /* CP C */
		WGB_INSTR_CP_R8(gb->cpu_reg.bc.bytes.c);
		break;

	case 0xBA: /* CP D */
		WGB_INSTR_CP_R8(gb->cpu_reg.de.bytes.d);
		break;

	case 0xBB: /* CP E */
		WGB_INSTR_CP_R8(gb->cpu_reg.de.bytes.e);
		break;

	case 0xBC: /* CP H */
		WGB_INSTR_CP_R8(gb->cpu_reg.hl.bytes.h);
		break;

	case 0xBD: /* CP L */
		WGB_INSTR_CP_R8(gb->cpu_reg.hl.bytes.l);
		break;

	case 0xBE: /* CP (HL) */
		WGB_INSTR_CP_R8(__gb_read(gb, gb->cpu_reg.hl.reg));
		break;

	case 0xBF: /* CP A */
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.z = 1;
		gb->cpu_reg.f.f_bits.n = 1;
		break;

	case 0xC0: /* RET NZ */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2;
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
        inst_cycles += 12;
		}

		break;

	case 0xC1: /* POP BC */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.bc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.bc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.bc.bytes.b = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;

	case 0xC2: /* JP NZ, imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xC3: /* JP imm */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
#else
{
    uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    gb->cpu_reg.pc.bytes.c = c;
    gb->cpu_reg.pc.bytes.p = p;
}
#endif
		break;

	case 0xC4: /* CALL NZ imm */
		if(!gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_DISABLED
			uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
			gb->cpu_reg.pc.reg += 2;

			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
			gb->cpu_reg.pc.reg = addr;
#else
			uint8_t c, p;
			c = __gb_read(gb, gb->cpu_reg.pc.reg++);
			p = __gb_read(gb, gb->cpu_reg.pc.reg++);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
			__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
			gb->cpu_reg.pc.bytes.c = c;
			gb->cpu_reg.pc.bytes.p = p;
#endif        
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xC5: /* PUSH BC */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.bc.bytes.b);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.bc.bytes.c);
		break;

	case 0xC6: /* ADD A, imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_ADC_R8(val, 0);
		break;
	}

	case 0xC7: /* RST 0x0000 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0000;
		break;

	case 0xC8: /* RET Z */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_DISABLED
        // Optional 16-bit path if stack is aligned
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2;
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
			inst_cycles += 12;
		}
		break;

	case 0xC9: /* RET */
	{
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;
	}

	case 0xCA: /* JP Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2;  // advance past the immediate word
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xCB: /* CB INST */
		inst_cycles = __gb_execute_cb(gb);
		break;

	case 0xCC: /* CALL Z, imm */
		if(gb->cpu_reg.f.f_bits.z)
		{
#if WALNUT_GB_16_BIT_DISABLED
        uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance past immediate

        // push old PC
        __gb_write(gb, --gb->cpu_reg.sp.reg, (gb->cpu_reg.pc.reg >> 8) & 0xFF);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg & 0xFF);

        gb->cpu_reg.pc.reg = addr;
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xCD: /* CALL imm */
#if WALNUT_GB_16_BIT_DISABLED
{
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2; // advance past immediate

    // push old PC
    __gb_write(gb, --gb->cpu_reg.sp.reg, (gb->cpu_reg.pc.reg >> 8) & 0xFF);
    __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg & 0xFF);

    gb->cpu_reg.pc.reg = addr;
}
#else
{
    uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
    __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
    __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
    gb->cpu_reg.pc.bytes.c = c;
    gb->cpu_reg.pc.bytes.p = p;
}
#endif
	break;

	case 0xCE: /* ADC A, imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_ADC_R8(val, gb->cpu_reg.f.f_bits.c);
		break;
	}

	case 0xCF: /* RST 0x0008 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0008;
		break;

	case 0xD0: /* RET NC */

    if (!gb->cpu_reg.f.f_bits.c)
    {
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
        inst_cycles += 12;
    }

		break;

	case 0xD1: /* POP DE */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.de.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2;
#else
    gb->cpu_reg.de.bytes.e = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.de.bytes.d = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;

	case 0xD2: /* JP NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past immediate
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xD4: /* CALL NC, imm */
		if(!gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_DISABLED
        uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past immediate
        __gb_write(gb, --gb->cpu_reg.sp.reg, (addr >> 8) & 0xFF);
        __gb_write(gb, --gb->cpu_reg.sp.reg, addr & 0xFF);
        gb->cpu_reg.pc.reg = addr;
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xD5: /* PUSH DE */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.de.bytes.d);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.de.bytes.e);
		break;

	case 0xD6: /* SUB imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		uint16_t temp = gb->cpu_reg.a - val;
		gb->cpu_reg.f.f_bits.z = ((temp & 0xFF) == 0x00);
		gb->cpu_reg.f.f_bits.n = 1;
		gb->cpu_reg.f.f_bits.h =
			(gb->cpu_reg.a ^ val ^ temp) & 0x10 ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = (temp & 0xFF00) ? 1 : 0;
		gb->cpu_reg.a = (temp & 0xFF);
		break;
	}

	case 0xD7: /* RST 0x0010 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0010;
		break;

	case 0xD8: /* RET C */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
        gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
        gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
        gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
			inst_cycles += 12;
		}

		break;

	case 0xD9: /* RETI */
	{
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2; // advance SP past the popped PC
#else
    gb->cpu_reg.pc.bytes.c = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.pc.bytes.p = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		gb->gb_ime = true;
	}
	break;

	case 0xDA: /* JP C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_DISABLED
        gb->cpu_reg.pc.reg = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past the immediate word
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 4;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xDC: /* CALL C, imm */
		if(gb->cpu_reg.f.f_bits.c)
		{
#if WALNUT_GB_16_BIT_DISABLED
        uint16_t target = __gb_read16(gb, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg += 2; // advance PC past the immediate word
        // push current PC onto stack
        gb->cpu_reg.sp.reg -= 2;
        __gb_write16(gb, gb->cpu_reg.sp.reg, gb->cpu_reg.pc.reg);
        gb->cpu_reg.pc.reg = target;
#else
        uint8_t c = __gb_read(gb, gb->cpu_reg.pc.reg++);
        uint8_t p = __gb_read(gb, gb->cpu_reg.pc.reg++);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
        __gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
        gb->cpu_reg.pc.bytes.c = c;
        gb->cpu_reg.pc.bytes.p = p;
#endif
			inst_cycles += 12;
		}
		else
			gb->cpu_reg.pc.reg += 2;

		break;

	case 0xDE: /* SBC A, imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_SBC_R8(val, gb->cpu_reg.f.f_bits.c);
		break;
	}

	case 0xDF: /* RST 0x0018 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0018;
		break;

	case 0xE0: /* LD (0xFF00+imm), A */
		__gb_write(gb, 0xFF00 | __gb_read(gb, gb->cpu_reg.pc.reg++),
			   gb->cpu_reg.a);
		break;

	case 0xE1: /* POP HL */
#if WALNUT_GB_16_BIT_DISABLED
    gb->cpu_reg.hl.reg = __gb_read16(gb, gb->cpu_reg.sp.reg);
    gb->cpu_reg.sp.reg += 2; // advance SP past the popped value
#else
    gb->cpu_reg.hl.bytes.l = __gb_read(gb, gb->cpu_reg.sp.reg++);
    gb->cpu_reg.hl.bytes.h = __gb_read(gb, gb->cpu_reg.sp.reg++);
#endif
		break;

	case 0xE2: /* LD (C), A */
		__gb_write(gb, 0xFF00 | gb->cpu_reg.bc.bytes.c, gb->cpu_reg.a);
		break;

	case 0xE5: /* PUSH HL */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.hl.bytes.h);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.hl.bytes.l);
		break;

	case 0xE6: /* AND imm */
	{
		uint8_t temp = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_AND_R8(temp);
		break;
	}

	case 0xE7: /* RST 0x0020 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0020;
		break;

	case 0xE8: /* ADD SP, imm */
	{
		int8_t offset = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.h = ((gb->cpu_reg.sp.reg & 0xF) + (offset & 0xF) > 0xF) ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = ((gb->cpu_reg.sp.reg & 0xFF) + (offset & 0xFF) > 0xFF);
		gb->cpu_reg.sp.reg += offset;
		break;
	}

	case 0xE9: /* JP (HL) */
		gb->cpu_reg.pc.reg = gb->cpu_reg.hl.reg;
		break;

	case 0xEA: /* LD (imm), A */
	{
#if WALNUT_GB_16_BIT_DISABLED
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2; // advance past the immediate
#else
    uint8_t l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
#endif
		__gb_write(gb, addr, gb->cpu_reg.a);
		break;
	}

	case 0xEE: /* XOR imm */
		WGB_INSTR_XOR_R8(__gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0xEF: /* RST 0x0028 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0028;
		break;

	case 0xF0: /* LD A, (0xFF00+imm) */
		gb->cpu_reg.a =
			__gb_read(gb, 0xFF00 | __gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0xF1: /* POP AF */
	{
		uint8_t temp_8 = __gb_read(gb, gb->cpu_reg.sp.reg++);
		gb->cpu_reg.f.f_bits.z = (temp_8 >> 7) & 1;
		gb->cpu_reg.f.f_bits.n = (temp_8 >> 6) & 1;
		gb->cpu_reg.f.f_bits.h = (temp_8 >> 5) & 1;
		gb->cpu_reg.f.f_bits.c = (temp_8 >> 4) & 1;
		gb->cpu_reg.a = __gb_read(gb, gb->cpu_reg.sp.reg++);
		break;
	}

	case 0xF2: /* LD A, (C) */
		gb->cpu_reg.a = __gb_read(gb, 0xFF00 | gb->cpu_reg.bc.bytes.c);
		break;

	case 0xF3: /* DI */
		gb->gb_ime = false;
		break;

	case 0xF5: /* PUSH AF */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.a);
		__gb_write(gb, --gb->cpu_reg.sp.reg,
			   gb->cpu_reg.f.f_bits.z << 7 | gb->cpu_reg.f.f_bits.n << 6 |
			   gb->cpu_reg.f.f_bits.h << 5 | gb->cpu_reg.f.f_bits.c << 4);
		break;

	case 0xF6: /* OR imm */
		WGB_INSTR_OR_R8(__gb_read(gb, gb->cpu_reg.pc.reg++));
		break;

	case 0xF7: /* PUSH AF */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0030;
		break;

	case 0xF8: /* LD HL, SP+/-imm */
	{
		/* Taken from SameBoy, which is released under MIT Licence. */
		int8_t offset = (int8_t) __gb_read(gb, gb->cpu_reg.pc.reg++);
		gb->cpu_reg.hl.reg = gb->cpu_reg.sp.reg + offset;
		gb->cpu_reg.f.reg = 0;
		gb->cpu_reg.f.f_bits.h = ((gb->cpu_reg.sp.reg & 0xF) + (offset & 0xF) > 0xF) ? 1 : 0;
		gb->cpu_reg.f.f_bits.c = ((gb->cpu_reg.sp.reg & 0xFF) + (offset & 0xFF) > 0xFF) ? 1 : 0;
		break;
	}

	case 0xF9: /* LD SP, HL */
		gb->cpu_reg.sp.reg = gb->cpu_reg.hl.reg;
		break;

	case 0xFA: /* LD A, (imm) */
	{
#if WALNUT_GB_16_BIT_DISABLED
    uint16_t addr = __gb_read16(gb, gb->cpu_reg.pc.reg);
    gb->cpu_reg.pc.reg += 2; // advance past the immediate
#else
    uint8_t l = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint8_t h = __gb_read(gb, gb->cpu_reg.pc.reg++);
    uint16_t addr = WALNUT_GB_U8_TO_U16(h, l);
#endif
		gb->cpu_reg.a = __gb_read(gb, addr);
		break;
	}

	case 0xFB: /* EI */
		gb->gb_ime = true;
		break;

	case 0xFE: /* CP imm */
	{
		uint8_t val = __gb_read(gb, gb->cpu_reg.pc.reg++);
		WGB_INSTR_CP_R8(val);
		break;
	}

	case 0xFF: /* RST 0x0038 */
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.p);
		__gb_write(gb, --gb->cpu_reg.sp.reg, gb->cpu_reg.pc.bytes.c);
		gb->cpu_reg.pc.reg = 0x0038;
		break;

	default:
		/* Return address where invalid opcode that was read. */
		(gb->gb_error)(gb, GB_INVALID_OPCODE, gb->cpu_reg.pc.reg - 1);
		WGB_UNREACHABLE();
	}

	do
	{
		/* DIV register timing */
		gb->counter.div_count += inst_cycles;
		while(gb->counter.div_count >= DIV_CYCLES)
		{
			gb->hram_io[IO_DIV]++;
			gb->counter.div_count -= DIV_CYCLES;
		}

		/* Check for RTC tick. */
		if(gb->mbc == 3 && (gb->rtc_real.reg.high & 0x40) == 0)
		{
			gb->counter.rtc_count += inst_cycles;
			while(WGB_UNLIKELY(gb->counter.rtc_count >= RTC_CYCLES))
			{
				gb->counter.rtc_count -= RTC_CYCLES;

				/* Detect invalid rollover. */
				if(WGB_UNLIKELY(gb->rtc_real.reg.sec == 63))
				{
					gb->rtc_real.reg.sec = 0;
					continue;
				}

				if(++gb->rtc_real.reg.sec != 60)
					continue;

				gb->rtc_real.reg.sec = 0;
				if(gb->rtc_real.reg.min == 63)
				{
					gb->rtc_real.reg.min = 0;
					continue;
				}
				if(++gb->rtc_real.reg.min != 60)
					continue;

				gb->rtc_real.reg.min = 0;
				if(gb->rtc_real.reg.hour == 31)
				{
					gb->rtc_real.reg.hour = 0;
					continue;
				}
				if(++gb->rtc_real.reg.hour != 24)
					continue;

				gb->rtc_real.reg.hour = 0;
				if(++gb->rtc_real.reg.yday != 0)
					continue;

				if(gb->rtc_real.reg.high & 1)  /* Bit 8 of days*/
					gb->rtc_real.reg.high |= 0x80; /* Overflow bit */

				gb->rtc_real.reg.high ^= 1;
			}
		}

		/* Check serial transmission. */
		if(gb->hram_io[IO_SC] & SERIAL_SC_TX_START)
		{
			unsigned int serial_cycles = SERIAL_CYCLES_1KB;

			/* If new transfer, call TX function. */
			if(gb->counter.serial_count == 0 &&
				gb->gb_serial_tx != NULL)
				(gb->gb_serial_tx)(gb, gb->hram_io[IO_SB]);

#if WALNUT_FULL_GBC_SUPPORT
			if(gb->hram_io[IO_SC] & 0x3)
				serial_cycles = SERIAL_CYCLES_32KB;
#endif

			gb->counter.serial_count += inst_cycles;

			/* If it's time to receive byte, call RX function. */
			if(gb->counter.serial_count >= serial_cycles)
			{
				/* If RX can be done, do it. */
				/* If RX failed, do not change SB if using external
				 * clock, or set to 0xFF if using internal clock. */
				uint8_t rx;

				if(gb->gb_serial_rx != NULL &&
					(gb->gb_serial_rx(gb, &rx) ==
						GB_SERIAL_RX_SUCCESS))
				{
					gb->hram_io[IO_SB] = rx;

					/* Inform game of serial TX/RX completion. */
					gb->hram_io[IO_SC] &= 0x01;
					gb->hram_io[IO_IF] |= SERIAL_INTR;
				}
				else if(gb->hram_io[IO_SC] & SERIAL_SC_CLOCK_SRC)
				{
					/* If using internal clock, and console is not
					 * attached to any external peripheral, shifted
					 * bits are replaced with logic 1. */
					gb->hram_io[IO_SB] = 0xFF;

					/* Inform game of serial TX/RX completion. */
					gb->hram_io[IO_SC] &= 0x01;
					gb->hram_io[IO_IF] |= SERIAL_INTR;
				}
				else
				{
					/* If using external clock, and console is not
					 * attached to any external peripheral, bits are
					 * not shifted, so SB is not modified. */
				}

				gb->counter.serial_count = 0;
			}
		}

		/* TIMA register timing */
		/* TODO: Change tac_enable to struct of TAC timer control bits. */
		if(gb->hram_io[IO_TAC] & IO_TAC_ENABLE_MASK)
		{
			gb->counter.tima_count += inst_cycles;

			while(gb->counter.tima_count >=
				TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK])
			{
				gb->counter.tima_count -=
					TAC_CYCLES[gb->hram_io[IO_TAC] & IO_TAC_RATE_MASK];

				if(++gb->hram_io[IO_TIMA] == 0)
				{
					gb->hram_io[IO_IF] |= TIMER_INTR;
					/* On overflow, set TMA to TIMA. */
					gb->hram_io[IO_TIMA] = gb->hram_io[IO_TMA];
				}
			}
		}

		/* If LCD is off, don't update LCD state or increase the LCD
		 * ticks. Instead, keep track of the amount of time that is
		 * being passed. */
		if(!(gb->hram_io[IO_LCDC] & LCDC_ENABLE))
		{
			gb->counter.lcd_off_count += inst_cycles;
			if(gb->counter.lcd_off_count >= LCD_FRAME_CYCLES)
			{
				gb->counter.lcd_off_count -= LCD_FRAME_CYCLES;
				gb->gb_frame = true;
			}
			continue;
		}

		/* LCD Timing */
#if WALNUT_FULL_GBC_SUPPORT
        if (inst_cycles > 1) {
            gb->counter.lcd_count += (inst_cycles >> gb->cgb.doubleSpeed);
        } else {
#endif
		gb->counter.lcd_count += inst_cycles;
#if WALNUT_FULL_GBC_SUPPORT
	}
#endif

		/* New Scanline. HBlank -> VBlank or OAM Scan */
		if(gb->counter.lcd_count >= LCD_LINE_CYCLES)
		{
			gb->counter.lcd_count -= LCD_LINE_CYCLES;

			/* Next line */
			gb->hram_io[IO_LY] = gb->hram_io[IO_LY] + 1;
			if (gb->hram_io[IO_LY] == LCD_VERT_LINES)
				gb->hram_io[IO_LY] = 0;

			/* LYC Update */
			if(gb->hram_io[IO_LY] == gb->hram_io[IO_LYC])
			{
				gb->hram_io[IO_STAT] |= STAT_LYC_COINC;

				if(gb->hram_io[IO_STAT] & STAT_LYC_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;
			}
			else
				gb->hram_io[IO_STAT] &= 0xFB;

			/* Check if LCD should be in Mode 1 (VBLANK) state */
			if(gb->hram_io[IO_LY] == LCD_HEIGHT)
			{
				gb->hram_io[IO_STAT] =
					(gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_VBLANK;
				gb->gb_frame = true;
				gb->hram_io[IO_IF] |= VBLANK_INTR;
				gb->lcd_blank = false;

				if(gb->hram_io[IO_STAT] & STAT_MODE_1_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;

#if ENABLE_LCD
				/* If frame skip is activated, check if we need to draw
				 * the frame or skip it. */
				if(gb->direct.frame_skip)
				{
					gb->display.frame_skip_count =
						!gb->display.frame_skip_count;
				}

				/* If interlaced is activated, change which lines get
				 * updated. Also, only update lines on frames that are
				 * actually drawn when frame skip is enabled. */
				if(gb->direct.interlace &&
						(!gb->direct.frame_skip ||
						 gb->display.frame_skip_count))
				{
					gb->display.interlace_count =
						!gb->display.interlace_count;
				}
#endif
                                /* If halted forever, then return on VBLANK. */
                                if(gb->gb_halt && !gb->hram_io[IO_IE])
					break;
			}
			/* Start of normal Line (not in VBLANK) */
			else if(gb->hram_io[IO_LY] < LCD_HEIGHT)
			{
				if(gb->hram_io[IO_LY] == 0)
				{
					/* Clear Screen */
					gb->display.WY = gb->hram_io[IO_WY];
					gb->display.window_clear = 0;
				}

				/* OAM Search occurs at the start of the line. */
				gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_OAM_SCAN;
				gb->counter.lcd_count = 0;

#if WALNUT_FULL_GBC_SUPPORT
				//DMA GBC
				if(gb->cgb.cgbMode && !gb->cgb.dmaActive && gb->cgb.dmaMode)
				{
#if WALNUT_GB_32BIT_DMA
					// Optimized 16-bit path
					for (uint8_t i = 0; i < 0x10; i += 4)
					{
							uint32_t val = __gb_read32(gb, (gb->cgb.dmaSource & 0xFFF0) + i);
							__gb_write32(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// 8-bit logic if there is some cause to fall back
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 1, val >> 8);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 2, val >> 16);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 3, val >> 24);
					}
#elif WALNUT_GB_16BIT_DMA
					// Optimized 16-bit path
					for (uint8_t i = 0; i < 0x10; i += 2)
					{
							uint16_t val = __gb_read16(gb, (gb->cgb.dmaSource & 0xFFF0) + i);
							__gb_write16(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val);
							// 8-bit logic if there is some cause to fall back
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i, val & 0xFF);
							// __gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i + 1, val >> 8);
					}
#else
			    // Original 8-bit path
					for (uint8_t i = 0; i < 0x10; i++)
					{
						__gb_write(gb, ((gb->cgb.dmaDest & 0x1FF0) | 0x8000) + i,
										__gb_read(gb, (gb->cgb.dmaSource & 0xFFF0) + i));
					}
#endif

					gb->cgb.dmaSource += 0x10;
					gb->cgb.dmaDest += 0x10;
					if(!(--gb->cgb.dmaSize)) {gb->cgb.dmaActive = 1;
					}
				}
#endif
				if(gb->hram_io[IO_STAT] & STAT_MODE_2_INTR)
					gb->hram_io[IO_IF] |= LCDC_INTR;

				/* If halted immediately jump to next LCD mode.
				 * From OAM Search to LCD Draw. */
				//if(gb->counter.lcd_count < LCD_MODE2_OAM_SCAN_END)
				//	inst_cycles = LCD_MODE2_OAM_SCAN_END - gb->counter.lcd_count;
				inst_cycles = LCD_MODE2_OAM_SCAN_DURATION;
			}
		}
		/* Go from Mode 3 (LCD Draw) to Mode 0 (HBLANK). */
		else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_LCD_DRAW &&
				gb->counter.lcd_count >= LCD_MODE3_LCD_DRAW_END)
		{
			gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_HBLANK;

			if(gb->hram_io[IO_STAT] & STAT_MODE_0_INTR)
				gb->hram_io[IO_IF] |= LCDC_INTR;

			/* If halted immediately, jump from OAM Scan to LCD Draw. */
			if (gb->counter.lcd_count < LCD_MODE0_HBLANK_MAX_DRUATION)
				inst_cycles = LCD_MODE0_HBLANK_MAX_DRUATION - gb->counter.lcd_count;
		}
		/* Go from Mode 2 (OAM Scan) to Mode 3 (LCD Draw). */
		else if((gb->hram_io[IO_STAT] & STAT_MODE) == IO_STAT_MODE_OAM_SCAN &&
				gb->counter.lcd_count >= LCD_MODE2_OAM_SCAN_END)
		{
			gb->hram_io[IO_STAT] = (gb->hram_io[IO_STAT] & ~STAT_MODE) | IO_STAT_MODE_LCD_DRAW;
#if ENABLE_LCD
			if(!gb->lcd_blank)
				__gb_draw_line(gb);
#endif
			/* If halted immediately jump to next LCD mode. */
			if (gb->counter.lcd_count < LCD_MODE3_LCD_DRAW_MIN_DURATION)
				inst_cycles = LCD_MODE3_LCD_DRAW_MIN_DURATION - gb->counter.lcd_count;
		}
	} while(gb->gb_halt && (gb->hram_io[IO_IF] & gb->hram_io[IO_IE]) == 0);
	/* If halted, loop until an interrupt occurs. */
}

/* Undefine CPU Flag helper functions. */
#undef WALNUT_GB_CPUFLAG_MASK_CARRY
#undef WALNUT_GB_CPUFLAG_MASK_HALFC
#undef WALNUT_GB_CPUFLAG_MASK_ARITH
#undef WALNUT_GB_CPUFLAG_MASK_ZERO
#undef WALNUT_GB_CPUFLAG_BIT_CARRY
#undef WALNUT_GB_CPUFLAG_BIT_HALFC
#undef WALNUT_GB_CPUFLAG_BIT_ARITH
#undef WALNUT_GB_CPUFLAG_BIT_ZERO
#undef WGB_SET_CARRY
#undef WGB_SET_HALFC
#undef WGB_SET_ARITH
#undef WGB_SET_ZERO
#undef WGB_GET_CARRY
#undef WGB_GET_HALFC
#undef WGB_GET_ARITH
#undef WGB_GET_ZERO
#endif //WALNUT_GB_H








