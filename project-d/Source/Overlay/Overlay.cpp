#include <Pch.hpp>
#include <SDK.hpp>
#include <chrono>
#include <spdlog/spdlog.h>

#include "Overlay.hpp"
#include "Fonts/IBMPlexMono_Medium.h"
#include "Fonts/font_awesome.h"
#include "Fonts/font_awesome.cpp"
#include "Features.hpp"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
/*
If you want to change the icons or see what all icons there are available, you can use the font viewwer I have that works with the font_awesome.h/cpp.
To use it, just uncomment the line below. Then open the normal menu which in my current case is Insert, then once the menu is open, press F10 to open the icon viewer.
*/
// #define SHOW_ICON_FONT_VIEWER
ID3D11Device* Overlay::device = nullptr;

ID3D11DeviceContext* Overlay::device_context = nullptr;

IDXGISwapChain* Overlay::swap_chain = nullptr;

ID3D11RenderTargetView* Overlay::render_targetview = nullptr;

HWND Overlay::overlay = nullptr;
WNDCLASSEX Overlay::wc = { };
ImFont* iconFont = nullptr; // Global/static for Overlay.cpp
ImFont* titleFont = nullptr; // Title font (bold, 19pt)
ImFont* tabFont = nullptr;   // Tab font (16.5pt)
ImFont* featureFont = nullptr; // Feature font (15pt)

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static const ImVec4 blueAccent = ImVec4(0.22f, 0.40f, 0.80f, 1.00f);

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

	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

	sd.OutputWindow = overlay;

	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;

	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

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
		&swap_chain,
		&device,
		&featureLevel,
		&device_context);

	if (result == DXGI_ERROR_UNSUPPORTED) {
		result = D3D11CreateDeviceAndSwapChain(
			nullptr,
			D3D_DRIVER_TYPE_WARP,
			nullptr,
			0U,
			featureLevelArray,
			2, D3D11_SDK_VERSION,
			&sd,
			&swap_chain,
			&device,
			&featureLevel,
			&device_context);

		LOG_ERROR("Created with D3D_DRIVER_TYPE_WARP");
	}

	if (result != S_OK) {
		LOG_ERROR("Device not supported");
		return false;
	}

	ID3D11Texture2D* back_buffer{ nullptr };
	swap_chain->GetBuffer(0U, IID_PPV_ARGS(&back_buffer));

	if (back_buffer)
	{
		device->CreateRenderTargetView(back_buffer, nullptr, &render_targetview);
		back_buffer->Release();
		return true;
	}

	LOG_ERROR("Failed to create device");
	return false;
}

void Overlay::DestroyDevice()
{
    if (device_context)
    {
        device_context->Release();
        device_context = nullptr;
    }
    else
    {
        LOG_ERROR("device_context is null during cleanup");
    }

    if (swap_chain)
    {
        swap_chain->Release();
        swap_chain = nullptr;
    }
    else
    {
        LOG_ERROR("swap_chain is null during cleanup");
    }

    if (render_targetview)
    {
        render_targetview->Release();
        render_targetview = nullptr;
    }
    else
    {
        LOG_ERROR("render_targetview is null during cleanup");
    }

    if (device)
    {
        device->Release();
        device = nullptr;
    }
    else
    {
        LOG_ERROR("device is null during cleanup");
    }
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
		LOG_ERROR("Failed to create overlay");
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

	SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

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
		LOG_ERROR("Failed ImGui_ImplWin32_Init");
		return false;
	}

	if (!ImGui_ImplDX11_Init(device, device_context)) {
		LOG_ERROR("Failed ImGui_ImplDX11_Init");
		return false;
	}

	// Font loading (only ONCE, after context is created)
	static bool fontAtlasBuilt = false;
	if (!fontAtlasBuilt) {
		ImGuiIO& IO = ImGui::GetIO();
        titleFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\TahomaBD.ttf", 35.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        tabFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Tahoma.ttf", 20.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        featureFont = IO.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\Tahoma.ttf", 18.0f, nullptr, IO.Fonts->GetGlyphRangesDefault());
        ImFont* MainFont = tabFont;
        IO.FontDefault = MainFont;
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
    auto beforeRender = std::chrono::steady_clock::now();
    ImGui::Render();
    auto afterRender = std::chrono::steady_clock::now();

    float color[4];
    if (config.Visuals.Background) // Black bg
    {
        color[0] = 0; color[1] = 0; color[2] = 0; color[3] = 1;
    }
    else // Transparent bg
    {
        color[0] = 0; color[1] = 0; color[2] = 0; color[3] = 0;
    }

    device_context->OMSetRenderTargets(1, &render_targetview, nullptr);
    device_context->ClearRenderTargetView(render_targetview, color);

    // Always draw FOV circle if enabled (before rendering draw data)
    if (config.Aim.DrawFov)
    {
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        ImVec2 center(Screen.x / 2.0f, Screen.y / 2.0f);
        float radius = config.Aim.AimbotFov;
        ImVec4 color = config.Aim.AimbotFovColor;
        drawList->AddCircle(center, radius, ImGui::GetColorU32(color), 0, 2.0f);
    }
    
    // Add watermark if enabled
    if (config.Visuals.Watermark)
    {
        // Get FPS for watermark
        float fps = ImGui::GetIO().Framerate;
        
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
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
        
        // Draw shadow for better visibility against any background
        drawList->AddText(ImVec2(posX + 1, posY + 1), IM_COL32(0, 0, 0, 180), watermarkText);
        
        // Draw normal text part
        drawList->AddText(ImVec2(posX, posY), textColor, watermarkText);
        
        // Draw "Arctic" in bold (simulated by drawing it multiple times with slight offsets)
        // First draw the bold Arctic text with shadow
        drawList->AddText(ImVec2(posX + textSize.x + 1, posY + 1), IM_COL32(0, 0, 0, 180), "Arctic");
        
        // Create bold effect by drawing the text multiple times with slight offsets
        for (float dx = -0.5f; dx <= 0.5f; dx += 0.5f) {
            for (float dy = -0.5f; dy <= 0.5f; dy += 0.5f) {
                if (dx != 0 || dy != 0) {
                    drawList->AddText(ImVec2(posX + textSize.x + dx, posY + dy), textColor, "Arctic");
                }
            }
        }
        
        // Draw main "Arctic" text on top
        drawList->AddText(ImVec2(posX + textSize.x, posY), textColor, "Arctic");
    }

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    auto beforePresent = std::chrono::steady_clock::now();
    
    // Use DXGI_PRESENT_DO_NOT_WAIT to prevent blocking on VSync if possible
    UINT presentFlags = 0;
    if (!config.Visuals.VSync) {
        presentFlags = 0; // Use 0 for immediate mode
    }
    
    HRESULT hr = swap_chain->Present(config.Visuals.VSync ? 1 : 0, presentFlags);
    
    auto afterPresent = std::chrono::steady_clock::now();

    // Ensure overlay stays topmost every frame to prevent flicker
    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Log timings for diagnostics
    static int frameCount = 0;
    frameCount++;
#ifdef OVERLAY_LOGGING_ENABLED
    if (frameCount % 60 == 0) { // Log every 60 frames
        auto frameStart = beforeRender; // Use beforeRender as frame start
        auto frameEnd = afterPresent;
        spdlog::info("Overlay timings (us): NewFrame+Render={} | RenderDrawData={} | Present={} | FullFrame={}",
            std::chrono::duration_cast<std::chrono::microseconds>(beforeRender - frameStart).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(afterRender - beforeRender).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(afterPresent - beforePresent).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count()
        );
    }
#endif
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
    style.Colors[ImGuiCol_FrameBgHovered]     = blueAccent;
    style.Colors[ImGuiCol_FrameBgActive]      = blueAccent;
    style.Colors[ImGuiCol_TitleBg]            = darkBg;
    style.Colors[ImGuiCol_TitleBgActive]      = darkBg;
    style.Colors[ImGuiCol_TitleBgCollapsed]   = darkBg;
    style.Colors[ImGuiCol_Border]             = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
    style.Colors[ImGuiCol_ButtonHovered]      = blueAccent;
    style.Colors[ImGuiCol_ButtonActive]       = blueAccent;
    style.Colors[ImGuiCol_HeaderHovered]      = blueAccent;
    style.Colors[ImGuiCol_HeaderActive]       = blueAccent;
    style.Colors[ImGuiCol_SliderGrab]         = blueAccent;
    style.Colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.32f, 0.50f, 0.90f, 1.00f);
    style.Colors[ImGuiCol_CheckMark]          = blueAccent;
    style.Colors[ImGuiCol_Text]               = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]       = ImVec4(0.60f, 0.62f, 0.65f, 1.00f);
    style.Colors[ImGuiCol_Separator]          = ImVec4(0.18f, 0.19f, 0.22f, 0.60f);
    style.Colors[ImGuiCol_TabHovered]         = blueAccent;
    style.Colors[ImGuiCol_TabActive]          = blueAccent;
    style.Colors[ImGuiCol_TabUnfocusedActive] = blueAccent;
    style.Colors[ImGuiCol_DragDropTarget]     = blueAccent;
    style.Colors[ImGuiCol_NavHighlight]       = blueAccent;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = blueAccent;
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = blueAccent;

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
    ImU32 col_bg = ImGui::GetColorU32(*v ? ImVec4(0.22f, 0.40f, 0.80f, 1.0f) : ImVec4(0.18f, 0.19f, 0.22f, 1.0f));
    // Draw label
    draw_list->AddText(p, ImGui::GetColorU32(ImGuiCol_Text), label);
    // Draw toggle
    ImVec2 togglePos = p + ImVec2(ImGui::CalcTextSize(label).x + spacing, 0);
    draw_list->AddRectFilled(togglePos, togglePos + ImVec2(width, height), col_bg, height * 0.5f);
    draw_list->AddCircleFilled(togglePos + ImVec2(height * 0.5f + t * (width - height), height * 0.5f), height * 0.4f, ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.98f, 1.0f)));
    return *v;
}

void Overlay::RenderMenu()
{
    static std::string configNotification;
    static float notificationTimer = 0.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    StyleMenu(io, style);
    float OverlayFps = ImGui::GetIO().Framerate;

    // Toast system (simple)
    struct Toast { std::string text; std::chrono::steady_clock::time_point expiry; };
    static std::vector<Toast> toasts;
    auto pushToast = [&](const std::string& msg, float seconds = 2.0f) {
        toasts.push_back({ msg, std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000)) });
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
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver); // Wider default size
        ImGui::Begin("Font Awesome Glyphs", &showIconFontViewer, ImGuiWindowFlags_NoCollapse); // Resizable by default
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

    ImGui::SetNextWindowSize(ImVec2(1100, 750), ImGuiCond_Always);
    ImGui::Begin("Aetherial", &shouldRenderMenu, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // --- Gradient Title Bar ---
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    float textSize = titleFont ? titleFont->FontSize : 35.0f;
    float iconSize = iconFont ? iconFont->FontSize : 28.0f;
    float paddingY = 5.0f; // 5px above and below text
    float titleBarHeight = ImMax(textSize, iconSize) + 2 * paddingY;
    float rounding = 16.0f;
    // Increased contrast but enforce min brightness of RGB(25,25,25)
    const float kMin = 25.0f / 255.0f;
    ImVec4 leftCol = ImVec4(0.20f, 0.22f, 0.26f, 1.0f); // a bit brighter left
    ImVec4 rightCol = ImVec4(0.10f, 0.10f, 0.12f, 1.0f); // clamp >= 25/255
    leftCol.x = ImMax(leftCol.x, kMin); leftCol.y = ImMax(leftCol.y, kMin); leftCol.z = ImMax(leftCol.z, kMin);
    rightCol.x = ImMax(rightCol.x, kMin); rightCol.y = ImMax(rightCol.y, kMin); rightCol.z = ImMax(rightCol.z, kMin);
    ImU32 titleLeft = ImGui::ColorConvertFloat4ToU32(leftCol);
    ImU32 titleRight = ImGui::ColorConvertFloat4ToU32(rightCol);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Draw background gradient full-width
    dl->PushClipRect(winPos, winPos + winSize, false);
    dl->AddRectFilledMultiColor(winPos, winPos + ImVec2(winSize.x, titleBarHeight), titleLeft, titleRight, titleRight, titleLeft);
    dl->AddRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight), titleLeft, rounding, ImDrawFlags_RoundCornersTop, 2.0f);
    dl->AddRectFilled(winPos + ImVec2(0, titleBarHeight - 2), winPos + ImVec2(winSize.x, titleBarHeight + 8), ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,0.22f)));

    // Animated accent underline across title bar
    {
        float underlineY = winPos.y + titleBarHeight - 3.0f;
        float underlineH = 2.0f;
        // Base subtle line
        dl->AddRectFilled(ImVec2(winPos.x, underlineY), ImVec2(winPos.x + winSize.x, underlineY + underlineH), ImGui::GetColorU32(ImVec4(0.20f, 0.22f, 0.26f, 1.0f)));
        // Moving highlight segment
        float t = (float)ImGui::GetTime();
        float segW = 140.0f;
        float speed = 120.0f; // px/sec
        float x = fmodf(t * speed, winSize.x + segW) - segW;
        ImVec2 a = ImVec2(winPos.x + x, underlineY);
        ImVec2 b = ImVec2(winPos.x + x + segW, underlineY + underlineH);
        dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.22f, 0.40f, 0.90f, 0.85f)));
    }

    dl->PopClipRect();

    // Window drag via title bar (avoid when hovering items)
    ImRect titleBarRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight));
    if (ImGui::IsMouseHoveringRect(titleBarRect.Min, titleBarRect.Max) && !ImGui::IsAnyItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImGui::SetWindowPos(ImGui::GetWindowPos() + io.MouseDelta);
    }

    // Ensure all title content is not clipped by inner padding
    dl->PushClipRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight), false);

    // Title text + left icon
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
    ImGui::TextColored(blueAccent, ICON_FA_MOON);
    ImGui::PopFont();
    ImGui::SameLine(0, 18.0f);
    ImGui::SetCursorPos(ImVec2(startX + iconSize + 18.0f, textY));
    ImGui::PushFont(titleFont);
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.00f), "%s", titleText);
    ImGui::PopFont();

    // Title bar buttons (right): settings and close
    float btnSize = 26.0f;
    float btnPadding = 8.0f;
    ImVec2 btnPosClose = winPos + ImVec2(winSize.x - btnPadding - btnSize, (titleBarHeight - btnSize) * 0.5f);
    ImVec2 btnPosSettings = btnPosClose - ImVec2(btnSize + 6.0f, 0);

    auto drawTitleButton = [&](const ImVec2& pos, const char* icon, ImU32 fg) -> bool {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton(icon, ImVec2(btnSize, btnSize));
        bool hovered = ImGui::IsItemHovered();
        ImU32 bgCol = hovered ? ImGui::GetColorU32(ImVec4(0.22f, 0.40f, 0.80f, 0.25f)) : ImGui::GetColorU32(ImVec4(0,0,0,0));
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
        ImGui::Checkbox("VSync", &config.Visuals.VSync);
        ImGui::Checkbox("Black Background", &config.Visuals.Background);
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

    ImGui::Dummy(ImVec2(0, titleBarHeight - ImMax(iconSize, textSize)));

    // --- Sidebar ---
    static const char* tabIcons[] = {
        ICON_FA_CROSSHAIRS, // Aim
        ICON_FA_EYE,        // Visuals
        ICON_FA_COG,        // Config
        ICON_FA_INFO_CIRCLE // Info
    };
    float footerHeight = 32.0f;
    float sidebarWidth = 220.0f;
    float sidebarHeight = winSize.y - titleBarHeight - footerHeight;
    ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, sidebarHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::SetScrollY(0);
        ImGui::PushFont(iconFont);
        float tabSpacing = 4.0f;
        float tabHeight = ImMin((sidebarHeight - ((m_Tabs.size() - 1) * tabSpacing)) / m_Tabs.size(), 40.0f);
        float iconTextSpacing = 16.0f;
        float tabPadding = 22.0f;
        float tabWidth = sidebarWidth;
        static float animPillY = 0.0f; // animated selection marker Y
        for (int i = 0; i < m_Tabs.size(); i++) {
            ImGui::PushID(i);
            bool selected = (m_iSelectedPage == i);
            ImVec2 itemSize(tabWidth, tabHeight);
            ImVec2 itemPos = ImGui::GetCursorScreenPos();
            if (selected) {
                // Animated selection pill at left side
                float targetY = itemPos.y + 6.0f;
                if (animPillY == 0.0f) animPillY = targetY;
                animPillY = animPillY + (targetY - animPillY) * 0.15f; // smooth
                ImVec2 pillA = ImVec2(itemPos.x + 6.0f, animPillY);
                ImVec2 pillB = ImVec2(itemPos.x + 10.0f, animPillY + tabHeight - 12.0f);
                ImGui::GetWindowDrawList()->AddRectFilled(pillA, pillB, ImGui::GetColorU32(blueAccent), 4.0f);

                // Selected background
                ImU32 tabLeft = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.40f, 0.80f, 0.45f));
                ImU32 tabRight = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.40f, 0.80f, 0.18f));
                ImGui::GetWindowDrawList()->AddRectFilledMultiColor(itemPos, itemPos + ImVec2(tabWidth, tabHeight), tabLeft, tabRight, tabRight, tabLeft);
                ImGui::GetWindowDrawList()->AddRect(itemPos, itemPos + ImVec2(tabWidth, tabHeight), ImGui::GetColorU32(ImVec4(0.22f, 0.40f, 0.80f, 0.85f)), 8.0f, 0, 2.0f);
            }
            float startX = itemPos.x + tabPadding;
            float iconY = itemPos.y + (tabHeight - (iconFont ? iconFont->FontSize : 21.5f)) / 2 + 2.0f;
            float textY = itemPos.y + (tabHeight - (tabFont ? tabFont->FontSize : 16.5f)) / 2;
            float textX = startX + (iconFont ? iconFont->FontSize : 21.5f) + iconTextSpacing;
            ImGui::SetCursorScreenPos(ImVec2(startX, iconY));
            ImGui::TextColored(selected ? ImVec4(0.22f, 0.40f, 0.80f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", tabIcons[i]);
            ImGui::SetCursorScreenPos(ImVec2(textX, textY));
            ImGui::PushFont(tabFont);
            ImGui::TextColored(selected ? ImVec4(0.95f, 0.96f, 0.98f, 1.0f) : ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", m_Tabs[i]);
            ImGui::PopFont();
            ImGui::SetCursorScreenPos(itemPos);
            if (ImGui::InvisibleButton("##tab", itemSize)) {
                m_iSelectedPage = i;
            }
            ImGui::PopID();
            if (i < m_Tabs.size() - 1) {
                ImGui::Dummy(ImVec2(0, tabSpacing));
            }
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Main Content ---
    // Remove extra outer rounded rectangle: no bordered MainContent, only panel per tab
    ImGui::BeginChild("MainContent", ImVec2(0, sidebarHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::PushFont(featureFont);

        // One panel per tab (no extra outer border). Only inner sections get borders.
        if (m_iSelectedPage == 0) // Aim
        {
            // Section header (no container border here)
            ImAdd::SeparatorText("Aim");

            ImGui::Columns(2, nullptr, false);
            float yStart = ImGui::GetCursorPosY();

            // Left: Aimbot
            ImGui::SetCursorPosY(yStart);
            ImGui::BeginChild("AimbotSection", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            if (ImGui::BeginMenuBar()) { ImGui::TextColored(ImVec4(0.85f, 0.86f, 0.88f, 1.0f), "Aimbot"); ImGui::EndMenuBar(); }
            {
                ToggleSwitch("Enable", &config.Aim.Aimbot);
                if (config.Aim.Aimbot)
                {
                    if (ProcInfo::KmboxInitialized)
                    {
                        ToggleSwitch("Draw FOV", &config.Aim.DrawFov);
                        ToggleSwitch("Aim Visible", &config.Aim.AimVisible);
                        ToggleSwitch("Aim Teammates", &config.Aim.AimFriendly);
                        ImAdd::KeyBindOptions KeyMode = (ImAdd::KeyBindOptions)config.Aim.AimbotKeyMode;
                        ImAdd::KeyBind("Aimbot Key", &config.Aim.AimbotKey, 0, &KeyMode);
                        ImAdd::SliderFloat("Aimbot Fov", &config.Aim.AimbotFov, 0.0f, 180.0f);
                        ImAdd::SliderFloat("Aimbot Smooth", &config.Aim.AimbotSmooth, 0.0f, 100.0f);
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "KMBOX not connected.");
                    }
                }
            }
            ImGui::EndChild();

            ImGui::NextColumn();

            // Right: Triggerbot
            ImGui::SetCursorPosY(yStart);
            ImGui::BeginChild("TriggerbotSection", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            if (ImGui::BeginMenuBar()) { ImGui::TextColored(ImVec4(0.85f, 0.86f, 0.88f, 1.0f), "Triggerbot"); ImGui::EndMenuBar(); }
            {
                ToggleSwitch("Enable", &config.Aim.Trigger);
                if (config.Aim.Trigger)
                {
                    if (ProcInfo::KmboxInitialized)
                    {
                        ImAdd::KeyBindOptions KeyMode = (ImAdd::KeyBindOptions)config.Aim.TriggerKeyMode;
                        ImAdd::KeyBind("Trigger Key", &config.Aim.TriggerKey, 0, &KeyMode);
                        ImAdd::SliderInt("Trigger Delay (ms)", &config.Aim.TriggerDelay, 0, 250);
                    }
                    else
                    {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "KMBOX not connected.");
                    }
                }
            }
            ImGui::EndChild();
            ImGui::Columns(1);
        }
        else if (m_iSelectedPage == 1) // Visuals
        {
            ImAdd::SeparatorText("Visuals");

            ToggleSwitch("Enable", &config.Visuals.Enabled);
            if (config.Visuals.Enabled)
            {
                ImAdd::SeparatorText("General");
                ImGui::BeginGroup();
                ToggleSwitch("Watermark", &config.Visuals.Watermark);
                if (config.Visuals.Watermark) {
                    ImAdd::ColorEdit4("Watermark Color", (float*)&config.Visuals.WatermarkColor);
                    // Watermark Position: label on the left, dropdown to the right with higher contrast and aligned vertically
                    const char* positions[] = { "Top Right", "Top Left", "Top Middle", "Bottom Left", "Bottom Right" };
                    int currentPos = static_cast<int>(config.Visuals.WatermarkPos);

                    // Vertically align text with frame height
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted("Watermark Position");
                    ImGui::SameLine();

                    // Increase contrast for just this combo
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg,        ImVec4(0.18f, 0.18f, 0.18f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.22f, 0.22f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  ImVec4(0.24f, 0.24f, 0.24f, 1.00f));
                    ImGui::PushStyleColor(ImGuiCol_Border,         ImVec4(0.22f, 0.40f, 0.80f, 0.60f));
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::Combo("##WatermarkPosition", &currentPos, positions, IM_ARRAYSIZE(positions))) {
                        config.Visuals.WatermarkPos = static_cast<Structs::WatermarkPosition>(currentPos);
                    }
                    ImGui::PopStyleColor(4);
                    ImGui::PopStyleVar();
                }
                ToggleSwitch("Background", &config.Visuals.Background);
                ImGui::EndGroup();

                ImAdd::SeparatorText("Visual");
                ImGui::BeginGroup();
                ToggleSwitch("VSync", &config.Visuals.VSync);
                ToggleSwitch("Team Check", &config.Visuals.TeamCheck);
                ToggleSwitch("Visible Check", &config.Visuals.VisibleCheck);
                ToggleSwitch("Hitmarker", &config.Visuals.Hitmarker);
                if (config.Visuals.Hitmarker)
                    ImAdd::ColorEdit4("Hitmarker Color", (float*)&config.Visuals.HitmarkerColor);
                ImGui::EndGroup();

                ImAdd::SeparatorText("Players");
                ImGui::BeginGroup();
                ToggleSwitch("Name", &config.Visuals.Name);
                if (config.Visuals.Name)
                    ImAdd::ColorEdit4("Name Color", (float*)&config.Visuals.NameColor);
                ToggleSwitch("Health", &config.Visuals.Health);
                ToggleSwitch("Box", &config.Visuals.Box);
                if (config.Visuals.Box)
                {
                    ImAdd::ColorEdit4("Box Color", (float*)&config.Visuals.BoxColor);
                    ImAdd::ColorEdit4("Box Color Visible", (float*)&config.Visuals.BoxColorVisible);
                }
                ToggleSwitch("Weapon", &config.Visuals.Weapon);
                if (config.Visuals.Weapon)
                    ImAdd::ColorEdit4("Weapon Color", (float*)&config.Visuals.WeaponColor);
                ToggleSwitch("Bones", &config.Visuals.Bones);
                if (config.Visuals.Bones)
                    ImAdd::ColorEdit4("Bones Color", (float*)&config.Visuals.BonesColor);
                ImGui::EndGroup();
            }
        }
        else if (m_iSelectedPage == 2) // Config
        {
            ImAdd::SeparatorText("Configs");
            // Render actual config controls (moved out of inner child)
            static char configName[128] = "";
            static std::vector<std::string> configFiles;
            static int lastSelectedTab = -1;
            static bool menuWasOpen = false;
            if (shouldRenderMenu && !menuWasOpen) { menuWasOpen = true; configFiles = config.ListConfigs("configs/"); }
            if (!shouldRenderMenu) { menuWasOpen = false; }
            if (m_iSelectedPage == MenuPage_Config && lastSelectedTab != MenuPage_Config) { configFiles = config.ListConfigs("configs/"); }
            lastSelectedTab = m_iSelectedPage;

            if (ImAdd::Button("Refresh")) { configFiles = config.ListConfigs("configs/"); LOG_INFO("Refreshed config list"); }
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
                if (!config.LoadFromFile(filePath)) { LOG_ERROR("Failed to load config: {}", filePath); }
                else { LOG_INFO("Loaded config: {}", filePath); }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Save", ImVec2(buttonWidth, 0)))
            {
                std::string filePath = "configs/" + std::string(configName);
                if (!config.SaveToFile(filePath)) { LOG_ERROR("Failed to save config: {}", filePath); }
                else { LOG_INFO("Saved config: {}", filePath); }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Delete", ImVec2(buttonWidth, 0)))
            {
                std::string filePath = "configs/" + std::string(configName);
                if (!config.DeleteConfigFile(filePath)) { LOG_ERROR("Failed to delete config: {}", filePath); }
                else { LOG_INFO("Deleted config: {}", filePath); configFiles = config.ListConfigs("configs/"); }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Import", ImVec2(buttonWidth, 0)))
            {
                if (!config.LoadFromClipboard()) { LOG_ERROR("Failed to import config from clipboard"); }
                else { LOG_INFO("Config imported from clipboard"); }
            }
            ImGui::SameLine(0.0f, buttonSpacing);
            if (ImAdd::Button("Unload", ImVec2(buttonWidth, 0)))
            {
                Globals::Running = false; shouldRun = false; ExitProcess(0);
            }
        }
        else if (m_iSelectedPage == 3) // Info
        {
            ImAdd::SeparatorText("Info");
            ImAdd::SeparatorText("Hardware");
            ImGui::Text("DMA:"); ImGui::SameLine(); ImGui::TextColored(ProcInfo::DmaInitialized ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1), "%s", ProcInfo::DmaInitialized ? "Connected" : "Disconnected");
            ImGui::Text("KMBOX:"); ImGui::SameLine(); ImGui::TextColored(ProcInfo::KmboxInitialized ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1), "%s", ProcInfo::KmboxInitialized ? "Connected" : "Disconnected");
            ImAdd::SeparatorText("Game");
            ImGui::Text("Client:"); ImGui::SameLine(); ImGui::Text("0x%llx", Globals::ClientBase);
            ImAdd::SeparatorText("Cheat");
            ImGui::Text("Overlay FPS: %.2f", OverlayFps);
            float buttonWidth = 100.0f; float buttonSpacing = 20.0f;
            ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 2 * buttonWidth - buttonSpacing) / 2);
            if (ImAdd::Button("Open folder", ImVec2(buttonWidth, 0))) { ShellExecuteA(nullptr, "open", "explorer.exe", ".\\", nullptr, SW_SHOW); }
            ImGui::SameLine();
            if (ImAdd::Button("Unload", ImVec2(buttonWidth, 0))) { Globals::Running = false; shouldRun = false; ExitProcess(0); }
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
    m_iSelectedPage = 0;

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
