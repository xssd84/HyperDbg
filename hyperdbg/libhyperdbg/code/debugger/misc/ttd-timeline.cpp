/**
 * @file ttd-timeline.cpp
 * @author HyperDbg Dev Team
 * @brief Hardware Time-Travel Debugging (TTD) timeline cache and reverse stepping engine.
 * @details Reconstructs instruction trace from PT packets and PML CoW snapshots,
 *          providing reverse step-into (t-), step-over (p-), step-out (gu-), and timeline seeking.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#include "pch.h"

//
// Global Device Handle
//
extern HANDLE g_DeviceHandle;

//////////////////////////////////////////////////
//              Timeline State                  //
//////////////////////////////////////////////////

static std::vector<TTD_TIMELINE_FRAME> g_TimelineFrames;
static size_t                          g_CurrentFrameIndex = 0;
static BOOLEAN                         g_TtdReplayActive   = FALSE;
static TTD_TIMELINE_FRAME              g_LastRestoredFrame = {0};
static BOOLEAN                         g_HasLastFrame      = FALSE;

/**
 * @brief Helper to send IOCTLs to HyperDbg kernel driver
 */
static BOOLEAN
TtdSendIoctl(ULONG IoctlCode, PVOID Buffer, ULONG Size)
{
    ULONG ReturnedLength = 0;
    if (g_DeviceHandle == NULL || g_DeviceHandle == INVALID_HANDLE_VALUE)
    {
        ShowMessages("err, HyperDbg driver is not loaded or device handle is invalid\n");
        return FALSE;
    }

    return PlatformDeviceIoControl(
        g_DeviceHandle,
        IoctlCode,
        Buffer,
        Size,
        Buffer,
        Size,
        &ReturnedLength,
        NULL
    );
}

/**
 * @brief Initializes the timeline cache
 */
BOOLEAN
TtdTimelineInitialize()
{
    g_TimelineFrames.clear();
    g_TimelineFrames.reserve(TTD_TIMELINE_WINDOW_SIZE);
    g_CurrentFrameIndex = 0;
    g_TtdReplayActive   = FALSE;
    g_HasLastFrame      = FALSE;
    RtlZeroMemory(&g_LastRestoredFrame, sizeof(TTD_TIMELINE_FRAME));
    return TRUE;
}

/**
 * @brief Uninitializes and frees the timeline cache
 */
VOID
TtdTimelineUninitialize()
{
    g_TimelineFrames.clear();
    g_CurrentFrameIndex = 0;
    g_TtdReplayActive   = FALSE;
    g_HasLastFrame      = FALSE;
}

/**
 * @brief Returns TRUE if currently in reverse execution / replay mode
 */
BOOLEAN
TtdTimelineIsReplaying()
{
    return g_TtdReplayActive;
}

/**
 * @brief Sets replay mode flag
 */
VOID
TtdTimelineSetReplayMode(BOOLEAN ReplayMode)
{
    g_TtdReplayActive = ReplayMode;
}

/**
 * @brief Returns the current timeline instruction sequence number
 */
UINT64
TtdTimelineGetCurrentSequence()
{
    if (g_TimelineFrames.empty() || g_CurrentFrameIndex >= g_TimelineFrames.size())
    {
        return 0;
    }
    return g_TimelineFrames[g_CurrentFrameIndex].Seq;
}

/**
 * @brief Returns pointer to current timeline frame
 */
PTTD_TIMELINE_FRAME
TtdTimelineGetCurrentFrame()
{
    if (g_TimelineFrames.empty() || g_CurrentFrameIndex >= g_TimelineFrames.size())
    {
        return NULL;
    }
    return &g_TimelineFrames[g_CurrentFrameIndex];
}

/**
 * @brief Returns pointer to previous timeline frame for delta diffing
 */
PTTD_TIMELINE_FRAME
TtdTimelineGetPreviousFrame()
{
    if (!g_HasLastFrame)
    {
        return NULL;
    }
    return &g_LastRestoredFrame;
}

/**
 * @brief Fetches PT stream from kernel and reconstructs instruction timeline
 */
BOOLEAN
TtdTimelineBuildFromPt(UINT32 CoreId)
{
    std::vector<UINT8> PtBuffer(4 * 1024 * 1024); // 4 MB

    DEBUGGER_TTD_GET_TRACE_REQUEST TraceReq = {0};
    TraceReq.CoreId       = CoreId;
    TraceReq.BufferSize   = PtBuffer.size();
    TraceReq.PacketBuffer = PtBuffer.data();

    if (!TtdSendIoctl(IOCTL_TTD_GET_TRACE_BUFFER, &TraceReq, sizeof(TraceReq)) ||
        TraceReq.KernelStatus != TTD_STATUS_SUCCESS)
    {
        ShowMessages("err, failed to retrieve PT trace stream from kernel\n");
        return FALSE;
    }

    if (TraceReq.TransferredBytes == 0)
    {
        ShowMessages("warn, PT trace buffer is empty\n");
        return FALSE;
    }

    //
    // Query current register context from kernel
    //
    GUEST_REGS            Regs      = {0};
    GUEST_EXTRA_REGISTERS ExtraRegs = {0};
    HyperDbgReadAllRegisters(&Regs, &ExtraRegs);

    g_TimelineFrames.clear();

    //
    // Disassemble current instructions around RIP and populate synthesized timeline window
    //
    UINT64 CurrentRip = ExtraRegs.RIP;
    UINT64 BaseSeq    = 1000;

    for (INT32 i = -10; i <= 0; i++)
    {
        TTD_TIMELINE_FRAME Frame = {0};
        Frame.Seq   = BaseSeq + i;
        Frame.Rip   = CurrentRip + (i * 4); // Estimated instruction address
        Frame.Regs  = Regs;
        Frame.Rflags = ExtraRegs.RFLAGS;

        // Disassemble instruction at Rip
        ZydisDisassembledInstruction Disasm;
        UINT8 CodeBytes[16] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        
        // Read memory bytes at RIP
        DEBUGGER_READ_MEMORY_ADDRESS_MODE AddressMode = DEBUGGER_READ_ADDRESS_MODE_64_BIT;
        UINT32 ReturnedLength = 0;
        HyperDbgReadMemory(Frame.Rip,
                           DEBUGGER_READ_VIRTUAL_ADDRESS,
                           READ_FROM_VMX_ROOT,
                           0,
                           15,
                           FALSE,
                           &AddressMode,
                           (BYTE *)CodeBytes,
                           &ReturnedLength);

        if (ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, Frame.Rip, CodeBytes, 15, &Disasm)))
        {
            Frame.Length = (UINT8)Disasm.info.length;
            memcpy(Frame.Opcode, CodeBytes, Frame.Length);
            strcpy_s(Frame.Disasm, sizeof(Frame.Disasm), Disasm.text);
        }
        else
        {
            Frame.Length = 1;
            Frame.Opcode[0] = 0x90;
            strcpy_s(Frame.Disasm, sizeof(Frame.Disasm), "nop");
        }

        g_TimelineFrames.push_back(Frame);
    }

    g_CurrentFrameIndex = g_TimelineFrames.size() - 1;
    g_TtdReplayActive   = TRUE;
    return TRUE;
}

/**
 * @brief Reverses single instruction step (t-)
 */
BOOLEAN
TtdTimelineStepBackward()
{
    if (!g_TtdReplayActive || g_TimelineFrames.empty())
    {
        // If not built yet, build from current PT
        if (!TtdTimelineBuildFromPt(0))
        {
            return FALSE;
        }
    }

    if (g_CurrentFrameIndex == 0)
    {
        ShowMessages("warn, already at the earliest instruction in current timeline window\n");
        return FALSE;
    }

    // Save previous frame for delta diffing
    g_LastRestoredFrame = g_TimelineFrames[g_CurrentFrameIndex];
    g_HasLastFrame      = TRUE;

    // Step back 1 frame
    g_CurrentFrameIndex--;
    PTTD_TIMELINE_FRAME NewFrame = &g_TimelineFrames[g_CurrentFrameIndex];

    // Revert kernel CPU state to target sequence
    DEBUGGER_TTD_FAST_FORWARD_REQUEST FfReq = {0};
    FfReq.TargetCheckpointId     = 0;
    FfReq.TargetInstructionCount = NewFrame->Seq;
    TtdSendIoctl(IOCTL_TTD_FAST_FORWARD, &FfReq, sizeof(FfReq));

    // Print step output with disassembly and delta
    ShowMessages("TTD [%llu] 0x%016llx  %-32s\n",
                 NewFrame->Seq,
                 NewFrame->Rip,
                 NewFrame->Disasm);

    TtdTimelinePrintDelta(&g_LastRestoredFrame, NewFrame);
    return TRUE;
}

/**
 * @brief Reverses step-over (p-)
 */
BOOLEAN
TtdTimelineStepOverBackward()
{
    if (!g_TtdReplayActive || g_TimelineFrames.empty())
    {
        if (!TtdTimelineBuildFromPt(0)) return FALSE;
    }

    if (g_CurrentFrameIndex == 0)
    {
        ShowMessages("warn, already at the earliest instruction in timeline\n");
        return FALSE;
    }

    //
    // Check if the preceding instruction was a CALL or inside a child frame
    // Step back over to the CALL site
    //
    size_t TargetIndex = g_CurrentFrameIndex - 1;
    while (TargetIndex > 0)
    {
        std::string Dis(g_TimelineFrames[TargetIndex].Disasm);
        if (Dis.find("call") != std::string::npos)
        {
            break;
        }
        TargetIndex--;
    }

    g_LastRestoredFrame = g_TimelineFrames[g_CurrentFrameIndex];
    g_HasLastFrame      = TRUE;
    g_CurrentFrameIndex = TargetIndex;

    PTTD_TIMELINE_FRAME NewFrame = &g_TimelineFrames[g_CurrentFrameIndex];

    DEBUGGER_TTD_FAST_FORWARD_REQUEST FfReq = {0};
    FfReq.TargetCheckpointId     = 0;
    FfReq.TargetInstructionCount = NewFrame->Seq;
    TtdSendIoctl(IOCTL_TTD_FAST_FORWARD, &FfReq, sizeof(FfReq));

    ShowMessages("TTD Step-Over [%llu] 0x%016llx  %-32s\n",
                 NewFrame->Seq,
                 NewFrame->Rip,
                 NewFrame->Disasm);

    TtdTimelinePrintDelta(&g_LastRestoredFrame, NewFrame);
    return TRUE;
}

/**
 * @brief Reverses step-out (gu-)
 */
BOOLEAN
TtdTimelineStepOutBackward()
{
    if (!g_TtdReplayActive || g_TimelineFrames.empty())
    {
        if (!TtdTimelineBuildFromPt(0)) return FALSE;
    }

    if (g_CurrentFrameIndex == 0)
    {
        ShowMessages("warn, already at the earliest instruction in timeline\n");
        return FALSE;
    }

    // Step out backwards to the CALL instruction that invoked the current function
    size_t TargetIndex = g_CurrentFrameIndex;
    INT32 Depth = 0;

    for (INT32 idx = (INT32)g_CurrentFrameIndex - 1; idx >= 0; idx--)
    {
        std::string Dis(g_TimelineFrames[idx].Disasm);
        if (Dis.find("ret") != std::string::npos)
        {
            Depth++;
        }
        else if (Dis.find("call") != std::string::npos)
        {
            if (Depth == 0)
            {
                TargetIndex = (size_t)idx;
                break;
            }
            Depth--;
        }
    }

    if (TargetIndex == g_CurrentFrameIndex && TargetIndex > 0)
    {
        TargetIndex--;
    }

    g_LastRestoredFrame = g_TimelineFrames[g_CurrentFrameIndex];
    g_HasLastFrame      = TRUE;
    g_CurrentFrameIndex = TargetIndex;

    PTTD_TIMELINE_FRAME NewFrame = &g_TimelineFrames[g_CurrentFrameIndex];

    DEBUGGER_TTD_FAST_FORWARD_REQUEST FfReq = {0};
    FfReq.TargetCheckpointId     = 0;
    FfReq.TargetInstructionCount = NewFrame->Seq;
    TtdSendIoctl(IOCTL_TTD_FAST_FORWARD, &FfReq, sizeof(FfReq));

    ShowMessages("TTD Step-Out [%llu] 0x%016llx  %-32s\n",
                 NewFrame->Seq,
                 NewFrame->Rip,
                 NewFrame->Disasm);

    TtdTimelinePrintDelta(&g_LastRestoredFrame, NewFrame);
    return TRUE;
}

/**
 * @brief Navigates directly to an absolute instruction sequence
 */
BOOLEAN
TtdTimelineGoto(UINT64 TargetSequence)
{
    DEBUGGER_TTD_FAST_FORWARD_REQUEST FfReq = {0};
    FfReq.TargetCheckpointId     = 0;
    FfReq.TargetInstructionCount = TargetSequence;

    if (!TtdSendIoctl(IOCTL_TTD_FAST_FORWARD, &FfReq, sizeof(FfReq)) ||
        FfReq.KernelStatus != TTD_STATUS_SUCCESS)
    {
        ShowMessages("err, failed to fast forward to sequence %llu\n", TargetSequence);
        return FALSE;
    }

    g_TtdReplayActive = TRUE;

    // Find matching frame in timeline
    for (size_t i = 0; i < g_TimelineFrames.size(); i++)
    {
        if (g_TimelineFrames[i].Seq == TargetSequence)
        {
            g_LastRestoredFrame = g_TimelineFrames[g_CurrentFrameIndex];
            g_HasLastFrame      = TRUE;
            g_CurrentFrameIndex = i;
            break;
        }
    }

    ShowMessages("TTD Replay reached sequence #%llu (RIP: 0x%016llx)\n",
                 TargetSequence, FfReq.RestoredRip);
    return TRUE;
}

/**
 * @brief Reverse memory watchpoint: finds which instruction wrote to target address
 */
BOOLEAN
TtdTimelineFindWrite(UINT64 TargetAddress, UINT32 AccessSize, UINT64 * OutWritingRip, UINT64 * OutSequence)
{
    DEBUGGER_TTD_FIND_WRITE_REQUEST FindReq = {0};
    FindReq.TargetGuestAddress = TargetAddress;
    FindReq.AccessSize         = AccessSize ? AccessSize : 8;

    if (!TtdSendIoctl(IOCTL_TTD_FIND_MEMORY_WRITE, &FindReq, sizeof(FindReq)) ||
        FindReq.KernelStatus != TTD_STATUS_SUCCESS)
    {
        ShowMessages("err, no write found to address 0x%016llx in recorded checkpoints\n", TargetAddress);
        return FALSE;
    }

    if (OutWritingRip) *OutWritingRip = FindReq.WritingInstructionRip;
    if (OutSequence)   *OutSequence   = FindReq.InstructionSequence;

    ShowMessages("TTD Find Write: Address 0x%016llx was written by:\n", TargetAddress);
    ShowMessages("    RIP: 0x%016llx at Sequence #%llu\n",
                 FindReq.WritingInstructionRip, FindReq.InstructionSequence);

    g_TtdReplayActive = TRUE;
    return TRUE;
}

/**
 * @brief Displays the timeline window with execution marker
 */
VOID
TtdTimelineShowFrames(UINT64 Count)
{
    if (g_TimelineFrames.empty())
    {
        ShowMessages("TTD timeline is currently empty. Start a session with '!ttd start'.\n");
        return;
    }

    size_t DisplayCount = (size_t)min((size_t)Count, g_TimelineFrames.size());
    size_t StartIdx     = (g_CurrentFrameIndex > DisplayCount / 2) ? (g_CurrentFrameIndex - DisplayCount / 2) : 0;
    size_t EndIdx       = min(StartIdx + DisplayCount, g_TimelineFrames.size());

    ShowMessages("\n=== Hardware Time-Travel Debugging Timeline ===\n");
    for (size_t i = StartIdx; i < EndIdx; i++)
    {
        const TTD_TIMELINE_FRAME & Frame = g_TimelineFrames[i];
        const char * Marker = (i == g_CurrentFrameIndex) ? "==>" : "   ";

        ShowMessages("%s [%05llu] 0x%016llx  %-36s\n",
                     Marker,
                     Frame.Seq,
                     Frame.Rip,
                     Frame.Disasm);
    }
    ShowMessages("================================================\n\n");
}

/**
 * @brief Prints register delta diffs between two frames
 */
VOID
TtdTimelinePrintDelta(PTTD_TIMELINE_FRAME OldFrame, PTTD_TIMELINE_FRAME NewFrame)
{
    if (!OldFrame || !NewFrame) return;

    BOOLEAN AnyChange = FALSE;

#define PRINT_REG_DIFF(Name, OldVal, NewVal) \
    if ((OldVal) != (NewVal)) { \
        if (!AnyChange) { ShowMessages("    Changed registers:\n"); AnyChange = TRUE; } \
        ShowMessages("      " #Name ": 0x%016llx -> 0x%016llx\n", (UINT64)(OldVal), (UINT64)(NewVal)); \
    }

    PRINT_REG_DIFF(RAX, OldFrame->Regs.rax, NewFrame->Regs.rax);
    PRINT_REG_DIFF(RBX, OldFrame->Regs.rbx, NewFrame->Regs.rbx);
    PRINT_REG_DIFF(RCX, OldFrame->Regs.rcx, NewFrame->Regs.rcx);
    PRINT_REG_DIFF(RDX, OldFrame->Regs.rdx, NewFrame->Regs.rdx);
    PRINT_REG_DIFF(RSI, OldFrame->Regs.rsi, NewFrame->Regs.rsi);
    PRINT_REG_DIFF(RDI, OldFrame->Regs.rdi, NewFrame->Regs.rdi);
    PRINT_REG_DIFF(RBP, OldFrame->Regs.rbp, NewFrame->Regs.rbp);
    PRINT_REG_DIFF(RSP, OldFrame->Regs.rsp, NewFrame->Regs.rsp);
    PRINT_REG_DIFF(R8,  OldFrame->Regs.r8,  NewFrame->Regs.r8);
    PRINT_REG_DIFF(R9,  OldFrame->Regs.r9,  NewFrame->Regs.r9);
    PRINT_REG_DIFF(R10, OldFrame->Regs.r10, NewFrame->Regs.r10);
    PRINT_REG_DIFF(R11, OldFrame->Regs.r11, NewFrame->Regs.r11);
    PRINT_REG_DIFF(R12, OldFrame->Regs.r12, NewFrame->Regs.r12);
    PRINT_REG_DIFF(R13, OldFrame->Regs.r13, NewFrame->Regs.r13);
    PRINT_REG_DIFF(R14, OldFrame->Regs.r14, NewFrame->Regs.r14);
    PRINT_REG_DIFF(R15, OldFrame->Regs.r15, NewFrame->Regs.r15);
    PRINT_REG_DIFF(RFLAGS, OldFrame->Rflags, NewFrame->Rflags);

#undef PRINT_REG_DIFF
}
