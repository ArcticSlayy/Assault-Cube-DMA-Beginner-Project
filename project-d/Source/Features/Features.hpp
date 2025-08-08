#pragma once
#include <vector>
#include <mutex>
#include "Aimbot/Aimbot.hpp"
#include <SDK.hpp>

struct EntityData {
    std::string name;
    int health;
    int team;
    int score;
    int kills;
    int deaths;
    Vector3 headPosition;
    Vector3 footPosition;
    int weaponClass; // Weapon type index
    std::string weaponName; // Weapon name string
};

namespace EntityManager {
    extern std::vector<EntityData> entities;
    extern std::mutex entities_mutex;
    void StartEntityUpdateThread();
}

class Features
{
public:

	void InitAimbot()
	{
		std::thread([&]()
		{
			while (Globals::Running)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
		InitAimbot();
		return true;
	}
};

inline Features& features = Features::Get();