#pragma once
#include <vector>
#include <mutex>
#include "Features.hpp"

// Do NOT redefine EntityData here, only use the one from Features.hpp
// Only declare extern variables here, do NOT use inline or define them here

namespace EntityManager {
    extern std::vector<EntityData> entities;
    extern std::mutex entities_mutex;
    void StartEntityUpdateThread();
}

class ESP
{
private:
    void Render(ImDrawList* drawList);

public:
    void Update(ImDrawList* drawList)
    {
        TIMER("ESP render");
        Render(drawList);
    }
    static ESP& Get()
    {
        static ESP instance;
        return instance;
    }
};

inline ESP& esp = ESP::Get();
