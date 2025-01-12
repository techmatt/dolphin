// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/Arm64Emitter.h"
#include "Common/Common.h"

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/JitArm64/Jit.h"
#include "Core/PowerPC/JitArm64/JitArm64_RegCache.h"
#include "Core/PowerPC/JitArm64/JitAsm.h"

using namespace Arm64Gen;

void JitArm64::lfXX(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITLoadStoreFloatingOff);

	u32 a = inst.RA, b = inst.RB;

	s32 offset = inst.SIMM_16;
	u32 flags = BackPatchInfo::FLAG_LOAD;
	bool update = false;
	s32 offset_reg = -1;

	switch (inst.OPCD)
	{
		case 31:
			switch (inst.SUBOP10)
			{
				case 567: // lfsux
					flags |= BackPatchInfo::FLAG_SIZE_F32;
					update = true;
					offset_reg = b;
				break;
				case 535: // lfsx
					flags |= BackPatchInfo::FLAG_SIZE_F32;
					offset_reg = b;
				break;
				case 631: // lfdux
					flags |= BackPatchInfo::FLAG_SIZE_F64;
					update = true;
					offset_reg = b;
				break;
				case 599: // lfdx
					flags |= BackPatchInfo::FLAG_SIZE_F64;
					offset_reg = b;
				break;
			}
		break;
		case 49: // lfsu
			flags |= BackPatchInfo::FLAG_SIZE_F32;
			update = true;
		break;
		case 48: // lfs
			flags |= BackPatchInfo::FLAG_SIZE_F32;
		break;
		case 51: // lfdu
			flags |= BackPatchInfo::FLAG_SIZE_F64;
			update = true;
		break;
		case 50: // lfd
			flags |= BackPatchInfo::FLAG_SIZE_F64;
		break;
	}

	u32 imm_addr = 0;
	bool is_immediate = false;

	bool only_lower = !!(flags & BackPatchInfo::FLAG_SIZE_F64);

	fpr.BindToRegister(inst.FD, false, only_lower);

	ARM64Reg VD = fpr.R(inst.FD, only_lower);
	ARM64Reg addr_reg = W0;

	if (!fpr.IsLower(inst.FD))
		only_lower = false;

	if (only_lower)
		flags |= BackPatchInfo::FLAG_ONLY_LOWER;

	gpr.Lock(W0, W30);
	fpr.Lock(Q0);

	if (update)
	{
		// Always uses RA
		if (gpr.IsImm(a) && offset_reg == -1)
		{
			is_immediate = true;
			imm_addr = offset + gpr.GetImm(a);
		}
		else if (gpr.IsImm(a) && offset_reg != -1 && gpr.IsImm(offset_reg))
		{
			is_immediate = true;
			imm_addr = gpr.GetImm(a) + gpr.GetImm(offset_reg);
		}
		else
		{
			if (offset_reg == -1)
			{
				if (offset >= 0 && offset < 4096)
				{
					ADD(addr_reg, gpr.R(a), offset);
				}
				else if (offset < 0 && offset > -4096)
				{
					SUB(addr_reg, gpr.R(a), std::abs(offset));
				}
				else
				{
					MOVI2R(addr_reg, offset);
					ADD(addr_reg, addr_reg, gpr.R(a));
				}			}
			else
			{
				ADD(addr_reg, gpr.R(offset_reg), gpr.R(a));
			}
		}
	}
	else
	{
		if (offset_reg == -1)
		{
			if (a && gpr.IsImm(a))
			{
				is_immediate = true;
				imm_addr = gpr.GetImm(a) + offset;
			}
			else if (a)
			{
				if (offset >= 0 && offset < 4096)
				{
					ADD(addr_reg, gpr.R(a), offset);
				}
				else if (offset < 0 && offset > -4096)
				{
					SUB(addr_reg, gpr.R(a), std::abs(offset));
				}
				else
				{
					MOVI2R(addr_reg, offset);
					ADD(addr_reg, addr_reg, gpr.R(a));
				}			}
			else
			{
				is_immediate = true;
				imm_addr = offset;
			}
		}
		else
		{
			if (a && gpr.IsImm(a) && gpr.IsImm(offset_reg))
			{
				is_immediate = true;
				imm_addr = gpr.GetImm(a) + gpr.GetImm(offset_reg);
			}
			else if (!a && gpr.IsImm(offset_reg))
			{
				is_immediate = true;
				imm_addr = gpr.GetImm(offset_reg);
			}
			else if (a)
			{
				ADD(addr_reg, gpr.R(a), gpr.R(offset_reg));
			}
			else
			{
				MOV(addr_reg, gpr.R(offset_reg));
			}
		}
	}

	ARM64Reg XA = EncodeRegTo64(addr_reg);

	if (is_immediate)
		MOVI2R(XA, imm_addr);

	if (update)
	{
		gpr.BindToRegister(a, false);
		MOV(gpr.R(a), addr_reg);
	}

	BitSet32 regs_in_use = gpr.GetCallerSavedUsed();
	BitSet32 fprs_in_use = fpr.GetCallerSavedUsed();
	regs_in_use[W0] = 0;
	fprs_in_use[0] = 0; // Q0
	fprs_in_use[VD - Q0] = 0;

	if (is_immediate && PowerPC::IsOptimizableRAMAddress(imm_addr))
	{
		EmitBackpatchRoutine(flags, true, false, VD, XA, BitSet32(0), BitSet32(0));
	}
	else
	{
		EmitBackpatchRoutine(flags,
			jo.fastmem,
			jo.fastmem,
			VD, XA,
			regs_in_use, fprs_in_use);
	}

	gpr.Unlock(W0, W30);
	fpr.Unlock(Q0);
}

void JitArm64::stfXX(UGeckoInstruction inst)
{
	INSTRUCTION_START
	JITDISABLE(bJITLoadStoreFloatingOff);

	u32 a = inst.RA, b = inst.RB;

	s32 offset = inst.SIMM_16;
	u32 flags = BackPatchInfo::FLAG_STORE;
	bool update = false;
	s32 offset_reg = -1;

	switch (inst.OPCD)
	{
		case 31:
			switch (inst.SUBOP10)
			{
				case 663: // stfsx
					flags |= BackPatchInfo::FLAG_SIZE_F32;
					offset_reg = b;
				break;
				case 695: // stfsux
					flags |= BackPatchInfo::FLAG_SIZE_F32;
					update = true;
					offset_reg = b;
				break;
				case 727: // stfdx
					flags |= BackPatchInfo::FLAG_SIZE_F64;
					offset_reg = b;
				break;
				case 759: // stfdux
					flags |= BackPatchInfo::FLAG_SIZE_F64;
					update = true;
					offset_reg = b;
				break;
				case 983: // stfiwx
					flags |= BackPatchInfo::FLAG_SIZE_F32I;
					offset_reg = b;
				break;
			}
		break;
		case 53: // stfsu
			flags |= BackPatchInfo::FLAG_SIZE_F32;
			update = true;
		break;
		case 52: // stfs
			flags |= BackPatchInfo::FLAG_SIZE_F32;
		break;
		case 55: // stfdu
			flags |= BackPatchInfo::FLAG_SIZE_F64;
			update = true;
		break;
		case 54: // stfd
			flags |= BackPatchInfo::FLAG_SIZE_F64;
		break;
	}

	u32 imm_addr = 0;
	bool is_immediate = false;

	ARM64Reg V0 = fpr.R(inst.FS);
	ARM64Reg addr_reg = W1;

	gpr.Lock(W0, W1, W30);
	fpr.Lock(Q0);

	if (update)
	{
		// Always uses RA
		if (gpr.IsImm(a) && offset_reg == -1)
		{
			is_immediate = true;
			imm_addr = offset + gpr.GetImm(a);
		}
		else if (gpr.IsImm(a) && offset_reg != -1 && gpr.IsImm(offset_reg))
		{
			is_immediate = true;
			imm_addr = gpr.GetImm(a) + gpr.GetImm(offset_reg);
		}
		else
		{
			if (offset_reg == -1)
			{
				if (offset >= 0 && offset < 4096)
				{
					ADD(addr_reg, gpr.R(a), offset);
				}
				else if (offset < 0 && offset > -4096)
				{
					SUB(addr_reg, gpr.R(a), std::abs(offset));
				}
				else
				{
					MOVI2R(addr_reg, offset);
					ADD(addr_reg, addr_reg, gpr.R(a));
				}
			}
			else
			{
				ADD(addr_reg, gpr.R(offset_reg), gpr.R(a));
			}
		}
	}
	else
	{
		if (offset_reg == -1)
		{
			if (a && gpr.IsImm(a))
			{
				is_immediate = true;
				imm_addr = gpr.GetImm(a) + offset;
			}
			else if (a)
			{
				if (offset >= 0 && offset < 4096)
				{
					ADD(addr_reg, gpr.R(a), offset);
				}
				else if (offset < 0 && offset > -4096)
				{
					SUB(addr_reg, gpr.R(a), std::abs(offset));
				}
				else
				{
					MOVI2R(addr_reg, offset);
					ADD(addr_reg, addr_reg, gpr.R(a));
				}			}
			else
			{
				is_immediate = true;
				imm_addr = offset;
			}
		}
		else
		{
			if (a && gpr.IsImm(a) && gpr.IsImm(offset_reg))
			{
				is_immediate = true;
				imm_addr = gpr.GetImm(a) + gpr.GetImm(offset_reg);
			}
			else if (!a && gpr.IsImm(offset_reg))
			{
				is_immediate = true;
				imm_addr = gpr.GetImm(offset_reg);
			}
			else if (a)
			{
				ADD(addr_reg, gpr.R(a), gpr.R(offset_reg));
			}
			else
			{
				MOV(addr_reg, gpr.R(offset_reg));
			}
		}
	}

	ARM64Reg XA = EncodeRegTo64(addr_reg);

	if (is_immediate && !(jit->jo.optimizeGatherPipe && PowerPC::IsOptimizableGatherPipeWrite(imm_addr)))
	{
		MOVI2R(XA, imm_addr);

		if (update)
		{
			gpr.BindToRegister(a, false);
			MOV(gpr.R(a), addr_reg);
		}
	}
	else if (!is_immediate && update)
	{
		gpr.BindToRegister(a, false);
		MOV(gpr.R(a), addr_reg);
	}

	BitSet32 regs_in_use = gpr.GetCallerSavedUsed();
	BitSet32 fprs_in_use = fpr.GetCallerSavedUsed();
	regs_in_use[W0] = 0;
	regs_in_use[W1] = 0;
	fprs_in_use[0] = 0; // Q0

	if (is_immediate)
	{
		if (jit->jo.optimizeGatherPipe && PowerPC::IsOptimizableGatherPipeWrite(imm_addr))
		{
			int accessSize;
			if (flags & BackPatchInfo::FLAG_SIZE_F64)
				accessSize = 64;
			else
				accessSize = 32;

			u64 base_ptr = std::min((u64)&GPFifo::m_gatherPipeCount, (u64)&GPFifo::m_gatherPipe);
			u32 count_off = (u64)&GPFifo::m_gatherPipeCount - base_ptr;
			u32 pipe_off = (u64)&GPFifo::m_gatherPipe - base_ptr;

			MOVI2R(X30, base_ptr);

			if (pipe_off)
				ADD(X1, X30, pipe_off);

			LDR(INDEX_UNSIGNED, W0, X30, count_off);
			if (accessSize == 64)
			{
				m_float_emit.REV64(8, Q0, V0);
				if (pipe_off)
					m_float_emit.STR(64, Q0, X1, ArithOption(X0));
				else
					m_float_emit.STR(64, Q0, X30, ArithOption(X0));
			}
			else if (accessSize == 32)
			{
				m_float_emit.FCVT(32, 64, D0, EncodeRegToDouble(V0));
				m_float_emit.REV32(8, D0, D0);
				if (pipe_off)
					m_float_emit.STR(32, D0, X1, ArithOption(X0));
				else
					m_float_emit.STR(32, D0, X30, ArithOption(X0));

			}
			ADD(W0, W0, accessSize >> 3);
			STR(INDEX_UNSIGNED, W0, X30, count_off);
			jit->js.fifoBytesThisBlock += accessSize >> 3;

			if (update)
			{
				// Chance of this happening is fairly low, but support it
				gpr.BindToRegister(a, false);
				MOVI2R(gpr.R(a), imm_addr);
			}
		}
		else if (PowerPC::IsOptimizableRAMAddress(imm_addr))
		{
			EmitBackpatchRoutine(flags, true, false, V0, XA, BitSet32(0), BitSet32(0));
		}
		else
		{
			EmitBackpatchRoutine(flags, false, false, V0, XA, regs_in_use, fprs_in_use);
		}
	}
	else
	{
		EmitBackpatchRoutine(flags,
			jo.fastmem,
			jo.fastmem,
			V0, XA,
			regs_in_use, fprs_in_use);
	}
	gpr.Unlock(W0, W1, W30);
	fpr.Unlock(Q0);
}
