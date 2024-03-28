# pragma once
#include "Scheduler.h"
#include "Timer.h"
#include "mutex.h"
#include <memory>
#include <functional>
#include <atomic>
#include <vector>

class IOManager: public Scheduler, public TimerManager {
public:
    using ptr = std::shared_ptr<IOManager>;
    // 涉及读写，因此使用读写锁
    using RWMutexType = RWMutex;
    /**
     * @brief IO事件，继承自epoll对事件的定义
     * @details 这里只关心socket fd的读写事件，为简化状态转移，其他事件归类到这两种事件中
    */
    enum Event {
        /// 无事件
        NONE = 0x0,
        /// 读事件(EPOLLIN)
        READ = 0x1,
        /// 写事件(EPOLLOUT)
        WRITE = 0x4,
    };
private:
    /**
     * @brief socket fd上下文类
     * @details 每个socket fd都对应一个FdContext，包括其描述符值，fd上的事件，以及fd的读写事件上下文
    */
    struct FdContext {
        using MutexType = Mutex;
        /**
         * @brief 事件上下文类
         * @details fd的每个事件都有一个事件上下文，保存这个事件的回调函数以及执行回调函数的调度器
        */
        struct EventContext {
            /// 执行事件回调的调度器
            Scheduler *scheduler = nullptr;
            /// 事件回调协程
            Fiber::ptr fiber;
            /// 事件回调函数
            std::function<void()> cb;
        };

        /**
         * @brief 获取事件上下文类
         * @param[in] event 事件类型
         * @return 返回对应事件的上下文
        */
        EventContext &getEventContext(Event event);

        /**
         * @brief 重置事件上下文
         * @param[in, out] ctx 待重置的事件上下文对象
        */
        void resetEventContext(EventContext &ctx);

        /**
         * @brief 触发事件
         * @details 根据事件类型调用上下文结构中的调度器去调度回调协程或回调函数
         * @param[in] event 事件类型
        */
        void triggerEvent(Event event);

        /// 读事件上下文
        EventContext read;
        /// 写事件上下文
        EventContext write;
        /// 事件关联的句柄
        int fd = 0;
        /// 该fd添加了哪些事件的回调函数，或者说该fd关心哪些事件
        Event events = NONE;
        /// 事件的mutex
        MutexType mutex;
    };
public:
    /**
     * @brief 构造函数
     * @param[in] threads 线程数量
     * @param[in] use_caller caller线程是否参与调度
     * @param[in] name 调度器的名称
    */
    IOManager(size_t threads = 1, bool use_caller = true, const std::string& name = "");

    /**
     * @brief 析构函数
    */
    ~IOManager();

    /**
     * @brief 添加事件
     * @param[in] fd socket 句柄
     * @param[in] event 事件类型
     * @param[in] cb 事件回调函数
     * @return 添加成功返回0，否则返回-1
    */
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

    /**
     * @brief 删除事件
     * @param[in] fd socket 句柄
     * @param[in] event 事件类型
     * @attention 不会触发任何事件
    */
    bool delEvent(int fd, Event event);

    /**
     * @brief 取消事件
     * @param[in] fd socket 句柄
     * @param[in] event 事件类型
     * @attention 如果事件存在则触发事件
    */
    bool cancelEvent(int fd, Event event); 

    /**
     * @brief 取消所有事件
     * @param[in] fd socket 句柄
     * @result 是否取消成功
    */
    bool cancelAll(int fd);   

    /**
     * @brief 返回当前的IOManager
    */
    static IOManager* GetThis();
private:
    void tickle() override;
    bool stopping() override;
    void idle() override;
    void onTimerInsertedAtFront() override;

    /**
     * @brief 重置socket句柄上下文的容器大小
     * @param[in] size 容量大小
    */
    void contextResize(size_t size);
    /**
     * @brief 判断是否可以停止
     * @param[out] timeout 最近要出发的定时器事件间隔
     * @return 返回是否可以停止
     */
    bool stopping(uint64_t& timeout);
private:
    /// epoll文件句柄
    int m_epfd = 0;
    /// pipe文件句柄，fd[0]读端，fd[1]写端
    int m_tickleFds[2];
    /// 当前等待执行的IO事件数量
    std::atomic<size_t> m_pendingEventCount = {0};
    /// IOManager的锁
    RWMutexType m_mutex;
    /// socket事件上下文的容器
    std::vector<FdContext* > m_fdContexts;
};