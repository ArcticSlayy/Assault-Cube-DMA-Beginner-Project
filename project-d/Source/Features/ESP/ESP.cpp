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
        std::unordered_map<std::string, size_t> nameToIndex;
        std::mutex mutex;
        
        EntityHistory* get(const std::string& name) {
            // First try a quick read-only lookup without locking
            {
                // This is safe because we don't modify the map in this block
                auto it = nameToIndex.find(name);
                if (it != nameToIndex.end() && it->second < histories.size()) {
                    // Found existing history, return pointer - no lock needed
                    return &histories[it->second];
                }
            }
            
            // Not found in read-only pass, need to lock and maybe add
            std::lock_guard<std::mutex> lock(mutex);
            auto it = nameToIndex.find(name);
            if (it == nameToIndex.end()) {
                // Add new entity to cache
                size_t index = histories.size();
                histories.push_back(EntityHistory());
                histories.back().name = name;
                nameToIndex[name] = index;
                return &histories.back();
            }
            return &histories[it->second];
        }
        
        void removeStaleEntities(int maxFailedFrames) {
            std::lock_guard<std::mutex> lock(mutex);
            
            // More efficient removal algorithm to minimize data movement
            size_t writeIndex = 0;
            std::unordered_map<std::string, size_t> newMap;
            
            // First pass: Mark entries to keep
            for (size_t i = 0; i < histories.size(); i++) {
                if (histories[i].failedFrames <= maxFailedFrames) {
                    if (i != writeIndex) {
                        // Move the history to its new position
                        histories[writeIndex] = std::move(histories[i]);
                    }
                    // Update index mapping
                    newMap[histories[writeIndex].name] = writeIndex;
                    writeIndex++;
                }
            }
            
            // Resize the vector to remove stale entries
            if (writeIndex < histories.size()) {
                histories.resize(writeIndex);
            }
            
            // Replace the map with the updated one
            nameToIndex = std::move(newMap);
        }
        
        void clear() {
            std::lock_guard<std::mutex> lock(mutex);
            histories.clear();
            nameToIndex.clear();
        }
        
        // Get history without adding if not found
        EntityHistory* tryGet(const std::string& name) {
            auto it = nameToIndex.find(name);
            if (it != nameToIndex.end() && it->second < histories.size()) {
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
                // Use a precision sleep to avoid burning CPU, with adaptive sleep duration
                // Use platform-specific sleep if available for better precision
                #ifdef _WIN32
                if (currentSleepMicroseconds <= 1000) {
                    // For very short sleeps, use spinwait instead to avoid scheduler overhead
                    auto spinUntil = std::chrono::steady_clock::now() + std::chrono::microseconds(currentSleepMicroseconds);
                    while (std::chrono::steady_clock::now() < spinUntil) {
                        // Yield to other threads but stay on the same core (if possible)
                        _mm_pause();  // Intel/AMD specific pause instruction, reduces power consumption in spin loops
                    }
                } else {
                    Sleep(0);  // Yield the remainder of the time slice to other threads
                }
                #else
                std::this_thread::sleep_for(std::chrono::microseconds(currentSleepMicroseconds));
                #endif
                continue;
            }
            
            firstRun = false;
            auto frameStartTime = now;
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
                // Minimize lock contention by preparing data outside the lock
                ViewMatrixBuffer newCurrentMatrix;
                newCurrentMatrix.matrix = newViewMatrix;
                newCurrentMatrix.timestamp = now;
                
                // Non-blocking matrix update - always prefer not blocking over consistency
                // since a slightly outdated matrix is better than hitching
                bool lockAcquired = false;
                {
                    if (viewMatrix_mutex.try_lock()) {
                        previousViewMatrix = currentViewMatrix;
                        currentViewMatrix = newCurrentMatrix;
                        viewMatrix_mutex.unlock();
                        lockAcquired = true;
                    }
                }
                
                // Update global view matrix without locking - this is fine because
                // matrix assignments are generally atomic for practical purposes on x86/x64
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
                
                // Store current names for history tracking
                std::set<std::string> currentNames;
                
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
                    
                    if (entityWeaponId[i] >= 0 && entityWeaponId[i] < offsets->arr_weapon_names.size())
                        entityData.weaponName = offsets->arr_weapon_names[entityWeaponId[i]];
                    else
                        entityData.weaponName = "Unknown";
                    
                    newEntities.push_back(entityData);
                    currentNames.insert(entityData.name);
                }
                
                // Second pass - update histories and apply to buffer
                for (const auto& entityData : newEntities) {
                    // Get entity history from cache (thread-safe)
                    auto history = entityHistoryCache.get(entityData.name);
                    
                    // Process entity data with history (no locks needed here)
                    if (history->lastUpdateTime.time_since_epoch().count() == 0) {
                        // Initialize history for new entities
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
                                float jitterMag = VectorMagnitude(jitterVec);
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
                                // This helps maintain smoother transitions during occasional bad reads
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
                // Process in batch using entityHistoryCache structure
                for (auto& history : entityHistoryCache.histories) {
                    if (currentNames.find(history.name) == currentNames.end()) {
                        // Entity not found in current frame
                        history.failedFrames++;
                        // Don't reduce consecutive valid positions to 0 immediately
                        // This helps maintain smoother transitions during occasional bad reads
                        history.consecutiveValidPositions = std::max(0, history.consecutiveValidPositions - 2);
                        history.positionConfidence = std::max(0.1f, history.positionConfidence - 0.025f);
                        
                        // Only mark as invalid if it's been gone too long
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
                
                // Perform buffer swap with minimal locking - use try_lock to avoid hitching
                // If we can't get the lock, skip this update - better to lose a frame than to hitch
                bool bufferSwapped = false;
                
                if (buffer_mutex.try_lock()) {
                    // Rotate buffers: render -> spare, update -> render, spare -> update
                    EntityBuffer* temp = renderBuffer;
                    renderBuffer = updateBuffer;
                    updateBuffer = spareBuffer;
                    spareBuffer = temp;
                    
                    // Update pointer for legacy code compatibility
                    renderEntities = &renderBuffer->entities;
                    
                    buffer_mutex.unlock();
                    bufferSwapped = true;
                }
                
                // Only signal if we actually swapped
                if (bufferSwapped) {
                    buffer_ready.store(true, std::memory_order_release);
                }
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
                    // Increase sleep time to reduce contention with rendering thread
                    currentSleepMicroseconds = std::min(2000, currentSleepMicroseconds + 50);
                }
            } else {
                consecutiveSlowFrames = std::max(0, consecutiveSlowFrames - 1);
                if (consecutiveSlowFrames == 0) {
                    // Gradually reduce sleep time when performance is good
                    currentSleepMicroseconds = std::max(MIN_SLEEP_MICROSECONDS, 
                                                      currentSleepMicroseconds - 25);
                }
            }

#ifdef ESP_LOGGING_ENABLED
            if (duration_us > 10000) { // Log if update takes >10ms - reduced threshold
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
// This prevents contention and locking for animation data
thread_local struct {
    std::unordered_map<std::string, float> animHealthPerc;
    std::unordered_map<std::string, float> lastBoxWidths;
    std::unordered_map<std::string, float> lastBoxHeights;
    
    // Frame timing for animations
    float deltaTime = 0.016f;  // Default to 60fps
} tlsAnimationState;

void ESP::Render(ImDrawList* drawList)
{
    // Skip rendering if visuals are disabled
    if (!config.Visuals.Enabled) return;
    
    auto frameStart = std::chrono::steady_clock::now();
    
    // Calculate delta time for smooth animations with protection against time jumps
    static auto lastFrameTime = std::chrono::steady_clock::now();
    float deltaTime = std::chrono::duration<float>(frameStart - lastFrameTime).count();
    lastFrameTime = frameStart;
    
    // Update EntityManager's lastRenderTime for coordination with update thread
    EntityManager::lastRenderTime = frameStart;
    
    // Cap delta time to prevent large jumps after freezes, alt-tabbing or FPS drops
    const float MAX_DELTA_TIME = 0.05f; // 50ms maximum delta (20fps minimum)
    deltaTime = std::min(std::max(deltaTime, 0.0f), MAX_DELTA_TIME);
    
    // Update thread-local delta time for animations
    tlsAnimationState.deltaTime = deltaTime;
    
    // Update average frame time for more stable animations (low-pass filter)
    EntityManager::avgFrameTime = EntityManager::avgFrameTime * EntityManager::frameTimeSmoothing + 
                                deltaTime * (1.0f - EntityManager::frameTimeSmoothing);
    
    // Use a consistent time step for animations to avoid jitter
    float animationDeltaTime = std::min(deltaTime, 0.033f); // Cap at 30fps for animations
    
    int width = (int)Screen.x;
    int height = (int)Screen.y;
    
    // Local cache for the rendering phase to minimize lock time
    static std::vector<EntityData> entitiesCopy; // Static to avoid reallocation
    static Matrix viewMatrix; // Static to avoid reallocation
    static Matrix prevViewMatrix; 
    int localPlayerTeam = 0;
    auto currentTime = std::chrono::steady_clock::now();
    bool dataUpdated = false;
    
    // COMPLETELY LOCK-FREE DESIGN: Try to get fresh data, but NEVER block if we can't get it
    // Use the copy we have from the last frame if the lock is contended
    bool gotFreshData = false;
    
    // Check if there's fresh data ready using the atomic flag
    bool newBufferReady = EntityManager::buffer_ready.exchange(false, std::memory_order_acquire);
    
    if (newBufferReady) {
        // Only try to copy data if it's been marked as ready
        // This avoids unnecessary lock attempts when there's no new data
        if (EntityManager::buffer_mutex.try_lock()) {
            entitiesCopy = EntityManager::renderBuffer->entities;
            EntityManager::buffer_mutex.unlock();
            gotFreshData = true;
        }
    }
    
    // Always try to get fresh view matrix, but never block
    if (EntityManager::viewMatrix_mutex.try_lock()) {
        viewMatrix = EntityManager::currentViewMatrix.matrix;
        prevViewMatrix = EntityManager::previousViewMatrix.matrix;
        EntityManager::viewMatrix_mutex.unlock();
    }
    
    // Extract local player team if available
    if (!entitiesCopy.empty()) {
        localPlayerTeam = entitiesCopy[0].team;
    }
    
    // If we have no entities to render, exit early
    if (entitiesCopy.empty()) {
        return;
    }
    
    // Track which entities we've drawn for performance metrics
    int totalEntities = 0;
    int renderedEntities = 0;
    
    // First pass: Process and render entities directly
    for (const auto& entity : entitiesCopy) {
        totalEntities++;
        
        // Skip teammates if team check is enabled
        if (config.Visuals.TeamCheck && entity.team == localPlayerTeam)
            continue;
        
        // Get entity history for position prediction
        auto history = EntityManager::entityHistoryCache.get(entity.name);
        bool hasValidHistory = history && history->isValid;
        
        // Skip entities without valid history data if we have multiple entities
        // But be less strict about validation to reduce flickering
        if (!hasValidHistory && entitiesCopy.size() > 1) {
            if (!history || history->consecutiveValidPositions < 2) {
                continue;
            }
        }
        
        // Get base positions
        Vector3 headPos = entity.headPosition;
        Vector3 footPos = entity.footPosition;
        
        // Calculate true 3D distance for accurate scaling
        float distance = 0.0f;
        {
            // Use Euclidean distance from origin (camera position)
            Vector3 diff;
            diff.x = headPos.x;
            diff.y = headPos.y; 
            diff.z = headPos.z;
            distance = std::sqrt(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
        }
        
        // Apply position prediction based on velocity if we have history
        if (hasValidHistory) {
            // Calculate time since last update for prediction
            float timeDelta = std::chrono::duration<float>(currentTime - history->lastUpdateTime).count();
            
            // Only predict if we have a valid velocity and the time delta isn't too large
            if (timeDelta > 0 && timeDelta < 0.5f) {
                // Get time between previous and current update for interpolation purposes
                float updateInterval = std::chrono::duration<float>(history->lastUpdateTime - history->previousUpdateTime).count();
                if (updateInterval <= 0) updateInterval = EntityManager::avgFrameTime; // Use avg frame time if invalid
                
                // For small time deltas, use interpolation between known points
                if (timeDelta <= updateInterval && history->consecutiveValidPositions >= 2) {
                    // Get interpolation factor (0 to 1)
                    float t = timeDelta / updateInterval;
                    
                    // Scale velocity for tangent calculation (with stability factor)
                    float velocityScale = 0.5f * updateInterval * history->stabilityFactor;
                    Vector3 v0 = history->smoothedVelocity * velocityScale;
                    Vector3 v1 = history->smoothedVelocity * velocityScale;
                    
                    // Use cubic hermite interpolation for smoother curves
                    headPos = EntityManager::SmoothInterpolate(
                        history->previousHeadPosition, 
                        history->lastHeadPosition,
                        v0, v1, t
                    );
                    
                    footPos = EntityManager::SmoothInterpolate(
                        history->previousFootPosition, 
                        history->lastFootPosition,
                        v0, v1, t
                    );
                } 
                // For larger time deltas, use extrapolation
                else {
                    // Advanced prediction using velocity and acceleration with stability weighting
                    float stabilityFactor = history->stabilityFactor;
                    
                    // Scale prediction accuracy by stability and time delta
                    // Longer predictions get less acceleration influence
                    float velocityWeight = std::min(1.0f, stabilityFactor * (1.0f - timeDelta * 0.5f));
                    float accelWeight = std::min(0.3f, stabilityFactor * (1.0f - timeDelta)) * 0.5f;
                    
                    // s = s0 + v*t + 0.5*a*t^2
                    Vector3 velocityOffset = history->smoothedVelocity * timeDelta * velocityWeight;
                    Vector3 accelOffset = history->acceleration * 0.5f * timeDelta * timeDelta * accelWeight;
                    
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
        if ((!headOk || !footOk) && prevViewMatrix[0][0] != 0) {
            headOk = sdk.WorldToScreen(headPos, headScreenPos, prevViewMatrix, width, height);
            footOk = sdk.WorldToScreen(footPos, footScreenPos, prevViewMatrix, width, height);
        }
        
        // Skip if positions can't be projected to screen
        if (!headOk || !footOk) {
            continue;
        }
        
        // Validate screen positions are within reasonable bounds
        // Allow slightly outside screen bounds to avoid pop-in at screen edges
        const float SCREEN_MARGIN = 0.2f; // 20% margin beyond screen edges
        if (headScreenPos.x < -width * SCREEN_MARGIN || headScreenPos.x > width * (1 + SCREEN_MARGIN) || 
            headScreenPos.y < -height * SCREEN_MARGIN || headScreenPos.y > height * (1 + SCREEN_MARGIN) ||
            footScreenPos.x < -width * SCREEN_MARGIN || footScreenPos.x > width * (1 + SCREEN_MARGIN) || 
            footScreenPos.y < -height * SCREEN_MARGIN || footScreenPos.y > height * (1 + SCREEN_MARGIN)) {
            continue;
        }
        
        // Calculate ESP box dimensions with bounds checking
        float box_height = std::max(footScreenPos.y - headScreenPos.y, 1.0f);
        
        // Skip unrealistic boxes (too tall or too short)
        if (box_height < EntityManager::MIN_BOX_HEIGHT || box_height > EntityManager::MAX_BOX_HEIGHT) {
            continue;
        }
        
        // ---- IMPROVED BOX SIZE CALCULATION BASED ON TRUE 3D DISTANCE AND PLAYER MODEL ----
        
        // Calculate proper box width based on player model proportions and distance
        // Calculate width-to-height ratio with proper distance scaling
        float baseBoxRatio = 0.42f;  // Base ratio for normal distances (slightly narrower than previous)
        
        // Calculate box width from height using a fixed ratio for consistent visuals
        float box_width = box_height * baseBoxRatio;
        
        // Apply minimum width regardless of distance to ensure visibility
        const float MIN_BOX_WIDTH_PIXELS = 3.0f;
        box_width = std::max(box_width, MIN_BOX_WIDTH_PIXELS);
        
        // Position the box centered at the head position
        float box_x = headScreenPos.x - (box_width / 2.0f);
        float box_y = headScreenPos.y;
        
        // Apply box size smoothing using thread-local storage
        auto& lastBoxWidth = tlsAnimationState.lastBoxWidths[entity.name];
        auto& lastBoxHeight = tlsAnimationState.lastBoxHeights[entity.name];
        
        // Initialize box dimensions if first time
        if (lastBoxWidth <= 0) {
            lastBoxWidth = box_width;
            lastBoxHeight = box_height;
        }
        
        // Calculate consistent smoothing factor based on frame rate
        // We want the same visual smoothness at all distances
        float stability = hasValidHistory ? history->stabilityFactor : 0.5f;
        
        // Fixed smoothing factor that doesn't vary by distance
        float boxSmoothFactor = std::min(0.25f, animationDeltaTime * 7.5f);
        
        // Scale by stability for smoother movement on stable entities
        boxSmoothFactor *= (0.5f + stability * 0.5f); // Scale by stability (50-100%)
        
        // Calculate max allowed change per frame as percentage of current size
        // This prevents boxes from changing size too rapidly
        const float MAX_SIZE_CHANGE_RATE = 0.15f; // Max 15% change per frame
        float maxWidthDelta = lastBoxWidth * MAX_SIZE_CHANGE_RATE;
        float maxHeightDelta = lastBoxHeight * MAX_SIZE_CHANGE_RATE;
        
        // Clamp changes to avoid flickering
        float targetWidth = std::min(std::max(
            box_width,
            lastBoxWidth - maxWidthDelta),
            lastBoxWidth + maxWidthDelta
        );
        
        float targetHeight = std::min(std::max(
            box_height,
            lastBoxHeight - maxHeightDelta),
            lastBoxHeight + maxHeightDelta
        );
        
        // Apply smoothing with variable factor based on delta time and distance
        box_width = lastBoxWidth * (1.0f - boxSmoothFactor) + targetWidth * boxSmoothFactor;
        box_height = lastBoxHeight * (1.0f - boxSmoothFactor) + targetHeight * boxSmoothFactor;
        
        // Update stored values for next frame
        lastBoxWidth = box_width;
        lastBoxHeight = box_height;
        
        // Recenter the box after smoothing
        box_x = headScreenPos.x - (box_width / 2.0f);
        
        // Calculate opacity based on position confidence and distance
        float opacityBase = hasValidHistory ? std::min(1.0f, history->positionConfidence) : 0.7f;
        
        // Fade out very distant players slightly to reduce visual clutter
        float distanceOpacityFactor = 1.0f;
        if (distance > 2000.0f) {  // Only apply fade to very distant players
            distanceOpacityFactor = std::max(0.6f, 1.0f - ((distance - 2000.0f) / 10000.0f));
        }
        
        float opacity = opacityBase * distanceOpacityFactor;
        
        // ---- DIRECTLY RENDER ELEMENTS WITHOUT USING RENDERQUEUE ----
        
        // Render ESP box
        if (config.Visuals.Box) {
            ImU32 boxColor = IM_COL32(
                (int)(config.Visuals.BoxColor.x * 255),
                (int)(config.Visuals.BoxColor.y * 255),
                (int)(config.Visuals.BoxColor.z * 255),
                (int)(config.Visuals.BoxColor.w * 255 * opacity)
            );
            
            // Draw box with slightly rounded corners for better appearance
            drawList->AddRect(
                ImVec2(box_x, box_y), 
                ImVec2(box_x + box_width, box_y + box_height), 
                boxColor, 0.0f, 0, 1.5f  // Use consistent line thickness
            );
        }
        
        // Render health bar
        if (config.Visuals.Health) {
            float healthPerc = std::min(std::max(entity.health / 100.0f, 0.0f), 1.0f);
            float hb_height = box_height;
            float hb_width = 6.0f;
            float hb_x = box_x - hb_width - 4.0f;
            float hb_y = box_y;
            
            ImU32 col_top = IM_COL32(0, 255, 0, (int)(255 * opacity));
            ImU32 col_bottom = IM_COL32(255, 0, 0, (int)(255 * opacity));
            
            // Smooth health bar animation using thread-local storage
            float& animPerc = tlsAnimationState.animHealthPerc[entity.name];
            if (animPerc == 0.0f) animPerc = healthPerc; // Initialize
            
            // Calculate animation speed based on deltaTime
            float healthChangeSpeed = (animPerc > healthPerc) ? 
                EntityManager::ANIMATION_SPEED_FAST : // Faster for health decrease
                EntityManager::ANIMATION_SPEED_BASE;  // Normal speed for health increase
                
            float frameAdjustedSpeed = healthChangeSpeed * animationDeltaTime;
            
            // Apply smooth transition with frame-rate independent change
            animPerc += std::min(std::max(healthPerc - animPerc, -frameAdjustedSpeed), frameAdjustedSpeed);
            
            // Calculate animated filled height
            float anim_filled_height = hb_height * animPerc;
            float anim_empty_height = hb_height - anim_filled_height;
            
            // Draw health bar with animation
            drawList->AddRectFilledMultiColor(
                ImVec2(hb_x, hb_y + anim_empty_height),
                ImVec2(hb_x + hb_width, hb_y + hb_height),
                col_top, col_top, col_bottom, col_bottom
            );
            
            if (anim_empty_height > 0.0f) {
                ImU32 bgColor = IM_COL32(40, 40, 40, (int)(180 * opacity));
                drawList->AddRectFilled(
                    ImVec2(hb_x, hb_y),
                    ImVec2(hb_x + hb_width, hb_y + anim_empty_height),
                    bgColor
                );
            }
            
            drawList->AddRect(
                ImVec2(hb_x, hb_y), 
                ImVec2(hb_x + hb_width, hb_y + hb_height), 
                IM_COL32(0,0,0,(int)(180 * opacity)), 
                0
            );
        }
        
        // Render name
        float textY = box_y - 4;
        if (config.Visuals.Name) {
            ImU32 nameColor = IM_COL32(
                (int)(config.Visuals.NameColor.x * 255),
                (int)(config.Visuals.NameColor.y * 255),
                (int)(config.Visuals.NameColor.z * 255),
                (int)(config.Visuals.NameColor.w * 255 * opacity)
            );
            ImVec2 textSize = ImGui::CalcTextSize(entity.name.c_str());
            ImVec2 namePos(box_x + (box_width - textSize.x) / 2.0f, textY - textSize.y);
            
            // Add slight shadow for better readability
            drawList->AddText(
                ImVec2(namePos.x + 1, namePos.y + 1), 
                IM_COL32(0, 0, 0, (int)(120 * opacity)), 
                entity.name.c_str()
            );
            drawList->AddText(
                ImVec2(namePos.x, namePos.y), 
                nameColor, 
                entity.name.c_str()
            );
            textY = namePos.y;
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
            ImVec2 weaponPos(box_x + (box_width - weaponTextSize.x) / 2.0f, box_y + box_height + 2.0f);
            
            // Add slight shadow for better readability
            drawList->AddText(
                ImVec2(weaponPos.x + 1, weaponPos.y + 1), 
                IM_COL32(0, 0, 0, (int)(120 * opacity)), 
                entity.weaponName.c_str()
            );
            drawList->AddText(
                ImVec2(weaponPos.x, weaponPos.y), 
                weaponColor, 
                entity.weaponName.c_str()
            );
        }
        
        renderedEntities++;
    }
    
    auto end = std::chrono::steady_clock::now();
#ifdef ESP_LOGGING_ENABLED
    auto renderTime = std::chrono::duration_cast<std::chrono::microseconds>(end - frameStart).count();
    spdlog::info("ESP::Render: {} us, Entities: {}/{}", renderTime, renderedEntities, totalEntities);
#endif
}