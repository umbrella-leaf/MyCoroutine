#pragma once
#include "Fiber.h"
#include <list>
#include <iostream>
#include <functional>

/**
 * @brief 简单协程调度类，支持添加调度任务以及运行调度任务
*/
class Scheduler {
public:
    /**
     * @brief 添加协程调度任务
    */
    void schedule(Fiber::ptr task) {
        m_tasks.push_back(task);
    }

    /**
     * @brief 执行调度任务
     * @note 为简化流程，此处选择先来先服务策略
    */
    void run() {
        Fiber::ptr task;
        auto it = m_tasks.begin();
        while (it != m_tasks.end()) {
            task = *it;
            m_tasks.erase(it++);
            task->resume();
        }
    }

private:
    /// 任务队列
    std::list<Fiber::ptr> m_tasks;
};