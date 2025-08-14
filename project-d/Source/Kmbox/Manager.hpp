#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "Config.hpp"
#include "Excluded.hpp"

class KmBoxMouse
{
public:
    soft_mouse_t MouseData{};
public:
    // Move
    int Move(int x, int y);
    // Relative move
    int MoveRelative(int dx, int dy);
    // Auto move
    int Move_Auto(int x, int y, int Runtime);
    // Left button
    int Left(bool Down);
    // Right button
    int Right(bool Down);
    // Middle button
    int Middle(bool Down);

    // Convenience wrappers
    int ClickLeft() { int r = Left(true); Sleep(1); return r == 0 ? Left(false) : r; }
    int ClickRight() { int r = Right(true); Sleep(1); return r == 0 ? Right(false) : r; }
    int MoveTo(int x, int y) { return Move(x, y); }
    int MoveBy(int dx, int dy) { return MoveRelative(dx, dy); }
};

class KmBoxKeyBoard
{
public:
    thread t_Listen;
    WORD MonitorPort;
    SOCKET s_ListenSocket = 0;
    bool ListenerRuned = false;
public:
    standard_keyboard_report_t hw_Keyboard;
    standard_mouse_report_t hw_Mouse;
public:
    ~KmBoxKeyBoard();
    void ListenThread();
    int StartMonitor(WORD Port);
    void EndMonitor();
public:
    bool GetKeyState(WORD vKey);
};

class KmBoxNetManager
{
private:
    SOCKADDR_IN AddrServer{};
    SOCKET s_Client = INVALID_SOCKET;
    client_data ReceiveData{};
    client_data PostData{};
    bool m_WsaStarted = false;
private:
    int NetHandler();
    int SendData(int DataLength);
public:
    ~KmBoxNetManager();
    // Initialize device
    int InitDevice(const string& IP, WORD Port, const string& Mac);
    // Reboot device
    int RebootDevice();
    // Set device config
    int SetConfig(const string& IP, WORD Port);
	// Speed test
	void SpeedTest(int count = 1000);

    // Fill LCD with color
    int FillLCDColor(unsigned short rgb565);
    // Change picture
    int ChangePicture(const unsigned char* buff_128_160);
    // Change bottom picture
    int ChangePictureBottom(const unsigned char* buff_128_80);
public:
    friend class KmBoxMouse;
    KmBoxMouse Mouse;
    friend class KmBoxKeyBoard;
    KmBoxKeyBoard KeyBoard;
};

inline KmBoxNetManager Kmbox;
