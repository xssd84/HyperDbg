/**
 * @file ttd-timeline.h
 * @author HyperDbg Dev Team
 * @brief Header for Hardware Time-Travel Debugging (TTD) timeline cache and reverse stepping.
 * @details Reconstructs instruction trace from PT packets and PML CoW snapshots,
 *          providing reverse step-into (t-), step-over (p-), step-out (gu-), and timeline seeking.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#pragma once

#include "SDK/headers/TtdDefinitions.h"
#include <vector>
#include <string>

//////////////////////////////////////////////////
//                  Functions                   //
//////////////////////////////////////////////////

BOOLEAN
TtdTimelineInitialize();

VOID
TtdTimelineUninitialize();

BOOLEAN
TtdTimelineIsReplaying();

VOID
TtdTimelineSetReplayMode(BOOLEAN ReplayMode);

UINT64
TtdTimelineGetCurrentSequence();

PTTD_TIMELINE_FRAME
TtdTimelineGetCurrentFrame();

PTTD_TIMELINE_FRAME
TtdTimelineGetPreviousFrame();

BOOLEAN
TtdTimelineBuildFromPt(UINT32 CoreId);

BOOLEAN
TtdTimelineStepBackward();

BOOLEAN
TtdTimelineStepOverBackward();

BOOLEAN
TtdTimelineStepOutBackward();

BOOLEAN
TtdTimelineGoto(UINT64 TargetSequence);

BOOLEAN
TtdTimelineFindWrite(UINT64 TargetAddress, UINT32 AccessSize, UINT64 * OutWritingRip, UINT64 * OutSequence);

VOID
TtdTimelineShowFrames(UINT64 Count);

VOID
TtdTimelinePrintDelta(PTTD_TIMELINE_FRAME OldFrame, PTTD_TIMELINE_FRAME NewFrame);
