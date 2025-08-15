#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "Config.hpp"
#include "Excluded.hpp"
#include <atomic>
#include <mutex>
#include <string>

class KmBoxMouse
{
public:
    soft_mouse_t MouseData{};
public:
    // Move
    int Move(int x, int y);
    // Relative move
    int MoveRelative(int dx, int dy);
    // Auto move (simulate human-like movement)
    int Move_Auto(int x, int y, int Runtime);
    // Full mouse report (button | x | y | wheel)
    int All(int button, int x, int y, int wheel);
    // Wheel
    int Wheel(int wheel);
    // Left button
    int Left(bool Down);
    // Right button
    int Right(bool Down);
    // Middle button
    int Middle(bool Down);
    // Side button 1
    int Side1(bool Down);
    // Side button 2
    int Side2(bool Down);
    // Bezier curve move
    int BezierMove(int x, int y, int ms, int x1, int y1, int x2, int y2);

    // Convenience wrappers
    int ClickLeft() { int r = Left(true); Sleep(1); return r == 0 ? Left(false) : r; }
    int ClickRight() { int r = Right(true); Sleep(1); return r == 0 ? Right(false) : r; }
    int MoveTo(int x, int y) { return Move(x, y); }
    int MoveBy(int dx, int dy) { return MoveRelative(dx, dy); }
};

class KmBoxKeyBoard
{
public:
    std::thread t_Listen;
    WORD MonitorPort;
    SOCKET s_ListenSocket = 0;
    std::atomic<bool> ListenerRuned{false};
private:
    std::mutex m_MonitorMutex; // Protect hw_Mouse/hw_Keyboard snapshots
public:
    standard_keyboard_report_t hw_Keyboard;
    standard_mouse_report_t hw_Mouse;
    soft_keyboard_t SoftKeyboardData{}; // software keyboard state to send
public:
    ~KmBoxKeyBoard();
    void ListenThread();
    // Start/stop hardware monitor on given UDP port (0 disables)
    int StartMonitor(WORD Port);
    void EndMonitor();
public:
    // Query hardware key state while monitoring
    bool GetKeyState(WORD vKey);
    int MonitorMouseLeft();
    int MonitorMouseMiddle();
    int MonitorMouseRight();
    int MonitorMouseSide1();
    int MonitorMouseSide2();
    int MonitorMouseXY(int& x, int& y);
    int MonitorMouseWheel(int& wheel);
    // Thread-safe snapshot helpers
    int GetMouseButtons(unsigned char& buttons) { if (!ListenerRuned.load()) return -1; std::lock_guard<std::mutex> lg(m_MonitorMutex); buttons = hw_Mouse.buttons; return 0; }

    // Send keyboard events to KMBox
    int KeyDown(int vk_key);
    int KeyUp(int vk_key);
    int KeyPress(int vk_key, int ms);
};

class KmBoxNetManager
{
private:
    SOCKADDR_IN AddrServer{};
    SOCKET s_Client = INVALID_SOCKET;
    client_data ReceiveData{};
    client_data PostData{};
    bool m_WsaStarted = false;
    int m_maskKeyboardMouseFlag = 0; // mirror mask state
private:
    int NetHandler();
    int SendData(int DataLength);
public:
    ~KmBoxNetManager();
    // Initialize device
    int InitDevice(const std::string& IP, WORD Port, const std::string& Mac);
    // Reboot device
    int RebootDevice();
    // Set device config
    int SetConfig(const std::string& IP, WORD Port);
	// Speed test
	void SpeedTest(int count = 1000);

    // Fill LCD with color
    int FillLCDColor(unsigned short rgb565);
    // Change picture
    int ChangePicture(const unsigned char* buff_128_160);
    // Change bottom picture
    int ChangePictureBottom(const unsigned char* buff_128_80);

    // Mask/unmask functions
    int MaskMouseLeft(int enable);
    int MaskMouseRight(int enable);
    int MaskMouseMiddle(int enable);
    int MaskMouseSide1(int enable);
    int MaskMouseSide2(int enable);
    int MaskMouseX(int enable);
    int MaskMouseY(int enable);
    int MaskMouseWheel(int enable);
    int MaskKeyboard(short vkey);
    int UnmaskKeyboard(short vkey);
    int UnmaskAll();

    // Set VID/PID exposed by device
    int SetVidPid(unsigned short vid, unsigned short pid);
    // Enable hardware trace/curve processing
    int Trace(int type, int value);
public:
    friend class KmBoxMouse;
    KmBoxMouse Mouse;
    friend class KmBoxKeyBoard;
    KmBoxKeyBoard KeyBoard;
};

inline KmBoxNetManager Kmbox;
