#ifndef HTTP_CONN_H
#define HTTP_CONN_H
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <map>
#include <string>
#include "../threadpool/threadpool.h"
#include "../sql_pool/sql_connection_pool.h"

class http_conn
{
public:
    static int m_epollfd;   //所有socket事件都被注册到同一个epoll对象中
    static int m_user_count;    //统计用户的数量
    static const int READ_BUF_SIZE = 2048;
    static const int WRITE_BUF_SIZE = 2048;
    static const int FILENAME_LEN = 200;
    // HTTP请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
public:
    http_conn();
    ~http_conn();

    void process();
    void init(int sockfd, struct sockaddr_in & addr);           //初始化新接受的连接
    void close_conn();                                          //关闭连接
    bool read();                                                //非阻塞的读
    bool write();                                               //非阻塞的写
    void initmysql_result(connection_pool *connPool);

    MYSQL* mysql;
private:
    void init();                                                //初始化解析参数
    char* get_line() {return m_read_buf + m_start_line;}

    HTTP_CODE process_read();                                   //解析HTTP报文
    HTTP_CODE parse_request_line(char * text);                  //解析请求行
    HTTP_CODE parse_headers(char * text);                       //解析头部
    HTTP_CODE parse_content(char * text);                       //解析内容
    LINE_STATUS parse_line();                                   //解析一行
    HTTP_CODE do_request();

    void unmap();
    bool process_write(HTTP_CODE ret);                          //生成响应报文
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_lenght );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

private:
    int m_sockfd;                                   //当前http连接的socket
    struct sockaddr_in m_address;                   //通信的socket地址
    char m_read_buf[READ_BUF_SIZE];                 //读缓冲区
    int m_read_idx;                                 //标示读缓冲区中已经读入的客户端的数据的最后一个字节的下一个位置
    
    int m_checked_idx;                              //当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                               //正在解析的行的起始位置
    CHECK_STATE m_checked_status;                   //主状态机的状态

    METHOD m_method;                                 //请求方法
    char* m_url;                                    //请求的url
    char* m_version;                                //请求行的版本号
    char* m_host;                                     //请求头里的Host
    int m_content_length;                           //请求头里的Content-Length
    bool m_linger;                                  //请求头里的Connection
    char* m_content;                                //存储请求的content部分

    char m_real_file[FILENAME_LEN];                 //客户请求的目标文件的完整路径
    struct stat m_file_stat;                        //目标文件的状态
    char* m_file_address;                           //客户请求的目标文件被mmap映射在内存中的起始位置

    char m_write_buf[WRITE_BUF_SIZE];               //写缓冲区
    int m_write_idx;                                //写缓冲区中待发送的字节数
    struct iovec m_iv[2];                           //使用writev的方式来执行写操作
    int m_iv_count;
    int bytes_have_send;                            // 已经发送的字节
    int bytes_to_send;                              // 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数

    //数据库相关
    char sql_user[100];                             //连接数据库需要的用户名
    char sql_passwd[100];                           //连接数据库的用户密码
    char sql_dbname[100];                           //选择的数据库的名称
};

#endif