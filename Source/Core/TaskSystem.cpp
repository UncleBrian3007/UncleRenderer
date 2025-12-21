#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "TaskSystem.h"
#include "Logger.h"
#include <algorithm>

// FTask implementation

FTask::FTask(FTaskFunction InFunction)
    : Function(std::move(InFunction))
{
}

void FTask::Execute()
{
    if (Function)
    {
        Function();
    }

    {
        std::lock_guard<std::mutex> Lock(CompletionMutex);
        bCompleted.store(true, std::memory_order_release);
    }
    CompletionCV.notify_all();
}

void FTask::Wait() const
{
    std::unique_lock<std::mutex> Lock(CompletionMutex);
    CompletionCV.wait(Lock, [this]() { return bCompleted.load(std::memory_order_acquire); });
}

// FTaskScheduler implementation

FTaskScheduler& FTaskScheduler::Get()
{
    static FTaskScheduler Instance;
    return Instance;
}

FTaskScheduler::~FTaskScheduler()
{
    Shutdown();
}

void FTaskScheduler::Initialize(uint32_t NumThreads)
{
    if (bIsRunning.load(std::memory_order_acquire))
    {
        LogWarning("TaskScheduler is already running");
        return;
    }

    if (NumThreads == 0)
    {
        NumThreads = std::thread::hardware_concurrency();
        if (NumThreads == 0)
        {
            NumThreads = 4; // Fallback to 4 threads
        }
    }

    // Reserve one thread for main thread, use rest for workers
    NumThreads = std::max(1u, NumThreads - 1);

    LogInfo("Initializing TaskScheduler with " + std::to_string(NumThreads) + " worker threads");

    bIsRunning.store(true, std::memory_order_release);

    WorkerThreads.reserve(NumThreads);
    for (uint32_t i = 0; i < NumThreads; ++i)
    {
        WorkerThreads.emplace_back(&FTaskScheduler::WorkerThreadFunction, this);
    }

    LogInfo("TaskScheduler initialized successfully");
}

void FTaskScheduler::Shutdown()
{
    if (!bIsRunning.load(std::memory_order_acquire))
    {
        return;
    }

    LogInfo("Shutting down TaskScheduler");

    bIsRunning.store(false, std::memory_order_release);
    QueueCV.notify_all();

    for (std::thread& Thread : WorkerThreads)
    {
        if (Thread.joinable())
        {
            Thread.join();
        }
    }

    WorkerThreads.clear();

    // Clear any remaining tasks
    {
        std::lock_guard<std::mutex> Lock(QueueMutex);
        while (!TaskQueue.empty())
        {
            TaskQueue.pop();
        }
    }

    LogInfo("TaskScheduler shut down complete");
}

FTaskRef FTaskScheduler::ScheduleTask(FTask::FTaskFunction Function)
{
    FTaskRef Task = std::make_shared<FTask>(std::move(Function));
    EnqueueTask(Task);
    return Task;
}

std::vector<FTaskRef> FTaskScheduler::ScheduleTaskBatch(const std::vector<FTask::FTaskFunction>& Functions)
{
    std::vector<FTaskRef> Tasks;
    Tasks.reserve(Functions.size());

    for (const auto& Function : Functions)
    {
        Tasks.push_back(ScheduleTask(Function));
    }

    return Tasks;
}

void FTaskScheduler::WaitForAll()
{
    // Wait until all tasks are complete
    while (true)
    {
        {
            std::lock_guard<std::mutex> Lock(QueueMutex);
            if (TaskQueue.empty() && ActiveTaskCount.load(std::memory_order_acquire) == 0)
            {
                break;
            }
        }
        std::this_thread::yield();
    }
}

void FTaskScheduler::WaitForTask(const FTaskRef& Task)
{
    if (Task)
    {
        Task->Wait();
    }
}

void FTaskScheduler::WorkerThreadFunction()
{
    while (bIsRunning.load(std::memory_order_acquire))
    {
        FTaskRef Task = GetNextTask();

        if (Task)
        {
            ActiveTaskCount.fetch_add(1, std::memory_order_acq_rel);
            Task->Execute();
            ActiveTaskCount.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
}

FTaskRef FTaskScheduler::GetNextTask()
{
    std::unique_lock<std::mutex> Lock(QueueMutex);
    QueueCV.wait(Lock, [this]()
    {
        return !TaskQueue.empty() || !bIsRunning.load(std::memory_order_acquire);
    });

    if (!bIsRunning.load(std::memory_order_acquire))
    {
        return nullptr;
    }

    if (TaskQueue.empty())
    {
        return nullptr;
    }

    FTaskRef Task = TaskQueue.front();
    TaskQueue.pop();
    return Task;
}

void FTaskScheduler::EnqueueTask(FTaskRef Task)
{
    {
        std::lock_guard<std::mutex> Lock(QueueMutex);
        TaskQueue.push(Task);
    }
    QueueCV.notify_one();
}

// FParallelFor implementation

void FParallelFor::Execute(uint32_t Start, uint32_t End, std::function<void(uint32_t)> Function)
{
    if (Start >= End)
    {
        return;
    }

    FTaskScheduler& Scheduler = FTaskScheduler::Get();
    if (!Scheduler.IsRunning())
    {
        // Fallback to serial execution if scheduler is not initialized
        for (uint32_t i = Start; i < End; ++i)
        {
            Function(i);
        }
        return;
    }

    const uint32_t WorkerCount = Scheduler.GetWorkerThreadCount();
    const uint32_t Range = End - Start;
    const uint32_t BatchSize = std::max(1u, Range / (WorkerCount * 2));

    ExecuteBatched(Start, End, BatchSize, Function);
}

void FParallelFor::ExecuteBatched(uint32_t Start, uint32_t End, uint32_t BatchSize, std::function<void(uint32_t)> Function)
{
    if (Start >= End)
    {
        return;
    }

    if (BatchSize == 0)
    {
        BatchSize = 1;
    }

    std::vector<FTask::FTaskFunction> Tasks;
    for (uint32_t i = Start; i < End; i += BatchSize)
    {
        const uint32_t BatchEnd = std::min(i + BatchSize, End);
        Tasks.push_back([i, BatchEnd, Function]()
        {
            for (uint32_t Index = i; Index < BatchEnd; ++Index)
            {
                Function(Index);
            }
        });
    }

    FTaskScheduler& Scheduler = FTaskScheduler::Get();
    std::vector<FTaskRef> ScheduledTasks = Scheduler.ScheduleTaskBatch(Tasks);

    // Wait for all tasks to complete
    for (const FTaskRef& Task : ScheduledTasks)
    {
        Scheduler.WaitForTask(Task);
    }
}
