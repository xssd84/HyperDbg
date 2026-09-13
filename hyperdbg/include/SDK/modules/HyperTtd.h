/**
 * @file HyperTtd.h
 * @author HyperDbg Dev Team
 * @brief HyperDbg's SDK module for hypervisor-assisted Hardware Time-Travel Debugging (TTD).
 * @details C API contracts for Intel PT ToPA streaming, EPT PML dirty-page incremental
 *          CoW undo logging, PMU hardware counter fast-forwarding, and reverse execution.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#pragma once

#include "SDK/headers/BasicTypes.h"
#include "SDK/headers/TtdDefinitions.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IMPORT_EXPORT_LIBHYPERDBG
#    ifdef _WIN32
#        ifdef HYPERDBG_LIBHYPERDBG
#            define IMPORT_EXPORT_LIBHYPERDBG __declspec(dllexport)
#        else
#            define IMPORT_EXPORT_LIBHYPERDBG __declspec(dllimport)
#        endif
#    else
#        ifdef HYPERDBG_LIBHYPERDBG
#            define IMPORT_EXPORT_LIBHYPERDBG __attribute__((visibility("default")))
#        else
#            define IMPORT_EXPORT_LIBHYPERDBG
#        endif
#    endif
#endif

//////////////////////////////////////////////////
//              High-Level SDK APIs             //
//////////////////////////////////////////////////

/**
 * @brief Starts a Hardware Time-Travel Debugging recording session.
 *
 * @param TargetPid Target process ID to trace (0 for kernel/all).
 * @param PinToCore Optional core affinity (0xFFFFFFFF for any/all).
 * @param PtBufferSize Size of circular Intel PT ToPA ring buffer in bytes.
 * @param MaxCheckpoints Maximum number of checkpoints in the incremental ring.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStart(
    UINT32 TargetPid,
    UINT32 PinToCore,
    UINT64 PtBufferSize,
    UINT32 MaxCheckpoints
);

/**
 * @brief Stops active Hardware Time-Travel Debugging session and flushes buffers.
 *
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStop(VOID);

/**
 * @brief Queries current TTD recording or replaying session metrics.
 *
 * @param OutStatus Pointer to destination status structure.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdGetStatus(
    PTTD_SESSION_STATUS OutStatus
);

/**
 * @brief Explicitly creates an incremental checkpoint at the current vCPU state.
 *
 * @param OutCheckpointId Pointer to receive the allocated checkpoint identifier.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdTakeCheckpoint(
    PUINT32 OutCheckpointId
);

/**
 * @brief Rolls back physical RAM and architectural vCPU context to target checkpoint.
 *
 * @param CheckpointId Checkpoint ID to restore.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdRestoreCheckpoint(
    UINT32 CheckpointId
);

/**
 * @brief Reverses single instruction step (t-) to previous retired instruction.
 *
 * @param OutFrame Optional pointer to receive the restored instruction frame.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStepBackward(
    PTTD_TIMELINE_FRAME OutFrame
);

/**
 * @brief Reverses instruction step-over (p-) across function calls.
 *
 * @param OutFrame Optional pointer to receive the restored instruction frame.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStepOverBackward(
    PTTD_TIMELINE_FRAME OutFrame
);

/**
 * @brief Reverses instruction step-out (gu-) to caller function.
 *
 * @param OutFrame Optional pointer to receive the restored instruction frame.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStepOutBackward(
    PTTD_TIMELINE_FRAME OutFrame
);

/**
 * @brief Replays execution forward or backward to an exact global instruction sequence.
 *
 * @param TargetSequence Absolute instruction count sequence to seek.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdGotoInstruction(
    UINT64 TargetSequence
);

/**
 * @brief Reverse memory watchpoint: finds the instruction that wrote to a guest address.
 *
 * @param TargetAddress Guest virtual or physical address to investigate.
 * @param AccessSize Size in bytes of memory access (1, 2, 4, 8).
 * @param OutWritingRip Pointer to receive the RIP that performed the write.
 * @param OutSequence Pointer to receive the instruction sequence of the write.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdFindMemoryWrite(
    UINT64  TargetAddress,
    UINT32  AccessSize,
    PUINT64 OutWritingRip,
    PUINT64 OutSequence
);

/**
 * @brief Fetches raw Intel PT packet buffer from the hypervisor.
 *
 * @param CoreId Core index.
 * @param OutBuffer Buffer to receive raw PT packet bytes.
 * @param BufferSize Allocated capacity of OutBuffer.
 * @param OutTransferredBytes Number of bytes transferred.
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdGetTraceBuffer(
    UINT32  CoreId,
    PVOID   OutBuffer,
    UINT64  BufferSize,
    PUINT64 OutTransferredBytes
);

#ifdef __cplusplus
}
#endif
