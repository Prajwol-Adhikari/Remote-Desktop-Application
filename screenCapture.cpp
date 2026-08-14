// ==========================================
// screenCapture.cpp (Zero-Dependency Stateful Delta Compression)
// ==========================================
#include "screenCapture.h"
#include <cstring>
#include <algorithm>
//worked

ScreenCapturer::ScreenCapturer() { UpdateDisplayDimensions(); }
ScreenCapturer::~ScreenCapturer() { Stop(); }

void ScreenCapturer::LogEvent(const std::string &message)
{
    if (OnLogMessage) OnLogMessage("[ScreenCapturer] " + message);
}

void ScreenCapturer::TriggerError(const std::string &title, const std::string &message)
{
    LogEvent("ERROR: [" + title + "] " + message);
    if (OnErrorOccurred) OnErrorOccurred(title, message);
}

bool ScreenCapturer::UpdateDisplayDimensions()
{
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }

    if (w != m_Width.load() || h != m_Height.load())
    {
        m_Width.store(w);
        m_Height.store(h);

        std::memset(&m_BitmapInfo, 0, sizeof(BITMAPINFO));
        m_BitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        m_BitmapInfo.bmiHeader.biWidth = w;
        m_BitmapInfo.bmiHeader.biHeight = -h;
        m_BitmapInfo.bmiHeader.biPlanes = 1;
        m_BitmapInfo.bmiHeader.biBitCount = 32;
        m_BitmapInfo.bmiHeader.biCompression = BI_RGB;

        size_t size = static_cast<size_t>(w) * h * 4;
        m_RawBuffer.resize(size);
        m_PreviousRawBuffer.resize(size, 0);
        return true;
    }
    return false;
}

void ScreenCapturer::Start()
{
    if (m_IsRunning) return;
    m_IsRunning = true;
    m_WorkerThread = std::thread(&ScreenCapturer::CaptureLoop, this);
}

void ScreenCapturer::Stop()
{
    if (!m_IsRunning) return;
    m_IsRunning = false;
    if (m_WorkerThread.joinable()) m_WorkerThread.join();
}

void ScreenCapturer::CaptureLoop()
{
    HDC hScreenDC = GetDC(HWND_DESKTOP);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, m_Width.load(), m_Height.load());
    HGDIOBJ hOld = SelectObject(hMemoryDC, hBitmap);

    while (m_IsRunning)
    {
        if (UpdateDisplayDimensions())
        {
            SelectObject(hMemoryDC, hOld);
            DeleteObject(hBitmap);
            DeleteDC(hMemoryDC);
            hScreenDC = GetDC(HWND_DESKTOP);
            hMemoryDC = CreateCompatibleDC(hScreenDC);
            hBitmap = CreateCompatibleBitmap(hScreenDC, m_Width.load(), m_Height.load());
            hOld = SelectObject(hMemoryDC, hBitmap);
        }

        int w = m_Width.load();
        int h = m_Height.load();

        if (!BitBlt(hMemoryDC, 0, 0, w, h, hScreenDC, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), SRCCOPY | CAPTUREBLT))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            continue;
        }

        GetDIBits(hMemoryDC, hBitmap, 0, h, m_RawBuffer.data(), &m_BitmapInfo, DIB_RGB_COLORS);

        std::vector<uint8_t> compressedData;
        compressedData.reserve(m_RawBuffer.size());

        const uint32_t* curr = reinterpret_cast<const uint32_t*>(m_RawBuffer.data());
        const uint32_t* prev = reinterpret_cast<const uint32_t*>(m_PreviousRawBuffer.data());
        size_t pixelCount = static_cast<size_t>(w) * h;

        size_t i = 0;
        while (i < pixelCount)
        {
            size_t sameCount = 0;
            while (i + sameCount < pixelCount && curr[i + sameCount] == prev[i + sameCount] && sameCount < 65535)
            {
                sameCount++;
            }

            if (sameCount > 0)
            {
                uint8_t marker = 0x00; 
                compressedData.push_back(marker);
                uint16_t count16 = static_cast<uint16_t>(sameCount);
                compressedData.insert(compressedData.end(), reinterpret_cast<const uint8_t*>(&count16), reinterpret_cast<const uint8_t*>(&count16) + 2);
                i += sameCount;
            }

            size_t diffCount = 0;
            while (i + diffCount < pixelCount && curr[i + diffCount] != prev[i + diffCount] && diffCount < 65535)
            {
                diffCount++;
            }

            if (diffCount > 0)
            {
                uint8_t marker = 0x01; 
                compressedData.push_back(marker);
                uint16_t count16 = static_cast<uint16_t>(diffCount);
                compressedData.insert(compressedData.end(), reinterpret_cast<const uint8_t*>(&count16), reinterpret_cast<const uint8_t*>(&count16) + 2);
                
                const uint8_t* pixelBytes = reinterpret_cast<const uint8_t*>(&curr[i]);
                compressedData.insert(compressedData.end(), pixelBytes, pixelBytes + (diffCount * 4));
                i += diffCount;
            }
        }

        std::memcpy(m_PreviousRawBuffer.data(), m_RawBuffer.data(), m_RawBuffer.size());

        FrameHeader header{};
        header.width = static_cast<uint32_t>(w);
        header.height = static_cast<uint32_t>(h);
        header.uncompressedSize = static_cast<uint32_t>(m_RawBuffer.size());
        header.compressedSize = static_cast<uint32_t>(compressedData.size());
        header.frameIndex = ++m_FrameCounter;

        m_BackBuffer.resize(sizeof(FrameHeader) + compressedData.size());
        std::memcpy(m_BackBuffer.data(), &header, sizeof(FrameHeader));
        std::memcpy(m_BackBuffer.data() + sizeof(FrameHeader), compressedData.data(), compressedData.size());

        {
            std::lock_guard<std::mutex> lock(m_BufferMutex);
            m_FrontBuffer.swap(m_BackBuffer);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    SelectObject(hMemoryDC, hOld);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(HWND_DESKTOP, hScreenDC);
}

bool ScreenCapturer::DecompressFrame(const std::vector<uint8_t> &inBuffer, std::vector<uint8_t> &outPixels, uint32_t &width, uint32_t &height)
{
    static std::vector<uint8_t> s_lastDecodedFrame;
    static uint32_t s_lastWidth = 0;
    static uint32_t s_lastHeight = 0;

    if (inBuffer.size() < sizeof(FrameHeader)) return false;

    FrameHeader header{};
    std::memcpy(&header, inBuffer.data(), sizeof(FrameHeader));

    if (header.uncompressedSize == 0 || inBuffer.size() < sizeof(FrameHeader) + header.compressedSize) return false;

    width = header.width;
    height = header.height;
    outPixels.resize(header.uncompressedSize);

    if (s_lastWidth != width || s_lastHeight != height || s_lastDecodedFrame.size() != header.uncompressedSize)
    {
        s_lastDecodedFrame.resize(header.uncompressedSize, 0);
        s_lastWidth = width;
        s_lastHeight = height;
    }

    // Preserve previous frame state for unchanged blocks
    std::memcpy(outPixels.data(), s_lastDecodedFrame.data(), header.uncompressedSize);

    const uint8_t* ptr = inBuffer.data() + sizeof(FrameHeader);
    const uint8_t* end = ptr + header.compressedSize;
    uint32_t* outPixelsPtr = reinterpret_cast<uint32_t*>(outPixels.data());
    size_t pixelCount = static_cast<size_t>(width) * height;
    size_t outIndex = 0;

    while (ptr < end && outIndex < pixelCount)
    {
        uint8_t marker = *ptr++;
        if (ptr + 2 > end) break;
        uint16_t count = *reinterpret_cast<const uint16_t*>(ptr);
        ptr += 2;

        if (marker == 0x00) // Unchanged: leave existing frame pixels intact
        {
            outIndex += count;
        }
        else if (marker == 0x01) // Changed: overwrite with incoming delta pixels
        {
            size_t byteSize = count * 4;
            if (ptr + byteSize > end) break;
            std::memcpy(&outPixelsPtr[outIndex], ptr, byteSize);
            ptr += byteSize;
            outIndex += count;
        }
    }

    std::memcpy(s_lastDecodedFrame.data(), outPixels.data(), header.uncompressedSize);
    return true;
}

bool ScreenCapturer::GetLatestFrame(std::vector<uint8_t> &outBuffer)
{
    std::lock_guard<std::mutex> lock(m_BufferMutex);
    if (m_FrontBuffer.empty()) return false;

    outBuffer = m_FrontBuffer;
    return true;
}