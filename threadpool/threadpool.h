#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <cstdio>
#include <exception>
#include "../lock/locker.h"
#include "../sql_pool/sql_connection_pool.h"
#include <list>

//T代表任务类
template <typename T>
class threadpool{
public:
    threadpool(connection_pool *connPool, int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);

private:
    static void* worker(void* arg);
    void run();

private:
    //线程的数量
    int m_thread_number;

    //描述线程池的数组，大小为m_thread_number
    pthread_t* m_threads;

    //请求队列中最多允许的、等待请求的数量
    int m_max_requests;

    //请求队列
    std::list<T *> m_workqueue;

    //保护请求队列的锁
    locker m_queuelocker;

    //是否有任务需要处理的信号量
    sem m_queuestat;

    //是否停止线程
    bool m_stop;

    //数据库连接池
    connection_pool* m_connPool;
};

template <typename T>
threadpool<T>::threadpool(connection_pool* connPool, int thread_number, int max_requests) :
        m_connPool(connPool), m_thread_number(thread_number), m_max_requests(max_requests), 
        m_stop(false), m_threads(NULL){
    if(m_thread_number <= 0 || m_max_requests <= 0){
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if(m_threads == NULL){
        throw std::exception();
    }

    for(int i = 0; i < m_thread_number; ++i){
        printf("create the %dth thread...\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete[] m_threads;
            throw std::exception();
        }

        if(pthread_detach(m_threads[i]) != 0){
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop = true;     //析构函数调用完这个值的不存在了，还设置它有什么意义呢？
}

template <typename T>
bool threadpool<T>::append(T* request) {
    m_queuelocker.lock();
    if(m_workqueue.size() >= m_max_requests){
        printf("append failed : the workqueue has been full.\n");
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();   //这里unlock和post的顺序能否互换？
    m_queuestat.post();
    return true;
}


template <typename T>
void* threadpool<T>::worker(void* arg){
    threadpool<T>* pool = (threadpool<T>*) arg;  //这里是threadpool*还是threadpool<T>*
    pool->run();
    return pool;   //这里为什么返回pool，返回NULL不行吗
}

template <typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request){
            continue;
        }
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        request->process();
    }
}


#endif