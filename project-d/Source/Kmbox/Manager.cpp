#include <Pch.hpp>
#include "Manager.hpp"

// Align implementation closely with public kmboxNet reference
namespace {
    constexpr int kRxBufferBytes = 1024; // per packet payload size for KMBox showpic
}

KmBoxNetManager::~KmBoxNetManager()
{
    if (s_Client != INVALID_SOCKET)
    {
        closesocket(s_Client);
        s_Client = INVALID_SOCKET;
    }
    if (m_WsaStarted)
    {
        WSACleanup();
        m_WsaStarted = false;
    }
}

int KmBoxNetManager::InitDevice(const std::string& IP, WORD Port, const std::string& Mac)
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return err_creat_socket;
    m_WsaStarted = true;

    // UDP socket
    s_Client = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_Client == INVALID_SOCKET)
        return err_creat_socket;

    // Server address
    ZeroMemory(&AddrServer, sizeof(AddrServer));
    AddrServer.sin_family = AF_INET;
    AddrServer.sin_port = htons(Port);
    AddrServer.sin_addr.S_un.S_addr = inet_addr(IP.c_str());

    // Prepare connect packet
    ZeroMemory(&PostData, sizeof(PostData));
    PostData.head.mac = stoll(Mac, nullptr, 16);
    PostData.head.rand = rand();
    PostData.head.indexpts = 0;
    PostData.head.cmd = cmd_connect;

    // Send connect and wait for response
    int status = sendto(s_Client, reinterpret_cast<const char*>(&PostData), sizeof(cmd_head_t), 0,
        reinterpret_cast<sockaddr*>(&AddrServer), sizeof(AddrServer));
    if (status == SOCKET_ERROR)
        return err_creat_socket;

    int fromLen = sizeof(AddrServer);
    status = recvfrom(s_Client, reinterpret_cast<char*>(&ReceiveData), kRxBufferBytes, 0,
        reinterpret_cast<sockaddr*>(&AddrServer), &fromLen);
    if (status <= 0)
        return err_net_rx_timeout;

    return NetHandler();
}

int KmBoxNetManager::SendData(int dataLength)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;

    int status = sendto(s_Client, reinterpret_cast<const char*>(&PostData), dataLength, 0,
        reinterpret_cast<sockaddr*>(&AddrServer), sizeof(AddrServer));
    if (status == SOCKET_ERROR)
        return err_creat_socket;

    SOCKADDR_IN newClient{};
    int fromLen = sizeof(newClient);
    status = recvfrom(s_Client, reinterpret_cast<char*>(&ReceiveData), kRxBufferBytes, 0,
        reinterpret_cast<sockaddr*>(&newClient), &fromLen);
    if (status <= 0)
        return err_net_rx_timeout;

    return NetHandler();
}

int KmBoxNetManager::RebootDevice()
{
    if (s_Client == INVALID_SOCKET)
        return err_creat_socket;

    PostData.head.indexpts++;
    PostData.head.cmd = cmd_reboot;
    PostData.head.rand = rand();

    int length = sizeof(cmd_head_t);
    int rc = SendData(length);

    // After reboot request, clean up local socket
    closesocket(s_Client);
    s_Client = INVALID_SOCKET;
    if (m_WsaStarted)
    {
        WSACleanup();
        m_WsaStarted = false;
    }

    return rc < 0 ? err_net_rx_timeout : NetHandler();
}

int KmBoxNetManager::SetConfig(const std::string& IP, WORD Port)
{
    if (s_Client == INVALID_SOCKET)
        return err_creat_socket;

    PostData.head.indexpts++;
    PostData.head.cmd = cmd_setconfig;
    PostData.head.rand = inet_addr(IP.c_str());
    PostData.u8buff.buff[0] = static_cast<unsigned char>(Port >> 8);
    PostData.u8buff.buff[1] = static_cast<unsigned char>(Port & 0xFF);

    int length = sizeof(cmd_head_t) + 2;
    return SendData(length);
}

void KmBoxNetManager::SpeedTest(int count)
{
    auto startTime = std::chrono::steady_clock::now();
    for (int i = count; i > 0; i -= 2)
    {
        int ret = Kmbox.Mouse.Move(0, -100);
        if (ret != 0) LOG_ERROR("tx error {} ret1= {}", i, ret);
        ret = Kmbox.Mouse.Move(0, 100);
        if (ret != 0) LOG_ERROR("tx error {} ret2= {}", i, ret);
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    LOG_INFO("Speed test ({} calls) took {} ms", count, ms);
}

int KmBoxNetManager::NetHandler()
{
    if (ReceiveData.head.cmd != PostData.head.cmd)
        return err_net_cmd;
    if (ReceiveData.head.indexpts != PostData.head.indexpts)
        return err_net_pts;
    return 0;
}

// ================== Restored implementations needed by linker ==================
int KmBoxMouse::Move(int x, int y)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;
    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_move;
    Kmbox.PostData.head.rand = rand();
    this->MouseData.x = x;
    this->MouseData.y = y;
    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
    this->MouseData.x = 0; this->MouseData.y = 0;
    return Kmbox.SendData(length);
}

int KmBoxMouse::MoveRelative(int dx, int dy)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;
    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_move;
    Kmbox.PostData.head.rand = rand();
    this->MouseData.x = dx;
    this->MouseData.y = dy;
    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
    this->MouseData.x = 0; this->MouseData.y = 0;
    return Kmbox.SendData(length);
}

KmBoxKeyBoard::~KmBoxKeyBoard()
{
    EndMonitor();
}

int KmBoxKeyBoard::MonitorMouseRight()
{
    if (!ListenerRuned.load()) return -1;
    std::lock_guard<std::mutex> lg(m_MonitorMutex);
    return (hw_Mouse.buttons & 0x02) ? 1 : 0;
}

/*
Listener thread: reads UDP hardware monitor packets and updates snapshots.
*/
void KmBoxKeyBoard::ListenThread()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return;

    s_ListenSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_ListenSocket == INVALID_SOCKET)
        return;

    // Reuse address to avoid bind failures on quick restarts
    BOOL reuse = TRUE;
    setsockopt(s_ListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.S_un.S_addr = INADDR_ANY;
    addr.sin_port = htons(this->MonitorPort);

    if (::bind(s_ListenSocket, reinterpret_cast<SOCKADDR*>(&addr), sizeof(SOCKADDR)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        LOG_ERROR("KMBox monitor bind failed on UDP {}: WSA {}", (int)this->MonitorPort, err);
        closesocket(s_ListenSocket);
        s_ListenSocket = INVALID_SOCKET;
        return;
    }

    SOCKADDR addrClient{};
    int fromLen = sizeof(SOCKADDR);

    char buffer[kRxBufferBytes]{};

    ListenerRuned.store(true, std::memory_order_release);
    while (ListenerRuned.load(std::memory_order_acquire))
    {
        int ret = recvfrom(s_ListenSocket, buffer, sizeof(buffer), 0, &addrClient, &fromLen);
        if (ret > 0)
        {
            std::lock_guard<std::mutex> lg(m_MonitorMutex);
            memcpy(&hw_Mouse, buffer, sizeof(hw_Mouse));
            memcpy(&hw_Keyboard, &buffer[sizeof(hw_Mouse)], sizeof(hw_Keyboard));
        }
        else {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != 0) {
                LOG_WARN("KMBox monitor recvfrom failed: WSA {}", err);
            }
            break;
        }
    }
    ListenerRuned.store(false, std::memory_order_release);
    if (s_ListenSocket != INVALID_SOCKET)
    {
        closesocket(s_ListenSocket);
    }
    s_ListenSocket = 0;
}

int KmBoxKeyBoard::StartMonitor(WORD Port)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_monitor;

    this->MonitorPort = Port;
    Kmbox.PostData.head.rand = Port | 0xaa55 << 16;

    int length = sizeof(cmd_head_t);

    int sendRc = Kmbox.SendData(length);
    if (sendRc != 0) return sendRc;

    if (this->s_ListenSocket > 0)
    {
        closesocket(this->s_ListenSocket);
        this->s_ListenSocket = 0;
    }

    {
        std::lock_guard<std::mutex> lg(m_MonitorMutex);
        memset(&hw_Mouse, 0, sizeof(hw_Mouse));
        memset(&hw_Keyboard, 0, sizeof(hw_Keyboard));
    }

    if (t_Listen.joinable()) t_Listen.join();
    this->t_Listen = std::thread(&KmBoxKeyBoard::ListenThread, this);

    auto waitStart = std::chrono::steady_clock::now();
    while (!ListenerRuned.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(1)) {
            LOG_ERROR("KMBox monitor: listener failed to start within timeout on UDP {}", (int)Port);
            return err_creat_socket;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(8));

    return 0;
}

void KmBoxKeyBoard::EndMonitor()
{
    if (this->ListenerRuned.load(std::memory_order_acquire))
    {
        this->ListenerRuned.store(false, std::memory_order_release);

        if (this->s_ListenSocket)
            closesocket(this->s_ListenSocket);

        this->s_ListenSocket = 0;
        this->MonitorPort = 0;
        if (t_Listen.joinable())
            this->t_Listen.join();
    }
}