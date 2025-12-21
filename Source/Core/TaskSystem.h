#pragma once

#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

// Forward declarations
class FTaskScheduler;

/**
 * A task represents a unit of work that can be executed asynchronously.
 * Tasks can have dependencies on other tasks.
 */
class FTask
{
public:
    using FTaskFunction = std::function<void()>;

    FTask() = default;
    explicit FTask(FTaskFunction InFunction);

    void Execute();
    bool IsComplete() const { return bCompleted.load(std::memory_order_acquire); }
    void Wait() const;

private:
    friend class FTaskScheduler;
    
    FTaskFunction Function;
    std::atomic<bool> bCompleted{ false };
    mutable std::mutex CompletionMutex;
    mutable std::condition_variable CompletionCV;
};

using FTaskRef = std::shared_ptr<FTask>;

/**
 * Task scheduler manages a pool of worker threads and distributes tasks among them.
 * Supports task dependencies and parallel execution.
 */
class FTaskScheduler
{
public:
    static FTaskScheduler& Get();

    void Initialize(uint32_t NumThreads = 0);
    void Shutdown();

    /**
     * Schedule a task for asynchronous execution.
     * @param Function The function to execute
     * @return Shared pointer to the task for tracking completion
     */
    FTaskRef ScheduleTask(FTask::FTaskFunction Function);

    /**
     * Schedule multiple tasks for parallel execution.
     * @param Functions Array of functions to execute
     * @return Vector of task references
     */
    std::vector<FTaskRef> ScheduleTaskBatch(const std::vector<FTask::FTaskFunction>& Functions);

    /**
     * Wait for all scheduled tasks to complete.
     */
    void WaitForAll();

    /**
     * Wait for a specific task to complete.
     */
    void WaitForTask(const FTaskRef& Task);

    /**
     * Get the number of worker threads.
     */
    uint32_t GetWorkerThreadCount() const { return static_cast<uint32_t>(WorkerThreads.size()); }

    /**
     * Check if the scheduler is running.
     */
    bool IsRunning() const { return bIsRunning.load(std::memory_order_acquire); }

private:
    FTaskScheduler() = default;
    ~FTaskScheduler();

    // Prevent copying
    FTaskScheduler(const FTaskScheduler&) = delete;
    FTaskScheduler& operator=(const FTaskScheduler&) = delete;

    void WorkerThreadFunction();
    FTaskRef GetNextTask();
    void EnqueueTask(FTaskRef Task);

private:
    std::vector<std::thread> WorkerThreads;
    std::queue<FTaskRef> TaskQueue;
    std::mutex QueueMutex;
    std::condition_variable QueueCV;
    std::atomic<bool> bIsRunning{ false };
    std::atomic<uint32_t> ActiveTaskCount{ 0 };
};

/**
 * Helper class for parallel for loops.
 */
class FParallelFor
{
public:
    /**
     * Execute a function in parallel over a range.
     * @param Start Starting index (inclusive)
     * @param End Ending index (exclusive)
     * @param Function Function to execute for each index (receives index as parameter)
     */
    static void Execute(uint32_t Start, uint32_t End, std::function<void(uint32_t)> Function);

    /**
     * Execute a function in parallel over a range with specified batch size.
     * @param Start Starting index (inclusive)
     * @param End Ending index (exclusive)
     * @param BatchSize Number of iterations per task
     * @param Function Function to execute for each index
     */
    static void ExecuteBatched(uint32_t Start, uint32_t End, uint32_t BatchSize, std::function<void(uint32_t)> Function);
};
