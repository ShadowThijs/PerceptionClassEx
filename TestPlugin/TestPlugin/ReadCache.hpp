#pragma once

#include "Plugin.h"
#include <vector>
#include <map>
#include <queue>
#include <mutex>
#include <shared_mutex> 

struct ReadKey {
    uintptr_t address;
    size_t size;

    bool operator<(const ReadKey& other) const {
        return std::tie(address, size) < std::tie(other.address, other.size);
    }
};

struct CachedBlock {
    std::vector<uint8_t> data;
    // optionally timestamp, validity flags, etc.
};

extern std::map<ReadKey, CachedBlock> g_cache;
extern std::shared_mutex g_cache_mutex;

enum class JobType {
    Read,
    Write,
    OpenProcess,
    CloseProcess
};

struct Job {
    JobType type;
    uintptr_t address;
    std::vector<uint8_t> data; // for write
    //std::string processName;   // for open_process
	uint32_t pid;              // for open_process
};

extern std::queue<Job> g_jobs;
extern std::mutex g_jobs_mutex;

extern std::atomic<bool> g_workerRunning;
extern std::thread* g_workerThread;

//void worker_thread();
bool StartWorker();
void StopWorker();