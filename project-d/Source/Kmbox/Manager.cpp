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
        if (ret != 0) LOG_ERROR("tx error {} ret1= {}", i, ret);
        ret = Kmbox.Mouse.Move(0, 100);
        if (ret != 0) LOG_ERROR("tx error {} ret2= {}", i, ret);
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

/*
Move the mouse by x,y units. Move once without trajectory simulation, fastest speed.
Use this function when writing the trajectory movement yourself.
Return value: 0 for normal execution, other values for exceptions.
*/
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

/*
Relative mouse move by dx,dy units.
*/
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

/*
Move the mouse by x,y units with device-side human-like simulation.
Runtime: desired duration in ms.
*/
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

/*
Mouse left button control. Down=true press, false release.
*/
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

/*
Mouse right button control. Down=true press, false release.
*/
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

/*
Mouse middle button control. Down=true press, false release.
*/
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

/* Mouse wheel control */
int KmBoxMouse::Wheel(int wheel)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_wheel;
    Kmbox.PostData.head.rand = rand();

    this->MouseData.wheel = wheel;
    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
    this->MouseData.wheel = 0;
    return Kmbox.SendData(length);
}

/* Mouse full report control */
int KmBoxMouse::All(int button, int x, int y, int wheel)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_wheel; // matches reference implementation
    Kmbox.PostData.head.rand = rand();

    this->MouseData.button = button;
    this->MouseData.x = x;
    this->MouseData.y = y;
    this->MouseData.wheel = wheel;

    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));

    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);

    this->MouseData.x = 0;
    this->MouseData.y = 0;
    this->MouseData.wheel = 0;

    return Kmbox.SendData(length);
}

/* Mouse side button 1 control */
int KmBoxMouse::Side1(bool Down)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;
    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_right; // as per reference
    Kmbox.PostData.head.rand = rand();
    this->MouseData.button = (Down ? (this->MouseData.button | 0x08) : (this->MouseData.button & (~0x08)));
    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
    return Kmbox.SendData(length);
}

/* Mouse side button 2 control */
int KmBoxMouse::Side2(bool Down)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;
    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_mouse_right; // as per reference
    Kmbox.PostData.head.rand = rand();
    this->MouseData.button = (Down ? (this->MouseData.button | 0x10) : (this->MouseData.button & (~0x10)));
    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
    return Kmbox.SendData(length);
}

/* Second-order Bezier curve control */
int KmBoxMouse::BezierMove(int x, int y, int ms, int x1, int y1, int x2, int y2)
{
    if (Kmbox.s_Client == INVALID_SOCKET)
        return err_creat_socket;
    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_bazerMove;
    Kmbox.PostData.head.rand = ms;
    this->MouseData.x = x;
    this->MouseData.y = y;
    this->MouseData.point[0] = x1;
    this->MouseData.point[1] = y1;
    this->MouseData.point[2] = x2;
    this->MouseData.point[3] = y2;
    memcpy_s(&Kmbox.PostData.cmd_mouse, sizeof(soft_mouse_t), &this->MouseData, sizeof(soft_mouse_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_mouse_t);
    this->MouseData.x = 0;
    this->MouseData.y = 0;
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

// Convenience monitor helpers mirroring kmNet_monitor_* API
int KmBoxKeyBoard::MonitorMouseLeft() { if (!ListenerRuned) return -1; return (hw_Mouse.buttons & 0x01) ? 1 : 0; }
int KmBoxKeyBoard::MonitorMouseMiddle() { if (!ListenerRuned) return -1; return (hw_Mouse.buttons & 0x04) ? 1 : 0; }
int KmBoxKeyBoard::MonitorMouseRight() { if (!ListenerRuned) return -1; return (hw_Mouse.buttons & 0x02) ? 1 : 0; }
int KmBoxKeyBoard::MonitorMouseSide1() { if (!ListenerRuned) return -1; return (hw_Mouse.buttons & 0x08) ? 1 : 0; }
int KmBoxKeyBoard::MonitorMouseSide2() { if (!ListenerRuned) return -1; return (hw_Mouse.buttons & 0x10) ? 1 : 0; }
int KmBoxKeyBoard::MonitorMouseXY(int& x, int& y)
{
    static int lastx = 0, lasty = 0;
    if (!ListenerRuned) return -1;
    x = hw_Mouse.x; y = hw_Mouse.y;
    if (x != lastx || y != lasty) { lastx = x; lasty = y; return 1; }
    return 0;
}
int KmBoxKeyBoard::MonitorMouseWheel(int& wheel)
{
    static int lastwheel = 0;
    if (!ListenerRuned) return -1;
    wheel = hw_Mouse.wheel;
    if (wheel != lastwheel) { lastwheel = wheel; return 1; }
    return 0;
}

// Keyboard send helpers
int KmBoxKeyBoard::KeyDown(int vk_key)
{
    if (Kmbox.s_Client == INVALID_SOCKET) return err_creat_socket;

    if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI)
    {
        switch (vk_key)
        {
        case KEY_LEFTCONTROL: SoftKeyboardData.ctrl |= BIT0; break;
        case KEY_LEFTSHIFT:   SoftKeyboardData.ctrl |= BIT1; break;
        case KEY_LEFTALT:     SoftKeyboardData.ctrl |= BIT2; break;
        case KEY_LEFT_GUI:    SoftKeyboardData.ctrl |= BIT3; break;
        case KEY_RIGHTCONTROL:SoftKeyboardData.ctrl |= BIT4; break;
        case KEY_RIGHTSHIFT:  SoftKeyboardData.ctrl |= BIT5; break;
        case KEY_RIGHTALT:    SoftKeyboardData.ctrl |= BIT6; break;
        case KEY_RIGHT_GUI:   SoftKeyboardData.ctrl |= BIT7; break;
        }
    }
    else
    {
        for (int i = 0; i < 10; ++i)
        {
            if (SoftKeyboardData.button[i] == vk_key)
                goto SEND_DOWN;
        }
        for (int i = 0; i < 10; ++i)
        {
            if (SoftKeyboardData.button[i] == 0)
            {
                SoftKeyboardData.button[i] = static_cast<unsigned char>(vk_key);
                goto SEND_DOWN;
            }
        }
        // queue full -> drop oldest
        memcpy(&SoftKeyboardData.button[0], &SoftKeyboardData.button[1], 9);
        SoftKeyboardData.button[9] = static_cast<unsigned char>(vk_key);
    }
SEND_DOWN:
    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_keyboard_all;
    Kmbox.PostData.head.rand = rand();

    memcpy_s(&Kmbox.PostData.cmd_keyboard, sizeof(soft_keyboard_t), &SoftKeyboardData, sizeof(soft_keyboard_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
    return Kmbox.SendData(length);
}

int KmBoxKeyBoard::KeyUp(int vk_key)
{
    if (Kmbox.s_Client == INVALID_SOCKET) return err_creat_socket;

    if (vk_key >= KEY_LEFTCONTROL && vk_key <= KEY_RIGHT_GUI)
    {
        switch (vk_key)
        {
        case KEY_LEFTCONTROL: SoftKeyboardData.ctrl &= ~BIT0; break;
        case KEY_LEFTSHIFT:   SoftKeyboardData.ctrl &= ~BIT1; break;
        case KEY_LEFTALT:     SoftKeyboardData.ctrl &= ~BIT2; break;
        case KEY_LEFT_GUI:    SoftKeyboardData.ctrl &= ~BIT3; break;
        case KEY_RIGHTCONTROL:SoftKeyboardData.ctrl &= ~BIT4; break;
        case KEY_RIGHTSHIFT:  SoftKeyboardData.ctrl &= ~BIT5; break;
        case KEY_RIGHTALT:    SoftKeyboardData.ctrl &= ~BIT6; break;
        case KEY_RIGHT_GUI:   SoftKeyboardData.ctrl &= ~BIT7; break;
        }
    }
    else
    {
        for (int i = 0; i < 10; ++i)
        {
            if (SoftKeyboardData.button[i] == vk_key)
            {
                memcpy(&SoftKeyboardData.button[i], &SoftKeyboardData.button[i + 1], 9 - i);
                SoftKeyboardData.button[9] = 0;
                break;
            }
        }
    }

    Kmbox.PostData.head.indexpts++;
    Kmbox.PostData.head.cmd = cmd_keyboard_all;
    Kmbox.PostData.head.rand = rand();

    memcpy_s(&Kmbox.PostData.cmd_keyboard, sizeof(soft_keyboard_t), &SoftKeyboardData, sizeof(soft_keyboard_t));
    int length = sizeof(cmd_head_t) + sizeof(soft_keyboard_t);
    return Kmbox.SendData(length);
}

int KmBoxKeyBoard::KeyPress(int vk_key, int ms)
{
    KeyDown(vk_key);
    Sleep(ms / 2);
    KeyUp(vk_key);
    Sleep(ms / 2);
    return 0;
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

/* Mask/unmask helpers mirror kmNet_mask_* API */
int KmBoxNetManager::MaskMouseLeft(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT0) : (m_maskKeyboardMouseFlag &= ~BIT0);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseRight(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT1) : (m_maskKeyboardMouseFlag &= ~BIT1);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseMiddle(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT2) : (m_maskKeyboardMouseFlag &= ~BIT2);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseSide1(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT3) : (m_maskKeyboardMouseFlag &= ~BIT3);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseSide2(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT4) : (m_maskKeyboardMouseFlag &= ~BIT4);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseX(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT5) : (m_maskKeyboardMouseFlag &= ~BIT5);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseY(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT6) : (m_maskKeyboardMouseFlag &= ~BIT6);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskMouseWheel(int enable)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    PostData.head.rand = enable ? (m_maskKeyboardMouseFlag |= BIT7) : (m_maskKeyboardMouseFlag &= ~BIT7);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::MaskKeyboard(short vkey)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_mask_mouse;
    unsigned char vk = vkey & 0xff;
    PostData.head.rand = (m_maskKeyboardMouseFlag & 0xff) | (vk << 8);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::UnmaskKeyboard(short vkey)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_unmask_all;
    unsigned char vk = vkey & 0xff;
    PostData.head.rand = (m_maskKeyboardMouseFlag & 0xff) | (vk << 8);
    return SendData(sizeof(cmd_head_t));
}
int KmBoxNetManager::UnmaskAll()
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_unmask_all;
    m_maskKeyboardMouseFlag = 0;
    PostData.head.rand = m_maskKeyboardMouseFlag;
    return SendData(sizeof(cmd_head_t));
}

int KmBoxNetManager::SetVidPid(unsigned short vid, unsigned short pid)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_setvidpid;
    PostData.head.rand = vid | (pid << 16);
    return SendData(sizeof(cmd_head_t));
}

/* Enable hardware trace/curve processing */
int KmBoxNetManager::Trace(int type, int value)
{
    if (s_Client == INVALID_SOCKET) return err_creat_socket;
    PostData.head.indexpts++; PostData.head.cmd = cmd_trace_enable;
    PostData.head.rand = (type << 24) | value;
    return SendData(sizeof(cmd_head_t));
}