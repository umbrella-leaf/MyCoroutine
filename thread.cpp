#include "thread.h"
#include <iostream>
#include <unistd.h>
#include <syscall.h>

static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOWN";

Thread* Thread::GetThis() {
    return t_thread;
}

const std::string& Thread::Getname() {
    return t_thread_name;
}

void Thread::SetName(const std::string& name) {
    if (name.empty()) {
        return;
    } 
    if (t_thread) {
        t_thread->m_name = name;
    }
    t_thread_name = name;
 }

Thread::Thread(std::function<void()> cb, const std::string& name)
    :m_cb(cb),
    m_name(name) {
    if (name.empty()) {
        m_name = "UNKNOWN";
    }
    // 这里要额外包一层run的原因主要是获取线程的真正ID
    // 这个ID只有在创建的线程运行时才能拿到，因此实际的流程是
    // 创建线程->【线程运行->拿到线程ID】-> 回到caller线程
    // 因此需要信号量来进行同步，caller创建完线程马上阻塞在信号量(信号量除初始值为0)
    // 转为线程运行，拿到线程ID，然后唤醒阻塞在信号量上的caller线程，最后创建完成
    // 所以这里信号量的主要意义是同步
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);
    if (rt) {
        std::cerr << "pthread_create thread fail, rt=" << rt << " name=" << name << std::endl;
        throw std::logic_error("pthread_create error");
    }
    m_semaphore.wait();
}

Thread::~Thread() {
    if (m_thread) {
        pthread_detach(m_thread);
    }
}

void Thread::join() {
    if (m_thread) {
        int rt = pthread_join(m_thread, nullptr);
        if (rt) {
            std::cerr << "pthread_join thread fail, rt=" << rt << " name=" << m_name << std::endl;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

pid_t GetThreadId() {
    return syscall(SYS_gettid);
}

void* Thread::run(void* arg) {
    Thread* thread = (Thread*) arg;
    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = GetThreadId();
    pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(thread->m_cb);

    thread->m_semaphore.notify();

    cb();
    return 0;
}