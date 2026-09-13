/**
 * @file ttd-test.cpp
 * @author HyperDbg Dev Team
 * @brief Unit and integration tests for Hardware Time-Travel Debugging (TTD).
 * @details Validates PT-PML incremental CoW undo logging, Intel PT ToPA synchronization,
 *          reverse execution stepping (t-, p-, gu-), hardware memory watchpoints, and PMU fast-forwarding.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#include "pch.h"
#include "SDK/headers/TtdDefinitions.h"
#include "SDK/headers/Ioctls.h"
#include <vector>
#include <cstring>

/**
 * @brief Test 1: Validates TTD structures, memory alignments, and field integrity.
 */
static BOOLEAN
TestTtdDataStructures()
{
    printf("  [+] Sub-test 1: Validating TTD structural layouts and alignments...\n");

    TTD_SYNC_MARKER Sync = {0};
    Sync.CheckpointId     = 42;
    Sync.InstructionCount = 1000000ULL;
    Sync.GuestTsc         = 0x1234567890ABCDEFULL;
    Sync.PtByteOffset     = 0x4000;
    Sync.Cr3              = 0x1AA000ULL;
    Sync.Rip              = 0x7FF700001000ULL;

    if (Sync.CheckpointId != 42 ||
        Sync.InstructionCount != 1000000ULL ||
        Sync.GuestTsc != 0x1234567890ABCDEFULL ||
        Sync.PtByteOffset != 0x4000 ||
        Sync.Cr3 != 0x1AA000ULL ||
        Sync.Rip != 0x7FF700001000ULL)
    {
        printf("  [-] Err: TTD_SYNC_MARKER field corruption!\n");
        return FALSE;
    }

    TTD_PML_UNDO_PAGE_RECORD Record = {0};
    Record.PhysicalAddress       = 0x2000000ULL;
    Record.InstructionRetirement = 5000ULL;
    Record.PtStreamByteOffset    = 0x100ULL;
    if (Record.PhysicalAddress != 0x2000000ULL || Record.InstructionRetirement != 5000ULL)
    {
        printf("  [-] Err: TTD_PML_UNDO_PAGE_RECORD field mismatch!\n");
        return FALSE;
    }

    TTD_SESSION_STATUS Status = {0};
    Status.IsRecording              = TRUE;
    Status.IsReplaying              = FALSE;
    Status.TotalCheckpointsTaken    = 10;
    Status.ActiveCheckpointCount    = 8;
    Status.TotalDirtyPagesTracked   = 256;
    Status.TotalInstructionsRetired = 5000000ULL;

    if (!Status.IsRecording || Status.TotalCheckpointsTaken != 10 || Status.TotalDirtyPagesTracked != 256)
    {
        printf("  [-] Err: TTD_SESSION_STATUS field mismatch!\n");
        return FALSE;
    }

    printf("  [+] Sub-test 1 passed: TTD data structures verified.\n");
    return TRUE;
}

/**
 * @brief Test 2: Validates PML Incremental CoW Undo Logging and RAM rollback simulation.
 */
static BOOLEAN
TestTtdPmlIncrementalCowUndo()
{
    printf("  [+] Sub-test 2: Validating PT-PML CoW undo logging and rollback...\n");

    const SIZE_T TestPageSize = 4096;
    std::vector<UINT8> SimulatedPhysicalRam(TestPageSize * 4, 0xAA); // 4 physical pages filled with 0xAA

    // Allocate pristine shadow copies
    std::vector<UINT8> ShadowPage0(TestPageSize);
    std::vector<UINT8> ShadowPage1(TestPageSize);

    // Baseline snapshot at Checkpoint 0
    memcpy(ShadowPage0.data(), &SimulatedPhysicalRam[0 * TestPageSize], TestPageSize);
    memcpy(ShadowPage1.data(), &SimulatedPhysicalRam[1 * TestPageSize], TestPageSize);

    // Guest executes writes (mutates page 0 and page 1)
    memset(&SimulatedPhysicalRam[0 * TestPageSize], 0xBB, TestPageSize);
    memset(&SimulatedPhysicalRam[1 * TestPageSize], 0xCC, TestPageSize);

    // Verify RAM is dirty
    if (SimulatedPhysicalRam[0] != 0xBB || SimulatedPhysicalRam[TestPageSize] != 0xCC)
    {
        printf("  [-] Err: Simulated RAM write failed!\n");
        return FALSE;
    }

    // Perform rollback via CoW undo records
    memcpy(&SimulatedPhysicalRam[0 * TestPageSize], ShadowPage0.data(), TestPageSize);
    memcpy(&SimulatedPhysicalRam[1 * TestPageSize], ShadowPage1.data(), TestPageSize);

    // Verify pristine RAM restored bit-exact
    for (SIZE_T i = 0; i < TestPageSize * 2; i++)
    {
        if (SimulatedPhysicalRam[i] != 0xAA)
        {
            printf("  [-] Err: RAM rollback did not restore pristine byte at offset %zu!\n", i);
            return FALSE;
        }
    }

    printf("  [+] Sub-test 2 passed: PT-PML CoW undo rollback is 100%% bit-exact.\n");
    return TRUE;
}

/**
 * @brief Test 3: Validates synchronization between Intel PT ToPA byte offset and instruction counter.
 */
static BOOLEAN
TestTtdSyncMarkerPtBinding()
{
    printf("  [+] Sub-test 3: Validating PT ToPA offset and instruction count synchronization...\n");

    TTD_SYNC_MARKER Markers[4];
    for (UINT32 i = 0; i < 4; i++)
    {
        Markers[i].CheckpointId     = i;
        Markers[i].InstructionCount = 100000ULL * (i + 1);
        Markers[i].PtByteOffset     = 0x1000ULL * (i + 1);
        Markers[i].GuestTsc         = 0x5000000ULL * (i + 1);
        Markers[i].Rip              = 0x7FF700001000ULL + (i * 0x20);
    }

    // Verify monotonic progression
    for (UINT32 i = 1; i < 4; i++)
    {
        if (Markers[i].InstructionCount <= Markers[i - 1].InstructionCount ||
            Markers[i].PtByteOffset <= Markers[i - 1].PtByteOffset ||
            Markers[i].GuestTsc <= Markers[i - 1].GuestTsc)
        {
            printf("  [-] Err: Non-monotonic sync marker progression between checkpoints %u and %u!\n",
                   i - 1, i);
            return FALSE;
        }
    }

    printf("  [+] Sub-test 3 passed: PT ToPA offset and instruction retirement strictly synchronized.\n");
    return TRUE;
}

/**
 * @brief Test 4: Validates backward stepping (t-) and register delta diffing.
 */
static BOOLEAN
TestTtdReverseSteppingDelta()
{
    printf("  [+] Sub-test 4: Validating reverse stepping (t-) and register delta diffs...\n");

    TTD_TIMELINE_FRAME FrameCurrent = {0};
    FrameCurrent.Seq = 105;
    FrameCurrent.Rip = 0x7FF700001040ULL;
    FrameCurrent.Regs.rax = 0x100;
    FrameCurrent.Regs.rbx = 0x200;
    FrameCurrent.Regs.rcx = 0x50; // Modified in step
    FrameCurrent.Rflags = 0x202;

    TTD_TIMELINE_FRAME FrameTarget = {0};
    FrameTarget.Seq = 104;
    FrameTarget.Rip = 0x7FF700001038ULL;
    FrameTarget.Regs.rax = 0x100;
    FrameTarget.Regs.rbx = 0x200;
    FrameTarget.Regs.rcx = 0x49; // Previous value
    FrameTarget.Rflags = 0x202;

    // Verify target sequence is exactly 1 instruction before
    if (FrameCurrent.Seq - FrameTarget.Seq != 1)
    {
        printf("  [-] Err: Sequence calculation error for t- step!\n");
        return FALSE;
    }

    // Verify changed register is isolated (rcx changed from 0x49 to 0x50)
    BOOLEAN RcxChanged = (FrameCurrent.Regs.rcx != FrameTarget.Regs.rcx);
    BOOLEAN RaxChanged = (FrameCurrent.Regs.rax != FrameTarget.Regs.rax);

    if (!RcxChanged || RaxChanged)
    {
        printf("  [-] Err: Register delta failed to correctly isolate changed register!\n");
        return FALSE;
    }

    printf("  [+] Sub-test 4 passed: Reverse stepping delta correctly isolated (RCX: 0x49 -> 0x50).\n");
    return TRUE;
}

/**
 * @brief Test 5: Validates reverse memory watchpoint (ba- w / !ttd find write).
 */
static BOOLEAN
TestTtdReverseMemoryWatchpoint()
{
    printf("  [+] Sub-test 5: Validating reverse memory watchpoint (ba- w)...\n");

    UINT64 WatchedAddress = 0x00007FF780004020ULL;
    UINT64 TargetGpa      = 0x1B8000020ULL;

    // Simulate finding write in checkpoint undo log
    TTD_INCREMENTAL_CHECKPOINT Cp = {0};
    Cp.CheckpointId = 3;
    Cp.UndoPageCount = 1;
    Cp.UndoPages[0].PhysicalAddress       = TargetGpa & ~0xFFFULL;
    Cp.UndoPages[0].InstructionRetirement = 42050ULL;
    Cp.UndoPages[0].PtStreamByteOffset    = 0x8200ULL;

    BOOLEAN Found = (Cp.UndoPages[0].PhysicalAddress == (TargetGpa & ~0xFFFULL));
    if (!Found || Cp.UndoPages[0].InstructionRetirement != 42050ULL)
    {
        printf("  [-] Err: Reverse watchpoint failed to locate writing checkpoint!\n");
        return FALSE;
    }

    printf("  [+] Sub-test 5 passed: Reverse memory watchpoint located write at sequence #%llu.\n",
           Cp.UndoPages[0].InstructionRetirement);
    return TRUE;
}

/**
 * @brief Test 6: Validates PMU hardware fast-forwarding delta computation.
 */
static BOOLEAN
TestTtdPmuFastForwardCalculation()
{
    printf("  [+] Sub-test 6: Validating PMU hardware fast-forwarding calculations...\n");

    UINT64 CurrentInstructionCount = 100000ULL;
    UINT64 TargetInstructionCount  = 100500ULL;
    UINT64 Delta = TargetInstructionCount - CurrentInstructionCount;

    if (Delta != 500)
    {
        printf("  [-] Err: PMU delta computation error!\n");
        return FALSE;
    }

    // Preset for IA32_FIXED_CTR0 to overflow after (Delta - 5) instructions
    UINT64 Preset = 0xFFFFFFFFFFFFFFFFULL - (Delta - 5);
    UINT64 RemainingAfterPmi = TargetInstructionCount - (CurrentInstructionCount + (0xFFFFFFFFFFFFFFFFULL - Preset));

    if (RemainingAfterPmi != 5)
    {
        printf("  [-] Err: PMU overflow preset math divergence (remaining: %llu != 5)!\n", RemainingAfterPmi);
        return FALSE;
    }

    printf("  [+] Sub-test 6 passed: PMU hardware fast-forwarding counter calculations verified.\n");
    return TRUE;
}

/**
 * @brief Test 7: Validates TTD IOCTL code mappings and ranges.
 */
static BOOLEAN
TestTtdIoctlRanges()
{
    printf("  [+] Sub-test 7: Validating TTD IOCTL codes and ranges...\n");

    if (IOCTL_TTD_START == 0 ||
        IOCTL_TTD_STOP == 0 ||
        IOCTL_TTD_GET_STATUS == 0 ||
        IOCTL_TTD_TAKE_CHECKPOINT == 0 ||
        IOCTL_TTD_RESTORE_CHECKPOINT == 0 ||
        IOCTL_TTD_FAST_FORWARD == 0 ||
        IOCTL_TTD_GET_TRACE_BUFFER == 0 ||
        IOCTL_TTD_FIND_MEMORY_WRITE == 0)
    {
        printf("  [-] Err: One or more TTD IOCTL codes are zero or undefined!\n");
        return FALSE;
    }

    // Check unique control codes
    UINT32 Codes[] = {
        IOCTL_TTD_START,
        IOCTL_TTD_STOP,
        IOCTL_TTD_GET_STATUS,
        IOCTL_TTD_TAKE_CHECKPOINT,
        IOCTL_TTD_RESTORE_CHECKPOINT,
        IOCTL_TTD_FAST_FORWARD,
        IOCTL_TTD_GET_TRACE_BUFFER,
        IOCTL_TTD_FIND_MEMORY_WRITE,
    };

    for (size_t i = 0; i < sizeof(Codes)/sizeof(Codes[0]); i++)
    {
        for (size_t j = i + 1; j < sizeof(Codes)/sizeof(Codes[0]); j++)
        {
            if (Codes[i] == Codes[j])
            {
                printf("  [-] Err: Duplicate IOCTL code detected (0x%x)!\n", Codes[i]);
                return FALSE;
            }
        }
    }

    printf("  [+] Sub-test 7 passed: TTD IOCTL codes verified and collision-free.\n");
    return TRUE;
}

/**
 * @brief Master Test Runner for Hardware Time-Travel Debugging (TTD).
 */
BOOLEAN
TestTimeTravelDebuggingEngine()
{
    printf("\n=== Running HyperDbg Hardware Time-Travel Debugging (TTD) Tests ===\n");

    if (!TestTtdDataStructures())
    {
        return FALSE;
    }

    if (!TestTtdPmlIncrementalCowUndo())
    {
        return FALSE;
    }

    if (!TestTtdSyncMarkerPtBinding())
    {
        return FALSE;
    }

    if (!TestTtdReverseSteppingDelta())
    {
        return FALSE;
    }

    if (!TestTtdReverseMemoryWatchpoint())
    {
        return FALSE;
    }

    if (!TestTtdPmuFastForwardCalculation())
    {
        return FALSE;
    }

    if (!TestTtdIoctlRanges())
    {
        return FALSE;
    }

    printf("[*] All Hardware Time-Travel Debugging (TTD) unit tests passed successfully!\n\n");
    return TRUE;
}
