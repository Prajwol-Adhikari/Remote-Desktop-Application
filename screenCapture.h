
#ifndef SCREEN_CAPTURE_H
#define SCREEN_CAPTURE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <functional>
#include <string>

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t width{0};
    uint32_t height{0};
    uint32_t uncompressedSize{0};
    uint32_t compressedSize{0};
    uint32_t frameIndex{0};
};
#pragma pack(pop)

class ScreenCapturer {
public:
    ScreenCapturer();
    ~ScreenCapturer();

    void Start();
    void Stop();

    int GetWidth() const { return m_Width.load(); }
    int GetHeight() const { return m_Height.load(); }
    bool IsRunning() const { return m_IsRunning.load(); }

    // Fixed: Copy instead of move out so buffer remains valid if consumer misses a frame
    bool GetLatestFrame(std::vector<uint8_t>& outBuffer);

    static bool DecompressFrame(const std::vector<uint8_t>& inBuffer, 
                                std::vector<uint8_t>& outPixels, 
                                uint32_t& width, 
                                uint32_t& height);

    std::function<void(const std::string& title, const std::string& message)> OnErrorOccurred;
    std::function<void(const std::string& logMessage)> OnLogMessage;

private:
    void CaptureLoop();
    bool UpdateDisplayDimensions();
    void LogEvent(const std::string& message);
    void TriggerError(const std::string& title, const std::string& message);

    std::atomic<int> m_Width{0};
    std::atomic<int> m_Height{0};
    BITMAPINFO m_BitmapInfo{};

    std::vector<uint8_t> m_RawBuffer;
    std::vector<uint8_t> m_PreviousRawBuffer;
    std::vector<uint8_t> m_FrontBuffer;
    std::vector<uint8_t> m_BackBuffer;
    std::mutex m_BufferMutex;

    std::thread m_WorkerThread;
    std::atomic<bool> m_IsRunning{ false };
    uint32_t m_FrameCounter{ 0 };
};

#endif // SCREEN_CAPTURE_H