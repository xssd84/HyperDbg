/**
 * @file TtdDefinitions.h
 * @author HyperDbg Dev Team
 * @brief Hardware Time-Travel Debugging (TTD) shared definitions and data structures.
 * @details Declarations for PT-PML incremental CoW undo logging, PMU hardware counter
 *          fast-forwarding, ToPA stream synchronization, and user-kernel TTD IOCTLs.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#pragma once

#include "BasicTypes.h"
#include "Constants.h"
#include "Ioctls.h"
#include "SnapshotFuzzing.h"

#ifndef DECLSPEC_ALIGN
#    if defined(_MSC_VER)
#        define DECLSPEC_ALIGN(x) __declspec(align(x))
#    elif defined(__GNUC__) || defined(__clang__)
#        define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
#    else
#        define DECLSPEC_ALIGN(x)
#    endif
#endif

//////////////////////////////////////////////////
//                  Constants                   //
//////////////////////////////////////////////////

#define TTD_PML_ENTRIES_PER_EXIT         512ULL
#define TTD_MAX_CHECKPOINTS_IN_RING      16ULL
#define TTD_MAX_PAGE_DIFFS_PER_INTERVAL  4096ULL
#define TTD_TIMELINE_WINDOW_SIZE         5000ULL
#define TTD_DEFAULT_INTERVAL_CYCLES      500000ULL
#define TTD_HTTR_MAGIC                   0x5254544867624448ULL /* "HDbgTTR" */

//
// TTD Session Execution Status Codes
//
#define TTD_STATUS_SUCCESS               0x00000000
#define TTD_STATUS_ALREADY_RECORDING     0x00000001
#define TTD_STATUS_NOT_RECORDING         0x00000002
#define TTD_STATUS_BUFFER_EXHAUSTED      0x00000003
#define TTD_STATUS_CHECKPOINT_NOT_FOUND  0x00000004
#define TTD_STATUS_REPLAY_DIVERGENCE     0x00000005
#define TTD_STATUS_INVALID_PARAMETER     0x00000006

//
// TTD Target Scopes
//
#define TTD_SCOPE_USER_PROCESS           0x00000001
#define TTD_SCOPE_KERNEL_SYSTEM          0x00000002

//////////////////////////////////////////////////
//             Hardware Synchronization         //
//////////////////////////////////////////////////

/**
 * @brief Synchronization marker linking Intel PT ToPA offset to instruction count and checkpoint
 */
typedef struct _TTD_SYNC_MARKER {
    UINT32 CheckpointId;
    UINT64 InstructionCount;       /* Monotonic value from IA32_FIXED_CTR0 (INST_RETIRED.ANY) */
    UINT64 GuestTsc;               /* Virtualized synthetic guest TSC                        */
    UINT64 PtByteOffset;           /* Byte offset in continuous PT circular ToPA buffer      */
    UINT64 Cr3;                    /* Guest page directory base address                      */
    UINT64 Rip;                    /* Guest instruction pointer at marker                    */

} TTD_SYNC_MARKER, *PTTD_SYNC_MARKER;

/**
 * @brief Represents a single 4KB pristine physical frame undo record in the CoW chain
 */
typedef struct _TTD_PML_UNDO_PAGE_RECORD {
    UINT64   PhysicalAddress;          /* Aligned 4KB Guest Physical Address (GPA)           */
    PVOID    PristineShadowVa;         /* Kernel VA containing the pristine 4KB backup copy  */
    UINT64   PtStreamByteOffset;       /* MSR 0x561 ToPA byte offset when write was logged   */
    UINT64   InstructionRetirement;    /* IA32_FIXED_CTR0 count when write was logged        */
    UINT64   GuestTsc;                 /* Virtualized guest TSC timestamp                    */

} TTD_PML_UNDO_PAGE_RECORD, *PTTD_PML_UNDO_PAGE_RECORD;

/**
 * @brief An incremental checkpoint containing vCPU context and its PML delta undo list
 */
typedef struct _TTD_INCREMENTAL_CHECKPOINT {
    UINT32                     CheckpointId;
    TTD_SYNC_MARKER            Sync;
    SNAPSHOT_VCPU_CONTEXT      VcpuContext;        /* Architectural registers & XSAVE AVX-512 */
    UINT32                     UndoPageCount;
    TTD_PML_UNDO_PAGE_RECORD   UndoPages[TTD_MAX_PAGE_DIFFS_PER_INTERVAL];

} TTD_INCREMENTAL_CHECKPOINT, *PTTD_INCREMENTAL_CHECKPOINT;

/**
 * @brief Global Manager for PT-PML Incremental CoW Tracking
 */
typedef struct _TTD_PML_COW_TRACKER {
    BOOLEAN                    Active;
    UINT32                     TotalCheckpoints;
    UINT32                     ActiveCheckpointCount;
    UINT64                     TotalDirtyPagesTracked;
    TTD_INCREMENTAL_CHECKPOINT Ring[TTD_MAX_CHECKPOINTS_IN_RING];
    UINT32                     HeadIndex;
    UINT32                     TailIndex;

} TTD_PML_COW_TRACKER, *PTTD_PML_COW_TRACKER;

/**
 * @brief Session status structure returned to user space
 */
typedef struct _TTD_SESSION_STATUS {
    BOOLEAN                    IsRecording;
    BOOLEAN                    IsReplaying;
    UINT32                     TargetPid;
    UINT32                     TargetCore;
    UINT32                     TotalCheckpointsTaken;
    UINT32                     ActiveCheckpointCount;
    UINT64                     TotalDirtyPagesTracked;
    UINT64                     TotalPtBytesRecorded;
    UINT64                     TotalInstructionsRetired;
    UINT64                     CurrentReplaySequence;
    UINT64                     CurrentRip;

} TTD_SESSION_STATUS, *PTTD_SESSION_STATUS;

/**
 * @brief User-mode timeline cache frame representing a single retired instruction
 */
typedef struct _TTD_TIMELINE_FRAME {
    UINT64                     Seq;
    UINT64                     Rip;
    UINT8                      Length;
    UINT8                      Opcode[16];
    CHAR                       Disasm[64];
    GUEST_REGS                 Regs;
    UINT64                     Rflags;
    UINT64                     MemWriteAddr;       /* Virtual address written (0 if none)    */
    UINT64                     MemWriteOldVal;     /* Pre-write 8-byte memory content        */
    UINT64                     MemWriteNewVal;     /* Written 8-byte memory content          */
    UINT32                     MemWriteSize;       /* Bytes written                          */

} TTD_TIMELINE_FRAME, *PTTD_TIMELINE_FRAME;

//////////////////////////////////////////////////
//             IOCTL Request Packets            //
//////////////////////////////////////////////////

/**
 * @brief Request packet for IOCTL_TTD_START
 */
typedef struct _DEBUGGER_TTD_START_REQUEST {
    UINT32                     TargetPid;          /* PID to trace (0 for kernel/all)        */
    UINT32                     PinToCore;          /* Optional core affinity (0xFFFFFFFF=any)*/
    UINT64                     PtBufferSize;       /* Buffer size per core for ToPA ring     */
    UINT32                     MaxCheckpoints;     /* Ring depth (default 16)                */
    UINT64                     WatermarkDirtyPages;/* Dirty page threshold to trigger checkpoint*/
    UINT32                     KernelStatus;

} DEBUGGER_TTD_START_REQUEST, *PDEBUGGER_TTD_START_REQUEST;

/**
 * @brief Request packet for IOCTL_TTD_RESTORE_CHECKPOINT
 */
typedef struct _DEBUGGER_TTD_RESTORE_REQUEST {
    UINT32                     TargetCheckpointId; /* Checkpoint ID to revert to             */
    UINT64                     TargetInstructionCount;
    UINT64                     RestoredRip;
    UINT32                     RestoredPages;
    UINT32                     KernelStatus;

} DEBUGGER_TTD_RESTORE_REQUEST, *PDEBUGGER_TTD_RESTORE_REQUEST;

/**
 * @brief Request packet for IOCTL_TTD_FAST_FORWARD
 */
typedef struct _DEBUGGER_TTD_FAST_FORWARD_REQUEST {
    UINT32                     TargetCheckpointId;
    UINT64                     TargetInstructionCount;
    UINT64                     RestoredRip;
    UINT32                     KernelStatus;

} DEBUGGER_TTD_FAST_FORWARD_REQUEST, *PDEBUGGER_TTD_FAST_FORWARD_REQUEST;

/**
 * @brief Request packet for IOCTL_TTD_GET_TRACE_BUFFER
 */
typedef struct _DEBUGGER_TTD_GET_TRACE_REQUEST {
    UINT32                     CoreId;
    UINT64                     BufferSize;
    PVOID                      PacketBuffer;
    UINT64                     TransferredBytes;
    UINT32                     KernelStatus;

} DEBUGGER_TTD_GET_TRACE_REQUEST, *PDEBUGGER_TTD_GET_TRACE_REQUEST;

/**
 * @brief Request packet for IOCTL_TTD_TAKE_CHECKPOINT
 */
typedef struct _DEBUGGER_TTD_CHECKPOINT_REQUEST {
    UINT32                     CheckpointId;
    UINT64                     InstructionCount;
    UINT64                     PtByteOffset;
    UINT64                     Rip;
    UINT32                     KernelStatus;

} DEBUGGER_TTD_CHECKPOINT_REQUEST, *PDEBUGGER_TTD_CHECKPOINT_REQUEST;

/**
 * @brief Request packet for IOCTL_TTD_FIND_MEMORY_WRITE
 */
typedef struct _DEBUGGER_TTD_FIND_WRITE_REQUEST {
    UINT64                     TargetGuestAddress; /* Virtual or Physical address to watch   */
    UINT32                     AccessSize;         /* 1, 2, 4, 8 bytes                       */
    UINT64                     WritingInstructionRip; /* RIP that performed the write        */
    UINT64                     OldValue;           /* Value before write                     */
    UINT64                     NewValue;           /* Value after write                      */
    UINT64                     InstructionSequence;/* Instruction count when write occurred  */
    UINT32                     KernelStatus;

} DEBUGGER_TTD_FIND_WRITE_REQUEST, *PDEBUGGER_TTD_FIND_WRITE_REQUEST;
