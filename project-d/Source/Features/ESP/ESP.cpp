#include <Pch.hpp>
#include <SDK.hpp>
#include "ESP.hpp"
#include "Offsets.h"
#include "Features.hpp"
void ESP::Render(ImDrawList* drawList)
{
    int playerCount = mem.Read<int>(Globals::ClientBase + p_game->player_count);
    uint32_t dwLocalPlayer = mem.Read<uint32_t>(Globals::ClientBase + p_game->local_player);

    EntityManager::entities.clear();

    for (auto i = 1; i < playerCount; i++)
    {
        uint32_t entityListAddr = mem.Read<uint32_t>(Globals::ClientBase + p_game->entity_list);
        uint32_t dwEntity = mem.Read<uint32_t>(entityListAddr + (i * 0x4));
        if (!dwEntity)
            continue;

        bool bIsDead = mem.Read<bool>(dwEntity + p_entity->i_dead);
        if (bIsDead)
            continue;

        EntityData entityData;
        auto scatterHandle = mem.CreateScatterHandle();

        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_health, &entityData.health);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_team, &entityData.team);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_score, &entityData.score);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_kills, &entityData.kills);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_deaths, &entityData.deaths);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->v3_head_pos, &entityData.headPosition);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->v3_foot_pos, &entityData.footPosition);

        char name[260];
        mem.Read(dwEntity + p_entity->str_name, name, sizeof(name)); // Direct memory read
        name[259] = '\0'; // Ensure null-termination
        entityData.name = std::string(name); // Assign the content of the buffer to the std::string

        mem.ExecuteReadScatter(scatterHandle);
        mem.CloseScatterHandle(scatterHandle);

        EntityManager::entities.push_back(entityData);
    }
}