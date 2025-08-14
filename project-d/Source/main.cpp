#include <Pch.hpp>

#include <Features.hpp>
#include <Overlay.hpp>
#include <ESP/ESP.hpp>

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

    if (!c_exception_handler::setup())
    {
        LOG_ERROR("Failed to setup Exception Handler");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }

    if (!config.Init())
    {
        LOG_ERROR("Failed to initialize Config");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 1;
    }

    if (config.Kmbox.Enabled)
    {
        if (Kmbox.InitDevice(config.Kmbox.Ip, config.Kmbox.Port, config.Kmbox.Uuid) == 0)
        {
            ProcInfo::KmboxInitialized = true;
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

    // Start entity update thread for ESP
    EntityManager::StartEntityUpdateThread();

    if (!overlay.Create())
    {
		LOG_ERROR("Failed to create Overlay");
		std::this_thread::sleep_for(std::chrono::seconds(5));
		return 1;
	}

    LOG_INFO("Initialization complete! Press INSERT to open the menu");

    while (overlay.shouldRun)
    {
        TIMER("Global render");

        overlay.StartRender();
         
        if (overlay.shouldRenderMenu)
            overlay.RenderMenu();

        ImDrawList* drawList = overlay.GetBackgroundDrawList();
        if (!drawList)
            continue;

        esp.Update(drawList);

        overlay.EndRender();
    }

	overlay.Destroy();

    system("pause");
    return 0;
}