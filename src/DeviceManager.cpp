#include "DeviceManager.h"

#include <algorithm>
#include <cmath>

namespace RLA {

DeviceManager::DeviceManager() {
    EnumerateDevices();
}

void DeviceManager::EnumerateDevices() {
    devices_.clear();

    UINT numDevices = 0;
    GetRawInputDeviceList(nullptr, &numDevices, sizeof(RAWINPUTDEVICELIST));

    if (numDevices == 0) return;

    std::vector<RAWINPUTDEVICELIST> deviceList(numDevices);
    if (GetRawInputDeviceList(deviceList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1)) {
        return;
    }

    for (const auto& device : deviceList) {
        if (device.dwType == RIM_TYPEMOUSE) {
            MouseDevice md;
            md.handle = device.hDevice;
            md.name = GetDeviceName(device.hDevice);
            md.path = GetDevicePath(device.hDevice);
            md.assigned = (device.hDevice == mouseA_ || device.hDevice == mouseB_);
            devices_.push_back(md);
        }
    }
}

std::wstring DeviceManager::GetDeviceName(HANDLE hDevice) {
    UINT size = 0;
    GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &size);

    if (size == 0) return L"Unknown Device";

    std::wstring name(size, L'\0');
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, name.data(), &size) == static_cast<UINT>(-1)) {
        return L"Unknown Device";
    }

    // Extract a friendly name from the device path
    // Typical format: \\?\HID#VID_046D&PID_C539...
    auto vidPos = name.find(L"VID_");
    auto pidPos = name.find(L"PID_");

    if (vidPos != std::wstring::npos && pidPos != std::wstring::npos) {
        std::wstring vid = name.substr(vidPos + 4, 4);
        std::wstring pid = name.substr(pidPos + 4, 4);
        return L"Mouse [VID:" + vid + L" PID:" + pid + L"]";
    }

    return L"Mouse Device";
}

std::wstring DeviceManager::GetDevicePath(HANDLE hDevice) {
    UINT size = 0;
    GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, nullptr, &size);

    if (size == 0) return L"";

    std::wstring path(size, L'\0');
    if (GetRawInputDeviceInfoW(hDevice, RIDI_DEVICENAME, path.data(), &size) == static_cast<UINT>(-1)) {
        return L"";
    }

    path.resize(path.find(L'\0') == std::wstring::npos ? path.size() : path.find(L'\0'));
    return path;
}

void DeviceManager::StartAssigningMouseA() {
    assigningMouseA_ = true;
    assigningMouseB_ = false;
    movementAccumulator_.clear();
}

void DeviceManager::StartAssigningMouseB() {
    assigningMouseA_ = false;
    assigningMouseB_ = true;
    movementAccumulator_.clear();
}

void DeviceManager::CancelAssignment() {
    assigningMouseA_ = false;
    assigningMouseB_ = false;
    movementAccumulator_.clear();
}

bool DeviceManager::ProcessEvent(const MouseEvent& event) {
    if (!assigningMouseA_ && !assigningMouseB_) {
        return false;
    }

    // Skip if this device is already assigned to the other slot
    if (assigningMouseA_ && event.deviceHandle == mouseB_) {
        return false;
    }
    if (assigningMouseB_ && event.deviceHandle == mouseA_) {
        return false;
    }

    // Accumulate movement
    int64_t movement = std::abs(static_cast<int64_t>(event.deltaX)) +
                       std::abs(static_cast<int64_t>(event.deltaY));
    movementAccumulator_[event.deviceHandle] += movement;

    // Check if threshold reached
    if (movementAccumulator_[event.deviceHandle] >= assignmentThreshold_) {
        if (assigningMouseA_) {
            mouseA_ = event.deviceHandle;
            assigningMouseA_ = false;
        } else if (assigningMouseB_) {
            mouseB_ = event.deviceHandle;
            assigningMouseB_ = false;
        }

        movementAccumulator_.clear();
        EnumerateDevices(); // Refresh assigned status
        return true;
    }

    return false;
}

const MouseDevice* DeviceManager::GetMouseADevice() const {
    if (!mouseA_) return nullptr;

    for (const auto& device : devices_) {
        if (device.handle == mouseA_) {
            return &device;
        }
    }
    return nullptr;
}

const MouseDevice* DeviceManager::GetMouseBDevice() const {
    if (!mouseB_) return nullptr;

    for (const auto& device : devices_) {
        if (device.handle == mouseB_) {
            return &device;
        }
    }
    return nullptr;
}

void DeviceManager::ResetAssignments() {
    mouseA_ = nullptr;
    mouseB_ = nullptr;
    assigningMouseA_ = false;
    assigningMouseB_ = false;
    movementAccumulator_.clear();
    EnumerateDevices();
}

} // namespace RLA
