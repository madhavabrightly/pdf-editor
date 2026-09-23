#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace rpfg {

// A fixed-size pool serving a priority work queue.
//
// Lower `priority` values run first; equal priorities run in submission order.
// Workers are handed their index on start and stop so callers can attach
// per-thread resources (a MuPDF context per thread, here) without resorting to
// thread_local bookkeeping of their own. workerIndex() lets a running task
// discover which worker it is on.
class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool(std::size_t threadCount,
               std::string name,
               std::function<void(int)> onWorkerStart,
               std::function<void(int)> onWorkerStop);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(int priority, Task task);
    void stop();

    std::size_t threadCount() const { return workers_.size(); }
    std::size_t queued() const;

    static int workerIndex();

private:
    struct Entry {
        int priority;
        std::uint64_t sequence;
        Task task;
    };

    struct EntryOrder {
        bool operator()(const Entry& lhs, const Entry& rhs) const {
            if (lhs.priority != rhs.priority) {
                return lhs.priority > rhs.priority;
            }
            return lhs.sequence > rhs.sequence;
        }
    };

    void workerLoop(int index);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::priority_queue<Entry, std::vector<Entry>, EntryOrder> queue_;
    std::vector<std::thread> workers_;
    std::function<void(int)> onWorkerStart_;
    std::function<void(int)> onWorkerStop_;
    std::string name_;
    std::uint64_t nextSequence_ = 0;
    bool stopping_ = false;

    static thread_local int workerIndex_;
};

}  // namespace rpfg
