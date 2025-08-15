#pragma once
#include <Overlay/Overlay.hpp>
#include <thread>
#include <chrono>

class SDK
{
public:

	bool Init();

	static SDK& Get()
	{
		static SDK instance;
		return instance;
	}

	void InitUpdateSdk()
	{
		if (m_UpdateThread.joinable()) return; // already running
		m_UpdateThread = std::thread([&]()
		{
			while (Globals::Running)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				// Update shit here
			}
		});
	}

	void Shutdown();

	// Optimized WorldToScreen using column-major matrix
	bool WorldToScreen(const Vector3& pos, Vector2& out, const Matrix& matrix, int width, int height);
	
	// Batch version for multiple world positions - useful for future optimizations
	bool WorldToScreenBatch(const Vector3* positions, Vector2* outputs, int count, const Matrix& matrix, int width, int height);

private:
	std::thread m_UpdateThread{};
};

inline SDK& sdk = SDK::Get();