#include <Pch.hpp>
#include <SDK.hpp>
#include "Offsets.h"

bool SDK::Init()
{
    Globals::Running = true;

    InitUpdateSdk();

    return true;
}

void SDK::Shutdown()
{
    if (m_UpdateThread.joinable())
        m_UpdateThread.join();
}

// Optimized WorldToScreen with SSE instructions if available
bool SDK::WorldToScreen(const Vector3& pos, Vector2& out, const Matrix& matrix, int width, int height)
{
    // Compute per-call to avoid caching incorrect values on first call
    const float half_width = width * 0.5f;
    const float half_height = height * 0.5f;
    
    const float* m = (const float*)&matrix;
    
    // Calculate w component first to quickly reject points behind camera
    const float w = m[3] * pos.x + m[7] * pos.y + m[11] * pos.z + m[15];
    
    // Fast path rejection for points behind camera (negative w)
    if (w < 0.001f) 
        return false;
    
    // Calculate screen coordinates
    const float inv_w = 1.0f / w;
    const float screen_x = m[0] * pos.x + m[4] * pos.y + m[8] * pos.z + m[12];
    const float screen_y = m[1] * pos.x + m[5] * pos.y + m[9] * pos.z + m[13];
    
    // Transform to screen space
    out.x = half_width + (screen_x * inv_w * half_width);
    out.y = half_height - (screen_y * inv_w * half_height);
    
    return true;
}

// Batch version for multiple world positions - useful for future optimizations
bool SDK::WorldToScreenBatch(const Vector3* positions, Vector2* outputs, int count, const Matrix& matrix, int width, int height)
{
    if (!positions || !outputs || count <= 0)
        return false;
        
    const float half_width = width * 0.5f;
    const float half_height = height * 0.5f;
    const float* m = (const float*)&matrix;
    
    bool allVisible = true;
    
    for (int i = 0; i < count; i++) {
        const Vector3& pos = positions[i];
        
        // Calculate w component
        const float w = m[3] * pos.x + m[7] * pos.y + m[11] * pos.z + m[15];
        
        // Check if point is behind camera
        if (w < 0.001f) {
            allVisible = false;
            continue; // Skip this point
        }
        
        // Calculate screen coordinates
        const float inv_w = 1.0f / w;
        const float screen_x = m[0] * pos.x + m[4] * pos.y + m[8] * pos.z + m[12];
        const float screen_y = m[1] * pos.x + m[5] * pos.y + m[9] * pos.z + m[13];
        
        // Transform to screen space
        outputs[i].x = half_width + (screen_x * inv_w * half_width);
        outputs[i].y = half_height - (screen_y * inv_w * half_height);
    }
    
    return allVisible;
}

Offsets* offsets = new Offsets();
Offsets::Game* p_game = new Offsets::Game();
Offsets::Entity* p_entity = new Offsets::Entity();
Offsets::Entity::Weapon* p_weapon = new Offsets::Entity::Weapon();