#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/addons/imgui_addons.h"

#include "Render/Renderer.hpp"

enum eMenuPage_ : int
{
	MenuPage_Aim,
	MenuPage_Visuals,
	MenuPage_Entities,
	MenuPage_Config,
	MenuPage_Info,
	MenuPage_COUNT
};

class Overlay {
public:
	Renderer render;

	static bool CreateDevice();
	static void DestroyDevice();

	static bool CreateOverlay();
	static void DestroyOverlay();

	static bool CreateImGui();
	static void DestroyImGui();

	static void StartRender();
	static void EndRender();

	static void StyleMenu(ImGuiIO& IO, ImGuiStyle& style);

	static void RenderMenu();

	static bool Create();
	static void Destroy();

	static HWND overlay;
	static WNDCLASSEX wc;

	static bool IsWindowInForeground(HWND window) { return GetForegroundWindow() == window; }
	static bool BringToForeground(HWND window) { return SetForegroundWindow(window); }

	static void SetForeground(HWND window);

	static ImDrawList* GetBackgroundDrawList() { return ImGui::GetBackgroundDrawList(); }

	// Use ComPtr to manage lifetimes automatically
	static Microsoft::WRL::ComPtr<ID3D11Device> device;
	static Microsoft::WRL::ComPtr<ID3D11DeviceContext> device_context;
	static Microsoft::WRL::ComPtr<IDXGISwapChain> swap_chain;
	static Microsoft::WRL::ComPtr<ID3D11RenderTargetView> render_targetview;

	inline static bool shouldRenderMenu;

	inline static bool shouldRun;

	// Helpers for RT lifecycle (public for WM_SIZE handler)
	static void CreateRenderTarget();
	static void CleanupRenderTarget();

private:
	inline static int m_iSelectedPage;
	inline static std::vector<const char*> m_Tabs;

	inline static float OverlayFps;
};

inline Overlay overlay;