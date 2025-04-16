#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <functional>
#include <future>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 2;
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒

// 线程池支持的模式
enum class ThreadPoolMode
{
    MODE_FIXED, // 固定数量的线程
    MODE_CACHED // 可变数量的线程
};

// 线程类型
class Thread
{
public:
    using ThreadFunc = std::function<void(int)>;

    // 线程构造函数
    Thread(ThreadFunc func)
        : func_(func), threadId_(generateId_++)
    {
    }
    // 线程析构函数
    ~Thread()
    {
    }
    // 启动线程
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_);
        if (t.joinable())
        {
            t.detach(); // 如果创建线程成功，则分离线程
        }
    }
    // 获取线程id
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_; // 线程函数
    int threadId_;    // 线程id
    static int generateId_;
};

int Thread::generateId_ = 0;

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造函数
    ThreadPool()
        : initThreadSize_(0), taskSize_(0), idleThreadSize_(0), curThreadSize_(0), taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD), threadSizeThreshHold_(THREAD_MAX_THRESHHOLD), poolMode_(ThreadPoolMode::MODE_FIXED), isPoolRunning_(false)
    {
    }

    // 线程池析构函数
    ~ThreadPool()
    {
        isPoolRunning_ = false;
        { // 等待线程池里面所有的线程返回  有两种状态：阻塞 & 正在执行任务中
            std::unique_lock<std::mutex> lock(taskQueMutex_);
            // 唤醒所有正在等待任务的线程，告诉它们任务队列已经没有任务了
            notEmpty_.notify_all();
        }
        // 等待直到线程池中的所有线程都执行完毕并退出
        std::unique_lock<std::mutex> lock(exitMutex_);
        exitCond_.wait(lock, [&]() -> bool
                       { return threads_.size() == 0; });
    }

    // 启动线程池
    void start(int initThreadSize = std::thread::hardware_concurrency()) // 初始化线程池的大小，默认为系统的并发线程数
    {
        // 设置线程池的运行状态
        isPoolRunning_ = true;
        // 记录初始线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize_;
        // 集中创建线程对象
        for (int i = 0; i < initThreadSize_; ++i)
        {
            // 创建thread线程对象时，把线程函数给到thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            // threads_.emplace_back(std::move(ptr));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
        }
        // 启动线程
        for (int i = 0; i < initThreadSize_; ++i)
        {
            threads_[i]->start(); // 执行一个线程函数
            idleThreadSize_++;    // 记录初始空闲线程的数量
        }
    }

    // 设置线程池工作模式
    void setMode(ThreadPoolMode mode)
    {
        if (checkRunningState())
            return;
        poolMode_ = mode;
    }

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshHold)
    {
        if (checkRunningState())
            return;
        taskQueMaxThreshHold_ = threshHold;
    }

    // 设置线程池cached模式的线程上限阈值
    void setThreadSizeThreshHold(int threshhold)
    {
        if (checkRunningState())
            return;
        if (ThreadPoolMode::MODE_CACHED == poolMode_)
        {
            threadSizeThreshHold_ = threshhold;
        }
    }

    // 给线程池提交任务
    // 使用可变参模板编程，让submitTask可以接收任意任务函数和任意数量的参数
    // pool.submitTask(sum1,10,20);
    // 返回值类型future<>
    template <typename Func, typename... Args>
    auto submitTask(Func &&func, Args &&...args) -> std::future<decltype(func(args...))>
    {
        // 打包任务，放入任务队列里面
        using RType = decltype(func(args...)); // 获取函数返回值类型
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        std::future<RType> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        // 用户提交任务，最长不能阻塞超过1s，否则判断提交任务失败，返回
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
                               [&]() -> bool
                               { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; }))
        {
            // 表示notFull_等待1s种，条件依然没有满足
            std::cerr << "task queue is full, submit task fail." << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>(
                []() -> RType
                { return RType(); });
            (*task)();
            return task->get_future();
        }

        // 如果有空余，把任务放入任务队列中
        // taskQue_.emplace(sp);
        // using Task = std::function<void()>;
        taskQue_.emplace([task]()
                         { (*task)(); });
        taskSize_++;

        // 因为新放了任务，任务队列肯定不空了，在notEmpty_上进行通知，赶快分配线程执行任务
        notEmpty_.notify_all();

        // cached模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << ">>> create new thread..." << std::endl;

            // 创建新的线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // 启动线程
            threads_[threadId]->start();
            // 修改线程个数相关的变量
            curThreadSize_++;
            idleThreadSize_++;
        }

        // 返回任务的Result对象
        return result;
    }

    ThreadPool(const ThreadPool &) = delete;            // 不允许线程池拷贝
    ThreadPool &operator=(const ThreadPool &) = delete; // 不允许线程池赋值

private:
    // 定义线程函数
    void threadFunc(int threadId)
    {
        auto lastTime = std::chrono::high_resolution_clock().now(); // 记录上一次线程空闲的时间

        // 所有任务必须全部执行完，线程池才可以回收所有线程资源
        for (;;)
        {
            Task task;
            {
                // 获取锁
                std::unique_lock<std::mutex> lock(taskQueMutex_);

                std::cout << "tid:" << std::this_thread::get_id() << " 尝试获取任务..." << std::endl;

                // cached模式下，有可能已经创建了很多的线程，但是空闲时间超过60s，应该把多余的线程结束回收掉
                // 超过initThreadSize_数量的线程，要进行回收
                // 当前时间 - 上一次线程执行的时间 > 60s
                // 每1s返回一次      怎么区分：超时返回？还是有任务待执行返回
                // 锁 + 双重判断
                while (taskQueue_.size() == 0)
                {
                    // 首先检查线程池是否正在关闭
                    if (!isPoolRunning_)
                        // ---- 退出路径 1：线程池关闭 ----
                        // 在获取 exitMutex_ 之前必须释放 taskQueMutex_
                        lock.unlock(); // 显式解锁
                    {                  // 为 exitMutex_ 创建新的作用域
                        std::unique_lock<std::mutex> threadsLock(exitMutex_);
                        threads_.erase(threadId); // std::this_thread::getid()
                        curThreadSize_--;
                        std::cout << "threadid:" << std::this_thread::get_id() << " exit (shutdown)!"
                                  << std::endl;
                        exitCond_.notify_all(); // 通知析构函数
                        return;                 // 线程函数结束就是当前线程结束
                    } // exitMutex_ 释放
                    // 根据模式处理等待（仍然持有 taskQueMutex_）
                    if (ThreadPoolMode::MODE_CACHED == poolMode_)
                    {
                        // 条件变量，超时返回
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto now = std::chrono::high_resolution_clock().now();                       // 当前时间
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime); // 当前时间 - 上一次线程执行的时间
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                            {
                                // ---- 退出路径 2：Cached 模式超时 ----
                                // 在获取 exitMutex_ 之前必须释放 taskQueMutex_
                                lock.unlock(); // 显式解锁
                                {              // 为 exitMutex_ 创建新的作用域
                                    std::unique_lock<std::mutex> threadsLock(exitMutex_);
                                    // 开始回收当前线程
                                    // 记录线程数量的修改
                                    // 把线程对象从线程列表中删除
                                    threads_.erase(threadId); // 不能传入是std::this_thread::get_id()
                                    curThreadSize_--;
                                    idleThreadSize_--;

                                    std::cout << "threadid:" << std::this_thread::get_id() << " exit!(timeout)!"
                                              << std::endl;
                                    exitCond_.notify_all(); // 通知析构函数
                                } // exitMutex_ 释放
                                return; // 线程函数结束就是当前线程结束
                            }
                            // 如果超时但未退出，循环继续（持有 taskQueMutex_）
                        }
                        // 如果被 notify 或伪唤醒，循环继续
                    }
                    else // MODE_FIXED
                    {
                        // 等待任务队列不空
                        notEmpty_.wait(lock); // 无限期等待（持有 taskQueMutex_）
                        // 等待结束后，重新检查循环条件
                    }
                } // 结束 while (taskQueue_.size() == 0)

                // --- 如果到达这里，说明任务队列非空 ---
                // 仍然持有循环或初始获取的 taskQueMutex_ 锁
                idleThreadSize_--;

                std::cout << "tid:" << std::this_thread::get_id() << " 获取任务成功..." << std::endl;

                // 从任务队列中取出任务
                task = taskQueue_.front();
                taskQueue_.pop();
                taskSize_--;

                // 如果队列还有任务，继续通知其他线程执行任务
                if (taskQueue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }
                // 取出一个任务，通知生产者线程可以继续提交任务
                notFull_.notify_all();
            } // 线程取完任务就应该释放锁

            // 线程执行任务
            if (task != nullptr)
            {
                // task->run(); // 执行任务;把任务的返回值通过setVal方法设置到Result中
                // task->exec();
                task(); // 执行function<void()>
            }

            idleThreadSize_++;
            auto lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
        }
    }

    // 检查线程池的运行状态
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    // std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表

    int initThreadSize_;             // 初始的线程数量
    int threadSizeThreshHold_;       // 线程数量上限阈值
    std::atomic_int curThreadSize_;  // 记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; // 记录空闲线程的数量

    // Task任务 -> 函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQueue_; // 任务队列
    std::atomic_int taskSize_;   // 任务的数量
    int taskQueMaxThreshHold_;   // 任务队列数量上限阈值

    std::mutex taskQueMutex_;          // 保证任务队列的线程安全
    std::condition_variable notFull_;  // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::mutex exitMutex_;             // 用于保护线程列表和退出条件
    std::condition_variable exitCond_; // 等到线程资源全部回收

    ThreadPoolMode poolMode_;        // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif