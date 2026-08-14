#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional> 
#include "screenCapture.h"
#include "inputInjector.h"

#pragma comment(lib, "Ws2_32.lib")

enum class ConnectionStatus
{
    Disconnected,
    WaitingForPermission,
    Connected,
    Rejected
};

enum PacketType : uint8_t
{
    PACKET_SCREEN_FRAME = 1,
    PACKET_CHAT = 2,
    PACKET_SCREEN_SHARE_START = 3,
    PACKET_SCREEN_SHARE_STOP = 4
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
    ClientNode(const ClientNode &) = delete;
    ClientNode &operator=(const ClientNode &) = delete;

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
        {
            closesocket(socket);
        }
        if (handlerThread.joinable())
        {
            handlerThread.join();
        }
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
    NetworkManager();
    ~NetworkManager();

    bool InitializeWinsock();
    void CleanupWinsock();

    // Server methods
    bool StartServer(int port);
    void StopServer();
    void AcceptClient(uint64_t clientId, bool accept = true);
    void DisconnectClient(uint64_t clientId);
    void SetClientScreenSharePermission(uint64_t clientId, bool enabled);
    void SetClientInputControlPermission(uint64_t clientId, bool enabled);

    // Client methods
    void ConnectToServerAsync(const std::string &ip, int port);
    void Disconnect();

    // UDP Discovery methods
    bool StartDiscoveryBroadcast(int port);
    void StopDiscoveryBroadcast();
    bool StartServerSearch(int port);
    void StopServerSearch();

    // Frame Streaming Pipeline Methods & Helpers
    bool GetLatestIncomingFrame(std::vector<uint8_t> &outBuffer);
    void SendFrame(const std::vector<uint8_t> &frameData);
    void SendScreenFrameToClient(uint64_t clientId, const std::vector<uint8_t> &frameData);
    void SendScreenFrameToServer(const std::vector<uint8_t> &frameData);
    void BroadcastServerScreenFrame(const std::vector<uint8_t> &frameData);

    // Getters & Setters & State Checks
    bool IsRunning() const { return m_isRunning; }
    bool IsServerRunning() const { return m_isRunning && m_listenSocket != INVALID_SOCKET; }
    bool IsClientConnected() const { return m_status == ConnectionStatus::Connected && m_activeSocket != INVALID_SOCKET; }

    ConnectionStatus GetStatus() const { return m_status; }
    std::string GetConnectedClientIP() const;
    std::vector<std::string> GetDiscoveredServers() const { return m_discoveredServers; }
    std::string GetLastErrorMsg() const { return m_lastErrorMsg; }

    // Multi-client server accessors
    std::vector<ClientNode> &GetConnectedClients() { return m_connectedClients; }
    std::mutex &GetClientMutex() { return m_clientMutex; }

    std::vector<ClientUIInfo> GetClientUIInfos()
    {
        std::lock_guard<std::mutex> lock(m_clientMutex);
        std::vector<ClientUIInfo> list;
        list.reserve(m_connectedClients.size());
        for (const auto &client : m_connectedClients)
        {
            list.push_back({client.id, client.ip, client.status, client.canViewServerScreen, client.inputControlEnabled});
        }
        return list;
    }

    void SendInputPacket(const std::vector<uint8_t> &payload);
    void SendChatPacket(const std::vector<uint8_t> &payload);

    void SetClientScreenPermission(uint64_t clientId, bool allowed);
    std::optional<uint64_t> GetScreenShareClientId() const;
    bool CanViewServerScreen() const { return m_canViewServerScreen; }
    void SetCanViewServerScreen(bool allowed) { m_canViewServerScreen = allowed; }
    bool GetLatestDecodedFrame(std::vector<uint8_t> &outPixels, uint32_t &outWidth, uint32_t &outHeight);
    bool ReceiveExact(SOCKET sock, char *dest, size_t totalBytes);
    bool ReadScreenPacket(SOCKET serverSocket, std::vector<uint8_t> &outFrameBuffer);

    using ChatCallback = std::function<void(uint64_t, const std::string &)>;
    using ServerChatCallback = std::function<void(uint64_t senderId, const std::string& message)>;

    void SetServerOnChatReceivedCallback(ServerChatCallback cb);
    void SendServerChatToClient(uint64_t clientId, const std::vector<uint8_t>& payload);
    void SetOnChatReceivedCallback(ChatCallback cb);
    void SetOnClientChatReceivedCallback(std::function<void(const std::string &)> cb);

private:
    SOCKET m_listenSocket;
    SOCKET m_activeSocket;
    SOCKET m_udpBroadcastSocket;
    SOCKET m_udpSearchSocket;

    std::atomic<bool> m_isInitialized;
    std::atomic<bool> m_isRunning;
    std::atomic<bool> m_isBroadcasting;
    std::atomic<bool> m_isSearching;
    std::atomic<ConnectionStatus> m_status;

    std::vector<ClientNode> m_connectedClients;
    std::mutex m_clientMutex;
    uint64_t m_nextClientId = 1;

    std::vector<std::string> m_discoveredServers;
    std::string m_lastErrorMsg;

    // Incoming Frame Pipeline storage (Client receiving server screen)
    std::vector<uint8_t> m_latestIncomingFrame;
    std::mutex m_frameMutex;

    std::thread m_serverListenThread;
    std::thread m_clientConnectThread;
    std::thread m_broadcastThread;
    std::thread m_searchThread;
    std::thread m_clientReceiveThread;

    std::atomic<bool> m_canViewServerScreen{false};

    std::vector<uint8_t> m_latestDecodedPixels;
    uint32_t m_latestWidth = 0;
    uint32_t m_latestHeight = 0;

    uint64_t m_ActiveInputClientId = 0;
    InputInjector m_InputInjector;

    ChatCallback m_onChatReceived;
    std::function<void(const std::string &)> m_onClientChatReceived;
    ServerChatCallback m_serverChatCallback;

    void ListenLoop();
    void ConnectAsyncWorker(std::string ip, int port);
    void ClientHandlerLoop(SOCKET sock);
    void BroadcastLoop(int port);
    void SearchLoop(int port);
    void ClientReceiveLoop();

    // TCP Protocol Stream Helpers
    bool SendAll(SOCKET sock, const char *data, int totalBytes);
    bool RecvAll(SOCKET sock, char *data, int totalBytes);
};

#endif // NETWORK_MANAGER_H