#include "IOManager.h"
/// epoll头文件
#include <sys/epoll.h>
/// pipe头文件
#include <unistd.h>
/// memset头文件
#include <cstring>
/// 文件操作头文件
#include <fcntl.h>
#include <assert.h>

enum EpollCtlOp {
};

IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(IOManager::Event event) {
    switch(event) {
        case IOManager::READ:
            return read;
        case IOManager::WRITE:
            return write;
        default:
            assert(("getContext", false));
    }
    throw std::invalid_argument("getContext invalid event");
}

void IOManager::FdContext::resetEventContext(EventContext& ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    // Fd上下文必须有注册该事件
    assert(events & event);

    events = (Event)(events & ~event);
    EventContext& ctx = getEventContext(event);
    if (ctx.cb) {
        ctx.scheduler->schedule(&ctx.cb);
    } else {
        ctx.scheduler->schedule(&ctx.fiber);
    }

    ctx.scheduler = nullptr;
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
    : Scheduler(threads, use_caller, name) {
    // 创建epoll实例
    m_epfd = epoll_create(5000);
    assert(m_epfd > 0);

    // 创建pipe，获取m_tickleFds[2]，其中0是读端，1是写端
    int rt = pipe(m_tickleFds);
    assert(!rt);

    // 注册pipe读端的可读事件，用于tickle调度协程，通过epool_event.data.fd保存描述符
    epoll_event event;
    memset(&event, 0, sizeof(epoll_event));
    event.events = EPOLLIN | EPOLLOUT;
    event.data.fd = m_tickleFds[0];

    // 非阻塞方式，配合边缘触发，因为边缘触发必须要循环读取
    // 这就意味着，在循环中不应该阻塞，那就必须使用非阻塞方式
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK);
    assert(!rt);

    // 将管道的读描述符加入epoll多路复用，如果管道可读，idle中的epoll_wait会返回
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    assert(!rt);

    contextResize(32);

    // 开启调度器
    start();
}

/**
 * @brief 通知调度器有任务要调度
 * @details 写pipe让idle协程从epoll_wait退出，等待idle协程yield之后
 * Scheduler::run就可以调度其他任务，如果当前没有空闲调度线程，就没必要发通知
*/
void IOManager::tickle() {
    // std::cout << "tickle" << std::endl;
    if (!hasIdleThreads()) {
        return;
    }
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

/**
 * @brief idle协程
 * @details 对于IO协程调度来说，应阻塞在等待IO事件上，idel退出的时机是epoll_wait返回，对应的操作是tickle或者注册的IO事件就绪
 * 调度器没有调度任务时会阻塞在idle协程上，对IO调度器而言，idle状态应该关注两件事，一是有没有新的调度任务
 * 对应schedule方法。如有新调度任务应该立即退出idle状态，并执行对应任务；而是关注当前注册所有IO事件有没有触发
 * 如果触发，那么马上执行IO事件对应的回调函数
*/
void IOManager::idle() {
    // std::cout << "idle" << std::endl;
    // 一次epoll_wait最多检测256个就绪事件，如果就绪事件超过这个数，会到下轮epoll_wait再处理
    const uint64_t MAX_EVENTS = 256;
    epoll_event *events = new epoll_event[MAX_EVENTS]();
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {
        delete[] ptr;
    });
    while (true) {
        // 获取下一个定时器超时时间，顺便判断调度器是否停止
        uint64_t next_timeout = 0;
        if (stopping(next_timeout)) {
            // std::cout << "name=" << getName() << "idle stopping exit" << std::endl;
            break;
        }

        int rt = 0;
        do {
            // 阻塞在epoll_wait上，等到事件发生
            static const int MAX_TIMEOUT = 5000;
            // 还有定时器，那么距离下一次超时的时间就是min(最大超时时间，当前时间距离首个定时器的时间间隔)
            if (next_timeout != ~0ull) {
                next_timeout = (int)next_timeout > MAX_TIMEOUT
                                ? MAX_TIMEOUT : next_timeout;
            // 没有定时器，则距离下一次超时的时间就是MAX_TIMEOUT
            } else {
                next_timeout = MAX_TIMEOUT;
            }
            rt = epoll_wait(m_epfd, events, MAX_EVENTS, (int)next_timeout);
            if (rt < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        } while (true);

        // 收集所有的已超时定时器，执行回调函数
        std::vector<std::function<void()>> cbs;
        // 这是TimerManager执行并检查超时的唯一机会
        listExpiredCb(cbs);
        if (!cbs.empty()) {
            for (const auto& cb: cbs) {
                schedule(cb);
            }
            cbs.clear();
        }

        // 遍历发生事件，根据epoll_event.data.ptr找到对应的FdContext，进行事件处理
        for (int i = 0; i < rt; ++i) {
            epoll_event &event = events[i];
            if (event.data.fd == m_tickleFds[0]) {
                // 管道读端用于通知协程调度，这时只需要把管道里的内容读完即可
                // 本轮idle结束之后，调度器的run方法会重新执行协程调度
                uint8_t dummy[256];
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }
            // 通过epoll_event的数据指针获取FdContext
            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            // 锁住这个fd上下文
            FdContext::MutexType::Lock lock(fd_ctx->mutex);
            /**
             * EPOLLERR: 出错，比如写读端已关闭的pipe
             * EPOLLHUP：套接字对端关闭
             * 出现此两种情况，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执行不到的情况
             * 所以还要和注册事件做一个&，也就是在这两种情况下，文件上下文注册的读/写事件总会被触发
            */
            if (event.events & (EPOLLERR | EPOLLHUP)) {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }
            int real_events = NONE;
            if (event.events & EPOLLIN) {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) {
                real_events |= WRITE;
            }
            // 如果实际收到的事件和等待事件完全不一样，就找下一个就绪socket
            if ((fd_ctx->events & real_events) == NONE) {
                continue;
            }

            // 剔除已经发生的事件，将剩下的事件重新加入epoll_wait，
            // 如果剩下的事件为0，表示这个fd已经不需要关注了，直接从epoll中删除
            int left_events = (fd_ctx->events & ~real_events);
            int op = left_events ? EPOLL_CTL_MOD: EPOLL_CTL_DEL;
            // 使用ET边缘触发模式，由于fd上下文只有读/写事件，要额外加上ET标志
            event.events = EPOLLET | left_events;

            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) {
                std::cerr << "epoll_ctl(" << m_epfd << ", "
                          << (EpollCtlOp)op << ", " << fd_ctx->fd << ", "
                          << (EPOLL_EVENTS)event.events << "):" << rt2
                          << " (" << errno << ") (" << strerror(errno) << ")";
                continue;
            }
            // 处理已经发生的事件
            if (real_events & READ) {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        }
        /**
         * 一旦处理完毕所有事件，idle协程yield，这样可以让调度协程调用Scheduler::run方法
         * 重新检查是否有新任务要调度，triggerEvent本质上是把任务协程加入调度，要执行的话需要等idle退出。
        */
        Fiber::ptr cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset();

        raw_ptr->yield();
    }
}

void IOManager::contextResize (size_t size) {
    m_fdContexts.resize(size);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (!m_fdContexts[i]) {
            m_fdContexts[i] = new FdContext;
            m_fdContexts[i]->fd = i;
        }
    }
}

/**
 * @brief 添加事件
 * @details fd描述符发生了event事件时执行cb函数
 * @param[in] fd socket句柄
 * @param[in] event 事件类型
 * @param[in] cb 事件回调函数，如果空，则默认将当前协程当前回调执行体
 * @return 添加成功返回0，否则返回-1
*/
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    // 找到fd对应的FdContext，如果不存在，那就分配一个
    FdContext* fd_ctx = nullptr;
    // 防止并发修改m_fdContexts，但允许并发读取
    RWMutexType::ReadLock lock(m_mutex);
    // 文件描述符小于队列大小则直接取，否则需要扩容
    // 此时取出的FdContext可能是空的也可能有事件
    if ((int)m_fdContexts.size() > fd) {
        fd_ctx = m_fdContexts[fd];
        lock.unlock();
    } else {
        lock.unlock();
        // 这里是禁止并发读写，因为要扩容
        RWMutexType::WriteLock lock2(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }

    // 同一个fd不允许重复添加相同的事件，只锁fd上下文
    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (fd_ctx->events & event) {
        std::cerr << "addEvent assert fd=" << fd
        << " event=" << (EPOLL_EVENTS)event
        << " fd_ctx.event=" << (EPOLL_EVENTS)fd_ctx->events;
        assert(!(fd_ctx->events & event));
    }
    // 将新的事件加入epoll_wait，使用epoll_event的私有指针储存FdContext的位置
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    epevent.events = EPOLLET | fd_ctx->events | event;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        std::cerr << "epoll_ctl(" << m_epfd << ", "
        << (EpollCtlOp)op << ", " << fd << ", " <<
        (EPOLL_EVENTS)epevent.events << "):"
        << rt << " (" << errno << ") (" << strerror(errno) <<
        ") fd_ctx->events="
        << (EPOLL_EVENTS)fd_ctx->events;
        return -1;
    }

    // 待执行IO事件数+1
    ++m_pendingEventCount;

    // 找到这个fd的event事件对应的EventContext，对其中的scheduler, cb, fiber进行赋值
    fd_ctx->events = (Event)(fd_ctx->events | event);
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);

    // 赋值scheduler和回调函数，如果回调函数为空，则把当前协程当成回调执行体
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) {
        event_ctx.cb.swap(cb);
    } else {
        event_ctx.fiber = Fiber::GetThis();
        // 当前协程必须正在运行
        assert(("state=" + event_ctx.fiber->getState(), event_ctx.fiber->getState() == Fiber::RUNNING));
    }
    return 0;
}

/**
 * @brief 删除事件
 * @param[in] fd socket 句柄
 * @param[in] event 事件类型
 * @attention 不会触发任何事件
*/
bool IOManager::delEvent(int fd, Event event) {
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) {
        return false;
    }

    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        std::cerr << "epoll_ctl(" << m_epfd << ", "
        << (EpollCtlOp)op << ", " << fd << ", " << (EPOLL_EVENTS)epevent.events << "):"
        << rt << " (" << errno << ") (" << strerror(errno) << ")";
        return false;
    }

    --m_pendingEventCount;
    // fd上下文中不再含有该事件
    fd_ctx->events = new_events;
    // 同时将fd上下文中对应的event上下文也删除
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);
    return true;
}

/**
* @brief 取消事件
* @param[in] fd socket句柄
* @param[in] event 事件类型
* @attention 如果该事件被注册过回调，那就触发⼀次回调事件
* @return 是否取消成功
*/
bool IOManager::cancelEvent(int fd, Event event) {
    // 找到fd对应的FdContext
    RWMutexType::ReadLock lock(m_mutex);
    // 如果fd超出范围，直接返回false
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock2(fd_ctx->mutex);
    // 如果删除的event和fd_ctx->events完全不一样，就删除失败
    if (!(fd_ctx->events & event)) {
        return false;
    }

    // 删除事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        std::cerr << "epoll_ctl(" << m_epfd << ", "
        << (EpollCtlOp)op << ", " << fd << ", " <<
        (EPOLL_EVENTS)epevent.events << "):"
        << rt << " (" << errno << ") (" << strerror(errno) <<
        ")";
        return false;
    }

    // 删除之前触发一次事件
    fd_ctx->triggerEvent(event);
    // 活跃事件数-1
    --m_pendingEventCount;
    return true;
}


bool IOManager::cancelAll(int fd) {
    RWMutexType::ReadLock lock(m_mutex);
    if ((int)m_fdContexts.size() <= fd) {
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    lock.unlock();

    FdContext::MutexType::Lock lock(fd_ctx->mutex);
    // 没有事件则返回false
    if (!fd_ctx->events) {
        return false;
    }

    // 删除全部事件
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;

    epevent.events = 0;
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        std::cerr << "epoll_ctl(" << m_epfd << ", "
        << (EpollCtlOp)op << ", " << fd << ", " <<
        (EPOLL_EVENTS)epevent.events << "):"
        << rt << " (" << errno << ") (" << strerror(errno) <<
        ")";
        return false;
    }

    // 触发该fd上下文全部已注册事件
    if (fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }
    if (fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i]) {
            delete m_fdContexts[i];
        }
    }
}

bool IOManager::stopping() {
    uint64_t timeout = 0;
    return stopping(timeout);
}

bool IOManager::stopping(uint64_t& timeout) {
    timeout = getNextTimer();
    // 对于IOManager而言，必须等到没有定时器，并且全部调度的IO事件都执行完才能退出
    return timeout == ~0ull 
        && m_pendingEventCount == 0 
        && Scheduler::stopping();
}

void IOManager::onTimerInsertedAtFront() {
    tickle();
}



