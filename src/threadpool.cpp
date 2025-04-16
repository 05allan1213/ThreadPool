#include "threadpool.h"

const int TASK_MAX_THRESHHOLD = INT32_MAX;
const int THREAD_MAX_THRESHHOLD = 2;
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒
//////////// 线程池类实现 ////////////////
// 线程池构造函数
ThreadPool::ThreadPool()
    : initThreadSize_(0), taskSize_(0), idleThreadSize_(0), curThreadSize_(0), taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD), threadSizeThreshHold_(THREAD_MAX_THRESHHOLD), poolMode_(ThreadPoolMode::MODE_FIXED), isPoolRunning_(false)
{
}

// 线程池析构函数
ThreadPool::~ThreadPool()
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

// 设置线程池工作模式
void ThreadPool::setMode(ThreadPoolMode mode)
{
    if (checkRunningState())
        return;
    poolMode_ = mode;
}

// 设置task任务队列上限阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshHold)
{
    if (checkRunningState())
        return;
    taskQueMaxThreshHold_ = threshHold;
}

// 设置线程池中线程数量上限阈值
void ThreadPool::setThreadSizeThreshHold(int threshhold)
{
    if (checkRunningState())
        return;
    if (ThreadPoolMode::MODE_CACHED == poolMode_)
    {
        threadSizeThreshHold_ = threshhold;
    }
}

// 给线程池提交任务  用户调用该接口，传入任务对象，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMutex_);
    // 等待线程任务队列不满
    // 但用户提交任务不能一直阻塞，最多阻塞1s，否则判断提交任务失败，返回
    if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]() -> bool
                           { return taskQueue_.size() < taskQueMaxThreshHold_; }))
    {
        // 说明等待1s，线程任务队列仍旧为满，提交任务失败
        std::cerr << "task queue is full, submit task fail." << std::endl;
        // return task->getResult(); // 线程执行完task，task对象就被析构掉了
        return Result(sp, false);
    }
    // 向任务队列中放入任务
    taskQueue_.emplace(sp);
    taskSize_++;
    // 通知线程池分配线程处理任务
    notEmpty_.notify_all();

    // cached模式 任务处理比较紧急 场景：小而快的任务
    // 根据任务数量和空闲线程数量，判断是否需要创建新的线程
    if (ThreadPoolMode::MODE_CACHED == poolMode_ && idleThreadSize_ < taskSize_ && curThreadSize_ < threadSizeThreshHold_)
    {
        std::cout << ">>> create new thread..." << std::endl;

        // 创建一个新线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        // threads_.emplace_back(std::move(ptr));
        threads_.emplace(threadId, std::move(ptr));
        // 启动线程
        threads_[threadId]->start();
        // 记录线程数量的修改
        idleThreadSize_++;
        curThreadSize_++;
    }

    // 返回任务结果Result对象
    // return task->getResult();
    return Result(sp);
}

// 启动线程池
void ThreadPool::start(int initThreadSize)
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

// 线程池中的线程函数  线程池中的所有线程从任务队列中取任务并消费
void ThreadPool::threadFunc(int threadId) // 线程函数返回，相应的线程就结束了
{
    auto lastTime = std::chrono::high_resolution_clock().now(); // 记录上一次线程空闲的时间

    // 所有任务必须全部执行完，线程池才可以回收所有线程资源
    for (;;)
    {
        std::shared_ptr<Task> task;
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

                // if (!isPoolRunning_)
                // {
                //     threads_.erase(threadId); // std::this_thread::getid()
                //     std::cout << "threadid:" << std::this_thread::get_id() << " exit!"
                //               << std::endl;
                //     exitCond_.notify_all();
                //     return; // 线程函数结束就是当前线程结束
                // }
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
            task->exec();
        }

        idleThreadSize_++;
        auto lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
    }
}

// 检查线程池的运行状态
bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

//////////////// 线程方法实现 ////////////////
int Thread::generateId_ = 0;

// 线程构造函数
Thread::Thread(ThreadFunc func)
    : func_(func), threadId_(generateId_++)
{
}

// 线程析构函数
Thread::~Thread()
{
}

// 启动线程
void Thread::start()
{
    // 创建一个线程来执行一个线程函数
    std::thread t(func_, threadId_);
    if (t.joinable())
    {
        t.detach(); // 如果创建线程成功，则分离线程
    }
}

// 获取线程id
int Thread::getId() const
{
    return threadId_;
}

////////// Semaphore的实现 ////////////////
Semaphore::Semaphore(int limit)
    : resLimit_(limit), isExit_(false)
{
}

Semaphore::~Semaphore()
{
    isExit_ = true; // 退出信号量
}

// 获取一个信号量资源
void Semaphore::wait()
{
    if (isExit_) // 如果isExit_标志为true，则直接返回，不进行等待
        return;
    std::unique_lock<std::mutex> lock(mtx_);
    // 等待信号量有资源，没有资源的话，会阻塞当前线程
    cond_.wait(lock, [&]() -> bool
               { return resLimit_ > 0; });
    --resLimit_;
}

// 增加一个信号量资源
void Semaphore::post()
{
    if (isExit_)
        return;
    std::unique_lock<std::mutex> lock(mtx_);
    ++resLimit_;
    cond_.notify_all();
}

/////////////////  Task方法实现
Task::Task()
    : result_(nullptr)
{
}

// 执行任务并设置结果值
void Task::exec()
{
    if (result_ != nullptr)
    {
        result_->setVal(run()); // 这里发生多态调用
    }
}

// 设置任务所属的结果对象
void Task::setResult(Result *res)
{
    result_ = res;
}

////////// Result方法的实现 ///////////////
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : task_(task), isValid_(isValid)
{
    task_->setResult(this); // 在 Result 对象创建时，就将自己(this)注册给 Task
}

// 用户调用这个方法获取task的返回值
Any Result::get() // 用户调用的
{
    if (!isValid_)
    {
        return "";
    }
    sem_.wait(); // task任务如果没有执行完，这里会阻塞用户的线程
    return std::move(any_);
}

// 设置任务执行完的返回值
void Result::setVal(Any any) // 谁调用的呢？？？
{
    // 存储task的返回值
    this->any_ = std::move(any);
    sem_.post(); // 已经获取的任务的返回值，增加信号量资源
}