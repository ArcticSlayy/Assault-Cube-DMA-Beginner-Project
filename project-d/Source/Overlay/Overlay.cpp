#include <Pch.hpp>
#include <SDK.hpp>
#include <chrono>
#include <spdlog/spdlog.h>
#include <dxgi1_5.h>
#pragma comment(lib, "dxgi.lib")

#include "Overlay.hpp"
#include "Fonts/IBMPlexMono_Medium.h"
#include "Fonts/font_awesome.h"
#include "Fonts/font_awesome.cpp"
#include "Features.hpp"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
/*
Overlay.cpp
- Hosts all ImGui setup, styling, and the main menu rendering.
- Contains a light-weight UI framework (PropertyRow, ToggleSwitch, etc.) used by all tabs.
- Goal: beautiful yet performant UI with minimal per-frame allocations.
*/
/*
If you want to change the icons or see what all icons there are available, you can use the font viewver I have that works with the font_awesome.h/cpp.
To use it, just uncomment the line below. Then open the normal menu which in my current case is Insert, then once the menu is open, press F10 to open the icon viewer.
*/
// #define SHOW_ICON_FONT_VIEWER
Microsoft::WRL::ComPtr<ID3D11Device> Overlay::device = nullptr;
Microsoft::WRL::ComPtr<ID3D11DeviceContext> Overlay::device_context = nullptr;
Microsoft::WRL::ComPtr<IDXGISwapChain> Overlay::swap_chain = nullptr;
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> Overlay::render_targetview = nullptr;

HWND Overlay::overlay = nullptr;
WNDCLASSEX Overlay::wc = { };
ImFont* iconFont = nullptr; // Global/static for Overlay.cpp
ImFont* titleFont = nullptr; // Title font (bold, 19pt)
ImFont* tabFont = nullptr;   // Tab font (16.5pt)
ImFont* featureFont = nullptr; // Feature font (15pt)
ImFont* sectionFont = nullptr; // Slightly larger font for section headers
static ImVec4 s_LastAppliedAccent = ImVec4(-1, -1, -1, -1);

static bool g_AllowTearing = false; // runtime capability

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static const ImVec4 blueAccent = ImVec4(0.22f, 0.40f, 0.80f, 1.00f);
ImVec4 gAccent = ImVec4(0.22f, 0.40f, 0.80f, 1.00f); // mutable accent (global)

/*
Small drawing helpers to add subtle polish with negligible cost.
*/
static void DrawShadowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col, int layers = 3, float spread = 4.0f, float alphaDecay = 0.35f)
{
    for (int i = 0; i < layers; ++i)
    {
        float t = 1.0f + (float)i * 0.5f;
        ImU32 c = ImGui::GetColorU32(ImVec4(
            ((col >> 0) & 0xFF) / 255.0f,
            ((col >> 8) & 0xFF) / 255.0f,
            ((col >> 16) & 0xFF) / 255.0f,
            ImClamp(((col >> 24) & 0xFF) / 255.0f * powf(1.0f - alphaDecay, (float)i), 0.0f, 1.0f)
        ));
        dl->AddRect(a - ImVec2(spread * t, spread * t), b + ImVec2(spread * t, spread * t), c, rounding + t, 0, 1.0f);
    }
}

// Enhanced slider fill track helper (draw on current ItemRect)
static void DrawSliderProgressOnLastItem(float t, ImU32 fillCol)
{
    ImRect bb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    float clamped = ImClamp(t, 0.0f, 1.0f);
    float w = bb.Max.x - bb.Min.x;
    ImVec2 a = bb.Min;
    ImVec2 b = ImVec2(bb.Min.x + w * clamped, bb.Max.y);
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, fillCol, ImGui::GetStyle().FrameRounding);
}

static void HelpMarker(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Help marker centered vertically to the last item's height, to be called right after the item
static void HelpMarkerCentered(const char* desc)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Property row: left label column aligned, value control aligned to a fixed X, help marker on far right (no overlap)
template<typename Fn>
static void PropertyRow(const char* label, Fn drawer, const char* help = nullptr)
{
    ImGuiStyle& st = ImGui::GetStyle();
    const float labelWidth = 260.0f; // left column width for better readability
    const float gap = st.ItemInnerSpacing.x + 8.0f; // spacing between label and control

    float localMinX = ImGui::GetWindowContentRegionMin().x;
    float localMaxX = ImGui::GetWindowContentRegionMax().x;
    float valueX = localMinX + labelWidth + gap;
    float availW = ImMax(0.0f, localMaxX - valueX - 28.0f); // reserve space for help marker and padding

    // Label (left)
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);

    // Control (aligned to same X)
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(valueX);
    ImGui::SetNextItemWidth(availW);
    drawer();

    // Insert help marker just before the control boundary to avoid overlap, vertically centered to control
    if (help)
    {
        ImVec2 ctlMin = ImGui::GetItemRectMin();
        ImVec2 ctlMax = ImGui::GetItemRectMax();
        float markerH = ImGui::GetTextLineHeight();
        float y = ctlMin.y + (ctlMax.y - ctlMin.y - markerH) * 0.5f;
        float markerW = ImGui::CalcTextSize("(?)").x;
        float x = ctlMin.x - markerW - 6.0f;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
            ImGui::TextUnformatted(help);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // Row spacing
    ImGui::Dummy(ImVec2(0.0f, st.ItemSpacing.y * 0.5f));
}

// Helpers for render target lifecycle
void Overlay::CreateRenderTarget()
{
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
    if (swap_chain && SUCCEEDED(swap_chain->GetBuffer(0U, IID_PPV_ARGS(back_buffer.ReleaseAndGetAddressOf()))) && back_buffer)
    {
        device->CreateRenderTargetView(back_buffer.Get(), nullptr, render_targetview.ReleaseAndGetAddressOf());
    }
}

void Overlay::CleanupRenderTarget()
{
    if (render_targetview) { render_targetview.Reset(); }
}

LRESULT CALLBACK window_procedure(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(window, msg, wParam, lParam))
		return true;

	switch (msg)
	{
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && Overlay::device && Overlay::swap_chain)
        {
            Overlay::CleanupRenderTarget();
            UINT w = LOWORD(lParam);
            UINT h = HIWORD(lParam);
            Overlay::swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
            Overlay::CreateRenderTarget();
        }
        return 0;

	case WM_DESTROY:
		Overlay::DestroyOverlay();
		Overlay::DestroyImGui();
		PostQuitMessage(0);
		return 0;

	case WM_CLOSE:
		Overlay::DestroyDevice();
		Overlay::DestroyOverlay();
		Overlay::DestroyImGui();
		return 0;
	}

	return DefWindowProc(window, msg, wParam, lParam);
}

bool Overlay::CreateDevice()
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));

    sd.BufferCount = 2;

    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;

    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;

    // Query tearing support at runtime
    {
        IDXGIFactory5* factory5 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void**)&factory5)) && factory5)
        {
            BOOL allow = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                g_AllowTearing = allow == TRUE;
            factory5->Release();
        }
    }

    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (g_AllowTearing) sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; // enable tearing if supported

    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    sd.OutputWindow = overlay;

    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;

    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // flip model

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };

    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0U,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        swap_chain.ReleaseAndGetAddressOf(),
        device.ReleaseAndGetAddressOf(),
        &featureLevel,
        device_context.ReleaseAndGetAddressOf());

    if (result == DXGI_ERROR_UNSUPPORTED) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0U,
            featureLevelArray,
            2, D3D11_SDK_VERSION,
            &sd,
            swap_chain.ReleaseAndGetAddressOf(),
            device.ReleaseAndGetAddressOf(),
            &featureLevel,
            device_context.ReleaseAndGetAddressOf());

        // downgraded silently
    }

    if (result != S_OK) {
        return false;
    }

    CreateRenderTarget();
    return true;
}

void Overlay::DestroyDevice()
{
    CleanupRenderTarget();

    if (device_context) device_context.Reset();
    if (swap_chain) swap_chain.Reset();
    if (device) device.Reset();
}

bool Overlay::CreateOverlay()
{
	wc.cbSize = sizeof(wc);
	wc.style = CS_CLASSDC;
	wc.lpfnWndProc = window_procedure;
	wc.hInstance = GetModuleHandleA(0);
	wc.lpszClassName = L"Awhare";

	RegisterClassEx(&wc);

	overlay = CreateWindowEx(
		WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
		wc.lpszClassName,
		L"Awhare",
		WS_POPUP,
		0,
		0,
		GetSystemMetrics(SM_CXSCREEN),
		GetSystemMetrics(SM_CYSCREEN),
		NULL,
		NULL,
		wc.hInstance,
		NULL
	);

	if (overlay == NULL)
	{
		return false;
	}

	SetLayeredWindowAttributes(overlay, RGB(0, 0, 0), BYTE(255), LWA_ALPHA);

	{
		RECT client_area{};
		RECT window_area{};

		GetClientRect(overlay, &client_area);
		GetWindowRect(overlay, &window_area);

		POINT diff{};
		ClientToScreen(overlay, &diff);

		const MARGINS margins{
			window_area.left + (diff.x - window_area.left),
			window_area.top + (diff.y - window_area.top),
			client_area.right,
			client_area.bottom
		};

		DwmExtendFrameIntoClientArea(overlay, &margins);
	}

	ShowWindow(overlay, SW_SHOW);
	UpdateWindow(overlay);

	// Topmost is set once; avoid reasserting every frame
	SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // Ensure transparency is set initially when menu is closed
    SetWindowLong(overlay, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED);

	return true;
}

void Overlay::DestroyOverlay()
{
	DestroyWindow(overlay);
	UnregisterClass(wc.lpszClassName, wc.hInstance);
}

bool Overlay::CreateImGui()
{
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	if (!ImGui_ImplWin32_Init(overlay)) {
		return false;
	}

	if (!ImGui_ImplDX11_Init(device.Get(), device_context.Get())) {
		return false;
	}

	// Font loading (only ONCE, after context is created)
	static bool fontAtlasBuilt = false;
	if (!fontAtlasBuilt) {
		ImGuiIO& IO = ImGui::GetIO();
        // System fonts: light, crisp look
        titleFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\TahomaBD.ttf", 35.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        tabFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Tahoma.ttf", 20.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        featureFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Tahoma.ttf", 18.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        sectionFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Tahoma.ttf", 22.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        ImFont* MainFont = tabFont;
        IO.FontDefault = MainFont;
        // Icon overlay font (merged)
        static const ImWchar icon_ranges[] = { 0xf000, 0xf8ff, 0 };
        ImFontConfig icons_config;
        icons_config.MergeMode = true;
        icons_config.PixelSnapH = true;
        icons_config.OversampleH = 3;
        icons_config.OversampleV = 3;
        iconFont = IO.Fonts->AddFontFromMemoryCompressedTTF(font_awesome_data, font_awesome_size, 28.0f, &icons_config, icon_ranges);
        IO.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        IO.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        fontAtlasBuilt = true;
    }
	return true;
}

void Overlay::DestroyImGui()
{
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

void Overlay::SetForeground(HWND window)
{
	if (!IsWindowInForeground(window))
		BringToForeground(window);
}

void Overlay::StartRender()
{
    static std::chrono::steady_clock::time_point frameStart;
    frameStart = std::chrono::steady_clock::now();
    
    // Process Windows messages efficiently with a small timeout
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Check for menu toggle key
    if (GetAsyncKeyState(VK_INSERT) & 1)
    {
        shouldRenderMenu = !shouldRenderMenu;

        if (shouldRenderMenu)
        {
            SetWindowLong(overlay, GWL_EXSTYLE, WS_EX_TOOLWINDOW);
        }
        else
        {
            SetWindowLong(overlay, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED);
        }
    }
}

void Overlay::EndRender()
{
    ImGui::Render();

    float color[4];
    if (config.Visuals.Background) // Black bg
    {
        color[0] = 0; color[1] = 0; color[2] = 0; color[3] = 1;
    }
    else // Transparent bg
    {
        color[0] = 0; color[1] = 0; color[2] = 0; color[3] = 0;
    }

    device_context->OMSetRenderTargets(1, render_targetview.GetAddressOf(), nullptr);
    device_context->ClearRenderTargetView(render_targetview.Get(), color);

    // Always draw FOV circle if enabled (before rendering draw data)
    if (config.Aim.DrawFov)
    {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImVec2 center(Screen.x / 2.0f, Screen.y / 2.0f);
        float radius = config.Aim.AimbotFov;
        ImVec4 c4 = config.Aim.AimbotFovColor;
        ImU32 base = ImGui::GetColorU32(c4);
        // Soft glow: multiple passes with fading alpha
        const int layers = 6;
        for (int i = layers - 1; i >= 0; --i)
        {
            float t = (float)i / (float)layers;
            float r = radius + t * 8.0f; // expand a bit
            float a = c4.w * (0.10f + 0.12f * (1.0f - t));
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(c4.x, c4.y, c4.z, a));
            drawList->AddCircle(center, r, col, 0, 1.5f);
        }
        // Main crisp ring
        drawList->AddCircle(center, radius, base, 0, 2.0f);
        // Center dot
        drawList->AddCircleFilled(center, 2.0f, ImGui::GetColorU32(ImVec4(1,1,1,0.9f)));
    }
    
    // Add watermark if enabled (cheap text draws)
    if (config.Visuals.Watermark)
    {
        float fps = ImGui::GetIO().Framerate;
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        ImVec4 watermarkColor = config.Visuals.WatermarkColor;
        ImU32 textColor = ImGui::GetColorU32(watermarkColor);
        
        // Calculate string dimensions
        char watermarkText[128];
        snprintf(watermarkText, sizeof(watermarkText), "%.1f FPS | Made by ", fps);
        
        ImVec2 textSize = ImGui::CalcTextSize(watermarkText);
        ImVec2 arcticSize = ImGui::CalcTextSize("Arctic");
        float fullWidth = textSize.x + arcticSize.x;
        
        // Calculate position based on setting
        float padding = 10.0f;
        float posX = 0.0f;
        float posY = 0.0f;
        
        switch (config.Visuals.WatermarkPos)
        {
            case Structs::WatermarkPosition::TopLeft:
                posX = padding;
                posY = padding;
                break;
                
            case Structs::WatermarkPosition::TopMiddle:
                posX = (Screen.x - fullWidth) / 2.0f;
                posY = padding;
                break;
                
            case Structs::WatermarkPosition::BottomLeft:
                posX = padding;
                posY = Screen.y - textSize.y - padding;
                break;
                
            case Structs::WatermarkPosition::BottomRight:
                posX = Screen.x - fullWidth - padding;
                posY = Screen.y - textSize.y - padding;
                break;
                
            case Structs::WatermarkPosition::TopRight:
            default:
                posX = Screen.x - fullWidth - padding;
                posY = padding;
                break;
        }
        
        // Glow shadow for better readability
        ImU32 shadowCol = ImGui::GetColorU32(ImVec4(0,0,0,0.60f));
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(ImVec2(posX + 1, posY + 1), shadowCol, watermarkText);
        dl->AddText(ImVec2(posX, posY), textColor, watermarkText);
        dl->AddText(ImVec2(posX + textSize.x + 1, posY + 1), shadowCol, "Arctic");
        dl->AddText(ImVec2(posX + textSize.x, posY), textColor, "Arctic");
    }

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    UINT presentFlags = 0;
    if (!config.Visuals.VSync && g_AllowTearing)
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;

    swap_chain->Present(config.Visuals.VSync ? 1 : 0, presentFlags);
}

void Overlay::StyleMenu(ImGuiIO& IO, ImGuiStyle& style)
{
    // Modern dark theme with blue accent
    style.WindowRounding    = 14;
    style.ChildRounding     = 14;
    style.FrameRounding     = 8;
    style.GrabRounding      = 8;
    style.PopupRounding     = 8;
    style.TabRounding       = 8;
    style.ScrollbarRounding = 8;
    style.WindowBorderSize  = 0;
    style.FrameBorderSize   = 0;
    style.PopupBorderSize   = 0;
    style.ScrollbarSize     = 12.f;
    style.GrabMinSize       = 12.f;
    style.WindowPadding     = ImVec2(24, 24);
    style.FramePadding      = ImVec2(12, 8);
    style.ItemSpacing       = ImVec2(12, 8);
    style.ItemInnerSpacing  = ImVec2(8, 4);
    style.WindowTitleAlign  = ImVec2(0.0f, 0.5f);
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
    // Colors
    ImVec4 darkBg = ImVec4(0.102f, 0.102f, 0.102f, 1.00f); // #1A1A1A
    ImVec4 buttonBg = ImVec4(0.16f, 0.16f, 0.16f, 1.00f); // Slightly lighter for contrast
    style.Colors[ImGuiCol_WindowBg]           = darkBg;
    style.Colors[ImGuiCol_ChildBg]            = darkBg;
    style.Colors[ImGuiCol_FrameBg]            = darkBg;
    style.Colors[ImGuiCol_Button]             = buttonBg;
    style.Colors[ImGuiCol_Header]             = darkBg;
    style.Colors[ImGuiCol_Tab]                = darkBg;
    style.Colors[ImGuiCol_TabUnfocused]       = darkBg;
    style.Colors[ImGuiCol_PopupBg]            = darkBg;
    style.Colors[ImGuiCol_ScrollbarBg]        = darkBg;
    style.Colors[ImGuiCol_ScrollbarGrab]      = darkBg;
    // Don't color all framed widgets to accent (hard to tell state for checkboxes)
    style.Colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive]      = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    style.Colors[ImGuiCol_TitleBg]            = darkBg;
    style.Colors[ImGuiCol_TitleBgActive]      = darkBg;
    style.Colors[ImGuiCol_TitleBgCollapsed]   = darkBg;
    style.Colors[ImGuiCol_Border]             = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
    style.Colors[ImGuiCol_ButtonHovered]      = gAccent;
    style.Colors[ImGuiCol_ButtonActive]       = gAccent;
    style.Colors[ImGuiCol_HeaderHovered]      = gAccent;
    style.Colors[ImGuiCol_HeaderActive]      = gAccent;
    style.Colors[ImGuiCol_SliderGrab]         = gAccent;
    style.Colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.32f, 0.50f, 0.90f, 1.00f);
    style.Colors[ImGuiCol_CheckMark]          = gAccent;
    style.Colors[ImGuiCol_Text]               = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]       = ImVec4(0.60f, 0.62f, 0.65f, 1.00f);
    style.Colors[ImGuiCol_Separator]          = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
    style.Colors[ImGuiCol_TabHovered]         = gAccent;
    style.Colors[ImGuiCol_TabActive]          = gAccent;
    style.Colors[ImGuiCol_TabUnfocusedActive] = gAccent;
    style.Colors[ImGuiCol_DragDropTarget]     = gAccent;
    style.Colors[ImGuiCol_NavHighlight]       = gAccent;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = gAccent;
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = gAccent;

    // Enforce minimum brightness (RGB >= 25/255)
    const float minC = 25.0f / 255.0f;
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        ImVec4 c = style.Colors[i];
        c.x = (i == ImGuiCol_Text || i == ImGuiCol_TextDisabled) ? c.x : ImMax(c.x, minC);
        c.y = (i == ImGuiCol_Text || i == ImGuiCol_TextDisabled) ? c.y : ImMax(c.y, minC);
        c.z = (i == ImGuiCol_Text || i == ImGuiCol_TextDisabled) ? c.z : ImMax(c.z, minC);
        style.Colors[i] = c;
    }
}

bool ToggleSwitch(const char* label, bool* v, float scale = 0.55f)
{
    // Smaller toggle, always show label, horizontal layout
    ImGuiStyle& style = ImGui::GetStyle();
    float height = ImGui::GetFrameHeight() * scale;
    float width = height * 1.6f;
    float spacing = style.ItemInnerSpacing.x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton(label, ImVec2(width + ImGui::CalcTextSize(label).x + spacing, height));
    if (ImGui::IsItemClicked())
        *v = !*v;
    float t = *v ? 1.0f : 0.0f;
    ImU32 col_bg = ImGui::GetColorU32(*v ? ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.0f) : ImVec4(0.18f, 0.19f, 0.22f, 1.0f));
    // Draw label
    draw_list->AddText(p, ImGui::GetColorU32(ImGuiCol_Text), label);
    // Draw toggle
    ImVec2 togglePos = p + ImVec2(ImGui::CalcTextSize(label).x + spacing, 0);
    draw_list->AddRectFilled(togglePos, togglePos + ImVec2(width, height), col_bg, height * 0.5f);
    draw_list->AddCircleFilled(togglePos + ImVec2(height * 0.5f + t * (width - height), height * 0.5f), height * 0.4f, ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.98f, 1.0f)));
    return *v;
}

// Toggle switch without drawing a label (for use inside property tables)
static bool ToggleSwitchNoLabel(const char* id, bool* v, float scale = 0.55f)
{
    ImGuiStyle& style = ImGui::GetStyle();
    float height = ImGui::GetFrameHeight() * scale;
    float width = height * 1.6f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    if (ImGui::IsItemClicked())
        *v = !*v;
    float t = *v ? 1.0f : 0.0f;
    ImU32 col_bg = ImGui::GetColorU32(*v ? ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.0f) : ImVec4(0.18f, 0.19f, 0.22f, 1.0f));
    draw_list->AddRectFilled(p, p + ImVec2(width, height), col_bg, height * 0.5f);
    draw_list->AddCircleFilled(p + ImVec2(height * 0.5f + t * (width - height), height * 0.5f), height * 0.4f, ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.98f, 1.0f)));
    return *v;
}

void Overlay::RenderMenu()
{
    static std::string configNotification;
    static float notificationTimer = 0.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    // Apply style only when accent changes
    if (s_LastAppliedAccent.x != gAccent.x || s_LastAppliedAccent.y != gAccent.y || s_LastAppliedAccent.z != gAccent.z || s_LastAppliedAccent.w != gAccent.w)
    {
        StyleMenu(io, style);
        s_LastAppliedAccent = gAccent;
    }

    float OverlayFps = ImGui::GetIO().Framerate;

    // Toast system (drawn later)
    enum class ToastType { Info, Success, Error };
    struct Toast { std::string text; std::chrono::steady_clock::time_point expiry; ToastType type; };
    static std::vector<Toast> toasts;
    auto pushToast = [&](const std::string& msg, ToastType type = ToastType::Info, float seconds = 2.0f) {
        if (!config.Visuals.ToastsEnabled) return; // obey user toggle
        toasts.push_back({ msg, std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000)), type });
    };

    // FPS history for sparkline
    static std::vector<float> fpsHistory(120, 0.0f);
    static int fpsIndex = 0;
    fpsHistory[fpsIndex] = OverlayFps;
    fpsIndex = ( fpsIndex + 1 ) % ( int )fpsHistory.size( );

#ifdef SHOW_ICON_FONT_VIEWER
    // --- Icon Font Debug Viewer ---
    static bool showIconFontViewer = false;
    if (ImGui::IsKeyPressed(ImGuiKey_F10))
        showIconFontViewer = !showIconFontViewer;
    if (showIconFontViewer && iconFont)
    {
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
        ImGui::Begin("Font Awesome Glyphs", &showIconFontViewer, ImGuiWindowFlags_NoCollapse);
        ImGui::Text("Available Font Awesome glyphs in loaded font:");
        ImGui::Separator();
        ImGui::PushFont(iconFont);
        int columns = 16;
        ImGui::Columns(columns, nullptr, false);
        for (int i = 0; i < iconFont->Glyphs.Size; ++i)
        {
            ImFontGlyph& glyph = iconFont->Glyphs[i];
            char buf[8] = { 0 };
            ImTextCharToUtf8(buf, glyph.Codepoint);
            ImGui::Text("%s", buf);
            ImGui::TextDisabled("U+%04X", glyph.Codepoint);
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
        ImGui::PopFont();
        ImGui::End();
    }
#endif

    ImGui::SetNextWindowSize(ImVec2(1220, 750), ImGuiCond_Always);
    ImGui::Begin("Aetherial", &shouldRenderMenu, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    {
        ImDrawList* sdl = ImGui::GetForegroundDrawList();
        ImU32 shadowCol1 = ImGui::GetColorU32(ImVec4(0,0,0,0.12f));
        ImU32 shadowCol2 = ImGui::GetColorU32(ImVec4(0,0,0,0.08f));
        ImU32 shadowCol3 = ImGui::GetColorU32(ImVec4(0,0,0,0.05f));
        ImU32 shadowCol4 = ImGui::GetColorU32(ImVec4(0,0,0,0.03f));
        float r = 18.0f;
        sdl->AddRect(winPos + ImVec2(-6, -4), winPos + winSize + ImVec2(6, 10), shadowCol1, r);
        sdl->AddRect(winPos + ImVec2(-8, -6), winPos + winSize + ImVec2(8, 14), shadowCol2, r);
        sdl->AddRect(winPos + ImVec2(-10, -8), winPos + winSize + ImVec2(10, 18), shadowCol3, r);
        sdl->AddRect(winPos + ImVec2(-12, -10), winPos + winSize + ImVec2(12, 22), shadowCol4, r);
    }
    float textSize = titleFont ? titleFont->FontSize : 35.0f;
    float iconSize = iconFont ? iconFont->FontSize : 28.0f;
    float paddingY = 5.0f; // 5px above and below text
    float titleBarHeight = ImMax(textSize, iconSize) + 2 * paddingY;
    float rounding = 16.0f;
    const float kMin = 25.0f / 255.0f;
    ImVec4 baseBg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    auto mix = [](const ImVec4& a, const ImVec4& b, float t){ return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f); };
    ImVec4 leftCol = mix(baseBg, ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.0f), 0.10f);
    ImVec4 rightCol = mix(baseBg, ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.0f), 0.05f);
    leftCol.x *= 0.80f; leftCol.y *= 0.80f; leftCol.z *= 0.80f;
    rightCol.x *= 0.80f; rightCol.y *= 0.80f; rightCol.z *= 0.80f;
    leftCol.x = ImMax(leftCol.x, kMin); leftCol.y = ImMax(leftCol.y, kMin); leftCol.z = ImMax(leftCol.z, kMin);
    rightCol.x = ImMax(rightCol.x, kMin); rightCol.y = ImMax(rightCol.y, kMin); rightCol.z = ImMax(rightCol.z, kMin);
    ImU32 titleLeft = ImGui::ColorConvertFloat4ToU32(leftCol);
    ImU32 titleRight = ImGui::ColorConvertFloat4ToU32(rightCol);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(winPos, winPos + winSize, false);
    dl->AddRectFilledMultiColor(winPos, winPos + ImVec2(winSize.x, titleBarHeight), titleLeft, titleRight, titleRight, titleLeft);
    dl->AddRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight), titleLeft, rounding, ImDrawFlags_RoundCornersTop, 2.0f);
    dl->AddRectFilled(winPos + ImVec2(0, titleBarHeight - 2), winPos + ImVec2(winSize.x, titleBarHeight + 8), ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,0.22f)));

    {
        float underlineY = winPos.y + titleBarHeight - 3.0f;
        float underlineH = 2.0f;
        dl->AddRectFilled(ImVec2(winPos.x, underlineY), ImVec2(winPos.x + winSize.x, underlineY + underlineH), ImGui::GetColorU32(ImVec4(0.20f, 0.22f, 0.26f, 1.0f)));
        float t = (float)ImGui::GetTime();
        float segW = 160.0f;
        float speedBase = 140.0f;
        float wave = 1.0f + 0.25f * sinf(t * 1.6f);
        float speed = speedBase * wave;
        float x = fmodf(t * speed, winSize.x + segW) - segW;
        ImVec2 a = ImVec2(winPos.x + x, underlineY);
        ImVec2 b = ImVec2(winPos.x + x + segW, underlineY + underlineH);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.85f)));
    }

    // Filler under title bar to avoid blank/transparent band
    {
        ImVec4 fill = ImVec4(ImMax(baseBg.x, kMin), ImMax(baseBg.y, kMin), ImMax(baseBg.z, kMin), 1.0f);
        ImU32 fillCol = ImGui::ColorConvertFloat4ToU32(fill);
        float gapH = 6.0f; // small gap height
        dl->AddRectFilled(ImVec2(winPos.x + 10, winPos.y + titleBarHeight), ImVec2(winPos.x + winSize.x - 10, winPos.y + titleBarHeight + gapH), fillCol, 0.0f);
    }

    dl->PopClipRect();

    ImRect titleBarRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight));
    if (ImGui::IsMouseHoveringRect(titleBarRect.Min, titleBarRect.Max) && !ImGui::IsAnyItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImGui::SetWindowPos(ImGui::GetWindowPos() + io.MouseDelta);
    }

    dl->PushClipRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight), false);

    const char* titleText = "Aetherial";
    ImVec2 textDim = ImGui::CalcTextSize(titleText);
    float totalWidth = iconSize + 18.0f + textDim.x;
    float centerX = (winSize.x - totalWidth) * 0.5f;
    float centerY = paddingY + (titleBarHeight - 2 * paddingY - ImMax(iconSize, textSize)) / 2.0f;
    float iconYOffset = 4.2f;
    float iconY = centerY + (ImMax(iconSize, textSize) - iconSize) / 2.0f + iconYOffset;
    float textY = centerY + (ImMax(iconSize, textSize) - textSize) / 2.0f;
    float startX = centerX;
    ImGui::SetCursorPos(ImVec2(startX, iconY));
    ImGui::PushFont(iconFont);
    ImGui::TextColored(gAccent, ICON_FA_MOON);
    ImGui::PopFont();
    ImGui::SameLine(0, 18.0f);
    ImGui::SetCursorPos(ImVec2(startX + iconSize + 18.0f, textY));
    ImGui::PushFont(titleFont);
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.00f), "%s", titleText);
    ImGui::PopFont();

    float btnSize = 26.0f;
    float btnPadding = 8.0f;
    ImVec2 btnPosClose = winPos + ImVec2(winSize.x - btnPadding - btnSize, (titleBarHeight - btnSize) * 0.5f);
    ImVec2 btnPosSettings = btnPosClose - ImVec2(btnSize + 6.0f, 0);

    auto drawTitleButton = [&](const ImVec2& pos, const char* icon, ImU32 fg) -> bool {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton(icon, ImVec2(btnSize, btnSize));
        bool hovered = ImGui::IsItemHovered();
        ImVec4 hoverCol = ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.22f);
        ImU32 bgCol = hovered ? ImGui::GetColorU32(hoverCol) : ImGui::GetColorU32(ImVec4(0, 0, 0, 0));
        dl->AddRectFilled(pos, pos + ImVec2(btnSize, btnSize), bgCol, 6.0f);
        ImVec2 iconPos = pos + ImVec2((btnSize - (iconFont ? iconFont->FontSize : 18.0f)) * 0.5f, (btnSize - (iconFont ? iconFont->FontSize : 18.0f)) * 0.5f);
        ImGui::PushFont(iconFont);
        dl->AddText(iconFont, iconFont ? iconFont->FontSize : 18.0f, iconPos, fg, icon);
        ImGui::PopFont();
        return ImGui::IsItemClicked();
    };

    bool settingsClicked = drawTitleButton(btnPosSettings, ICON_FA_COG, ImGui::GetColorU32(ImVec4(0.9f,0.9f,0.95f,1.0f)));
    if (settingsClicked) {
        ImGui::OpenPopup("##settings_popup");
    }

    if (ImGui::BeginPopup("##settings_popup")) {
        ImGui::Text("Settings");
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.18f, 0.18f, 0.18f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.24f, 0.24f, 0.24f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,      ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.00f));
        ImGui::Checkbox("VSync", &config.Visuals.VSync);
        ImGui::Checkbox("Black Background", &config.Visuals.Background);
        ImGui::Checkbox("Notifications", &config.Visuals.ToastsEnabled);
        ImGui::PopStyleColor(4);
        ImGui::Separator();
        ImGui::Text("Accent Presets");
        auto applyAccent = [&](ImVec4 c){
             ImGuiStyle& st = ImGui::GetStyle();
             st.Colors[ImGuiCol_ButtonHovered] = c;
             st.Colors[ImGuiCol_ButtonActive]  = c;
             st.Colors[ImGuiCol_HeaderHovered] = c;
             st.Colors[ImGuiCol_HeaderActive]  = c;
             st.Colors[ImGuiCol_SliderGrab]    = c;
             st.Colors[ImGuiCol_CheckMark]     = c;
            gAccent = c;
            config.Visuals.Accent = c;
         };
        if (ImGui::Button("Blue")) applyAccent(ImVec4(0.22f, 0.40f, 0.80f, 1.00f)); ImGui::SameLine();
        if (ImGui::Button("Purple")) applyAccent(ImVec4(0.55f, 0.30f, 0.75f, 1.00f)); ImGui::SameLine();
        if (ImGui::Button("Cyan")) applyAccent(ImVec4(0.20f, 0.70f, 0.80f, 1.00f)); ImGui::SameLine();
        if (ImGui::Button("Lime")) applyAccent(ImVec4(0.35f, 0.75f, 0.35f, 1.00f)); ImGui::SameLine();
        if (ImGui::Button("Orange")) applyAccent(ImVec4(0.90f, 0.55f, 0.25f, 1.00f)); ImGui::SameLine();
        if (ImGui::Button("Pink")) applyAccent(ImVec4(0.95f, 0.35f, 0.75f, 1.00f));
        ImGui::EndPopup();
    }

#ifdef ICON_FA_TIMES
    const char* closeIcon = ICON_FA_TIMES;
#else
    const char* closeIcon = "X";
#endif
    bool closeClicked = drawTitleButton(btnPosClose, closeIcon, ImGui::GetColorU32(ImVec4(0.95f,0.35f,0.35f,1.0f)));
    if (closeClicked) {
        Globals::Running = false;
        shouldRun = false;
        ExitProcess(0);
    }

    dl->PopClipRect();

    // Reserve content space under title bar without leaving a blank stripe
    ImGui::Dummy(ImVec2(0, titleBarHeight - ImMax(iconSize, textSize)));

    // Sidebar and content panels
    static const char* tabIcons[] = { ICON_FA_CROSSHAIRS, ICON_FA_EYE, ICON_FA_COG, ICON_FA_INFO_CIRCLE };
    float footerHeight = 32.0f;
    float sidebarWidth = 220.0f;
    float sidebarHeight = winSize.y - titleBarHeight - footerHeight;
    {
        ImVec2 panelMin = winPos + ImVec2(10, titleBarHeight + 6); // tighter gap, filled above
        ImVec2 panelMax = winPos + ImVec2(winSize.x - 10, winSize.y - 10);
        ImU32 bg = ImGui::GetColorU32(ImVec4(0.10f, 0.10f, 0.11f, 1.0f));
        DrawShadowRect(dl, panelMin, panelMax, 18.0f, ImGui::GetColorU32(ImVec4(0,0,0,0.20f)), 4, 4.0f, 0.35f);
        dl->AddRectFilled(panelMin, panelMax, bg, 18.0f);
        dl->AddRect(panelMin, panelMax, ImGui::GetColorU32(ImVec4(1,1,1,0.04f)), 18.0f);
    }

    ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, sidebarHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::SetScrollY(0);
        ImGui::PushFont(iconFont);
        float tabSpacing = 6.0f;
        float tabHeight = ImMin((sidebarHeight - ((m_Tabs.size() - 1) * tabSpacing)) / m_Tabs.size(), 44.0f);
        float iconTextSpacing = 14.0f;
        float tabPadding = 20.0f;
        float tabWidth = sidebarWidth;

        // Animated selection marker position (left pill) across tabs
        static float selMarkerY = -1.0f;
        float targetMarkerY = selMarkerY;

        // Animated hover/selection intensity per tab
        static std::vector<float> tabAnim; // 0..1 intensity
        if (tabAnim.size() != m_Tabs.size()) tabAnim.assign(m_Tabs.size(), 0.0f);

        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 childPos = ImGui::GetWindowPos();
        float dt = io.DeltaTime;

        for (int i = 0; i < m_Tabs.size(); i++) {
            ImGui::PushID(i);
            ImVec2 itemSize(tabWidth, tabHeight);
            ImVec2 itemPos = ImGui::GetCursorScreenPos();

            // Capture hover/click first
            ImGui::SetCursorScreenPos(itemPos);
            ImGui::InvisibleButton("##tab_btn", itemSize);
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) {
                m_iSelectedPage = i;
                config.Ui.LastTab = m_iSelectedPage;
            }
            bool selected = (m_iSelectedPage == i);

            // Animate intensity
            float target = selected ? 1.0f : (hovered ? 0.6f : 0.0f);
            float k = 1.0f - expf(-(selected ? 12.0f : 10.0f) * dt); // smooth factor based on dt
            tabAnim[i] += (target - tabAnim[i]) * k;

            // Update target selection marker Y for smooth slide
            if (selected) {
                targetMarkerY = itemPos.y + 6.0f;
                if (selMarkerY < 0.0f) selMarkerY = targetMarkerY; // first frame init
            }

            // Background gradient card with rounded corners
            ImU32 tabLeftCol = ImGui::ColorConvertFloat4ToU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.10f + 0.35f * tabAnim[i]));
            ImU32 tabRightCol = ImGui::ColorConvertFloat4ToU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.05f + 0.18f * tabAnim[i]));
            wdl->AddRectFilledMultiColor(itemPos, itemPos + ImVec2(tabWidth, tabHeight), tabLeftCol, tabRightCol, tabRightCol, tabLeftCol);
            // Outer border subtle accent
            float outline = 1.0f + 1.5f * tabAnim[i];
            wdl->AddRect(itemPos, itemPos + ImVec2(tabWidth, tabHeight), ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.40f + 0.35f * tabAnim[i])), 10.0f, 0, outline);

            // Active tab animated shine sweep
            if (selected && tabAnim[i] > 0.2f) {
                float t = (float)ImGui::GetTime();
                float sweepW = ImClamp(tabWidth * 0.35f, 80.0f, 160.0f);
                float sx = fmodf(t * 220.0f, tabWidth + sweepW) - sweepW;
                ImVec2 a = ImVec2(itemPos.x + sx, itemPos.y + 4.0f);
                ImVec2 b = ImVec2(itemPos.x + sx + sweepW, itemPos.y + tabHeight - 4.0f);
                ImU32 c1 = ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.14f * tabAnim[i]));
                ImU32 c2 = ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.02f));
                wdl->AddRectFilledMultiColor(a, b, c1, c2, c2, c1);
            }

            // Icon + text with subtle hover/active motion
            float startX = itemPos.x + tabPadding;
            float lift = (selected ? 1.0f : (hovered ? 0.5f : 0.0f)) * tabAnim[i];
            float iconY = itemPos.y + (tabHeight - (iconFont ? iconFont->FontSize : 21.5f)) / 2 + 1.5f - lift;
            float textY = itemPos.y + (tabHeight - (tabFont ? tabFont->FontSize : 16.5f)) / 2 - lift;
            float textX = startX + (iconFont ? iconFont->FontSize : 21.5f) + iconTextSpacing;

            ImGui::SetCursorScreenPos(ImVec2(startX, iconY));
            ImGui::PushFont(iconFont);
            ImVec4 ic = ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.50f + 0.45f * tabAnim[i]);
            ImGui::TextColored(ic, "%s", tabIcons[i]);
            ImGui::PopFont();

            ImGui::SetCursorScreenPos(ImVec2(textX, textY));
            ImGui::PushFont(tabFont);
            ImVec4 tc = selected ? ImVec4(0.95f, 0.96f, 0.98f, 1.00f) : ImVec4(0.78f, 0.80f, 0.83f, 0.95f);
            if (hovered && !selected) tc = ImVec4(0.88f, 0.88f, 0.92f, 1.0f);
            ImGui::TextColored(tc, "%s", m_Tabs[i]);
            ImGui::PopFont();

            // Move cursor to next row and add spacing
            ImGui::SetCursorScreenPos(ImVec2(itemPos.x, itemPos.y + itemSize.y));
            if (i < m_Tabs.size() - 1) {
                ImGui::Dummy(ImVec2(0, tabSpacing));
            }
            ImGui::PopID();
        }

        // Smoothly move selection marker (dt-based smoothing)
        if (targetMarkerY >= 0.0f) {
            float mk = 1.0f - expf(-12.0f * dt);
            selMarkerY += (targetMarkerY - selMarkerY) * mk;
        }
        if (selMarkerY >= 0.0f) {
            float padY = 6.0f;
            float markerW = 6.0f;
            float corner = 4.0f;
            ImVec2 markerA = ImVec2(childPos.x + 6.0f, selMarkerY);
            ImVec2 markerB = ImVec2(childPos.x + 6.0f + markerW, selMarkerY + (tabHeight - 2 * padY));
            ImU32 pillCol = ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.90f));
            wdl->AddRectFilled(markerA, markerB, pillCol, corner);
            DrawShadowRect(wdl, markerA, markerB, corner, ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.25f)), 2, 3.0f, 0.55f);
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Main Content ---
    ImGui::BeginChild("MainContent", ImVec2(0, sidebarHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::PushFont(featureFont);

        // Helper: draw gradient aligned to SeparatorText line, starting to the right of the text
        auto SectionHeader = [&](const char* label, bool large = true)
         {
            if (large && sectionFont) ImGui::PushFont(sectionFont);
            else if (large && tabFont) ImGui::PushFont(tabFont);
            ImAdd::SeparatorText(label);
            if (large && (sectionFont || tabFont)) ImGui::PopFont();

              ImDrawList* sdl = ImGui::GetWindowDrawList();
              ImVec2 itemMin = ImGui::GetItemRectMin();
              ImVec2 itemMax = ImGui::GetItemRectMax();
             float centerY = itemMin.y + (itemMax.y - itemMin.y) * 0.5f;
             ImVec2 contentMin = ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin();
             ImVec2 contentMax = ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMax();
             ImGuiStyle& st = ImGui::GetStyle();
             float text_w = ImGui::CalcTextSize(label).x;
             float padX = st.SeparatorTextPadding.x;
             float avail_w = contentMax.x - contentMin.x;
             float align = st.SeparatorTextAlign.x; // 0.0f=left, 0.5f=center
              float label_start_x = contentMin.x + ((avail_w - text_w - padX * 2.0f) * align) + padX;
              float startX = label_start_x + text_w + 1.0f;
              float h = ImMax(1.0f, st.SeparatorTextBorderSize);
              ImU32 leftC = ImGui::ColorConvertFloat4ToU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.00f));
              ImU32 rightC= ImGui::ColorConvertFloat4ToU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.25f));
              sdl->AddRectFilledMultiColor(ImVec2(startX, centerY - h * 0.5f), ImVec2(contentMax.x, centerY + h * 0.5f), leftC, rightC, rightC, leftC);
          };

        if (m_iSelectedPage == 0) // Aim
        {
            SectionHeader("Aim", true);

            // Split Aim into two panes (left: Aimbot, right: Triggerbot)
            float totalW = ImGui::GetContentRegionAvail().x;
            float leftW = totalW * 0.5f - 8.0f;

            ImGui::BeginChild("AimLeft", ImVec2(leftW, 0), false);
            {
                SectionHeader("Aimbot", true);
                ToggleSwitch("Enable", &config.Aim.Aimbot);
                if (config.Aim.Aimbot)
                {
                    if (ProcInfo::KmboxInitialized)
                    {
                        PropertyRow("Draw FOV", [&]{ ToggleSwitchNoLabel("##DrawFov", &config.Aim.DrawFov); }, "Draw circle representing aimbot FOV on screen");
                        PropertyRow("FOV Color", [&]{ ImAdd::ColorEdit4("##FovColor", (float*)&config.Aim.AimbotFovColor); });
                        PropertyRow("Aim Visible", [&]{ ToggleSwitchNoLabel("##AimVisible", &config.Aim.AimVisible); });
                        PropertyRow("Aim Teammates", [&]{ ToggleSwitchNoLabel("##AimFriendly", &config.Aim.AimFriendly); });
                        PropertyRow("Aimbot Key", [&]{ ImAdd::KeyBindOptions KeyMode = (ImAdd::KeyBindOptions)config.Aim.AimbotKeyMode; ImAdd::KeyBind("##AimbotKey", &config.Aim.AimbotKey, 0, &KeyMode); config.Aim.AimbotKeyMode = (int)KeyMode; });
                        PropertyRow("Aimbot FOV", [&]{ ImAdd::SliderFloat("##AimbotFov", &config.Aim.AimbotFov, 0.0f, 180.0f); }, "Maximum angle in degrees from crosshair to target to allow aimbot");
                        PropertyRow("Aimbot Smooth", [&]{ ImAdd::SliderFloat("##AimbotSmooth", &config.Aim.AimbotSmooth, 0.0f, 100.0f); }, "Higher = slower aiming for more human-like behavior");
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "KMBOX not connected.");
                    }
                }
            }
            ImGui::EndChild();

            ImGui::SameLine(0.0f, 14.0f);
            ImGui::BeginChild("AimRight", ImVec2(0, 0), false);
            {
                SectionHeader("Triggerbot", true);
                ToggleSwitch("Enable", &config.Aim.Trigger);
                if (config.Aim.Trigger)
                {
                    if (ProcInfo::KmboxInitialized)
                    {
                        PropertyRow("Trigger Key", [&]{ ImAdd::KeyBindOptions KeyMode = (ImAdd::KeyBindOptions)config.Aim.TriggerKeyMode; ImAdd::KeyBind("##TriggerKey", &config.Aim.TriggerKey, 0, &KeyMode); config.Aim.TriggerKeyMode = (int)KeyMode; });
                        PropertyRow("Trigger Delay (ms)", [&]{ ImAdd::SliderInt("##TriggerDelay", &config.Aim.TriggerDelay, 0, 250); });
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "KMBOX not connected.");
                    }
                }
            }
            ImGui::EndChild();
        }
        else if (m_iSelectedPage == 1) // Visuals
        {
            SectionHeader("Visuals", true);

            ToggleSwitch("Enable", &config.Visuals.Enabled);
            if (config.Visuals.Enabled)
            {
                // Split visuals into two panes to reduce vertical size
                float totalW = ImGui::GetContentRegionAvail().x;
                float leftW = totalW * 0.5f - 8.0f;
                ImGui::BeginChild("VisualsLeft", ImVec2(leftW, 0), false);
                {
                    SectionHeader("General", true);
                    PropertyRow("Watermark", [&]{ ToggleSwitchNoLabel("##Watermark", &config.Visuals.Watermark); });
                    if (config.Visuals.Watermark) {
                        PropertyRow("Watermark Color", [&]{ ImAdd::ColorEdit4("##WatermarkColor", (float*)&config.Visuals.WatermarkColor); });
                        PropertyRow("Watermark Position", [&]{
                            const char* positions[] = { "Top Right", "Top Left", "Top Middle", "Bottom Left", "Bottom Right" };
                            int currentPos = static_cast<int>(config.Visuals.WatermarkPos);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.18f, 0.18f, 0.18f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.24f, 0.24f, 0.24f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.60f));
                            float avail = ImGui::GetContentRegionAvail().x;
                            ImGui::SetNextWindowSizeConstraints(ImVec2(150.0f, 0.0f), ImVec2(ImMax(150.0f, avail), 300.0f));
                            if (ImGui::BeginCombo("##WatermarkPosition", positions[currentPos], ImGuiComboFlags_PopupAlignLeft)) {
                                for (int i = 0; i < IM_ARRAYSIZE(positions); ++i) {
                                    bool selected = (currentPos == i);
                                    if (ImGui::Selectable(positions[i], selected)) {
                                        currentPos = i;
                                        config.Visuals.WatermarkPos = static_cast<Structs::WatermarkPosition>(currentPos);
                                    }
                                    if (selected)
                                        ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::PopStyleColor(4);
                            ImGui::PopStyleVar();
                        });
                    }
                    PropertyRow("Accent", [&]{ ImAdd::ColorEdit4("##Accent", (float*)&config.Visuals.Accent); gAccent = config.Visuals.Accent; });
                    PropertyRow("Background", [&]{ ToggleSwitchNoLabel("##Background", &config.Visuals.Background); });
                    PropertyRow("Toasts", [&]{ ToggleSwitchNoLabel("##Toasts", &config.Visuals.ToastsEnabled); }, "Enable small notifications for actions like Save/Load");
                }
                ImGui::EndChild();

                ImGui::SameLine(0.0f, 14.0f);
                ImGui::BeginChild("VisualsRight", ImVec2(0, 0), false);
                {
                    SectionHeader("Players", true);
                    PropertyRow("Name", [&]{ ToggleSwitchNoLabel("##Name", &config.Visuals.Name); });
                    if (config.Visuals.Name) {
                        PropertyRow("Name Color", [&]{ ImAdd::ColorEdit4("##NameColor", (float*)&config.Visuals.NameColor); });
                    }
                    PropertyRow("Health", [&]{ ToggleSwitchNoLabel("##Health", &config.Visuals.Health); });
                    if (config.Visuals.Health) {
                        PropertyRow("Health Display", [&]{
                            const char* modes[] = { "Bar", "Bar + Number", "Number Only" };
                            int cur = static_cast<int>(config.Visuals.HealthType);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.18f, 0.18f, 0.18f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.24f, 0.24f, 0.24f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.60f));
                            float avail = ImGui::GetContentRegionAvail().x;
                            ImGui::SetNextWindowSizeConstraints(ImVec2(150.0f, 0.0f), ImVec2(ImMax(150.0f, avail), 300.0f));
                            if (ImGui::BeginCombo("##HealthDisplay", modes[cur], ImGuiComboFlags_PopupAlignLeft)) {
                                for (int i = 0; i < IM_ARRAYSIZE(modes); ++i) {
                                    bool selected = (cur == i);
                                    if (ImGui::Selectable(modes[i], selected)) {
                                        cur = i;
                                        config.Visuals.HealthType = static_cast<Structs::HealthDisplayMode>(cur);
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::PopStyleColor(4);
                            ImGui::PopStyleVar();
                        });
                    }
                    PropertyRow("Box", [&]{ ToggleSwitchNoLabel("##Box", &config.Visuals.Box); });
                    if (config.Visuals.Box) {
                        PropertyRow("Box Color", [&]{ ImAdd::ColorEdit4("##BoxColor", (float*)&config.Visuals.BoxColor); });
                        PropertyRow("Box Color Visible", [&]{ ImAdd::ColorEdit4("##BoxColorVisible", (float*)&config.Visuals.BoxColorVisible); });
                        PropertyRow("Box Style", [&]{
                            const char* styles[] = { "Outline", "Corners", "Filled" };
                            int cur = static_cast<int>(config.Visuals.BoxType);
                            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                            ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.18f, 0.18f, 0.18f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.24f, 0.24f, 0.24f, 1.00f));
                            ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.60f));
                            float avail = ImGui::GetContentRegionAvail().x;
                            ImGui::SetNextWindowSizeConstraints(ImVec2(150.0f, 0.0f), ImVec2(ImMax(150.0f, avail), 300.0f));
                            if (ImGui::BeginCombo("##BoxStyle", styles[cur], ImGuiComboFlags_PopupAlignLeft)) {
                                for (int i = 0; i < IM_ARRAYSIZE(styles); ++i) {
                                    bool selected = (cur == i);
                                    if (ImGui::Selectable(styles[i], selected)) {
                                        cur = i;
                                        config.Visuals.BoxType = static_cast<Structs::BoxStyle>(cur);
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                                ImGui::EndCombo();
                            }
                            ImGui::PopStyleColor(4);
                            ImGui::PopStyleVar();
                        });
                        PropertyRow("Box Thickness", [&]{
                            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.25f));
                            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.35f));
                            ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.85f));
                            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(gAccent.x, gAccent.y, gAccent.z, 1.00f));
                            float v_min = 0.5f, v_max = 6.0f;
                            ImGui::PushID("##BoxThickness");
                            ImVec2 pos = ImGui::GetCursorScreenPos();
                            float avail = ImGui::GetContentRegionAvail().x;
                            ImRect bb(pos, pos + ImVec2(avail, ImGui::GetFrameHeight()));
                            ImAdd::SliderFloat("", &config.Visuals.BoxThickness, v_min, v_max);
                            float t = (config.Visuals.BoxThickness - v_min) / (v_max - v_min);
                            ImU32 fillCol = ImGui::GetColorU32(ImVec4(gAccent.x, gAccent.y, gAccent.z, 0.35f));
                            DrawSliderProgressOnLastItem(t, fillCol);
                            ImGui::PopID();
                            ImGui::PopStyleColor(5);
                        }, "Outline thickness in pixels");
                    }
                    PropertyRow("Weapon", [&]{ ToggleSwitchNoLabel("##Weapon", &config.Visuals.Weapon); });
                    if (config.Visuals.Weapon) {
                        PropertyRow("Weapon Color", [&]{ ImAdd::ColorEdit4("##WeaponColor", (float*)&config.Visuals.WeaponColor); });
                    }
                    PropertyRow("Bones", [&]{ ToggleSwitchNoLabel("##Bones", &config.Visuals.Bones); });
                    if (config.Visuals.Bones) {
                        PropertyRow("Bones Color", [&]{ ImAdd::ColorEdit4("##BonesColor", (float*)&config.Visuals.BonesColor); });
                    }
                }
                ImGui::EndChild();
            }
        }
        else if (m_iSelectedPage == 2)
        {
            SectionHeader("Configs", true);
            static char configName[128] = "";
            static std::vector<std::string> configFiles;
            static int lastSelectedTab = -1;
            static bool menuWasOpen = false;
            if (shouldRenderMenu && !menuWasOpen) { menuWasOpen = true; configFiles = config.ListConfigs("configs/"); }
            if (!shouldRenderMenu) { menuWasOpen = false; }
            if (m_iSelectedPage == MenuPage_Config && lastSelectedTab != MenuPage_Config) { configFiles = config.ListConfigs("configs/"); }
            lastSelectedTab = m_iSelectedPage;

            if (ImAdd::Button("Refresh")) { configFiles = config.ListConfigs("configs/"); }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.13f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.22f, 0.5f));
            if (ImGui::BeginListBox("Config list"))
            {
                if (configFiles.empty())
                    ImGui::Selectable("No configs found", false, ImGuiSelectableFlags_Disabled);
                else
                {
                    for (const auto& file : configFiles)
                    {
                        bool isSelected = (file == configName);
                        if (ImGui::Selectable(file.c_str(), isSelected))
                            strcpy(configName, file.c_str());
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndListBox();
            }
            ImGui::PopStyleColor(2);

            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.13f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.22f, 0.5f));
            ImGui::InputText("Config Name", configName, IM_ARRAYSIZE(configName));
            ImGui::PopStyleColor(2);

            float buttonWidth = 75.0f;
            float buttonSpacing = 10.0f;
            ImGui::Dummy(ImVec2(0.0f, 5.0f));

            if (ImAdd::Button("Load", ImVec2(buttonWidth, 0)))
            {
                std::string filePath = "configs/" + std::string(configName);
                if (!config.LoadFromFile(filePath)) { /* toasts below */ }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Save", ImVec2(buttonWidth, 0)))
            {
                std::string filePath = "configs/" + std::string(configName);
                if (!config.SaveToFile(filePath)) { /* toasts below */ }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Delete", ImVec2(buttonWidth, 0)))
            {
                std::string filePath = "configs/" + std::string(configName);
                if (!config.DeleteConfigFile(filePath)) { /* toasts below */ }
                else { configFiles = config.ListConfigs("configs/"); }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Import", ImVec2(buttonWidth, 0)))
            {
                if (!config.LoadFromClipboard()) { /* toasts below */ }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Unload", ImVec2(buttonWidth, 0)))
            {
                Globals::Running = false; shouldRun = false; ExitProcess(0);
            }
        }
        else if (m_iSelectedPage == 3)
        {
            SectionHeader("Info", true);
            SectionHeader("Hardware", true);
            ImGui::Text("DMA:"); ImGui::SameLine(); ImGui::TextColored(ProcInfo::DmaInitialized ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1), "%s", ProcInfo::DmaInitialized ? "Connected" : "Disconnected");
            ImGui::Text("KMBOX:"); ImGui::SameLine(); ImGui::TextColored(ProcInfo::KmboxInitialized ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1), "%s", ProcInfo::KmboxInitialized ? "Connected" : "Disconnected");
            SectionHeader("Game", true);
            ImGui::Text("Client:"); ImGui::SameLine(); ImGui::Text("0x%llx", Globals::ClientBase);
            SectionHeader("Cheat", true);
            ImGui::Text("Overlay FPS: %.2f", OverlayFps);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0,0,0,0));
            ImGui::PlotLines("", fpsHistory.data(), (int)fpsHistory.size(), fpsIndex, nullptr, 0.0f, 240.0f, ImVec2(-1, 60.0f));
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
             float buttonWidth = 100.0f; float buttonSpacing = 20.0f;
             ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 2 * buttonWidth - buttonSpacing) / 2);
             if (ImAdd::Button("Open folder", ImVec2(buttonWidth, 0))) { ShellExecuteA(nullptr, "open", "explorer.exe", ".\\", nullptr, SW_SHOW); }
             ImGui::SameLine();
             if (ImAdd::Button("Unload", ImVec2(buttonWidth, 0))) { Globals::Running = false; shouldRun = false; ExitProcess(0); }
        }

        // Toasts drawn top-right (stack downward), if enabled
        if (config.Visuals.ToastsEnabled && !toasts.empty()) {
            auto now = std::chrono::steady_clock::now();
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            float padding = 12.0f;
            float spacing = 8.0f;
            ImVec2 base(Screen.x - padding, padding);
            for (int i = (int)toasts.size() - 1, idx = 0; i >= 0; --i, ++idx) {
                float remaining = std::chrono::duration<float>(toasts[i].expiry - now).count();
                if (remaining <= 0.0f) { toasts.erase(toasts.begin() + i); continue; }
                float alpha = remaining < 0.25f ? remaining / 0.25f : 1.0f;
                ImVec4 bg = ImVec4(0.10f, 0.10f, 0.11f, 0.92f * alpha);
                ImVec4 border;
                const char* icon = ICON_FA_INFO_CIRCLE;
                if (toasts[i].type == ToastType::Success) { border = ImVec4(0.25f, 0.75f, 0.35f, alpha); icon = ICON_FA_CHECK; }
                else if (toasts[i].type == ToastType::Error) { border = ImVec4(0.95f, 0.35f, 0.35f, alpha); icon = ICON_FA_EXCLAMATION_TRIANGLE; }
                else { border = ImVec4(gAccent.x, gAccent.y, gAccent.z, alpha); }
                ImVec2 textSz = ImGui::CalcTextSize(toasts[i].text.c_str());
                float height = ImMax(36.0f, textSz.y + 14.0f);
                float width = textSz.x + 56.0f;
                ImVec2 bMin(base.x - width, base.y + idx * (height + spacing));
                ImVec2 bMax(base.x, bMin.y + height);
                DrawShadowRect(fdl, bMin, bMax, 8.0f, ImGui::GetColorU32(ImVec4(0,0,0,0.35f * alpha)), 3, 3.5f, 0.45f);
                fdl->AddRectFilled(bMin, bMax, ImGui::GetColorU32(bg), 8.0f);
                fdl->AddRect(bMin, bMax, ImGui::GetColorU32(border), 8.0f, 0, 1.5f);
                ImVec2 iconPos = ImVec2(bMin.x + 10.0f, bMin.y + (height - (iconFont ? iconFont->FontSize : 18.0f)) * 0.5f - 1.0f);
                ImGui::PushFont(iconFont);
                fdl->AddText(iconFont, iconFont ? iconFont->FontSize : 18.0f, iconPos, ImGui::GetColorU32(border), icon);
                ImGui::PopFont();
                ImVec2 textPos = ImVec2(iconPos.x + 28.0f, bMin.y + (height - textSz.y) * 0.5f);
                fdl->AddText(textPos, ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.98f, alpha)), toasts[i].text.c_str());
            }
        }

        ImGui::PopFont();
    }
    ImGui::End();
}

bool Overlay::Create()
{
    shouldRun = true;
    shouldRenderMenu = false;

    // Initialize tabs only once
    m_Tabs.clear();
    m_Tabs.push_back("Aim");
    m_Tabs.push_back("Visuals");
    m_Tabs.push_back("Config");
    m_Tabs.push_back("Info");
    m_iSelectedPage = std::clamp(config.Ui.LastTab, 0, (int)3);

    if (!CreateOverlay())
        return false;

    if (!CreateDevice())
        return false;

    if (!CreateImGui())
        return false;

    SetForeground(GetConsoleWindow());
    return true;
}

void Overlay::Destroy()
{
    DestroyImGui();
    DestroyDevice();
    DestroyOverlay();
}
