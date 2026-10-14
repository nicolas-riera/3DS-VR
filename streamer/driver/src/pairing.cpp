#include "pairing.h"

#include <cstdio>

#include "log.h"

static std::wstring StorePath() {
	return AppDataDir() + L"\\paired_consoles.txt";
}

void PairingStore::Load() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_entries.clear();
	FILE* f = _wfopen(StorePath().c_str(), L"r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		unsigned long long id = 0, token = 0;
		int consumed = 0;
		if (sscanf(line, "%llx %llx %n", &id, &token, &consumed) >= 2) {
			std::string name = line + consumed;
			while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
			m_entries.push_back({id, token, name});
		}
	}
	fclose(f);
	Log("%zu paired console(s)", m_entries.size());
}

void PairingStore::Save() {
	FILE* f = _wfopen(StorePath().c_str(), L"w");
	if (!f) return;
	for (const Entry& e : m_entries)
		fprintf(f, "%016llx %016llx %s\n", (unsigned long long)e.id, (unsigned long long)e.token, e.name.c_str());
	fclose(f);
}

bool PairingStore::IsPaired(uint64_t consoleId, uint64_t token) {
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const Entry& e : m_entries)
		if (e.id == consoleId && e.token == token) return true;
	return false;
}

void PairingStore::Add(uint64_t consoleId, uint64_t token, const std::string& name) {
	std::lock_guard<std::mutex> lock(m_mutex);
	for (Entry& e : m_entries) {
		if (e.id == consoleId) {
			e.token = token;
			e.name = name;
			Save();
			return;
		}
	}
	m_entries.push_back({consoleId, token, name});
	Save();
}

void PairingStore::Clear() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_entries.clear();
	Save();
}

size_t PairingStore::Count() {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_entries.size();
}
