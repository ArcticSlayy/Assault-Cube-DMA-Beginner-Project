#pragma once

namespace Structs
{
    // Watermark position options
    enum class WatermarkPosition {
        TopRight = 0,
        TopLeft = 1, 
        TopMiddle = 2,
        BottomLeft = 3,
        BottomRight = 4
    };

    struct KmboxConfig 
    {
        bool Enabled;
        std::string Ip;
        unsigned short Port;
        std::string Uuid;
    };

    struct AimConfig 
    {
        bool Trigger;
        int TriggerKey;
        int TriggerKeyMode;
        int TriggerDelay;

        bool Aimbot;

        bool DrawFov;
        ImVec4 AimbotFovColor;

        bool AimFriendly;
        bool AimVisible;

        int AimbotKey;
        int AimbotKeyMode;
        float AimbotFov;
        float AimbotSmooth;
    };

    struct VisualsConfig 
    {
        bool Enabled;
        bool VSync;
        bool TeamCheck;
        bool VisibleCheck;

        bool Background;

        bool Hitmarker;
        ImVec4 HitmarkerColor;

        bool Watermark;
        ImVec4 WatermarkColor;
        WatermarkPosition WatermarkPos = WatermarkPosition::TopRight;

        bool Name;
        ImVec4 NameColor;

        bool Box;
        ImVec4 BoxColor;
        ImVec4 BoxColorVisible;

        bool Health;

        bool Weapon;
        ImVec4 WeaponColor;

        bool Bones;
        ImVec4 BonesColor;
    };
}
