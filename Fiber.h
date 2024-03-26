#pragma once
#include <memory>
#include <functional>
#include <ucontext.h>


/**
 * @brief 协程类
*/
class Fiber: public std::enable_shared_from_this<Fiber> {
public:
    using ptr = std::shared_ptr<Fiber>;

    /**
     * @brief 协程状态
     * @details 只定义三种状态关系，协程要么在运行中，要么就绪，要么已经运行结束
     * 不区分协程初始状态，初始就是READY，不区分协程正常结束还是异常结束
     * 只要结束就是TERM，而如果未结束也没有在运行，就是READY就绪态
    */
    enum State {
        /// 就绪态，刚创建或者yield之后的状态
        READY,
        /// 运行态，resume之后的状态
        RUNNING,
        /// 结束态，协程回调函数执行完毕之后的状态
        TERM
    };

private:
    /**
     * @brief 无参构造函数
     * @attention 无参构造只用于创建线程的主协程，也就是线程主函数对应的那个协程，
     * 这个协程只能由特殊方法调用来创建，不可直接创建，因此定义为私有函数。
    */
    Fiber();

public:
    /**
     * @brief 构造函数，用于创建用户协程
     * @param{in} cb 协程入口函数
     * @param{in} stacksize 栈大小，默认128k
     * @param{in} run_in_scheduler 本协程是否参与调度器调度默认true
    */
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);

    /**
     * @brief 协程析构函数
    */
    ~Fiber();

    /**
     * @brief 重置协程状态和入口函数，复用栈空间，不重新创建栈
     * @param{in} cb 新的协程函数
    */
    void reset(std::function<void()> cb);

    /**
     * @brief 将当前协程切到执行状态
     * @details 当前协程和正在运行的协程进行交换，前者状态变为RUNNING，后者状态变为READY
    */
    void resume();

    /**
     * @brief 当前协程让出执行权
     * @details 当前协程与上次resume退到后台的协程进行交换，前者状态变为READY，后者状态变为RUNNING
    */
    void yield();

    /**
     * @brief 获取协程Id
    */
    uint64_t getId() const {return m_id;}

    /**
     * @brief 获取协程状态
    */
    State getState() const {return m_state;}

public:
    /**
     * @brief 设置当前正在运行的协程，即设置thread_local局部变量t_fiber值
    */
    static void SetThis(Fiber* f);

    /**
     * @brief 返回当前线程正在执行的协程
     * @details 如果当前线程还未创建协程，则创建线程的主协程
     * 其他协程都通过这个协程来调度，也就是说，其他协程结束时，
     * 都需要切换回这个主协程，由主协程选择新的协程进行resume
     * @attention 线程如果要创建协程，应该先执行此操作，来初始化主协程
    */
    static Fiber::ptr GetThis();

    /**
     * @brief 获取总协程数
    */
    static uint64_t TotalFibers();

    /**
     * @brief 协程入口函数
    */
    static void MainFunc();

    /**
     * @brief 获取当前协程Id
    */
    static uint64_t GetFiberId();

private:
    /// 协程ID
    uint64_t m_id = 0;
    /// 协程栈大小
    uint32_t m_stacksize = 0;
    /// 协程状态
    State m_state = READY;
    /// 协程上下文
    ucontext_t m_ctx;
    /// 协程栈地址
    void *m_stack = nullptr;
    /// 协程入口函数
    std::function<void()> m_cb;
    /// 本协程是否参与调度器调度
    bool m_runInScheduler;
};