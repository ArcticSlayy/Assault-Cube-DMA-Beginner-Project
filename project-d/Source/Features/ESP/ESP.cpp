#include <Pch.hpp>
#include <SDK.hpp>
#include "ESP.hpp"
#include "Offsets.h"
#include "Features.hpp"
#include <algorithm>

// Helper: WorldToScreen using column-major float[16] (mythos style)
bool WorldToScreenMythos(const Vector3& pos, Vector2& out, const float* matrix, int width, int height)
{
    float x = pos.x, y = pos.y, z = pos.z;
    float w = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
    if (w < 0.001f) return false;
    float inv_w = 1.0f / w;
    float screen_x = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
    float screen_y = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
    screen_x = (width / 2.0f) + (screen_x / w) * (width / 2.0f);
    screen_y = (height / 2.0f) - (screen_y / w) * (height / 2.0f);
    out.x = screen_x;
    out.y = screen_y;
    return true;
}

void ESP::Render(ImDrawList* drawList)
{
    // Update the view matrix from memory before using it
    mem.Read(Globals::ClientBase + p_game->view_matrix, &Globals::ViewMatrix, sizeof(Globals::ViewMatrix));

    int playerCount = mem.Read<int>(Globals::ClientBase + p_game->player_count);
    uint32_t dwLocalPlayer = mem.Read<uint32_t>(Globals::ClientBase + p_game->local_player);

    EntityManager::entities.clear();

    uint32_t entityListAddr = mem.Read<uint32_t>(Globals::ClientBase + p_game->entity_list);

    auto scatterHandle = mem.CreateScatterHandle();
    if (!scatterHandle)
    {
        return;
    }

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

        entityData.headPosition.x = mem.Read<float>(dwEntity + p_entity->v3_head_pos);
        entityData.headPosition.y = mem.Read<float>(dwEntity + p_entity->v3_head_pos + 0x4);
        entityData.headPosition.z = mem.Read<float>(dwEntity + p_entity->v3_head_pos + 0x8);

        entityData.footPosition.x = mem.Read<float>(dwEntity + p_entity->v3_foot_pos);
        entityData.footPosition.y = mem.Read<float>(dwEntity + p_entity->v3_foot_pos + 0x4);
        entityData.footPosition.z = mem.Read<float>(dwEntity + p_entity->v3_foot_pos + 0x8);

        char name[260];
        if (!mem.Read(dwEntity + p_entity->str_name, name, sizeof(name)))
        {
            continue;
        }
        name[259] = '\0'; // Ensure null-termination
        entityData.name = std::string(name);

        EntityManager::entities.push_back(entityData);
    }

    mem.ExecuteReadScatter(scatterHandle);
    mem.CloseScatterHandle(scatterHandle);

    int width = (int)Screen.x;
    int height = (int)Screen.y;

    for (const auto& entity : EntityManager::entities)
    {
        Vector2 headScreenPos, footScreenPos;
        if (WorldToScreenMythos(entity.headPosition, headScreenPos, (float*)&Globals::ViewMatrix, width, height) &&
            WorldToScreenMythos(entity.footPosition, footScreenPos, (float*)&Globals::ViewMatrix, width, height))
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