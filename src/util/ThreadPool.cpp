#include "util/ThreadPool.h"

#include <windows.h>

#include <utility>

#include "util/Logger.h"

namespace rpfg {

thread_local int ThreadPool::workerIndex_ = -1;

ThreadPool::ThreadPool(std::size_t threadCount,
                       std::string name,
                       std::function<void(int)> onWorkerStart,
                       std::function<void(int)> onWorkerStop)
    : onWorkerStart_(std::move(onWorkerStart)),
      onWorkerStop_(std::move(onWorkerStop)),
      name_(std::move(name)) {
    const std::size_t count = threadCount == 0 ? 1 : threadCount;
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this, index = static_cast<int>(i)]() { workerLoop(index); });
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::submit(int priority, Task task) {
    if (!task) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) {
            return;
        }
        queue_.push(Entry{priority, nextSequence_++, std::move(task)});
    }
    wake_.notify_one();
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    std::lock_guard<std::mutex> guard(mutex_);
    std::priority_queue<Entry, std::vector<Entry>, EntryOrder> empty;
    queue_.swap(empty);
}

std::size_t ThreadPool::queued() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return queue_.size();
}

int ThreadPool::workerIndex() {
    return workerIndex_;
}

void ThreadPool::workerLoop(int index) {
    workerIndex_ = index;

    const std::wstring wideName = std::wstring(name_.begin(), name_.end()) + L"-" +
                                  std::to_wstring(index);
    SetThreadDescription(GetCurrentThread(), wideName.c_str());

    if (onWorkerStart_) {
        onWorkerStart_(index);
    }

    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (stopping_) {
                    break;
                }
                continue;
            }
            task = std::move(const_cast<Entry&>(queue_.top()).task);
            queue_.pop();
        }

        try {
            task();
        } catch (const std::exception& ex) {
            logError(std::string("worker task threw: ") + ex.what());
        } catch (...) {
            logError("worker task threw an unknown exception");
        }
    }

    if (onWorkerStop_) {
        onWorkerStop_(index);
    }
    workerIndex_ = -1;
}

}  // namespace rpfg
