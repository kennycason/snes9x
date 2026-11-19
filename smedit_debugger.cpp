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
#define SMEDIT_DEBUG_MAX_WATCHPOINTS 64
#define SMEDIT_DEBUG_HIT_VALUE_BEFORE_VALID 1
#define SMEDIT_DEBUG_HIT_VALUE_AFTER_VALID 2

static_assert(sizeof(SmeditDebugCpuState) == 48, "SMEDIT debugger ABI layout changed");
static_assert(sizeof(SmeditDebugWatchpoint) == 16, "SMEDIT watchpoint ABI layout changed");
static_assert(sizeof(SmeditDebugWatchpointHit) == 40, "SMEDIT watchpoint-hit ABI layout changed");

static bool8 smedit_debug_active = FALSE;
static bool8 smedit_debug_single_step = FALSE;
static enum SmeditDebugStepControl smedit_debug_step_control = SMEDIT_DEBUG_CONTROL_NONE;
static bool8 smedit_debug_first_instruction = FALSE;
static uint32 smedit_debug_start_pc = 0;
static uint8 smedit_debug_current_opcode = 0;
static uint32 smedit_debug_call_depth = 0;
static uint32 smedit_debug_interrupt_depth = 0;
static bool8 smedit_debug_root_interrupt = FALSE;
static uint32 smedit_debug_addresses[SMEDIT_DEBUG_MAX_BREAKPOINTS];
static uint32 smedit_debug_address_count = 0;
static uint64 smedit_debug_instruction_limit = 0;
static uint64 smedit_debug_instructions = 0;
static enum SmeditDebugHaltReason smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_NONE;
static struct SmeditDebugWatchpoint smedit_debug_watchpoints[SMEDIT_DEBUG_MAX_WATCHPOINTS];
static uint32 smedit_debug_watchpoint_count = 0;
static struct SmeditDebugWatchpointHit smedit_debug_watchpoint_hit;
static bool8 smedit_debug_watchpoint_hit_pending = FALSE;
static bool8 smedit_debug_instruction_active = FALSE;
static uint32 smedit_debug_instruction_pc = 0;

void SmeditDebuggerReset(void)
{
	smedit_debug_active = FALSE;
	smedit_debug_single_step = FALSE;
	smedit_debug_step_control = SMEDIT_DEBUG_CONTROL_NONE;
	smedit_debug_first_instruction = FALSE;
	smedit_debug_start_pc = 0;
	smedit_debug_current_opcode = 0;
	smedit_debug_call_depth = 0;
	smedit_debug_interrupt_depth = 0;
	smedit_debug_root_interrupt = FALSE;
	smedit_debug_address_count = 0;
	smedit_debug_instruction_limit = 0;
	smedit_debug_instructions = 0;
	smedit_debug_halt_reason = SMEDIT_DEBUG_HALT_NONE;
	memset(&smedit_debug_watchpoint_hit, 0, sizeof(smedit_debug_watchpoint_hit));
	smedit_debug_watchpoint_hit.size = sizeof(smedit_debug_watchpoint_hit);
	smedit_debug_watchpoint_hit_pending = FALSE;
	smedit_debug_instruction_active = FALSE;
	smedit_debug_instruction_pc = 0;
}

bool8 SmeditDebuggerSetWatchpoints(const struct SmeditDebugWatchpoint *watchpoints, uint32 count)
{
	if (count > SMEDIT_DEBUG_MAX_WATCHPOINTS || (count != 0 && watchpoints == NULL))
		return FALSE;
	for (uint32 i = 0; i < count; i++)
	{
		uint32 access = watchpoints[i].access;
		uint64 end = (uint64) watchpoints[i].address + watchpoints[i].size;
		if (watchpoints[i].address > 0x00ffffff || watchpoints[i].size == 0 ||
			watchpoints[i].size > 4 || end > 0x01000000 || access == 0 ||
			(access & ~(SMEDIT_DEBUG_WATCH_READ | SMEDIT_DEBUG_WATCH_WRITE)) != 0)
			return FALSE;
	}
	for (uint32 i = 0; i < count; i++)
		smedit_debug_watchpoints[i] = watchpoints[i];
	smedit_debug_watchpoint_count = count;
	return TRUE;
}

bool8 SmeditDebuggerGetWatchpointHit(struct SmeditDebugWatchpointHit *hit)
{
	if (hit == NULL || hit->size < sizeof(struct SmeditDebugWatchpointHit) ||
		smedit_debug_halt_reason != SMEDIT_DEBUG_HALT_WATCHPOINT)
		return FALSE;
	*hit = smedit_debug_watchpoint_hit;
	return TRUE;
}

void SmeditDebuggerInstructionStarted(uint32 pc, uint8 opcode)
{
	if (!smedit_debug_active)
		return;
	smedit_debug_instruction_pc = pc & 0x00ffffff;
	smedit_debug_current_opcode = opcode;
	smedit_debug_instruction_active = TRUE;
}

void SmeditDebuggerInstructionEnded(void)
{
	smedit_debug_instruction_active = FALSE;
}

static bool8 SmeditDebuggerRangesOverlap(uint32 first, uint32 first_size, uint32 second, uint32 second_size)
{
	return (uint64) first < (uint64) second + second_size &&
		(uint64) second < (uint64) first + first_size;
}

static void SmeditDebuggerObserveAccess(uint32 address, uint32 size, uint32 access, uint32 value)
{
	if (!smedit_debug_active || !smedit_debug_instruction_active || CPU.InDMAorHDMA || size == 0 || size > 4)
		return;
	address &= 0x00ffffff;
	for (uint32 i = 0; i < smedit_debug_watchpoint_count; i++)
	{
		const struct SmeditDebugWatchpoint *watchpoint = &smedit_debug_watchpoints[i];
		if ((watchpoint->access & access) == 0 ||
			!SmeditDebuggerRangesOverlap(address, size, watchpoint->address, watchpoint->size))
			continue;

		/* A word helper may contain byte helpers. Prefer the complete outer
		 * access, but otherwise retain the first matching access in the
		 * instruction so the reported PC/value pair remains deterministic. */
		if (smedit_debug_watchpoint_hit_pending)
		{
			uint64 pending_end = (uint64) smedit_debug_watchpoint_hit.access_address +
				smedit_debug_watchpoint_hit.access_size;
			uint64 access_end = (uint64) address + size;
			bool8 contains_pending = access == smedit_debug_watchpoint_hit.access &&
				address <= smedit_debug_watchpoint_hit.access_address && access_end >= pending_end;
			if (!contains_pending || size <= smedit_debug_watchpoint_hit.access_size)
				return;
		}

		memset(&smedit_debug_watchpoint_hit, 0, sizeof(smedit_debug_watchpoint_hit));
		smedit_debug_watchpoint_hit.size = sizeof(smedit_debug_watchpoint_hit);
		smedit_debug_watchpoint_hit.watchpoint_index = i;
		smedit_debug_watchpoint_hit.instruction_pc = smedit_debug_instruction_pc;
		smedit_debug_watchpoint_hit.access_address = address;
		smedit_debug_watchpoint_hit.access_size = size;
		smedit_debug_watchpoint_hit.access = access;
		smedit_debug_watchpoint_hit.value_size = size;
		smedit_debug_watchpoint_hit.value_after = value;
		smedit_debug_watchpoint_hit.flags = SMEDIT_DEBUG_HIT_VALUE_AFTER_VALID;
		if (access == SMEDIT_DEBUG_WATCH_WRITE)
		{
			uint32 before = 0;
			bool8 before_valid = TRUE;
			for (uint32 byte_index = 0; byte_index < size; byte_index++)
			{
				uint8 byte = 0;
				if (!SmeditDebuggerPeekByte(address + byte_index, &byte))
				{
					before_valid = FALSE;
					break;
				}
				before |= ((uint32) byte) << (byte_index * 8);
			}
			if (before_valid)
			{
				smedit_debug_watchpoint_hit.value_before = before;
				smedit_debug_watchpoint_hit.flags |= SMEDIT_DEBUG_HIT_VALUE_BEFORE_VALID;
			}
		}
		smedit_debug_watchpoint_hit_pending = TRUE;
		return;
	}
}

void SmeditDebuggerObserveRead(uint32 address, uint32 size, uint32 value)
{
	SmeditDebuggerObserveAccess(address, size, SMEDIT_DEBUG_WATCH_READ, value);
}

void SmeditDebuggerObserveWrite(uint32 address, uint32 size, uint32 value)
{
	SmeditDebuggerObserveAccess(address, size, SMEDIT_DEBUG_WATCH_WRITE, value);
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

void SmeditDebuggerEnterInterrupt(void)
{
	if (smedit_debug_active && smedit_debug_step_control != SMEDIT_DEBUG_CONTROL_NONE)
		smedit_debug_interrupt_depth++;
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

bool8 SmeditDebuggerBeginStructuredStep(
	enum SmeditDebugStepControl control,
	const uint32 *addresses,
	uint32 address_count,
	uint64 instruction_limit)
{
	if (control != SMEDIT_DEBUG_CONTROL_OVER && control != SMEDIT_DEBUG_CONTROL_OUT)
		return FALSE;
	if (!SmeditDebuggerBeginRun(addresses, address_count, instruction_limit))
		return FALSE;
	smedit_debug_step_control = control;
	smedit_debug_first_instruction = TRUE;
	smedit_debug_start_pc = Registers.PBPC & 0x00ffffff;
	return TRUE;
}

static bool8 SmeditDebuggerCallOpcode(uint8 opcode)
{
	return opcode == 0x20 || opcode == 0x22 || opcode == 0xfc; // JSR abs, JSL long, JSR (abs,X)
}

static bool8 SmeditDebuggerReturnOpcode(uint8 opcode)
{
	return opcode == 0x60 || opcode == 0x6b; // RTS, RTL. RTI does not leave the selected routine.
}

bool8 SmeditDebuggerBeforeInstruction(void)
{
	if (!smedit_debug_active || smedit_debug_single_step)
		return FALSE;

	uint32 pc = Registers.PBPC & 0x00ffffff;
	bool8 skip_initial_breakpoint = smedit_debug_first_instruction && pc == smedit_debug_start_pc;
	if (!skip_initial_breakpoint)
	{
		for (uint32 i = 0; i < smedit_debug_address_count; i++)
		{
			if (smedit_debug_addresses[i] == pc)
			{
				SmeditDebuggerStop(SMEDIT_DEBUG_HALT_BREAKPOINT);
				return TRUE;
			}
		}
	}
	if (smedit_debug_step_control != SMEDIT_DEBUG_CONTROL_NONE &&
		!SmeditDebuggerPeekByte(pc, &smedit_debug_current_opcode))
	{
		SmeditDebuggerStop(SMEDIT_DEBUG_HALT_INVALID_REQUEST);
		return TRUE;
	}
	return FALSE;
}

bool8 SmeditDebuggerAfterInstruction(void)
{
	if (!smedit_debug_active)
		return FALSE;

	smedit_debug_instructions++;
	if (smedit_debug_watchpoint_hit_pending)
	{
		SmeditDebuggerStop(SMEDIT_DEBUG_HALT_WATCHPOINT);
		return TRUE;
	}
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
	if (smedit_debug_step_control != SMEDIT_DEBUG_CONTROL_NONE)
	{
		bool8 prelude_interrupt = smedit_debug_first_instruction &&
			smedit_debug_interrupt_depth > 0 && !smedit_debug_root_interrupt;
		bool8 call = SmeditDebuggerCallOpcode(smedit_debug_current_opcode);
		bool8 return_instruction = SmeditDebuggerReturnOpcode(smedit_debug_current_opcode);
		bool8 interrupt_entry = smedit_debug_current_opcode == 0x00 || smedit_debug_current_opcode == 0x02; // BRK / COP
		bool8 interrupt_return = smedit_debug_current_opcode == 0x40; // RTI closes only an interrupt scope.
		if (interrupt_entry)
		{
			if (smedit_debug_first_instruction && smedit_debug_step_control == SMEDIT_DEBUG_CONTROL_OVER)
				smedit_debug_root_interrupt = TRUE;
			smedit_debug_interrupt_depth++;
		}
		else if (interrupt_return && smedit_debug_interrupt_depth > 0)
		{
			smedit_debug_interrupt_depth--;
			if (smedit_debug_root_interrupt && smedit_debug_interrupt_depth == 0)
			{
				SmeditDebuggerStop(SMEDIT_DEBUG_HALT_STEP_OVER);
				return TRUE;
			}
		}
		else if (smedit_debug_interrupt_depth > 0)
		{
			// Handler-local calls are irrelevant to the selected routine. Their
			// stack frame is closed before RTI and must not change call depth.
		}
		else if (smedit_debug_step_control == SMEDIT_DEBUG_CONTROL_OVER)
		{
			if (smedit_debug_first_instruction && !call)
			{
				SmeditDebuggerStop(SMEDIT_DEBUG_HALT_STEP_OVER);
				return TRUE;
			}
			if (call)
				smedit_debug_call_depth++;
			else if (return_instruction && smedit_debug_call_depth > 0)
			{
				smedit_debug_call_depth--;
				if (smedit_debug_call_depth == 0)
				{
					SmeditDebuggerStop(SMEDIT_DEBUG_HALT_STEP_OVER);
					return TRUE;
				}
			}
		}
		else
		{
			if (call)
				smedit_debug_call_depth++;
			else if (return_instruction)
			{
				if (smedit_debug_call_depth == 0)
				{
					SmeditDebuggerStop(SMEDIT_DEBUG_HALT_STEP_OUT);
					return TRUE;
				}
				smedit_debug_call_depth--;
			}
		}
		if (!prelude_interrupt)
			smedit_debug_first_instruction = FALSE;
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
