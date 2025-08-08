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
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

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

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    auto beforePresent = std::chrono::steady_clock::now();
    swap_chain->Present(config.Visuals.VSync ? 1U : 0U, 0U);
    auto afterPresent = std::chrono::steady_clock::now();

    // Ensure overlay stays topmost every frame to prevent flicker
    SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

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
    ImVec4 blueAccent = ImVec4(0.22f, 0.40f, 0.80f, 1.00f);
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
    ImGuiStyle& style = ImGui::GetStyle();
    ImGuiIO& io = ImGui::GetIO();
    StyleMenu(io, style);
    float OverlayFps = ImGui::GetIO().Framerate;

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
    ImU32 titleLeft = ImGui::ColorConvertFloat4ToU32(ImVec4(0.13f, 0.15f, 0.18f, 1.0f));
    ImU32 titleRight = ImGui::ColorConvertFloat4ToU32(ImVec4(0.09f, 0.10f, 0.12f, 1.0f));
    // Fill full width, no rounding
    ImGui::GetWindowDrawList()->AddRectFilledMultiColor(winPos, winPos + ImVec2(winSize.x, titleBarHeight), titleLeft, titleRight, titleRight, titleLeft);
    // Overlay rounded corners only at top
    ImGui::GetWindowDrawList()->AddRect(winPos, winPos + ImVec2(winSize.x, titleBarHeight), titleLeft, rounding, ImDrawFlags_RoundCornersTop, 2.0f);
    // Subtle shadow under title bar
    ImGui::GetWindowDrawList()->AddRectFilled(winPos + ImVec2(0, titleBarHeight - 2), winPos + ImVec2(winSize.x, titleBarHeight + 8), ImGui::ColorConvertFloat4ToU32(ImVec4(0,0,0,0.18f)));
    // Center icon and text horizontally and vertically
    const char* titleText = "Aetherial";
    ImVec2 textDim = ImGui::CalcTextSize(titleText);
    float totalWidth = iconSize + 18.0f + textDim.x;
    float centerX = (winSize.x - totalWidth) * 0.5f;
    float centerY = paddingY + (titleBarHeight - 2 * paddingY - ImMax(iconSize, textSize)) / 2.0f;
    float iconY = centerY + (ImMax(iconSize, textSize) - iconSize) / 2.0f;
    float textY = centerY + (ImMax(iconSize, textSize) - textSize) / 2.0f;
    float startX = centerX;
    ImGui::SetCursorPos(ImVec2(startX, iconY));
    ImGui::PushFont(iconFont);
    ImGui::Text(ICON_FA_BOOK);
    ImGui::PopFont();
    ImGui::SameLine(0, 18.0f);
    ImGui::SetCursorPos(ImVec2(startX + iconSize + 18.0f, textY));
    ImGui::PushFont(titleFont);
    ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "%s", titleText);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, titleBarHeight - ImMax(iconSize, textSize)));

    // --- Sidebar ---
    static const char* tabIcons[] = {
        ICON_FA_CROSSHAIRS, // Aim
        ICON_FA_EYE,        // Visuals
        ICON_FA_COG,        // Config
        ICON_FA_INFO_CIRCLE // Info
    };
    float footerHeight = 32.0f;
    float sidebarWidth = 220.0f; // Make sidebar wider for icons/text
    float sidebarHeight = winSize.y - titleBarHeight - footerHeight;
    ImGui::BeginChild("Sidebar", ImVec2(sidebarWidth, sidebarHeight), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImGui::SetScrollY(0); // Force scroll position to top
        ImGui::PushFont(iconFont);
        float tabSpacing = 4.0f;
        float tabHeight = ImMin((sidebarHeight - ((m_Tabs.size() - 1) * tabSpacing)) / m_Tabs.size(), 40.0f);
        float iconTextSpacing = 16.0f;
        float tabPadding = 22.0f;
        float tabWidth = sidebarWidth; // Tabs fill sidebar
        for (int i = 0; i < m_Tabs.size(); i++) {
            ImGui::PushID(i);
            bool selected = (m_iSelectedPage == i);
            ImVec2 itemSize(tabWidth, tabHeight);
            ImVec2 itemPos = ImGui::GetCursorScreenPos();
            if (selected) {
                // Gradient only for active tab, fit child width
                ImU32 tabLeft = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.40f, 0.80f, 0.45f));
                ImU32 tabRight = ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.40f, 0.80f, 0.18f));
                ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
                    itemPos,
                    itemPos + ImVec2(tabWidth, tabHeight),
                    tabLeft, tabRight, tabRight, tabLeft
                );
                ImGui::GetWindowDrawList()->AddRectFilled(
                    itemPos, itemPos + ImVec2(tabWidth, tabHeight),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.40f, 0.80f, 0.25f)), 8.0f
                );
                // Blue accent border for active tab
                ImGui::GetWindowDrawList()->AddRect(
                    itemPos, itemPos + ImVec2(tabWidth, tabHeight),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.40f, 0.80f, 0.85f)), 8.0f, 0, 2.0f
                );
            }
            float startX = itemPos.x + tabPadding;
            // Adjust iconY to align icon with text more closely
            float iconY = itemPos.y + (tabHeight - (iconFont ? iconFont->FontSize : 21.5f)) / 2 + 2.0f; // +2.0f for better vertical alignment
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
    ImGui::BeginChild("MainContent", ImVec2(0, sidebarHeight), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        float fGroupWidth = (ImGui::GetWindowWidth() - style.WindowPadding.x * 2 - style.ItemSpacing.x * 8) / 2.0f + 60.0f;
        ImGui::PushFont(featureFont);
        if (m_iSelectedPage == 0) // Aim
        {
            ImGui::Columns(2, nullptr, false);
            float yStart = ImGui::GetCursorPosY();
            // Left: Aimbot
            ImGui::SetCursorPosY(yStart);
            ImGui::BeginChild("AimbotSection", ImVec2(0, 340), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            if (ImGui::BeginMenuBar()) {
                ImGui::TextColored(ImVec4(0.85f, 0.86f, 0.88f, 1.0f), "Aimbot");
                ImGui::EndMenuBar();
            }
            {
                ToggleSwitch("Enable", &config.Aim.Aimbot);
                if (config.Aim.Aimbot)
                {
                    if (ProcInfo::KmboxInitialized)
                    {
                        ToggleSwitch("Draw FOV", &config.Aim.DrawFov);
                        if (config.Aim.DrawFov)
                        {
                            ImAdd::ColorEdit4("Fov Color", (float*)&config.Aim.AimbotFovColor);
                        }
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
            ImGui::SetCursorPosY(yStart); // Use the same yStart as left column
            ImGui::BeginChild("TriggerbotSection", ImVec2(0, 340), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            if (ImGui::BeginMenuBar()) {
                ImGui::TextColored(ImVec4(0.85f, 0.86f, 0.88f, 1.0f), "Triggerbot");
                ImGui::EndMenuBar();
            }
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
            ImGui::BeginChild("Visuals", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            if (ImGui::BeginMenuBar()) {
                ImGui::TextColored(ImVec4(0.85f, 0.86f, 0.88f, 1.0f), "Visuals");
                ImGui::EndMenuBar();
            }
            {
                ToggleSwitch("Enable", &config.Visuals.Enabled);
                if (config.Visuals.Enabled)
                {
                    ImAdd::SeparatorText("General");
                    ImGui::BeginGroup();
                    ToggleSwitch("Watermark", &config.Visuals.Watermark);
                    if (config.Visuals.Watermark)
                        ImAdd::ColorEdit4("Watermark Color", (float*)&config.Visuals.WatermarkColor);
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
            ImGui::EndChild();
        }
        else if (m_iSelectedPage == 2) // Config
        {
            ImGui::BeginChild("Configs", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            {
                if (ImGui::BeginMenuBar()) {
                    ImGui::Text("Configs");
                    ImGui::EndMenuBar();
                }

                static char configName[128] = "";
                static std::vector<std::string> configFiles;
                static int lastSelectedTab = -1;
                static bool menuWasOpen = false;
                // Refresh config list when menu is opened or tab is switched to Config
                if (shouldRenderMenu && !menuWasOpen) {
                    menuWasOpen = true;
                    configFiles = config.ListConfigs("configs/");
                }
                if (!shouldRenderMenu) {
                    menuWasOpen = false;
                }
                if (m_iSelectedPage == MenuPage_Config && lastSelectedTab != MenuPage_Config) {
                    configFiles = config.ListConfigs("configs/");
                }
                lastSelectedTab = m_iSelectedPage;

                if (ImAdd::Button("Refresh"))
                {
                    configFiles = config.ListConfigs("configs/");
                    LOG_INFO("Refreshed config list");
                }

                ImGui::Separator();

                // Config List
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.13f, 0.13f, 1.0f)); // lighter bg
ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.22f, 0.5f)); // less contrast
if (ImGui::BeginListBox("Config list"))
{
    if (configFiles.empty())
    {
        ImGui::Selectable("No configs found", false, ImGuiSelectableFlags_Disabled);
    }
    else
    {
        for (const auto& file : configFiles)
        {
            bool isSelected = (file == configName);
            if (ImGui::Selectable(file.c_str(), isSelected))
            {
                strcpy(configName, file.c_str());
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
    }
    ImGui::EndListBox();
}
ImGui::PopStyleColor(2);

                // Config Name Input
                ImGui::InputText("Config Name", configName, IM_ARRAYSIZE(configName));

                // Control Buttons
                float buttonWidth = 75.0f;
                float buttonSpacing = 10.0f;
                ImGui::Dummy(ImVec2(0.0f, 5.0f));

                if (ImAdd::Button("Load", ImVec2(buttonWidth, 0)))
                {
                    std::string filePath = "configs/" + std::string(configName);
                    if (!config.LoadFromFile(filePath))
                    {
                        LOG_ERROR("Failed to load config: {}", filePath);
                    }
                    else
                    {
                        LOG_INFO("Loaded config: {}", filePath);
                    }
                }

                ImGui::SameLine(0.0f, buttonSpacing);

                if (ImAdd::Button("Save", ImVec2(buttonWidth, 0)))
                {
                    std::string filePath = "configs/" + std::string(configName);
                    if (!config.SaveToFile(filePath))
                    {
                        LOG_ERROR("Failed to save config: {}", filePath);
                    }
                    else
                    {
                        LOG_INFO("Saved config: {}", filePath);
                    }
                }

                ImGui::SameLine(0.0f, buttonSpacing);

                if (ImAdd::Button("Delete", ImVec2(buttonWidth, 0)))
                {
                    std::string filePath = "configs/" + std::string(configName);
                    if (!config.DeleteConfigFile(filePath))
                    {
                        LOG_ERROR("Failed to delete config: {}", filePath);
                    }
                    else
                    {
                        LOG_INFO("Deleted config: {}", filePath);
                        configFiles = config.ListConfigs("configs/");
                    }
                }

                ImGui::SameLine(0.0f, buttonSpacing);

                if (ImAdd::Button("Import", ImVec2(buttonWidth, 0)))
                {
                    if (config.LoadFromClipboard())
                    {
                        LOG_INFO("Config imported from clipboard");
                    }
                    else
                    {
                        LOG_ERROR("Failed to import config from clipboard");
                    }
                }

                ImGui::SameLine(0.0f, buttonSpacing);

                if (ImAdd::Button("Unload", ImVec2(buttonWidth, 0)))
                {
                    Globals::Running = false;
                    shouldRun = false;
                    ExitProcess(0); // Immediate exit, let OS cleanup
                }
            }
            ImGui::EndChild();
        }
        else if (m_iSelectedPage == 3) // Info
        {
            ImGui::BeginChild("Info", ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_MenuBar);
            {
                if (ImGui::BeginMenuBar()) {
                    ImGui::Text("Info");
                    ImGui::EndMenuBar();
                }

                ImAdd::SeparatorText("Hardware");

                ImGui::Text("DMA:");
                ImGui::SameLine();
                ImGui::TextColored(ProcInfo::DmaInitialized ? ImVec4(0, 1, 0, 1)/* green */ : ImVec4(1, 0, 0, 1)/* red */, "%s", ProcInfo::DmaInitialized ? "Connected" : "Disconnected");

                ImGui::Text("KMBOX:");
                ImGui::SameLine();
                ImGui::TextColored(ProcInfo::KmboxInitialized ? ImVec4(0, 1, 0, 1)/* green */ : ImVec4(1, 0, 0, 1)/* red */, "%s", ProcInfo::KmboxInitialized ? "Connected" : "Disconnected");

                ImAdd::SeparatorText("Game");

                ImGui::Text("Client:");
                ImGui::SameLine();
                ImGui::Text("0x%llx", Globals::ClientBase);

                ImAdd::SeparatorText("Cheat");

                ImGui::Text("Overlay FPS: %.2f", OverlayFps);

                float buttonWidth = 100.0f;
                float buttonSpacing = 20.0f;
                ImGui::SetCursorPosX((ImGui::GetWindowSize().x - 2 * buttonWidth - buttonSpacing) / 2);

                if (ImAdd::Button("Open folder", ImVec2(buttonWidth, 0)))
                {
                    ShellExecuteA(nullptr, "open", "explorer.exe", ".\\", nullptr, SW_SHOW);
                }

                ImGui::SameLine();

                if (ImAdd::Button("Unload", ImVec2(buttonWidth, 0)))
                {
                    Globals::Running = false;
                    shouldRun = false;
                    ExitProcess(0); // Immediate exit, let OS cleanup
                }
            }
            ImGui::EndChild();
        }
        ImGui::PopFont(); // featureFont
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
