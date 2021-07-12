#ifndef SQL_CONNECTION_POOL_H
#define SQL_CONNECTION_POOL_H

#include <mysql/mysql.h>
#include <string>
#include <list>
#include <iostream>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
public:
    static connection_pool* get_instance();

    void init(string url, int Port, string User, string Passwd, string DBName, int MaxConn);
    MYSQL* getConnection();    //获取数据库连接
    int getFreeConn();         //获取空闲连接数
    bool releaseConnection(MYSQL* conn);    //释放连接
    void destroyPool();                     //销毁所有连接

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;   //最大连接数
    int m_CurConn;   //当前已使用的连接数
    int m_FreeConn;  //当前空闲的连接数
    list<MYSQL*> connList;
    locker lock;
    sem reserve;

public:
    string m_url;
    int m_Port;
    string m_User;
    string m_Passwd;
    string m_DBName;

};

//RAII  resource acquisition is initialzation
class connectionRAII
{
public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL* connRAII;
    connection_pool* poolRAII;
};


#endif