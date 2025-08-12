#include "Pch.hpp"
#include "DMA.hpp"
#include <SDK/Offsets.h>

bool DMA::Init()
{
    // Configure optimal DMA settings for memory read heavy operations
    VMMDLL_ConfigSet(mem.vHandle, VMMDLL_OPT_CONFIG_STATISTICS_FUNCTIONCALL, 1);
    
    // Use more aggressive caching for better performance
    // Set memory cache to be slightly longer lived
    VMMDLL_ConfigSet(mem.vHandle, VMMDLL_OPT_CONFIG_READCACHE_TICKS, 4);
    
    // Set the TLB cache to be slightly longer lived
    VMMDLL_ConfigSet(mem.vHandle, VMMDLL_OPT_CONFIG_TLBCACHE_TICKS, 3);

    if (!mem.Init(GAME_NAME))
    {
        LOG_ERROR("Failed to initialize DMA");
        return 1;
    }

    Globals::ClientBase = mem.GetBaseDaddy(CLIENT_DLL);
    if (!Globals::ClientBase || Globals::ClientBase == NULL)
    {
        LOG_ERROR("Failed to get ClientBase");
        return false;
    }

    if (!mem.GetKeyboard()->InitKeyboard())
    {
        LOG_ERROR("Failed to initialize DMA Keyboard");
        return 1;
    }

    if (!mem.FixCr3())
    {
        LOG_ERROR("Failed to fix CR3");
		return false;
    }

    // Prefetch common memory regions is handled internally
    // The offsets are handled automatically
    
    ProcInfo::DmaInitialized = true;
    return true;
}
