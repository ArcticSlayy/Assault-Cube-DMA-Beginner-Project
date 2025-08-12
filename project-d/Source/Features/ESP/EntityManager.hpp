    // Update control
    extern std::atomic<float> updateInterval;
    
    // Functions
    void StartEntityUpdateThread();
    void StopEntityUpdateThread();
    void UpdateEntities();
}