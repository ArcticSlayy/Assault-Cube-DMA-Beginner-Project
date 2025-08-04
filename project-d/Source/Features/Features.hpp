#pragma once
#include <vector>
#include <string>
#include <SDK.hpp>
#include "Aimbot/Aimbot.hpp"


struct EntityData {
    char name[260];
    int health;
    int team;
    int score;
    int kills;
    int deaths;
    Vector3 headPosition;
    Vector3 footPosition;
};

namespace EntityManager {
    inline std::vector<EntityData> entities;
}

class Features
{
public:

	void InitAimbot()
	{
		thread([&]()
		{
			while (Globals::Running)
			{
				this_thread::sleep_for(chrono::milliseconds(1));

				aim.Update();
			}
		}).detach();
	}

	static Features& Get()
	{
		static Features instance;
		return instance;
	}

	bool Init()
	{
		//InitAimbot();

		return true;
	}
};

inline Features& features = Features::Get();