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
#define OK_MSG "HTTP/1.1 200 OK\r\n\r\n"
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

void send_data(struct connection* conn);
static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
    struct connection *conn = (struct connection *)p->data;

    memcpy(conn->request_path, buf, len);
    conn->request_path[len] = '\0';
    conn->have_path = 1;

    return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	dlog(LOG_DEBUG,"connection_send_404");
    memcpy(conn->send_buffer,OK_MSG,strlen(OK_MSG));
	conn->send_len = strlen(OK_MSG);
	conn->send_pos = 0;

}

static void connection_prepare_send_404(struct connection *conn)
{
    memcpy(conn->send_buffer,ERROR_MSG,strlen(ERROR_MSG));
	conn->send_len = strlen(ERROR_MSG);
	conn->send_pos = 0;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
    if (strstr(conn->request_path, "static") != NULL ){
        return RESOURCE_TYPE_STATIC;
		}
    
	if(strstr(conn->request_path,"dynamic") != NULL){
			return RESOURCE_TYPE_DYNAMIC;
	}
	return RESOURCE_TYPE_NONE;
}
struct connection *connection_create(int sockfd)
{
    /* TODO: Initialize connection structure on given socket. */
    struct connection *conn = malloc(sizeof(*conn));

    DIE(conn == NULL, "malloc");

    conn->fd = -1;
    memset(conn->filename, 0, BUFSIZ);

    conn->eventfd = -1;
    conn->sockfd = sockfd;
    io_setup(1, &conn->ctx);
    memset(&conn->iocb, 0, sizeof(struct iocb));
    conn->piocb[0] = &conn->iocb;
    conn->file_size = 0;

    memset(conn->recv_buffer, 0, BUFSIZ);
    conn->recv_len = 0;

    memset(conn->send_buffer, 0, BUFSIZ);
    conn->send_len = 0;
    conn->send_pos = 0;
    conn->file_pos = 0;
    conn->async_read_len = 0;

    conn->have_path = 0;
    memset(conn->request_path, 0, BUFSIZ);
    conn->res_type = RESOURCE_TYPE_NONE;
    conn->state = STATE_INITIAL;

    return conn;
}

void connection_start_async_io(struct connection *conn)
{
    
}

void connection_remove(struct connection *conn)
{
    close(conn->sockfd);
    if (conn->fd >= 0)
        close(conn->fd);
    conn->state = STATE_CONNECTION_CLOSED;
    free(conn);
}

void handle_new_connection(void)
{
    static int sockfd;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    struct sockaddr_in addr;
    struct connection *conn;
    int rc;

    sockfd = accept(listenfd, (struct sockaddr *)&addr, &addrlen);
    DIE(sockfd < 0, "accept");
	int flags = fcntl(sockfd,F_GETFL,0);
	flags  = flags | O_NONBLOCK;

    fcntl(sockfd, F_SETFL, flags);

    conn = connection_create(sockfd);

    rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
    DIE(rc < 0, "w_epoll_add_in");

    http_parser_init(&conn->request_parser, HTTP_REQUEST);
	//conn->request_parser.data = conn;
    
}

void receive_data(struct connection *conn)
{
    int rc;
	char buffer[64];
	ssize_t bytes_recv = 0;
	rc = get_peer_address(conn->sockfd,buffer,64);
	if(rc < 0){
		ERR("get_peer_adress");
		goto remove_connection;
	}

	while(bytes_recv < 4096){
		ssize_t received = recv(conn->sockfd, conn->recv_buffer + bytes_recv,4096 - bytes_recv,0);
		if(received <= 0 || strstr(conn->recv_buffer,"\r\n\r\n") != NULL || strstr(conn->recv_buffer,"\n\n")!= NULL){
			break;
		}
		bytes_recv += received;
	}
	
	if (bytes_recv < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication from: %s\n", buffer);
		goto remove_connection;
	}
	if (bytes_recv == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed from: %s\n", buffer);
		goto remove_connection;
	}

	dlog(LOG_DEBUG, "Received message from: %s\n", buffer);

	printf("--\n%s--\n", conn->recv_buffer);

	conn->recv_len = bytes_recv;
	conn->state = STATE_REQUEST_RECEIVED;

	return;



remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);
	conn->state = STATE_CONNECTION_CLOSED;

	//return STATE_CONNECTION_CLOSED;
    

}

int connection_open_file(struct connection *conn)
{
	char *filename = (char *)malloc(BUFSIZ);
	memset(filename,0,BUFSIZ);
	sprintf(filename,"%s%s", AWS_DOCUMENT_ROOT,(conn->request_path + 1));
	dlog(LOG_DEBUG,"filename : %s\n",filename);
	strcpy(conn->filename,filename);
	
    conn->fd = open(filename, O_RDWR);
    if (conn->fd == -1) {
        sprintf(conn->send_buffer,ERROR_MSG);
		dlog(LOG_DEBUG,"404 not found\n");
		conn->send_len = strlen(conn->send_buffer);
		conn->state = STATE_SENDING_404;
    }
	else{
		sprintf(conn->send_buffer,OK_MSG);
		dlog(LOG_DEBUG,"200 OK\n");
		conn->send_len = strlen(conn->send_buffer);
		conn->state = STATE_SENDING_HEADER;
	}
	int rc = w_epoll_update_ptr_out(epollfd,conn->sockfd,conn);
	DIE(rc < 0,"epoll ptr add error");
	

    return conn->fd;
}
void connection_complete_async_io(struct connection *conn)
{
    connection_prepare_send_reply_header(conn);
    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLOUT;
    ev.data.ptr = conn;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, conn->sockfd, &ev);
}

int parse_header(struct connection *conn)
{
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
	http_parser *parser = &conn->request_parser;
	parser->data =   conn;
    ssize_t parsed = http_parser_execute(parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	conn->res_type = connection_get_resource_type(conn);
	if(parsed != conn->recv_len){
		return -1;
	}
    return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
  struct stat st;
  stat(conn->filename,&st);
  conn->file_size = st.st_size;
  ssize_t total_bytes_sent = 0;
  while(total_bytes_sent < conn->file_size){
	ssize_t bytes_sent = sendfile(conn->sockfd,conn->fd,&conn->file_pos,conn->file_size);
	if(bytes_sent <= 0){
		dlog(LOG_ERR,"Error");
		//break;
  	}
  total_bytes_sent += bytes_sent;
  }
  int rc = w_epoll_update_ptr_in(epollfd,conn->sockfd,conn);
  conn->state = STATE_DATA_SENT;
  return STATE_DATA_SENT;
   
}

int connection_send_data(struct connection *conn)
{
   ssize_t bytes_sent = 0;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}
	while(bytes_sent < conn->send_len){
		ssize_t bytes = send(conn->sockfd, conn->send_buffer + bytes_sent, conn->send_len - bytes_sent, 0);
	if (bytes < 0) {		/* error in communication */
		dlog(LOG_ERR, "Error in communication to %s\n", abuffer);
		goto remove_connection;
	}
	if (bytes == 0) {		/* connection closed */
		dlog(LOG_INFO, "Connection closed to %s\n", abuffer);
		goto remove_connection;
	}
	bytes_sent += bytes;
	}

	dlog(LOG_DEBUG, "Sending message to %s\n", abuffer);

	dlog(LOG_DEBUG,"--\n%s--\n", conn->send_buffer);

	/* all done - remove out notification */
	rc = w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");

	conn->state = STATE_DATA_SENT;

	return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

int connection_send_dynamic(struct connection *conn)
{
    
}

void handle_input(struct connection *conn)
{
	
     switch (conn->state) {
        case STATE_INITIAL:
			receive_data(conn);
			handle_input(conn);
        case STATE_REQUEST_RECEIVED:
			if(parse_header(conn) == 0){
				dlog(LOG_DEBUG,"parsed succesfully");
				connection_open_file(conn);
			}
			else{
				conn->state = STATE_SENDING_404;
				send_data(conn);
			}
			break;
		case STATE_DATA_SENT:
			connection_remove(conn);
			break;
		case STATE_404_SENT:
			connection_remove(conn);
			break;
		case STATE_CONNECTION_CLOSED:
			break;
        default:
            fprintf(stderr, "Unexpected state %d\n", conn->state);
            exit(1);
    }
	
}
void send_data(struct connection* conn){
	if(conn->state == STATE_SENDING_404){
		connection_prepare_send_404(conn);
		connection_send_data(conn);
		conn->state = STATE_404_SENT;
	}
	else{
		if(conn->state == STATE_SENDING_HEADER){
			connection_prepare_send_reply_header(conn);
			connection_send_data(conn);
			conn->state = STATE_HEADER_SENT;
			
		}
		else{
			if(conn->state == STATE_SENDING_DATA){
				connection_send_data(conn);
				conn->state = STATE_DATA_SENT;
			}
		}
	}
	handle_output(conn);

}
void handle_output(struct connection *conn)
{
	
    switch (conn->state) {
        
        case STATE_REQUEST_RECEIVED:
			conn->state = STATE_SENDING_DATA;
			send_data(conn);
			break;
		case STATE_HEADER_SENT:
			if(conn->res_type == RESOURCE_TYPE_STATIC){
				connection_send_static(conn);
				dlog(LOG_DEBUG,"sending static");
			} else if(conn->res_type == RESOURCE_TYPE_DYNAMIC){

			}
			else{
				connection_prepare_send_404(conn);
				conn->state = STATE_SENDING_404;
			}
			handle_output(conn);
			break;
		case STATE_SENDING_HEADER:
			send_data(conn);
			break;
		case STATE_SENDING_404:
			send_data(conn);
			break;
		case STATE_404_SENT:
			connection_remove(conn);
			break;
		case STATE_DATA_SENT:
			connection_remove(conn);
			break;
		case STATE_CONNECTION_CLOSED:
			break;

        default:
            fprintf(stderr, "Unexpected state %d\n", conn->state);
            exit(1);
    }
	
}

/*
void handle_client(uint32_t event, struct connection *conn)
{
    if (event & EPOLLIN) {
        dlog(LOG_DEBUG, "New message\n");
        handle_input(conn);
    }
    if (event & EPOLLOUT) {
        dlog(LOG_DEBUG, "Ready to send message\n");
        handle_output(conn);
    }
}
*/

int main(void)
{
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
        rc = w_epoll_wait_infinite(epollfd, &rev);
        DIE(rc < 0, "w_epoll_wait_infinite");

        if (rev.data.fd == listenfd) {
            dlog(LOG_DEBUG, "New connection\n");
            if (rev.events & EPOLLIN)
                handle_new_connection();
        } else {
			if(rev.events & EPOLLIN){
				struct connection *conn = (struct connection *)rev.data.ptr;
				handle_input(conn);
			}
			if(rev.events & EPOLLOUT){
				struct connection *conn = (struct connection *)rev.data.ptr;
				handle_output(conn);
			}
			if(rev.events & EPOLLERR){
				struct connection *conn = (struct connection *)rev.data.ptr;
				connection_remove(conn);
				conn->state = STATE_CONNECTION_CLOSED;
			}
        }
    }
    return 0;
}