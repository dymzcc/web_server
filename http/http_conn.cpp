#include "http_conn.h"

#include <string.h>

/* 定义http响应的一些状态信息 */
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form =
    "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form =
    "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form =
    "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form =
    "There was an unusual problem serving the request file.\n";

/* 网站的根目录 */
const char *doc_root = "/root/cpp_workspace/workspace/webserver/file";

/* 设置文件非阻塞 */
int setnonblocking(int fd) {
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
  return old_option;
}

/* 往epoll内核注册表上注册事件 */
void addfd(int epollfd, int fd, bool one_shot) {
  epoll_event event;
  event.data.fd = fd;
  event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
  if (one_shot) {
    event.events |= EPOLLONESHOT;
  }
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  setnonblocking(fd);
}

/* 从epoll内核注册表上移除事件 */
void removedfd(int epollfd, int fd) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
  close(fd); /*  */
}

/* 在epoll内核注册表上更改监听的事件 */
void modfd(int epollfd, int fd, int ev) {
  epoll_event event;
  event.data.fd = fd;
  event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

/* 关闭一个连接 */
void http_conn::close_conn(bool real_close) {
  if (real_close && (m_sockfd != -1)) {
    removedfd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    m_user_count--; /* 关闭一个连接时，将客户总量-1 */
  }
}

void http_conn::init(int sockfd, const sockaddr_in &addr) {
  m_sockfd = sockfd;
  m_address = addr;
  addfd(m_epollfd, sockfd, true);
  m_user_count++;
  init();
}

void http_conn::init() {
  m_check_state = CHECK_STATE_REQUESTLINE;
  m_linger = false;

  m_method = GET;
  m_url = 0;
  m_version = 0;
  m_content_length = 0;
  m_host = 0;
  m_start_line = 0;
  m_checked_idx = 0;
  m_read_idx = 0;
  m_write_idx = 0;

  memset(m_read_buf, '\0', READ_BUFFER_SIZE);
  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
  memset(m_real_file, '\0', FILENAME_LEN);
}

/* 从状态机 */
http_conn::LINE_STATUS http_conn::parse_line() {
  char temp;
  for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
    temp = m_read_buf[m_checked_idx];
    if (temp == '\r') {
      if ((m_checked_idx + 1) == m_read_idx)
        return LINE_OPEN;
      else if (m_read_buf[m_checked_idx + 1] == '\n') {
        m_read_buf[m_checked_idx++] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    } else if (temp == '\n') {
      if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
        m_read_buf[m_checked_idx - 1] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    }
  }
  return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read() {
  if (m_read_idx >= READ_BUFFER_SIZE) {
    return false;
  }
  int bytes_read = 0;

  // ET读数据
  while (true) {
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                      READ_BUFFER_SIZE - m_read_idx, 0);
    if (bytes_read == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return false;
    } else if (bytes_read == 0) {
      return false;
    }
    m_read_idx += bytes_read;
  }
  return true;
}

/* 解析http请求行，获得请求方法，目标url及http版本号 */
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
  m_url = strpbrk(text, " \t");
  if (!m_url) {
    return BAD_REQUEST;
  }
  *m_url++ = '\0';
  char *method = text;

  if (strcasecmp(method, "GET") == 0) {
    m_method = GET;
  } else {
    return BAD_REQUEST;
  }
  m_url += strspn(m_url, " \t");
  m_version = strpbrk(m_url, " \t");
  if (!m_version) {
    return BAD_REQUEST;
  }
  *m_version++ = '\0';
  m_version += strspn(m_version, " \t");
  if (strcasecmp(m_version, "HTTP/1.1") != 0) {
    return BAD_REQUEST;
  }
  if (strncasecmp(m_url, "http://", 7) == 0) {
    m_url += 7;
    m_url = strchr(m_url, '/');
  }

  if (strncasecmp(m_url, "https://", 8) == 0) {
    m_url += 8;
    m_url = strchr(m_url, '/');
  }

  if (!m_url || m_url[0] != '/') {
    return BAD_REQUEST;
  }
  //当url为/时，显示判断界面
  if (strlen(m_url) == 1) {
    strcat(m_url, "judge.html");
  }
  m_check_state = CHECK_STATE_HEADER;
  return NO_REQUEST;
}

/* 解析http请求的一个头部信息 */
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
  if (text[0] == '\0') {
    if (m_content_length != 0) {
      m_check_state = CHECK_STATE_CONTENT;
      return NO_REQUEST;
    }
    return GET_REQUEST;
  } else if (strncasecmp(text, "Connection:", 11) == 0) {
    text += 11;
    text += strspn(text, " \t");
    if (strcasecmp(text, "keep-alive") == 0) {
      m_linger = true;
    }
  } else if (strncasecmp(text, "Content-length:", 15) == 0) {
    text += 15;
    text += strspn(text, " \t");
    m_content_length = atol(text);
  } else if (strncasecmp(text, "Host:", 5) == 0) {
    text += 5;
    text += strspn(text, " \t");
    m_host = text;
  } else {
    printf("oop!unknow header: %s\n", text);
  }
  return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
  if (m_read_idx >= (m_content_length + m_checked_idx)) {
    text[m_content_length] = '\0';
    return GET_REQUEST;
  }
  return NO_REQUEST;
}

/* 主状态机 */
http_conn::HTTP_CODE http_conn::process_read() {
  LINE_STATUS line_status = LINE_OK;
  HTTP_CODE ret = NO_REQUEST;
  char *text = 0;

  while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
         ((line_status = parse_line()) == LINE_OK)) {
    text = get_line();
    m_start_line = m_checked_idx;
    printf("%s\n", text);
    switch (m_check_state) {
      case CHECK_STATE_REQUESTLINE: {
        ret = parse_request_line(text);
        if (ret == BAD_REQUEST) return BAD_REQUEST;
        break;
      }
      case CHECK_STATE_HEADER: {
        ret = parse_headers(text);
        if (ret == BAD_REQUEST)
          return BAD_REQUEST;
        else if (ret == GET_REQUEST) {
          return do_request();
        }
        break;
      }
      case CHECK_STATE_CONTENT: {
        ret = parse_content(text);
        if (ret == GET_REQUEST) return do_request();
        line_status = LINE_OPEN;
        break;
      }
      default:
        return INTERNAL_ERROR;
    }
  }
  return NO_REQUEST;
}

/* 当得到一个完整的、正确的HTTP请求时，我们就分析目标文件的属性。如果目标文件存在，对所有用户可读，
且不是目录，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功
 */
http_conn::HTTP_CODE http_conn::do_request() {
  strcpy(m_real_file, doc_root);
  int len = strlen(doc_root);
  printf("m_url:%s\n", m_url);
  strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
  printf("m_real_file:%s\n", m_real_file);
  if (stat(m_real_file, &m_file_stat) < 0) {
    return NO_RESOURCE;
  }
  if (!(m_file_stat.st_mode & S_IROTH)) {
    return FORBIDDEN_REQUEST;
  }
  if (S_ISDIR(m_file_stat.st_mode)) {
    return BAD_REQUEST;
  }
  
  int fd = open(m_real_file, O_RDONLY);
  m_file_address =
      (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  return FILE_REQUEST;
}

/* 对内存映射区执行munmap操作 */
void http_conn::unmap() {
  if (m_file_address) {
    munmap(m_file_address, m_file_stat.st_size);
    m_file_address = 0;
  }
}

/* 写HTTP响应 */
bool http_conn::write() {
  
  int temp = 0;
  int bytes_have_send = 0;
  int bytes_to_send = m_write_idx;

  if (bytes_to_send == 0) {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    init();
    return true;
  }

  while (1) {
    temp = writev(m_sockfd, m_iv, m_iv_count);

    if (temp <= -1) {
      /* 如果TCP写缓冲没有空闲，则等待下一轮EPPOLLOUT事件 */

      if (errno == EAGAIN) {
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return true;
      }
      unmap();
      return false;
    }


    bytes_to_send -= temp;
    bytes_have_send += temp;
    
    if (bytes_to_send <= bytes_have_send) {
      /* 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接 */

      unmap();
      if (m_linger) {
        init();
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return true;
      } else {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return false;
      }
    }
  }
}

bool http_conn::add_response(const char *format, ...) {

  if (m_write_idx >= WRITE_BUFFER_SIZE) {
    return false;
  }
  va_list arg_list;
  va_start(arg_list, format);
  int len = vsnprintf(m_write_buf + m_write_idx,
                      WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
  if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
    return false;
  }
  m_write_idx += len;

  va_end(arg_list);
  return true;
}

bool http_conn::add_status_line(int status, const char *title) {
  return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len) {
  add_content_length(content_len);
  add_linger();
  add_blank_line();
}
bool http_conn::add_content_length(int content_len) {
  return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() {
  return add_response("Connection:%s\r\n",
                      (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }
bool http_conn::add_content(const char *content) {
  return add_response("%s", content);
}

/* 根据服务器处理HTTP请求的结果，决定返回给客户端的内容 */
bool http_conn::process_write(HTTP_CODE ret) {

  switch (ret) {
    case INTERNAL_ERROR: {
      add_status_line(500, error_500_title);
      add_headers(strlen(error_500_form));
      if (!add_content(error_500_form)) return false;
      break;
    }
    case BAD_REQUEST: {
      add_status_line(404, error_404_title);
      add_headers(strlen(error_404_form));
      if (!add_content(error_404_form)) return false;
      break;
    }
    case NO_RESOURCE: {
      add_status_line(404, error_400_title);
      add_headers(strlen(error_404_form));
      if (!add_content(error_404_form)) {
        return false;
      }
      break;
    }
    case FORBIDDEN_REQUEST: {
      add_status_line(403, error_403_title);
      add_headers(strlen(error_403_form));
      if (!add_content(error_403_form)) return false;
      break;
    }
    case FILE_REQUEST: {

      add_status_line(200, ok_200_title);
      if (m_file_stat.st_size != 0) {
        add_headers(m_file_stat.st_size);
        
        /* 第一个iovec指针指向响应报文缓冲区，长度指向m_wirte_idx */
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_idx;
        /* 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小 */
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        return true;
      } else {
        /* 如果请求的资源大小为0，则返回空白html文件 */
        const char *ok_string = "<html><body></body></html>";
        add_headers(strlen(ok_string));
        if (!add_content(ok_string)) return false;
        
      }
    }
    default:
      return false;
  }
  m_iv[0].iov_base = m_write_buf;
  m_iv[0].iov_len = m_write_idx;
  m_iv_count = 1;
  return true;
}

/* 由线程池中的工作线程调用，这是处理HTTP请求的入口函数 */
void http_conn::process() {
  HTTP_CODE read_ret = process_read();
  if (read_ret == NO_RESOURCE) {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    return;
  }
  bool write_ret = process_write(read_ret);
  
  if (!write_ret) {
    close_conn();
  }
  modfd(m_epollfd, m_sockfd, EPOLLOUT);
  
}
