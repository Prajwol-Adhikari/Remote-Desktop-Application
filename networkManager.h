#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// --- Project Headers ---
#include "inputInjector.h"
#include "screenCapture.h"


enum class ConnectionStatus
{
    Disconnected,
    WaitingForPermission,
    Connected,
    Rejected
};

enum PacketType : uint8_t
{
    PACKET_SCREEN_FRAME       = 1,
    PACKET_CHAT               = 2,
    PACKET_SCREEN_SHARE_START = 3,
    PACKET_SCREEN_SHARE_STOP  = 4
};

struct ClientNode
{
    uint64_t id = 0;
    SOCKET socket = INVALID_SOCKET;
    std::string ip;
    ConnectionStatus status = ConnectionStatus::Disconnected;
    std::thread handlerThread;
    bool canViewServerScreen = false;
    bool inputControlEnabled = false;

    ClientNode() = default;
    
    // Non-copyable
    ClientNode(const ClientNode &) = delete;
    ClientNode &operator=(const ClientNode &) = delete;

    // Move Operations
    ClientNode(ClientNode &&other) noexcept
        : id(other.id),
          socket(other.socket),
          ip(std::move(other.ip)),
          status(other.status),
          handlerThread(std::move(other.handlerThread)),
          canViewServerScreen(other.canViewServerScreen),
          inputControlEnabled(other.inputControlEnabled)
    {
        other.socket = INVALID_SOCKET;
    }

    ClientNode &operator=(ClientNode &&other) noexcept
    {
        if (this != &other)
        {
            if (socket != INVALID_SOCKET)
                closesocket(socket);
            if (handlerThread.joinable())
                handlerThread.join();

            id = other.id;
            socket = other.socket;
            ip = std::move(other.ip);
            status = other.status;
            handlerThread = std::move(other.handlerThread);
            canViewServerScreen = other.canViewServerScreen;
            inputControlEnabled = other.inputControlEnabled;

            other.socket = INVALID_SOCKET;
        }
        return *this;
    }

    ~ClientNode()
    {
        if (socket != INVALID_SOCKET)
            closesocket(socket);
        if (handlerThread.joinable())
            handlerThread.join();
    }
};

struct ClientUIInfo
{
    uint64_t id;
    std::string ip;
    ConnectionStatus status;
    bool canViewServerScreen;
    bool inputControlEnabled;
};



class NetworkManager
{
public:
    // --- Type Aliases ---
    using ChatCallback               = std::function<void(uint64_t senderId, const std::string &message)>;
    using ClientChatCallback         = std::function<void(const std::string &message)>;
    using ServerChatCallback         = std::function<void(uint64_t senderId, const std::string &message)>;

    // --- Constructor / Destructor ---
    NetworkManager();
    ~NetworkManager();

    // --- Winsock Lifecycle ---
    bool InitializeWinsock();
    void CleanupWinsock();

    // --- Server Operations ---
    bool StartServer(int port);
    void StopServer();
    void AcceptClient(uint64_t clientId, bool accept = true);
    void DisconnectClient(uint64_t clientId);
    void SetClientScreenSharePermission(uint64_t clientId, bool enabled);
    void SetClientInputControlPermission(uint64_t clientId, bool enabled);
    void SetClientScreenPermission(uint64_t clientId, bool allowed);

    // --- Client Operations ---
    void ConnectToServerAsync(const std::string &ip, int port);
    void Disconnect();

    // --- UDP Discovery Service ---
    bool StartDiscoveryBroadcast(int port);
    void StopDiscoveryBroadcast();
    bool StartServerSearch(int port);
    void StopServerSearch();

    // --- Frame & Video Pipeline ---
    bool GetLatestIncomingFrame(std::vector<uint8_t> &outBuffer);
    bool GetLatestDecodedFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth, uint32_t &outHeight);
    void SendFrame(const std::vector<uint8_t> &frameData);
    void SendScreenFrameToClient(uint64_t clientId, const std::vector<uint8_t> &frameData);
    void SendScreenFrameToServer(const std::vector<uint8_t> &frameData);
    void BroadcastServerScreenFrame(const std::vector<uint8_t> &frameData);
    bool ReadScreenPacket(SOCKET serverSocket, std::vector<uint8_t> &outFrameBuffer);

    // --- Messaging & Input Control ---
    void SendInputPacket(const std::vector<uint8_t> &payload);
    void SendChatPacket(const std::vector<uint8_t> &payload);
    void SendServerChatToClient(uint64_t clientId, const std::vector<uint8_t> &payload);

    // Callbacks
    void SetOnChatReceivedCallback(ChatCallback cb);
    void SetOnClientChatReceivedCallback(ClientChatCallback cb);
    void SetServerOnChatReceivedCallback(ServerChatCallback cb);

    // --- Status & Getters ---
    bool IsRunning() const          { return m_isRunning; }
    bool IsServerRunning() const    { return m_isRunning && m_listenSocket != INVALID_SOCKET; }
    bool IsClientConnected() const  { return m_status == ConnectionStatus::Connected && m_activeSocket != INVALID_SOCKET; }
    
    ConnectionStatus GetStatus() const { return m_status; }
    std::string GetConnectedClientIP() const;
    std::vector<std::string> GetDiscoveredServers() const { return m_discoveredServers; }
    std::string GetLastErrorMsg() const { return m_lastErrorMsg; }

    bool CanViewServerScreen() const { return m_canViewServerScreen; }
    void SetCanViewServerScreen(bool allowed) { m_canViewServerScreen = allowed; }
    std::optional<uint64_t> GetScreenShareClientId() const;

    // --- Thread-Safe Client Accessors ---
    std::vector<ClientNode> &GetConnectedClients() { return m_connectedClients; }
    std::mutex &GetClientMutex()                   { return m_clientMutex; }
    std::vector<ClientUIInfo> GetClientUIInfos();

    // --- Socket I/O Helpers ---
    bool ReceiveExact(SOCKET sock, char *dest, size_t totalBytes);

private:
    // --- Socket Handles ---
    SOCKET m_listenSocket       = INVALID_SOCKET;
    SOCKET m_activeSocket       = INVALID_SOCKET;
    SOCKET m_udpBroadcastSocket = INVALID_SOCKET;
    SOCKET m_udpSearchSocket    = INVALID_SOCKET;

    // --- Atomic State Flags ---
    std::atomic<bool> m_isInitialized{false};
    std::atomic<bool> m_isRunning{false};
    std::atomic<bool> m_isBroadcasting{false};
    std::atomic<bool> m_isSearching{false};
    std::atomic<ConnectionStatus> m_status{ConnectionStatus::Disconnected};
    std::atomic<bool> m_canViewServerScreen{false};

    // --- Client Container & Synchronization ---
    std::vector<ClientNode> m_connectedClients;
    std::mutex m_clientMutex;
    uint64_t m_nextClientId = 1;

    // --- Network Discovery & Diagnostics ---
    std::vector<std::string> m_discoveredServers;
    std::string m_lastErrorMsg;

    // --- Frame Pipeline Buffers ---
    std::vector<uint8_t> m_latestIncomingFrame;
    std::vector<uint8_t> m_latestDecodedPixels;
    uint32_t m_latestWidth  = 0;
    uint32_t m_latestHeight = 0;
    std::mutex m_frameMutex;

    // --- Input & Control Injection ---
    uint64_t m_ActiveInputClientId = 0;
    InputInjector m_InputInjector;

    // --- Callbacks ---
    ChatCallback m_onChatReceived;
    ClientChatCallback m_onClientChatReceived;
    ServerChatCallback m_serverChatCallback;

    // --- Thread Handles ---
    std::thread m_serverListenThread;
    std::thread m_clientConnectThread;
    std::thread m_broadcastThread;
    std::thread m_searchThread;
    std::thread m_clientReceiveThread;

    // --- Internal Worker Loops ---
    void ListenLoop();
    void ConnectAsyncWorker(std::string ip, int port);
    void ClientHandlerLoop(SOCKET sock);
    void BroadcastLoop(int port);
    void SearchLoop(int port);
    void ClientReceiveLoop();

    // --- Low-Level I/O Utilities ---
    bool SendAll(SOCKET sock, const char *data, int totalBytes);
    bool RecvAll(SOCKET sock, char *data, int totalBytes);
};

#endif // NETWORK_MANAGER_H