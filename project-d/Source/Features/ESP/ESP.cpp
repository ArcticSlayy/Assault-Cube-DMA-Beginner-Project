#define ESP_LOGGING_ENABLED
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
    // Triple buffer system for entities
    struct EntityBuffer {
        std::vector<EntityData> entities;
        std::chrono::steady_clock::time_point timestamp;
        bool ready = false;
    };

    // Three buffers: one for rendering, one for updating, one as a spare
    EntityBuffer bufferA;
    EntityBuffer bufferB;
    EntityBuffer bufferC;

    // Pointers for role assignment
    EntityBuffer* renderBuffer = &bufferA;     // Read by rendering thread
    EntityBuffer* updateBuffer = &bufferB;     // Written to by update thread
    EntityBuffer* spareBuffer = &bufferC;      // Ready to swap in when needed
    
    // Atomic pointer for lock-free access by renderer
    std::atomic<EntityBuffer*> atomicRenderBuffer{ &bufferA };
    
    std::mutex buffer_mutex;                   // Protects buffer swapping
    std::atomic<bool> buffer_ready{false};     // Signals when a new buffer is ready
    
    // Legacy support for existing code
    std::vector<EntityData>* renderEntities = &bufferA.entities;
    
    // View matrix double buffering
    struct ViewMatrixBuffer {
        Matrix matrix;
        std::chrono::steady_clock::time_point timestamp;
    };
    ViewMatrixBuffer currentViewMatrix;
    ViewMatrixBuffer previousViewMatrix;
    std::mutex viewMatrix_mutex;
    
    // Per-entity history for position prediction
    struct EntityHistory {
        std::string key;               // Unique key for identity (address/index/name)
        std::string name;              // Display name
        Vector3 lastHeadPosition;
        Vector3 lastFootPosition;
        Vector3 previousHeadPosition;   // Position from 2 updates ago
        Vector3 previousFootPosition;   // Position from 2 updates ago
        Vector3 velocity;               // Current velocity
        Vector3 smoothedVelocity;       // Smoothed velocity for more stable prediction
        Vector3 acceleration;           // Acceleration for better prediction
        Vector3 jitter;                 // Jitter measurement for adaptive smoothing
        std::chrono::steady_clock::time_point lastUpdateTime;
        std::chrono::steady_clock::time_point previousUpdateTime; // Time of update before last
        std::chrono::steady_clock::time_point firstSeenTime;
        int failedFrames = 0;
        int successFrames = 0;
        bool isValid = false;           // Whether this entity has reliable data
        float positionConfidence;       // How confident we are in the position (0-1)
        float stabilityFactor;          // How stable this entity's movement is (0-1)
        int consecutiveValidPositions = 0; // Count of valid position updates in a row
        
        // Box smoothing data
        float lastBoxWidth = 0.0f;     // Last calculated box width
        float lastBoxHeight = 0.0f;    // Last calculated box height
        float smoothedBoxWidth = 0.0f; // Smoothed box width
        float smoothedBoxHeight = 0.0f;// Smoothed box height
    };
    
    // Frame timing for consistent updates
    std::chrono::steady_clock::time_point lastRenderTime;
    float avgFrameTime = 0.016f;      // Default to 60fps (16.6ms)
    const float frameTimeSmoothing = 0.95f;
    
    // Thread scheduling and pacing
    const int UPDATE_THREAD_SLEEP_MICROSECONDS = 500;  // Sleep time between entity updates when no work
    std::atomic<bool> update_thread_active{false};     // Flag to indicate active updating
    
    // Cache-optimized entity history map to improve memory access patterns
    // This structure keeps entity histories in a contiguous memory block
    // for better cache locality and reduced memory fragmentation
    struct EntityHistoryCache {
        std::vector<EntityHistory> histories;
        std::unordered_map<std::string, size_t> keyToIndex;
        std::mutex mutex;
        
        EntityHistory* get(const std::string& key) {
            // First try a quick read-only lookup without locking
            {
                auto it = keyToIndex.find(key);
                if (it != keyToIndex.end() && it->second < histories.size()) {
                    return &histories[it->second];
                }
            }
            // Not found in read-only pass, need to lock and maybe add
            std::lock_guard<std::mutex> lock(mutex);
            auto it = keyToIndex.find(key);
            if (it == keyToIndex.end()) {
                size_t index = histories.size();
                histories.push_back(EntityHistory());
                histories.back().key = key;
                keyToIndex[key] = index;
                return &histories.back();
            }
            return &histories[it->second];
        }
        
        void removeStaleEntities(int maxFailedFrames) {
            std::lock_guard<std::mutex> lock(mutex);
            size_t writeIndex = 0;
            std::unordered_map<std::string, size_t> newMap;
            for (size_t i = 0; i < histories.size(); i++) {
                if (histories[i].failedFrames <= maxFailedFrames) {
                    if (i != writeIndex) {
                        histories[writeIndex] = std::move(histories[i]);
                    }
                    newMap[histories[writeIndex].key] = writeIndex;
                    writeIndex++;
                }
            }
            if (writeIndex < histories.size()) {
                histories.resize(writeIndex);
            }
            keyToIndex = std::move(newMap);
        }
        
        void clear() {
            std::lock_guard<std::mutex> lock(mutex);
            histories.clear();
            keyToIndex.clear();
        }
        
        // Get history without adding if not found
        EntityHistory* tryGet(const std::string& key) {
            auto it = keyToIndex.find(key);
            if (it != keyToIndex.end() && it->second < histories.size()) {
                return &histories[it->second];
            }
            return nullptr;
        }
    };
    
    EntityHistoryCache entityHistoryCache;
    
    // Dynamic update rate control
    std::atomic<int> updateRate{4};     // Milliseconds between updates, default 4ms (240Hz)
    std::atomic<int> badReadCount{0};   // Counter for failed reads to adjust update rate
    const int MAX_BAD_READS_BEFORE_SLOWDOWN = 5;
    const int MIN_UPDATE_RATE_MS = 2;   // 500Hz max
    const int MAX_UPDATE_RATE_MS = 8;   // 120Hz min

    // Validation constants
    constexpr float MAX_POSITION_JUMP = 500.0f;    // Max units an entity can move in one frame
    constexpr float MIN_VALID_POSITION = -16000.0f; // Min valid position value
    constexpr float MAX_VALID_POSITION = 16000.0f;  // Max valid position value
    constexpr int MIN_FRAMES_FOR_VALID = 3;         // Minimum frames before entity is considered valid
    constexpr int MAX_FAILED_FRAMES = 120;          // Maximum failed frames before entity is considered gone

    // Box size stability controls
    constexpr float MIN_BOX_HEIGHT = 4.0f;         // Minimum box height in pixels
    constexpr float MAX_BOX_HEIGHT = 800.0f;       // Maximum box height in pixels
    constexpr float MAX_BOX_HEIGHT_CHANGE_RATE = 0.2f;  // Max 20% change per frame
    constexpr float MIN_BOX_WIDTH = 2.0f;          // Minimum box width in pixels

    // Frame-independent animation settings
    const float ANIMATION_SPEED_BASE = 10.0f;       // Base animation speed 
    const float ANIMATION_SPEED_FAST = 15.0f;       // Fast animation speed for health decrease
    
    // Perspective scaling constants
    const float MIN_DISTANCE_FOR_SCALING = 100.0f;  // Minimum distance for scaling calculations
    const float DISTANCE_SCALING_FACTOR = 1000.0f;  // Reference distance for scaling calculations

    // Validate position vector
    bool IsPositionValid(const Vector3& pos) {
        return pos.x > MIN_VALID_POSITION && pos.x < MAX_VALID_POSITION &&
               pos.y > MIN_VALID_POSITION && pos.y < MAX_VALID_POSITION &&
               pos.z > MIN_VALID_POSITION && pos.z < MAX_VALID_POSITION;
    }

    // Check if position change is reasonable
    bool IsPositionChangeValid(const Vector3& oldPos, const Vector3& newPos, float maxDist) {
        if (!IsPositionValid(oldPos) || !IsPositionValid(newPos))
            return false;
            
        Vector3 diff = newPos - oldPos;
        float distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
        return distSq <= (maxDist * maxDist);
    }
    
    // Calculate vector magnitude
    float VectorMagnitude(const Vector3& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
    
    // Interpolate between two vectors with cubic hermite interpolation
    Vector3 SmoothInterpolate(const Vector3& p0, const Vector3& p1, const Vector3& v0, const Vector3& v1, float t) {
        float t2 = t * t;
        float t3 = t2 * t;
        
        // Hermite basis functions
        float h1 = 2*t3 - 3*t2 + 1;
        float h2 = -2*t3 + 3*t2;
        float h3 = t3 - 2*t2 + t;
        float h4 = t3 - t2;
        
        // Apply interpolation with tangents
        return {
            h1 * p0.x + h2 * p1.x + h3 * v0.x + h4 * v1.x,
            h1 * p0.y + h2 * p1.y + h3 * v0.y + h4 * v1.y,
            h1 * p0.z + h2 * p1.z + h3 * v0.z + h4 * v1.z
        };
    }
    
    // Update dynamic properties
    void UpdateDynamicProperties() {
        // Adjust update rate based on bad read count
        int badReads = badReadCount.exchange(0, std::memory_order_relaxed);
        
        // If too many bad reads, slow down update rate to reduce pressure on DMA
        if (badReads > MAX_BAD_READS_BEFORE_SLOWDOWN) {
            int newRate = std::min(updateRate.load(std::memory_order_relaxed) + 1, MAX_UPDATE_RATE_MS);
            updateRate.store(newRate, std::memory_order_relaxed);
        } 
        // If reads are good, gradually speed up
        else if (badReads == 0 && updateRate.load(std::memory_order_relaxed) > MIN_UPDATE_RATE_MS) {
            int newRate = updateRate.load(std::memory_order_relaxed) - 1;
            updateRate.store(newRate, std::memory_order_relaxed);
        }
    }

    // Pre-fetch and prepare for entity reading
    void PrepareEntityRead(VMMDLL_SCATTER_HANDLE scatterGlobals, uint32_t& playerCount, uint32_t& dwLocalPlayer, uint32_t& entityListAddr, Matrix& newViewMatrix, uint32_t& localPlayerTeam) {
        mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->player_count, &playerCount, sizeof(playerCount));
        mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->local_player, &dwLocalPlayer, sizeof(dwLocalPlayer));
        mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->entity_list, &entityListAddr, sizeof(entityListAddr));
        mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->view_matrix, &newViewMatrix, sizeof(newViewMatrix));
        if (dwLocalPlayer) {
            mem.AddScatterReadRequest(scatterGlobals, dwLocalPlayer + p_entity->i_team, &localPlayerTeam, sizeof(localPlayerTeam));
        }
    }
    
    void UpdateEntities() {
        constexpr int MAX_PLAYERS = 32;
        Matrix newViewMatrix; // Temporary view matrix
        int dmaErrorCount = 0;
        auto lastLogTime = std::chrono::steady_clock::now();
        static auto lastUpdate = std::chrono::steady_clock::now();
        static auto lastDynamicUpdate = std::chrono::steady_clock::now();
        static bool firstRun = true;
        
        // Pre-allocate memory for entity data to avoid reallocations during updates
        updateBuffer->entities.reserve(MAX_PLAYERS);
        renderBuffer->entities.reserve(MAX_PLAYERS);
        spareBuffer->entities.reserve(MAX_PLAYERS);
        
        // Set active flag for this thread
        update_thread_active.store(true, std::memory_order_release);
        
        // Advanced sleep pattern control for minimal CPU usage while maintaining responsiveness
        int consecutiveSlowFrames = 0;
        const int ADAPTIVE_SLEEP_THRESHOLD = 3; // Lower threshold for faster adaptation
        int currentSleepMicroseconds = UPDATE_THREAD_SLEEP_MICROSECONDS;
        const int MIN_SLEEP_MICROSECONDS = 100;  // Never sleep less than this to avoid CPU thrashing
        
        while (Globals::Running) {
            auto now = std::chrono::steady_clock::now();
            
            // Periodically update dynamic properties (every 1 second)
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDynamicUpdate).count() >= 1) {
                UpdateDynamicProperties();
                lastDynamicUpdate = now;
            }
            
            // Throttle based on dynamic update rate but allow first run immediately
            int currentUpdateRate = updateRate.load(std::memory_order_relaxed);
            if (!firstRun && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() < currentUpdateRate) {
                #ifdef _WIN32
                if (currentSleepMicroseconds <= 1000) {
                    auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(currentSleepMicroseconds);
                    while (std::chrono::steady_clock::now() < spinUntil) {
                        _mm_pause();
                    }
                } else {
                    Sleep(0);
                }
                #else
                std::this_thread::sleep_for(std::chrono::microseconds(currentSleepMicroseconds));
                #endif
                continue;
            }
            
            firstRun = false;
            lastUpdate = now;
            auto start = std::chrono::steady_clock::now();
            
            // Always update view matrix, even if entity reads fail
            auto scatterGlobals = mem.CreateScatterHandle();
            uint32_t playerCount = 0;
            uint32_t dwLocalPlayer = 0;
            uint32_t entityListAddr = 0;
            uint32_t localPlayerTeam = 0;
            bool globalsReadOk = false;
            
            if (scatterGlobals) {
                // First scatter: Get global info (player count, local player, entity list, view matrix)
                PrepareEntityRead(scatterGlobals, playerCount, dwLocalPlayer, entityListAddr, newViewMatrix, localPlayerTeam);
                mem.ExecuteReadScatter(scatterGlobals);
                mem.CloseScatterHandle(scatterGlobals);
                globalsReadOk = true;
            } else {
                dmaErrorCount++;
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            }
            
            // Update double buffered view matrix
            if (globalsReadOk) {
                ViewMatrixBuffer newCurrentMatrix;
                newCurrentMatrix.matrix = newViewMatrix;
                newCurrentMatrix.timestamp = now;
                if (viewMatrix_mutex.try_lock()) {
                    previousViewMatrix = currentViewMatrix;
                    currentViewMatrix = newCurrentMatrix;
                    viewMatrix_mutex.unlock();
                }
                Globals::ViewMatrix = newViewMatrix;
            }

            // Validate player count to avoid reading garbage
            if (playerCount <= 0 || playerCount > MAX_PLAYERS) {
                playerCount = MAX_PLAYERS; // Default to max if invalid
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            }

            bool allReadsSuccessful = globalsReadOk;
            
            // Arrays for entity data - preallocate for better performance
            std::vector<uint32_t> entityAddrs(MAX_PLAYERS, 0);
            std::vector<uint8_t> entityDead(MAX_PLAYERS, 0);
            std::vector<int> entityHealth(MAX_PLAYERS, 0);
            std::vector<int> entityTeam(MAX_PLAYERS, 0);
            std::vector<Vector3> entityHeadPos(MAX_PLAYERS);
            std::vector<Vector3> entityFootPos(MAX_PLAYERS);
            std::vector<std::array<char, 260>> nameBufs(MAX_PLAYERS);
            std::vector<std::string> entityNames(MAX_PLAYERS);
            std::vector<uint32_t> weaponPtrs(MAX_PLAYERS, 0);
            std::vector<int> entityWeaponId(MAX_PLAYERS, 0);
            
            // Step 1: Read entity addresses
            auto scatterEntities = mem.CreateScatterHandle();
            if (!scatterEntities) {
                allReadsSuccessful = false;
                dmaErrorCount++;
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    mem.AddScatterReadRequest(scatterEntities, entityListAddr + (i * 0x4), &entityAddrs[i], sizeof(entityAddrs[i]));
                }
                mem.ExecuteReadScatter(scatterEntities);
                mem.CloseScatterHandle(scatterEntities);
            }
            
            // Step 2: Check which entities are dead (to skip them)
            auto scatterDead = mem.CreateScatterHandle();
            if (!scatterDead) {
                allReadsSuccessful = false;
                dmaErrorCount++;
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i]) continue;
                    mem.AddScatterReadRequest(scatterDead, entityAddrs[i] + p_entity->i_dead, &entityDead[i], sizeof(entityDead[i]));
                }
                mem.ExecuteReadScatter(scatterDead);
                mem.CloseScatterHandle(scatterDead);
            }
            
            // Step 3: Read all entity data in one scatter operation
            auto scatterData = mem.CreateScatterHandle();
            if (!scatterData) {
                allReadsSuccessful = false;
                dmaErrorCount++;
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Batch all entity data reads in one go for better performance
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
                
                // Extract names after scatter read
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i] || entityDead[i]) continue;
                    entityNames[i] = std::string(nameBufs[i].data());
                    // Basic validation to avoid garbage names
                    if (entityNames[i].empty() || entityNames[i].length() > 32) {
                        entityNames[i] = "Player_" + std::to_string(i);
                    }
                }
            }

            // Step 4: Read weapon info 
            auto scatterWeaponId = mem.CreateScatterHandle();
            if (!scatterWeaponId) {
                allReadsSuccessful = false;
                dmaErrorCount++;
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            } else {
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i] || entityDead[i] || !weaponPtrs[i]) continue;
                    mem.AddScatterReadRequest(scatterWeaponId, weaponPtrs[i] + p_weapon->i_id, &entityWeaponId[i], sizeof(entityWeaponId[i]));
                }
                mem.ExecuteReadScatter(scatterWeaponId);
                mem.CloseScatterHandle(scatterWeaponId);
            }

            // Update entity cache if all reads succeed and there are valid entities
            if (allReadsSuccessful && playerCount > 0) {
                // First prepare the update buffer completely before any locking
                updateBuffer->entities.clear();
                updateBuffer->timestamp = std::chrono::steady_clock::now();
                
                // Store current keys for history tracking
                std::set<std::string> currentKeys;
                
                // First pass - collect data without locking
                std::vector<EntityData> newEntities;
                newEntities.reserve(playerCount);
                
                for (int i = 1; i < playerCount && i < MAX_PLAYERS; i++) {
                    if (!entityAddrs[i] || entityDead[i]) continue;
                    // Skip if health is invalid
                    if (entityHealth[i] <= 0 || entityHealth[i] > 200) continue;
                    // Validate positions
                    if (!IsPositionValid(entityHeadPos[i]) || !IsPositionValid(entityFootPos[i])) {
                        continue;
                    }
                    EntityData entityData;
                    entityData.name = entityNames[i];
                    entityData.health = entityHealth[i];
                    entityData.team = entityTeam[i];
                    entityData.headPosition = entityHeadPos[i];
                    entityData.footPosition = entityFootPos[i];
                    entityData.weaponClass = entityWeaponId[i];
                    entityData.id = static_cast<uint64_t>(entityAddrs[i]);
                    entityData.index = i;
                    if (entityWeaponId[i] >= 0 && entityWeaponId[i] < offsets->arr_weapon_names.size())
                        entityData.weaponName = offsets->arr_weapon_names[entityWeaponId[i]];
                    else
                        entityData.weaponName = "Unknown";
                    
                    // Build unique key for this entity
                    char keyBuf[128];
                    snprintf(keyBuf, sizeof(keyBuf), "%s|%d|%llX|%d", entityData.name.c_str(), entityData.team, (unsigned long long)entityData.id, entityData.index);
                    currentKeys.insert(std::string(keyBuf));
                    
                    newEntities.push_back(entityData);
                }
                
                // Second pass - update histories and apply to buffer
                for (const auto& entityData : newEntities) {
                    // Build key again (avoid extra allocations by reusing format)
                    char keyBuf[128];
                    snprintf(keyBuf, sizeof(keyBuf), "%s|%d|%llX|%d", entityData.name.c_str(), entityData.team, (unsigned long long)entityData.id, entityData.index);
                    std::string key(keyBuf);
                    
                    // Get entity history from cache (thread-safe)
                    auto history = entityHistoryCache.get(key);
                    
                    // Process entity data with history (no locks needed here)
                    if (history->lastUpdateTime.time_since_epoch().count() == 0) {
                        // Initialize history for new entities
                        history->key = key;
                        history->name = entityData.name;
                        history->firstSeenTime = now;
                        history->lastHeadPosition = entityData.headPosition;
                        history->lastFootPosition = entityData.footPosition;
                        history->previousHeadPosition = entityData.headPosition;
                        history->previousFootPosition = entityData.footPosition;
                        history->velocity = {0, 0, 0};
                        history->smoothedVelocity = {0, 0, 0};
                        history->acceleration = {0, 0, 0};
                        history->jitter = {0, 0, 0};
                        history->successFrames = 1;
                        history->failedFrames = 0;
                        history->positionConfidence = 0.1f;
                        history->stabilityFactor = 0.3f; // Start with higher stability for smoother initial boxes
                        history->consecutiveValidPositions = 1;
                        history->previousUpdateTime = now;
                        history->lastBoxWidth = 0.0f;
                        history->lastBoxHeight = 0.0f;
                        history->smoothedBoxWidth = 0.0f;
                        history->smoothedBoxHeight = 0.0f;
                    } 
                    else {
                        // Calculate time delta
                        float timeDelta = std::chrono::duration<float>(now - history->lastUpdateTime).count();
                        if (timeDelta > 0) {
                            // Validate position change (reject extreme jumps) with delta time scaling
                            float adjustedMaxJump = MAX_POSITION_JUMP * std::min(timeDelta, 0.1f) * 10.0f;
                            bool validHeadChange = IsPositionChangeValid(
                                history->lastHeadPosition, 
                                entityData.headPosition, 
                                adjustedMaxJump
                            );
                            
                            if (validHeadChange) {
                                // Store previous position before updating
                                history->previousHeadPosition = history->lastHeadPosition;
                                history->previousFootPosition = history->lastFootPosition;
                                history->previousUpdateTime = history->lastUpdateTime;
                                
                                // Calculate instantaneous velocity
                                Vector3 newVelocity = (entityData.headPosition - history->lastHeadPosition) * (1.0f / timeDelta);
                                
                                // Calculate acceleration
                                Vector3 newAcceleration = (newVelocity - history->velocity) * (1.0f / timeDelta);
                                
                                // Calculate jitter (rapid changes in acceleration)
                                Vector3 jitterVec = {
                                    std::abs(newAcceleration.x - history->acceleration.x),
                                    std::abs(newAcceleration.y - history->acceleration.y),
                                    std::abs(newAcceleration.z - history->acceleration.z)
                                };
                                
                                // Calculate jitter magnitude - use exponential decay to prevent spikes
                                history->jitter = {
                                    history->jitter.x * 0.85f + jitterVec.x * 0.15f,
                                    history->jitter.y * 0.85f + jitterVec.y * 0.15f,
                                    history->jitter.z * 0.85f + jitterVec.z * 0.15f
                                };
                                
                                // Calculate stability factor based on smoothed jitter magnitude
                                float smoothedJitterMag = VectorMagnitude(history->jitter);
                                float stabilityTarget = smoothedJitterMag < 5.0f ? 1.0f : 
                                                      (smoothedJitterMag > 50.0f ? 0.1f : 
                                                      (1.0f - (smoothedJitterMag - 5.0f) / 45.0f));
                                
                                // Smooth stability transition (slower transitions help reduce flickering)
                                history->stabilityFactor = history->stabilityFactor * 0.97f + stabilityTarget * 0.03f;
                                
                                // Adapt smoothing rates based on stability
                                float velocityAlpha = 0.1f + (0.3f * history->stabilityFactor);
                                float accelAlpha = 0.05f + (0.15f * history->stabilityFactor);
                                
                                // Apply smoothed updates
                                history->velocity = newVelocity;
                                history->smoothedVelocity = history->smoothedVelocity * (1.0f - velocityAlpha) + newVelocity * velocityAlpha;
                                history->acceleration = history->acceleration * (1.0f - accelAlpha) + newAcceleration * accelAlpha;
                                
                                // Update position
                                history->lastHeadPosition = entityData.headPosition;
                                history->lastFootPosition = entityData.footPosition;
                                
                                // Increase confidence and success count
                                history->successFrames++;
                                history->failedFrames = 0;
                                history->consecutiveValidPositions++;
                                history->positionConfidence = std::min(1.0f, history->positionConfidence + 0.05f);
                                
                                // Mark as valid once we have enough successful frames
                                if (history->successFrames >= MIN_FRAMES_FOR_VALID) {
                                    history->isValid = true;
                                }
                            } else {
                                // Position jump too large - might be teleport or bad data
                                history->failedFrames++;
                                // Don't reset consecutive valid positions to 0 immediately
                                history->consecutiveValidPositions = std::max(0, history->consecutiveValidPositions - 2);
                                history->positionConfidence = std::max(0.1f, history->positionConfidence - 0.05f);
                                // Don't reduce stability factor too quickly to avoid flickering
                                history->stabilityFactor = std::max(0.25f, history->stabilityFactor - 0.05f);
                            }
                        }
                    }
                    
                    // Update timestamp
                    history->lastUpdateTime = now;
                    
                    // Add to update buffer (which is still local, no locks needed)
                    updateBuffer->entities.push_back(entityData);
                }
                
                // Handle entities that disappeared this frame
                for (auto& history : entityHistoryCache.histories) {
                    if (currentKeys.find(history.key) == currentKeys.end()) {
                        history.failedFrames++;
                        history.consecutiveValidPositions = std::max(0, history.consecutiveValidPositions - 2);
                        history.positionConfidence = std::max(0.1f, history.positionConfidence - 0.025f);
                        if (history.failedFrames > MAX_FAILED_FRAMES) {
                            history.isValid = false;
                        }
                    }
                }
                
                // Clean up truly stale entities periodically (every 5 seconds)
                static auto lastCleanupTime = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanupTime).count() >= 5) {
                    entityHistoryCache.removeStaleEntities(MAX_FAILED_FRAMES * 2);
                    lastCleanupTime = now;
                }
                
                // Mark update buffer as ready
                updateBuffer->ready = true;
                
                // Perform buffer swap with a short critical section
                {
                    std::lock_guard<std::mutex> lg(buffer_mutex);
                    // Rotate buffers: render -> spare, update -> render, spare -> update
                    EntityBuffer* temp = renderBuffer;
                    renderBuffer = updateBuffer;
                    updateBuffer = spareBuffer;
                    spareBuffer = temp;
                    // Update pointers for renderer
                    renderEntities = &renderBuffer->entities;
                    atomicRenderBuffer.store(renderBuffer, std::memory_order_release);
                }
                buffer_ready.store(true, std::memory_order_release);
            } else {
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            }
            
            // Measure frame time and adjust sleep behavior
            auto end = std::chrono::steady_clock::now();
            auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            
            // Highly adaptive sleep time based on workload
            if (duration_us > 4000) { // If frame took > 4ms
                consecutiveSlowFrames++;
                if (consecutiveSlowFrames > ADAPTIVE_SLEEP_THRESHOLD) {
                    currentSleepMicroseconds = std::min(2000, currentSleepMicroseconds + 50);
                }
            } else {
                consecutiveSlowFrames = std::max(0, consecutiveSlowFrames - 1);
                if (consecutiveSlowFrames == 0) {
                    currentSleepMicroseconds = std::max(MIN_SLEEP_MICROSECONDS, currentSleepMicroseconds - 25);
                }
            }

#ifdef ESP_LOGGING_ENABLED
            if (duration_us > 10000) {
                spdlog::warn("UpdateEntities took {} us (dmaErrorCount={})", duration_us, dmaErrorCount);
            }
#endif
        }
        
        // Clear active flag when thread exits
        update_thread_active.store(false, std::memory_order_release);
    }
    
    // Start the entity update thread
    void StartEntityUpdateThread() {
        static std::thread updateThread;
        
        // Print local player address for debugging
        auto scatterGlobals = mem.CreateScatterHandle();
        if (scatterGlobals) {
            uint32_t dwLocalPlayer = 0;
            mem.AddScatterReadRequest(scatterGlobals, Globals::ClientBase + p_game->local_player, &dwLocalPlayer, sizeof(dwLocalPlayer));
            mem.ExecuteReadScatter(scatterGlobals);
            mem.CloseScatterHandle(scatterGlobals);
            
            if (dwLocalPlayer) {
                spdlog::info("==== LOCAL PLAYER ADDRESS: 0x{:X} ====", dwLocalPlayer);
                
                // Optional: Print additional information about the local player
                uint32_t health = 0;
                uint32_t team = 0;
                Vector3 position;
                char nameBuf[64] = {0};
                
                auto scatterDetails = mem.CreateScatterHandle();
                if (scatterDetails) {
                    mem.AddScatterReadRequest(scatterDetails, dwLocalPlayer + p_entity->i_health, &health, sizeof(health));
                    mem.AddScatterReadRequest(scatterDetails, dwLocalPlayer + p_entity->i_team, &team, sizeof(team));
                    mem.AddScatterReadRequest(scatterDetails, dwLocalPlayer + p_entity->v3_foot_pos, &position, sizeof(position));
                    mem.AddScatterReadRequest(scatterDetails, dwLocalPlayer + p_entity->str_name, nameBuf, sizeof(nameBuf));
                    mem.ExecuteReadScatter(scatterDetails);
                    mem.CloseScatterHandle(scatterDetails);
                    
                    spdlog::info("Local Player Info:");
                    spdlog::info(" - Name: {}", nameBuf);
                    spdlog::info(" - Health: {}", health);
                    spdlog::info(" - Team: {}", team);
                    spdlog::info(" - Position: [{:.2f}, {:.2f}, {:.2f}]", position.x, position.y, position.z);
                }
            } else {
                spdlog::warn("Local player address not found during initialization");
            }
        }
        
        // Only start if not already running
        if (!update_thread_active.load(std::memory_order_acquire)) {
            // Make sure any old thread is properly joined
            if (updateThread.joinable()) {
                updateThread.join();
            }
            
            // Start a new update thread
            updateThread = std::thread(UpdateEntities);
            
            // Set thread priority to slightly below normal to avoid interfering with game thread
            #ifdef _WIN32
            SetThreadPriority(updateThread.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
            #endif
            
            // Detach the thread to allow it to run independently
            updateThread.detach();
            
            #ifdef ESP_LOGGING_ENABLED
            spdlog::info("Entity update thread started");
            #endif
        }
    }
}

// Thread-local storage for per-entity animation state
thread_local struct {
    std::unordered_map<std::string, float> animHealthPerc;
    std::unordered_map<std::string, float> lastBoxWidths;
    std::unordered_map<std::string, float> lastBoxHeights;
    float deltaTime = 0.016f;  // Default to 60fps
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastSeen;
    
    void cleanupStaleEntries(const std::chrono::steady_clock::time_point& now, 
                             const std::chrono::seconds& maxAge = std::chrono::seconds(30)) {
        std::vector<std::string> toRemove;
        for (const auto& [key, lastSeenTime] : lastSeen) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastSeenTime) > maxAge) {
                toRemove.push_back(key);
            }
        }
        for (const auto& key : toRemove) {
            animHealthPerc.erase(key);
            lastBoxWidths.erase(key);
            lastBoxHeights.erase(key);
            lastSeen.erase(key);
        }
#ifdef ESP_LOGGING_ENABLED
        if (!toRemove.empty()) {
            spdlog::debug("Cleaned up {} stale TLS animation entries", toRemove.size());
        }
#endif
    }
    
    void updateLastSeen(const std::string& key, const std::chrono::steady_clock::time_point& now) {
        lastSeen[key] = now;
    }
} tlsAnimationState;

void ESP::Render(ImDrawList* drawList)
{
    if (!config.Visuals.Enabled) return;

    // Timing and smoothing
    static auto lastFrameMetricOutput = std::chrono::steady_clock::now();
    static int frameCounter = 0;
    static float avgRenderTime = 0.0f;
    static float maxRenderTime = 0.0f;
    static float minRenderTime = 9999.0f;
    static bool showDebugInfo = false;

    auto frameStart = std::chrono::steady_clock::now();
    static auto lastFrameTime = std::chrono::steady_clock::now();
    float deltaTime = std::chrono::duration<float>(frameStart - lastFrameTime).count();
    lastFrameTime = frameStart;
    EntityManager::lastRenderTime = frameStart;

    // Toggle debug
    static bool keyWasDown = false;
    bool keyIsDown = GetAsyncKeyState(VK_F9) & 0x8000;
    if (keyIsDown && !keyWasDown) {
        showDebugInfo = !showDebugInfo;
        spdlog::info("Debug visualization: {}", showDebugInfo ? "ON" : "OFF");
    }
    keyWasDown = keyIsDown;

    // Clamp delta time
    const float MAX_DELTA_TIME = 0.05f;
    deltaTime = std::min(std::max(deltaTime, 0.0f), MAX_DELTA_TIME);
    tlsAnimationState.deltaTime = deltaTime;
    EntityManager::avgFrameTime = EntityManager::avgFrameTime * EntityManager::frameTimeSmoothing +
                                  deltaTime * (1.0f - EntityManager::frameTimeSmoothing);
    float animationDeltaTime = std::min(deltaTime, 0.033f);

    // Cleanup TLS animation state periodically
    auto currentTime = std::chrono::steady_clock::now();
    static auto lastCleanupTime = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - lastCleanupTime).count() >= 5) {
        tlsAnimationState.cleanupStaleEntries(currentTime);
        lastCleanupTime = currentTime;
    }

    int width = (int)Screen.x;
    int height = (int)Screen.y;

    // Lock-free snapshot of current render buffer
    EntityManager::EntityBuffer* bufferPtr = EntityManager::atomicRenderBuffer.load(std::memory_order_acquire);
    if (!bufferPtr || bufferPtr->entities.empty()) {
        return;
    }
    const std::vector<EntityData>& entitiesRef = bufferPtr->entities;

    // View matrix with robust fallback
    static Matrix lastGoodViewMatrix{};
    static Matrix lastPrevViewMatrix{};
    Matrix viewMatrix = Globals::ViewMatrix;
    Matrix prevViewMatrix = lastPrevViewMatrix;
    if (EntityManager::viewMatrix_mutex.try_lock()) {
        viewMatrix = EntityManager::currentViewMatrix.matrix;
        prevViewMatrix = EntityManager::previousViewMatrix.matrix;
        EntityManager::viewMatrix_mutex.unlock();
        lastGoodViewMatrix = viewMatrix;
        lastPrevViewMatrix = prevViewMatrix;
    } else {
        viewMatrix = lastGoodViewMatrix;
        prevViewMatrix = lastPrevViewMatrix;
    }

    // Local player team
    int localPlayerTeam = 0;
    if (!entitiesRef.empty()) {
        localPlayerTeam = entitiesRef[0].team;
    }

    int totalEntities = 0;
    int renderedEntities = 0;
    int debugShownCount = 0; // reset every frame

    for (const auto& entity : entitiesRef) {
        totalEntities++;

        // Build stable key (prevents duplicate-name flicker)
        char keyBuf[128];
        snprintf(keyBuf, sizeof(keyBuf), "%s|%d|%llX|%d", entity.name.c_str(), entity.team, (unsigned long long)entity.id, entity.index);
        std::string entityKey(keyBuf);

        // Update animation last-seen
        tlsAnimationState.updateLastSeen(entityKey, currentTime);

        // Team check
        if (config.Visuals.TeamCheck && entity.team == localPlayerTeam)
            continue;

        // History lookup
        auto history = EntityManager::entityHistoryCache.get(entityKey);
        bool hasValidHistory = history && history->isValid;
        if (!hasValidHistory && entitiesRef.size() > 1) {
            if (!history || history->consecutiveValidPositions < 2) {
                continue;
            }
        }

        // Base positions
        Vector3 headPos = entity.headPosition;
        Vector3 footPos = entity.footPosition;

        // Distance (for opacity, optional)
        float distance = std::sqrt(headPos.x * headPos.x + headPos.y * headPos.y + headPos.z * headPos.z);

        // Prediction
        if (hasValidHistory) {
            float tDelta = std::chrono::duration<float>(currentTime - history->lastUpdateTime).count();
            if (tDelta > 0 && tDelta < 0.5f) {
                float updateInterval = std::chrono::duration<float>(history->lastUpdateTime - history->previousUpdateTime).count();
                if (updateInterval <= 0) updateInterval = EntityManager::avgFrameTime;
                if (tDelta <= updateInterval && history->consecutiveValidPositions >= 2) {
                    float t = tDelta / updateInterval;
                    float vScale = 0.5f * updateInterval * history->stabilityFactor;
                    Vector3 v0 = history->smoothedVelocity * vScale;
                    Vector3 v1 = history->smoothedVelocity * vScale;
                    headPos = EntityManager::SmoothInterpolate(history->previousHeadPosition, history->lastHeadPosition, v0, v1, t);
                    footPos = EntityManager::SmoothInterpolate(history->previousFootPosition, history->lastFootPosition, v0, v1, t);
                } else {
                    float stability = history->stabilityFactor;
                    float vW = std::min(1.0f, stability * (1.0f - tDelta * 0.5f));
                    float aW = std::min(0.3f, stability * (1.0f - tDelta)) * 0.5f;
                    Vector3 vOff = history->smoothedVelocity * tDelta * vW;
                    Vector3 aOff = history->acceleration * 0.5f * tDelta * tDelta * aW;
                    headPos = entity.headPosition + vOff + aOff;
                    footPos = entity.footPosition + vOff + aOff;
                }
            }
        }

        // Project
        Vector2 headScreenPos, footScreenPos;
        bool headOk = sdk.WorldToScreen(headPos, headScreenPos, viewMatrix, width, height);
        bool footOk = sdk.WorldToScreen(footPos, footScreenPos, viewMatrix, width, height);
        if ((!headOk || !footOk) && prevViewMatrix[0][0] != 0) {
            headOk = sdk.WorldToScreen(headPos, headScreenPos, prevViewMatrix, width, height);
            footOk = sdk.WorldToScreen(footPos, footScreenPos, prevViewMatrix, width, height);
        }
        if (!headOk || !footOk) continue;

        // Screen bounds with margin
        const float SCREEN_MARGIN = 0.2f;
        if (headScreenPos.x < -width * SCREEN_MARGIN || headScreenPos.x > width * (1 + SCREEN_MARGIN) ||
            headScreenPos.y < -height * SCREEN_MARGIN || headScreenPos.y > height * (1 + SCREEN_MARGIN) ||
            footScreenPos.x < -width * SCREEN_MARGIN || footScreenPos.x > width * (1 + SCREEN_MARGIN) ||
            footScreenPos.y < -height * SCREEN_MARGIN || footScreenPos.y > height * (1 + SCREEN_MARGIN)) {
            continue;
        }

        // Box
        float box_height = std::max(footScreenPos.y - headScreenPos.y, 1.0f);
        if (box_height < EntityManager::MIN_BOX_HEIGHT || box_height > EntityManager::MAX_BOX_HEIGHT) continue;
        float box_width = std::max(box_height * 0.42f, 3.0f);
        float box_x = headScreenPos.x - (box_width / 2.0f);
        float box_y = headScreenPos.y;

        // Box smoothing keyed by stable identity
        auto& lastBoxWidth = tlsAnimationState.lastBoxWidths[entityKey];
        auto& lastBoxHeight = tlsAnimationState.lastBoxHeights[entityKey];
        if (lastBoxWidth <= 0) { lastBoxWidth = box_width; lastBoxHeight = box_height; }
        float stability = hasValidHistory ? history->stabilityFactor : 0.5f;
        float boxSmoothFactor = std::min(0.25f, animationDeltaTime * 7.5f) * (0.5f + stability * 0.5f);
        const float MAX_SIZE_CHANGE_RATE = 0.15f;
        float maxWidthDelta = lastBoxWidth * MAX_SIZE_CHANGE_RATE;
        float maxHeightDelta = lastBoxHeight * MAX_SIZE_CHANGE_RATE;
        float targetWidth = std::clamp(box_width, lastBoxWidth - maxWidthDelta, lastBoxWidth + maxWidthDelta);
        float targetHeight = std::clamp(box_height, lastBoxHeight - maxHeightDelta, lastBoxHeight + maxHeightDelta);
        box_width = lastBoxWidth * (1.0f - boxSmoothFactor) + targetWidth * boxSmoothFactor;
        box_height = lastBoxHeight * (1.0f - boxSmoothFactor) + targetHeight * boxSmoothFactor;
        lastBoxWidth = box_width;
        lastBoxHeight = box_height;
        box_x = headScreenPos.x - (box_width / 2.0f);

        // Opacity
        float opacityBase = hasValidHistory ? std::min(1.0f, history->positionConfidence) : 0.7f;
        float distanceOpacityFactor = (distance > 2000.0f) ? std::max(0.6f, 1.0f - ((distance - 2000.0f) / 10000.0f)) : 1.0f;
        float opacity = opacityBase * distanceOpacityFactor;

        // Box draw
        if (config.Visuals.Box) {
            ImU32 boxColor = IM_COL32((int)(config.Visuals.BoxColor.x * 255), (int)(config.Visuals.BoxColor.y * 255), (int)(config.Visuals.BoxColor.z * 255), (int)(config.Visuals.BoxColor.w * 255 * opacity));
            drawList->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_width, box_y + box_height), boxColor, 0.0f, 0, 1.5f);
        }

        // Debug visuals (limited count per frame)
        if (showDebugInfo && hasValidHistory && debugShownCount < 5) {
            debugShownCount++;
            Vector3 predictedPos = headPos + history->smoothedVelocity * 0.25f;
            Vector2 predictedScreenPos;
            if (sdk.WorldToScreen(predictedPos, predictedScreenPos, viewMatrix, width, height)) {
                drawList->AddLine(ImVec2(headScreenPos.x, headScreenPos.y), ImVec2(predictedScreenPos.x, predictedScreenPos.y), IM_COL32(255, 0, 255, 180), 2.0f);
                drawList->AddCircle(ImVec2(predictedScreenPos.x, predictedScreenPos.y), 4.0f, IM_COL32(255, 0, 255, 200), 12, 2.0f);
            }
            float velocity = EntityManager::VectorMagnitude(history->smoothedVelocity);
            char velText[32]; snprintf(velText, sizeof(velText), "%.1f u/s", velocity);
            drawList->AddText(ImVec2(box_x + box_width + 5, box_y), IM_COL32(255, 255, 0, (int)(255 * opacity)), velText);
            float stabBarWidth = 30.0f, stabBarHeight = 4.0f, stabBarX = box_x + box_width + 5, stabBarY = box_y + 15;
            drawList->AddRectFilled(ImVec2(stabBarX, stabBarY), ImVec2(stabBarX + stabBarWidth, stabBarY + stabBarHeight), IM_COL32(80, 80, 80, (int)(150 * opacity)));
            ImU32 stabColor = IM_COL32((int)((1.0f - history->stabilityFactor) * 255), (int)(history->stabilityFactor * 255), 0, (int)(200 * opacity));
            drawList->AddRectFilled(ImVec2(stabBarX, stabBarY), ImVec2(stabBarX + stabBarWidth * history->stabilityFactor, stabBarY + stabBarHeight), stabColor);
        }

        // Health bar
        if (config.Visuals.Health) {
            float healthPerc = std::clamp(entity.health / 100.0f, 0.0f, 1.0f);
            float hb_height = box_height;
            float hb_width = 6.0f;
            float hb_x = box_x - hb_width - 4.0f;
            float hb_y = box_y;
            ImU32 col_top = IM_COL32(0, 255, 0, (int)(255 * opacity));
            ImU32 col_bottom = IM_COL32(255, 0, 0, (int)(255 * opacity));
            float& animPerc = tlsAnimationState.animHealthPerc[entityKey];
            if (animPerc == 0.0f) animPerc = healthPerc;
            float healthChangeSpeed = (animPerc > healthPerc) ? EntityManager::ANIMATION_SPEED_FAST : EntityManager::ANIMATION_SPEED_BASE;
            float frameAdjustedSpeed = healthChangeSpeed * animationDeltaTime;
            animPerc += std::clamp(healthPerc - animPerc, -frameAdjustedSpeed, frameAdjustedSpeed);
            float anim_filled_height = hb_height * animPerc;
            float anim_empty_height = hb_height - anim_filled_height;
            drawList->AddRectFilledMultiColor(ImVec2(hb_x, hb_y + anim_empty_height), ImVec2(hb_x + hb_width, hb_y + hb_height), col_top, col_top, col_bottom, col_bottom);
            if (anim_empty_height > 0.0f) {
                ImU32 bgColor = IM_COL32(40, 40, 40, (int)(180 * opacity));
                drawList->AddRectFilled(ImVec2(hb_x, hb_y), ImVec2(hb_x + hb_width, hb_y + anim_empty_height), bgColor);
            }
            drawList->AddRect(ImVec2(hb_x, hb_y), ImVec2(hb_x + hb_width, hb_y + hb_height), IM_COL32(0,0,0,(int)(180 * opacity)), 0);
        }

        // Name
        float textY = box_y - 4;
        if (config.Visuals.Name) {
            ImU32 nameColor = IM_COL32((int)(config.Visuals.NameColor.x * 255), (int)(config.Visuals.NameColor.y * 255), (int)(config.Visuals.NameColor.z * 255), (int)(config.Visuals.NameColor.w * 255 * opacity));
            ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
            ImVec2 namePos(box_x + (box_width - textSize.x) / 2.0f, textY - textSize.y);
            drawList->AddText(ImVec2(namePos.x + 1, namePos.y + 1), IM_COL32(0, 0, 0, (int)(120 * opacity)), entity.name.c_str());
            drawList->AddText(ImVec2(namePos.x, namePos.y), nameColor, entity.name.c_str());
            textY = namePos.y;
        }

        // Weapon
        if (config.Visuals.Weapon && !entity.weaponName.empty()) {
            ImU32 weaponColor = IM_COL32((int)(config.Visuals.WeaponColor.x * 255), (int)(config.Visuals.WeaponColor.y * 255), (int)(config.Visuals.WeaponColor.z * 255), (int)(config.Visuals.WeaponColor.w * 255 * opacity));
            ImVec2 weaponTextSize = ImGui::CalcTextSize(entity.weaponName.c_str());
            ImVec2 weaponPos(box_x + (box_width - weaponTextSize.x) / 2.0f, box_y + box_height + 2.0f);
            drawList->AddText(ImVec2(weaponPos.x + 1, weaponPos.y + 1), IM_COL32(0, 0, 0, (int)(120 * opacity)), entity.weaponName.c_str());
            drawList->AddText(ImVec2(weaponPos.x, weaponPos.y), weaponColor, entity.weaponName.c_str());
        }

        renderedEntities++;
    }

    auto end = std::chrono::steady_clock::now();
    float renderTimeMs = std::chrono::duration<float, std::milli>(end - frameStart).count();

    if (showDebugInfo) {
        int totalPlayers = (int)entitiesRef.size();
        int livingPlayers = 0;
        for (const auto& e : entitiesRef) if (e.health > 0) livingPlayers++;
        char debugText[128];
        snprintf(debugText, sizeof(debugText), "DEBUG MODE [F9]   Players: %d/%d", livingPlayers, totalPlayers);
        ImVec2 textSize = ImGui::CalcTextSize(debugText);
        float padding = 10.0f;
        drawList->AddRectFilled(ImVec2(width - textSize.x - padding*2, padding), ImVec2(width - padding, padding + textSize.y + padding*2 + 25), IM_COL32(40, 40, 40, 220));
        drawList->AddRect(ImVec2(width - textSize.x - padding*2, padding), ImVec2(width - padding, padding + textSize.y + padding*2 + 25), IM_COL32(255, 255, 0, 230));
        drawList->AddText(ImVec2(width - textSize.x - padding + 1, padding + padding/2 + 1), IM_COL32(0, 0, 0, 180), debugText);
        drawList->AddText(ImVec2(width - textSize.x - padding, padding + padding/2), IM_COL32(255, 255, 0, 255), debugText);
        char perfText[128];
        snprintf(perfText, sizeof(perfText), "ESP: %.2fms | Rendered: %d/%d", avgRenderTime, renderedEntities, totalEntities);
        ImVec2 perfTextSize = ImGui::CalcTextSize(perfText);
        drawList->AddText(ImVec2(width - perfTextSize.x - padding, padding + textSize.y + padding), IM_COL32(200, 200, 200, 255), perfText);
    }

    // Metrics
    avgRenderTime = avgRenderTime * 0.95f + renderTimeMs * 0.05f;
    maxRenderTime = std::max(maxRenderTime, renderTimeMs);
    minRenderTime = std::min(minRenderTime, renderTimeMs);
    frameCounter++;
    if (std::chrono::duration_cast<std::chrono::seconds>(end - lastFrameMetricOutput).count() >= 1) {
#ifdef ESP_LOGGING_ENABLED
        spdlog::info("ESP Performance: Avg={:.2f}ms, Min={:.2f}ms, Max={:.2f}ms, Entities: {}/{}", avgRenderTime, minRenderTime, maxRenderTime, renderedEntities, totalEntities);
#endif
        lastFrameMetricOutput = end;
        frameCounter = 0;
        maxRenderTime = 0.0f;
        minRenderTime = 9999.0f;
    }
#ifdef ESP_LOGGING_ENABLED
    if (renderTimeMs > 10.0f) {
        spdlog::warn("ESP::Render: {:.2f} ms, Entities: {}/{}", renderTimeMs, renderedEntities, totalEntities);
    }
#endif
}