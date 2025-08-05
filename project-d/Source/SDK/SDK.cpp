#include <Pch.hpp>
#include <SDK.hpp>
#include "Offsets.h"

bool SDK::Init()
{
    Globals::Running = true;

    InitUpdateSdk();

    return true;
}

// WorldToScreen using column-major float[16] (mythos style)
bool SDK::WorldToScreen(const Vector3& pos, Vector2& out, const Matrix& matrix, int width, int height)
{
    const float* m = (const float*)&matrix;
    float x = pos.x, y = pos.y, z = pos.z;
    float w = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (w < 0.001f) return false;
    float screen_x = m[0] * x + m[4] * y + m[8] * z + m[12];
    float screen_y = m[1] * x + m[5] * y + m[9] * z + m[13];
    screen_x = (width / 2.0f) + (screen_x / w) * (width / 2.0f);
    screen_y = (height / 2.0f) - (screen_y / w) * (height / 2.0f);
    out.x = screen_x;
    out.y = screen_y;
    return true;
}

Offsets* offsets = new Offsets();
Offsets::Game* p_game = new Offsets::Game();
Offsets::Entity* p_entity = new Offsets::Entity();
Offsets::Entity::Weapon* p_weapon = new Offsets::Entity::Weapon();