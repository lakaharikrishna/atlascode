#ifndef __SERVER_H__
#define __SERVER_H__

#include <arpa/inet.h>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#ifndef _WIN32
#include <dirent.h>
#include <execinfo.h>
#endif
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <netinet/tcp.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <queue>
#include <sstream>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#ifdef _WIN32
#include "posix_compat.h"
#else
#include <unistd.h>
#endif
#include <unordered_map>
#include <vector>

#include "client.h"
#include "utility.h"

class Client;
class MQTTClient;
class MySqlDatabase;
class BaseLogger;

class Server
{
 private:
    int server_socket, server_port;
    sockaddr_in server_addr, client_addr;

 public:
    Server();
    ~Server();

    static std::mutex clients_mutex;
    static std::map<std::array<char, 16>, Client *> g_clients;

    void accept_and_dispatch(void);
    bool configure_socket_options(int socket_fd);
    static void handle_client(std::unique_ptr<Client> client);
};

#endif // __SERVER_H__
