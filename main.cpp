#include "Scheduler.h"
#include <unistd.h>

void test_fiber(int i) {
    std::cout << "hello world " << i << std::endl;
}

int main() {
    /// 创建调度器
    Scheduler sc(3, true);
    sc.start();
    sleep(2);
    /// 添加调度任务
    for (auto i = 0; i < 10; ++i) {
        // 如果传入的不是共享指针，那么每次循环完成的话，
        // 协程对象被释放，会造成指针悬空
        Fiber::ptr fiber(new Fiber(
            std::bind(test_fiber, i)
        ));
        sc.schedule(fiber);
    }
    /// 执行调度任务
    sc.stop();
    return 0;
}