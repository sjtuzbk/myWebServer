#include "http_conn.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
//网站的根目录
const char * doc_root = "/home/user/C++projects/lessons/webserver/resources";

int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

map<string, string> users;                    //表中存储的用户名-密码键值对
locker m_lock;

void setnonblocking(int fd){
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, flag);
}

void addfd(int epollfd, int fd, bool one_shot){
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

//从epoll中删除要监听的文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev){
    struct epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLIN | EPOLLONESHOT | EPOLLHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

http_conn::http_conn()
{
}

http_conn::~http_conn()
{
}

void http_conn::initmysql_result(connection_pool* connPool){

    connectionRAII mysqlcon(&mysql, connPool);

    if(mysql_query(mysql, "SELECT username, passwd FROM user")){
        cout << "mysql error : mysql query." << endl;
    }

    MYSQL_RES* res = mysql_store_result(mysql);

    while(MYSQL_ROW row = mysql_fetch_row(res)){
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//初始化文件描述符，修改静态变量
void http_conn::init(int sockfd, struct sockaddr_in & addr){
    m_sockfd = sockfd;
    m_address = addr;

    int resuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &resuse, sizeof(resuse));
    addfd(m_epollfd, m_sockfd, true);
    ++m_user_count;
    init();
}
//初始化解析参数
void http_conn::init(){
    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;
    m_checked_status = CHECK_STATE_REQUESTLINE;
    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_file_address = NULL;
    m_content = NULL;
    m_content_length = 0;
    m_linger = false;
    m_write_idx = 0;
    bytes_have_send = 0;
    bytes_to_send = 0;
    mysql = NULL;

    bzero(m_read_buf, READ_BUF_SIZE);
    bzero(m_real_file, FILENAME_LEN);
    bzero(m_write_buf, WRITE_BUF_SIZE);
}

void http_conn::close_conn(){
    if(m_sockfd != -1){
        removefd(m_epollfd, m_sockfd);
        --m_user_count;
        m_sockfd = -1;
    }
}

http_conn::HTTP_CODE http_conn::process_read(){
    HTTP_CODE ret = NO_REQUEST;
    LINE_STATUS line_status = LINE_OK;
    char* text = 0;

    while(((m_checked_status == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) ||
            ((line_status = parse_line()) == LINE_OK)){
        text = get_line();
        m_start_line = m_checked_idx;
        switch (m_checked_status)
        {
        case CHECK_STATE_REQUESTLINE:{
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER: {
            ret = parse_headers(text);
            if(ret == BAD_REQUEST){
                return BAD_REQUEST;
            } else if(ret == GET_REQUEST){
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT: {
            ret = parse_content(text);
            if(ret == GET_REQUEST){
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:{
            return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \r");
    if(!m_url){
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    // GET\0/index.html HTTP/1.1
    char* method;
    method = text;
    if(strcasecmp(method, "GET") == 0){
        m_method = GET;
    } else if(strcasecmp(method, "POST") == 0){
        m_method = POST;
    } else {
        return BAD_REQUEST;
    }
    ///index.html HTTP/1.1
    m_version = strpbrk(m_url, " \r");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if(strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strpbrk(m_url, "/");
    }
    if(strncasecmp(m_url, "https://", 8) == 0){
        m_url += 8;
        m_url = strpbrk(m_url, "/"); 
    }
    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }
    if(strlen(m_url) == 1){
        strcat(m_url, "judge.html");
    }
    m_checked_status = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    //遇到空行，解析头部字段结束
    if(text[0] == '\0'){
        //如果Content-Length为0，说明请求中没有实体信息，返回GET_REQUEST，否则将主状态机的状态置为检测content
        if(m_content_length == 0){
            return GET_REQUEST;
        }
        m_checked_status = CHECK_STATE_CONTENT;
        return NO_REQUEST;
    } else if(strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        text += strspn(text, " \t");  // strspn 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
        if(strcasecmp(text, "keep-alive") == 0){
            m_linger = true;
        }
    } else if(strncasecmp(text, "Content-Length:", 15) == 0){
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atoi(text);
    } else if(strncasecmp(text, "Host:", 5) == 0){
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        //  printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}
//解析消息体
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        m_content = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
//解析一行,行以\r\n分隔
http_conn::LINE_STATUS http_conn::parse_line()
{
    char tmp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        tmp = m_read_buf[m_checked_idx];
        if(tmp == '\r')
        {
            if(m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            } else if(m_read_buf[m_checked_idx+1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if(tmp == '\n')
        {
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx-1] == '\r')
            {
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            } else {
                return LINE_BAD;
            }
        }
    }
    return LINE_OPEN;
}
http_conn::HTTP_CODE http_conn::do_request(){
    // /home/user/C++projects/lessons/webserver/resources
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    char* p = strrchr(m_url, '/');

    if(m_method == POST && (*(p+1) == '2' || *(p+1) == '3')){
        //从m_content中提取用户名和密码
        //user=123&password=123
        char username[100];
        char passwd[100];
        int i = 0;

        for(i = 5; m_content[i] != '&'; ++i){
            username[i-5] = m_content[i];
        }
        username[i-5] = '\0';

        int j = 0;
        for(i += 10; m_content[i] != '\0'; ++i, ++j){
            passwd[j] = m_content[i];
        }
        passwd[j] = '\0';

        if(*(p+1) == '3'){
            //判断是否有重名
            if(users.find(username) == users.end()){
                char* sql_instert = (char*)malloc(sizeof(char) * 200);
                strcpy(sql_instert, "INSERT INTO user(username, passwd) VALUES('");
                strcat(sql_instert, username);
                strcat(sql_instert, "', '");
                strcat(sql_instert, passwd);
                strcat(sql_instert, "')");

                m_lock.lock();
                int ret = mysql_query(mysql, sql_instert);

                users.insert(pair<string, string>(username, passwd));
                m_lock.unlock();

                if(!ret){
                    strcpy(m_url, "/log.html");
                } else {
                    strcpy(m_url, "/registerError.html");
                }
            } else {
                strcpy(m_url, "/registerError.html");
            }
        } else {
            if(users.find(username) != users.end() && users[username] == passwd){
                strcpy(m_url, "/welcome.html");
            } else {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    if(*(p+1) == '0')
    {
        char* m_real_url = (char*)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/register.html");
        strncpy(m_real_file + len, m_real_url, FILENAME_LEN - 1 - len);
        free(m_real_url);
    } else if(*(p+1) == '0')
    {
        char* m_real_url = (char*)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/register.html");
        strncpy(m_real_file + len, m_real_url, FILENAME_LEN - 1 - len);
        free(m_real_url);
    } else if(*(p+1) == '1')
    {
        char* m_real_url = (char*)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/log.html");
        strncpy(m_real_file + len, m_real_url, FILENAME_LEN - 1 - len);
        free(m_real_url);
    } else if(*(p+1) == '5')
    {
        char* m_real_url = (char*)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/picture.html");
        strncpy(m_real_file + len, m_real_url, FILENAME_LEN - 1 - len);
        free(m_real_url);
    } else if(*(p+1) == '6')
    {
        char* m_real_url = (char*)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/video.html");
        strncpy(m_real_file + len, m_real_url, FILENAME_LEN - 1 - len);
        free(m_real_url);
    } else if(*(p+1) == '7')
    {
        char* m_real_url = (char*)malloc(sizeof(char) * 200);
        strcpy(m_real_url, "/fans.html");
        strncpy(m_real_file + len, m_real_url, FILENAME_LEN - 1 - len);
        free(m_real_url);
    } else 
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - 1 - len);
    }
    
    if( stat(m_real_file, &m_file_stat) < 0 ){
        return NO_RESOURCE;
    }

    if( !(m_file_stat.st_mode & S_IROTH )){
        return FORBIDDEN_REQUEST;
    }

    if( S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}
//往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char* format, ...){
    if(m_write_idx >= WRITE_BUF_SIZE){
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUF_SIZE - 1 - m_write_idx, format, arg_list);
    if(len >= ( WRITE_BUF_SIZE - 1 - m_write_idx )){
        return false;
    }
    m_write_idx += len;
    return true;
}

bool http_conn::add_status_line(int status, const char* title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            if(m_file_stat.st_size != 0){
                add_headers(m_file_stat.st_size);
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_idx;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;

                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            } else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                    return false;
            }
        default:
            return false;
    }
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

bool http_conn::read(){
    if(m_read_idx >= READ_BUF_SIZE){
        printf("读缓冲区已满...\n");
        return false;
    }
    while(true){
        int read_bytes = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUF_SIZE - m_read_idx, 0);
        if(read_bytes == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        } else if(read_bytes == 0){  //对方关闭了连接
            return false;
        }
        m_read_idx += read_bytes;
    }
    return true;
}
//写HTTP响应
bool http_conn::write(){
    int temp = 0;
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_have_send >= m_iv[0].iov_len){
            //头已经发送完毕
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        if ( bytes_to_send <= 0 ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

void http_conn::process(){
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    //生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}