#include "ReadCache.hpp"
#include "WebSocketServer.hpp"

std::map<ReadKey, CachedBlock> g_cache;
std::shared_mutex              g_cache_mutex;

std::queue<Job>                g_jobs;
std::mutex                     g_jobs_mutex;
std::condition_variable        g_jobs_cv;

std::atomic<bool> g_workerRunning{ false };
std::thread* g_workerThread = nullptr;

static inline void ClearCache() {{std::unique_lock<std::shared_mutex> lock(g_cache_mutex);g_cache.clear();}};

void worker_thread()
{
    LOG("STARTING WORKER THREAD");

    while (g_workerRunning) {

        Job job;
        bool haveJob = false;

        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            if (!g_jobs.empty()) {
                job = std::move(g_jobs.front());
                g_jobs.pop();
                haveJob = true;
            }
        }

        if (!haveJob) {
            // No work to do right now, back off a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::string cmd, response;

        switch (job.type) {

        case JobType::OpenProcess: {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"cmd\":\"open_process\",\"pid\":%u}", job.pid);
            std::string cmd(buf), response;
            SendWebSocketCommand(cmd, response, 1000);
            break;
        }

        case JobType::CloseProcess: {
            cmd = "{\"cmd\":\"close_process\"}";
            SendWebSocketCommand(cmd, response, 500);
            break;
        }

        case JobType::Read: {
            // job.read.address = requested start address
            // job.read.size    = requested size (may cross pages)

            const uintptr_t req_addr = job.read.address;
            const size_t    req_size = job.read.size;



            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"cmd\":\"read\",\"pid\":%u,\"address\":%llu,\"size\":%llu}",
                job.pid,
                (unsigned long long)req_addr,
                (unsigned long long)req_size);
            cmd = buf;

            if (!SendWebSocketCommand(cmd, response, 200)) {
                LOG(L"Read job failed: SendWebSocketCommand returned false (pid=%u, addr=0x%llX, size=%llu)",
                    job.pid, (unsigned long long)req_addr, (unsigned long long)req_size);
                ClearCache();
                break;
            }
            if (response.empty()) {
                LOG(L"Read job failed: empty response (pid=%u, addr=0x%llX, size=%llu)",
                    job.pid, (unsigned long long)req_addr, (unsigned long long)req_size);
                ClearCache();
                break;
            }

            auto pos = response.find("\"data\":\"");
            if (pos == std::string::npos) {
                LOG(L"Read job failed: no data field in response: %hs", response.c_str());
                ClearCache();
                break;
            }

            pos += 8;
            auto end = response.find("\"", pos);
            if (end != std::string::npos) {
                std::string hex = response.substr(pos, end - pos);
                std::vector<uint8_t> bytes;
                bytes.reserve(hex.size() / 2);
                for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                    unsigned int b = 0;
                    sscanf(hex.c_str() + i, "%2x", &b);
                    bytes.push_back((uint8_t)b);
                }

                if (!bytes.empty()) {

                    // Clamp to what we actually requested, just in case
                    size_t total_len = bytes.size();
                    if (total_len > req_size)
                        total_len = req_size;

                    size_t pos_bytes = 0;
                    const auto now = std::chrono::steady_clock::now();

                    while (pos_bytes < total_len) {
                        uintptr_t cur_addr = req_addr + pos_bytes;
                        uintptr_t page_base = PAGE_BASE(cur_addr);
                        size_t    offset =
                            static_cast<size_t>(cur_addr - page_base);

                        size_t page_cap = PAGE_SIZE - offset;
                        size_t remaining = total_len - pos_bytes;
                        size_t to_copy =
                            (remaining < page_cap) ? remaining : page_cap;

                        ReadKey key(page_base);

                        {
                            std::unique_lock<std::shared_mutex> lock(g_cache_mutex);
                            CachedBlock& block = g_cache[key];

                            // Copy into the page buffer at the correct offset
                            memcpy(block.data + offset,
                                bytes.data() + pos_bytes,
                                to_copy);

                            block.timestamp = now;

                            // Merge [offset, offset+to_copy) into existing valid window
                            if (block.valid_length == 0) {
                                // No previous valid data in this page
                                block.valid_start = offset;
                                block.valid_length = to_copy;
                            }
                            else {
                                size_t old_start = block.valid_start;
                                size_t old_end = block.valid_start + block.valid_length;
                                size_t new_start = offset;
                                size_t new_end = offset + to_copy;

                                size_t merged_start =
                                    (old_start < new_start) ? old_start : new_start;
                                size_t merged_end =
                                    (old_end > new_end) ? old_end : new_end;

                                if (merged_end > PAGE_SIZE)
                                    merged_end = PAGE_SIZE;

                                block.valid_start = merged_start;
                                block.valid_length = merged_end - merged_start;
                            }
                        }

                        pos_bytes += to_copy;
                    }
                }
            }
                
            
            break;
        }

        case JobType::Write: {
            static const char hex_digits[] = "0123456789ABCDEF";
            std::string hex;
            hex.reserve(job.data.size() * 2);
            for (auto b : job.data) {
                hex.push_back(hex_digits[(b >> 4) & 0xF]);
                hex.push_back(hex_digits[b & 0xF]);
            }

            char bufw[128];
            snprintf(bufw, sizeof(bufw),
                "{\"cmd\":\"write\",\"pid\":%u,\"address\":%llu,\"data\":\"%s\"}",
                job.pid,
                (unsigned long long)job.write.address,
                hex.c_str());

            cmd = bufw;
            SendWebSocketCommand(cmd, response, 100);
            break;
        }
        }
    }

    LOG("LEAVING WORKER THREAD");
}

bool StartWorker()
{
    bool expected = false;
    if (!g_workerRunning.compare_exchange_strong(expected, true))
        return false;

    g_workerThread = new std::thread(worker_thread);

    return g_workerThread != nullptr;
}

void StopWorker()
{
    bool expected = true;
    if (!g_workerRunning.compare_exchange_strong(expected, false))
        return;

    if (g_workerThread && g_workerThread->joinable())
        g_workerThread->join();

    delete g_workerThread;
    g_workerThread = nullptr;
}
