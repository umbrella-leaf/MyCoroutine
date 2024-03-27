#include "Fiber.h"
#include "Scheduler.h"
#include <assert.h>
#include <atomic>
#include <iostream>

static std::atomic<uint64_t> s_fiber_id {0};
static std::atomic<uint64_t> s_fiber_count{0};

static thread_local Fiber* t_fiber = nullptr;
static thread_local Fiber::ptr t_thread_fiber = nullptr;

static uint32_t g_fiber_stack_size {128 * 1024};

/**
 * @brief 栈空间分配器
*/
class StackAllocator {
public:
    static void* Alloc(size_t size) {
        return malloc(size);
    }
    static void Dealloc(void* vp, size_t size) {
        return free(vp);
    }
};

/**
 * @brief 无参构造函数
 * @attention 无参构造只用于创建线程的主协程，也就是线程主函数对应的那个协程，
 * 这个协程只能由特殊方法调用来创建，不可直接创建，因此定义为私有函数。
*/
Fiber::Fiber()
    : m_id(s_fiber_id++)
    , m_runInScheduler(false) {
    SetThis(this);
    m_state = RUNNING;

    if (getcontext(&m_ctx)) {
        assert(("getcontext", false));
    }

    ++s_fiber_count;
    // std::cout << "Fiber::Fiber() main id = " << m_id << std::endl;
}

/**
 * @brief 构造函数，用于创建用户协程
 * @param[in] cb 协程入口函数
 * @param[in] stacksize 栈大小，默认128k
 * @param[in] run_in_scheduler 本协程是否参与调度器调度默认true
*/
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler)
    : m_id(s_fiber_id++)
    , m_cb(cb)
    , m_runInScheduler(run_in_scheduler) {
    ++s_fiber_count;
    m_stacksize = stacksize ? stacksize : g_fiber_stack_size;
    m_stack = StackAllocator::Alloc(m_stacksize);

    if (getcontext(&m_ctx)) {
        assert(("getcontext", false));
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);

    // std::cout << "Fiber::Fiber() id = " << m_id << std::endl;
}

/**
 * @brief 返回当前线程正在执行的协程
 * @details 如果当前线程还未创建协程，则创建线程的主协程
 * 其他协程都通过这个协程来调度，也就是说，其他协程结束时，
 * 都需要切换回这个主协程，由主协程选择新的协程进行resume
 * @attention 线程如果要创建协程，应该先执行此操作，来初始化主协程
*/
Fiber::ptr Fiber::GetThis() {
    if (!t_fiber) {
        // 没有协程则创建一个主协程
        Fiber::ptr main_fiber(new Fiber);
        assert(t_fiber == main_fiber.get());
        t_thread_fiber = main_fiber;
    }
    return t_fiber->shared_from_this();
}

/**
 * @brief 设置当前正在运行的协程，即设置thread_local局部变量t_fiber值
*/
void Fiber::SetThis(Fiber* f) {
    t_fiber = f;
}

/**
 * @brief 将当前协程切到执行状态
 * @details 当前协程和正在运行的协程进行交换，前者状态变为RUNNING，后者状态变为READY
*/
void Fiber::resume() {
    // 只能继续就绪的协程
    assert(m_state != TERM && m_state != RUNNING);
    SetThis(this);
    m_state = RUNNING;

    // 如果协程参与调度器调度，那么应该和调度器的主协程进行swap，而不是线程主协程
    if (m_runInScheduler) {
        if (swapcontext(&(Scheduler::GetMainFiber()->m_ctx), &m_ctx)) {
            assert(("swapcontext", false));
        }
    } else {
        if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
            assert(("swapcontext", false));
        }
    }
}

/**
 * @brief 当前协程让出执行权
 * @details 当前协程与上次resume退到后台的协程进行交换，前者状态变为READY，后者状态变为RUNNING
*/
void Fiber::yield() {
    // 就绪态的协程无法yield，为什么结束态的进程也能yield呢？
    // 因为协程运行完毕之后自动yield一次用于回到主协程。
    assert(m_state == RUNNING || m_state == TERM);
    SetThis(t_thread_fiber.get());
    if (m_state != TERM) {
        m_state = READY;
    }
    // 与resume相反，如果该协程参与调度，也就是任务协程，那么其yield的对象应该是调度器主协程，
    // 也就是调度协程；否则其本身是调度线程，就应该yield到主协程
    if (m_runInScheduler) {
        if (swapcontext(&m_ctx, &(Scheduler::GetMainFiber()->m_ctx))) {
            assert(("swapcontext", false));
        } 
    } else {
        if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            assert(("swapcontext", false));
        }
    }
}

/**
 * @brief 协程入口函数
 * @note 此处无异常处理，主要是简化状态管理，
 * 而且协程既然是用户线程，那么理应由用户来处理异常
*/
void Fiber::MainFunc() {
    Fiber::ptr cur = GetThis();
    assert(cur);

    cur->m_cb();
    cur->m_cb = nullptr;
    cur->m_state = TERM;
    // 手动释放t_fiber
    auto raw_ptr = cur.get();
    cur.reset();
    raw_ptr->yield();  // 协程结束时自动yield，以回到主协程
}

/**
 * @brief 重置协程状态和入口函数，复用栈空间，不重新创建栈
 * @note 为简化状态管理，强制只有TERM状态协程才能重置，但其实刚创建好的协程也能重置
 * @param[in] cb 新的协程函数
*/
void Fiber::reset(std::function<void()> cb) {
    // 重置的协程必须有栈，否则无法复用
    assert(m_stack);
    assert(m_state == TERM);
    m_cb = cb;
    if (getcontext(&m_ctx)) {
        assert(("getcontext", false));
    }
    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack;
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    // 更换入口函数后，协程由终止变为就绪
    m_state = READY;
}

/**
 * @brief 协程析构函数
*/
Fiber::~Fiber() {
    --s_fiber_count;
    if (m_stack) {
        assert(m_state == TERM);
        StackAllocator::Dealloc(m_stack, m_stacksize);
    }
    // std::cout << "Fiber::~Fiber() main id = " << m_id
    //           << " total=" << s_fiber_count << std::endl;
}

uint64_t Fiber::TotalFibers() {
    return s_fiber_count;
}

uint64_t Fiber::GetFiberId() {
    return t_fiber->m_id;
}

