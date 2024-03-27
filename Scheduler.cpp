#include "Scheduler.h"
#include <assert.h>

/**
 * @brief 当前线程持有的调度器指针
 * @note 同一个线程最多创建一个调度器，不加thread_local的话总共只有一个调度器,
 * 不过无需担心工作线程，工作线程只会埋头干活，不会去创建调度器
*/
static thread_local Scheduler* t_scheduler = nullptr;
/// @brief 当前线程的调度协程指针
static thread_local Fiber* t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name) {
    assert(threads > 0);

    m_useCaller = use_caller;
    m_name = name;

    // 创建调度器的线程，如果作为调度线程，就无需新建调度线程
    // 并且该线程自身也不算做工作线程
    // 这意味着，如果use_caller为真，并且总的线程数为1，那么只有caller线程负责调度
    // 否则除caller线程外，需要额外创建threads - 1个线程负责调度
    // 如果use_caller为假，那么caller线程不参与调度，则创建threads个线程负责调度
    if (use_caller) {
        --threads;
        // 创建caller线程的主协程，以便调度结束后切换回来
        Fiber::GetThis();
        assert(GetThis() == nullptr);
        setThis();

        /**
         * 在user_caller为true的情况下，初始化caller线程的调度协程
         * caller线程的调度协程不会被调度器调度，而且，caller线程的调度协程停止时，应该返回caller线程的主线程
        */
        // 注意bind绑定成员函数需要绑this参数
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        // 当前的caller线程参与调度，因此重命名为调度器名称
        Thread::SetName(m_name);
        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread = GetThreadId();
        m_threadIds.push_back(m_rootThread);
    } else {
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler::~Scheduler() {
    assert(m_stopping);
    // 很明显，其他无关线程虽然可以在use caller为假时停止调度器
    // 但是最终释放调度器的线程一定是caller线程(废话)
    if(GetThis() == this) {
        t_scheduler = nullptr;
    }
}

void Scheduler::setThis() {
    t_scheduler = this;
}

Scheduler *Scheduler::GetThis() {
    return t_scheduler;
}

Fiber* Scheduler::GetMainFiber() {
    return t_scheduler_fiber;
}

/** 
 * @brief 启动调度器
 * @note 主要在这里初始化调度线程池，如果只使用caller线程来进行调度，
 * 也就是threads为1，use_caller为真的情况，那么实际上什么也不做(不需要创建额外线程)
 **/ 
void Scheduler::start() {
    // std::cout << "start" << std::endl;
    // 上锁以避免多个线程同时启动调度器
    MutexType::Lock lock(m_mutex);
    if (m_stopping) {
        std::cerr << "Scheduler is stopped" << std::endl;
        return;
    }
    assert(m_threads.empty());
    // 初始化线程池大小
    m_threads.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; ++i) {
        // 多个工作线程执行的是同一个调度器的调度函数，因为需要共用一个任务队列
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                        m_name + "_" + std::to_string(i)));
        // 线程通过同步已经保证创建完成时，其ID已经拿到。
        m_threadIds.push_back(m_threads[i]->getId());
    }
}

/**
 * @brief 运行调度器
 * @note 
*/
void Scheduler::run() {
    // std::cout << "run" << std::endl;
    // 运行时才设置当前线程调度器指针
    setThis();
    // 所有工作线程(非创建调度器的caller线程)，其调度协程就是主协程
    // 这意味着，所有工作线程中，调度协程，主协程，运行协程其实是一个东西
    if (GetThreadId() != m_rootThread) {
        t_scheduler_fiber = Fiber::GetThis().get();
    }

    // 除了主协程和调度协程之外，还创建一个idle协程来处理任务队列空的情况
    // 这个idle协程会直接yield回调度协程，
    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    Fiber::ptr cb_fiber;

    ScheduleTask task;
    while (true) {
        task.reset();
        bool tickle_me = false;
        {
            // 只在找任务过程中加锁
            MutexType::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            // 遍历所有调度任务
            while (it != m_tasks.end()) {
                // 任务指定了线程，但不是当前线程，需要通知其他线程，然后去下一个任务
                if (it->thread != -1 && it->thread != GetThreadId()) {
                    ++it;
                    tickle_me = true;
                    continue;
                }
                
                assert(it->fiber || it->cb);
                if (it->fiber) {
                    // 此时协程状态一定是READY
                    assert(it->fiber->getState() == Fiber::READY);
                }
                // 当前调度线程找到一个任务，准备开始调度，将其从任务队列移除，然后活动线程数+1
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，如果队列不为空，需要通知其他线程
            tickle_me |= (it != m_tasks.end());
        }

        if (tickle_me) {
            tickle();
        }

        if (task.fiber) {
            // 任务是协程，则resume协程
            task.fiber->resume();
            // 从resume返回后，要么完成，要么半路yield，任务就算执行完毕，当前线程不再活跃
            // 活跃线程数+1
            --m_activeThreadCount;
            task.reset();
        } else if (task.cb) {
            if (cb_fiber) {
                cb_fiber->reset(task.cb);
            } else {
                cb_fiber.reset(new Fiber(task.cb));
            }
            task.reset();
            // 函数转协程再resume
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        } else {
            // 任务队列空
            if (idle_fiber->getState() == Fiber::TERM) {
                // 如果idle协程结束，说明调度器停止
                // std::cout << "idle fiber term" << std::endl;
                break;
            }
            // 否则就resume idle协程，类似自旋锁，当前线程变为idle线程
            ++m_idleThreadCount;
            idle_fiber->resume();
            // 等到idle又yield回来，当前线程就不再是idle线程
            --m_idleThreadCount;
        }
    }
    // std::cout << "Scheduler::run() exit" << std::endl;
}

void Scheduler::stop() {
    // std::cout << "stop" << std::endl;
    if (stopping()) {
        return;
    }
    m_stopping = true;

    /// 如果是use caller，那么只能由caller发起stop，否则可由别的无关线程来停止调度器
    if (m_useCaller) {
        assert(GetThis() == this);
    } else {
        assert(GetThis() != this);
    }

    for (size_t i = 0; i < m_threadCount; ++i) {
        tickle();
    }

    if (m_rootFiber) {
        tickle();
    }

    /// 在use caller下，调度器协程结束时，需要返回caller调度协程
    if (m_rootFiber) {
        m_rootFiber->resume();
        // std::cout << "m_rootFiber end" << std::endl;
    }
    std::vector<Thread::ptr> thrs;
    // 交换置空
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }
    // 等待任务队列空，且每个线程的idle协程都结束为止(此时线程才可能结束)
    for (auto &i: thrs) {
        i->join();
    }
}

void Scheduler::idle() {
    // std::cout << "idle" << std::endl;
    while (!stopping()) {
        auto cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset();
        // 主要是避免yield后不回来，因此手动释放这个shared_ptr引用
        raw_ptr->yield();
    }
}

void Scheduler::tickle() {
    // std::cout << "tickle" << std::endl;
}

bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    // 真正的停止必须要满足：
    // 1. 有线程调用了调度器的停止方法
    // 2. 任务队列已经空，所有任务执行完毕
    // 3. 当前没有活跃线程在执行任务
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}