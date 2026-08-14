#include "NetworkManager.h"
#include <iostream>
#include <chrono>
#include <algorithm>
// worked

NetworkManager::NetworkManager()
    : m_listenSocket(INVALID_SOCKET), m_activeSocket(INVALID_SOCKET),
      m_udpBroadcastSocket(INVALID_SOCKET), m_udpSearchSocket(INVALID_SOCKET),
      m_isInitialized(false), m_isRunning(false), m_isBroadcasting(false), m_isSearching(false),
      m_status(ConnectionStatus::Disconnected)
{
    InitializeWinsock();
}

NetworkManager::~NetworkManager()
{
    Disconnect();
    StopServer();
    StopDiscoveryBroadcast();
    StopServerSearch();
    CleanupWinsock();
}

bool NetworkManager::InitializeWinsock()
{
    if (m_isInitialized)
        return true;
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
        return false;
    m_isInitialized = true;
    return true;
}

bool NetworkManager::StartServer(int port)
{
    if (!m_isInitialized)
    {
        m_lastErrorMsg = "Winsock not initialized.";
        return false;
    }

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET)
    {
        m_lastErrorMsg = "Failed to create TCP socket: " + std::to_string(WSAGetLastError());
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(m_listenSocket, (sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        m_lastErrorMsg = "Bind failed (Port in use): " + std::to_string(WSAGetLastError());
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        m_lastErrorMsg = "Listen failed: " + std::to_string(WSAGetLastError());
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    m_isRunning = true;
    m_lastErrorMsg = "";
    m_serverListenThread = std::thread(&NetworkManager::ListenLoop, this);
    StartDiscoveryBroadcast(port + 1);
    return true;
}

void NetworkManager::StopServer()
{
    m_isRunning = false;
    StopDiscoveryBroadcast();

    if (m_listenSocket != INVALID_SOCKET)
    {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        for (auto &client : m_connectedClients)
        {
            if (client.socket != INVALID_SOCKET)
            {
                shutdown(client.socket, SD_BOTH);
                closesocket(client.socket);
                client.socket = INVALID_SOCKET;
            }
        }
        m_connectedClients.clear();
    }

    if (m_serverListenThread.joinable())
    {
        m_serverListenThread.join();
    }
}

void NetworkManager::ListenLoop()
{
    while (m_isRunning)
    {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        SOCKET newClientSock = accept(m_listenSocket, (struct sockaddr *)&clientAddr, &addrLen);

        if (newClientSock == INVALID_SOCKET)
        {
            if (!m_isRunning)
                break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr, INET_ADDRSTRLEN);

        std::lock_guard<std::mutex> lock(m_clientMutex);

        m_connectedClients.emplace_back();
        size_t index = m_connectedClients.size() - 1;

        m_connectedClients[index].socket = newClientSock;
        m_connectedClients[index].ip = std::string(ipStr);
        m_connectedClients[index].status = ConnectionStatus::WaitingForPermission;
        m_connectedClients[index].id = m_nextClientId++;

        m_connectedClients[index].handlerThread = std::thread(&NetworkManager::ClientHandlerLoop, this, newClientSock);
    }
}

void NetworkManager::AcceptClient(uint64_t clientId, bool accept)
{
    std::lock_guard<std::mutex> lock(m_clientMutex);
    for (auto &client : m_connectedClients)
    {
        if (client.id == clientId)
        {
            if (accept)
            {
                client.status = ConnectionStatus::Connected;
                std::cout << "[SERVER] Accepted client " << clientId << std::endl;
            }
            else
            {
                client.status = ConnectionStatus::Rejected;
                std::cout << "[SERVER] Rejected client " << clientId << std::endl;
            }
            break;
        }
    }
}

void NetworkManager::DisconnectClient(size_t index)
{
    std::lock_guard<std::mutex> lock(m_clientMutex);
    if (index < m_connectedClients.size())
    {
        SOCKET sock = m_connectedClients[index].socket;
        if (sock != INVALID_SOCKET)
        {
            closesocket(sock);
            m_connectedClients[index].socket = INVALID_SOCKET;
        }

        if (m_connectedClients[index].handlerThread.joinable())
        {
            m_connectedClients[index].handlerThread.detach();
        }

        m_connectedClients.erase(m_connectedClients.begin() + index);
    }
}

void NetworkManager::ClientHandlerLoop(SOCKET sock)
{
    DWORD sendTimeout = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&sendTimeout, sizeof(sendTimeout));

    DWORD recvTimeout = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&recvTimeout, sizeof(recvTimeout));

    uint64_t currentClientId = 0;

    // Phase 1: Wait for Server to Accept or Reject
    while (m_isRunning)
    {
        ConnectionStatus currentStatus = ConnectionStatus::Disconnected;
        {
            std::lock_guard<std::mutex> lock(m_clientMutex);
            bool found = false;
            for (const auto &client : m_connectedClients)
            {
                if (client.socket == sock)
                {
                    currentStatus = client.status;
                    currentClientId = client.id;
                    found = true;
                    break;
                }
            }
            if (!found)
                return;
        }

        if (currentStatus == ConnectionStatus::Connected)
        {
            uint8_t packetType = 4;
            uint32_t payloadSize = 0;
            send(sock, reinterpret_cast<const char *>(&packetType), sizeof(packetType), 0);
            send(sock, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize), 0);
            break;
        }
        else if (currentStatus == ConnectionStatus::Rejected)
        {
            uint8_t packetType = 5;
            uint32_t payloadSize = 0;
            send(sock, reinterpret_cast<const char *>(&packetType), sizeof(packetType), 0);
            send(sock, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize), 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Phase 2: Active Connection Listener
    while (m_isRunning)
    {
        uint8_t packetType = 0;
        int recvResult = RecvAll(sock, reinterpret_cast<char *>(&packetType), sizeof(packetType));

        if (recvResult <= 0)
        {
            break;
        }

        uint32_t payloadSize = 0;
        if (!RecvAll(sock, reinterpret_cast<char *>(&payloadSize), sizeof(payloadSize)))
        {
            break;
        }

        if (payloadSize > 0 && payloadSize < 50 * 1024 * 1024)
        {
            std::vector<uint8_t> payload(payloadSize);
            if (!RecvAll(sock, reinterpret_cast<char *>(payload.data()), payloadSize))
            {
                break;
            }

            if (packetType == 1) // Input Packet
            {
                // No permission flags required anymore; inject directly!
                m_InputInjector.DeserializeAndInject(payload);
            }
            else if (packetType == 2) // Incoming Chat from Client
            {
                if (payload.size() >= sizeof(uint32_t))
                {
                    uint32_t strLen = 0;
                    std::memcpy(&strLen, payload.data(), sizeof(uint32_t));

                    if (payload.size() >= sizeof(uint32_t) + strLen)
                    {
                        std::string clientMsg(reinterpret_cast<const char*>(payload.data() + sizeof(uint32_t)), strLen);

                        // Fire callback to UI layer
                        if (m_serverChatCallback)
                        {
                            m_serverChatCallback(currentClientId, clientMsg);
                        }
                    }
                }
            }
        }
        else if (payloadSize >= 50 * 1024 * 1024)
        {
            break;
        }
    }

    // Phase 3: Safe Exit Marking
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        for (auto &client : m_connectedClients)
        {
            if (client.socket == sock)
            {
                if (client.socket != INVALID_SOCKET)
                {
                    closesocket(client.socket);
                    client.socket = INVALID_SOCKET;
                }
                client.status = ConnectionStatus::Disconnected;

                if (m_ActiveInputClientId == client.id)
                {
                    m_ActiveInputClientId = 0;
                }
                break;
            }
        }
    }
}

void NetworkManager::ConnectAsyncWorker(std::string ip, int port)
{
    if (!m_isInitialized)
    {
        m_lastErrorMsg = "Winsock not initialized.";
        m_status = ConnectionStatus::Disconnected;
        return;
    }
    StopServerSearch();

    SOCKET tempSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tempSocket == INVALID_SOCKET)
    {
        m_lastErrorMsg = "Failed to create client socket.";
        m_status = ConnectionStatus::Disconnected;
        return;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0)
    {
        m_lastErrorMsg = "Invalid IP address format.";
        closesocket(tempSocket);
        m_status = ConnectionStatus::Disconnected;
        return;
    }

    if (connect(tempSocket, (sockaddr *)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
    {
        m_lastErrorMsg = "Connection refused or server unreachable (" + std::to_string(WSAGetLastError()) + ")";
        closesocket(tempSocket);
        m_status = ConnectionStatus::Disconnected;
        return;
    }

    m_activeSocket = tempSocket;

    // Use a reasonable timeout only for initial handshake response
    DWORD timeout = 15000;
    setsockopt(m_activeSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    uint8_t responseType = 0;
    int bytesReceived = recv(m_activeSocket, reinterpret_cast<char *>(&responseType), sizeof(responseType), 0);

    if (bytesReceived > 0)
    {
        uint32_t payloadSize = 0;
        recv(m_activeSocket, reinterpret_cast<char *>(&payloadSize), sizeof(payloadSize), 0);
        if (payloadSize > 0 && payloadSize < 1024 * 1024)
        {
            std::vector<uint8_t> dummy(payloadSize);
            recv(m_activeSocket, reinterpret_cast<char *>(dummy.data()), payloadSize, 0);
        }

        if (responseType == 4)
        {
            m_status = ConnectionStatus::Connected;
            m_lastErrorMsg = "";

            // Disable timeouts completely for continuous streaming
            DWORD zeroTimeout = 0;
            setsockopt(m_activeSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&zeroTimeout, sizeof(zeroTimeout));
            setsockopt(m_activeSocket, SOL_SOCKET, SO_SNDTIMEO, (char *)&zeroTimeout, sizeof(zeroTimeout));

            if (m_clientReceiveThread.joinable())
            {
                m_clientReceiveThread.join();
            }
            m_clientReceiveThread = std::thread(&NetworkManager::ClientReceiveLoop, this);
            return;
        }
        else
        {
            m_status = ConnectionStatus::Rejected;
            m_lastErrorMsg = "Connection request rejected by server.";
        }
    }
    else
    {
        m_status = ConnectionStatus::Disconnected;
        m_lastErrorMsg = "Server response timeout or connection lost.";
    }

    Disconnect();
}

void NetworkManager::ConnectToServerAsync(const std::string &ip, int port)
{
    if (m_clientConnectThread.joinable())
    {
        m_clientConnectThread.join();
    }
    m_status = ConnectionStatus::WaitingForPermission;
    m_clientConnectThread = std::thread(&NetworkManager::ConnectAsyncWorker, this, ip, port);
}

void NetworkManager::Disconnect()
{
    m_status = ConnectionStatus::Disconnected;

    if (m_activeSocket != INVALID_SOCKET)
    {
        closesocket(m_activeSocket);
        m_activeSocket = INVALID_SOCKET;
    }

    if (m_clientReceiveThread.joinable())
    {
        m_clientReceiveThread.join();
    }
}

bool NetworkManager::StartDiscoveryBroadcast(int port)
{
    if (m_isBroadcasting)
        return true;
    m_udpBroadcastSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpBroadcastSocket == INVALID_SOCKET)
        return false;
    BOOL broadcast = TRUE;
    setsockopt(m_udpBroadcastSocket, SOL_SOCKET, SO_BROADCAST, (char *)&broadcast, sizeof(broadcast));
    m_isBroadcasting = true;
    m_broadcastThread = std::thread(&NetworkManager::BroadcastLoop, this, port);
    return true;
}

void NetworkManager::StopDiscoveryBroadcast()
{
    m_isBroadcasting = false;
    if (m_udpBroadcastSocket != INVALID_SOCKET)
    {
        closesocket(m_udpBroadcastSocket);
        m_udpBroadcastSocket = INVALID_SOCKET;
    }
    if (m_broadcastThread.joinable())
        m_broadcastThread.join();
}

void NetworkManager::BroadcastLoop(int port)
{
    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_port = htons(port);
    inet_pton(AF_INET, "255.255.255.255", &broadcastAddr.sin_addr);
    std::string msg = "RDP_SERVER_DISCOVERY";
    while (m_isBroadcasting)
    {
        sendto(m_udpBroadcastSocket, msg.c_str(), (int)msg.size(), 0, (sockaddr *)&broadcastAddr, sizeof(broadcastAddr));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

bool NetworkManager::StartServerSearch(int port)
{
    if (m_isSearching)
        return true;

    m_udpSearchSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSearchSocket == INVALID_SOCKET)
    {
        m_lastErrorMsg = "Failed to create UDP search socket.";
        return false;
    }

    int opt = 1;
    setsockopt(m_udpSearchSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(static_cast<u_short>(port));
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_udpSearchSocket, (sockaddr *)&localAddr, sizeof(localAddr)) == SOCKET_ERROR)
    {
        m_lastErrorMsg = "Failed to bind UDP search socket to port.";
        closesocket(m_udpSearchSocket);
        m_udpSearchSocket = INVALID_SOCKET;
        return false;
    }

    m_isSearching = true;
    m_discoveredServers.clear();

    m_searchThread = std::thread(&NetworkManager::SearchLoop, this, port);
    return true;
}

void NetworkManager::StopServerSearch()
{
    m_isSearching = false;
    if (m_udpSearchSocket != INVALID_SOCKET)
    {
        closesocket(m_udpSearchSocket);
        m_udpSearchSocket = INVALID_SOCKET;
    }
    if (m_searchThread.joinable())
        m_searchThread.join();
}

void NetworkManager::SearchLoop(int port)
{
    char buffer[256];
    sockaddr_in senderAddr;
    int senderAddrSize = sizeof(senderAddr);
    DWORD timeout = 1000;
    setsockopt(m_udpSearchSocket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));

    while (m_isSearching)
    {
        int bytes = recvfrom(m_udpSearchSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr *)&senderAddr, &senderAddrSize);
        if (bytes > 0)
        {
            buffer[bytes] = '\0';
            std::string message(buffer);
            if (message == "RDP_SERVER_DISCOVERY")
            {
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &senderAddr.sin_addr, ipStr, sizeof(ipStr));
                std::string serverIP(ipStr);
                bool found = false;
                for (const auto &ip : m_discoveredServers)
                {
                    if (ip == serverIP)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    m_discoveredServers.push_back(serverIP);
                }
            }
        }
    }
}

void NetworkManager::CleanupWinsock()
{
    if (m_isInitialized)
    {
        WSACleanup();
        m_isInitialized = false;
    }
}

void NetworkManager::SendFrame(const std::vector<uint8_t> &frameData)
{
    if (m_status != ConnectionStatus::Connected || frameData.empty() || m_activeSocket == INVALID_SOCKET)
        return;

    uint8_t packetType = 1;
    uint32_t payloadSize = static_cast<uint32_t>(frameData.size());

    if (!SendAll(m_activeSocket, reinterpret_cast<const char *>(&packetType), sizeof(packetType)))
        return;

    if (!SendAll(m_activeSocket, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize)))
        return;

    SendAll(m_activeSocket, reinterpret_cast<const char *>(frameData.data()), static_cast<int>(frameData.size()));
}

bool NetworkManager::GetLatestIncomingFrame(std::vector<uint8_t> &outFrame)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_latestIncomingFrame.empty())
    {
        return false;
    }

    outFrame = std::move(m_latestIncomingFrame);
    return true;
}

bool NetworkManager::SendAll(SOCKET sock, const char *data, int totalBytes)
{
    int bytesSent = 0;
    while (bytesSent < totalBytes)
    {
        int sent = send(sock, data + bytesSent, totalBytes - bytesSent, 0);
        if (sent <= 0)
        {
            return false;
        }
        bytesSent += sent;
    }
    return true;
}

bool NetworkManager::RecvAll(SOCKET sock, char *data, int totalBytes)
{
    int bytesReceived = 0;
    while (bytesReceived < totalBytes)
    {
        int received = recv(sock, data + bytesReceived, totalBytes - bytesReceived, 0);
        if (received <= 0)
            return false;
        bytesReceived += received;
    }
    return true;
}

void NetworkManager::SendScreenFrameToClient(uint64_t clientId, const std::vector<uint8_t> &frameData)
{
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        for (const auto &client : m_connectedClients)
        {
            if (client.id == clientId && client.status == ConnectionStatus::Connected)
            {
                sock = client.socket;
                break;
            }
        }
    }

    if (sock == INVALID_SOCKET || frameData.empty())
        return;

    uint8_t packetType = 1; // PACKET_SCREEN_FRAME
    uint32_t payloadSize = static_cast<uint32_t>(frameData.size());

    if (!SendAll(sock, reinterpret_cast<const char *>(&packetType), sizeof(packetType)))
        return;
    if (!SendAll(sock, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize)))
        return;

    if (!SendAll(sock, reinterpret_cast<const char *>(frameData.data()), payloadSize))
    {
        std::fprintf(stderr, "[WARNING] Network congestion: Dropped screen frame for client %llu\n", clientId);
        return;
    }
}

void NetworkManager::ClientReceiveLoop()
{
    while (m_status == ConnectionStatus::Connected && m_activeSocket != INVALID_SOCKET)
    {
        uint8_t packetType = 0;
        if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(&packetType), sizeof(packetType)))
        {
            std::fprintf(stderr, "[CLIENT ERROR] Failed to read packetType. WSA Error: %d\n", WSAGetLastError());
            break;
        }

        if (packetType == 3) // Permission Update
        {
            uint32_t packetSize = 0;
            if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(&packetSize), sizeof(packetSize)))
            {
                break;
            }

            if (packetSize > 0)
            {
                std::vector<uint8_t> permissionPayload(packetSize);
                if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(permissionPayload.data()), packetSize))
                {
                    break;
                }

                uint8_t permissionByte = permissionPayload[0];
                SetCanViewServerScreen(permissionByte == 1);
            }
            continue;
        }

        if (packetType == 4) // Handshake Accepted
        {
            uint32_t packetSize = 0;
            if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(&packetSize), sizeof(packetSize)))
            {
                break;
            }
            if (packetSize > 0)
            {
                std::vector<uint8_t> dummy(packetSize);
                RecvAll(m_activeSocket, reinterpret_cast<char *>(dummy.data()), packetSize);
            }
            m_status = ConnectionStatus::Connected;
            continue;
        }

        if (packetType == 5) // Handshake Rejected
        {
            uint32_t packetSize = 0;
            if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(&packetSize), sizeof(packetSize)))
            {
                break;
            }
            m_status = ConnectionStatus::Disconnected;
            break;
        }

        // Read packet size for data frames (Packet Type 1, 2, or others)
        uint32_t packetSize = 0;
        if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(&packetSize), sizeof(packetSize)))
        {
            std::fprintf(stderr, "[CLIENT ERROR] Failed to read packetSize for Type %d. WSA Error: %d\n", packetType, WSAGetLastError());
            break;
        }

        if (packetSize > 0 && packetSize < 50 * 1024 * 1024)
        {
            std::vector<uint8_t> payload(packetSize);
            if (!RecvAll(m_activeSocket, reinterpret_cast<char *>(payload.data()), packetSize))
            {
                std::fprintf(stderr, "[CLIENT ERROR] Failed to read payload of size %u. WSA Error: %d\n", packetSize, WSAGetLastError());
                break;
            }

            if (packetType == 1) // Screen Frame Packet
            {
                std::vector<uint8_t> decodedPixels;
                uint32_t width = 0, height = 0;

                if (ScreenCapturer::DecompressFrame(payload, decodedPixels, width, height))
                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_latestDecodedPixels = std::move(decodedPixels);
                    m_latestWidth = width;
                    m_latestHeight = height;
                }
                else
                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_latestDecodedPixels = std::move(payload);
                }
            }
            else if (packetType == 2) // Incoming Chat Packet from Server
            {
                if (payload.size() >= sizeof(uint32_t))
                {
                    uint32_t strLen = 0;
                    std::memcpy(&strLen, payload.data(), sizeof(uint32_t));

                    if (payload.size() >= sizeof(uint32_t) + strLen)
                    {
                        std::string serverMsg(reinterpret_cast<const char*>(payload.data() + sizeof(uint32_t)), strLen);
                        if (m_onClientChatReceived)
                        {
                            m_onClientChatReceived(serverMsg);
                        }
                    }
                }
            }
        }
        else if (packetSize >= 50 * 1024 * 1024)
        {
            std::fprintf(stderr, "[CLIENT ERROR] Packet size too large: %u bytes. Desync likely.\n", packetSize);
            break;
        }
    }

    if (m_status == ConnectionStatus::Connected)
    {
        m_status = ConnectionStatus::Disconnected;
    }
}

void NetworkManager::SetClientScreenPermission(uint64_t clientId, bool allowed)
{
    SOCKET targetSocket = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        for (auto &client : m_connectedClients)
        {
            if (client.id == clientId)
            {
                client.canViewServerScreen = allowed;
                targetSocket = client.socket;
                break;
            }
        }
    }

    if (targetSocket != INVALID_SOCKET)
    {
        uint8_t packetType = 3;
        uint32_t packetSize = 1;
        uint8_t permissionByte = allowed ? 1 : 0;

        send(targetSocket, reinterpret_cast<char *>(&packetType), sizeof(packetType), 0);
        send(targetSocket, reinterpret_cast<char *>(&packetSize), sizeof(packetSize), 0);
        send(targetSocket, reinterpret_cast<char *>(&permissionByte), sizeof(permissionByte), 0);
    }
}

std::optional<uint64_t> NetworkManager::GetScreenShareClientId() const
{
    for (const auto &client : m_connectedClients)
    {
        if (client.status == ConnectionStatus::Connected && client.canViewServerScreen)
        {
            return client.id;
        }
    }
    return std::nullopt;
}

void NetworkManager::BroadcastServerScreenFrame(const std::vector<uint8_t> &frameData)
{
    std::vector<uint64_t> targetClientIds;
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        for (const auto &client : m_connectedClients)
        {
            if (client.status == ConnectionStatus::Connected && client.canViewServerScreen)
            {
                targetClientIds.push_back(client.id);
            }
        }
    }

    for (uint64_t id : targetClientIds)
    {
        SendScreenFrameToClient(id, frameData);
    }
}

bool NetworkManager::ReceiveExact(SOCKET sock, char *dest, size_t totalBytes)
{
    size_t bytesReceived = 0;
    while (bytesReceived < totalBytes)
    {
        int result = recv(sock, dest + bytesReceived, static_cast<int>(totalBytes - bytesReceived), 0);
        if (result <= 0)
            return false;
        bytesReceived += result;
    }
    return true;
}

bool NetworkManager::ReadScreenPacket(SOCKET serverSocket, std::vector<uint8_t> &outFrameBuffer)
{
    uint32_t payloadSize = 0;
    if (!ReceiveExact(serverSocket, reinterpret_cast<char *>(&payloadSize), sizeof(payloadSize)))
        return false;

    if (payloadSize > 64 * 1024 * 1024)
        return false;

    outFrameBuffer.resize(payloadSize);
    if (!ReceiveExact(serverSocket, reinterpret_cast<char *>(outFrameBuffer.data()), payloadSize))
        return false;

    return true;
}

bool NetworkManager::GetLatestDecodedFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth, uint32_t &outHeight)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (m_latestDecodedPixels.empty())
        return false;

    outPixels = m_latestDecodedPixels;
    outWidth = m_latestWidth;
    outHeight = m_latestHeight;
    return true;
}

void NetworkManager::SendInputPacket(const std::vector<uint8_t> &payload)
{
    if (!IsClientConnected() || payload.empty())
    {
        return;
    }

    uint8_t packetType = 1; // PACKET_INPUT identifier
    uint32_t payloadSize = static_cast<uint32_t>(payload.size());

    // Send using your standard protocol structure: [Type (1 byte)][Size (4 bytes)][Payload]
    if (!SendAll(m_activeSocket, reinterpret_cast<const char *>(&packetType), sizeof(packetType)))
    {
        return;
    }
    if (!SendAll(m_activeSocket, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize)))
    {
        return;
    }
    SendAll(m_activeSocket, reinterpret_cast<const char *>(payload.data()), payloadSize);
}

void NetworkManager::SendServerChatToClient(uint64_t clientId, const std::vector<uint8_t> &payload)
{
    std::lock_guard<std::mutex> lock(m_clientMutex);
    for (const auto &client : m_connectedClients)
    {
        if (client.id == clientId && client.socket != INVALID_SOCKET)
        {
            uint8_t packetType = 2; // PACKET_CHAT
            uint32_t payloadSize = static_cast<uint32_t>(payload.size());

            send(client.socket, reinterpret_cast<const char *>(&packetType), sizeof(packetType), 0);
            send(client.socket, reinterpret_cast<const char *>(&payloadSize), sizeof(payloadSize), 0);
            send(client.socket, reinterpret_cast<const char *>(payload.data()), static_cast<int>(payloadSize), 0);
            break;
        }
    }
}


void NetworkManager::SendChatPacket(const std::vector<uint8_t> &payload)
{
    // If you are connected as a client, send the chat payload to the server
    if (IsClientConnected())
    {
        // Construct chat packet wrapper or send raw payload depending on your protocol
        // Example structure: [PacketType::PACKET_CHAT][Payload Size][Payload Data]
        uint32_t payloadSize = static_cast<uint32_t>(payload.size());
        std::vector<uint8_t> packet;
        packet.push_back(static_cast<uint8_t>(PacketType::PACKET_CHAT));
        
        // Append size (4 bytes)
        const char* sizePtr = reinterpret_cast<const char*>(&payloadSize);
        packet.insert(packet.end(), sizePtr, sizePtr + sizeof(payloadSize));
        
        // Append actual message payload
        packet.insert(packet.end(), payload.begin(), payload.end());

        SendAll(m_activeSocket, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()));
    }
}

void NetworkManager::SetOnChatReceivedCallback(ChatCallback cb)
{
    m_onChatReceived = cb;
}

void NetworkManager::SetOnClientChatReceivedCallback(std::function<void(const std::string &)> cb)
{
    m_onClientChatReceived = cb;
}

void NetworkManager::SetServerOnChatReceivedCallback(ServerChatCallback cb)
{
    m_serverChatCallback = cb;
}