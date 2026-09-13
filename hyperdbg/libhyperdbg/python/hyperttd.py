#!/usr/bin/env python3
"""
HyperDbg Hardware Time-Travel Debugging (TTD) Python SDK & Automation Bridge

Provides a Python ctypes interface for HyperDbg's hypervisor-backed Time-Travel
Debugging engine, synchronizing Intel PT ToPA streaming with EPT PML CoW undo logging.
"""

import ctypes
import os
import sys
import time
import argparse
from ctypes import wintypes

#
# Constants & Codes (mirrored from TtdDefinitions.h and Ioctls.h)
#
TTD_PML_ENTRIES_PER_EXIT        = 512
TTD_MAX_CHECKPOINTS_IN_RING     = 16
TTD_MAX_PAGE_DIFFS_PER_INTERVAL = 4096

TTD_STATUS_SUCCESS              = 0x00000000
TTD_STATUS_ALREADY_RECORDING    = 0x00000001
TTD_STATUS_NOT_RECORDING        = 0x00000002
TTD_STATUS_BUFFER_EXHAUSTED     = 0x00000003
TTD_STATUS_CHECKPOINT_NOT_FOUND = 0x00000004
TTD_STATUS_REPLAY_DIVERGENCE    = 0x00000005
TTD_STATUS_INVALID_PARAMETER    = 0x00000006

#
# IOCTL Codes
#
def CTL_CODE(device_type, function, method, access):
    return (device_type << 16) | (access << 14) | (function << 2) | method

FILE_DEVICE_UNKNOWN = 0x00000022
METHOD_BUFFERED     = 0
FILE_ANY_ACCESS     = 0
IOCTL_TTD_BASE      = 0x800 + 0x500

IOCTL_TTD_START              = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x01, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_STOP               = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x02, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_GET_STATUS         = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_TAKE_CHECKPOINT    = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x04, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_RESTORE_CHECKPOINT = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x05, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_FAST_FORWARD       = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x06, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_GET_TRACE_BUFFER   = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x07, METHOD_BUFFERED, FILE_ANY_ACCESS)
IOCTL_TTD_FIND_MEMORY_WRITE  = CTL_CODE(FILE_DEVICE_UNKNOWN, IOCTL_TTD_BASE + 0x08, METHOD_BUFFERED, FILE_ANY_ACCESS)

#
# ctypes C Structures
#
class TTD_SESSION_STATUS(ctypes.Structure):
    _fields_ = [
        ("IsRecording",              wintypes.BOOLEAN),
        ("IsReplaying",              wintypes.BOOLEAN),
        ("TargetPid",                ctypes.c_uint32),
        ("TargetCore",               ctypes.c_uint32),
        ("TotalCheckpointsTaken",    ctypes.c_uint32),
        ("ActiveCheckpointCount",    ctypes.c_uint32),
        ("TotalDirtyPagesTracked",   ctypes.c_uint64),
        ("TotalPtBytesRecorded",     ctypes.c_uint64),
        ("TotalInstructionsRetired", ctypes.c_uint64),
        ("CurrentReplaySequence",    ctypes.c_uint64),
        ("CurrentRip",               ctypes.c_uint64),
    ]

class DEBUGGER_TTD_START_REQUEST(ctypes.Structure):
    _fields_ = [
        ("TargetPid",           ctypes.c_uint32),
        ("PinToCore",           ctypes.c_uint32),
        ("PtBufferSize",        ctypes.c_uint64),
        ("MaxCheckpoints",      ctypes.c_uint32),
        ("WatermarkDirtyPages", ctypes.c_uint64),
        ("KernelStatus",        ctypes.c_uint32),
    ]

class DEBUGGER_TTD_RESTORE_REQUEST(ctypes.Structure):
    _fields_ = [
        ("TargetCheckpointId",    ctypes.c_uint32),
        ("TargetInstructionCount",ctypes.c_uint64),
        ("RestoredRip",           ctypes.c_uint64),
        ("RestoredPages",         ctypes.c_uint32),
        ("KernelStatus",          ctypes.c_uint32),
    ]

class DEBUGGER_TTD_FAST_FORWARD_REQUEST(ctypes.Structure):
    _fields_ = [
        ("TargetCheckpointId",    ctypes.c_uint32),
        ("TargetInstructionCount",ctypes.c_uint64),
        ("RestoredRip",           ctypes.c_uint64),
        ("KernelStatus",          ctypes.c_uint32),
    ]

class DEBUGGER_TTD_CHECKPOINT_REQUEST(ctypes.Structure):
    _fields_ = [
        ("CheckpointId",    ctypes.c_uint32),
        ("InstructionCount",ctypes.c_uint64),
        ("PtByteOffset",    ctypes.c_uint64),
        ("Rip",             ctypes.c_uint64),
        ("KernelStatus",    ctypes.c_uint32),
    ]

class DEBUGGER_TTD_FIND_WRITE_REQUEST(ctypes.Structure):
    _fields_ = [
        ("TargetGuestAddress",   ctypes.c_uint64),
        ("AccessSize",           ctypes.c_uint32),
        ("WritingInstructionRip",ctypes.c_uint64),
        ("OldValue",             ctypes.c_uint64),
        ("NewValue",             ctypes.c_uint64),
        ("InstructionSequence",  ctypes.c_uint64),
        ("KernelStatus",         ctypes.c_uint32),
    ]

#
# Python TTD Controller Class
#
class HyperDbgTtd:
    """
    High-level Hardware Time-Travel Debugging Controller for HyperDbg.
    """
    def __init__(self, device_path=r"\\.\HyperDbgDevice"):
        self.device_path   = device_path
        self.device_handle = None
        self._connect()

    def _connect(self):
        INVALID_HANDLE_VALUE = -1
        GENERIC_READ  = 0x80000000
        GENERIC_WRITE = 0x40000000
        OPEN_EXISTING = 3

        handle = ctypes.windll.kernel32.CreateFileW(
            self.device_path,
            GENERIC_READ | GENERIC_WRITE,
            0,
            None,
            OPEN_EXISTING,
            0,
            None
        )

        if handle == INVALID_HANDLE_VALUE:
            err = ctypes.windll.kernel32.GetLastError()
            raise RuntimeError(f"Could not connect to HyperDbg device {self.device_path} (Win32 Error: {err})")

        self.device_handle = handle

    def close(self):
        if self.device_handle:
            ctypes.windll.kernel32.CloseHandle(self.device_handle)
            self.device_handle = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _send_ioctl(self, ioctl_code, in_struct, out_struct):
        returned = wintypes.DWORD(0)
        in_buf   = ctypes.byref(in_struct) if in_struct else None
        in_size  = ctypes.sizeof(in_struct) if in_struct else 0
        out_buf  = ctypes.byref(out_struct) if out_struct else None
        out_size = ctypes.sizeof(out_struct) if out_struct else 0

        ok = ctypes.windll.kernel32.DeviceIoControl(
            self.device_handle,
            ioctl_code,
            in_buf,
            in_size,
            out_buf,
            out_size,
            ctypes.byref(returned),
            None
        )
        return bool(ok)

    def start(self, pid=0, core=0xFFFFFFFF, pt_buffer_size=4*1024*1024, max_checkpoints=16):
        """Starts hardware-assisted TTD recording."""
        req = DEBUGGER_TTD_START_REQUEST()
        req.TargetPid           = pid
        req.PinToCore           = core
        req.PtBufferSize        = pt_buffer_size
        req.MaxCheckpoints      = max_checkpoints
        req.WatermarkDirtyPages = 2048

        ok = self._send_ioctl(IOCTL_TTD_START, req, req)
        return ok and (req.KernelStatus == TTD_STATUS_SUCCESS)

    def stop(self):
        """Stops active TTD recording."""
        status = ctypes.c_uint32(0)
        ok = self._send_ioctl(IOCTL_TTD_STOP, status, status)
        return ok and (status.value == TTD_STATUS_SUCCESS)

    def get_status(self):
        """Queries session status and metrics."""
        status = TTD_SESSION_STATUS()
        ok = self._send_ioctl(IOCTL_TTD_GET_STATUS, None, status)
        if not ok:
            return None
        return {
            "is_recording":          bool(status.IsRecording),
            "is_replaying":          bool(status.IsReplaying),
            "target_pid":            status.TargetPid,
            "target_core":           status.TargetCore,
            "total_checkpoints":     status.TotalCheckpointsTaken,
            "active_checkpoints":    status.ActiveCheckpointCount,
            "pt_bytes_recorded":     status.TotalPtBytesRecorded,
            "instructions_retired":  status.TotalInstructionsRetired,
            "replay_sequence":       status.CurrentReplaySequence,
            "current_rip":           hex(status.CurrentRip),
        }

    def checkpoint(self):
        """Takes an explicit incremental checkpoint."""
        req = DEBUGGER_TTD_CHECKPOINT_REQUEST()
        ok = self._send_ioctl(IOCTL_TTD_TAKE_CHECKPOINT, req, req)
        if ok and req.KernelStatus == TTD_STATUS_SUCCESS:
            return {
                "checkpoint_id":     req.CheckpointId,
                "rip":               hex(req.Rip),
                "instruction_count": req.InstructionCount,
                "pt_byte_offset":    hex(req.PtByteOffset),
            }
        return None

    def restore(self, checkpoint_id):
        """Reverts RAM and CPU state to target checkpoint."""
        req = DEBUGGER_TTD_RESTORE_REQUEST()
        req.TargetCheckpointId = checkpoint_id
        ok = self._send_ioctl(IOCTL_TTD_RESTORE_CHECKPOINT, req, req)
        if ok and req.KernelStatus == TTD_STATUS_SUCCESS:
            return {
                "restored_rip":   hex(req.RestoredRip),
                "restored_pages": req.RestoredPages,
            }
        return None

    def goto_sequence(self, sequence):
        """Fast forwards CPU to exact instruction sequence."""
        req = DEBUGGER_TTD_FAST_FORWARD_REQUEST()
        req.TargetCheckpointId     = 0
        req.TargetInstructionCount = sequence
        ok = self._send_ioctl(IOCTL_TTD_FAST_FORWARD, req, req)
        if ok and req.KernelStatus == TTD_STATUS_SUCCESS:
            return hex(req.RestoredRip)
        return None

    def find_memory_write(self, target_address, access_size=8):
        """Reverse memory watchpoint: finds which instruction wrote to target address."""
        req = DEBUGGER_TTD_FIND_WRITE_REQUEST()
        req.TargetGuestAddress = target_address
        req.AccessSize         = access_size
        ok = self._send_ioctl(IOCTL_TTD_FIND_MEMORY_WRITE, req, req)
        if ok and req.KernelStatus == TTD_STATUS_SUCCESS:
            return {
                "writing_rip": hex(req.WritingInstructionRip),
                "sequence":    req.InstructionSequence,
            }
        return None

def main():
    parser = argparse.ArgumentParser(description="HyperDbg Hardware Time-Travel Debugging (TTD) CLI Bridge")
    parser.add_argument("action", choices=["start", "stop", "status", "checkpoint", "restore", "goto", "find"])
    parser.add_argument("--pid", type=lambda x: int(x, 0), default=0, help="Target process ID")
    parser.add_argument("--core", type=int, default=0xFFFFFFFF, help="Pin to core index")
    parser.add_argument("--checkpoint-id", type=int, default=0, help="Checkpoint ID to restore")
    parser.add_argument("--seq", type=int, default=0, help="Instruction sequence for goto")
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=0, help="Address for memory write search")
    args = parser.parse_args()

    print("[*] HyperDbg Hardware Time-Travel Debugging Bridge")
    with HyperDbgTtd() as ttd:
        if args.action == "start":
            print(f"[*] Starting TTD session (PID: {args.pid}, Core: {args.core})...")
            if ttd.start(pid=args.pid, core=args.core):
                print("[+] TTD session started successfully.")
            else:
                print("[-] Failed to start TTD session.")

        elif args.action == "stop":
            print("[*] Stopping TTD session...")
            if ttd.stop():
                print("[+] TTD session stopped.")
            else:
                print("[-] Failed to stop TTD session.")

        elif args.action == "status":
            st = ttd.get_status()
            if st:
                print(f"[+] TTD Status:\n{st}")
            else:
                print("[-] Could not retrieve TTD status.")

        elif args.action == "checkpoint":
            cp = ttd.checkpoint()
            print(f"[+] Checkpoint: {cp}")

        elif args.action == "restore":
            res = ttd.restore(args.checkpoint_id)
            print(f"[+] Restored: {res}")

        elif args.action == "goto":
            rip = ttd.goto_sequence(args.seq)
            print(f"[+] Replay reached RIP: {rip}")

        elif args.action == "find":
            found = ttd.find_memory_write(args.addr)
            print(f"[+] Memory write found: {found}")

if __name__ == "__main__":
    main()
