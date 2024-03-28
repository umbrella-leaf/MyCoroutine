#include "Timer.h"
#include <sys/time.h>

uint64_t GetCurrentMS() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000ul  + tv.tv_usec / 1000;
}

bool Timer::Comparator::operator() (const Timer::ptr& lhs
                        , const Timer::ptr& rhs) const {
    if (!lhs && !rhs) {
        return false;
    }
    if (!lhs) {
        return true;
    }
    if (!rhs) {
        return false;
    }
    if (lhs->m_next < rhs->m_next) {
        return true;
    }
    if (lhs->m_next > rhs->m_next) {
        return false;
    }
    return lhs.get() < rhs.get();
}

Timer::Timer(uint64_t ms, std::function<void()> cb,
            bool recurring, class TimerManager* manager)
    :m_recurring(recurring)
    ,m_ms(ms)
    ,m_cb(cb)
    ,m_manager(manager) {
    m_next = GetCurrentMS() + m_ms;
}

Timer::Timer(uint64_t next)
    :m_next(next) {
}

bool Timer::cancel() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (m_cb) {
        // 释放回调并从set中删除
        m_cb = nullptr;
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

bool Timer::refresh() {
    // 同一时间只有一个timer能被操作，因为要操作TimeManager的计时器集合
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }
    // 删除原计时器
    m_manager->m_timers.erase(it);
    // 刷新计时器过期时间
    m_next = GetCurrentMS() + m_ms;
    // 插入新的计时器
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

bool Timer::reset(uint64_t ms, bool from_now) {
    // 执行周期没变，并且不要求从此刻开始，就不做任何操作
    if (ms == m_ms && !from_now) {
        return true;
    }
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if (!m_cb) {
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if (it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    uint64_t start = 0;
    // 如果指定从此刻重新开始，就重置开始时间
    if (from_now) {
        start = GetCurrentMS();
    // 否则还是用原来的开始时间
    } else {
        start = m_next - m_ms;
    }
    m_ms = ms;
    // 使用更新后的超时时间(执行时间)
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;
}

TimerManager::TimerManager() {
    m_previousTime = GetCurrentMS();
}

TimerManager::~TimerManager() {

}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb
                                  ,bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutexType::WriteLock lock(m_mutex);
    // 加入TimerManager计时器集合
    addTimer(timer, lock);
    return timer;
}

static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock();
    if (tmp) {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer (uint64_t ms, std::function<void()> cb
                        ,std::weak_ptr<void> weak_cond
                        ,bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

uint64_t TimerManager::getNextTimer() {
    // 因为只是读，但不允许读取时有任何修改，因此加读锁
    RWMutexType::ReadLock lock(m_mutex);
    m_tickled = false;
    if (m_timers.empty()) {
        return ~0ull;
    }
    // 获取定时器集合首个计时器，即为最近定时器
    const Timer::ptr& next = *m_timers.begin();
    uint64_t now_ms = GetCurrentMS();
    // 如果已经过期，返回0，否则返回剩余时间
    if (now_ms >= next->m_next) {
        return 0;
    } else {
        return next->m_next - now_ms;
    }
}

void TimerManager::listExpiredCb(std::vector<std::function<void()>> &cbs) {
    uint64_t now_ms = GetCurrentMS();
    std::vector<Timer::ptr> expired;
    {
        RWMutexType::ReadLock lock(m_mutex);
        if (m_timers.empty()) {
            return;
        }
    }
    // 要动定时器集合，上写锁
    RWMutexType::WriteLock lock(m_mutex);
    if (m_timers.empty()) {
        return;
    }
    bool rollover = detectClockRollover(now_ms);
    // 服务器时间未调后，并且首个定时器还未到期，那么不需要遍历，肯定所有定时器都未到期
    if (!rollover && ((*m_timers.begin())->m_next > now_ms)) {
        return;
    }

    Timer::ptr now_timer(new Timer(now_ms));
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
    while (it != m_timers.end() && (*it)->m_next == now_ms) {
        ++it;
    }
    // 如果服务器时间调后，那么所有定时器均过期;
    // 否则所有m_next <= 当前时间的定时器过期
    expired.insert(expired.begin(), m_timers.begin(), it);
    // 从定时器集合中删除过期定时器
    m_timers.erase(m_timers.begin(), it);
    cbs.reserve(expired.size());

    for (auto& timer: expired) {
        cbs.push_back(timer->m_cb);
        if (timer->m_recurring) {
            timer->m_next = now_ms + timer->m_ms;
            m_timers.insert(timer);
        } else {
            timer->m_cb = nullptr;
        }
    }
}

void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock& lock) {
    // 这个addTimer是因为可能刷新已有的定时器，因此传入参数是定时器
    // 还有一个参数是锁的原因因为在调用外部可能已经上锁，保证锁状态不变
    auto it = m_timers.insert(val).first;
    bool at_front = (it == m_timers.begin()) && !m_tickled;
    if (at_front) {
        m_tickled = true;
    }
    lock.unlock();
    // 触发首部插入回调，但这里不用加锁，因为不是在访问TimerManager
    if (at_front) {
        onTimerInsertedAtFront();
    }
}

bool TimerManager::detectClockRollover(uint64_t now_ms) {
    bool rollover = false;
    if (now_ms < m_previousTime &&
            now_ms < (m_previousTime - 60 * 60 * 1000)) {
        rollover = true;
    }
    m_previousTime = now_ms;
    return rollover;
}

bool TimerManager::hasTimer() {
    // 防止别的线程修改定时器集合，上读锁
    RWMutexType::ReadLock lock(m_mutex);
    return !m_timers.empty();
}







