#ifndef SERVER_H
#define SERVER_H

#include <cstdint>

class Server {

public:
  Server();

  void init_server();
  void close_server();
  void restart_server();
  void send_header_server(uint8_t *data, uint32_t size);

  int send_data(uint8_t *data, uint32_t size);
  int recv_data(uint8_t **data, uint32_t size);

  int is_connected();

private:
  int s_socket = -1; // socket
  int c_socket = -1; // connect socket

  const int port = 53516;
};

#endif
