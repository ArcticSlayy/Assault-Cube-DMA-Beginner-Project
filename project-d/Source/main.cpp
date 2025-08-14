#include <Pch.hpp>

#include <Features.hpp>
#include <Overlay.hpp>
#include <ESP/ESP.hpp>

/*
main.cpp
- Bootstraps config, logging, KMBOX (optional), DMA + SDK, features, background entity update thread, and overlay.
- Keeps initialization linear and early-outs on failure with logs.
- Main loop strictly handles overlay rendering; heavy work is offloaded to UpdateEntities() thread.
*/

int main()
{
    SetConsoleTitleA("Console - Debug");
    spdlog::set_level(spdlog::level::trace);

    std::cout << R"(
     _______ _______ _______ _______ _______ _______ 
    |\     /|\     /|\     /|\     /|\     /|\     /|
    | +---+ | +---+ | +---+ | +---+ | +---+ | +---+ |
    | |   | | |   | | |   | | |   | | |   | | |   | |
    | |A  | | |W  | | |H  | | |A  | | |R  | | |E  | |
    | +---+ | +---+ | +---+ | +---+ | +---+ | +---+ |
    |/_____\|/_____\|/_____\|/_____\|/_____\|/_____\|
)" << '\n';

    // Exception handler must be first to catch early issues
    if (!c_exception_handler::setup())
    {
        LOG_ERROR("Failed to setup Exception Handler");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }

    // Config before any feature uses it
    if (!config.Init())
    {
        LOG_ERROR("Failed to initialize Config");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }

    // Optional: KMBOX hardware
    if (config.Kmbox.Enabled)
    {
        if (Kmbox.InitDevice(config.Kmbox.Ip, config.Kmbox.Port, config.Kmbox.Uuid) == 0)
        {
            ProcInfo::KmboxInitialized = true;

            // Demo: move mouse left/up by 100 pixels (sanity test)
            int moveRc = Kmbox.Mouse.MoveRelative(-100, -100);
            if (moveRc == 0) LOG_INFO("KMBox: moved mouse by (-100, -100)");
            else LOG_ERROR("KMBox: MoveRelative failed: {}", moveRc);

            // Start KMBOX input monitor on a local UDP port
            constexpr WORD kMonitorPort = 23333;
            int monRc = Kmbox.KeyBoard.StartMonitor(kMonitorPort);
            if (monRc == 0) LOG_INFO("KMBox monitor started on UDP port {}", kMonitorPort);
            else LOG_ERROR("KMBox monitor start failed: {}", monRc);

            // Background watcher: log when right mouse button is pressed (non-blocking)
            static std::thread s_kmboxMouseWatch([&]() {
                bool lastRight = false;
                while (Globals::Running)
                {
                    // Right button bit is 0x02 (consistent with send path)
                    bool curRight = (Kmbox.KeyBoard.hw_Mouse.buttons & 0x02) != 0;
                    if (curRight && !lastRight)
                        LOG_INFO("KMBox: Right mouse button pressed");
                    lastRight = curRight;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
            s_kmboxMouseWatch.detach();
        }
        else
        {
            LOG_ERROR("Failed to initialize KMBOX");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            return 1;
        }
    }
    else
    {
        ProcInfo::KmboxInitialized = false;
    }

    // DMA, SDK, features bring-up
    if (!dma.Init())
    {
        LOG_ERROR("Failed to initialize DMA");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }

    if (!sdk.Init())
    {
        LOG_ERROR("Failed to initialize SDK");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }

	if (!features.Init())
    {
		LOG_ERROR("Failed to initialize Features");
		std::this_thread::sleep_for(std::chrono::seconds(5));
		return 1;
	}

    // Start entity update thread for ESP (lock-free render path)
    EntityManager::StartEntityUpdateThread();

    // UI overlay last for a responsive window
    if (!overlay.Create())
    {
		LOG_ERROR("Failed to create Overlay");
		std::this_thread::sleep_for(std::chrono::seconds(5));
		return 1;
	}

    LOG_INFO("Initialization complete! Press INSERT to open the menu");

    // Main render loop (tight, no heavy work here)
    while (overlay.shouldRun)
    {
        TIMER("Global render");

        overlay.StartRender();
         
        if (overlay.shouldRenderMenu)
            overlay.RenderMenu();

        ImDrawList* drawList = overlay.GetBackgroundDrawList();
        if (!drawList)
            continue;

        // Cheap per-frame update and draw of ESP
        esp.Update(drawList);

        overlay.EndRender();
    }

	overlay.Destroy();

    system("pause");
    return 0;
}