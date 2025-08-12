#pragma once
#include <vector>
#include <mutex>
#include <atomic>
#include <map>
#include <chrono>
#include "Features.hpp"

// Forward declarations
namespace EntityManager {
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
    
    // Legacy support - points to the current renderBuffer's entities vector
    extern std::vector<EntityData>* renderEntities;
    
    // Synchronization
    extern std::mutex buffer_mutex;
    extern std::atomic<bool> buffer_ready;
    
    // Entity position tracking for prediction
    extern std::map<std::string, EntityHistory> entityHistory;
    extern std::mutex history_mutex;
    
    // Dynamic update rate control
    extern std::atomic<int> updateRate;
    extern std::atomic<int> badReadCount;
    extern const int MAX_BAD_READS_BEFORE_SLOWDOWN;
    extern const int MIN_UPDATE_RATE_MS;
    extern const int MAX_UPDATE_RATE_MS;
    
    // Helper functions
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
