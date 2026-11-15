/*****************************************************************************\
    Deterministic instruction control used by SMEDIT's source debugger.

    The normal Snes9x debugger is an interactive frontend. This bridge instead
    places two tiny checks in the CPU loop and keeps all control synchronous on
    libretro's owning thread. Outside an explicit debug call both checks are a
    single inactive branch and normal emulation is unchanged.
\*****************************************************************************/

#include <string.h>

#include "smedit_debugger.h"
#include "memmap.h"

#define SMEDIT_DEBUG_MAX_BREAKPOINTS 256

static_assert(sizeof(SmeditDebugCpuState) == 48, "SMEDIT debugger ABI layout changed");

static bool8 smedit_debug_active = FALSE;
static bool8 smedit_debug_single_step = FALSE;
static uint32 smedit_debug_addresses[SMEDIT_DEBUG_MAX_BREAKPOINTS];
static uint32 smedit_debug_address_count = 0;
static uint64 smedit_debug_instruction_limit = 0;
static uint64 smedit_debug_instructions = 0;
static enum SmeditDebugHaltReason smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_NONE;

void SmeditDebuggerReset(void)
{
	smedit_debug_active = FALSE;
	smedit_debug_single_step = FALSE;
	smedit_debug_address_count = 0;
	smedit_debug_instruction_limit = 0;
	smedit_debug_instructions = 0;
	smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_NONE;
}

bool8 SmeditDebuggerBeginStep(void)
{
	SmeditDebuggerReset();
	if (CPU.Flags & HALTED_FLAG)
	{
		smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_CPU_STOPPED;
		return FALSE;
	}
	smedit_debug_single_step = TRUE;
	smedit_debug_instruction_limit = 1;
	smedit_debug_active = TRUE;
	return TRUE;
}

bool8 SmeditDebuggerBeginRun(const uint32 *addresses, uint32 address_count, uint64 instruction_limit)
{
	SmeditDebuggerReset();
	if (instruction_limit == 0 || address_count > SMEDIT_DEBUG_MAX_BREAKPOINTS ||
		(address_count != 0 && addresses == NULL))
	{
		smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_INVALID_REQUEST;
		return FALSE;
	}
	if (CPU.Flags & HALTED_FLAG)
	{
		smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_CPU_STOPPED;
		return FALSE;
	}
	for (uint32 i = 0; i < address_count; i++)
		smedit_debug_addresses[i] = addresses[i] & 0x00ffffff;
	smedit_debug_address_count = address_count;
	smedit_debug_instruction_limit = instruction_limit;
	smedit_debug_active = TRUE;
	return TRUE;
}

bool8 SmeditDebuggerBeforeInstruction(void)
{
	if (!smedit_debug_active || smedit_debug_single_step)
		return FALSE;

	uint32 pc = Registers.PBPC & 0x00ffffff;
	for (uint32 i = 0; i < smedit_debug_address_count; i++)
	{
		if (smedit_debug_addresses[i] == pc)
		{
			SmeditDebuggerStop(SMEDIT_DEBUG_HALT_BREAKPOINT);
			return TRUE;
		}
	}
	return FALSE;
}

bool8 SmeditDebuggerAfterInstruction(void)
{
	if (!smedit_debug_active)
		return FALSE;

	smedit_debug_instructions++;
	if (CPU.Flags & HALTED_FLAG)
	{
		SmeditDebuggerStop(SMEDIT_DEBUG_HALT_CPU_STOPPED);
		return TRUE;
	}
	if (smedit_debug_single_step)
	{
		SmeditDebuggerStop(SMEDIT_DEBUG_HALT_STEP);
		return TRUE;
	}
	if (smedit_debug_instructions >= smedit_debug_instruction_limit)
	{
		SmeditDebuggerStop(SMEDIT_DEBUG_HALT_INSTRUCTION_LIMIT);
		return TRUE;
	}
	return FALSE;
}

bool8 SmeditDebuggerIsActive(void)
{
	return smedit_debug_active;
}

void SmeditDebuggerStop(enum SmeditDebugHaltReason reason)
{
	smedit_debug_active = FALSE;
	smedit_debug_halt_reason = reason;
}

enum SmeditDebugHaltReason SmeditDebuggerHaltReason(void)
{
	return smedit_debug_halt_reason;
}

uint64 SmeditDebuggerInstructions(void)
{
	return smedit_debug_instructions;
}

void SmeditDebuggerGetCpuState(struct SmeditDebugCpuState *state)
{
	if (state == NULL)
		return;

	S9xPackStatus();
	memset(state, 0, sizeof(*state));
	state->size = sizeof(*state);
	state->pc = Registers.PBPC & 0x00ffffff;
	state->a = Registers.A.W;
	state->x = Registers.X.W;
	state->y = Registers.Y.W;
	state->d = Registers.D.W;
	state->s = Registers.S.W;
	state->p = Registers.P.W;
	state->db = Registers.DB;
	state->pb = Registers.PB;
	state->memory_width = CheckMemory() ? 8 : 16;
	state->index_width = CheckIndex() ? 8 : 16;
	state->emulation = CheckEmulation() ? 1 : 0;
	state->waiting_for_interrupt = CPU.WaitingForInterrupt ? 1 : 0;
	state->halted = (CPU.Flags & HALTED_FLAG) ? 1 : 0;
	state->cycles = CPU.Cycles;
	state->v_counter = CPU.V_Counter;
	state->core_frame = ICPU.Frame;
	state->instructions = smedit_debug_instructions;
}

bool8 SmeditDebuggerPeekByte(uint32 address, uint8 *value)
{
	if (value == NULL)
		return FALSE;
	int block = (address & 0x00ffffff) >> MEMMAP_SHIFT;
	uint8 *mapped = Memory.Map[block];
	if (mapped >= (uint8 *) CMemory::MAP_LAST)
	{
		*value = *(mapped + (address & 0xffff));
		return TRUE;
	}

	switch ((pint) mapped)
	{
		case CMemory::MAP_LOROM_SRAM:
		case CMemory::MAP_SA1RAM:
			*value = *(Memory.SRAM + ((((address & 0xff0000) >> 1) | (address & 0x7fff)) & Memory.SRAMMask));
			return TRUE;
		case CMemory::MAP_LOROM_SRAM_B:
			*value = *(Multi.sramB + ((((address & 0xff0000) >> 1) | (address & 0x7fff)) & Multi.sramMaskB));
			return TRUE;
		case CMemory::MAP_HIROM_SRAM:
		case CMemory::MAP_RONLY_SRAM:
			*value = *(Memory.SRAM + (((address & 0x7fff) - 0x6000 + ((address & 0x1f0000) >> 3)) & Memory.SRAMMask));
			return TRUE;
		case CMemory::MAP_BWRAM:
			*value = *(Memory.BWRAM + ((address & 0x7fff) - 0x6000));
			return TRUE;
		case CMemory::MAP_NONE:
			*value = OpenBus;
			return TRUE;
		default:
			/* CPU, PPU and coprocessor reads may acknowledge or advance
			 * hardware state. Refuse those ranges instead of mutating a
			 * paused debug session merely because the user inspected it. */
			return FALSE;
	}
}
