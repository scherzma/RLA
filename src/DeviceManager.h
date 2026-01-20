#pragma once

#include "Types.h"

#include <vector>
#include <unordered_map>
#include <string>

namespace RLA {

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager() = default;

    // Enumerate all connected mice
    void EnumerateDevices();

    // Get list of all detected mice
    const std::vector<MouseDevice>& GetDevices() const { return devices_; }

    // Assignment state management
    void StartAssigningMouseA();
    void StartAssigningMouseB();
    void CancelAssignment();

    bool IsAssigning() const { return assigningMouseA_ || assigningMouseB_; }
    bool IsAssigningMouseA() const { return assigningMouseA_; }
    bool IsAssigningMouseB() const { return assigningMouseB_; }

    // Process mouse event during assignment
    // Returns true if assignment was completed
    bool ProcessEvent(const MouseEvent& event);

    // Get assigned devices
    HANDLE GetMouseA() const { return mouseA_; }
    HANDLE GetMouseB() const { return mouseB_; }

    bool IsMouseAAssigned() const { return mouseA_ != nullptr; }
    bool IsMouseBAssigned() const { return mouseB_ != nullptr; }

    // Get device info for assigned mice
    const MouseDevice* GetMouseADevice() const;
    const MouseDevice* GetMouseBDevice() const;

    // Check if event belongs to Mouse A or B
    bool IsMouseA(HANDLE handle) const { return handle == mouseA_; }
    bool IsMouseB(HANDLE handle) const { return handle == mouseB_; }

    // Reset assignments
    void ResetAssignments();

    // Movement threshold for assignment (total movement required)
    void SetAssignmentThreshold(int32_t threshold) { assignmentThreshold_ = threshold; }
    int32_t GetAssignmentThreshold() const { return assignmentThreshold_; }

private:
    std::wstring GetDeviceName(HANDLE hDevice);
    std::wstring GetDevicePath(HANDLE hDevice);

    std::vector<MouseDevice> devices_;
    std::unordered_map<HANDLE, int64_t> movementAccumulator_; // Track movement per device

    HANDLE mouseA_ = nullptr;
    HANDLE mouseB_ = nullptr;

    bool assigningMouseA_ = false;
    bool assigningMouseB_ = false;

    int32_t assignmentThreshold_ = 500; // Total movement pixels required
};

} // namespace RLA
