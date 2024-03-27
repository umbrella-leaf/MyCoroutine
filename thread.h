#pragma once

#include "mutex.h"
#include <memory>
#include <functional>
#include <pthread.h>


/**
 * @brief 对pthread_t的封装，原因是std::thread不够灵活
*/
class Thread {
public:
    /// 线程智能指针
    using ptr = std::shared_ptr<Thread>;

    /**
     * @brief 线程构造函数
     * @param{in} cb 线程执行函数
     * @param{in} name 线程名称
    */
    Thread(std::function<void()> cb, const std::string& name);

    Thread(const Thread& thread) = delete;

    /**
     * @brief 析构函数
    */
    ~Thread();

    /**
     * @brief 线程ID 
    */
    pid_t getId() const {return m_id;}

    /**
     * @brief 线程名称
    */
    const std::string& getName() const {return m_name;}

    /**
     * @brief 等待线程执行完成
    */
    void join();

    /**
     * @brief 获取当前线程指针
    */
    static Thread* GetThis();

    /**
     * @brief 获取当前线程名称
    */
    static const std::string& Getname();

    /**
     * @brief 设置当前线程名称
    */
    static void SetName(const std::string& name);
private:
    /**
     * @brief 线程执行函数
    */
    static void* run(void* arg);
private:
    /// 线程ID
    pid_t m_id = -1;
    /// 线程结构
    pthread_t m_thread = 0;
    /// 协程执行函数
    std::function<void()> m_cb;
    /// 线程名称
    std::string m_name;
    /// 信号量
    Semaphore m_semaphore;
};

pid_t GetThreadId();