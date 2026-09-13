/**
 * @file TtdEngine.c
 * @author HyperDbg Dev Team
 * @brief In-kernel hypervisor Hardware Time-Travel Debugging (TTD) engine.
 * @details Implements PT-PML incremental CoW undo logging, Intel PT ToPA streaming,
 *          hardware PMU counter fast-forwarding, and reverse execution state reconstruction.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#include "pch.h"
#include "features/TtdEngine.h"
#include "features/DirtyLogging.h"
#include "features/SnapshotFuzzing.h"
#include "SDK/headers/PtDefinitions.h"

//////////////////////////////////////////////////
//              Global Engine State             //
//////////////////////////////////////////////////

static TTD_PML_COW_TRACKER g_TtdTracker                             = {0};
static BOOLEAN             g_TtdInitialized                         = FALSE;
static UINT32              g_TtdTargetPid                           = 0;
static UINT32              g_TtdTargetCore                          = 0xFFFFFFFF;
static UINT64              g_TtdPtBufferSize                        = 4 * 1024 * 1024; /* 4 MB */
#define TTD_MAX_PROCESSOR_COUNT 256
static PVOID               g_TtdPtBuffers[TTD_MAX_PROCESSOR_COUNT]        = {0};
static UINT64              g_TtdPtBufferPhysical[TTD_MAX_PROCESSOR_COUNT] = {0};
static BOOLEAN             g_TtdReplayMode                          = FALSE;
static UINT64              g_TtdCurrentReplaySeq                    = 0;

//////////////////////////////////////////////////
//            Initialization & Teardown         //
//////////////////////////////////////////////////

/**
 * @brief Initializes the Hardware Time-Travel Debugging engine.
 *
 * @return BOOLEAN TRUE on success, FALSE otherwise.
 */
BOOLEAN
TtdEngineInitialize()
{
    if (g_TtdInitialized)
    {
        return TRUE;
    }

    RtlZeroMemory(&g_TtdTracker, sizeof(TTD_PML_COW_TRACKER));
    g_TtdTracker.Active                = FALSE;
    g_TtdTracker.TotalCheckpoints      = 0;
    g_TtdTracker.ActiveCheckpointCount = 0;
    g_TtdTracker.HeadIndex             = 0;
    g_TtdTracker.TailIndex             = 0;
    g_TtdReplayMode                    = FALSE;
    g_TtdCurrentReplaySeq              = 0;

    g_TtdInitialized = TRUE;
    LogInfo("TTD Engine initialized successfully");
    return TRUE;
}

/**
 * @brief Uninitializes the Hardware Time-Travel Debugging engine and deallocates buffers.
 */
VOID
TtdEngineUninitialize()
{
    if (!g_TtdInitialized)
    {
        return;
    }

    //
    // Stop recording if active
    //
    if (g_TtdTracker.Active)
    {
        UINT32 Status = 0;
        TtdEngineStop(&Status);
    }

    //
    // Free all undo pages in the checkpoint ring
    //
    for (UINT32 i = 0; i < TTD_MAX_CHECKPOINTS_IN_RING; i++)
    {
        PTTD_INCREMENTAL_CHECKPOINT Cp = &g_TtdTracker.Ring[i];
        for (UINT32 j = 0; j < Cp->UndoPageCount; j++)
        {
            if (Cp->UndoPages[j].PristineShadowVa != NULL)
            {
                PlatformMemFreePool(Cp->UndoPages[j].PristineShadowVa);
                Cp->UndoPages[j].PristineShadowVa = NULL;
            }
        }
        Cp->UndoPageCount = 0;
    }

    //
    // Free per-core Intel PT buffers
    //
    ULONG ProcessorsCount = KeQueryActiveProcessorCount(0);
    for (ULONG Core = 0; Core < ProcessorsCount; Core++)
    {
        if (g_TtdPtBuffers[Core] != NULL)
        {
            PlatformMemFreePool(g_TtdPtBuffers[Core]);
            g_TtdPtBuffers[Core]        = NULL;
            g_TtdPtBufferPhysical[Core] = 0;
        }
    }

    RtlZeroMemory(&g_TtdTracker, sizeof(TTD_PML_COW_TRACKER));
    g_TtdInitialized = FALSE;
    LogInfo("TTD Engine uninitialized successfully");
}

/**
 * @brief Returns TRUE if TTD is actively recording.
 */
BOOLEAN
TtdEngineIsRecording(VOID)
{
    return g_TtdTracker.Active;
}

/**
 * @brief Returns TRUE if TTD is currently in replay/reverse execution mode.
 */
BOOLEAN
TtdEngineIsReplaying(VOID)
{
    return g_TtdReplayMode;
}

//////////////////////////////////////////////////
//            Recording Control APIs            //
//////////////////////////////////////////////////

/**
 * @brief Starts Hardware Time-Travel Debugging recording session.
 *
 * @param Request Start request configuration.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineStart(PDEBUGGER_TTD_START_REQUEST Request)
{
    if (!Request)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!g_CompatibilityCheck.PmlSupport)
    {
        LogError("Err, hardware PML is not supported on this processor for TTD");
        Request->KernelStatus = TTD_STATUS_INVALID_PARAMETER;
        return STATUS_NOT_SUPPORTED;
    }

    if (g_TtdTracker.Active)
    {
        Request->KernelStatus = TTD_STATUS_ALREADY_RECORDING;
        return STATUS_ALREADY_COMMITTED;
    }

    g_TtdTargetPid  = Request->TargetPid;
    g_TtdTargetCore = Request->PinToCore;
    if (Request->PtBufferSize >= 64 * 1024 && Request->PtBufferSize <= 128 * 1024 * 1024)
    {
        g_TtdPtBufferSize = Request->PtBufferSize;
    }

    ULONG ProcessorsCount = KeQueryActiveProcessorCount(0);

    //
    // Allocate per-core Intel PT ToPA buffers if not already allocated
    //
    for (ULONG Core = 0; Core < ProcessorsCount; Core++)
    {
        if (g_TtdPtBuffers[Core] == NULL)
        {
            g_TtdPtBuffers[Core] = PlatformMemAllocateNonPagedPool((SIZE_T)g_TtdPtBufferSize);
            if (g_TtdPtBuffers[Core] == NULL)
            {
                LogError("Err, failed to allocate PT ToPA buffer for Core %u", Core);
                Request->KernelStatus = TTD_STATUS_BUFFER_EXHAUSTED;
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            RtlZeroBytes(g_TtdPtBuffers[Core], (SIZE_T)g_TtdPtBufferSize);
            g_TtdPtBufferPhysical[Core] = VirtualAddressToPhysicalAddress(g_TtdPtBuffers[Core]);
        }
    }

    //
    // Enable dirty logging (PML) across all cores
    //
    if (!DirtyLoggingInitialize())
    {
        LogError("Err, failed to initialize PML dirty logging for TTD");
        Request->KernelStatus = TTD_STATUS_INVALID_PARAMETER;
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Configure Intel PT MSRs on target or all cores
    //
    for (ULONG Core = 0; Core < ProcessorsCount; Core++)
    {
        if (g_TtdTargetCore != 0xFFFFFFFF && g_TtdTargetCore != Core)
        {
            continue;
        }

        //
        // Reset PT MSRs and configure circular buffer
        //
        CpuWriteMsr(MSR_IA32_RTIT_CTL, 0);
        CpuWriteMsr(MSR_IA32_RTIT_STATUS, 0);
        CpuWriteMsr(MSR_IA32_RTIT_OUTPUT_BASE, g_TtdPtBufferPhysical[Core]);
        CpuWriteMsr(MSR_IA32_RTIT_OUTPUT_MASK_PTRS, 0);

        //
        // Enable Intel PT tracing: User/Kernel depending on PID, BranchEn = 1
        // Bit 0: TraceEn, Bit 2: OS, Bit 3: User, Bit 13: BranchEn
        //
        UINT64 RtitCtl = (1ULL << 0) | (1ULL << 13);
        if (g_TtdTargetPid == 0)
        {
            RtitCtl |= (1ULL << 2) | (1ULL << 3); /* Trace both OS and User */
        }
        else
        {
            RtitCtl |= (1ULL << 3); /* Trace User mode */
        }

        CpuWriteMsr(MSR_IA32_RTIT_CTL, RtitCtl);
    }

    //
    // Arm Hardware PMU Fixed Counter 0 (INST_RETIRED.ANY)
    //
    // Enable CTR0 in IA32_FIXED_CTR_CTRL (Bits 0-3: 0x3 = count OS + Usr)
    UINT64 FixedCtrCtrl = CpuReadMsr(IA32_FIXED_CTR_CTRL);
    FixedCtrCtrl |= 0x3ULL;
    CpuWriteMsr(IA32_FIXED_CTR_CTRL, FixedCtrCtrl);

    // Enable in IA32_PERF_GLOBAL_CTRL (Bit 32: Fixed Ctr 0)
    UINT64 GlobalCtrl = CpuReadMsr(IA32_PERF_GLOBAL_CTRL);
    GlobalCtrl |= (1ULL << 32);
    CpuWriteMsr(IA32_PERF_GLOBAL_CTRL, GlobalCtrl);

    //
    // Reset tracker state
    //
    g_TtdTracker.Active                = TRUE;
    g_TtdTracker.TotalCheckpoints      = 0;
    g_TtdTracker.ActiveCheckpointCount = 0;
    g_TtdTracker.HeadIndex             = 0;
    g_TtdTracker.TailIndex             = 0;
    g_TtdTracker.TotalDirtyPagesTracked = 0;
    g_TtdReplayMode                    = FALSE;

    //
    // Take baseline Checkpoint 0
    //
    DEBUGGER_TTD_CHECKPOINT_REQUEST BaselineReq = {0};
    TtdEngineTakeCheckpoint(&BaselineReq);

    Request->KernelStatus = TTD_STATUS_SUCCESS;
    LogInfo("TTD recording session started successfully (TargetPid: %u)", g_TtdTargetPid);
    return STATUS_SUCCESS;
}

/**
 * @brief Stops active Hardware Time-Travel Debugging session.
 *
 * @param Status Pointer to receive status code.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineStop(PUINT32 Status)
{
    if (!g_TtdTracker.Active)
    {
        if (Status) *Status = TTD_STATUS_NOT_RECORDING;
        return STATUS_SUCCESS;
    }

    //
    // Disable Intel PT tracing on all cores
    //
    ULONG ProcessorsCount = KeQueryActiveProcessorCount(0);
    for (ULONG Core = 0; Core < ProcessorsCount; Core++)
    {
        CpuWriteMsr(MSR_IA32_RTIT_CTL, 0);
    }

    //
    // Flush pending PML buffers and teardown dirty logging
    //
    DirtyLoggingUninitialize();

    g_TtdTracker.Active = FALSE;
    if (Status) *Status = TTD_STATUS_SUCCESS;
    LogInfo("TTD recording session stopped (Total checkpoints: %u, dirty pages: %llu)",
            g_TtdTracker.TotalCheckpoints, g_TtdTracker.TotalDirtyPagesTracked);
    return STATUS_SUCCESS;
}

/**
 * @brief Queries current TTD recording/replaying session metrics.
 *
 * @param Status Pointer to receive session metrics.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineGetStatus(PTTD_SESSION_STATUS Status)
{
    if (!Status)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status->IsRecording              = g_TtdTracker.Active;
    Status->IsReplaying              = g_TtdReplayMode;
    Status->TargetPid                = g_TtdTargetPid;
    Status->TargetCore               = g_TtdTargetCore;
    Status->TotalCheckpointsTaken    = g_TtdTracker.TotalCheckpoints;
    Status->ActiveCheckpointCount    = g_TtdTracker.ActiveCheckpointCount;
    Status->TotalDirtyPagesTracked   = g_TtdTracker.TotalDirtyPagesTracked;
    Status->TotalPtBytesRecorded     = CpuReadMsr(MSR_IA32_RTIT_OUTPUT_MASK_PTRS);
    Status->TotalInstructionsRetired = CpuReadMsr(IA32_FIXED_CTR0);
    Status->CurrentReplaySequence    = g_TtdCurrentReplaySeq;
    Status->CurrentRip               = GetGuestRIP();

    return STATUS_SUCCESS;
}

//////////////////////////////////////////////////
//       PT-PML Incremental CoW Logging         //
//////////////////////////////////////////////////

/**
 * @brief Explicitly creates an incremental checkpoint synchronized with PT and PML.
 *
 * @param Request Checkpoint request structure.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineTakeCheckpoint(PDEBUGGER_TTD_CHECKPOINT_REQUEST Request)
{
    VIRTUAL_MACHINE_STATE * VCpu = &g_GuestState[KeGetCurrentProcessorNumber()];

    UINT32 NewHead = (g_TtdTracker.HeadIndex + 1) % TTD_MAX_CHECKPOINTS_IN_RING;

    //
    // If ring is full, advance tail and free old checkpoint's undo pages
    //
    if (g_TtdTracker.ActiveCheckpointCount >= TTD_MAX_CHECKPOINTS_IN_RING)
    {
        PTTD_INCREMENTAL_CHECKPOINT Evicted = &g_TtdTracker.Ring[g_TtdTracker.TailIndex];
        for (UINT32 j = 0; j < Evicted->UndoPageCount; j++)
        {
            if (Evicted->UndoPages[j].PristineShadowVa != NULL)
            {
                PlatformMemFreePool(Evicted->UndoPages[j].PristineShadowVa);
                Evicted->UndoPages[j].PristineShadowVa = NULL;
            }
        }
        Evicted->UndoPageCount  = 0;
        g_TtdTracker.TailIndex = (g_TtdTracker.TailIndex + 1) % TTD_MAX_CHECKPOINTS_IN_RING;
    }
    else
    {
        g_TtdTracker.ActiveCheckpointCount++;
    }

    g_TtdTracker.HeadIndex = NewHead;
    PTTD_INCREMENTAL_CHECKPOINT Cp = &g_TtdTracker.Ring[NewHead];

    Cp->CheckpointId  = g_TtdTracker.TotalCheckpoints++;
    Cp->UndoPageCount = 0;

    //
    // Synchronize PT byte offset, instruction count, TSC, CR3, and RIP
    //
    Cp->Sync.CheckpointId     = Cp->CheckpointId;
    Cp->Sync.InstructionCount = CpuReadMsr(IA32_FIXED_CTR0);
    Cp->Sync.GuestTsc         = SnapshotGetVirtualizedTsc(VCpu);
    Cp->Sync.PtByteOffset     = CpuReadMsr(MSR_IA32_RTIT_OUTPUT_MASK_PTRS);
    Cp->Sync.Cr3              = GetGuestCr3();
    Cp->Sync.Rip              = GetGuestRIP();

    //
    // Capture complete architectural vCPU context & XSAVE area
    //
    SnapshotSaveVcpuContext(VCpu, &Cp->VcpuContext);

    if (Request)
    {
        Request->CheckpointId     = Cp->CheckpointId;
        Request->InstructionCount = Cp->Sync.InstructionCount;
        Request->PtByteOffset     = Cp->Sync.PtByteOffset;
        Request->Rip              = Cp->Sync.Rip;
        Request->KernelStatus     = TTD_STATUS_SUCCESS;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Records a pristine shadow copy of a physical 4KB frame prior to modification.
 * @details Called from DirtyLoggingFlushPmlBuffer before clearing the EPT Dirty bit.
 *
 * @param VCpu Current virtual machine processor state.
 * @param AccessedPhysAddr Guest physical address reported by hardware PML.
 */
VOID
TtdEngineRecordPmlPageDiff(VIRTUAL_MACHINE_STATE * VCpu, UINT64 AccessedPhysAddr)
{
    if (!g_TtdTracker.Active)
    {
        return;
    }

    UINT64 AlignedGpa = AccessedPhysAddr & ~0xFFFULL;
    PTTD_INCREMENTAL_CHECKPOINT ActiveCp = &g_TtdTracker.Ring[g_TtdTracker.HeadIndex];

    //
    // 1. Check if GPA is already recorded in the active checkpoint interval (deduplication)
    //
    for (UINT32 i = 0; i < ActiveCp->UndoPageCount; i++)
    {
        if (ActiveCp->UndoPages[i].PhysicalAddress == AlignedGpa)
        {
            return;
        }
    }

    //
    // 2. Check capacity
    //
    if (ActiveCp->UndoPageCount >= TTD_MAX_PAGE_DIFFS_PER_INTERVAL)
    {
        LogWarning("Warn, checkpoint %u reached max undo page limit (%llu)",
                   ActiveCp->CheckpointId, TTD_MAX_PAGE_DIFFS_PER_INTERVAL);
        return;
    }

    //
    // 3. Allocate pristine 4KB shadow backup frame
    //
    PVOID ShadowFrame = PlatformMemAllocateNonPagedPool(PAGE_SIZE);
    if (ShadowFrame == NULL)
    {
        LogError("Err, failed to allocate shadow frame for GPA: 0x%llx", AlignedGpa);
        return;
    }

    //
    // 4. Map physical frame to virtual address and copy pristine bytes
    //
    PVOID SourceVa = (PVOID)PhysicalAddressToVirtualAddress(AlignedGpa);
    if (SourceVa == NULL)
    {
        PlatformMemFreePool(ShadowFrame);
        return;
    }

    RtlCopyMemory(ShadowFrame, SourceVa, PAGE_SIZE);

    //
    // 5. Store undo record with hardware synchronization telemetry
    //
    UINT32 Index = ActiveCp->UndoPageCount++;
    ActiveCp->UndoPages[Index].PhysicalAddress       = AlignedGpa;
    ActiveCp->UndoPages[Index].PristineShadowVa      = ShadowFrame;
    ActiveCp->UndoPages[Index].PtStreamByteOffset    = CpuReadMsr(MSR_IA32_RTIT_OUTPUT_MASK_PTRS);
    ActiveCp->UndoPages[Index].InstructionRetirement = CpuReadMsr(IA32_FIXED_CTR0);
    ActiveCp->UndoPages[Index].GuestTsc              = SnapshotGetVirtualizedTsc(VCpu);

    g_TtdTracker.TotalDirtyPagesTracked++;
}

//////////////////////////////////////////////////
//       Reverse Execution & State Revert       //
//////////////////////////////////////////////////

/**
 * @brief Rolls back physical RAM and architectural vCPU context to target checkpoint.
 *
 * @param Request Restore request configuration.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineRestoreCheckpoint(PDEBUGGER_TTD_RESTORE_REQUEST Request)
{
    if (!Request)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIRTUAL_MACHINE_STATE * VCpu = &g_GuestState[KeGetCurrentProcessorNumber()];
    UINT32 TargetId              = Request->TargetCheckpointId;

    //
    // 1. Locate target checkpoint in ring
    //
    PTTD_INCREMENTAL_CHECKPOINT TargetCp = NULL;
    INT32 TargetIndex = -1;

    for (UINT32 i = 0; i < TTD_MAX_CHECKPOINTS_IN_RING; i++)
    {
        if (g_TtdTracker.Ring[i].CheckpointId == TargetId)
        {
            TargetCp    = &g_TtdTracker.Ring[i];
            TargetIndex = (INT32)i;
            break;
        }
    }

    if (TargetCp == NULL)
    {
        LogError("Err, target checkpoint ID %u not found in active ring", TargetId);
        Request->KernelStatus = TTD_STATUS_CHECKPOINT_NOT_FOUND;
        return STATUS_NOT_FOUND;
    }

    //
    // 2. Roll back physical RAM by walking backwards from current head to target
    //
    UINT32 RestoredPageCount = 0;
    INT32 Curr = (INT32)g_TtdTracker.HeadIndex;

    while (TRUE)
    {
        PTTD_INCREMENTAL_CHECKPOINT Cp = &g_TtdTracker.Ring[Curr];

        //
        // Walk undo pages in reverse chronological order
        //
        for (INT32 p = (INT32)Cp->UndoPageCount - 1; p >= 0; p--)
        {
            PTTD_PML_UNDO_PAGE_RECORD Rec = &Cp->UndoPages[p];
            if (Rec->PristineShadowVa != NULL)
            {
                PVOID TargetVa = (PVOID)PhysicalAddressToVirtualAddress(Rec->PhysicalAddress);
                if (TargetVa != NULL)
                {
                    RtlCopyMemory(TargetVa, Rec->PristineShadowVa, PAGE_SIZE);
                    RestoredPageCount++;
                }
            }
        }

        if (Curr == TargetIndex)
        {
            break;
        }

        Curr = (Curr - 1 + TTD_MAX_CHECKPOINTS_IN_RING) % TTD_MAX_CHECKPOINTS_IN_RING;
    }

    //
    // 3. Restore architectural vCPU registers, MSRs, and XSAVE area
    //
    SnapshotRestoreVcpuContext(VCpu, &TargetCp->VcpuContext);

    //
    // 4. Invalidate EPT / TLB mappings to ensure CPU caches see restored memory
    //
    EptInveptSingleContext(VCpu->EptPointer.AsUInt);
    VpidInvvpidAllContext();

    //
    // 5. Update session state
    //
    g_TtdTracker.HeadIndex = (UINT32)TargetIndex;
    g_TtdReplayMode        = TRUE;
    g_TtdCurrentReplaySeq  = TargetCp->Sync.InstructionCount;

    Request->RestoredRip   = TargetCp->Sync.Rip;
    Request->RestoredPages = RestoredPageCount;
    Request->KernelStatus  = TTD_STATUS_SUCCESS;

    LogInfo("Restored checkpoint %u (Pages reverted: %u, RIP: 0x%llx)",
            TargetId, RestoredPageCount, TargetCp->Sync.Rip);
    return STATUS_SUCCESS;
}

/**
 * @brief Fast-forwards physical CPU execution to target instruction count via PMU.
 *
 * @param Request Fast forward request.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineFastForward(PDEBUGGER_TTD_FAST_FORWARD_REQUEST Request)
{
    if (!Request)
    {
        return STATUS_INVALID_PARAMETER;
    }

    //
    // 1. First restore baseline checkpoint
    //
    DEBUGGER_TTD_RESTORE_REQUEST RestoreReq = {0};
    RestoreReq.TargetCheckpointId = Request->TargetCheckpointId;
    NTSTATUS Status = TtdEngineRestoreCheckpoint(&RestoreReq);
    if (!NT_SUCCESS(Status))
    {
        Request->KernelStatus = RestoreReq.KernelStatus;
        return Status;
    }

    UINT64 CurrentInst = RestoreReq.RestoredRip; /* Baseline instruction count */
    UINT64 TargetInst  = Request->TargetInstructionCount;

    if (TargetInst <= CurrentInst)
    {
        Request->RestoredRip  = GetGuestRIP();
        Request->KernelStatus = TTD_STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }

    UINT64 Delta = TargetInst - CurrentInst;

    //
    // If delta > 10, program PMU fixed counter to overflow near target
    //
    if (Delta > 10)
    {
        // Program Fixed Ctr 0 to trigger PMI on overflow
        UINT64 CtrPreset = 0xFFFFFFFFFFFFFFFFULL - (Delta - 5);
        CpuWriteMsr(IA32_FIXED_CTR0, CtrPreset);
    }
    else
    {
        // For fine-grained stepping, enable hardware Monitor Trap Flag (MTF)
        VmFuncSetMonitorTrapFlag(TRUE);
    }

    g_TtdCurrentReplaySeq = TargetInst;
    Request->RestoredRip  = GetGuestRIP();
    Request->KernelStatus = TTD_STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/**
 * @brief Fetches raw Intel PT ToPA packet trace buffer.
 *
 * @param Request Trace retrieval request.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineGetTraceBuffer(PDEBUGGER_TTD_GET_TRACE_REQUEST Request)
{
    if (!Request || !Request->PacketBuffer)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG Core = Request->CoreId;
    if (Core >= KeQueryActiveProcessorCount(0) || g_TtdPtBuffers[Core] == NULL)
    {
        Request->KernelStatus = TTD_STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    UINT64 WrittenBytes = CpuReadMsr(MSR_IA32_RTIT_OUTPUT_MASK_PTRS) & 0xFFFFFFFFULL;
    UINT64 CopyBytes    = min(Request->BufferSize, WrittenBytes);

    if (CopyBytes > 0)
    {
        RtlCopyMemory(Request->PacketBuffer, g_TtdPtBuffers[Core], (SIZE_T)CopyBytes);
    }

    Request->TransferredBytes = CopyBytes;
    Request->KernelStatus     = TTD_STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

/**
 * @brief Reverse memory watchpoint: finds which instruction wrote to target address.
 *
 * @param Request Find write request.
 * @return NTSTATUS
 */
IMPORT_EXPORT_VMM NTSTATUS
TtdEngineFindMemoryWrite(PDEBUGGER_TTD_FIND_WRITE_REQUEST Request)
{
    if (!Request)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIRTUAL_MACHINE_STATE * VCpu = &g_GuestState[KeGetCurrentProcessorNumber()];
    UINT64 TargetGpa = Request->TargetGuestAddress;

    //
    // Convert to GPA if given virtual address
    //
    if (TargetGpa > 0xFFFF800000000000ULL || (g_TtdTargetPid != 0 && TargetGpa < 0x00007FFFFFFFFFFFULL))
    {
        TargetGpa = VirtualAddressToPhysicalAddress((PVOID)TargetGpa);
    }

    UINT64 AlignedGpa = TargetGpa & ~0xFFFULL;

    //
    // 1. Search backwards in PML CoW undo records for the most recent write to this GPA
    //
    INT32 Curr = (INT32)g_TtdTracker.HeadIndex;
    BOOLEAN Found = FALSE;

    for (UINT32 k = 0; k < g_TtdTracker.ActiveCheckpointCount; k++)
    {
        PTTD_INCREMENTAL_CHECKPOINT Cp = &g_TtdTracker.Ring[Curr];

        for (INT32 p = (INT32)Cp->UndoPageCount - 1; p >= 0; p--)
        {
            if (Cp->UndoPages[p].PhysicalAddress == AlignedGpa)
            {
                //
                // Found the checkpoint where this page was modified!
                // Revert to the checkpoint prior to the write
                //
                DEBUGGER_TTD_RESTORE_REQUEST RestoreReq = {0};
                RestoreReq.TargetCheckpointId = Cp->CheckpointId;
                TtdEngineRestoreCheckpoint(&RestoreReq);

                //
                // Clear WriteAccess in EPT PML1 to trigger hardware EPT Violation VM-exit on write
                //
                BOOLEAN IsLargePage;
                PVOID PmlEntry = EptGetPml1OrPml2Entry(VCpu->EptPageTable, AlignedGpa, &IsLargePage);
                if (PmlEntry != NULL && !IsLargePage)
                {
                    ((PEPT_PML1_ENTRY)PmlEntry)->WriteAccess = FALSE;
                    EptInveptSingleContext(VCpu->EptPointer.AsUInt);
                }

                Request->WritingInstructionRip = Cp->UndoPages[p].PhysicalAddress; // Will be refined on EPT exit
                Request->InstructionSequence   = Cp->UndoPages[p].InstructionRetirement;
                Request->KernelStatus          = TTD_STATUS_SUCCESS;
                Found                          = TRUE;
                break;
            }
        }

        if (Found) break;
        Curr = (Curr - 1 + TTD_MAX_CHECKPOINTS_IN_RING) % TTD_MAX_CHECKPOINTS_IN_RING;
    }

    if (!Found)
    {
        Request->KernelStatus = TTD_STATUS_CHECKPOINT_NOT_FOUND;
        return STATUS_NOT_FOUND;
    }

    return STATUS_SUCCESS;
}
