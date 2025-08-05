#include <Pch.hpp>
#include <SDK.hpp>
#include "ESP.hpp"
#include "Offsets.h"
#include "Features.hpp"
#include <algorithm>

void ESP::Render(ImDrawList* drawList)
{
    // Use scatter read for view matrix and all entity data
    auto scatterHandle = mem.CreateScatterHandle();
    if (!scatterHandle)
    {
        return;
    }

    // Queue view matrix read
    mem.AddScatterReadRequest(scatterHandle, Globals::ClientBase + p_game->view_matrix, &Globals::ViewMatrix, sizeof(Globals::ViewMatrix));

    int playerCount = mem.Read<int>(Globals::ClientBase + p_game->player_count);
    uint32_t dwLocalPlayer = mem.Read<uint32_t>(Globals::ClientBase + p_game->local_player);

    EntityManager::entities.clear();

    uint32_t entityListAddr = mem.Read<uint32_t>(Globals::ClientBase + p_game->entity_list);

    std::vector<std::pair<uint32_t, EntityData>> entityRequests;
    for (auto i = 1; i < playerCount; i++)
    {
        uint32_t dwEntity = mem.Read<uint32_t>(entityListAddr + (i * 0x4));
        if (!dwEntity)
            continue;

        bool bIsDead = mem.Read<bool>(dwEntity + p_entity->i_dead);
        if (bIsDead)
            continue;

        EntityData entityData;
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_health, &entityData.health);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_team, &entityData.team);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_score, &entityData.score);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_kills, &entityData.kills);
        mem.AddScatterReadRequest(scatterHandle, dwEntity + p_entity->i_deaths, &entityData.deaths);
        // Read head and foot positions as Vector3 structs in one go
        entityData.headPosition = mem.Read<Vector3>(dwEntity + p_entity->v3_head_pos);
        entityData.footPosition = mem.Read<Vector3>(dwEntity + p_entity->v3_foot_pos);
        char name[260];
        if (!mem.Read(dwEntity + p_entity->str_name, name, sizeof(name)))
        {
            continue;
        }
        name[259] = '\0';
        entityData.name = std::string(name);
        entityRequests.emplace_back(dwEntity, entityData);
    }

    // Execute all scatter reads (including view matrix and entity stats)
    mem.ExecuteReadScatter(scatterHandle);
    mem.CloseScatterHandle(scatterHandle);

    // Now push the entities to the manager
    for (auto& [dwEntity, entityData] : entityRequests)
    {
        EntityManager::entities.push_back(entityData);
    }

    int width = (int)Screen.x;
    int height = (int)Screen.y;

    for (const auto& entity : EntityManager::entities)
    {
        Vector2 headScreenPos, footScreenPos;
        if (sdk.WorldToScreen(entity.headPosition, headScreenPos, Globals::ViewMatrix, width, height) &&
            sdk.WorldToScreen(entity.footPosition, footScreenPos, Globals::ViewMatrix, width, height))
        {
            float box_height = std::max(footScreenPos.y - headScreenPos.y, 1.0f);
            float box_width = box_height / 2.0f;
            float box_x = headScreenPos.x - (box_width / 2.0f);
            float box_y = headScreenPos.y;

            // Draw box
            drawList->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_width, box_y + box_height), IM_COL32(255, 0, 0, 255), 0.0f, 0, 2.0f);

            // Draw centered name above head
            ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
            ImVec2 namePos(headScreenPos.x - textSize.x / 2.0f, headScreenPos.y - textSize.y - 2.0f);
            drawList->AddText(namePos, IM_COL32(255, 255, 255, 255), entity.name.c_str());
        }
    }
}