#include "webserver.h"

#define PORT 8888

int main(int argc, char* argv[])
{
    WebServer server;

    //设置端口号
    server.init(PORT, "root", "Abcd1234", "webserver", 8);

    //开启数据库连接池
    server.sql_pool();

    //开辟线程池
    server.thread_pool();

    //监听
    server.eventListen();

    //事件循环
    server.eventLoop();

    return 0;
}