#ifndef __MAIN_H__
#define __MAIN_H__

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
#ifndef _WIN32
#include <poll.h>
#endif
#include <queue>
#include <sstream>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef _WIN32
#include "posix_compat.h"
#else
#include <unistd.h>
#endif
#include <unordered_map>
#include <vector>

#define BUFF_SIZE 2048

#endif // __MAIN_H__