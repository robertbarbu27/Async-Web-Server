#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <assert.h>

#include <sys/types.h>

#include <unistd.h>

#include <fcntl.h>

#include <sys/stat.h>

#include <sys/epoll.h>

#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>

#include <sys/sendfile.h>

#include <sys/eventfd.h>

#include <libaio.h>

#include <errno.h>

#include <string.h>

#include "aws.h"

#include "utils/util.h"

#include "utils/debug.h"

#include "utils/sock_util.h"

#include "utils/w_epoll.h"

#define ERROR_MSG "HTTP/1.1 404 Not Found\r\n\r\n"
#define OK_MSG "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: %ld\r\n\r\n"
int dynamic = 0;
int connection_send_data(struct connection * conn);
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;


static int aws_on_path_cb(http_parser * p,
  const char * buf, size_t len) {
  struct connection * conn = (struct connection * ) p -> data;

  memcpy(conn -> request_path, buf, len);
  conn -> request_path[len] = '\0';
  conn -> have_path = 1;

  return 0;
}

static void connection_prepare_send_reply_header(struct connection * conn) {
  dlog(LOG_DEBUG, "connection_prepare_send_reply_header\n");
  sprintf(conn -> send_buffer, OK_MSG, conn -> file_size);
  conn -> send_len = strlen(conn -> send_buffer);

}

static void connection_prepare_send_404(struct connection * conn) {
  dlog(LOG_DEBUG, "connection_prepare_send_404\n");
  memcpy(conn -> send_buffer, ERROR_MSG, strlen(ERROR_MSG));
  conn -> send_len = strlen(ERROR_MSG);

}

static enum resource_type connection_get_resource_type(struct connection * conn) {
  if (strstr(conn -> request_path, "static") != NULL) {
    return RESOURCE_TYPE_STATIC;
  }

  if (strstr(conn -> request_path, "dynamic") != NULL) {
    return RESOURCE_TYPE_DYNAMIC;
  }
  return RESOURCE_TYPE_NONE;
}
struct connection * connection_create(int sockfd) {
  /* TODO: Initialize connection structure on given socket. */
  struct connection * conn = calloc(1, sizeof(struct connection));
  DIE(conn == NULL, "calloc");
  conn -> sockfd = sockfd;
  conn -> state = STATE_INITIAL;
  return conn;
}

void connection_start_async_io(struct connection * conn) {

  // Set up the iocb for asynchronous read operation

}

void connection_remove(struct connection * conn) {
  close(conn -> sockfd);
  if (conn -> fd >= 0)
    close(conn -> fd);
  free(conn);
}

void handle_new_connection(void) {
  static int sockfd;
  socklen_t addrlen = sizeof(struct sockaddr_in);
  struct sockaddr_in addr;
  struct connection * conn;
  int rc;

  sockfd = accept(listenfd, (struct sockaddr * ) & addr, & addrlen);
  DIE(sockfd < 0, "accept");
  int flags = fcntl(sockfd, F_GETFL, 0);
  flags = flags | O_NONBLOCK;

  fcntl(sockfd, F_SETFL, flags);

  conn = connection_create(sockfd);

  rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
  DIE(rc < 0, "w_epoll_add_in");

  http_parser_init( & conn -> request_parser, HTTP_REQUEST);
  conn -> request_parser.data = conn;

}

void receive_data(struct connection * conn) {
  int rc;
  char buffer[64];
  ssize_t bytes_recv = 0;
  rc = get_peer_address(conn -> sockfd, buffer, 64);
  if (rc < 0) {
    ERR("get_peer_adress");
    goto remove_connection;
  }

  while (strstr(conn -> recv_buffer, "\r\n\r\n") == NULL) {
    ssize_t received = recv(conn -> sockfd, conn -> recv_buffer + bytes_recv, BUFSIZ, 0);
    if (received <= 0 ) {
      break;
    }
    bytes_recv += received;
  }

  dlog(LOG_DEBUG, "Received message from: %s\n", buffer);

  conn -> recv_len = bytes_recv;
  conn -> state = STATE_REQUEST_RECEIVED;

  return;

  remove_connection:
    rc = w_epoll_remove_ptr(epollfd, conn -> sockfd, conn);
  DIE(rc < 0, "w_epoll_remove_ptr");

  connection_remove(conn);

}

int connection_open_file(struct connection * conn) {

  sprintf(conn -> filename, "%s%s", AWS_DOCUMENT_ROOT, (conn -> request_path + 1));
  dlog(LOG_DEBUG, "connection_open_file");
  dlog(LOG_DEBUG, "filename : %s\n", conn -> filename);
  conn -> fd = open(conn -> filename, O_RDWR);
  if (conn -> fd == -1) {
    connection_prepare_send_404(conn);
    conn -> state = STATE_SENDING_404;
  } else {
	  struct stat st;
  	stat(conn -> filename, & st);
    conn -> file_size = st.st_size;
    connection_prepare_send_reply_header(conn);
    conn -> state = STATE_SENDING_HEADER;
  }
  int rc = w_epoll_update_ptr_inout(epollfd, conn -> sockfd, conn);
  DIE(rc < 0, "epoll ptr add error");

  return conn -> fd;
}
void connection_complete_async_io(struct connection * conn) {
  dlog(LOG_DEBUG, "connection_complete_async_io\n");
  ssize_t total_bytes_sent = 0;
  dynamic = 1;
  conn -> send_len = conn -> file_size;
  connection_send_data(conn);
  dynamic = 0;
  dlog(LOG_DEBUG, "Here\n");
  conn -> state = STATE_DATA_SENT;
  w_epoll_remove_ptr(epollfd, conn -> eventfd, conn);
  w_epoll_remove_ptr(epollfd, conn -> sockfd, conn);
  io_destroy(conn -> ctx);
  close(conn -> eventfd);
  dlog(LOG_DEBUG, "Here\n");

}

int parse_header(struct connection * conn) {
  http_parser_settings settings_on_path = {
    .on_message_begin = 0,
    .on_header_field = 0,
    .on_header_value = 0,
    .on_path = aws_on_path_cb,
    .on_url = 0,
    .on_fragment = 0,
    .on_query_string = 0,
    .on_body = 0,
    .on_headers_complete = 0,
    .on_message_complete = 0

  };
  ssize_t parsed = http_parser_execute( & conn -> request_parser, & settings_on_path, conn -> recv_buffer, conn -> recv_len);
  conn -> res_type = connection_get_resource_type(conn);
  return 0;
}

enum connection_state connection_send_static(struct connection * conn) {
  dlog(LOG_DEBUG, "connection_send_static\n");
  ssize_t total_bytes_sent = 0;
  while (total_bytes_sent < conn -> file_size) {
    dlog(LOG_DEBUG, "conn->file_size %d\n", conn -> file_size);
    ssize_t bytes_sent = sendfile(conn -> sockfd, conn -> fd, & conn -> file_pos, conn -> file_size);
    if (bytes_sent < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
      dlog(LOG_ERR, "Error");
      break;
    }
    if (bytes_sent == 0) {
      break;
    }
    total_bytes_sent += bytes_sent;
  }
  int rc = w_epoll_update_ptr_in(epollfd, conn -> sockfd, conn);
  conn -> state = STATE_DATA_SENT;
  return STATE_DATA_SENT;

}

int connection_send_data(struct connection * conn) {
 ssize_t bytes_sent = 0;
 int rc;
  while (bytes_sent < conn -> send_len) {
	ssize_t bytes;
	if(dynamic == 1){
     bytes = send(conn -> sockfd, conn -> dynamic_buffer + bytes_sent, conn -> send_len - bytes_sent, 0);
	}
	else{
		 bytes = send(conn -> sockfd, conn -> send_buffer + bytes_sent, conn -> send_len - bytes_sent, 0);
	}
    if (bytes <= 0 && dynamic == 0) {
      dlog(LOG_ERR, "Log error in sending data\n");
      goto remove_connection;
    }
	if(bytes > 0){
    bytes_sent += bytes;
	}
  }


  if (bytes_sent == conn -> send_len) {
    conn -> state = STATE_DATA_SENT;
  }

  return STATE_DATA_SENT;

  remove_connection:
 rc = w_epoll_remove_ptr(epollfd, conn -> sockfd, conn);
  DIE(rc < 0, "w_epoll_remove_ptr");

  /* remove current connection */
  connection_remove(conn);

  return STATE_CONNECTION_CLOSED;
}

void connection_send_dynamic(struct connection * conn) {
  dlog(LOG_DEBUG, "sending_dynamic\n");
  conn -> eventfd = eventfd(0, O_NONBLOCK);
  io_setup(1, & conn -> ctx);
  conn -> piocb[0] = & conn -> iocb;

  
  dlog(LOG_DEBUG, "filename: %s\n", conn -> filename);
  conn -> fd = open(conn -> filename, O_RDWR);

  if (conn -> fd == -1) {
    connection_prepare_send_404(conn);
    conn -> state = STATE_SENDING_404;
    return;
  }

  dlog(LOG_DEBUG, "file_size: %d\n", conn -> file_size);
  conn -> dynamic_buffer = calloc(sizeof(char), conn -> file_size + 5);
  ssize_t total_bytes_sent = 0;

  dlog(LOG_DEBUG, "parsed: %s", conn -> dynamic_buffer);
  io_prep_pread( & conn -> iocb, conn -> fd, conn -> dynamic_buffer, conn -> file_size, 0);
  io_set_eventfd( & conn -> iocb, conn -> eventfd);
  int rc = w_epoll_add_ptr_in(epollfd, conn -> eventfd, conn);

  io_submit(conn -> ctx, 1, conn -> piocb);
  conn -> state = STATE_ASYNC_ONGOING;

}

void handle_input(struct connection * conn) {
  receive_data(conn);

  switch (conn -> state) {
  case STATE_REQUEST_RECEIVED:
    parse_header(conn);
    connection_open_file(conn);
    break;

  case STATE_CONNECTION_CLOSED:
    break;
  default:
    fprintf(stderr, "Unexpected state %d\n", conn -> state);
    exit(1);
  }

}
void handle_output(struct connection * conn) {
  dlog(LOG_DEBUG, "handling_mata\n");

  switch(conn -> state){
	case STATE_SENDING_404:
		connection_prepare_send_404(conn);
		connection_send_data(conn);
		conn -> state = STATE_DATA_SENT;
		handle_output(conn);
		break;
	case STATE_SENDING_HEADER:
		connection_prepare_send_reply_header(conn);
		connection_send_data(conn);
		conn -> state = STATE_HEADER_SENT;
		handle_output(conn);
		break;
	case STATE_HEADER_SENT:
		if(conn -> res_type == RESOURCE_TYPE_STATIC){
			connection_send_static(conn);
		}
		else if(conn -> res_type == RESOURCE_TYPE_DYNAMIC){
			connection_send_dynamic(conn);
			connection_complete_async_io(conn);
		}
		else{
			connection_remove(conn);
			return;
		}
		conn -> state = STATE_DATA_SENT;
		handle_output(conn);
	break;
	case STATE_DATA_SENT:
		connection_remove(conn);
		break;
	
	default:
		exit(1);

  }
}

void handle_client(uint32_t event, struct connection * conn) {
  if (event & EPOLLIN) {
    dlog(LOG_DEBUG, "New message\n");
    handle_input(conn);
  }
  if (event & EPOLLOUT) {
    dlog(LOG_DEBUG, "Ready to send message\n");
    handle_output(conn);
  }
}

int main(void) {
  int rc;

  // Init multiplexing
  epollfd = w_epoll_create();
  DIE(epollfd < 0, "w_epoll_create");

  // Create server socket
  listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
  DIE(listenfd < 0, "tcp_create_listener");

  // Make the socket non-blocking
  fcntl(listenfd, F_SETFL, O_RDWR | O_NONBLOCK);

  rc = w_epoll_add_fd_in(epollfd, listenfd);
  DIE(rc < 0, "w_epoll_add_fd_in");

  // Server main loop
  while (1) {
    struct epoll_event rev;
    rc = w_epoll_wait_infinite(epollfd, & rev);
    DIE(rc < 0, "w_epoll_wait_infinite");
    dlog(LOG_DEBUG, "here\n");

    if (rev.data.fd == listenfd) {
      dlog(LOG_DEBUG, "New connection\n");
      if (rev.events & EPOLLIN)
        handle_new_connection();
    } else {
      handle_client(rev.events, (struct connection * ) rev.data.ptr);

    }
  }
  return 0;
}
