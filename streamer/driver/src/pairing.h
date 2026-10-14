#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Consoles the user accepted, in %APPDATA%\3DS VR\paired_consoles.txt.
class PairingStore {
public:
	void Load();
	bool IsPaired(uint64_t consoleId, uint64_t token);
	void Add(uint64_t consoleId, uint64_t token, const std::string& name);
	void Clear();
	size_t Count();

private:
	struct Entry {
		uint64_t id, token;
		std::string name;
	};
	void Save();
	std::mutex m_mutex;
	std::vector<Entry> m_entries;
};
