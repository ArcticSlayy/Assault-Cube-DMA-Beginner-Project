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

	// WorldToScreen using column-major float[16] (mythos style)
	bool WorldToScreen(const Vector3& pos, Vector2& out, const Matrix& matrix, int width, int height);
};

inline SDK& sdk = SDK::Get();