#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

// Any类型：可以接收任意数据的类型
class Any
{
public:
    Any() = default;
    ~Any() = default;
    Any(const Any &) = delete;
    Any &operator=(const Any &) = delete;
    Any(Any &&) = default;
    Any &operator=(Any &&) = default;

    // 这个构造函数可以让Any类型接收任意其它的数据
    template <typename T> // T:int    Derive<int>
    Any(T data) : base_(std::make_unique<Derive<T>>(data))
    {
    }

    // 这个方法能把Any对象里面存储的data数据提取出来
    template <typename T>
    T cast_()
    {
        // 怎么从base_找到它所指向的Derive对象，从它里面取出data成员变量
        // 基类指针 -> 派生类指针   RTTI
        Derive<T> *pd = dynamic_cast<Derive<T> *>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch!";
        }
        return pd->data_;
    }

private:
    // 基类类型
    class Base
    {
    public:
        virtual ~Base() = default;
    };

    // 派生类类型
    template <typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data)
        {
        }
        T data_; // 保存了任意的其它类型
    };

private:
    // 定义一个基类的指针
    std::unique_ptr<Base> base_;
};

// 实现一个信号量类
class Semaphore
{
public:
    Semaphore(int limit = 0);
    ~Semaphore();

    // 获取一个信号量资源
    void wait();

    // 增加一个信号量资源
    void post();

private:
    std::mutex mtx_;
    std::condition_variable cond_;
    int resLimit_;
    std::atomic_bool isExit_;
};

// 线程池支持的模式
enum class ThreadPoolMode
{
    MODE_FIXED, // 固定数量的线程
    MODE_CACHED // 可变数量的线程
};

// Task类型的前置声明
class Result;

// 任务抽象基类
class Task
{
public:
    Task();
    ~Task() = default;

    // 执行任务并设置结果值
    void exec();
    // 设置任务所属的结果对象
    void setResult(Result *res);

    // 用户可以自定义任意任务类型，从Task继承，重写run方法，实现自定义任务处理
    virtual Any run() = 0;

private:
    Result *result_; // Result对象的生命周期 > Task对象的生命周期
};

// 实现接收提交到线程池的task任务执行完成后的返回值类型Result
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    // setVal方法，设置任务执行完的返回值
    void setVal(Any any);

    // get方法，用户调用这个方法获取task的返回值
    Any get();

private:
    Any any_;                    // 存储任务的返回值
    Semaphore sem_;              // 线程通信信号量
    std::shared_ptr<Task> task_; // 指向对应获取返回值的任务对象
    std::atomic_bool isValid_;   // 返回值是否有效
};

// 线程类型
class Thread
{
public:
    using ThreadFunc = std::function<void(int)>;

    // 线程构造函数
    Thread(ThreadFunc func);
    // 线程析构函数
    ~Thread();
    // 启动线程
    void start();
    // 获取线程id
    int getId() const;

private:
    ThreadFunc func_; // 线程函数
    int threadId_;    // 线程id
    static int generateId_;
};

/*
example:
    ThreadPool pool;
    pool.start(4);

    class MyTask : public Task
    {
        public:
            void run() override
            { 线程代码... }
    };

    pool.submitTask(std::make_shared<MyTask>());
*/

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造函数
    ThreadPool();

    // 线程池析构函数
    ~ThreadPool();

    // 启动线程池
    void start(int initThreadSize = std::thread::hardware_concurrency()); // 初始化线程池的大小，默认为系统的并发线程数

    // 设置线程池工作模式
    void setMode(ThreadPoolMode mode);

    // 设置task任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshHold);

    // 设置线程池cached模式的线程上限阈值
    void setThreadSizeThreshHold(int threshhold);

    // 给线程池提交任务
    Result submitTask(std::shared_ptr<Task> sp);

    ThreadPool(const ThreadPool &) = delete;            // 不允许线程池拷贝
    ThreadPool &operator=(const ThreadPool &) = delete; // 不允许线程池赋值

private:
    // 定义线程函数
    void threadFunc(int threadId);

    // 检查线程池的运行状态
    bool checkRunningState() const;

private:
    // std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表

    int initThreadSize_;             // 初始的线程数量
    int threadSizeThreshHold_;       // 线程数量上限阈值
    std::atomic_int curThreadSize_;  // 记录当前线程池里面线程的总数量
    std::atomic_int idleThreadSize_; // 记录空闲线程的数量

    std::queue<std::shared_ptr<Task>> taskQueue_; // 任务队列
    std::atomic_int taskSize_;                    // 任务的数量
    int taskQueMaxThreshHold_;                    // 任务队列数量上限阈值

    std::mutex taskQueMutex_;          // 保证任务队列的线程安全
    std::condition_variable notFull_;  // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::mutex exitMutex_;             // 用于保护线程列表和退出条件
    std::condition_variable exitCond_; // 等到线程资源全部回收

    ThreadPoolMode poolMode_;        // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif