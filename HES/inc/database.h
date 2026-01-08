#ifndef __DATABASE_H__
#define __DATABASE_H__

#include <mysql/errmsg.h>
#include <mysql/mysql.h>
#ifdef _WIN32
#include "posix_compat.h"
#else
#include <unistd.h>
#endif

#include <iostream>
#include <string>

#include "../inc/String_functions.h"
#include "pull.h"
#include "push.h"
#include "utility.h"
#include <queue>
#include <unistd.h>
#include <vector>

#define attribute_id_ip  0x0100
#define attribute_id_dlp 0x0300
#define attribute_id_blp 0x0400
#define attribute_id_bh  0x0200

#define MAX_QUERY_BUFFER 86500
// Forward declarations to avoid circular include with client.h

#include "mysql_database.h"

#endif // __DATABASE_H__