#ifndef INPUT_INJECTOR_H
#define INPUT_INJECTOR_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <vector>
#include <cstdint>

enum InputType : uint8_t {
    INPUT_TYPE_MOUSE    = 1,
    INPUT_TYPE_KEYBOARD = 2
};

#pragma pack(push, 1)
struct InputPayload {
    uint8_t  type{ 0 };            // InputType (Mouse or Keyboard)
    
    // Mouse Fields
    float    normalizedX{ 0.0f };   // 0.0f to 1.0f (Viewport relative)
    float    normalizedY{ 0.0f };   // 0.0f to 1.0f (Viewport relative)
    uint32_t mouseFlags{ 0 };      // MOUSEEVENTF_*
    int32_t  mouseData{ 0 };       // Wheel delta or extra button data
    
    // Keyboard Fields
    uint16_t virtualKey{ 0 };      // VK_* code
    uint16_t scanCode{ 0 };        // Hardware scan code
    uint8_t  keyUp{ 0 };           // 0 = KeyDown, 1 = KeyUp
    uint8_t  isExtendedKey{ 0 };   // Extended key bit flag
};
#pragma pack(pop)

class InputInjector {
public:
    static void InjectMouse(float normalizedX, float normalizedY, DWORD mouseFlags, DWORD mouseData = 0);
    static void InjectKeyboard(WORD virtualKey, bool keyUp, WORD scanCode = 0, bool isExtended = false);

    static std::vector<uint8_t> SerializeMouse(float localX, float localY, float renderWidth, float renderHeight, DWORD mouseFlags, DWORD mouseData = 0);
    static std::vector<uint8_t> SerializeKeyboard(WORD virtualKey, bool keyUp);

    static bool DeserializeAndInject(const std::vector<uint8_t>& packetData);
};

#endif // INPUT_INJECTOR_H