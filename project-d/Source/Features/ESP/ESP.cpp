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
        std::string name;
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
    };
    std::map<std::string, EntityHistory> entityHistory;
    std::mutex history_mutex;
    
    // Dynamic update rate control
    std::atomic<int> updateRate{4};     // Milliseconds between updates, default 4ms (240Hz)
    std::atomic<int> badReadCount{0};   // Counter for failed reads to adjust update rate
    const int MAX_BAD_READS_BEFORE_SLOWDOWN = 5;
    const int MIN_UPDATE_RATE_MS = 4;   // 240Hz max
    const int MAX_UPDATE_RATE_MS = 8;   // 120Hz min

    // Validation constants
    constexpr float MAX_POSITION_JUMP = 500.0f;    // Max units an entity can move in one frame
    constexpr float MIN_VALID_POSITION = -16000.0f; // Min valid position value
    constexpr float MAX_VALID_POSITION = 16000.0f;  // Max valid position value
    constexpr int MIN_FRAMES_FOR_VALID = 3;         // Minimum frames before entity is considered valid
    constexpr int MAX_FAILED_FRAMES = 120;          // Maximum failed frames before entity is considered gone

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

    void UpdateEntities() {
        constexpr int MAX_PLAYERS = 32;
        Matrix newViewMatrix; // Temporary view matrix
        int dmaErrorCount = 0;
        auto lastLogTime = std::chrono::steady_clock::now();
        static auto lastUpdate = std::chrono::steady_clock::now();
        static auto lastDynamicUpdate = std::chrono::steady_clock::now();
        static bool firstRun = true;
        
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
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            
            firstRun = false;
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
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            }
            
            // Update double buffered view matrix
            if (globalsReadOk) {
                std::lock_guard<std::mutex> lock(viewMatrix_mutex);
                previousViewMatrix = currentViewMatrix;
                currentViewMatrix.matrix = newViewMatrix;
                currentViewMatrix.timestamp = now;
            }
            
            // Update global view matrix
            Globals::ViewMatrix = newViewMatrix;

            // Validate player count to avoid reading garbage
            if (playerCount <= 0 || playerCount > MAX_PLAYERS) {
                playerCount = MAX_PLAYERS; // Default to max if invalid
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            }

            bool allReadsSuccessful = globalsReadOk;
            std::vector<uint32_t> entityAddrs(MAX_PLAYERS, 0);
            auto scatterEntities = mem.CreateScatterHandle();
            auto t_addr_start = std::chrono::steady_clock::now();
            if (!scatterEntities) {
                allReadsSuccessful = false;
                dmaErrorCount++;
                badReadCount.fetch_add(1, std::memory_order_relaxed);
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
                badReadCount.fetch_add(1, std::memory_order_relaxed);
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
                badReadCount.fetch_add(1, std::memory_order_relaxed);
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
                    if (!entityAddrs[i] || entityDead[i]) continue;
                    entityNames[i] = std::string(nameBufs[i].data());
                    
                    // Basic validation to avoid garbage names
                    if (entityNames[i].empty() || entityNames[i].length() > 32) {
                        entityNames[i] = "Player_" + std::to_string(i);
                    }
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
                badReadCount.fetch_add(1, std::memory_order_relaxed);
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
                // Process data into update buffer
                updateBuffer->entities.clear();
                updateBuffer->timestamp = std::chrono::steady_clock::now();
                
                // Store current names for history tracking
                std::set<std::string> currentNames;
                
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
                    
                    if (entityWeaponId[i] >= 0 && entityWeaponId[i] < offsets->arr_weapon_names.size())
                        entityData.weaponName = offsets->arr_weapon_names[entityWeaponId[i]];
                    else
                        entityData.weaponName = "Unknown";
                    
                    updateBuffer->entities.push_back(entityData);
                    currentNames.insert(entityData.name);
                    
                    // Update position history for this entity
                    {
                        std::lock_guard<std::mutex> lock(history_mutex);
                        auto& history = entityHistory[entityData.name];
                        
                        // Initialize history for new entities
                        if (history.lastUpdateTime.time_since_epoch().count() == 0) {
                            history.name = entityData.name;
                            history.firstSeenTime = now;
                            history.lastHeadPosition = entityData.headPosition;
                            history.lastFootPosition = entityData.footPosition;
                            history.previousHeadPosition = entityData.headPosition;
                            history.previousFootPosition = entityData.footPosition;
                            history.velocity = {0, 0, 0};
                            history.smoothedVelocity = {0, 0, 0};
                            history.acceleration = {0, 0, 0};
                            history.jitter = {0, 0, 0};
                            history.successFrames = 1;
                            history.failedFrames = 0;
                            history.positionConfidence = 0.1f;
                            history.stabilityFactor = 0.1f;
                            history.consecutiveValidPositions = 1;
                            history.previousUpdateTime = now;
                        } 
                        else {
                            // Calculate time delta
                            float timeDelta = std::chrono::duration<float>(now - history.lastUpdateTime).count();
                            if (timeDelta > 0) {
                                // Validate position change (reject extreme jumps)
                                bool validHeadChange = IsPositionChangeValid(
                                    history.lastHeadPosition, 
                                    entityData.headPosition, 
                                    MAX_POSITION_JUMP * timeDelta
                                );
                                
                                if (validHeadChange) {
                                    // Store previous position before updating
                                    history.previousHeadPosition = history.lastHeadPosition;
                                    history.previousFootPosition = history.lastFootPosition;
                                    history.previousUpdateTime = history.lastUpdateTime;
                                    
                                    // Calculate instantaneous velocity
                                    Vector3 newVelocity = (entityData.headPosition - history.lastHeadPosition) * (1.0f / timeDelta);
                                    
                                    // Calculate acceleration
                                    Vector3 newAcceleration = (newVelocity - history.velocity) * (1.0f / timeDelta);
                                    
                                    // Calculate jitter (rapid changes in acceleration)
                                    Vector3 jitterVec = {
                                        std::abs(newAcceleration.x - history.acceleration.x),
                                        std::abs(newAcceleration.y - history.acceleration.y),
                                        std::abs(newAcceleration.z - history.acceleration.z)
                                    };
                                    history.jitter = jitterVec;
                                    
                                    // Calculate stability factor based on jitter magnitude
                                    float jitterMag = VectorMagnitude(jitterVec);
                                    float stabilityTarget = jitterMag < 5.0f ? 1.0f : 
                                                          (jitterMag > 50.0f ? 0.1f : 
                                                          (1.0f - (jitterMag - 5.0f) / 45.0f));
                                    
                                    // Smooth stability transition
                                    history.stabilityFactor = history.stabilityFactor * 0.9f + stabilityTarget * 0.1f;
                                    
                                    // Adapt smoothing rates based on stability
                                    float velocityAlpha = 0.1f + (0.6f * history.stabilityFactor);
                                    float accelAlpha = 0.05f + (0.2f * history.stabilityFactor);
                                    
                                    // Apply smoothed updates
                                    history.velocity = newVelocity;
                                    history.smoothedVelocity = history.smoothedVelocity * (1.0f - velocityAlpha) + newVelocity * velocityAlpha;
                                    history.acceleration = history.acceleration * (1.0f - accelAlpha) + newAcceleration * accelAlpha;
                                    
                                    // Update position
                                    history.lastHeadPosition = entityData.headPosition;
                                    history.lastFootPosition = entityData.footPosition;
                                    
                                    // Increase confidence and success count
                                    history.successFrames++;
                                    history.failedFrames = 0;
                                    history.consecutiveValidPositions++;
                                    history.positionConfidence = std::min(1.0f, history.positionConfidence + 0.1f);
                                    
                                    // Mark as valid once we have enough successful frames
                                    if (history.successFrames >= MIN_FRAMES_FOR_VALID) {
                                        history.isValid = true;
                                    }
                                } else {
                                    // Position jump too large - might be teleport or bad data
                                    history.failedFrames++;
                                    history.consecutiveValidPositions = 0;
                                    history.positionConfidence = std::max(0.0f, history.positionConfidence - 0.2f);
                                    history.stabilityFactor = std::max(0.1f, history.stabilityFactor - 0.2f);
                                }
                            }
                        }
                        
                        // Update timestamp
                        history.lastUpdateTime = now;
                    }
                }
                
                // Handle entities that disappeared this frame
                {
                    std::lock_guard<std::mutex> lock(history_mutex);
                    for (auto it = entityHistory.begin(); it != entityHistory.end(); ++it) {
                        if (currentNames.find(it->first) == currentNames.end()) {
                            // Entity not found in current frame
                            it->second.failedFrames++;
                            it->second.consecutiveValidPositions = 0;
                            it->second.positionConfidence = std::max(0.0f, it->second.positionConfidence - 0.1f);
                            
                            // Only mark as invalid if it's been gone too long
                            if (it->second.failedFrames > MAX_FAILED_FRAMES) {
                                it->second.isValid = false;
                            }
                        }
                    }
                    
                    // Clean up truly stale entities (gone for too long)
                    for (auto it = entityHistory.begin(); it != entityHistory.end();) {
                        if (it->second.failedFrames > MAX_FAILED_FRAMES * 2) {
                            it = entityHistory.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                
                // Mark update buffer as ready and perform the swap
                updateBuffer->ready = true;
                
                {
                    std::lock_guard<std::mutex> lock(buffer_mutex);
                    // Rotate buffers: render -> spare, update -> render, spare -> update
                    EntityBuffer* temp = renderBuffer;
                    renderBuffer = updateBuffer;
                    updateBuffer = spareBuffer;
                    spareBuffer = temp;
                    
                    // Update pointer for legacy code compatibility
                    renderEntities = &renderBuffer->entities;
                }
                
                // Signal that a new buffer is ready
                buffer_ready.store(true, std::memory_order_release);
            } else {
                badReadCount.fetch_add(1, std::memory_order_relaxed);
            }
            
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
    // Skip rendering if visuals are disabled
    if (!config.Visuals.Enabled) return;
    
    auto start = std::chrono::steady_clock::now();
    int width = (int)Screen.x;
    int height = (int)Screen.y;
    
    // Local cache for the rendering phase to minimize lock time
    std::vector<EntityData> entitiesCopy;
    std::map<std::string, EntityManager::EntityHistory> historyCache;
    Matrix viewMatrix;
    Matrix prevViewMatrix;
    std::chrono::steady_clock::time_point viewMatrixTimestamp;
    std::chrono::steady_clock::time_point prevViewMatrixTimestamp;
    int localPlayerTeam = 0;
    auto currentTime = std::chrono::steady_clock::now();
    
    // Fast copy of current rendering data with minimal locking
    {
        // Quick check if we need to do a copy (using atomic bool)
        bool newBufferReady = EntityManager::buffer_ready.exchange(false, std::memory_order_acquire);
        
        // Minimal lock scope for entities - just copy the data we need
        {
            std::lock_guard<std::mutex> lock(EntityManager::buffer_mutex);
            entitiesCopy = EntityManager::renderBuffer->entities;
        }
        
        // Copy view matrix data with minimal locking
        {
            std::lock_guard<std::mutex> lock(EntityManager::viewMatrix_mutex);
            viewMatrix = EntityManager::currentViewMatrix.matrix;
            viewMatrixTimestamp = EntityManager::currentViewMatrix.timestamp;
            prevViewMatrix = EntityManager::previousViewMatrix.matrix;
            prevViewMatrixTimestamp = EntityManager::previousViewMatrix.timestamp;
        }
        
        // Extract local player team if available
        if (!entitiesCopy.empty()) {
            localPlayerTeam = entitiesCopy[0].team;
        }
        
        // Copy position history for prediction
        {
            std::lock_guard<std::mutex> lock(EntityManager::history_mutex);
            historyCache = EntityManager::entityHistory;
        }
    }
    
    // Process each entity for rendering
    for (const auto& entity : entitiesCopy)
    {
        // Skip teammates if team check is enabled
        if (config.Visuals.TeamCheck && entity.team == localPlayerTeam)
            continue;
        
        // Get entity history for position prediction
        auto historyIt = historyCache.find(entity.name);
        bool hasValidHistory = (historyIt != historyCache.end() && historyIt->second.isValid);
        
        // Skip entities without valid history data if we have multiple entities
        if (!hasValidHistory && historyCache.size() > 1 && historyIt->second.consecutiveValidPositions < 3) {
            continue;
        }
        
        // Get base positions
        Vector3 headPos = entity.headPosition;
        Vector3 footPos = entity.footPosition;
        
        // Apply position prediction based on velocity if we have history
        if (hasValidHistory) {
            auto& history = historyIt->second;
            
            // Calculate time since last update for prediction
            float timeDelta = std::chrono::duration<float>(currentTime - history.lastUpdateTime).count();
            
            // Only predict if we have a valid velocity and the time delta isn't too large
            if (timeDelta > 0 && timeDelta < 1.0f) {
                // Get time between previous and current update for interpolation purposes
                float updateInterval = std::chrono::duration<float>(history.lastUpdateTime - history.previousUpdateTime).count();
                if (updateInterval <= 0) updateInterval = 0.004f; // Default to 4ms if invalid
                
                // For small time deltas, use interpolation between known points
                if (timeDelta <= updateInterval && history.consecutiveValidPositions >= 2) {
                    // Get interpolation factor (0 to 1)
                    float t = timeDelta / updateInterval;
                    
                    // Scale velocity for tangent calculation
                    Vector3 v0 = history.velocity * updateInterval * 0.5f;
                    Vector3 v1 = history.velocity * updateInterval * 0.5f;
                    
                    // Use cubic hermite interpolation for smoother curves
                    headPos = EntityManager::SmoothInterpolate(
                        history.previousHeadPosition, 
                        history.lastHeadPosition,
                        v0, v1, t
                    );
                    
                    footPos = EntityManager::SmoothInterpolate(
                        history.previousFootPosition, 
                        history.lastFootPosition,
                        v0, v1, t
                    );
                } 
                // For larger time deltas, use extrapolation
                else {
                    // Advanced prediction using velocity and acceleration with stability weighting
                    float stabilityFactor = history.stabilityFactor;
                    
                    // Scale prediction accuracy by stability and time delta
                    // Longer predictions get less acceleration influence
                    float velocityWeight = std::min(1.0f, stabilityFactor * (1.0f - timeDelta * 0.5f));
                    float accelWeight = std::min(0.5f, stabilityFactor * (1.0f - timeDelta)) * 0.5f;
                    
                    // s = s0 + v*t + 0.5*a*t^2
                    Vector3 velocityOffset = history.smoothedVelocity * timeDelta * velocityWeight;
                    Vector3 accelOffset = history.acceleration * 0.5f * timeDelta * timeDelta * accelWeight;
                    
                    // Apply prediction
                    headPos = entity.headPosition + velocityOffset + accelOffset;
                    footPos = entity.footPosition + velocityOffset + accelOffset;
                }
            }
        }
        
        // Transform world positions to screen positions
        Vector2 headScreenPos, footScreenPos;
        bool headOk = sdk.WorldToScreen(headPos, headScreenPos, viewMatrix, width, height);
        bool footOk = sdk.WorldToScreen(footPos, footScreenPos, viewMatrix, width, height);
        
        // If primary projection fails, try with previous view matrix
        if ((!headOk || !footOk) && prevViewMatrixTimestamp.time_since_epoch().count() > 0) {
            headOk = sdk.WorldToScreen(headPos, headScreenPos, prevViewMatrix, width, height);
            footOk = sdk.WorldToScreen(footPos, footScreenPos, prevViewMatrix, width, height);
        }
        
        // Skip if positions can't be projected to screen
        if (!headOk || !footOk) {
            continue;
        }
        
        // Validate screen positions are within reasonable bounds
        // Allow slightly outside screen bounds to avoid pop-in at screen edges
        if (headScreenPos.x < -width*0.5f || headScreenPos.x > width*1.5f || 
            headScreenPos.y < -height*0.5f || headScreenPos.y > height*1.5f ||
            footScreenPos.x < -width*0.5f || footScreenPos.x > width*1.5f || 
            footScreenPos.y < -height*0.5f || footScreenPos.y > height*1.5f) {
            continue;
        }
        
        // Calculate ESP box dimensions with bounds checking
        float box_height = std::max(footScreenPos.y - headScreenPos.y, 1.0f);
        
        // Skip unrealistic boxes (too tall or too short)
        if (box_height < 5.0f || box_height > height * 2.0f) {
            continue;
        }
        
        float box_width = box_height / 2.0f;
        float box_x = headScreenPos.x - (box_width / 2.0f);
        float box_y = headScreenPos.y;
        
        // Skip if box is too thin
        if (box_width < 2.0f) {
            continue;
        }
        
        // Apply confidence-based rendering (entities with higher confidence get more opacity)
        float opacity = 1.0f;
        if (hasValidHistory) {
            opacity = std::min(1.0f, historyIt->second.positionConfidence);
        }
        
        // Render ESP box
        if (config.Visuals.Box) {
            ImU32 boxColor = IM_COL32(
                (int)(config.Visuals.BoxColor.x * 255),
                (int)(config.Visuals.BoxColor.y * 255),
                (int)(config.Visuals.BoxColor.z * 255),
                (int)(config.Visuals.BoxColor.w * 255 * opacity)
            );
            drawList->AddRect(ImVec2(box_x, box_y), ImVec2(box_x + box_width, box_y + box_height), boxColor, 0.0f, 0, 2.0f);
        }
        
        // Render health bar
        if (config.Visuals.Health) {
            float healthPerc = std::clamp(entity.health / 100.0f, 0.0f, 1.0f);
            float hb_height = box_height;
            float hb_width = 6.0f;
            float hb_x = box_x - hb_width - 4.0f;
            float hb_y = box_y;
            
            ImU32 col_top = IM_COL32(0, 255, 0, (int)(255 * opacity));
            ImU32 col_bottom = IM_COL32(255, 0, 0, (int)(255 * opacity));
            
            float filled_height = hb_height * healthPerc;
            float empty_height = hb_height - filled_height;
            
            // Smooth health bar animation
            static std::map<std::string, float> animHealthPerc;
            float& animPerc = animHealthPerc[entity.name];
            
            // Faster animation when health decreases, slower when it increases
            float animationSpeed = animPerc > healthPerc ? 20.0f : 8.0f;
            animPerc += (healthPerc - animPerc) * (ImGui::GetIO().DeltaTime * animationSpeed);
            float anim_filled_height = hb_height * animPerc;
            
            // Draw health bar with animation
            drawList->AddRectFilledMultiColor(
                ImVec2(hb_x, hb_y + empty_height),
                ImVec2(hb_x + hb_width, hb_y + hb_height),
                col_top, col_top, col_bottom, col_bottom
            );
            
            if (empty_height > 0.0f) {
                ImU32 bgColor = IM_COL32(40, 40, 40, (int)(180 * opacity));
                drawList->AddRectFilled(
                    ImVec2(hb_x, hb_y),
                    ImVec2(hb_x + hb_width, hb_y + empty_height),
                    bgColor
                );
            }
            
            drawList->AddRect(ImVec2(hb_x, hb_y), ImVec2(hb_x + hb_width, hb_y + hb_height), IM_COL32(0,0,0,(int)(180 * opacity)), 2.0f);
            
            // Clean up stale animations
            if (!hasValidHistory) {
                auto it = animHealthPerc.find(entity.name);
                if (it != animHealthPerc.end()) {
                    animHealthPerc.erase(it);
                }
            }
        }
        
        // Render name
        float textY = headScreenPos.y;
        if (config.Visuals.Name) {
            ImU32 nameColor = IM_COL32(
                (int)(config.Visuals.NameColor.x * 255),
                (int)(config.Visuals.NameColor.y * 255),
                (int)(config.Visuals.NameColor.z * 255),
                (int)(config.Visuals.NameColor.w * 255 * opacity)
            );
            ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
            ImVec2 namePos(headScreenPos.x - textSize.x / 2.0f, textY - textSize.y - 2.0f);
            drawList->AddText(namePos, nameColor, entity.name.c_str());
            textY = namePos.y + textSize.y + 2.0f;
        }
        
        // Render weapon name
        if (config.Visuals.Weapon && !entity.weaponName.empty()) {
            ImU32 weaponColor = IM_COL32(
                (int)(config.Visuals.WeaponColor.x * 255),
                (int)(config.Visuals.WeaponColor.y * 255),
                (int)(config.Visuals.WeaponColor.z * 255),
                (int)(config.Visuals.WeaponColor.w * 255 * opacity)
            );
            ImVec2 weaponTextSize = ImGui::CalcTextSize(entity.weaponName.c_str());
            ImVec2 weaponPos(footScreenPos.x - weaponTextSize.x / 2.0f, footScreenPos.y + 10.0f);
            drawList->AddText(weaponPos, weaponColor, entity.weaponName.c_str());
        }
    }
    
    auto end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
    spdlog::info("ESP::Render: {} us", std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
#endif
}