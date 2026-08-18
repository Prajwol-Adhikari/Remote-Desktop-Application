#include "uiRenderer.h"
#include <cstring>
#include <iostream>

UIRenderer::UIRenderer(NetworkManager &netManager)
    : m_netManager(netManager), m_CurrentState(STATE_ROLE_SELECTION), m_SelectedRole(ROLE_NONE), m_portInput(8080)
{
    strcpy_s(m_ipInput, "127.0.0.1");
    strcpy_s(m_statusMessage, "Awaiting node initialization...");
    ApplyCustomStyle();

    // 1. Server-side: when any connected client sends a chat message
    m_netManager.SetServerOnChatReceivedCallback([this](uint64_t clientId, const std::string &msg)
                                                 {
        std::string formattedMsg = "[Peer #" + std::to_string(clientId) + "]: " + msg;
        std::lock_guard<std::mutex> lock(m_ServerClientChats[clientId].chatMutex);
        m_ServerClientChats[clientId].messages.push_back({formattedMsg, ImVec4(0.8f, 0.9f, 0.2f, 1.0f)}); });

    // 2. Client-side: when the server sends a message back (routed to hostChat session)
    m_netManager.SetOnClientChatReceivedCallback([this](const std::string &msg)
                                                 {
        ClientChatSession &hostChat = m_ServerClientChats[0];
        std::lock_guard<std::mutex> lock(hostChat.chatMutex);
        hostChat.messages.push_back({msg, ImVec4(0.2f, 0.8f, 1.0f, 1.0f)}); });
}

UIRenderer::~UIRenderer()
{
    StopScreenSharingPipeline();
}

void UIRenderer::StartScreenSharingPipeline()
{
    if (m_isSharingScreen)
        return;

    std::cout << "[SCREEN] Starting server capture\n";

    m_Capturer.Start();

    m_isSharingScreen = true;

    m_screenShareThread = std::thread([this]()
                                      {
        std::vector<uint8_t> frame;

        while (m_isSharingScreen && m_Capturer.IsRunning())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 FPS cap

            if (m_TargetClientId == 0)
            {
                std::cout << "[SCREEN] No target client\n";
                continue;
            }

            if (m_Capturer.GetLatestFrame(frame))
            {
                if (!frame.empty())
                {
                    // Optional: skip logging every single frame to avoid console spam slowing down execution
                    m_netManager.SendScreenFrameToClient(
                        m_TargetClientId,
                        frame);
                }
            }
        } });
}

void UIRenderer::StopScreenSharingPipeline()
{
    m_isSharingScreen = false;
    if (m_screenShareThread.joinable())
    {
        m_screenShareThread.join();
    }
    m_Capturer.Stop();
}

void RenderClientScreenView(NetworkManager &netManager, GLuint &m_ScreenTexture, int &m_LastTextureWidth, int &m_LastTextureHeight)
{
    // 1. Explicitly initialize the OpenGL texture handle if not already created
    if (m_ScreenTexture == 0)
    {
        glGenTextures(1, &m_ScreenTexture);
        if (m_ScreenTexture == 0)
        {
            std::cout << "[OpenGL Error] Failed to generate screen texture handle!" << std::endl;
            return;
        }

        glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // 2. Poll the latest incoming frame
    std::vector<uint8_t> incomingPacket;
    if (netManager.GetLatestIncomingFrame(incomingPacket))
    {
        std::vector<uint8_t> rawPixels;
        uint32_t frameWidth = 0;
        uint32_t frameHeight = 0;

        // 3. Decompress / validate packet header and bounds
        if (ScreenCapturer::DecompressFrame(incomingPacket, rawPixels, frameWidth, frameHeight))
        {
            if (frameWidth > 0 && frameHeight > 0 && !rawPixels.empty())
            {
                glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);

                // 4. Update GPU texture data
                if (frameWidth != static_cast<uint32_t>(m_LastTextureWidth) || frameHeight != static_cast<uint32_t>(m_LastTextureHeight))
                {
                    glTexImage2D(
                        GL_TEXTURE_2D,
                        0,
                        GL_RGBA8,
                        frameWidth,
                        frameHeight,
                        0,
                        GL_BGRA,
                        GL_UNSIGNED_BYTE,
                        rawPixels.data());

                    m_LastTextureWidth = static_cast<int>(frameWidth);
                    m_LastTextureHeight = static_cast<int>(frameHeight);
                }
                else
                {
                    glTexSubImage2D(
                        GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        frameWidth,
                        frameHeight,
                        GL_BGRA,
                        GL_UNSIGNED_BYTE,
                        rawPixels.data());
                }

                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }
    }

    // 5. Render the screen view inside the ImGui tab/window maintaining correct aspect ratio
    if (m_ScreenTexture != 0 && m_LastTextureWidth > 0 && m_LastTextureHeight > 0)
    {
        ImVec2 availSize = ImGui::GetContentRegionAvail();
        if (availSize.x <= 0)
            availSize.x = 640.0f;
        if (availSize.y <= 0)
            availSize.y = 360.0f;

        float targetAspect = static_cast<float>(m_LastTextureWidth) / static_cast<float>(m_LastTextureHeight);
        ImVec2 drawSize = availSize;

        if (drawSize.x / drawSize.y > targetAspect)
        {
            drawSize.x = drawSize.y * targetAspect;
        }
        else
        {
            drawSize.y = drawSize.x / targetAspect;
        }

        // Center display canvas inside the available tab layout
        ImVec2 cursorPadding = ImVec2((availSize.x - drawSize.x) * 0.5f, (availSize.y - drawSize.y) * 0.5f);
        if (cursorPadding.x > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cursorPadding.x);
        if (cursorPadding.y > 0)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + cursorPadding.y);

        ImGui::Image((ImTextureID)(intptr_t)m_ScreenTexture, drawSize);
    }
    else
    {
        ImGui::Text("Waiting for incoming screen stream...");
    }
}

void UIRenderer::ApplyCustomStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    ImVec4 *colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.17f, 0.20f, 0.30f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_Border] = ImVec4(0.24f, 0.27f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.30f, 0.37f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.33f, 0.38f, 0.47f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.14f, 0.16f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.17f, 0.20f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.28f, 0.56f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.28f, 0.56f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.66f, 1.00f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.25f, 0.42f, 0.68f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.31f, 0.52f, 0.85f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.23f, 0.41f, 0.68f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.25f, 0.42f, 0.68f, 0.70f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.31f, 0.52f, 0.85f, 0.80f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.42f, 0.68f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.26f, 0.32f, 0.42f, 1.00f);
    colors[ImGuiCol_TabActive] = ImVec4(0.25f, 0.42f, 0.68f, 1.00f);
}

void UIRenderer::RenderUI()
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("RDP Master Core Workspace", nullptr, window_flags);

    switch (m_CurrentState)
    {
    case STATE_ROLE_SELECTION:
        RenderRoleSelection();
        break;
    case STATE_CONNECTION_HOME:
        RenderConnectionHome();
        break;
    case STATE_SESSION_ACTIVE:
        RenderActiveSession();
        break;
    }

    ImGui::End();
}

void UIRenderer::RenderRoleSelection()
{
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.30f);
    ImGui::BeginGroup();
    {
        float windowWidth = ImGui::GetWindowWidth();
        float itemWidth = 320.0f;

        ImGui::SetCursorPosX((windowWidth - itemWidth) * 0.5f);
        ImGui::Text("WELCOME TO UNIFIED PORTAL MONITOR");

        ImGui::SetCursorPosX((windowWidth - itemWidth) * 0.5f);
        ImGui::TextDisabled("Configure network module stack node below:");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::SetCursorPosX((windowWidth - itemWidth) * 0.5f);
        if (ImGui::Button("Initialize as HOST SERVER", ImVec2(itemWidth, 45)))
        {
            m_SelectedRole = ROLE_SERVER;
            m_CurrentState = STATE_CONNECTION_HOME;
        }

        ImGui::Spacing();
        ImGui::SetCursorPosX((windowWidth - itemWidth) * 0.5f);
        if (ImGui::Button("Initialize as REMOTE CLIENT", ImVec2(itemWidth, 45)))
        {
            m_SelectedRole = ROLE_CLIENT;
            m_CurrentState = STATE_CONNECTION_HOME;
        }
    }
    ImGui::EndGroup();
}

void UIRenderer::RenderConnectionHome()
{
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.20f);
    float windowWidth = ImGui::GetWindowWidth();
    float itemWidth = 380.0f;

    ImGui::SetCursorPosX((windowWidth - itemWidth) * 0.5f);
    ImGui::BeginChild("ConnectionPanel", ImVec2(itemWidth, 420), true);
    {
        if (m_SelectedRole == ROLE_SERVER)
        {
            ImGui::Text("=== SERVER CONFIGURATION ===");
            ImGui::Spacing();
            ImGui::InputInt("TCP Port", &m_portInput);
            ImGui::Spacing();

            if (!m_netManager.IsRunning())
            {
                if (ImGui::Button("Start Server Node", ImVec2(-1, 40)))
                {
                    m_netManager.StartServer(m_portInput);
                }

                std::string err = m_netManager.GetLastErrorMsg();
                if (!err.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Error: %s", err.c_str());
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Server running on port %d...", m_portInput);
                ImGui::TextDisabled("Broadcasting presence via UDP...");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Client Queue & Sessions:");
                ImGui::BeginChild("ClientQueueList", ImVec2(0, 160), true);
                {
                    auto localClients = m_netManager.GetClientUIInfos();

                    if (localClients.empty())
                    {
                        ImGui::TextDisabled("No clients connected yet.");
                    }
                    else
                    {
                        for (size_t i = 0; i < localClients.size(); ++i)
                        {
                            ImGui::PushID((int)i);
                            ImGui::Text("[%s]", localClients[i].ip.c_str());
                            ImGui::SameLine();

                            if (localClients[i].status == ConnectionStatus::WaitingForPermission)
                            {
                                ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Pending");
                                ImGui::SameLine();
                                if (ImGui::Button("Accept"))
                                {
                                    m_netManager.AcceptClient(i, true);
                                    m_CurrentState = STATE_SESSION_ACTIVE;
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Reject"))
                                {
                                    m_netManager.AcceptClient(i, false);
                                }
                            }
                            else if (localClients[i].status == ConnectionStatus::Connected)
                            {
                                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected");
                                ImGui::SameLine();
                                if (ImGui::Button("Disconnect"))
                                {
                                    m_netManager.DisconnectClient(i);
                                }
                            }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndChild();

                ImGui::Spacing();
                if (ImGui::Button("Stop Server", ImVec2(-1, 35)))
                {
                    m_netManager.StopServer();
                }
            }
        }
        else if (m_SelectedRole == ROLE_CLIENT)
        {
            ImGui::Text("=== CLIENT CONFIGURATION ===");
            ImGui::Spacing();
            ImGui::InputText("Server IP", m_ipInput, IM_ARRAYSIZE(m_ipInput));
            ImGui::InputInt("TCP Port", &m_portInput);
            ImGui::Spacing();

            ConnectionStatus status = m_netManager.GetStatus();
            if (status == ConnectionStatus::Disconnected)
            {
                if (ImGui::Button("Connect via IP/Port", ImVec2(-1, 35)))
                {
                    m_netManager.ConnectToServerAsync(m_ipInput, m_portInput);
                }

                std::string err = m_netManager.GetLastErrorMsg();
                if (!err.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "Error: %s", err.c_str());
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Automatic Discovery (UDP)");
                if (ImGui::Button("Scan Local Network", ImVec2(-1, 35)))
                {
                    m_netManager.StartServerSearch(m_portInput + 1);
                }

                auto servers = m_netManager.GetDiscoveredServers();
                if (!servers.empty())
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Discovered Servers:");
                    for (const auto &srvIP : servers)
                    {
                        std::string label = "Connect to " + srvIP;
                        if (ImGui::Button(label.c_str(), ImVec2(-1, 30)))
                        {
                            strcpy_s(m_ipInput, srvIP.c_str());
                            m_netManager.ConnectToServerAsync(m_ipInput, m_portInput);
                        }
                    }
                }
            }
            else if (status == ConnectionStatus::WaitingForPermission)
            {
                ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Connecting / Waiting for server permission...");
                if (ImGui::Button("Cancel", ImVec2(-1, 35)))
                {
                    m_netManager.Disconnect();
                }
            }
            else if (status == ConnectionStatus::Rejected)
            {
                ImGui::TextColored(ImVec4(1, 0, 0, 1), "Connection Rejected by Server.");
                std::string err = m_netManager.GetLastErrorMsg();
                if (!err.empty())
                {
                    ImGui::TextWrapped("%s", err.c_str());
                }
                if (ImGui::Button("Try Again", ImVec2(-1, 35)))
                {
                    m_netManager.Disconnect();
                }
            }
            else if (status == ConnectionStatus::Connected)
            {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Connected to Server!");
                ImGui::Spacing();
                if (ImGui::Button("Enter Active Session", ImVec2(-1, 40)))
                {
                    m_CurrentState = STATE_SESSION_ACTIVE;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("<< Back to Role Selection", ImVec2(-1, 30)))
        {
            m_netManager.StopServer();
            m_netManager.Disconnect();
            m_CurrentState = STATE_ROLE_SELECTION;
        }
    }
    ImGui::EndChild();
}

void UIRenderer::RenderActiveSession()
{
    // ==========================================
    // 0. AUTO-DISCONNECT / CONNECTION LOSS MONITOR
    // ==========================================
    m_CanViewServerScreen = m_netManager.CanViewServerScreen();

    if (m_SelectedRole == ROLE_SERVER)
    {
        // If the server stopped unexpectedly
        if (!m_netManager.IsServerRunning())
        {
            m_netManager.StopServer();
            m_SelectedRole = ROLE_NONE;
            m_CurrentState = STATE_ROLE_SELECTION;
            return;
        }
    }
    else if (m_SelectedRole == ROLE_CLIENT)
    {
        // Only trigger connection loss if the client has been fully connected
        // long enough for the network loop to stabilize, or avoid checking on frame 1.
    }

    // ==========================================
    // 1. DEDICATED HOST SERVER DASHBOARD (MULTI-CLIENT)
    // ==========================================
    if (m_SelectedRole == ROLE_SERVER)
    {
        ImGui::Text("Active Hosting Port: %s | Role: Host Server Dashboard", m_Port);
        ImGui::SameLine(ImGui::GetWindowWidth() - 140);
        if (ImGui::Button("Stop & Disconnect", ImVec2(130, 0)))
        {
            m_netManager.StopServer();
            m_SelectedRole = ROLE_NONE;
            m_CurrentState = STATE_ROLE_SELECTION;
            return;
        }
        ImGui::Separator();
        ImGui::Spacing();

        // --- SERVER UI TAB BLOCK ---
        if (ImGui::BeginTabBar("ServerManagementTabBar"))
        {
            // --- TAB 1: CONNECTED & PENDING DEVICES ---
            if (ImGui::BeginTabItem("Connected Devices & Permissions"))
            {
                ImGui::Spacing();
                ImGui::Text("Peers & Connection Requests:");
                ImGui::Spacing();

                auto rawClients = m_netManager.GetClientUIInfos();

                // Filter out disconnected or rejected clients so they are removed from the server table view
                std::vector<ClientUIInfo> localClients;
                for (const auto &c : rawClients)
                {
                    if (c.status != ConnectionStatus::Disconnected && c.status != ConnectionStatus::Rejected)
                    {
                        localClients.push_back(c);
                    }
                }

                if (ImGui::BeginTable("ServerClientsTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(-1, 250)))
                {
                    ImGui::TableSetupColumn("Peer ID & IP", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                    ImGui::TableSetupColumn("Target Select", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Permissions / Approval", ImGuiTableColumnFlags_WidthFixed, 220.0f);
                    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Status Info", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    if (localClients.empty())
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("No active peers or requests.");
                    }
                    else
                    {
                        for (size_t i = 0; i < localClients.size(); ++i)
                        {
                            uint64_t currentClientId = localClients[i].id;
                            std::string idStr = std::to_string(currentClientId);

                            ImGui::PushID(static_cast<int>(currentClientId));
                            ImGui::TableNextRow();

                            // Column 0: Peer ID & IP
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("ID: %llu\n%s", static_cast<unsigned long long>(currentClientId), localClients[i].ip.c_str());

                            // Column 1: Target Selection Button
                            ImGui::TableSetColumnIndex(1);
                            bool isSelected = (m_TargetClientId == currentClientId);
                            if (localClients[i].status == ConnectionStatus::Connected)
                            {
                                if (isSelected)
                                {
                                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
                                    ImGui::Button("Selected", ImVec2(90, 0));
                                    ImGui::PopStyleColor();
                                }
                                else
                                {
                                    if (ImGui::Button(("Set Target##" + idStr).c_str(), ImVec2(90, 0)))
                                    {
                                        m_TargetClientId = currentClientId;
                                    }
                                }
                            }
                            else
                            {
                                ImGui::TextDisabled("Awaiting Auth");
                            }

                            // Column 2: Permissions or Pending Approval Action Buttons
                            ImGui::TableSetColumnIndex(2);
                            if (localClients[i].status == ConnectionStatus::WaitingForPermission)
                            {
                                if (ImGui::Button(("Accept##" + idStr).c_str(), ImVec2(90, 0)))
                                {
                                    m_netManager.AcceptClient(currentClientId, true);
                                }
                                ImGui::SameLine();
                                if (ImGui::Button(("Reject##" + idStr).c_str(), ImVec2(90, 0)))
                                {
                                    m_netManager.AcceptClient(currentClientId, false);
                                    if (m_TargetClientId == currentClientId)
                                    {
                                        StopScreenSharingPipeline();
                                        m_TargetClientId = 0;
                                    }
                                }
                            }
                            else
                            {
                                bool canShareScreen = localClients[i].canViewServerScreen;

                                // Screen Share Checkbox
                                if (ImGui::Checkbox(("Screen Share##" + idStr).c_str(), &canShareScreen))
                                {
                                    if (canShareScreen)
                                    {
                                        for (const auto &client : localClients)
                                        {
                                            if (client.id != currentClientId)
                                            {
                                                m_netManager.SetClientScreenPermission(client.id, false);
                                            }
                                        }

                                        m_netManager.SetClientScreenPermission(currentClientId, true);
                                        m_TargetClientId = currentClientId;

                                        std::cout << "[SCREEN] Permission granted"
                                                  << " clientId=" << currentClientId
                                                  << " target=" << m_TargetClientId
                                                  << std::endl;

                                        StartScreenSharingPipeline();
                                    }
                                    else
                                    {
                                        m_netManager.SetClientScreenPermission(currentClientId, false);
                                        if (m_TargetClientId == currentClientId)
                                        {
                                            StopScreenSharingPipeline();
                                            m_TargetClientId = 0;
                                        }
                                    }
                                }
                            }

                            // Column 3: Actions (Disconnect Peer)
                            ImGui::TableSetColumnIndex(3);
                            if (ImGui::Button(("Disconnect##" + idStr).c_str(), ImVec2(100, 0)))
                            {
                                m_netManager.DisconnectClient(currentClientId);
                                if (m_TargetClientId == currentClientId)
                                {
                                    StopScreenSharingPipeline();
                                    m_TargetClientId = 0;
                                }
                                if (m_ActiveInputClientId == currentClientId)
                                {
                                    m_ActiveInputClientId = 0;
                                }
                            }

                            // Column 4: Status Info
                            ImGui::TableSetColumnIndex(4);
                            if (localClients[i].status == ConnectionStatus::Connected)
                            {
                                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Connected");
                            }
                            else if (localClients[i].status == ConnectionStatus::WaitingForPermission)
                            {
                                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Pending Request");
                            }

                            ImGui::PopID();
                        }
                    }
                    ImGui::EndTable();
                }

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Active Target ID: #%llu | Input Control ID: #%llu",
                                   static_cast<unsigned long long>(m_TargetClientId),
                                   static_cast<unsigned long long>(m_ActiveInputClientId));
                ImGui::EndTabItem();
            }

            // --- TAB 2: INDIVIDUAL CHAT SYSTEM ---
            if (ImGui::BeginTabItem("Individual Chat System"))
            {
                ImGui::Text("Console Messenger (Target Peer ID: #%llu)", static_cast<unsigned long long>(m_TargetClientId));
                ImGui::Spacing();

                if (m_TargetClientId == 0)
                {
                    ImGui::TextDisabled("Please select an approved target client from the 'Connected Devices' tab first.");
                }
                else
                {
                    ClientChatSession &targetChat = m_ServerClientChats[m_TargetClientId];

                    if (ImGui::BeginChild("ServerMessageHistory", ImVec2(-1, ImGui::GetWindowHeight() * 0.45f), true))
                    {
                        std::lock_guard<std::mutex> lock(targetChat.chatMutex);
                        for (size_t idx = 0; idx < targetChat.messages.size(); ++idx)
                        {
                            ImGui::PushID(static_cast<int>(idx));
                            ImGui::TextColored(targetChat.messages[idx].second, "%s", targetChat.messages[idx].first.c_str());
                            ImGui::PopID();
                        }
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                            ImGui::SetScrollHereY(1.0f);
                    }
                    ImGui::EndChild();

                    ImGui::PushItemWidth(ImGui::GetWindowWidth() - 140);
                    bool enterPressed = ImGui::InputText("##serverchatinput", m_MessageInput, sizeof(m_MessageInput), ImGuiInputTextFlags_EnterReturnsTrue);
                    ImGui::PopItemWidth();
                    ImGui::SameLine();

                    bool sendClicked = ImGui::Button("Send to Client", ImVec2(120, 0));

                    if ((enterPressed || sendClicked) && std::strlen(m_MessageInput) > 0)
                    {
                        std::string msgText(m_MessageInput);

                        // 1. Serialize chat message into a byte vector payload (Length prefix + string data)
                        std::vector<uint8_t> payload;
                        uint32_t strLen = static_cast<uint32_t>(msgText.size());
                        payload.resize(sizeof(uint32_t) + strLen);
                        std::memcpy(payload.data(), &strLen, sizeof(uint32_t));
                        std::memcpy(payload.data() + sizeof(uint32_t), msgText.data(), strLen);

                        // 2. Transmit the packet over the network to the specific target client
                        m_netManager.SendServerChatToClient(m_TargetClientId, payload);

                        // 3. Push message locally to the server's view buffer thread-safely matching existing structure
                        std::string displayMsg = "[Server -> Peer #" + std::to_string(m_TargetClientId) + "]: " + msgText;
                        {
                            std::lock_guard<std::mutex> lock(targetChat.chatMutex);
                            targetChat.messages.push_back({displayMsg, ImVec4(0.2f, 0.8f, 1.0f, 1.0f)});
                        }

                        std::memset(m_MessageInput, 0, sizeof(m_MessageInput));

                        // Keep focus active on the input field for continuous typing if Enter was pressed
                        if (enterPressed)
                        {
                            ImGui::SetKeyboardFocusHere(-1);
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        return;
    }

    // ==========================================
    // 2. REMOTE CLIENT DASHBOARD
    // ==========================================
    
if (m_SelectedRole == ROLE_CLIENT)
    {
        if (!m_netManager.IsClientConnected())
        {
            m_netManager.Disconnect();            // Clean up client sockets & threads
            m_SelectedRole = ROLE_NONE;            // Reset selected role
            m_CurrentState = STATE_ROLE_SELECTION; // Return to role selection menu
            return;                                // Stop rendering this frame
        }
        if (ImGui::BeginTabBar("ClientManagementTabBar"))
        {
            // --- TAB 1: SESSION INFO ---
            if (ImGui::BeginTabItem("Session Info"))
            {
                ImGui::Spacing();
                ImGui::Text("Connected to Server (%s)", m_ipInput);
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Disconnect & End Session", ImVec2(200, 40)))
                {
                    m_netManager.Disconnect();
                    m_SelectedRole = ROLE_NONE;
                    m_CurrentState = STATE_ROLE_SELECTION;
                }
                ImGui::EndTabItem();
            }

            // --- TAB 2: REMOTE SCREEN STREAM ---
            if (ImGui::BeginTabItem("Remote Screen Stream"))
            {
                EnsureScreenTexture();

                ImGui::Checkbox("Track/Inject Mouse", &m_TrackMouse);
                ImGui::SameLine();
                ImGui::Checkbox("Capture/Inject Keyboard", &m_TrackKeyboard);
                ImGui::SameLine();
                ImGui::Checkbox("Stretch Fullscreen Canvas", &m_ViewportFullscreen);

                ImGui::Spacing();

                if (!m_CanViewServerScreen)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "[Waiting]: Host has not granted screen viewing permissions yet.");
                }
                else
                {
                    std::vector<uint8_t> decodedPixels;
                    uint32_t curWidth = 0, curHeight = 0;

                    if (m_netManager.GetLatestDecodedFrame(decodedPixels, curWidth, curHeight) && !decodedPixels.empty())
                    {
                        if (curWidth > 0 && curHeight > 0)
                        {
                            glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);
                            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                            if (static_cast<int>(curWidth) != m_LastTextureWidth || static_cast<int>(curHeight) != m_LastTextureHeight)
                            {
                                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, curWidth, curHeight, 0, GL_BGRA, GL_UNSIGNED_BYTE, decodedPixels.data());
                                m_LastTextureWidth = static_cast<int>(curWidth);
                                m_LastTextureHeight = static_cast<int>(curHeight);
                            }
                            else
                            {
                                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, curWidth, curHeight, GL_BGRA, GL_UNSIGNED_BYTE, decodedPixels.data());
                            }
                            glBindTexture(GL_TEXTURE_2D, 0);
                        }
                    }
                }

                if (m_CanViewServerScreen && m_ScreenTexture != 0)
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Resolution: %dx%d",
                                       m_LastTextureWidth, m_LastTextureHeight);
                }

                ImVec2 viewportSize = m_ViewportFullscreen ? ImVec2(-1, -1) : ImVec2(-1, ImGui::GetWindowHeight() * 0.60f);

                if (ImGui::BeginChild("VideoFrameBufferViewport", viewportSize, true, ImGuiWindowFlags_NoScrollbar))
                {
                    if (!m_CanViewServerScreen)
                    {
                        ImGui::TextDisabled("Stream disabled. Waiting for permission from host...");
                    }
                    else if (m_ScreenTexture != 0 && m_LastTextureWidth > 0 && m_LastTextureHeight > 0)
                    {
                        ImVec2 availSize = ImGui::GetContentRegionAvail();
                        ImVec2 renderSize = availSize;

                        float aspect = static_cast<float>(m_LastTextureWidth) / static_cast<float>(m_LastTextureHeight);
                        if (renderSize.x / renderSize.y > aspect)
                        {
                            renderSize.x = renderSize.y * aspect;
                        }
                        else
                        {
                            renderSize.y = renderSize.x / aspect;
                        }

                        float offsetX = (availSize.x - renderSize.x) * 0.5f;
                        float offsetY = (availSize.y - renderSize.y) * 0.5f;

                        ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + offsetX, ImGui::GetCursorPosY() + offsetY));

                        ImVec2 screenSpaceImagePos = ImGui::GetCursorScreenPos();

                        ImGui::Image((void *)(intptr_t)m_ScreenTexture, renderSize);

                        ProcessRemoteViewportInput(screenSpaceImagePos, renderSize);
                    }
                    else
                    {
                        ImGui::TextDisabled("Awaiting remote capture stream frames...");
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            // --- TAB 3: CONSOLE MESSENGER ---
            if (ImGui::BeginTabItem("Console Messenger"))
            {
                ImGui::Text("Console Messenger (Connected to Host)");
                ImGui::Spacing();

                ClientChatSession &hostChat = m_ServerClientChats[0];

                if (ImGui::BeginChild("ClientMessageHistory", ImVec2(-1, ImGui::GetWindowHeight() * 0.52f), true))
                {
                    std::lock_guard<std::mutex> lock(hostChat.chatMutex);
                    for (size_t idx = 0; idx < hostChat.messages.size(); ++idx)
                    {
                        ImGui::PushID(static_cast<int>(idx));
                        ImGui::TextColored(hostChat.messages[idx].second, "%s", hostChat.messages[idx].first.c_str());
                        ImGui::PopID();
                    }
                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                        ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();

                ImGui::PushItemWidth(ImGui::GetWindowWidth() - 140);
                bool enterPressed = ImGui::InputText("##clientchatinput", m_MessageInput, sizeof(m_MessageInput), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::PopItemWidth();
                ImGui::SameLine();

                bool sendClicked = ImGui::Button("Send Packet", ImVec2(110, 0));

                if ((enterPressed || sendClicked) && std::strlen(m_MessageInput) > 0)
                {
                    std::string msgText(m_MessageInput);

                    std::vector<uint8_t> payload;
                    uint32_t strLen = static_cast<uint32_t>(msgText.size());

                    payload.resize(sizeof(uint32_t) + strLen);
                    std::memcpy(payload.data(), &strLen, sizeof(uint32_t));
                    std::memcpy(payload.data() + sizeof(uint32_t), msgText.data(), strLen);

                    m_netManager.SendChatPacket(payload);

                    {
                        std::lock_guard<std::mutex> lock(hostChat.chatMutex);
                        std::string userMsg = "[Client]: " + msgText;
                        hostChat.messages.push_back({userMsg, ImVec4(0.95f, 0.96f, 0.98f, 1.00f)});
                    }

                    std::memset(m_MessageInput, 0, sizeof(m_MessageInput));

                    if (enterPressed)
                    {
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
}

int UIRenderer::ImGuiKeyToVirtualKey(ImGuiKey key)
{
    switch (key)
    {
    case ImGuiKey_Tab:
        return VK_TAB;
    case ImGuiKey_LeftArrow:
        return VK_LEFT;
    case ImGuiKey_RightArrow:
        return VK_RIGHT;
    case ImGuiKey_UpArrow:
        return VK_UP;
    case ImGuiKey_DownArrow:
        return VK_DOWN;
    case ImGuiKey_PageUp:
        return VK_PRIOR;
    case ImGuiKey_PageDown:
        return VK_NEXT;
    case ImGuiKey_Home:
        return VK_HOME;
    case ImGuiKey_End:
        return VK_END;
    case ImGuiKey_Insert:
        return VK_INSERT;
    case ImGuiKey_Delete:
        return VK_DELETE;
    case ImGuiKey_Backspace:
        return VK_BACK;
    case ImGuiKey_Space:
        return VK_SPACE;
    case ImGuiKey_Enter:
        return VK_RETURN;
    case ImGuiKey_Escape:
        return VK_ESCAPE;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
        return VK_CONTROL;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
        return VK_SHIFT;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
        return VK_MENU;
    case ImGuiKey_0:
        return '0';
    case ImGuiKey_1:
        return '1';
    case ImGuiKey_2:
        return '2';
    case ImGuiKey_3:
        return '3';
    case ImGuiKey_4:
        return '4';
    case ImGuiKey_5:
        return '5';
    case ImGuiKey_6:
        return '6';
    case ImGuiKey_7:
        return '7';
    case ImGuiKey_8:
        return '8';
    case ImGuiKey_9:
        return '9';
    case ImGuiKey_A:
        return 'A';
    case ImGuiKey_B:
        return 'B';
    case ImGuiKey_C:
        return 'C';
    case ImGuiKey_D:
        return 'D';
    case ImGuiKey_E:
        return 'E';
    case ImGuiKey_F:
        return 'F';
    case ImGuiKey_G:
        return 'G';
    case ImGuiKey_H:
        return 'H';
    case ImGuiKey_I:
        return 'I';
    case ImGuiKey_J:
        return 'J';
    case ImGuiKey_K:
        return 'K';
    case ImGuiKey_L:
        return 'L';
    case ImGuiKey_M:
        return 'M';
    case ImGuiKey_N:
        return 'N';
    case ImGuiKey_O:
        return 'O';
    case ImGuiKey_P:
        return 'P';
    case ImGuiKey_Q:
        return 'Q';
    case ImGuiKey_R:
        return 'R';
    case ImGuiKey_S:
        return 'S';
    case ImGuiKey_T:
        return 'T';
    case ImGuiKey_U:
        return 'U';
    case ImGuiKey_V:
        return 'V';
    case ImGuiKey_W:
        return 'W';
    case ImGuiKey_X:
        return 'X';
    case ImGuiKey_Y:
        return 'Y';
    case ImGuiKey_Z:
        return 'Z';
    default:
        return 0;
    }
}

void UIRenderer::ProcessRemoteViewportInput(const ImVec2 &screenSpaceImagePos, const ImVec2 &renderSize)
{
    if (renderSize.x <= 0.0f || renderSize.y <= 0.0f)
        return;

    ImGuiIO &io = ImGui::GetIO();

    // Check if the mouse is currently hovering over the rendered screen image bounds directly
    ImVec2 mousePos = io.MousePos;
    bool isHoveringImage = (mousePos.x >= screenSpaceImagePos.x &&
                            mousePos.x <= (screenSpaceImagePos.x + renderSize.x) &&
                            mousePos.y >= screenSpaceImagePos.y &&
                            mousePos.y <= (screenSpaceImagePos.y + renderSize.y));

    // ==========================================
    // 1. MOUSE INJECTION LOGIC
    // ==========================================
    if (m_TrackMouse && isHoveringImage)
    {
        float localX = mousePos.x - screenSpaceImagePos.x;
        float localY = mousePos.y - screenSpaceImagePos.y;

        // Always send movement updates if the mouse is moving inside the viewport
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f)
        {
            auto packet = InputInjector::SerializeMouse(localX, localY, renderSize.x, renderSize.y, MOUSEEVENTF_MOVE);
            m_netManager.SendInputPacket(packet);
        }

        // Left Click Down / Up
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            auto packet = InputInjector::SerializeMouse(localX, localY, renderSize.x, renderSize.y, MOUSEEVENTF_LEFTDOWN);
            m_netManager.SendInputPacket(packet);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            auto packet = InputInjector::SerializeMouse(localX, localY, renderSize.x, renderSize.y, MOUSEEVENTF_LEFTUP);
            m_netManager.SendInputPacket(packet);
        }

        // Right Click Down / Up
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            auto packet = InputInjector::SerializeMouse(localX, localY, renderSize.x, renderSize.y, MOUSEEVENTF_RIGHTDOWN);
            m_netManager.SendInputPacket(packet);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        {
            auto packet = InputInjector::SerializeMouse(localX, localY, renderSize.x, renderSize.y, MOUSEEVENTF_RIGHTUP);
            m_netManager.SendInputPacket(packet);
        }

        // Mouse Wheel Scroll
        if (io.MouseWheel != 0.0f)
        {
            auto packet = InputInjector::SerializeMouse(localX, localY, renderSize.x, renderSize.y, MOUSEEVENTF_WHEEL, static_cast<DWORD>(io.MouseWheel * WHEEL_DELTA));
            m_netManager.SendInputPacket(packet);
        }
    }

    // ==========================================
    // 2. KEYBOARD INJECTION LOGIC
    // ==========================================
    if (m_TrackKeyboard && (isHoveringImage || ImGui::IsWindowFocused()))
    {
        for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
        {
            ImGuiKey imguiKey = static_cast<ImGuiKey>(key);
            int vk = ImGuiKeyToVirtualKey(imguiKey);
            if (vk != 0)
            {
                if (ImGui::IsKeyPressed(imguiKey, false))
                {
                    auto packet = InputInjector::SerializeKeyboard(vk, false); // false = Key Down
                    m_netManager.SendInputPacket(packet);
                }
                if (ImGui::IsKeyReleased(imguiKey))
                {
                    auto packet = InputInjector::SerializeKeyboard(vk, true); // true = Key Up
                    m_netManager.SendInputPacket(packet);
                }
            }
        }
    }
}

void UIRenderer::EnsureScreenTexture()
{
    if (m_ScreenTexture != 0)
        return;

    glGenTextures(1, &m_ScreenTexture);
    if (m_ScreenTexture == 0)
    {
        std::cerr << "[UIRenderer] Failed to create screen texture\n";
        return;
    }

    glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}