#include <windows.h>
#include "dependencies/imgui.h"
#include "dependencies/imgui_impl_win32.h"
#include "dependencies/imgui_impl_opengl3.h"
#include <GL/glew.h>  // MUST be first
#include <GLFW/glfw3.h> // Then GLFW / others
#include <GL/gl.h>

#include "NetworkManager.h"
#include "uiRenderer.h"

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void RunServerScreenShareLoop(NetworkManager& netManager, ScreenCapturer& capturer, int targetFps = 30)
{
    std::thread([&netManager, &capturer, targetFps]() {
        auto frameInterval = std::chrono::milliseconds(1000 / targetFps);

        while (netManager.IsServerRunning()) {
            auto startTime = std::chrono::steady_clock::now();

            std::vector<uint8_t> frameData;
            if (capturer.GetLatestFrame(frameData) && !frameData.empty()) {
                netManager.BroadcastServerScreenFrame(frameData);
            }

            // Cap the frame rate
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed < frameInterval) {
                std::this_thread::sleep_for(frameInterval - elapsed);
            }
        }
    }).detach();
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. Create application window
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"RDPWorkspaceClass", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"RDP Master Core Workspace", WS_OVERLAPPEDWINDOW, 100, 100, 1024, 768, nullptr, nullptr, wc.hInstance, nullptr);

    // 2. Initialize OpenGL context via Win32 device context
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0
    };
    int format = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, format, &pfd);
    HGLRC hglrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, hglrc);

    // Show the window
    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    // 3. Initialize Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;    // Enable Keyboard Controls

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplOpenGL3_Init("#version 130");

    // 4. Initialize Core Application Modules (OOP)
    NetworkManager networkManager;
    UIRenderer uiRenderer(networkManager);

    // Main loop
    bool done = false;
    while (!done) {
        // Poll Windows messages without blocking the frame rate
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render our Custom ImGui UI continuously
        uiRenderer.RenderUI();

        // Rendering
        ImGui::Render();
        glViewport(0, 0, (GLsizei)io.DisplaySize.x, (GLsizei)io.DisplaySize.y);
        glClearColor(0.12f, 0.14f, 0.16f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SwapBuffers(hdc);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(hglrc);
    ReleaseDC(hwnd, hdc);

    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}