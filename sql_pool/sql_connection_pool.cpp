#include "sql_connection_pool.h"

connection_pool* connection_pool::get_instance()
{
    static connection_pool pool;
    return &pool; 
}

connection_pool::connection_pool()
{
    m_FreeConn = 0;
    m_CurConn = 0;
}

connection_pool::~connection_pool(){
    destroyPool();
}

//初始化数据库参数，创建连接池
void connection_pool::init(string url, int Port, string User, string Passwd, string DBName, int MaxConn)
{
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_Passwd = Passwd;
    m_DBName = DBName;
    m_MaxConn = MaxConn;

    for(int i = 0; i < m_MaxConn; ++i)
    {
        MYSQL* conn = NULL;
        conn = mysql_init(conn);
        if(conn == NULL){
            cout << "error : mysql_init" << endl;
            exit(1);
        }
        mysql_real_connect(conn, url.c_str(),User.c_str(), Passwd.c_str(), DBName.c_str(), Port, NULL, 0);
        if(conn == NULL){
            cout << "error : mysql_real_connect" << endl;
            exit(1);
        }
        m_FreeConn++;
        connList.push_back(conn);
    }
    reserve = sem(MaxConn);
}


//从数据库连接池中取一个可用的空闲连接，更新已用和空闲连接数
MYSQL* connection_pool::getConnection()
{
    if(connList.size() == 0)
        return NULL;

    reserve.wait();
    lock.lock();

    MYSQL* ret = connList.front();
    connList.pop_front();

    m_FreeConn--;
    m_CurConn++;

    lock.unlock();

    return ret;
}

//释放当前使用的连接
bool connection_pool::releaseConnection(MYSQL* conn)
{
    if(conn == NULL)
        return false;
    lock.lock();

    connList.push_back(conn);
    --m_CurConn;
    ++m_FreeConn;

    lock.unlock();
    reserve.post();
    return true;
}

//销毁连接池
void connection_pool::destroyPool()
{
    lock.lock();
    if(connList.size() > 0){
        list<MYSQL*>::iterator it;
        for(it = connList.begin(); it != connList.end(); ++it){
            MYSQL* conn = *it;
            mysql_close(conn);
        }
        m_FreeConn = 0;
        m_CurConn = 0;
        connList.clear();
    }
    lock.unlock();
}

int connection_pool::getFreeConn()
{
    return this->m_FreeConn;
}

connectionRAII::connectionRAII(MYSQL **con, connection_pool *connPool)
{
    *con = connPool->getConnection();
    connRAII = *con;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->releaseConnection(connRAII);
}
