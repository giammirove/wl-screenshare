#include "server.hpp"
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

Server::Server() {}

void Server::init_server() {
  if (s_socket != -1)
    return;
  printf("[SERVER] Init\n");

  printf("[SERVER] SOCKET mode\n");

  s_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (s_socket >= 0) {
    struct sockaddr_in saAddr;
    memset(&saAddr, 0, sizeof(saAddr));
    saAddr.sin_family = AF_INET;
    saAddr.sin_addr.s_addr = htonl(0); // (IPADDR_ANY)
    saAddr.sin_port = htons(port);

    if ((bind(s_socket, (struct sockaddr *)&saAddr, sizeof(saAddr)) == 0) &&
        (listen(s_socket, 10) == 0)) {
      memset(&saAddr, 0, sizeof(saAddr));
      socklen_t len = sizeof(saAddr);
      c_socket = accept(s_socket, (struct sockaddr *)&saAddr, &len);
      // setting timeout
      struct timeval tv;
      tv.tv_sec = 1;
      tv.tv_usec = 0;
      setsockopt(c_socket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
                 sizeof tv);

      if (c_socket >= 0) {
        printf("[SERVER] Connection %d - %d\n", s_socket, c_socket);
      } else {
        perror("[SERVER] no connection");
      }
    } else {
      perror("[SERVER] cant bind");
      exit(-1);
    }
  } else {
    perror("[SERVER] create socket");
    exit(-1);
  }
}

void Server::close_server() {
  printf("[SERVER] Closing\n");
  shutdown(s_socket, SHUT_RDWR);
  shutdown(c_socket, SHUT_RDWR);
  close(s_socket);
  close(c_socket);
  printf("[SERVER] Closed\n");
}

void Server::restart_server() {
  printf("[SERVER] Restarting\n");
  close_server();
  sleep(1);
  printf("[SERVER] Reconnecting\n");
  init_server();
}

void Server::send_header_server(uint8_t *data, uint32_t size) {
  return;
  printf("[SERVER] header (%d)\n", size);
  send_data(data, size);

  printf("[SERVER] HEADER SENT\n");
}

// assuming 32 bit for an int
int32_t Server::send_data(uint8_t *data, uint32_t size) {
  init_server();
  if (c_socket == -1 || data == nullptr || size == 0)
    return -1;
  if (size <= 3)
    return -1;
  int32_t m = -1;
  uint8_t bsize[] = {(uint8_t)((size >> 24) & 0xff),
                     (uint8_t)((size >> 16) & 0xff),
                     (uint8_t)((size >> 8) & 0xff), (uint8_t)((size) & 0xff)};
  if ((m = send(c_socket, bsize, 4 * sizeof(uint8_t), 0)) < 0) {
    printf("The last error message is: %s\n", strerror(errno));
    printf("[SERVER] Can't send size from %d -> %d\n", c_socket, m);
    return -1;
  }
  if ((m = send(c_socket, data, size * sizeof(uint8_t), 0)) < 0) {
    printf("The last error message is: %s\n", strerror(errno));
    printf("[SERVER] Can't send data from %d -> %d\n", c_socket, m);
    return -1;
  }
  return m;
}

// assuming 32 bit for an int
int32_t Server::recv_data(uint8_t **data, uint32_t size) {
  int32_t m = -1;
  if ((m = recv(c_socket, *data, size, 0)) < 0) {
    printf("[SERVER] Can't recv data\n");
    return -1;
  }
  return m;
}

int Server::is_connected() { return c_socket != -1; }
