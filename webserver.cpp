#include "webserver.h"

WebServer::WebServer()
{
    users = new http_conn[MAX_FD];
    users_timer = new client_data[MAX_FD];  
}

void WebServer::sql_pool()
{
    m_connpool = connection_pool::get_instance();
    m_connpool->init("localhost", 3306, m_user, m_passwd, m_dbname, m_sql_num);

    users->initmysql_result(m_connpool);
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[0]);
    close(m_pipefd[1]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::thread_pool()
{
    m_pool = new threadpool<http_conn>(m_connpool);
}

void WebServer::init(int port, string user, string passwd, string dbname, int sql_num)
{
    m_port = port;
    m_user = user;
    m_passwd = passwd;
    m_dbname = dbname;
    m_sql_num = sql_num;
}

//为一个新的连接设置定时器
void WebServer::set_timer(int connfd, struct sockaddr_in client_address)
{
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer* timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    time_t cur_time = time(0);
    timer->expire = cur_time + 3 * TIMESLOT;
    timer->cb_func = cb_func;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

void WebServer::deal_timer(util_timer* timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if(timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
}

void WebServer::update_timer(util_timer* timer)
{
    time_t cur = time(0);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    char buf[100];
    ret = recv(m_pipefd[0], buf, 100, 0);
    if(ret == -1)
    {
        return false;
    } else if(ret == 0)
    {
        return false;
    } else {
        for (int i = 0; i < ret; ++i)
        {
            switch (buf[i])
            {
            case SIGALRM:
                timeout = true;
                break;
            case SIGTERM:
                stop_server = true;
                break;
            default:
                break;
            }
        }
    }
    return true;
}

void WebServer::eventListen()
{    
    //处理SIGPIPE信号
    utils.addsig(SIGPIPE, SIG_IGN);

    m_listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //设置端口复用
    int resuse = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &resuse, sizeof(resuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);
    int ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    ret = listen(m_listenfd, 8);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    //创建epoll对象和事件数组
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    epoll_event events[MAX_EVENT_NUMBER];

    utils.addfd(m_epollfd, m_listenfd, false);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false);

    utils.addsig(SIGPIPE, SIG_IGN); //SIG_IGN是忽略
    utils.addsig(SIGALRM, utils.sig_handler);
    utils.addsig(SIGTERM, utils.sig_handler);

    alarm(TIMESLOT);

    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::eventLoop()
{
    bool stop_server = false;
    bool timeout = false;
     while(!stop_server)
     {
        int num = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        
        if(num < 0 && errno != EINTR)
        {
            perror("epoll");
            break;
        }
        for(int i = 0; i < num; ++i)
        {
            int sockfd = events[i].data.fd;

            if(sockfd == m_listenfd)
            {
                //监听到有新的连接
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlen);

                if(http_conn::m_user_count >= MAX_FD){
                    close(connfd);
                    continue;
                }

                users[connfd].init(connfd, client_address);
                set_timer(connfd, client_address);

            } 
            else if(events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) 
            {
                //对方异常断开或者错误等事件
                // users[sockfd].close_conn();   //被弃用
                deal_timer(users_timer[sockfd].timer, sockfd);
            }
            else if((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                dealwithsignal(timeout, stop_server);
            } 
            else if(events[i].events & EPOLLIN) 
            {
                util_timer* timer = users_timer[sockfd].timer;
                if(users[sockfd].read())
                {
                    //一次性读完，加入到线程池里，读失败就关闭连接
                    m_pool->append(users + sockfd);
                    
                    if(timer)
                    {
                        update_timer(timer);
                    }
                } else {
                    // users[sockfd].close_conn();
                    deal_timer(timer, sockfd);
                }
            } 
            else if(events[i].events & EPOLLOUT) 
            {
                util_timer* timer = users_timer[sockfd].timer;
                if(users[sockfd].write())
                { //一次性写完
                    // users[sockfd].close_conn();
                    if(timer)
                    {
                        update_timer(timer);
                    }
                } else 
                {
                    deal_timer(timer, sockfd);
                }
            }
        }
        if (timeout)
        {
            utils.timer_handler();
            timeout = false;
        }
    }
}