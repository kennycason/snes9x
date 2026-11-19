/*****************************************************************************\
    SMEDIT's versioned, opt-in debugger bridge for the bundled Snes9x core.

    This is deliberately separate from libretro. A stock libretro core does not
    promise instruction stepping or CPU register access, so callers must probe
    smedit_debug_api_version before using any other entry point.
\*****************************************************************************/

#ifndef _SMEDIT_DEBUGGER_H_
#define _SMEDIT_DEBUGGER_H_

#include "snes9x.h"

enum SmeditDebugHaltReason
{
	SMEDIT_DEBUG_HALT_NONE = 0,
	SMEDIT_DEBUG_HALT_STEP = 1,
	SMEDIT_DEBUG_HALT_BREAKPOINT = 2,
	SMEDIT_DEBUG_HALT_INSTRUCTION_LIMIT = 3,
	SMEDIT_DEBUG_HALT_FRAME_LIMIT = 4,
	SMEDIT_DEBUG_HALT_CPU_STOPPED = 5,
	SMEDIT_DEBUG_HALT_INVALID_REQUEST = 6,
	SMEDIT_DEBUG_HALT_STEP_OVER = 7,
	SMEDIT_DEBUG_HALT_STEP_OUT = 8,
	SMEDIT_DEBUG_HALT_WATCHPOINT = 9
};

enum SmeditDebugWatchpointAccess
{
	SMEDIT_DEBUG_WATCH_READ = 1,
	SMEDIT_DEBUG_WATCH_WRITE = 2
};

enum SmeditDebugStepControl
{
	SMEDIT_DEBUG_CONTROL_NONE = 0,
	SMEDIT_DEBUG_CONTROL_OVER = 1,
	SMEDIT_DEBUG_CONTROL_OUT = 2
};

struct SmeditDebugCpuState
{
	uint32 size;
	uint32 pc;
	uint16 a;
	uint16 x;
	uint16 y;
	uint16 d;
	uint16 s;
	uint16 p;
	uint8 db;
	uint8 pb;
	uint8 memory_width;
	uint8 index_width;
	uint8 emulation;
	uint8 waiting_for_interrupt;
	uint8 halted;
	uint8 reserved;
	int32 cycles;
	int32 v_counter;
	uint32 core_frame;
	uint64 instructions;
};

struct SmeditDebugWatchpoint
{
	uint32 address;
	uint32 size;
	uint32 access;
	uint32 reserved;
};

struct SmeditDebugWatchpointHit
{
	uint32 size;
	uint32 watchpoint_index;
	uint32 instruction_pc;
	uint32 access_address;
	uint32 access_size;
	uint32 access;
	uint32 value_before;
	uint32 value_after;
	uint32 value_size;
	uint32 flags;
};

void SmeditDebuggerReset(void);
bool8 SmeditDebuggerBeginStep(void);
bool8 SmeditDebuggerBeginRun(const uint32 *addresses, uint32 address_count, uint64 instruction_limit);
bool8 SmeditDebuggerBeginStructuredStep(
	enum SmeditDebugStepControl control,
	const uint32 *addresses,
	uint32 address_count,
	uint64 instruction_limit);
void SmeditDebuggerEnterInterrupt(void);
void SmeditDebuggerInstructionStarted(uint32 pc, uint8 opcode);
void SmeditDebuggerInstructionEnded(void);
bool8 SmeditDebuggerSetWatchpoints(const struct SmeditDebugWatchpoint *watchpoints, uint32 count);
bool8 SmeditDebuggerGetWatchpointHit(struct SmeditDebugWatchpointHit *hit);
void SmeditDebuggerObserveRead(uint32 address, uint32 size, uint32 value);
void SmeditDebuggerObserveWrite(uint32 address, uint32 size, uint32 value);
bool8 SmeditDebuggerBeforeInstruction(void);
bool8 SmeditDebuggerAfterInstruction(void);
bool8 SmeditDebuggerIsActive(void);
void SmeditDebuggerStop(enum SmeditDebugHaltReason reason);
enum SmeditDebugHaltReason SmeditDebuggerHaltReason(void);
uint64 SmeditDebuggerInstructions(void);
void SmeditDebuggerGetCpuState(struct SmeditDebugCpuState *state);
bool8 SmeditDebuggerPeekByte(uint32 address, uint8 *value);

#endif
