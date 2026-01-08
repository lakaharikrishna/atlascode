#include "../inc/utility.h"

nlohmann::json Utility::configData;
std::once_flag Utility::configFlag;

// Convert ASCII hex string to byte array
void Utility::convert_asc_hex_string_to_bytes(uint8_t *destination, uint8_t *source, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        sscanf((char *)source + (i * 2), "%2hhX", &destination[i]);
    }
}

// Convert byte array to ASCII hex string
void Utility::convert_bytes_to_asc_hex_string(uint8_t *destination, uint8_t *source, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        sprintf((char *)destination + (i * 2), "%02X", source[i]);
    }
}

std::string Utility::mac_to_string(const uint8_t *mac)
{
    char buf[3 * 8] = {0};
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[6], mac[7]);
    return std::string(buf);
}

void Utility::loadConfig()
{
    std::call_once(configFlag, [] {
        std::ifstream file("config.json");

        if (!file.is_open())
        {
            throw std::runtime_error("Unable to open file: config.json");
        }
        file >> configData;
    });
}

std::string TimeUtility::now()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);

    std::tm tm{};

    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string TimeUtility::now_ms()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    auto t = system_clock::to_time_t(now);

    std::tm tm{};

    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

std::string Utility::mac_to_string_nocolon(const uint8_t *mac)
{
    char buf[17] = {0}; // 8 bytes * 2 hex
    snprintf(buf, sizeof(buf),
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5], mac[6], mac[7]);
    return std::string(buf);
}

/*.........................Added by LHK on 20112025...............*/
// Converts an ASCII hex string to its byte representation
void Utility::aschexstr2hex(unsigned char *destination, char *source, unsigned char length)
{
    unsigned char l_count, l_byteval, l_temp;

    if ((length % 2) != 0)
        length++; // make the length even

    for (l_count = 0x00; l_count < length; l_count++)
    {
        l_byteval = *source;

        if ((l_byteval >= '0') && (l_byteval <= '9'))
        {
            l_byteval -= 0x30;
        }
        else if ((l_byteval >= 'A') && (l_byteval <= 'F'))
        {
            l_byteval -= 55;
        }
        else if ((l_temp >= 'a') && (l_temp <= 'f'))
        {
            l_temp -= 'a';
        }

        l_byteval <<= 4; // MSB Nibble

        source++; // process for LSB Nibble

        l_temp = *source;

        if ((l_temp >= 0x30) && (l_temp <= 0x39))
        {
            l_temp -= 0x30;
        }
        else if ((l_temp >= 'A') && (l_temp <= 'F'))
        {
            l_temp -= ('A' - 10);
        }
        else if ((l_temp >= 'a') && (l_temp <= 'f'))
        {
            l_temp -= ('a' - 10);
        }

        l_byteval |= l_temp; // ORing lower Nibble
        *destination = l_byteval;
        destination++;
        source++;
    }
    return;
}

std::string Utility::bin_to_hex_upper(const unsigned char *mac, size_t len)
{
    char buf[64];
    for (size_t i = 0; i < len; ++i)
        sprintf(buf + (i * 2), "%02X", mac[i]);
    buf[len * 2] = '\0';
    return std::string(buf);
}

void Utility::extract_ids(const char *dcu_id_hex, unsigned char *panid, unsigned char *dcu_short_addr)
{
    unsigned char dcu_id[8];

    // Convert hex string to bytes
    for (int i = 0; i < 8; i++)
    {
        sscanf(dcu_id_hex + (i * 2), "%2hhX", &dcu_id[i]);
    }

    // Extract dcu_short_addr = bytes [4..7]
    memcpy(dcu_short_addr, &dcu_id[4], 4);

    // PAN ID = 00 00 + last two bytes of DCU ID
    panid[0] = 0x00;
    panid[1] = 0x00;
    panid[2] = dcu_id[6];
    panid[3] = dcu_id[7];
}

void Utility::ascii_hex_to_bin(unsigned char *out, const char *in, int len)
{
    char buf[3] = {0};
    for (int i = 0; i < len / 2; i++)
    {
        buf[0] = in[i * 2];
        buf[1] = in[i * 2 + 1];
        out[i] = (unsigned char)strtoul(buf, NULL, 16);
    }
}
