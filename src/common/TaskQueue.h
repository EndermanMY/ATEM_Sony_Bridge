#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "Logger.h"

// Owns one control worker. Shutdown rejects new tasks, discards queued work,
// and waits for the task already executing before destroying its dependencies.
class TaskQueue {
public:
    TaskQueue() : worker([this] { run(); }) {}
    ~TaskQueue() { stop(); }
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    bool post(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex);
        if(stopping) return false;
        tasks.push_back(std::move(task));
        changed.notify_one();
        return true;
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); stopping=true; tasks.clear(); }
        changed.notify_all();
        if(worker.joinable()) worker.join();
    }
private:
    void run() {
        for(;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock,[this] {return stopping || !tasks.empty();});
                if(stopping) return;
                task=std::move(tasks.front());tasks.pop_front();
            }
            try { task(); }
            catch(const std::exception& e) {Logger::error("Control task failed: {}",e.what());}
            catch(...) {Logger::error("Control task failed with unknown exception");}
        }
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> tasks;
    bool stopping=false;
    std::thread worker;
};
