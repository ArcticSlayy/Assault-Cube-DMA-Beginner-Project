#pragma once
#include <vector>
#include <mutex>
#include <atomic>
#include <map>
#include <chrono>
#include "Features.hpp"

// Forward declarations
namespace EntityManager {
    // Batch process all screen positions first to improve cache locality
    struct EntityRenderData {
        const EntityData* entity;
        Vector2 headScreenPos;
        Vector2 footScreenPos;
        float boxWidth;
        float boxHeight;
        float boxX;
        float boxY;
        float opacity;
        float distance;  // 3D distance from camera
        bool valid;
        struct EntityHistory* history;
    };
    
    // Triple buffer system for entities
    struct EntityBuffer;
    struct EntityHistory;
    
    // View matrix double buffering
    struct ViewMatrixBuffer;
    extern ViewMatrixBuffer currentViewMatrix;
    extern ViewMatrixBuffer previousViewMatrix;
    extern std::mutex viewMatrix_mutex;
    
    // External references to the buffer pointers
    extern EntityBuffer* renderBuffer;
    extern EntityBuffer* updateBuffer;
    extern EntityBuffer* spareBuffer;
    
    // Synchronization
    extern std::mutex buffer_mutex;
    extern std::atomic<bool> buffer_ready;
    
    // Dynamic update rate control
    extern std::atomic<int> updateRate;
    extern std::atomic<int> badReadCount;
    extern const int MAX_BAD_READS_BEFORE_SLOWDOWN;
    extern const int MIN_UPDATE_RATE_MS;
    extern const int MAX_UPDATE_RATE_MS;
    
    // Animation constants
    extern const float ANIMATION_SPEED_BASE;
    extern const float ANIMATION_SPEED_FAST;
    
    // Box size stability controls
    extern const float MIN_BOX_HEIGHT;
    extern const float MAX_BOX_HEIGHT;
    extern const float MAX_BOX_HEIGHT_CHANGE_RATE;
    extern const float MIN_BOX_WIDTH;
    
    // Helper functions
    template <typename T>
    T Clamp(T value, T min, T max) {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
    
    bool IsPositionValid(const Vector3& pos);
    bool IsPositionChangeValid(const Vector3& oldPos, const Vector3& newPos, float maxDist);
    float VectorMagnitude(const Vector3& v);
    Vector3 SmoothInterpolate(const Vector3& p0, const Vector3& p1, const Vector3& v0, const Vector3& v1, float t);
    void UpdateDynamicProperties();
    
    // Start the entity update thread
    void StartEntityUpdateThread();
}

class ESP
{
private:
    void Render(ImDrawList* drawList);

public:
    void Update(ImDrawList* drawList)
    {
        TIMER("ESP render");
        Render(drawList);
    }
    static ESP& Get()
    {
        static ESP instance;
        return instance;
    }
};

inline ESP& esp = ESP::Get();
