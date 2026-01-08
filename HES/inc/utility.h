#ifndef __UTILITY_H__
#define __UTILITY_H__

#include <stdarg.h>
#include <stdint.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

#include "nlohmann/json.hpp"

#if !defined(SUCCESS) && !defined(FAILURE)
static constexpr int SUCCESS = 0;
static constexpr int FAILURE = -1;
#endif

#ifndef ENABLED
#define ENABLED 1
#endif

const uint8_t proprietary_mac_address[4] = {0x3C, 0xC1, 0xF6, 0x01};

class Utility
{
 public:
    static void convert_asc_hex_string_to_bytes(uint8_t *destination, uint8_t *source, uint32_t length);
    static void convert_bytes_to_asc_hex_string(uint8_t *destination, uint8_t *source, uint32_t length);
    static std::string mac_to_string(const uint8_t *mac);

    //(added by Hari)
    void static aschexstr2hex(unsigned char *destination, char *source, unsigned char length);
    static std::string bin_to_hex_upper(const unsigned char *mac, size_t len);
    static void extract_ids(const char *dcu_id_hex, unsigned char *panid, unsigned char *dcu_short_addr);
    static void ascii_hex_to_bin(unsigned char *out, const char *in, int len);
    static std::string mac_to_string_nocolon(const uint8_t *mac);

    template <typename T>
    static T readConfig(const std::string &path)
    {
        loadConfig();

        const nlohmann::json *current = &configData;
        std::istringstream iss(path);
        std::string key;

        while (std::getline(iss, key, '.'))
        {
            if (!current->contains(key))
            {
                throw std::runtime_error("Key not found: " + key);
            }
            current = &((*current)[key]);
        }
        return current->get<T>();
    }

 private:
    static void loadConfig();
    static nlohmann::json configData;
    static std::once_flag configFlag;
};

class TimeUtility
{
 protected:
    std::string now();
    std::string now_ms();
};

class BaseLogger : public TimeUtility
{
 protected:
    std::ofstream log_file;

 public:
    virtual ~BaseLogger()
    {
        if (log_file.is_open())
        {
            log_file.close();
        }
    }

    bool open_log_file(const std::string &gateway_id)
    {
        if (log_file.is_open())
        {
            log_file.close();
        }

        log_file.open(gateway_id, std::ios::app | std::ios::binary);
        return log_file.is_open();
    }

    void print_and_log(const char *format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        std::cout << buffer;
        std::cout.flush();

        if (log_file.is_open())
        {
            log_file << buffer;
            log_file.flush();
        }
    }
};

enum Status
{
    CONNECTED = 0,
    DISCONNECTED
};

enum ClientTimeoutState
{
    TIMER_PGWID_RX = 0,
    TIMER_NONE = 1,
    TIMER_FUOTA_RESPONSE
};

class ClientCurrentState
{
 public:
    enum
    {
        INIT = 0,
        FUOTA_STATUS,
        FUOTA_DISABLE,
        FLASH_SAVE,
        SOFT_RESET,
        NAMEPLATE,
        DAILY_LOAD,
        BLOCK_LOAD,
        BILLING_HISTORY,
        INSTANTANEOUS_PROFILE,
        DLMS_CONNECT,
        INTERNAL_FV,
        IDLE
    };
};

class ClientTargetState
{
 public:
    enum
    {
        IDLE = 0,
        PULL
    };
};

#endif // __UTILITY_H__