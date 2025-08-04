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

    uint32_t entityListAddr = mem.Read<uint32_t>(Globals::ClientBase + p_game->entity_list);
    LOG_INFO("Entity List Address: 0x{}", entityListAddr);

    auto scatterHandle = mem.CreateScatterHandle();
    if (!scatterHandle)
    {
        LOG_ERROR("Failed to create scatter handle");
        return;
    }

    for (auto i = 1; i < playerCount; i++)
    {
        uint32_t dwEntity = mem.Read<uint32_t>(entityListAddr + (i * 0x4));
        //LOG_INFO("Entity Address: 0x{}", dwEntity);
        if (!dwEntity)
            continue;

        bool bIsDead = mem.Read<bool>(dwEntity + p_entity->i_dead);
        //LOG_INFO("Entity Dead Status: {}", bIsDead);
        if (bIsDead)
            continue;

        EntityData entityData;

        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_health, &entityData.health);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_team, &entityData.team);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_score, &entityData.score);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_kills, &entityData.kills);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_deaths, &entityData.deaths);

        entityData.headPosition.x = mem.Read<float>(dwEntity + p_entity->v3_head_pos);
        entityData.headPosition.y = mem.Read<float>(dwEntity + p_entity->v3_head_pos + 0x4);
        entityData.headPosition.z = mem.Read<float>(dwEntity + p_entity->v3_head_pos + 0x8);

        entityData.footPosition.x = mem.Read<float>(dwEntity + p_entity->v3_foot_pos);
        entityData.footPosition.y = mem.Read<float>(dwEntity + p_entity->v3_foot_pos + 0x4);
        entityData.footPosition.z = mem.Read<float>(dwEntity + p_entity->v3_foot_pos + 0x8);

        char name[260];
        if (!mem.Read(dwEntity + p_entity->str_name, name, sizeof(name)))
        {
            LOG_ERROR("Failed to read entity name");
            continue;
        }
        name[259] = '\0'; // Ensure null-termination
        entityData.name = std::string(name); // Assign the content of the buffer to the std::string

        EntityManager::entities.push_back(entityData);
    }

    //LOG_INFO("Executing scatter read...");
    mem.ExecuteReadScatter(scatterHandle);
    mem.CloseScatterHandle(scatterHandle);

    //LOG_INFO("Entity count: {}", EntityManager::entities.size());

    for (const auto& entity : EntityManager::entities)
    {
        LOG_INFO("Processing entity: {}", entity.name.c_str());

        // Render only visible entities
        Vector2 headScreenPos, footScreenPos;
        if (sdk.WorldToScreen(entity.headPosition, headScreenPos) && sdk.WorldToScreen(entity.footPosition, footScreenPos))
        {
            ImVec2 headImVec2(headScreenPos.x, headScreenPos.y);
            ImVec2 footImVec2(footScreenPos.x, footScreenPos.y);

            drawList->AddLine(headImVec2, footImVec2, IM_COL32(255, 0, 0, 255));
            drawList->AddText(headImVec2, IM_COL32(255, 255, 255, 255), entity.name.c_str());
        }

        LOG_INFO("Head Position: x={}, y={}, z={}", entity.headPosition.x, entity.headPosition.y, entity.headPosition.z);
        LOG_INFO("Foot Position: x={}, y={}, z={}", entity.footPosition.x, entity.footPosition.y, entity.footPosition.z);
        LOG_INFO("View Matrix: [{} {} {} {}] [{} {} {} {}] [{} {} {} {}] [{} {} {} {}]",
                 Globals::ViewMatrix[0][0], Globals::ViewMatrix[0][1], Globals::ViewMatrix[0][2], Globals::ViewMatrix[0][3],
                 Globals::ViewMatrix[1][0], Globals::ViewMatrix[1][1], Globals::ViewMatrix[1][2], Globals::ViewMatrix[1][3],
                 Globals::ViewMatrix[2][0], Globals::ViewMatrix[2][1], Globals::ViewMatrix[2][2], Globals::ViewMatrix[2][3],
                 Globals::ViewMatrix[3][0], Globals::ViewMatrix[3][1], Globals::ViewMatrix[3][2], Globals::ViewMatrix[3][3]);
    }

    LOG_INFO("Reading head position from offset: 0x{}", p_entity->v3_head_pos);
    LOG_INFO("Reading foot position from offset: 0x{}", p_entity->v3_foot_pos);
    LOG_INFO("Reading view matrix from Globals::ViewMatrix");
}