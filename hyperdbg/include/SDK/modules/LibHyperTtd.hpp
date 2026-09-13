/**
 * @file LibHyperTtd.hpp
 * @author HyperDbg Dev Team
 * @brief Header-only C++ Hardware Time-Travel Debugging (TTD) client library for HyperDbg.
 * @details Provides zero-overhead hypervisor-backed recording, incremental checkpointing,
 *          Intel PT ToPA packet retrieval, and reverse timeline execution navigation.
 * @version 0.1
 * @date 2026-09-13
 *
 * @copyright This project is released under the GNU Public License v3.
 *
 */
#pragma once

#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <vector>
#include <string>
#include <stdexcept>
#include <memory>
#include <optional>

#include "SDK/headers/BasicTypes.h"
#include "SDK/headers/TtdDefinitions.h"
#include "SDK/headers/Ioctls.h"
#include "SDK/modules/HyperTtd.h"

namespace hyperdbg {
namespace ttd {

/**
 * @brief High-performance C++ TTD Session Manager
 */
class HyperDbgTtdSession {
public:
    explicit HyperDbgTtdSession(const std::wstring& device_path = L"\\\\.\\HyperDbgDevice")
        : device_handle_(INVALID_HANDLE_VALUE), device_path_(device_path) {
        Connect();
    }

    ~HyperDbgTtdSession() {
        if (IsRecording()) {
            Stop();
        }
        Disconnect();
    }

    /**
     * @brief Connects to the HyperDbg kernel driver device
     */
    void Connect() {
        if (device_handle_ != INVALID_HANDLE_VALUE) {
            return;
        }

        device_handle_ = CreateFileW(
            device_path_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (device_handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to connect to HyperDbg driver at " +
                                     std::string(device_path_.begin(), device_path_.end()) +
                                     " (Error: " + std::to_string(GetLastError()) + ")");
        }
    }

    /**
     * @brief Disconnects from the kernel driver
     */
    void Disconnect() {
        if (device_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(device_handle_);
            device_handle_ = INVALID_HANDLE_VALUE;
        }
    }

    /**
     * @brief Starts hardware-backed TTD recording
     */
    bool Start(uint32_t pid = 0, uint32_t core = 0xFFFFFFFF, uint64_t pt_buffer_size = 4 * 1024 * 1024, uint32_t max_checkpoints = 16) {
        DEBUGGER_TTD_START_REQUEST req{};
        req.TargetPid           = pid;
        req.PinToCore           = core;
        req.PtBufferSize        = pt_buffer_size;
        req.MaxCheckpoints      = max_checkpoints;
        req.WatermarkDirtyPages = TTD_MAX_PAGE_DIFFS_PER_INTERVAL / 2;

        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_START,
            &req, sizeof(req),
            &req, sizeof(req),
            &returned,
            nullptr
        );

        return (status && req.KernelStatus == TTD_STATUS_SUCCESS);
    }

    /**
     * @brief Stops TTD recording and flushes pending PML buffers
     */
    bool Stop() {
        uint32_t kernel_status = 0;
        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_STOP,
            &kernel_status, sizeof(kernel_status),
            &kernel_status, sizeof(kernel_status),
            &returned,
            nullptr
        );

        return (status && kernel_status == TTD_STATUS_SUCCESS);
    }

    /**
     * @brief Queries session metrics and state
     */
    bool GetStatus(TTD_SESSION_STATUS* out_status) {
        if (!out_status) return false;

        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_GET_STATUS,
            out_status, sizeof(TTD_SESSION_STATUS),
            out_status, sizeof(TTD_SESSION_STATUS),
            &returned,
            nullptr
        );

        return (status != FALSE);
    }

    /**
     * @brief Checks if session is actively recording
     */
    bool IsRecording() {
        TTD_SESSION_STATUS status{};
        if (GetStatus(&status)) {
            return status.IsRecording;
        }
        return false;
    }

    /**
     * @brief Creates an incremental checkpoint
     */
    bool TakeCheckpoint(uint32_t* out_checkpoint_id = nullptr) {
        DEBUGGER_TTD_CHECKPOINT_REQUEST req{};
        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_TAKE_CHECKPOINT,
            &req, sizeof(req),
            &req, sizeof(req),
            &returned,
            nullptr
        );

        if (status && req.KernelStatus == TTD_STATUS_SUCCESS) {
            if (out_checkpoint_id) {
                *out_checkpoint_id = req.CheckpointId;
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Rolls back to a past checkpoint
     */
    bool RestoreCheckpoint(uint32_t checkpoint_id, uint64_t* out_rip = nullptr) {
        DEBUGGER_TTD_RESTORE_REQUEST req{};
        req.TargetCheckpointId = checkpoint_id;

        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_RESTORE_CHECKPOINT,
            &req, sizeof(req),
            &req, sizeof(req),
            &returned,
            nullptr
        );

        if (status && req.KernelStatus == TTD_STATUS_SUCCESS) {
            if (out_rip) {
                *out_rip = req.RestoredRip;
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Fast-forwards physical CPU execution to target instruction count
     */
    bool FastForward(uint32_t checkpoint_id, uint64_t target_instructions, uint64_t* out_rip = nullptr) {
        DEBUGGER_TTD_FAST_FORWARD_REQUEST req{};
        req.TargetCheckpointId       = checkpoint_id;
        req.TargetInstructionCount   = target_instructions;

        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_FAST_FORWARD,
            &req, sizeof(req),
            &req, sizeof(req),
            &returned,
            nullptr
        );

        if (status && req.KernelStatus == TTD_STATUS_SUCCESS) {
            if (out_rip) {
                *out_rip = req.RestoredRip;
            }
            return true;
        }
        return false;
    }

    /**
     * @brief Retrieves raw Intel PT packet trace from hypervisor
     */
    bool GetTraceBuffer(uint32_t core_id, std::vector<uint8_t>& out_bytes) {
        DEBUGGER_TTD_GET_TRACE_REQUEST req{};
        req.CoreId     = core_id;
        req.BufferSize = out_bytes.size();
        req.PacketBuffer = out_bytes.data();

        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_GET_TRACE_BUFFER,
            &req, sizeof(req),
            &req, sizeof(req),
            &returned,
            nullptr
        );

        if (status && req.KernelStatus == TTD_STATUS_SUCCESS) {
            out_bytes.resize(static_cast<size_t>(req.TransferredBytes));
            return true;
        }
        return false;
    }

    /**
     * @brief Reverse memory watchpoint: finds which instruction wrote to target address
     */
    bool FindMemoryWrite(uint64_t target_address, uint32_t access_size, uint64_t* out_writing_rip, uint64_t* out_sequence = nullptr) {
        DEBUGGER_TTD_FIND_WRITE_REQUEST req{};
        req.TargetGuestAddress = target_address;
        req.AccessSize         = access_size;

        DWORD returned = 0;
        BOOL status = DeviceIoControl(
            device_handle_,
            IOCTL_TTD_FIND_MEMORY_WRITE,
            &req, sizeof(req),
            &req, sizeof(req),
            &returned,
            nullptr
        );

        if (status && req.KernelStatus == TTD_STATUS_SUCCESS) {
            if (out_writing_rip) {
                *out_writing_rip = req.WritingInstructionRip;
            }
            if (out_sequence) {
                *out_sequence = req.InstructionSequence;
            }
            return true;
        }
        return false;
    }

private:
    HANDLE       device_handle_;
    std::wstring device_path_;
};

} // namespace ttd
} // namespace hyperdbg
