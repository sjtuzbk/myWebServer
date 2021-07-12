#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <string>
#include <cassert>
#include <time.h>
#include "threadpool/threadpool.h"
#include "http/http_conn.h"
#include "timer/lst_timer.h"

#define MAX_FD 65535
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void thread_pool();
    void init(int port, string user, string passwd, string dbname, int sql_num);
    void sql_pool();
    void eventListen();
    void eventLoop();

public:
    //数据库相关
    connection_pool* m_connpool;
    string m_user;
    string m_passwd;
    string m_dbname;
    int m_sql_num;

private:
    //基本信息
    int m_port;
    http_conn* users;

    //线程池相关
    threadpool<http_conn>* m_pool;

    //监听与时间循环
    struct epoll_event events[MAX_EVENT_NUMBER];
    int m_listenfd;
    int m_epollfd;

    //定时器相关
    Utils utils;
    int m_pipefd[2];
    client_data* users_timer;

private:
    void set_timer(int connfd, struct sockaddr_in client_address);

    void deal_timer(util_timer* timer, int sockfd);

    void update_timer(util_timer* timer);

    bool dealwithsignal(bool &timeout, bool &stop_server);

};