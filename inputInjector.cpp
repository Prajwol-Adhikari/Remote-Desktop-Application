#include "inputInjector.h"
#include <cstring>

void InputInjector::InjectMouse(float normalizedX, float normalizedY, DWORD mouseFlags, DWORD mouseData) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    
    // Map normalized coordinates (0.0 to 1.0) to absolute mouse coordinates (0 to 65535) for Windows API
    if (normalizedX >= 0.0f && normalizedY >= 0.0f) {
        input.mi.dx = static_cast<LONG>(normalizedX * 65535.0f);
        input.mi.dy = static_cast<LONG>(normalizedY * 65535.0f);
        input.mi.dwFlags = mouseFlags | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    } else {
        input.mi.dwFlags = mouseFlags;
    }

    input.mi.mouseData = static_cast<DWORD>(mouseData);
    SendInput(1, &input, sizeof(INPUT));
}

void InputInjector::InjectKeyboard(WORD virtualKey, bool keyUp, WORD scanCode, bool isExtended) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    input.ki.wScan = scanCode ? scanCode : static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    
    DWORD flags = 0;
    if (keyUp) flags |= KEYEVENTF_KEYUP;
    if (isExtended) flags |= KEYEVENTF_EXTENDEDKEY;
    
    input.ki.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

std::vector<uint8_t> InputInjector::SerializeMouse(float localX, float localY, float renderWidth, float renderHeight, DWORD mouseFlags, DWORD mouseData) {
    InputPayload payload{};
    payload.type = INPUT_TYPE_MOUSE;
    payload.normalizedX = (renderWidth > 0.0f) ? (localX / renderWidth) : 0.0f;
    payload.normalizedY = (renderHeight > 0.0f) ? (localY / renderHeight) : 0.0f;
    payload.mouseFlags = mouseFlags;
    payload.mouseData = static_cast<int32_t>(mouseData);

    std::vector<uint8_t> packet(sizeof(InputPayload));
    std::memcpy(packet.data(), &payload, sizeof(InputPayload));
    return packet;
}

std::vector<uint8_t> InputInjector::SerializeKeyboard(WORD virtualKey, bool keyUp) {
    InputPayload payload{};
    payload.type = INPUT_TYPE_KEYBOARD;
    payload.virtualKey = virtualKey;
    payload.keyUp = keyUp ? 1 : 0;

    std::vector<uint8_t> packet(sizeof(InputPayload));
    std::memcpy(packet.data(), &payload, sizeof(InputPayload));
    return packet;
}

bool InputInjector::DeserializeAndInject(const std::vector<uint8_t>& packetData) {
    if (packetData.size() < sizeof(InputPayload)) return false;

    InputPayload payload{};
    std::memcpy(&payload, packetData.data(), sizeof(InputPayload));

    if (payload.type == INPUT_TYPE_MOUSE) {
        InjectMouse(payload.normalizedX, payload.normalizedY, payload.mouseFlags, payload.mouseData);
        return true;
    } else if (payload.type == INPUT_TYPE_KEYBOARD) {
        InjectKeyboard(payload.virtualKey, payload.keyUp != 0, payload.scanCode, payload.isExtendedKey != 0);
        return true;
    }
    return false;
}