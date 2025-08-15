#pragma once

class c_exception_handler
{
public:
	static void log_file(const std::string& output)
	{
		const std::string file_name = "crashlog.txt";

		if (std::filesystem::exists(file_name))
		{
			if (!std::filesystem::remove(file_name))
			{
				LOG_ERROR("Failed to remove existing file '%s'", file_name.c_str());
				return;
			}
		}

		std::ofstream output_file(file_name);
		if (!output_file)
		{
			LOG_ERROR("Failed to open '%s' for writing", file_name.c_str());
			return;
		}

		output_file << output << std::endl;
	}

	static long __stdcall handler(EXCEPTION_POINTERS* info)
	{
		const auto code = info->ExceptionRecord->ExceptionCode;
		if (code != EXCEPTION_ACCESS_VIOLATION)
			return 0;

		const auto address = info->ExceptionRecord->ExceptionAddress;
		MEMORY_BASIC_INFORMATION memory_info{};
		uintptr_t alloc_base{};

		if (VirtualQuery(address, &memory_info, sizeof(memory_info)))
			alloc_base = reinterpret_cast<uintptr_t>(memory_info.AllocationBase);

		char buf[128]{};
		_snprintf_s(buf, sizeof(buf) - 1, _TRUNCATE, "App crashed at usermode.exe+0x%04llx", static_cast<unsigned long long>(static_cast<uintptr_t>(info->ContextRecord->Rip) - alloc_base));
		log_file(std::string(buf));

		return 0;
	}

	static bool setup()
	{
		const auto handle = AddVectoredExceptionHandler(false, handler);
		if (!handle)
		{
			LOG_ERROR("Failed to add vectored exception handler");
			return false;
		}

		return true;
	}
};