#include "IOManager.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <stack>
#include <cstring>

// void test_fiber(int i) {
//     std::cout << "hello world " << i << std::endl;
// }

// int main() {
//     /// 创建调度器
//     Scheduler sc(3, true);
//     sc.start();
//     sleep(2);
//     /// 添加调度任务
//     for (auto i = 0; i < 10; ++i) {
//         // 如果传入的不是共享指针，那么每次循环完成的话，
//         // 协程对象被释放，会造成指针悬空
//         Fiber::ptr fiber(new Fiber(
//             std::bind(test_fiber, i)
//         ));
//         sc.schedule(fiber);
//     }
//     /// 执行调度任务
//     sc.stop();
//     return 0;
// }

static int sock_listen_fd = -1;

void test_accept();

void error(const char* msg) {
    perror(msg);
    printf("erreur...\n");
    exit(1);
}

void watch_io_read() {
    IOManager::GetThis() ->
        addEvent(sock_listen_fd, IOManager::READ, test_accept);
}

void test_accept() {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);
    int fd = accept(sock_listen_fd, (struct sockaddr*)&addr, &len);
    if (fd < 0) {
        std::cout << "fd = " << fd << "accept false" << std::endl;
    } else {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        IOManager::GetThis()->
            addEvent(fd, IOManager::READ, [fd]() {
                char buffer[1024];
                memset(buffer, 0, sizeof(buffer));
                while (true) {
                    int ret = recv(fd, buffer, sizeof(buffer), 0);
                    if (ret > 0) {
                        ret = send(fd, buffer, ret, 0);
                    }
                    if (ret <= 0) {
                        // if (errno == EAGAIN) continue;
                        close(fd);
                        break;
                    }
                }
            });
    }
    IOManager::GetThis()->schedule(watch_io_read);
}

void test_iomanager() {
    int portno = 8080;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_listen_fd < 0) {
        error("Error creating socket...\n");
    }
    int yes = 1;
    setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset((char*)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(portno);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        error("Error binding socket...\n");

    if (listen(sock_listen_fd, 2048) < 0) {
        error("Error listening..\n");
    }
    printf("epoll echo server listening for connections on port: %d\n", portno);
    fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);

    IOManager iom(4, true);
    iom.addEvent(sock_listen_fd, IOManager::READ, test_accept);
}

int main(int argc, char *argv[]) {
    test_iomanager();
    return 0;
}

// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <unistd.h>
// #include <sys/socket.h>
// #include <arpa/inet.h>
// #include <sys/epoll.h>
// #include <fcntl.h>
// #define MAX_EVENTS 10000
// #define PORT 8888
// int main() {
//  int listen_fd, conn_fd, epoll_fd, event_count;
//  struct sockaddr_in server_addr, client_addr;
//  socklen_t addr_len = sizeof(client_addr);
//  struct epoll_event events[MAX_EVENTS], event;
//  // 创建监听套接字
//  if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
//  perror("socket");
//  return -1;
//  }
//  // 设置服务器地址和端⼝
//  memset(&server_addr, 0, sizeof(server_addr));
//  server_addr.sin_family = AF_INET;
//  server_addr.sin_port = htons(PORT);
//  server_addr.sin_addr.s_addr = INADDR_ANY;
//  // 绑定监听套接字到服务器地址和端⼝
//  if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
//  perror("bind");
//  return -1;
//  }
//  // 监听连接
//  if (listen(listen_fd, 10000) == -1) {
//  perror("listen");
//  return -1;
//  }
//  // 创建 epoll 实例
//  if ((epoll_fd = epoll_create1(0)) == -1) {
//  perror("epoll_create1");
//  return -1;
//  }
//  // 添加监听套接字到 epoll 实例中
//  event.events = EPOLLIN;
//  event.data.fd = listen_fd;
//  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1) {
//  perror("epoll_ctl");
//  return -1;
//  }
//  while (1) {
//  // 等待事件发⽣
//  event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
//  if (event_count == -1) {
//  perror("epoll_wait");
//  return -1;
//  }
//  // 处理事件
//  for (int i = 0; i < event_count; i++) {
//  if (events[i].data.fd == listen_fd) {
//  // 有新连接到达
//  conn_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
// //  printf("%d\n", conn_fd);
//  if (conn_fd == -1) {
//  perror("accept");
//  continue;
//  }
//  // 将新连接的套接字添加到 epoll 实例中
//  fcntl(conn_fd, F_SETFL, O_NONBLOCK);
//  event.events = EPOLLIN | EPOLLET;
//  event.data.fd = conn_fd;
//  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &event) == -1) {
//  perror("epoll_ctl");
//  return -1;
//  }
//  } else {
//  // 有数据可读
//  char buffer[1024];
//  while (true) {
//                     int ret = recv(conn_fd, buffer, sizeof(buffer), 0);
//                     if (ret > 0) {
//                         ret = send(conn_fd, buffer, ret, 0);
//                     }
//                     if (ret <= 0) {
//                         // if (errno == EAGAIN) continue;
//                         close(conn_fd);
//                         break;
//                     }
//                 }
//  }
//  }
//  }
//  // 关闭监听套接字和 epoll 实例
//  close(listen_fd);
//  close(epoll_fd);
//  return 0;
// }