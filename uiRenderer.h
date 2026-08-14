#ifndef UI_RENDERER_H
#define UI_RENDERER_H

#include "NetworkManager.h"
#include "screenCapture.h"
#include "inputInjector.h"
#include "dependencies/imgui.h"
#include <GL/glew.h>  
#include <GLFW/glfw3.h> 
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <map>
#include <thread>
#include <atomic>

enum AppState {
    STATE_ROLE_SELECTION,
    STATE_CONNECTION_HOME,
    STATE_SESSION_ACTIVE
};

enum UserRole {
    ROLE_NONE,
    ROLE_SERVER,
    ROLE_CLIENT
};

// Structure to hold chat logs per specific client ID on the server side
struct ClientChatSession {
    std::vector<std::pair<std::string, ImVec4>> messages;
    std::mutex chatMutex;
};

class UIRenderer {
public:
    UIRenderer(NetworkManager& netManager);
    ~UIRenderer();

    void ApplyCustomStyle();
    void RenderUI();


private:
    NetworkManager& m_netManager;
    AppState m_CurrentState;
    UserRole m_SelectedRole;

    // UI Input Buffers & Session Variables
    char m_ipInput[128];
    int m_portInput;
    char m_statusMessage[256];
    char m_Port[16];

    // Server-side multi-client dashboard tracking & permissions
    int m_TargetClientId = -1;
    int m_ActiveInputClientIndex = -1;
    std::vector<bool> m_ClientScreenShare;

    // Isolated chat storage per client index for the server, plus general chat for the client view
    std::map<int, ClientChatSession> m_ServerClientChats;
    std::vector<std::pair<std::string, ImVec4>> m_ClientChatLogs;
    std::mutex m_GeneralChatMutex;
    char m_MessageInput[256] = "";

    // File Pipeline variables
    char m_RemoteDirPath[256] = "";
    char m_LocalSavePath[256] = "";

    // Screen Share & Remote Viewport Integration Elements
    ScreenCapturer m_Capturer;
    GLuint m_ScreenTexture = 0;
    int m_LastTextureWidth = 0;
    int m_LastTextureHeight = 0;
    std::vector<uint8_t> m_FrameDataBuffer;
    bool m_TrackMouse = false;
    bool m_TrackKeyboard = false;
    bool m_ViewportFullscreen = false;

    // Background Screen Streaming Thread Variables
    std::thread m_screenShareThread;
    std::atomic<bool> m_isSharingScreen{false};

    uint64_t m_ActiveInputClientId = 0;   
    bool m_CanViewServerScreen = false;
    bool m_CanControlInput = false;

    
    void EnsureScreenTexture();
    void StartScreenSharingPipeline();
    void StopScreenSharingPipeline();

    void RenderRoleSelection();
    void RenderConnectionHome();
    void RenderActiveSession();
    int ImGuiKeyToVirtualKey(ImGuiKey key);
    void ProcessRemoteViewportInput(const ImVec2& screenSpaceImagePos, const ImVec2& renderSize);
    void RenderClientScreenView(NetworkManager& netManager, GLuint& screenTextureId, int frameWidth, int frameHeight);
};

#endif // UI_RENDERER_H