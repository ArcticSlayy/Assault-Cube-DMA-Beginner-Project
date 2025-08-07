#include <Pch.hpp>
#include <SDK.hpp>
#include "ESP.hpp"
#include "Offsets.h"
#include "Features.hpp"
#include <algorithm>

void ESP::Render(ImDrawList* drawList)
{
    constexpr int MAX_PLAYERS = 32;
    // --- First scatter: read game globals ---
    auto scatterGlobals = mem.CreateScatterHandle();
    if (!scatterGlobals)
        return;

    int playerCount = 0;
    uint32_t dwLocalPlayer = 0;
    uint32_t entityListAddr = 0;
    int localPlayerTeam = 0;
    mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->player_count, &playerCount, sizeof(playerCount));
    mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->local_player, &dwLocalPlayer, sizeof(dwLocalPlayer));
    mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->entity_list, &entityListAddr, sizeof(entityListAddr));
    mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->view_matrix, &Globals::ViewMatrix, sizeof(Globals::ViewMatrix));
    mem.AddScatterReadRequest(scatterGlobals, dwLocalPlayer + p_entity->i_team, &localPlayerTeam, sizeof(localPlayerTeam));
    mem.ExecuteReadScatter(scatterGlobals);
    mem.CloseScatterHandle(scatterGlobals);

    // --- Second scatter: read entity addresses ---
    auto scatterEntities = mem.CreateScatterHandle();
    if (!scatterEntities)
        return;

    uint32_t entityAddrs[MAX_PLAYERS] = {};
    for (int i = 1; i < MAX_PLAYERS; i++)
    {
        mem.AddScatterReadRequest(scatterEntities, entityListAddr + (i * 0x4), &entityAddrs[i], sizeof(entityAddrs[i]));
    }
    mem.ExecuteReadScatter(scatterEntities);
    mem.CloseScatterHandle(scatterEntities);

    // --- Third scatter: read entity data for valid addresses ---
    auto scatterData = mem.CreateScatterHandle();
    if (!scatterData)
        return;

    bool entityDead[MAX_PLAYERS] = {};
    int entityHealth[MAX_PLAYERS] = {};
    int entityTeam[MAX_PLAYERS] = {};
    int entityScore[MAX_PLAYERS] = {};
    int entityKills[MAX_PLAYERS] = {};
    int entityDeaths[MAX_PLAYERS] = {};
    Vector3 entityHeadPos[MAX_PLAYERS] = {};
    Vector3 entityFootPos[MAX_PLAYERS] = {};
    char entityNames[MAX_PLAYERS][260] = {};
    int entityWeaponId[MAX_PLAYERS] = {};

    for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++)
    {
        if (!entityAddrs[i])
            continue;
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_dead, &entityDead[i], sizeof(entityDead[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_health, &entityHealth[i], sizeof(entityHealth[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_team, &entityTeam[i], sizeof(entityTeam[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_score, &entityScore[i], sizeof(entityScore[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_kills, &entityKills[i], sizeof(entityKills[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_deaths, &entityDeaths[i], sizeof(entityDeaths[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->v3_head_pos, &entityHeadPos[i], sizeof(entityHeadPos[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->v3_foot_pos, &entityFootPos[i], sizeof(entityFootPos[i]));
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->str_name, entityNames[i], sizeof(entityNames[i]));
        // Mythos-style: weapon id via pointer chain
        mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->weapon_class + p_weapon->i_id, &entityWeaponId[i], sizeof(entityWeaponId[i]));
    }
    mem.ExecuteReadScatter(scatterData);
    mem.CloseScatterHandle(scatterData);

    EntityManager::entities.clear();
    for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++)
    {
        if (!entityAddrs[i] || entityDead[i])
            continue;
        entityNames[i][259] = '\0';
        EntityData entityData;
        entityData.name = std::string(entityNames[i]);
        entityData.health = entityHealth[i];
        entityData.team = entityTeam[i];
        entityData.score = entityScore[i];
        entityData.kills = entityKills[i];
        entityData.deaths = entityDeaths[i];
        entityData.headPosition = entityHeadPos[i];
        entityData.footPosition = entityFootPos[i];
        entityData.weaponClass = entityWeaponId[i];
        // Fallback to arr_weapon_names
        if (entityWeaponId[i] >= 0 && entityWeaponId[i] < offsets->arr_weapon_names.size())
            entityData.weaponName = offsets->arr_weapon_names[entityWeaponId[i]];
        else
            entityData.weaponName = "Unknown";
        EntityManager::entities.push_back(entityData);
    }

    int width = (int)Screen.x;
    int height = (int)Screen.y;
    for (const auto& entity : EntityManager::entities)
    {
        if (config.Visuals.TeamCheck && entity.team == localPlayerTeam)
            continue;
        Vector2 headScreenPos, footScreenPos;
        if (sdk.WorldToScreen(entity.headPosition, headScreenPos, Globals::ViewMatrix, width, height) &&
            sdk.WorldToScreen(entity.footPosition, footScreenPos, Globals::ViewMatrix, width, height))
        {
            float box_height = std::max(footScreenPos.y - headScreenPos.y, 1.0f);
            float box_width = box_height / 2.0f;
            float box_x = headScreenPos.x - (box_width / 2.0f);
            float box_y = headScreenPos.y;
            if (config.Visuals.Box) {
                ImU32 boxColor = IM_COL32(
                    (int)(config.Visuals.BoxColor.x * 255),
                    (int)(config.Visuals.BoxColor.y * 255),
                    (int)(config.Visuals.BoxColor.z * 255),
                    (int)(config.Visuals.BoxColor.w * 255)
                );
                drawList->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_width, box_y + box_height), boxColor, 0.0f, 0, 2.0f);
            }
            float textY = headScreenPos.y;
            if (config.Visuals.Name) {
                ImU32 nameColor = IM_COL32(
                    (int)(config.Visuals.NameColor.x * 255),
                    (int)(config.Visuals.NameColor.y * 255),
                    (int)(config.Visuals.NameColor.z * 255),
                    (int)(config.Visuals.NameColor.w * 255)
                );
                ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
                ImVec2 namePos(headScreenPos.x - textSize.x / 2.0f, textY - textSize.y - 2.0f);
                drawList->AddText(namePos, nameColor, entity.name.c_str());
                textY = namePos.y + textSize.y + 2.0f;
            }
            if (config.Visuals.Weapon && !entity.weaponName.empty()) {
                ImU32 weaponColor = IM_COL32(
                    (int)(config.Visuals.WeaponColor.x * 255),
                    (int)(config.Visuals.WeaponColor.y * 255),
                    (int)(config.Visuals.WeaponColor.z * 255),
                    (int)(config.Visuals.WeaponColor.w * 255)
                );
                ImVec2 weaponTextSize = ImGui::CalcTextSize(entity.weaponName.c_str());
                ImVec2 weaponPos(footScreenPos.x - weaponTextSize.x / 2.0f, footScreenPos.y + 5.0f);
                drawList->AddText(weaponPos, weaponColor, entity.weaponName.c_str());
            }
        }
    }
}