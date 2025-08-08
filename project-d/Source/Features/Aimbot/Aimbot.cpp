#include <Pch.hpp>
#include <SDK.hpp>
#include "Aimbot.hpp"
#include "Features.hpp"
#include "Kmbox/Manager.hpp"
#include "Config/Config.hpp"
#include "ESP/ESP.hpp"

static EntityData* currentTarget = nullptr;

static bool IsValidTarget(EntityData* entity, int localTeam)
{
    if (!entity) return false;
    if (entity->health <= 0) return false;
    if (config.Aim.AimFriendly == false && entity->team == localTeam) return false;
    Vector2 screenPos;
    bool w2s = sdk.WorldToScreen(entity->headPosition, screenPos, Globals::ViewMatrix, Screen.x, Screen.y);
    if (!w2s) return false;
    if (screenPos.x < 0 || screenPos.x > Screen.x || screenPos.y < 0 || screenPos.y > Screen.y) return false;
    float dx = screenPos.x - (Screen.x / 2.0f);
    float dy = screenPos.y - (Screen.y / 2.0f);
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > 50000.0f) return false;
    return true;
}

// Helper: Find the best target for aimbot (returns pointer to EntityData or nullptr)
static EntityData* FindBestTarget()
{
    std::lock_guard<std::mutex> lock(EntityManager::entities_mutex);
    EntityData* best = nullptr;
    float bestDist = std::numeric_limits<float>::max();
    int localTeam = -1;
    auto& entities = *EntityManager::renderEntities;
    if (!entities.empty())
        localTeam = entities[0].team;
    float fovRadius = config.Aim.AimbotFov; // FOV in pixels (or scale as needed)
    for (size_t i = 0; i < entities.size(); ++i)
    {
        auto& entity = entities[i];
        if (i == 0) continue;
        if (entity.health <= 0) continue;
        if (config.Aim.AimFriendly == false && entity.team == localTeam) continue;
        Vector2 screenPos;
        sdk.WorldToScreen(entity.headPosition, screenPos, Globals::ViewMatrix, Screen.x, Screen.y);
        float dx = screenPos.x - (Screen.x / 2.0f);
        float dy = screenPos.y - (Screen.y / 2.0f);
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > fovRadius) continue; // Only consider targets inside FOV
        if (dist < bestDist) {
            bestDist = dist;
            best = &entity;
        }
    }
    return best;
}

static float lastDx = 0.0f;
static float lastDy = 0.0f;

void Aimbot::Update()
{
    if (!config.Aim.Aimbot)
        return;

    int localTeam = -1;
    auto& entities = *EntityManager::renderEntities;
    if (!entities.empty())
        localTeam = entities[0].team;

    // If current target is invalid, find a new one
    if (!IsValidTarget(currentTarget, localTeam))
        currentTarget = FindBestTarget();

    if (!IsValidTarget(currentTarget, localTeam))
        return;

    Vector2 screenPos;
    bool w2s = sdk.WorldToScreen(currentTarget->headPosition, screenPos, Globals::ViewMatrix, Screen.x, Screen.y);
    if (!w2s)
        return;

    // Print ESP-style debug info for comparison
    printf("[AIMBOT] Target: %s, HeadScreen: (%.2f, %.2f)\n", currentTarget->name.c_str(), screenPos.x, screenPos.y);

    // Use config.Aim.AimbotSmooth for speed/sensitivity
    float smooth = config.Aim.AimbotSmooth;
    float scale = smooth > 0.0f ? 1.0f / smooth : 1.0f; // Higher smooth = slower

    float targetDx = (screenPos.x - (Screen.x / 2));
    float targetDy = (screenPos.y - (Screen.y / 2));

    // Interpolate (lerp) from last delta to target delta
    lastDx = lastDx + (targetDx - lastDx) * scale;
    lastDy = lastDy + (targetDy - lastDy) * scale;

    int dx = static_cast<int>(lastDx);
    int dy = static_cast<int>(lastDy);

    // Only move if within FOV (already filtered in FindBestTarget)
    Kmbox.Mouse.MoveRelative(dx, dy);
}