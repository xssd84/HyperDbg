/**
 * @file ttd.cpp
 * @author HyperDbg Dev Team
 * @brief !ttd command and reverse execution commands (t-, p-, gu-, g-, ba-) implementation.
 * @details Implements the CLI interface and public SDK exports for Hardware Time-Travel Debugging.
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

/**
 * @brief Displays help documentation for the !ttd command.
 */
VOID
CommandTtdHelp()
{
    ShowMessages("!ttd : manages hardware-assisted Time-Travel Debugging (TTD).\n\n");
    ShowMessages("syntax : \t!ttd [start | stop | status | checkpoint | restore | timeline | goto | find]\n\n");
    ShowMessages("\t\te.g : !ttd start\n");
    ShowMessages("\t\te.g : !ttd start pid 1a4\n");
    ShowMessages("\t\te.g : !ttd start core 2 size 800000\n");
    ShowMessages("\t\te.g : !ttd stop\n");
    ShowMessages("\t\te.g : !ttd status\n");
    ShowMessages("\t\te.g : !ttd checkpoint\n");
    ShowMessages("\t\te.g : !ttd restore 2\n");
    ShowMessages("\t\te.g : !ttd timeline 20\n");
    ShowMessages("\t\te.g : !ttd goto 1042\n");
    ShowMessages("\t\te.g : !ttd find write 0x7ffe0000 8\n\n");
    ShowMessages("reverse execution commands:\n");
    ShowMessages("\t t- / tr- : step backward into the previous retired instruction.\n");
    ShowMessages("\t p- / pr- : step backward over function calls.\n");
    ShowMessages("\t gu-      : step backward out to the caller function.\n");
    ShowMessages("\t g-       : reverse execution continue.\n");
    ShowMessages("\t ba- w    : reverse memory write watchpoint.\n");
}

/**
 * @brief Displays help documentation for t-
 */
VOID
CommandTMinusHelp()
{
    ShowMessages("t- : reverse step into previous instruction in time.\n");
    ShowMessages("syntax : \tt- [count]\n");
}

/**
 * @brief Displays help documentation for p-
 */
VOID
CommandPMinusHelp()
{
    ShowMessages("p- : reverse step over function call in time.\n");
    ShowMessages("syntax : \tp- [count]\n");
}

/**
 * @brief Displays help documentation for gu-
 */
VOID
CommandGuMinusHelp()
{
    ShowMessages("gu- : reverse step out to caller function in time.\n");
    ShowMessages("syntax : \tgu-\n");
}

/**
 * @brief Displays help documentation for g-
 */
VOID
CommandGMinusHelp()
{
    ShowMessages("g- : reverse continue execution to preceding breakpoint.\n");
    ShowMessages("syntax : \tg-\n");
}

/**
 * @brief Displays help documentation for ba-
 */
VOID
CommandBaMinusHelp()
{
    ShowMessages("ba- : reverse execution hardware memory watchpoint.\n");
    ShowMessages("syntax : \tba- w <address> [size]\n");
}

/**
 * @brief Handler for !ttd command
 */
VOID
CommandTtd(vector<CommandToken> CommandTokens, string Command)
{
    UNREFERENCED_PARAMETER(Command);

    if (CommandTokens.size() < 2)
    {
        CommandTtdHelp();
        return;
    }

    string SubCommand = GetLowerStringFromCommandToken(CommandTokens.at(1));

    if (SubCommand == "start")
    {
        DEBUGGER_TTD_START_REQUEST Request = {0};
        Request.TargetPid           = 0;
        Request.PinToCore           = 0xFFFFFFFF;
        Request.PtBufferSize        = 4 * 1024 * 1024;
        Request.MaxCheckpoints      = 16;
        Request.WatermarkDirtyPages = 2048;

        for (size_t i = 2; i < CommandTokens.size(); i++)
        {
            string Option = GetLowerStringFromCommandToken(CommandTokens.at(i));

            if (Option == "pid" && i + 1 < CommandTokens.size())
            {
                ConvertTokenToUInt32(CommandTokens.at(++i), &Request.TargetPid);
            }
            else if (Option == "core" && i + 1 < CommandTokens.size())
            {
                ConvertTokenToUInt32(CommandTokens.at(++i), &Request.PinToCore);
            }
            else if (Option == "size" && i + 1 < CommandTokens.size())
            {
                ConvertTokenToUInt64(CommandTokens.at(++i), &Request.PtBufferSize);
            }
        }

        ULONG Returned = 0;
        BOOL Status = PlatformDeviceIoControl(
            g_DeviceHandle,
            IOCTL_TTD_START,
            &Request, sizeof(Request),
            &Request, sizeof(Request),
            &Returned, NULL
        );

        if (Status && Request.KernelStatus == TTD_STATUS_SUCCESS)
        {
            ShowMessages("Hardware Time-Travel Debugging (TTD) session started successfully.\n");
            ShowMessages("  Target PID:   %u (%s)\n", Request.TargetPid, Request.TargetPid ? "user process" : "system/kernel");
            ShowMessages("  PT Buffer:    0x%llx bytes per core\n", Request.PtBufferSize);
            ShowMessages("  Checkpoints:  %u max ring depth\n", Request.MaxCheckpoints);
            TtdTimelineInitialize();
        }
        else
        {
            ShowMessages("err, failed to start TTD session (Status: 0x%x)\n", Request.KernelStatus);
        }
    }
    else if (SubCommand == "stop")
    {
        UINT32 KernelStatus = 0;
        ULONG Returned = 0;
        BOOL Status = PlatformDeviceIoControl(
            g_DeviceHandle,
            IOCTL_TTD_STOP,
            &KernelStatus, sizeof(KernelStatus),
            &KernelStatus, sizeof(KernelStatus),
            &Returned, NULL
        );

        if (Status && KernelStatus == TTD_STATUS_SUCCESS)
        {
            ShowMessages("Hardware Time-Travel Debugging (TTD) session stopped.\n");
            TtdTimelineUninitialize();
        }
        else
        {
            ShowMessages("err, failed to stop TTD session\n");
        }
    }
    else if (SubCommand == "status")
    {
        TTD_SESSION_STATUS Status = {0};
        ULONG Returned = 0;
        BOOL Ok = PlatformDeviceIoControl(
            g_DeviceHandle,
            IOCTL_TTD_GET_STATUS,
            &Status, sizeof(Status),
            &Status, sizeof(Status),
            &Returned, NULL
        );

        if (Ok)
        {
            ShowMessages("\n=== Hardware Time-Travel Debugging Status ===\n");
            ShowMessages("  Recording:             %s\n", Status.IsRecording ? "ACTIVE" : "INACTIVE");
            ShowMessages("  Replaying:             %s\n", Status.IsReplaying ? "ACTIVE" : "INACTIVE");
            ShowMessages("  Target PID:            %u\n", Status.TargetPid);
            ShowMessages("  Target Core:           %s\n", Status.TargetCore == 0xFFFFFFFF ? "ALL" : std::to_string(Status.TargetCore).c_str());
            ShowMessages("  Total Checkpoints:     %u\n", Status.TotalCheckpointsTaken);
            ShowMessages("  Active Checkpoints:    %u\n", Status.ActiveCheckpointCount);
            ShowMessages("  Dirty Pages Tracked:   %llu\n", Status.TotalDirtyPagesTracked);
            ShowMessages("  PT Bytes Recorded:     0x%llx\n", Status.TotalPtBytesRecorded);
            ShowMessages("  Instructions Retired:  %llu\n", Status.TotalInstructionsRetired);
            ShowMessages("  Current Sequence:      #%llu\n", Status.CurrentReplaySequence);
            ShowMessages("  Current RIP:           0x%016llx\n", Status.CurrentRip);
            ShowMessages("=============================================\n\n");
        }
        else
        {
            ShowMessages("err, failed to query TTD status\n");
        }
    }
    else if (SubCommand == "checkpoint")
    {
        DEBUGGER_TTD_CHECKPOINT_REQUEST Request = {0};
        ULONG Returned = 0;
        BOOL Ok = PlatformDeviceIoControl(
            g_DeviceHandle,
            IOCTL_TTD_TAKE_CHECKPOINT,
            &Request, sizeof(Request),
            &Request, sizeof(Request),
            &Returned, NULL
        );

        if (Ok && Request.KernelStatus == TTD_STATUS_SUCCESS)
        {
            ShowMessages("Created incremental checkpoint #%u at RIP: 0x%016llx (Inst: %llu, PT Offset: 0x%llx)\n",
                         Request.CheckpointId, Request.Rip, Request.InstructionCount, Request.PtByteOffset);
        }
        else
        {
            ShowMessages("err, failed to create checkpoint\n");
        }
    }
    else if (SubCommand == "restore")
    {
        if (CommandTokens.size() < 3)
        {
            ShowMessages("syntax : !ttd restore <CheckpointId>\n");
            return;
        }

        UINT32 TargetId = 0;
        ConvertTokenToUInt32(CommandTokens.at(2), &TargetId);
        DEBUGGER_TTD_RESTORE_REQUEST Request = {0};
        Request.TargetCheckpointId = TargetId;

        ULONG Returned = 0;
        BOOL Ok = PlatformDeviceIoControl(
            g_DeviceHandle,
            IOCTL_TTD_RESTORE_CHECKPOINT,
            &Request, sizeof(Request),
            &Request, sizeof(Request),
            &Returned, NULL
        );

        if (Ok && Request.KernelStatus == TTD_STATUS_SUCCESS)
        {
            ShowMessages("Restored checkpoint #%u (Reverted %u pages, RIP: 0x%016llx)\n",
                         TargetId, Request.RestoredPages, Request.RestoredRip);
            TtdTimelineSetReplayMode(TRUE);
        }
        else
        {
            ShowMessages("err, failed to restore checkpoint #%u\n", TargetId);
        }
    }
    else if (SubCommand == "timeline")
    {
        UINT64 Count = 15;
        if (CommandTokens.size() >= 3)
        {
            ConvertTokenToUInt64(CommandTokens.at(2), &Count);
        }
        TtdTimelineShowFrames(Count);
    }
    else if (SubCommand == "goto")
    {
        if (CommandTokens.size() < 3)
        {
            ShowMessages("syntax : !ttd goto <InstructionSequenceNumber>\n");
            return;
        }
        UINT64 TargetSeq = 0;
        ConvertTokenToUInt64(CommandTokens.at(2), &TargetSeq);
        TtdTimelineGoto(TargetSeq);
    }
    else if (SubCommand == "find")
    {
        if (CommandTokens.size() >= 4 && GetLowerStringFromCommandToken(CommandTokens.at(2)) == "write")
        {
            UINT64 Address = 0;
            ConvertTokenToUInt64(CommandTokens.at(3), &Address);
            UINT32 Size = 8;
            if (CommandTokens.size() >= 5)
            {
                ConvertTokenToUInt32(CommandTokens.at(4), &Size);
            }
            UINT64 WritingRip = 0;
            UINT64 Sequence   = 0;
            TtdTimelineFindWrite(Address, Size, &WritingRip, &Sequence);
        }
        else
        {
            ShowMessages("syntax : !ttd find write <address> [size]\n");
        }
    }
    else
    {
        CommandTtdHelp();
    }
}

/**
 * @brief Handler for t- / tr- (Step Into Backward)
 */
VOID
CommandTMinus(vector<CommandToken> CommandTokens, string Command)
{
    UNREFERENCED_PARAMETER(Command);

    UINT32 Count = 1;
    if (CommandTokens.size() >= 2)
    {
        ConvertTokenToUInt32(CommandTokens.at(1), &Count);
    }

    for (UINT32 i = 0; i < Count; i++)
    {
        if (!TtdTimelineStepBackward())
        {
            break;
        }
    }
}

/**
 * @brief Handler for p- / pr- (Step Over Backward)
 */
VOID
CommandPMinus(vector<CommandToken> CommandTokens, string Command)
{
    UNREFERENCED_PARAMETER(Command);

    UINT32 Count = 1;
    if (CommandTokens.size() >= 2)
    {
        ConvertTokenToUInt32(CommandTokens.at(1), &Count);
    }

    for (UINT32 i = 0; i < Count; i++)
    {
        if (!TtdTimelineStepOverBackward())
        {
            break;
        }
    }
}

/**
 * @brief Handler for gu- (Step Out Backward)
 */
VOID
CommandGuMinus(vector<CommandToken> CommandTokens, string Command)
{
    UNREFERENCED_PARAMETER(Command);
    UNREFERENCED_PARAMETER(CommandTokens);

    TtdTimelineStepOutBackward();
}

/**
 * @brief Handler for g- (Reverse Continue)
 */
VOID
CommandGMinus(vector<CommandToken> CommandTokens, string Command)
{
    UNREFERENCED_PARAMETER(Command);
    UNREFERENCED_PARAMETER(CommandTokens);

    ShowMessages("Reverse continue running...\n");
    // Step backward repeatedly until timeline boundary or watchpoint
    for (INT32 i = 0; i < 100; i++)
    {
        if (!TtdTimelineStepBackward())
        {
            break;
        }
    }
}

/**
 * @brief Handler for ba- (Reverse Hardware Watchpoint)
 */
VOID
CommandBaMinus(vector<CommandToken> CommandTokens, string Command)
{
    UNREFERENCED_PARAMETER(Command);

    if (CommandTokens.size() < 3 || GetLowerStringFromCommandToken(CommandTokens.at(1)) != "w")
    {
        CommandBaMinusHelp();
        return;
    }

    UINT64 Address = 0;
    ConvertTokenToUInt64(CommandTokens.at(2), &Address);
    UINT32 Size    = 8;
    if (CommandTokens.size() >= 4)
    {
        ConvertTokenToUInt32(CommandTokens.at(3), &Size);
    }

    UINT64 WritingRip = 0;
    UINT64 Sequence   = 0;
    TtdTimelineFindWrite(Address, Size, &WritingRip, &Sequence);
}

//////////////////////////////////////////////////
//           Public C SDK Implementations       //
//////////////////////////////////////////////////

extern "C" {

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStart(UINT32 TargetPid, UINT32 PinToCore, UINT64 PtBufferSize, UINT32 MaxCheckpoints)
{
    DEBUGGER_TTD_START_REQUEST Request = {0};
    Request.TargetPid           = TargetPid;
    Request.PinToCore           = PinToCore;
    Request.PtBufferSize        = PtBufferSize ? PtBufferSize : (4 * 1024 * 1024);
    Request.MaxCheckpoints      = MaxCheckpoints ? MaxCheckpoints : 16;
    Request.WatermarkDirtyPages = 2048;

    ULONG Returned = 0;
    BOOL Status = PlatformDeviceIoControl(
        g_DeviceHandle,
        IOCTL_TTD_START,
        &Request, sizeof(Request),
        &Request, sizeof(Request),
        &Returned, NULL
    );

    if (Status && Request.KernelStatus == TTD_STATUS_SUCCESS)
    {
        TtdTimelineInitialize();
        return TRUE;
    }
    return FALSE;
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStop(VOID)
{
    UINT32 StatusVal = 0;
    ULONG Returned = 0;
    BOOL Status = PlatformDeviceIoControl(
        g_DeviceHandle,
        IOCTL_TTD_STOP,
        &StatusVal, sizeof(StatusVal),
        &StatusVal, sizeof(StatusVal),
        &Returned, NULL
    );

    if (Status && StatusVal == TTD_STATUS_SUCCESS)
    {
        TtdTimelineUninitialize();
        return TRUE;
    }
    return FALSE;
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdGetStatus(PTTD_SESSION_STATUS OutStatus)
{
    if (!OutStatus) return FALSE;

    ULONG Returned = 0;
    return PlatformDeviceIoControl(
        g_DeviceHandle,
        IOCTL_TTD_GET_STATUS,
        OutStatus, sizeof(TTD_SESSION_STATUS),
        OutStatus, sizeof(TTD_SESSION_STATUS),
        &Returned, NULL
    );
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdTakeCheckpoint(PUINT32 OutCheckpointId)
{
    DEBUGGER_TTD_CHECKPOINT_REQUEST Request = {0};
    ULONG Returned = 0;
    BOOL Ok = PlatformDeviceIoControl(
        g_DeviceHandle,
        IOCTL_TTD_TAKE_CHECKPOINT,
        &Request, sizeof(Request),
        &Request, sizeof(Request),
        &Returned, NULL
    );

    if (Ok && Request.KernelStatus == TTD_STATUS_SUCCESS)
    {
        if (OutCheckpointId) *OutCheckpointId = Request.CheckpointId;
        return TRUE;
    }
    return FALSE;
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdRestoreCheckpoint(UINT32 CheckpointId)
{
    DEBUGGER_TTD_RESTORE_REQUEST Request = {0};
    Request.TargetCheckpointId = CheckpointId;

    ULONG Returned = 0;
    BOOL Ok = PlatformDeviceIoControl(
        g_DeviceHandle,
        IOCTL_TTD_RESTORE_CHECKPOINT,
        &Request, sizeof(Request),
        &Request, sizeof(Request),
        &Returned, NULL
    );

    return (Ok && Request.KernelStatus == TTD_STATUS_SUCCESS);
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStepBackward(PTTD_TIMELINE_FRAME OutFrame)
{
    BOOLEAN Res = TtdTimelineStepBackward();
    if (Res && OutFrame)
    {
        PTTD_TIMELINE_FRAME Curr = TtdTimelineGetCurrentFrame();
        if (Curr) *OutFrame = *Curr;
    }
    return Res;
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStepOverBackward(PTTD_TIMELINE_FRAME OutFrame)
{
    BOOLEAN Res = TtdTimelineStepOverBackward();
    if (Res && OutFrame)
    {
        PTTD_TIMELINE_FRAME Curr = TtdTimelineGetCurrentFrame();
        if (Curr) *OutFrame = *Curr;
    }
    return Res;
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdStepOutBackward(PTTD_TIMELINE_FRAME OutFrame)
{
    BOOLEAN Res = TtdTimelineStepOutBackward();
    if (Res && OutFrame)
    {
        PTTD_TIMELINE_FRAME Curr = TtdTimelineGetCurrentFrame();
        if (Curr) *OutFrame = *Curr;
    }
    return Res;
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdGotoInstruction(UINT64 TargetSequence)
{
    return TtdTimelineGoto(TargetSequence);
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdFindMemoryWrite(UINT64 TargetAddress, UINT32 AccessSize, PUINT64 OutWritingRip, PUINT64 OutSequence)
{
    return TtdTimelineFindWrite(TargetAddress, AccessSize, OutWritingRip, OutSequence);
}

IMPORT_EXPORT_LIBHYPERDBG BOOLEAN
HyperTtdGetTraceBuffer(UINT32 CoreId, PVOID OutBuffer, UINT64 BufferSize, PUINT64 OutTransferredBytes)
{
    DEBUGGER_TTD_GET_TRACE_REQUEST Request = {0};
    Request.CoreId       = CoreId;
    Request.BufferSize   = BufferSize;
    Request.PacketBuffer = OutBuffer;

    ULONG Returned = 0;
    BOOL Ok = PlatformDeviceIoControl(
        g_DeviceHandle,
        IOCTL_TTD_GET_TRACE_BUFFER,
        &Request, sizeof(Request),
        &Request, sizeof(Request),
        &Returned, NULL
    );

    if (Ok && Request.KernelStatus == TTD_STATUS_SUCCESS)
    {
        if (OutTransferredBytes) *OutTransferredBytes = Request.TransferredBytes;
        return TRUE;
    }
    return FALSE;
}

} // extern "C"
