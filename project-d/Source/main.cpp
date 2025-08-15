#include <Pch.hpp>

#include <Features.hpp>
#include <Overlay.hpp>
#include <ESP/ESP.hpp>
#include <thread>
#include <chrono>

/*
main.cpp
- Bootstraps config, logging, KMBOX (optional), DMA + SDK, features, background entity update thread, and overlay.
- Keeps initialization linear and early-outs on failure with logs.
- Main loop strictly handles overlay rendering; heavy work is offloaded to UpdateEntities() thread.
*/

static std::thread g_kmboxMouseWatch; // owned watcher

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
    |/_____\|/_____\|/_____\|/_____/|/_____/|/_____/|
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

    // Enter running state early so background workers can start
    Globals::Running = true;

    // Optional: KMBOX hardware
    if (config.Kmbox.Enabled)
    {
        int initRc = Kmbox.InitDevice(config.Kmbox.Ip, config.Kmbox.Port, config.Kmbox.Uuid);
        if (initRc == 0)
        {
            ProcInfo::KmboxInitialized = true;

            // Start KMBOX input monitor on a local UDP port (from config; fallback default)
            WORD monitorPort = config.Kmbox.Port ? config.Kmbox.Port : 23333;
            int monRc = Kmbox.KeyBoard.StartMonitor(monitorPort);
            if (monRc == 0) {
                // Background watcher: print when right mouse button is pressed (edge-triggered)
                g_kmboxMouseWatch = std::thread([&]() {
                    bool lastRight = false;
                    while (Globals::Running)
                    {
                        int right = Kmbox.KeyBoard.MonitorMouseRight(); // -1 = not running, 0 = up, 1 = down
                        if (right >= 0) {
                            bool curRight = (right == 1);
                            if (curRight && !lastRight) {
                                std::cout << "[KMBox] Right mouse button pressed" << std::endl;
                            }
                            lastRight = curRight;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                });
            } else {
                LOG_ERROR("KMBox monitor start failed: {}", monRc);
            }
        }
        else
        {
            LOG_ERROR("Failed to initialize KMBOX: {}", initRc);
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

    // Graceful shutdown
    Globals::Running = false;
    if (ProcInfo::KmboxInitialized)
        Kmbox.KeyBoard.EndMonitor();

    if (g_kmboxMouseWatch.joinable()) g_kmboxMouseWatch.join();

    sdk.Shutdown();

	overlay.Destroy();

#ifndef _DEBUG
    // Proper shutdown path: no console pause in release
    return 0;
#else
    system("pause");
    return 0;
#endif
}