#include "lst_timer.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

sort_timer_lst::~sort_timer_lst()
{
    util_timer* tmp;
    while(head)
    {
        tmp = head;
        head = head->next;
        delete tmp;
    }
}

void sort_timer_lst::add_timer(util_timer* timer)
{
    if(!timer)
        return;
    if(!head)
    {
        head = timer;
        tail = timer;
        return;
    }
    if(timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        head->prev = NULL;
        return;
    } else {
        add_timer(timer, head);
    }
}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head)
{
    if(!timer || !lst_head)
        return;
    util_timer* prev = lst_head;
    util_timer* tmp = lst_head->next;
    while(tmp)
    {
        if(timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = tmp;
            tmp->prev = timer;
            return;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    prev->next = timer;
    timer->prev = prev;
    timer->next = NULL;
    tail = timer;
}

void sort_timer_lst::del_timer(util_timer* timer)
{
    if(!timer)
        return;
    if(timer == head && timer == tail)
    {
        head = tail = NULL;
        delete timer;
        return;
    }
    if(timer == head)
    {
        head = timer->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if(timer == tail)
    {
        tail = timer->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::tick()
{
    if(!head)
        return;
    util_timer* tmp = head;
    while(tmp)
    {
        time_t cur = time(NULL);
        if(cur < tmp->expire)
            break;
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if(head)
            head->prev = NULL;
        delete tmp;
        tmp = head;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

void Utils::addsig(int sig, void (handler)(int)){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

void Utils::addfd(int epollfd, int fd, bool one_shot){
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot){
        //防止同一个socket被不同的线程处理
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符为非阻塞
    setnonblocking(fd);
}

void Utils::setnonblocking(int fd){
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flag);
}

//从epoll中删除要监听的文件描述符
void Utils::removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void Utils::modfd(int epollfd, int fd, int ev){
    struct epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLIN | EPOLLONESHOT | EPOLLHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int Utils::u_epollfd = 0;
int *Utils::u_pipefd = 0;

void Utils::sig_handler(int sig)
{
    int errno_save = errno;
    int msg = sig;
    int ret = send(u_pipefd[1], (void *)&msg, 1, 0);
    errno = errno_save;
}

void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void cb_func(client_data* user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    --http_conn::m_user_count;
}
