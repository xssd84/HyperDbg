/**
 * @file TtdEngine.h
 * @author HyperDbg Dev Team
 * @brief Header for in-kernel hypervisor Hardware Time-Travel Debugging (TTD) engine.
 * @details Declarations for PT-PML incremental CoW undo logging, hardware PMU counter
 *          fast-forwarding, ToPA stream capture, and reverse execution state reconstruction.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#pragma once

#include "SDK/headers/TtdDefinitions.h"

//////////////////////////////////////////////////
//                  Constants                   //
//////////////////////////////////////////////////

#define TTD_POOL_TAG 'dtTH' /* HTtd */

//////////////////////////////////////////////////
//                  Functions                   //
//////////////////////////////////////////////////

BOOLEAN
TtdEngineInitialize();

VOID
TtdEngineUninitialize();

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineStart(PDEBUGGER_TTD_START_REQUEST Request);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineStop(PUINT32 Status);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineGetStatus(PTTD_SESSION_STATUS Status);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineTakeCheckpoint(PDEBUGGER_TTD_CHECKPOINT_REQUEST Request);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineRestoreCheckpoint(PDEBUGGER_TTD_RESTORE_REQUEST Request);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineFastForward(PDEBUGGER_TTD_FAST_FORWARD_REQUEST Request);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineGetTraceBuffer(PDEBUGGER_TTD_GET_TRACE_REQUEST Request);

IMPORT_EXPORT_VMM NTSTATUS
TtdEngineFindMemoryWrite(PDEBUGGER_TTD_FIND_WRITE_REQUEST Request);

BOOLEAN
TtdEngineIsRecording(VOID);

BOOLEAN
TtdEngineIsReplaying(VOID);

VOID
TtdEngineRecordPmlPageDiff(VIRTUAL_MACHINE_STATE * VCpu, UINT64 AccessedPhysAddr);
