#include <Pch.hpp>
#include "Manager.hpp"

namespace {
    constexpr int kDefaultTimeoutMs = 1000;      // 1s timeout
    constexpr int kRxBufferBytes   = 1024;       // per packet payload size
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

static bool SetSocketTimeouts(SOCKET s, int ms)
{
    if (s == INVALID_SOCKET) return false;
    DWORD to = ms;
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to)) == 0 &&
           setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&to, sizeof(to)) == 0;
}

int KmBoxNetManager::InitDevice(const string& IP, WORD Port, const string& Mac)
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return err_creat_socket;
    m_WsaStarted = true;

    // UDP socket
    s_Client = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_Client == INVALID_SOCKET)
        return err_creat_socket;

    // Optional: allow fast reuse
    DWORD yes = 1;
    setsockopt(s_Client, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    // Apply timeouts
    SetSocketTimeouts(s_Client, kDefaultTimeoutMs);

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

    // Try a couple of times in case device is waking
    int attempts = 2;
    int status = SOCKET_ERROR;
    while (attempts-- > 0)
    {
        status = sendto(s_Client, reinterpret_cast<const char*>(&PostData), sizeof(cmd_head_t), 0,
            reinterpret_cast<sockaddr*>(&AddrServer), sizeof(AddrServer));
        if (status == SOCKET_ERROR)
            continue;

        this_thread::sleep_for(chrono::milliseconds(20));
        int fromLen = sizeof(AddrServer);
        status = recvfrom(s_Client, reinterpret_cast<char*>(&ReceiveData), kRxBufferBytes, 0,
            reinterpret_cast<sockaddr*>(&AddrServer), &fromLen);
        if (status > 0)
            break;
        this_thread::sleep_for(chrono::milliseconds(30));
    }
    if (status <= 0)
        return err_net_rx_timeout;

    LOG_INFO("Successfully connected to KMBOX: IP={} Port={} UUID={}", IP, Port, Mac);
    return NetHandler();
}

int KmBoxNetManager::SendData(int dataLength)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;

    int attempts = 2;
    int status = SOCKET_ERROR;
    while (attempts-- > 0)
    {
        status = sendto(s_Client, reinterpret_cast<const char*>(&PostData), dataLength, 0,
            reinterpret_cast<sockaddr*>(&AddrServer), sizeof(AddrServer));
        if (status == SOCKET_ERROR)
            continue;

        SOCKADDR_IN newClient{};
        int fromLen = sizeof(newClient);
        status = recvfrom(s_Client, reinterpret_cast<char*>(&ReceiveData), kRxBufferBytes, 0,
            reinterpret_cast<sockaddr*>(&newClient), &fromLen);
        if (status > 0)
            break;
        this_thread::sleep_for(chrono::milliseconds(5));
    }

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

    // After reboot request, clean up local socket; device will drop connection
    closesocket(s_Client);
    s_Client = INVALID_SOCKET;
    if (m_WsaStarted)
    {
        WSACleanup();
        m_WsaStarted = false;
    }

    return rc < 0 ? err_net_rx_timeout : NetHandler();
}

int KmBoxNetManager::SetConfig(const string& IP, WORD Port)
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
    auto startTime = chrono::steady_clock::now();
    for (int i = count; i > 0; i -= 2)
    {
        int ret = Kmbox.Mouse.Move(0, -100);
        if (ret != 0) LOG_ERROR("tx error {} ret1={}", i, ret);
        ret = Kmbox.Mouse.Move(0, 100);
        if (ret != 0) LOG_ERROR("tx error {} ret2={}", i, ret);
    }
    auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - startTime).count();
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

    this->MouseData.x = 0;
    this->MouseData.y = 0;

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

    this->MouseData.x = 0;
    this->MouseData.y = 0;

    return Kmbox.SendData(length);
}

int KmBoxMouse::Move_Auto(int x, int y, int Runtime)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_automove;
    Kmbox.PostData.head.rand = Runtime;

    this->MouseData.x = x;
    this->MouseData.y = y;

    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));

    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);

    this->MouseData.x = 0;
    this->MouseData.y = 0;

    return Kmbox.SendData(length);
}

int KmBoxMouse::Left(bool Down)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_left;
    Kmbox.PostData.head.rand = rand();

    this->MouseData.button = (Down ? (this->MouseData.button | 0x01) : (this->MouseData.button & (~0x01)));

    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));

    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);

    return Kmbox.SendData(length);
}

int KmBoxMouse::Right(bool Down)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_right;
    Kmbox.PostData.head.rand = rand();

    this->MouseData.button = (Down ? (this->MouseData.button | 0x02) : (this->MouseData.button & (~0x02)));

    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));

    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);

    return Kmbox.SendData(length);
}

int KmBoxMouse::Middle(bool Down)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_middle;
    Kmbox.PostData.head.rand = rand();

    this->MouseData.button = (Down ? (this->MouseData.button | 0x04) : (this->MouseData.button & (~0x04)));

    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));

    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);

    return Kmbox.SendData(length);
}

void KmBoxKeyBoard::ListenThread()
{
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return;

    s_ListenSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_ListenSocket == INVALID_SOCKET)
        return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.S_un.S_addr = INADDR_ANY;
    addr.sin_port = htons(this->MonitorPort);

    bind(s_ListenSocket, reinterpret_cast<SOCKADDR*>(&addr), sizeof(SOCKADDR));

    SOCKADDR addrClient{};
    int fromLen = sizeof(SOCKADDR);

    char buffer[kRxBufferBytes]{};

    ListenerRuned = true;
    while (ListenerRuned)
    {
        int ret = recvfrom(s_ListenSocket, buffer, sizeof(buffer), 0, &addrClient, &fromLen);
        if (ret > 0)
        {
            memcpy(&hw_Mouse, buffer, sizeof(hw_Mouse));
            memcpy(&hw_Keyboard, &buffer[sizeof(hw_Mouse)], sizeof(hw_Keyboard));
        }
        else break;
    }
    ListenerRuned = false;
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

    Kmbox.SendData(length);

    if (this->s_ListenSocket > 0)
    {
        closesocket(this->s_ListenSocket);
        this->s_ListenSocket = 0;
    }

    this->t_Listen = thread(&KmBoxKeyBoard::ListenThread, this);

    this_thread::sleep_for(chrono::milliseconds(8));

    return Kmbox.NetHandler();
}

void KmBoxKeyBoard::EndMonitor()
{
    if (this->ListenerRuned)
    {
        this->ListenerRuned = false;

        if (this->s_ListenSocket)
            closesocket(this->s_ListenSocket);

        this->s_ListenSocket = 0;
        this->MonitorPort = 0;
        if (t_Listen.joinable())
            this->t_Listen.join();
    }
}

KmBoxKeyBoard::~KmBoxKeyBoard()
{
    this->EndMonitor();
}

bool KmBoxKeyBoard::GetKeyState(WORD vKey)
{
    unsigned char keyValue = vKey & 0xff;
    if (!this->ListenerRuned)
        return false;

    if (keyValue >= KEY_LEFTCONTROL && keyValue <= KEY_RIGHT_GUI)
    {
        switch (keyValue)
        {
        case KEY_LEFTCONTROL: return  this->hw_Keyboard.buttons & BIT0 ? 1 : 0;
        case KEY_LEFTSHIFT:   return  this->hw_Keyboard.buttons & BIT1 ? 1 : 0;
        case KEY_LEFTALT:     return  this->hw_Keyboard.buttons & BIT2 ? 1 : 0;
        case KEY_LEFT_GUI:    return  this->hw_Keyboard.buttons & BIT3 ? 1 : 0;
        case KEY_RIGHTCONTROL:return  this->hw_Keyboard.buttons & BIT4 ? 1 : 0;
        case KEY_RIGHTSHIFT:  return  this->hw_Keyboard.buttons & BIT5 ? 1 : 0;
        case KEY_RIGHTALT:    return  this->hw_Keyboard.buttons & BIT6 ? 1 : 0;
        case KEY_RIGHT_GUI:   return  this->hw_Keyboard.buttons & BIT7 ? 1 : 0;
        default:              return false;
        }
    }
    else
    {
        for (auto i : this->hw_Keyboard.data)
        {
            if (i == keyValue)
                return true;
        }
    }
    return false;
}

// Fill LCD with color
int KmBoxNetManager::FillLCDColor(unsigned short rgb565) {
    if (this->s_Client == INVALID_SOCKET)
        return err_creat_socket;

    for (int y = 0; y < 40; y++) {
        this->PostData.head.indexpts++;
        this->PostData.head.cmd = cmd_showpic;
        this->PostData.head.rand = y * 4;

        // Fill u16 buffer correctly
        for (int i = 0; i < 512 / sizeof(unsigned short); ++i) {
            this->PostData.u16buff.buff[i] = rgb565;
        }

        int length = sizeof(cmd_head_t) + kRxBufferBytes;
        sendto(this->s_Client, reinterpret_cast<const char*>(&this->PostData), length, 0,
            reinterpret_cast<sockaddr*>(&this->AddrServer), sizeof(this->AddrServer));

        SOCKADDR_IN sclient{};
        int clen = sizeof(sclient);
        int err = recvfrom(this->s_Client, reinterpret_cast<char*>(&this->ReceiveData), length, 0,
            reinterpret_cast<sockaddr*>(&sclient), &clen);

        if (err <= 0) {
            return err_net_rx_timeout;
        }
    }

    return NetHandler();
}

// Change picture
int KmBoxNetManager::ChangePicture(const unsigned char* buff_128_160) {
    if (this->s_Client == INVALID_SOCKET)
        return err_creat_socket;

    for (int y = 0; y < 40; y++) {
        this->PostData.head.indexpts++;
        this->PostData.head.cmd = cmd_showpic;
        this->PostData.head.rand = y * 4;

        memcpy(this->PostData.u8buff.buff, &buff_128_160[y * kRxBufferBytes], kRxBufferBytes);

        int length = sizeof(cmd_head_t) + kRxBufferBytes;
        sendto(this->s_Client, reinterpret_cast<const char*>(&this->PostData), length, 0,
            reinterpret_cast<sockaddr*>(&this->AddrServer), sizeof(this->AddrServer));

        SOCKADDR_IN sclient{};
        int clen = sizeof(sclient);
        int err = recvfrom(this->s_Client, reinterpret_cast<char*>(&this->ReceiveData), length, 0,
            reinterpret_cast<sockaddr*>(&sclient), &clen);

        if (err <= 0) {
            return err_net_rx_timeout;
        }
    }

    return NetHandler();
}

// Change bottom picture
int KmBoxNetManager::ChangePictureBottom(const unsigned char* buff_128_80) {
    if (this->s_Client == INVALID_SOCKET)
        return err_creat_socket;

    for (int y = 0; y < 20; y++) {
        this->PostData.head.indexpts++;
        this->PostData.head.cmd = cmd_showpic;
        this->PostData.head.rand = 80 + y * 4;

        memcpy(this->PostData.u8buff.buff, &buff_128_80[y * kRxBufferBytes], kRxBufferBytes);

        int length = sizeof(cmd_head_t) + kRxBufferBytes;
        sendto(this->s_Client, reinterpret_cast<const char*>(&this->PostData), length, 0,
            reinterpret_cast<sockaddr*>(&this->AddrServer), sizeof(this->AddrServer));

        SOCKADDR_IN sclient{};
        int clen = sizeof(sclient);
        int err = recvfrom(this->s_Client, reinterpret_cast<char*>(&this->ReceiveData), length, 0,
            reinterpret_cast<sockaddr*>(&sclient), &clen);

        if (err <= 0) {
            return err_net_rx_timeout;
        }
    }

    return NetHandler();
}