#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <fcntl.h>
#include "../http/http_conn.h"

class util_timer;

struct client_data
{
    int sockfd;
    sockaddr_in address;
    util_timer* timer;
};

class util_timer{
public:
    util_timer() : prev(NULL), next(NULL) {}
    
public:
    time_t expire;

    void (*cb_func)(client_data* );
    util_timer* prev;
    util_timer* next;
    client_data* user_data;
};

class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer* timer);
    void adjust_timer(util_timer* timer);
    void del_timer(util_timer* timer);
    void tick();

private:
    void add_timer(util_timer* timer, util_timer* lst_head);

    util_timer *head;
    util_timer *tail;
};


class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //添加信号捕捉
    void addsig(int sig, void (handler)(int));

    //信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，
    //缩短异步执行时间，减少对主程序的影响。主循环收到信号后转而执行timer_handler函数
    static void sig_handler(int sig);

    //调用timer_lst里的tick函数，并且重新定时
    void timer_handler();

    //向epoll中添加要监听的文件描述符
    void addfd(int epollfd, int fd, bool one_shot);

    //设置文件描述符为非阻塞
    void setnonblocking(int fd);

    //从epoll中删除要监听的文件描述符
    void removefd(int epollfd, int fd);

    // 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
    void modfd(int epollfd, int fd, int ev);
public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    int m_TIMESLOT;
    static int u_epollfd;

};

void cb_func(client_data* user_data);
#endif


/*
如何应用util_timer类和sort_timer_lst类

1.WebServer类中定义一个m_pipefd[2]，用来传递信号，注意在析构函数中关闭
2.WebServer类中定义一个client_data数组——user_timer,它和http_conn数组——user是一一对应的，
即同一个下标对应同一个文件描述符
    - 若该数组是动态申请的，记得在析构函数中释放
3.WebServer类中有一个工具类对象——utils，它里面有一个sort_timer_lst对象，作为管理定时器的容器
4.WebServer类中定义一个函数：timer(int connfd, struct sockaddr_in client_address)
    该函数的功能：
    - 对connfd对应的http_conn做初始化 即users[connfd].init
    - 对connfd对应的util_timer做初始化
        - 设置address和sockfd
        - 定义一个新的临时util_timer，初始化其内容
            - user_data，即对应的users_timer
            - cb_func，设置回调函数
            - expire，设置计数时间
        - 对应的util_timer里的timer就置为这个临时的util_timer
        - 调用add_timer把这个新的timer添加到容器里
5.WebServer类中定义一个函数：deal_timer(util_timer *timer, int sockfd)
    该函数功能：
        调用timer的回调函数，关闭当前连接
        从容器中删除这个timer
6.回调函数在本头文件中定义，cb_func(client_data *user_data)
    该函数功能：
        取消epoll对该连接对应的文件描述符的监听，关闭该文件描述符
7.WebServer类中定义一个函数：dealwithsignal(bool &timeout, bool &stop_server)
    该函数功能：
        从管道的读端（0）读取信号
            - 若信号为SIGALRM，置timeout为true
                这里timeout的作用是为了尽快完成信号处理函数，主线程看到timeout为true的话就知道
                又SIGALRM信号被触发了，然后去执行真正的处理函数timer_handler，执行完以后再把timeout
                变回false
            - 若信号为SIGTERM，置stop_server为true
                若SIGTERM信号产生，终止envetloop的循环
8.WebServer类中定义一个函数：adjust_timer(util_timer *timer)
    该函数功能：
        定时器往后延迟3个单位，并对新的定时器在链表上的位置进行调整
9.eventListen函数
    用socketpair函数设置m_pipefd
    设置写端为非阻塞
    将读端加到epoll监听中
    为SIGPIPE，SIGALRM，SIGTERM注册信号处理函数(后两个是用utils里的sig_handler，往写端里发送信号)
    发送一个alarm信号
10.eventLoop函数
    如果!stop_server
        执行epoll_wait
            - sockfd为m_listenfd，执行timer函数
            - epoll_event为(EPOLLRDHUP | EPOLLHUP | EPOLLERR),执行deal_timer
            - sockfd为管道读端，执行dealwithsignal
            - EPOLLIN，若读成功，执行adjust_timer，否则执行deal_timer
            - EPOLLOUT，若写成功，执行adjust_timer，否则执行deal_timer
        如果timeout
            执行timer_handler()
11.utils里还需要实现的三个函数
sig_handler(int sig)  需要定义一个u_pipefd(static int *)
timer_handler()  需要定义一个m_timer_lst，和一个m_TIMEOUT
cb_func(client_data *user_data) 需要定义一个u_epollfd（static int）

*/