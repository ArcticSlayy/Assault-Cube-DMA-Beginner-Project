#pragma once
#include <Overlay/Overlay.hpp>

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
		thread([&]()
		{
			while (Globals::Running)
			{
				this_thread::sleep_for(chrono::milliseconds(1));
				// Update shit here
			}
		}).detach();
	}

	// Optimized WorldToScreen using column-major matrix
	bool WorldToScreen(const Vector3& pos, Vector2& out, const Matrix& matrix, int width, int height);
	
	// Batch version for multiple world positions - useful for future optimizations
	bool WorldToScreenBatch(const Vector3* positions, Vector2* outputs, int count, const Matrix& matrix, int width, int height);
};

inline SDK& sdk = SDK::Get();