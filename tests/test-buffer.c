#include "buffer.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

ssize_t reading_hook(server_pt srv, int fd,
                     void *buffer, size_t size) {
   ssize_t read = 0;
    if ((read = recv(fd, buffer, size, 0)) > 0) {
        Server.touch(srv,fd);
        return read;
    } else if (read && (errno & (EWOULDBLOCK | EAGAIN))) return 0;
    return -1;
}

ssize_t writing_hook(server_pt srv, int fd,
                     void *data, size_t len) {
  int sent = write(fd, data, len);
  if (sent < 0 && (errno & (EWOULDBLOCK | EAGAIN | EINTR)))
    sent = 0;
  return sent;
}

void on_open(server_pt server, int sockfd)
{
    printf("A connection was accepted on socket #%d.\n", sockfd);
    const char greeting[] = "\nSimple echo server. Type 'bye' to exit.\n";
    Server.write(server, sockfd, (char *) greeting, sizeof(greeting));
    /* apply other writing methods provided by buffer.c 
    * with Server_API rw_hooks 
    */
    Server.rw_hooks(server, sockfd, reading_hook, writing_hook);
}

void on_close(server_pt server, int sockfd)
{
    printf("Socket #%d is now disconnected.\n", sockfd);
}

/* simple echo, the main callback */
void on_data(server_pt server, int sockfd)
{
    char buff[1024];

    ssize_t incoming = 0;
    /* Read everything, this is edge triggered, `on_data` won't be called
     * again until all the data was read.
     */
    while ((incoming = Server.read(server, sockfd, buff, 1024)) > 0) {
        if (!memcmp(buff, "bye", 3)) {
            /* close the connection automatically AFTER buffer was sent */
            Server.close(server, sockfd);
        } else {
            /* since the data is stack allocated, we'll write a copy
             * optionally, we could avoid a copy using Server.write_move
	     */
            Server.write(server, sockfd, buff, incoming);
        }
    }
}

int main(void)
{
    struct Protocol protocol = {
        .on_open = on_open,
        .on_close = on_close,
        .on_data = on_data,
        .service = "echo"
    };
    
    start_server(.protocol = &protocol, .timeout = 10, .threads = 8);
    return 0;
}
