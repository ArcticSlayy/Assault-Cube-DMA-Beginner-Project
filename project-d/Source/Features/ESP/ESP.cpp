//#define ESP_LOGGING_ENABLED
#include <Pch.hpp>
#include <SDK.hpp>
#include "ESP.hpp"
#include "Offsets.h"
#include "Features.hpp"
#include <algorithm>
#include <thread>
#include <chrono>
#include <set>
#include <spdlog/spdlog.h>

namespace EntityManager {
    // Double buffer for entities
    std::vector<EntityData> entitiesA;
    std::vector<EntityData> entitiesB;
    std::vector<EntityData>* renderEntities = &entitiesA;
    std::vector<EntityData>* updateEntities = &entitiesB;
    std::mutex entities_mutex;

    void UpdateEntities() {
        constexpr int MAX_PLAYERS = 32;
        Matrix newViewMatrix; // Temporary view matrix
        int dmaErrorCount = 0;
        auto lastLogTime = std::chrono::steady_clock::now();
        static auto lastUpdate = std::chrono::steady_clock::now();
        while (Globals::Running) {
            auto now = std::chrono::steady_clock::now();
            // Throttle to 60Hz
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() < 16) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            lastUpdate = now;
            auto start = std::chrono::steady_clock::now();
            // Always update view matrix, even if entity reads fail
            auto scatterGlobals = mem.CreateScatterHandle();
            int playerCount = 0;
            uint32_t dwLocalPlayer = 0;
            uint32_t entityListAddr = 0;
            int localPlayerTeam = 0;
            bool globalsReadOk = false;
            if (scatterGlobals) {
                mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->player_count, &playerCount, sizeof(playerCount));
                mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->local_player, &dwLocalPlayer, sizeof(dwLocalPlayer));
                mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->entity_list, &entityListAddr, sizeof(entityListAddr));
                mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->view_matrix, &newViewMatrix, sizeof(newViewMatrix));
                mem.AddScatterReadRequest(scatterGlobals, dwLocalPlayer + p_entity->i_team, &localPlayerTeam, sizeof(localPlayerTeam));
                mem.ExecuteReadScatter(scatterGlobals);
                mem.CloseScatterHandle(scatterGlobals);
                globalsReadOk = true;
            } else {
                dmaErrorCount++;
            }
            // Update view matrix every frame
            Globals::ViewMatrix = newViewMatrix;

            bool allReadsSuccessful = globalsReadOk;
            std::vector<uint32_t> entityAddrs(MAX_PLAYERS, 0);
            auto scatterEntities = mem.CreateScatterHandle();
            auto t_addr_start = std::chrono::steady_clock::now();
            if (!scatterEntities) {
                allReadsSuccessful = false;
                dmaErrorCount++;
            }
            if (allReadsSuccessful) {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    mem.AddScatterReadRequest(scatterEntities, entityListAddr + (i * 0x4), &entityAddrs[i], sizeof(entityAddrs[i]));
                }
                mem.ExecuteReadScatter(scatterEntities);
                mem.CloseScatterHandle(scatterEntities);
            }
            auto t_addr_end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
            spdlog::info("ScatterRead: Entity addresses took {} us", std::chrono::duration_cast<std::chrono::microseconds>(t_addr_end - t_addr_start).count());
#endif

            std::vector<uint8_t> entityDead(MAX_PLAYERS, 0); // FIX: use uint8_t instead of bool
            auto scatterDead = mem.CreateScatterHandle();
            auto t_dead_start = std::chrono::steady_clock::now();
            if (!scatterDead) {
                allReadsSuccessful = false;
                dmaErrorCount++;
            }
            if (allReadsSuccessful) {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i]) continue;
                    mem.AddScatterReadRequest(scatterDead, entityAddrs[i] + p_entity->i_dead, &entityDead[i], sizeof(entityDead[i]));
                }
                mem.ExecuteReadScatter(scatterDead);
                mem.CloseScatterHandle(scatterDead);
            }
            auto t_dead_end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
            spdlog::info("ScatterRead: Dead check took {} us", std::chrono::duration_cast<std::chrono::microseconds>(t_dead_end - t_dead_start).count());
#endif

            std::vector<int> entityHealth(MAX_PLAYERS, 0);
            std::vector<int> entityTeam(MAX_PLAYERS, 0);
            std::vector<Vector3> entityHeadPos(MAX_PLAYERS);
            std::vector<Vector3> entityFootPos(MAX_PLAYERS);
            std::vector<std::array<char, 260>> nameBufs(MAX_PLAYERS);
            std::vector<std::string> entityNames(MAX_PLAYERS);
            std::vector<uint32_t> weaponPtrs(MAX_PLAYERS, 0);
            auto scatterData = mem.CreateScatterHandle();
            auto t_data_start = std::chrono::steady_clock::now();
            if (!scatterData) {
                allReadsSuccessful = false;
                dmaErrorCount++;
            }
            if (allReadsSuccessful) {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i] || entityDead[i]) continue;
                    mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_health, &entityHealth[i], sizeof(entityHealth[i]));
                    mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->i_team, &entityTeam[i], sizeof(entityTeam[i]));
                    mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->v3_head_pos, &entityHeadPos[i], sizeof(entityHeadPos[i]));
                    mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->v3_foot_pos, &entityFootPos[i], sizeof(entityFootPos[i]));
                    mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->str_name, nameBufs[i].data(), nameBufs[i].size());
                    mem.AddScatterReadRequest(scatterData, entityAddrs[i] + p_entity->weapon_class, &weaponPtrs[i], sizeof(weaponPtrs[i]));
                }
                mem.ExecuteReadScatter(scatterData);
                mem.CloseScatterHandle(scatterData);
                // Assign names after scatter read
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    entityNames[i] = std::string(nameBufs[i].data());
                }
            }
            auto t_data_end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
            spdlog::info("ScatterRead: Entity data took {} us", std::chrono::duration_cast<std::chrono::microseconds>(t_data_end - t_data_start).count());
#endif

            std::vector<int> entityWeaponId(MAX_PLAYERS, 0);
            auto scatterWeaponId = mem.CreateScatterHandle();
            auto t_weapon_start = std::chrono::steady_clock::now();
            if (!scatterWeaponId) {
                allReadsSuccessful = false;
                dmaErrorCount++;
            }
            if (allReadsSuccessful) {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i] || entityDead[i] || !weaponPtrs[i]) continue;
                    mem.AddScatterReadRequest(scatterWeaponId, weaponPtrs[i] + p_weapon->i_id, &entityWeaponId[i], sizeof(entityWeaponId[i]));
                }
                mem.ExecuteReadScatter(scatterWeaponId);
                mem.CloseScatterHandle(scatterWeaponId);
            }
            auto t_weapon_end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
            spdlog::info("ScatterRead: Weapon IDs took {} us", std::chrono::duration_cast<std::chrono::microseconds>(t_weapon_end - t_weapon_start).count());
#endif

            // Update entity cache if all reads succeed and there are valid entities
            if (allReadsSuccessful && playerCount > 0) {
                updateEntities->clear();
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i] || entityDead[i]) continue;
                    EntityData entityData;
                    entityData.name = entityNames[i];
                    entityData.health = entityHealth[i];
                    entityData.team = entityTeam[i];
                    entityData.headPosition = entityHeadPos[i];
                    entityData.footPosition = entityFootPos[i];
                    entityData.weaponClass = entityWeaponId[i];
                    if (entityWeaponId[i] >= 0 && entityWeaponId[i] < offsets->arr_weapon_names.size())
                        entityData.weaponName = offsets->arr_weapon_names[entityWeaponId[i]];
                    else
                        entityData.weaponName = "Unknown";
                    updateEntities->push_back(entityData);
                }
                if (!updateEntities->empty()) {
                    std::lock_guard<std::mutex> lock(entities_mutex);
                    std::swap(renderEntities, updateEntities);
                }
            }
            // If any read failed, do NOT clear or update entities!
            // No need to sleep here, throttling is handled above
            auto end = std::chrono::steady_clock::now();
            auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
#ifdef ESP_LOGGING_ENABLED
            if (duration_us > 20000) { // Log if update takes >20ms
                spdlog::warn("UpdateEntities took {} us (dmaErrorCount={})", duration_us, dmaErrorCount);
            }
#endif
        }
    }
    void StartEntityUpdateThread() {
        std::thread([]{
            // Lower thread priority to avoid competing with rendering
            #ifdef _WIN32
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            #endif
            UpdateEntities();
        }).detach();
    }
}

void ESP::Render(ImDrawList* drawList)
{
    auto start = std::chrono::steady_clock::now();
    int width = (int)Screen.x;
    int height = (int)Screen.y;
    int localPlayerTeam = 0;
    std::vector<EntityData> entitiesCopy;
    static std::map<std::string, Vector3> lastValidHeadPos;
    static std::map<std::string, Vector3> lastValidFootPos;
    static std::map<std::string, int> failedFrames;
    {
        auto mutexWaitStart = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(EntityManager::entities_mutex);
            auto mutexWaitEnd = std::chrono::steady_clock::now();
            auto mutexWait_us = std::chrono::duration_cast<std::chrono::microseconds>(mutexWaitEnd - mutexWaitStart).count();
#ifdef ESP_LOGGING_ENABLED
            if (mutexWait_us > 1000) { // Log if mutex wait >1ms
                spdlog::warn("ESP::Render mutex wait: {} us", mutexWait_us);
            }
#endif
            entitiesCopy = *EntityManager::renderEntities;
            if (!entitiesCopy.empty())
                localPlayerTeam = entitiesCopy[0].team;
        }
        std::set<std::string> currentNames;
        for (const auto& entity : entitiesCopy) currentNames.insert(entity.name);
        for (auto it = lastValidHeadPos.begin(); it != lastValidHeadPos.end(); ) {
            if (currentNames.find(it->first) == currentNames.end())
                it = lastValidHeadPos.erase(it);
            else
                ++it;
        }
        for (auto it = lastValidFootPos.begin(); it != lastValidFootPos.end(); ) {
            if (currentNames.find(it->first) == currentNames.end())
                it = lastValidFootPos.erase(it);
            else
                ++it;
        }
        for (const auto& entity : entitiesCopy)
        {
            if (config.Visuals.TeamCheck && entity.team == localPlayerTeam)
                continue;
            Vector2 headScreenPos, footScreenPos;
            bool headOk = sdk.WorldToScreen(entity.headPosition, headScreenPos, Globals::ViewMatrix, width, height);
            bool footOk = sdk.WorldToScreen(entity.footPosition, footScreenPos, Globals::ViewMatrix, width, height);
            if (!headOk || !footOk) {
                auto lastHeadIt = lastValidHeadPos.find(entity.name);
                auto lastFootIt = lastValidFootPos.find(entity.name);
                if (lastHeadIt != lastValidHeadPos.end() && lastFootIt != lastValidFootPos.end()) {
                    headOk = sdk.WorldToScreen(lastHeadIt->second, headScreenPos, Globals::ViewMatrix, width, height);
                    footOk = sdk.WorldToScreen(lastFootIt->second, footScreenPos, Globals::ViewMatrix, width, height);
                }
            } else {
                lastValidHeadPos[entity.name] = entity.headPosition;
                lastValidFootPos[entity.name] = entity.footPosition;
            }
            if (!headOk || !footOk) {
                failedFrames[entity.name]++;
                if (failedFrames[entity.name] > 30) {
                    continue;
                }
                continue;
            } else {
                failedFrames[entity.name] = 0;
            }
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
            if (config.Visuals.Health) {
                float healthPerc = std::clamp(entity.health / 100.0f, 0.0f, 1.0f);
                float hb_height = box_height;
                float hb_width = 6.0f;
                float hb_x = box_x - hb_width - 4.0f;
                float hb_y = box_y;
                ImU32 col_top = IM_COL32(0, 255, 0, 255);
                ImU32 col_bottom = IM_COL32(255, 0, 0, 255);
                float filled_height = hb_height * healthPerc;
                float empty_height = hb_height - filled_height;
                static std::map<std::string, float> animHealthPerc;
                float& animPerc = animHealthPerc[entity.name];
                animPerc += (healthPerc - animPerc) * (ImGui::GetIO().DeltaTime * 12.0f);
                float anim_filled_height = hb_height * animPerc;
                drawList->AddRectFilledMultiColor(
                    ImVec2(hb_x, hb_y + empty_height),
                    ImVec2(hb_x + hb_width, hb_y + hb_height),
                    col_top, col_top, col_bottom, col_bottom
                );
                if (empty_height > 0.0f) {
                    ImU32 bgColor = IM_COL32(40, 40, 40, 180);
                    drawList->AddRectFilled(
                        ImVec2(hb_x, hb_y),
                        ImVec2(hb_x + hb_width, hb_y + empty_height),
                        bgColor
                    );
                }
                drawList->AddRect(ImVec2(hb_x, hb_y), ImVec2(hb_x + hb_width, hb_y + hb_height), IM_COL32(0,0,0,180), 2.0f);
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
                ImVec2 weaponPos(footScreenPos.x - weaponTextSize.x / 2.0f, footScreenPos.y + 10.0f);
                drawList->AddText(weaponPos, weaponColor, entity.weaponName.c_str());
            }
        }
    }
    auto end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
    spdlog::info("ESP::Render: {} us", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
#endif
}