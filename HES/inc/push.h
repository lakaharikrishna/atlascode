#ifndef __PUSH_H__
#define __PUSH_H__

#include "mysql_database.h"
#include "packet_buffer.h"
#include "utility.h"
#include <arpa/inet.h>
#include <cstring>
#include <endian.h>

/* Frame ID's */
#define FI_OBIS_CODES_SCALER_LIST        0x0A
#define FI_OBIS_CODES_LIST               0x0B
#define FI_CACHE_DATA                    0x0C
#define FI_INSTANT_DATA                  0x0E
#define FI_INSTANT_OBJECT_READ_WRITE     0x0F
#define FI_INSTANT_EVENT_OBJECT_READ     0x10
#define FI_INSTANT_POWERFAIL_OBJECT_READ 0x11

/* DLMS Profile commands */
#define COMMAND_NAMEPLATE_PROFILE  0x00
#define COMMAND_IP_PROFILE         0x01
#define COMMAND_BILLING_PROFILE    0x02
#define COMMAND_DAILY_LOAD_PROFILE 0x03
#define COMMAND_BLOCK_LOAD_PROFILE 0x04

/* Start bytes */
#define PMESH_PUSH_START_BYTE 0x2E
#define PUSH_DATA_START_BYTE  0x2C
#define PULL_START_BYTE       0x2D

#pragma pack(push, 1)

struct PmeshPushResponse
{
    uint8_t start_byte{};
    uint8_t length{};
    uint8_t packet_type{};
    uint8_t pan_id[4]{};
    uint8_t gateway_addr[4]{};
    uint8_t destination_addr[4]{};
    uint8_t remaining_pkt_count{};
    uint8_t current_pkt_count{};
};

struct DlmsPushResponse
{
    uint8_t start_byte{};
    uint8_t length[2]{};
    uint8_t current_page_index{};
    uint8_t frame_id{};
    uint8_t command{};
    uint8_t sub_command{};
    uint8_t next_page_status{};
    uint8_t no_of_records{};
    uint8_t data[128]{};
};

#pragma pack(pop)

template <typename T>
T from_big_endian(const uint8_t *src)
{
    if (sizeof(T) == 1)
    {
        return *src;
    }
    else if (sizeof(T) == 2)
    {
        uint16_t v;
        memcpy(&v, src, 2);
        return ntohs(v);
    }
    else if (sizeof(T) == 4)
    {
        uint32_t v;
        memcpy(&v, src, 4);
        return ntohl(v);
    }
    else if (sizeof(T) == 8)
    {
        uint64_t v;
        memcpy(&v, src, 8);
        return be64toh(v);
    }
}

class PushDataResponse
{
 public:
    PmeshPushResponse pmesh{};
    DlmsPushResponse dlms{};
};

/// Class for Profiles Data type. (e.g. IP, DLP, BLP, BH)
template <typename T>
class DlmsDataType
{
 public:
    uint8_t data_index{};
    uint8_t status{};
    uint8_t data_type{};
    T data{};

    void parse(const uint8_t *p)
    {
        data_index = p[0];
        status = p[1];
        data_type = p[2];
        data = from_big_endian<T>(p + 3);
    }

    int total_size() const
    {
        return 3 + sizeof(T);
    }
};

/// Class for Profiles Octet String. (e.g. IP, DLP, BLP, BH)
class DlmsDataTypeOctetString
{
 public:
    uint8_t length{};
    uint8_t payload[128]{};
    void parse(const uint8_t *p)
    {
        length = p[3];
        if (length >= 128)
        {
            length = 0;
            std::cout << "Invalid Length" << std::endl;
            return;
        }
        memcpy(payload, p + 4, length);
    }

    int total_size() const
    {
        return 4 + length;
    }
};

/// Class for events Data Type Field. (e.g. int8_t, uint8_t, uint16_t, uint32_t) for Power On and Power Off
template <typename T>
class EventDlmsDataType
{
 public:
    uint8_t status{};
    uint8_t data_type{};
    T data{};

    void parse(const uint8_t *p)
    {
        status = p[0];
        data_type = p[1];
        data = from_big_endian<T>(p + 2);
    }

    int total_size() const
    {
        return 2 + sizeof(T);
    }
};

/// Class for events Octet String. (Power On and Power Off)
class EventDlmsDataTypeOctetString
{
 public:
    uint8_t length = 0;
    uint8_t data[128] = {0};

    void parse(const uint8_t *p)
    {
        length = p[2];
        if (length >= 128)
        {
            length = 0;
            std::cout << "Invalid Length" << std::endl;
            return;
        }
        memcpy(data, p + 3, length);
    }

    int total_size() const
    {
        return 3 + length;
    }
};

class ScalarProfile
{
 public:
};

class PacketBufferBlockLoad
{
 public:
    int total_packets_received{};
    std::chrono::system_clock::time_point last_packet_time{};
    std::vector<DlmsRecordMap> profiles_data; // Completed blocks
    DlmsRecordMap partial_profile{};          // In-progress block

    void clear()
    {
        profiles_data.clear();
        partial_profile.clear();
        total_packets_received = 0;
    }
};

class EventPowerFailureRestoration
{
 public:
    uint8_t ordinal_index_of_all_elements{};

    EventDlmsDataType<EventDlmsDataTypeOctetString> real_time_clock{};
    EventDlmsDataType<EventDlmsDataTypeOctetString> meter_sl_number{};
    EventDlmsDataType<EventDlmsDataTypeOctetString> device_id{};
    EventDlmsDataType<EventDlmsDataTypeOctetString> event_status_word{};

    EventDlmsDataType<uint16_t> voltage{};
    EventDlmsDataType<uint16_t> current{};
    EventDlmsDataType<uint16_t> power{};
    EventDlmsDataType<uint16_t> transaction{};
    EventDlmsDataType<uint16_t> other_event{};
    EventDlmsDataType<uint16_t> non_roll_over{};
    EventDlmsDataType<uint16_t> control{};
};

class EventPowerFailureOccurance
{
 public:
    uint8_t ordinal_index_of_all_elements{};

    EventDlmsDataType<EventDlmsDataTypeOctetString> meter_sl_number{};
    EventDlmsDataType<EventDlmsDataTypeOctetString> device_id{};

    EventDlmsDataType<uint16_t> power{};
};

class DLMSParser : public virtual BaseLogger
{
 private:
    std::map<DLMSDataType, std::string> typeNames;

    void initTypeNames()
    {
        typeNames[DLMS_DATA_TYPE_NONE] = "NONE";
        typeNames[DLMS_DATA_TYPE_ARRAY] = "ARRAY";
        typeNames[DLMS_DATA_TYPE_STRUCTURE] = "STRUCTURE";
        typeNames[DLMS_DATA_TYPE_BOOLEAN] = "BOOLEAN";
        typeNames[DLMS_DATA_TYPE_BIT_STRING] = "BIT_STRING";
        typeNames[DLMS_DATA_TYPE_INT32] = "INT32";
        typeNames[DLMS_DATA_TYPE_UINT32] = "UINT32";
        typeNames[DLMS_DATA_TYPE_OCTET_STRING] = "OCTET_STRING";
        typeNames[DLMS_DATA_TYPE_STRING] = "STRING";
        typeNames[DLMS_DATA_TYPE_STRING_UTF8] = "STRING_UTF8";
        typeNames[DLMS_DATA_TYPE_BINARY_CODED_DECIMAL] = "BCD";
        typeNames[DLMS_DATA_TYPE_INT8] = "INT8";
        typeNames[DLMS_DATA_TYPE_INT16] = "INT16";
        typeNames[DLMS_DATA_TYPE_UINT8] = "UINT8";
        typeNames[DLMS_DATA_TYPE_UINT16] = "UINT16";
        typeNames[DLMS_DATA_TYPE_COMPACT_ARRAY] = "COMPACT_ARRAY";
        typeNames[DLMS_DATA_TYPE_INT64] = "INT64";
        typeNames[DLMS_DATA_TYPE_UINT64] = "UINT64";
        typeNames[DLMS_DATA_TYPE_ENUM] = "ENUM";
        typeNames[DLMS_DATA_TYPE_FLOAT32] = "FLOAT32";
        typeNames[DLMS_DATA_TYPE_FLOAT64] = "FLOAT64";
        typeNames[DLMS_DATA_TYPE_DATETIME] = "DATETIME";
        typeNames[DLMS_DATA_TYPE_DATE] = "DATE";
        typeNames[DLMS_DATA_TYPE_TIME] = "TIME";
    }

 public:
    DLMSParser()
    {
        initTypeNames();
    }

    // Parse value based on data type
    bool parseByType(const std::vector<uint8_t> &data, size_t &offset, DLMSDataType dataType, DLMSValueStruct &value)
    {
        if (offset >= data.size())
        {
            return false;
        }

        value.type = dataType;
        value.typeName = typeNames[dataType];

        switch (dataType)
        {
            case DLMS_DATA_TYPE_ARRAY: // Type 1
                break;

            case DLMS_DATA_TYPE_BOOLEAN: // Type 3
                if (offset + 1 > data.size()) return false;
                value.boolValue = (data[offset] != 0);
                offset += 1;
                break;

            case DLMS_DATA_TYPE_INT32: // Type 5
                if (offset + 4 > data.size()) return false;
                value.int32Value = (static_cast<int32_t>(data[offset]) << 24) |
                                   (static_cast<int32_t>(data[offset + 1]) << 16) |
                                   (static_cast<int32_t>(data[offset + 2]) << 8) |
                                   static_cast<int32_t>(data[offset + 3]);
                offset += 4;
                break;

            case DLMS_DATA_TYPE_UINT32:   // Type 6
            case DLMS_DATA_TYPE_DATETIME: // Type 25
                if (offset + 4 > data.size()) return false;
                value.uint32Value = (static_cast<uint32_t>(data[offset]) << 24) |
                                    (static_cast<uint32_t>(data[offset + 1]) << 16) |
                                    (static_cast<uint32_t>(data[offset + 2]) << 8) |
                                    static_cast<uint32_t>(data[offset + 3]);
                offset += 4;
                break;

            case DLMS_DATA_TYPE_BIT_STRING:   // Type 4
            case DLMS_DATA_TYPE_OCTET_STRING: // Type 9
            case DLMS_DATA_TYPE_STRING:       // Type 10
            case DLMS_DATA_TYPE_STRING_UTF8:  // Type 12
                if (offset + 1 > data.size()) return false;
                {
                    uint8_t length = data[offset];
                    offset += 1;

                    if (offset + length > data.size()) return false;

                    value.octetString.clear();
                    for (uint8_t i = 0; i < length; i++)
                    {
                        value.octetString.push_back(data[offset + i]);
                    }
                    offset += length;
                }
                break;

            case DLMS_DATA_TYPE_INT8: // Type 15
                if (offset + 1 > data.size()) return false;
                value.int8Value = static_cast<int8_t>(data[offset]);
                offset += 1;
                break;

            case DLMS_DATA_TYPE_INT16: // Type 16
                if (offset + 2 > data.size()) return false;
                value.int16Value = (static_cast<int16_t>(data[offset]) << 8) |
                                   static_cast<int16_t>(data[offset + 1]);
                offset += 2;
                break;

            case DLMS_DATA_TYPE_UINT8: // Type 17
                if (offset + 1 > data.size()) return false;
                value.uint8Value = data[offset];
                offset += 1;
                break;

            case DLMS_DATA_TYPE_UINT16: // Type 18
                if (offset + 2 > data.size()) return false;
                value.uint16Value = (static_cast<uint16_t>(data[offset]) << 8) |
                                    static_cast<uint16_t>(data[offset + 1]);
                offset += 2;
                break;

            case DLMS_DATA_TYPE_INT64: // Type 20
                if (offset + 8 > data.size()) return false;
                value.int64Value = (static_cast<int64_t>(data[offset]) << 56) |
                                   (static_cast<int64_t>(data[offset + 1]) << 48) |
                                   (static_cast<int64_t>(data[offset + 2]) << 40) |
                                   (static_cast<int64_t>(data[offset + 3]) << 32) |
                                   (static_cast<int64_t>(data[offset + 4]) << 24) |
                                   (static_cast<int64_t>(data[offset + 5]) << 16) |
                                   (static_cast<int64_t>(data[offset + 6]) << 8) |
                                   static_cast<int64_t>(data[offset + 7]);
                offset += 8;
                break;

            case DLMS_DATA_TYPE_UINT64: // Type 21
                if (offset + 8 > data.size()) return false;
                value.uint64Value = (static_cast<uint64_t>(data[offset]) << 56) |
                                    (static_cast<uint64_t>(data[offset + 1]) << 48) |
                                    (static_cast<uint64_t>(data[offset + 2]) << 40) |
                                    (static_cast<uint64_t>(data[offset + 3]) << 32) |
                                    (static_cast<uint64_t>(data[offset + 4]) << 24) |
                                    (static_cast<uint64_t>(data[offset + 5]) << 16) |
                                    (static_cast<uint64_t>(data[offset + 6]) << 8) |
                                    static_cast<uint64_t>(data[offset + 7]);
                offset += 8;
                break;

            default:
                std::cerr << "Unsupported data type: " << static_cast<int>(dataType) << std::endl;
                return false;
        }

        return true;
    }

    // Print parsed records
    void printRecords(const std::map<uint8_t, DLMSValueStruct> &records)
    {
        for (const auto &pair : records)
        {
            uint8_t tag = pair.first;
            const DLMSValueStruct &val = pair.second;

            print_and_log("Record 0x%02X (%s): ", static_cast<unsigned>(tag), val.typeName.c_str());

            switch (val.type)
            {
                case DLMS_DATA_TYPE_NONE:
                case DLMS_DATA_TYPE_ARRAY:
                    print_and_log("No Data\n");
                    break;
                case DLMS_DATA_TYPE_BOOLEAN:
                    print_and_log("%s\n", val.boolValue ? "true" : "false");
                    break;

                case DLMS_DATA_TYPE_INT8:
                    print_and_log("%d\n", static_cast<int>(val.int8Value));
                    break;

                case DLMS_DATA_TYPE_INT16:
                    print_and_log("%d\n", val.int16Value);
                    break;

                case DLMS_DATA_TYPE_INT32:
                    print_and_log("%d\n", val.int32Value);
                    break;

                case DLMS_DATA_TYPE_INT64:
                    print_and_log("%lld\n", static_cast<long long>(val.int64Value));
                    break;

                case DLMS_DATA_TYPE_UINT8:
                    print_and_log("0x%X\n", static_cast<unsigned>(val.uint8Value));
                    break;

                case DLMS_DATA_TYPE_UINT16:
                    print_and_log("0x%X\n", val.uint16Value);
                    break;

                case DLMS_DATA_TYPE_UINT32:
                case DLMS_DATA_TYPE_DATETIME:
                    print_and_log("0x%X\n", val.uint32Value);
                    break;

                case DLMS_DATA_TYPE_UINT64:
                    print_and_log("0x%X\n", static_cast<unsigned long long>(val.uint64Value));
                    break;

                case DLMS_DATA_TYPE_BIT_STRING:
                case DLMS_DATA_TYPE_OCTET_STRING:
                case DLMS_DATA_TYPE_STRING:
                case DLMS_DATA_TYPE_STRING_UTF8: {
                    for (uint8_t byte : val.octetString)
                    {
                        print_and_log(" %02X", static_cast<unsigned>(byte));
                    }
                    print_and_log("\n");
                    break;
                }

                default:
                    print_and_log("Unknown type\n");
                    break;
            }
        }
    }
};

class PushNodeInfo
{
 public:
    PacketBuffer<DlmsRecordMap> instantaneous_profile{};
    PacketBuffer<DlmsRecordMap> daily_load_profile{};
    PacketBuffer<DlmsRecordMap> billing_history{};
    PacketBufferBlockLoad block_load_profile{};
#if PUSH_EVENT_DETAILED_PARSING
    PacketBuffer<EventPowerFailureRestoration> power_on_event{};
    PacketBuffer<EventPowerFailureOccurance> power_off_event{};
#else
    PacketBuffer<DlmsRecordMap> power_on_event{};
    PacketBuffer<DlmsRecordMap> power_off_event{};
#endif
};

class PushData : public virtual MySqlDatabase, public virtual BaseLogger, public DLMSParser
{
    std::map<std::array<uint8_t, 8>, PushNodeInfo> push_node_info;

 public:
    std::array<uint8_t, 8> to_mac_array(const uint8_t mac[4]);
    uint8_t calculate_checksum(const uint8_t *buff, size_t length);
    void cleanup_push_profiles(void);
    void process_push_data(uint8_t *buff, ssize_t length, const char *gateway_id);
    int parseDLMSRecords(const std::vector<uint8_t> &data, const uint8_t &number_of_records, PacketBuffer<DlmsRecordMap> &records);
    int parseBlockLoadDLMSRecords(const std::vector<uint8_t> &data, const uint8_t &number_of_records, PacketBufferBlockLoad &records);

    void process_IP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id);
    void process_DLP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id);
    void process_BLP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id);
    void process_BHP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id);
    void process_power_on_event(uint8_t *buff, ssize_t length, const char *gateway_id);
    void process_power_off_event(uint8_t *buff, ssize_t length, const char *gateway_id);
};

#endif // __PUSH_H__
