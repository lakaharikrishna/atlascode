#ifndef __CLIENT_H__
#define __CLIENT_H__

// #include <filesystem>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

#include "database.h"
#include "mqtt.h"
#include "pull.h"
#include "push.h"
#include "server.h"
#include "utility.h"

#include "fuota.h"

// class Fuota;
#define BUFFER_SIZE 1024

#define TIMEOUT_RECEIVED    0x06
#define COMMAND_IN_PROGRESS 0X07
// Use SUCCESS and FAILURE from utility.h
#define DLMS_CHECKSUM_ERROR    0x29
#define DLMS_CONNECTION_FAILED 0x25
#define INVALID_RESPONSE       0x49
#define DLMS_SUCCESS           0x37
#define SUCCES_NEXT_PAGE       0x02
#define PING_NODE              0x0D
#define PING_METER             0x0E
#define POLL_TIMEOUT           0xFF
#define PMESH_ERROR            0x0F
#define SECONDS_5H30M          0x4D58 /* 5 hours 30 minutes in seconds */

#define MAX_CLIENT_RX_BUFFER 4096

/* Mesh Tx Packet Types */
#define MESH_PING_REQUEST 0x0D

// DLMS _cmd_id
#define CMD_ID_NP_PROFILE    0x00
#define CMD_ID_IP_PROFILE    0x01
#define CMD_ID_BH_PROFILE    0x02
#define CMD_ID_DLP_PROFILE   0x03
#define CMD_ID_BLP_PROFILE   0x04
#define CMD_ID_EVENT_PROFILE 0x08

/* Mesh Tx Packet Types */
#define MESH_COMMISSION_PACKET        0x03
#define MESH_PARENT_SET               0x05
#define MESH_OTHER_PAN_NEIGHBOUR_SCAN 0x0B
#define MESH_SAME_PAN_NEIGHBOUR_SCAN  0x0C
#define MESH_PING_REQUEST             0x0D
#define MESH_DATA_QUERY               0x07

/* Mesh Rx Packet Types */
#define MESH_COMMISSION_PACKET_RESPONSE        0x04
#define MESH_PARENT_SET_RESPONSE               0x06
#define MESH_SAME_PAN_NEIGHBOUR_SCAN_RESPONSE  0x0F
#define MESH_OTHER_PAN_NEIGHBOUR_SCAN_RESPONSE 0x10
#define MESH_PING_RESPONSE                     0x0E
#define MESH_DATA_RESPONSE                     0x08

// frame id
#define FI_OBIS_CODES_SCALAR_LIST        0x0A
#define FI_OBIS_CODES_LIST               0x0B
#define FI_CACHE_DATA                    0x0C
#define FI_INSTANT_DATA                  0x0E
#define FI_INSTANT_OBJECT_READ_WRITE     0x0F
#define FI_INSTANT_EVENT_OBJECT_READ     0x10
#define FI_INSTANT_POWERFAIL_OBJECT_READ 0x11

#define PMESH_HOPCNT_INDX      0x0C // 12th index
#define DLMS_CMD_LEN           0x08
#define UNSILECE_CMD_LEN       0x02
#define UNSILENCE_RESPONSE_LEN 0x11
#define DLMS_ERROR_RES_LEN     0x1E

/* Start Bytes */
#define NMS_START_BYTE         0x2A
#define DLMS_START_BYTE        0x2B
#define HES_OTA_CMD_START_BYTE 0x2E
#define HES_START_BYTE         0x2E
#define PUSH_DATA_START_BYTE   0x2C
#define PULL_DATA_START_BYTE   0x2D

/* Error Codes */
#define PMESH_START_BYTE_ERROR    0x00
#define PMESH_LENGTH_ERROR        0x01
#define PMESH_PACKET_TYPE_ERROR   0x02
#define PMESH_PAN_ID_ERROR        0x03
#define PMESH_SOURCE_ADDR_ERROR   0x04
#define PMESH_ROUTER_IDX_ERROR    0x05
#define PMESH_TIMEOUT_ERROR       0x06
#define PMESH_COMMAND_IN_PROGRESS 0x07
#define PMESH_NMS_DISABLED        0x08
#define FAILED_RESPONSE           0x09

struct DLMSValue;

// typedef enum
// {
//     e_success_0 = 0,
//     e_success_1,
//     e_success_2,
//     FAILURE = -1,
// } Status1;
typedef enum
{
    RETRY_COUNT_0 = 0,
    RETRY_COUNT_1,
    RETRY_COUNT_2,
    RETRY_COUNT_3,
    RETRY_COUNT_4,
    RETRY_COUNT_5,
} RETRY_COUNT;

enum NP_Data_index
{
    NP_METER_SERIALNUMBER = 0x00,
    NP_DEVICE_ID = 0x01,
    NP_MANUFACTURE_NAME = 0x02,
    NP_METER_FIRMWARE = 0x03,
    NP_METER_PHASE = 0x04,
    NP_CATEGORY = 0x05,
    NP_CURRENT_RATING = 0x06,
    NP_MANUFACTURE_YEAR = 0x07
};

enum IP_Data_index
{
    IP_RTC = 0x00,
    IP_VOLTAGE = 0x01,
    IP_PHASE_CURRENT = 0x02,
    IP_NEUTRAL_CURRENT = 0x03,
    IP_SIGNED_POWER_FACTOR = 0x04,
    IP_FREQUENCY = 0x05,
    IP_APPARENT_POWER = 0x06,
    IP_ACTIVE_POWER = 0x07,
    IP_CUMULATIVE_ENERGY_IMPORT_KWH = 0x08,
    IP_CUMULATIVE_ENERGY_IMPORT_KVAH = 0x09,
    IP_MAXIMUM_DEMAND_KW = 0x0A,
    IP_MAXIMUM_DEMAND_KW_DATE_TIME = 0x0B,
    IP_MAXIMUM_DEMAND_KVA = 0x0C,
    IP_MAXIMUM_DEMAND_KVA_DATE_TIME = 0x0D,
    IP_CUMULATIVE_POWER_ON = 0x0E,
    IP_CUMULATIVE_TAMPER_COUNT = 0x0F,
    IP_CUMULATIVE_BILLING_COUNT = 0x10,
    IP_CUMULATIVE_PROGRAMMING_COUNT = 0x11,
    IP_CUMULATIVE_ENERGY_EXPORT_KWH = 0x12,
    IP_CUMULATIVE_ENERGY_EXPORT_KVAH = 0x13,
    IP_LOAD_LIMIT_FUNCTION_STATUS = 0x14,
    IP_LOAD_LIMIT_VALUE_KW = 0x15,
    // Proprietary data types
    IP_RSSI = 0xF0,
    IP_FREQUENCY_OFFSET = 0xF1,
    IP_NOISE_FLOOR = 0xF2,
    IP_TDC_VALUE = 0xF3,
    IP_TEMP = 0xF4
};

enum DLP_Data_index
{
    DLP_RTC = 0x00,
    DLP_CUMULATIVE_ENERGY_EXPORT_KWH = 0x01,
    DLP_CUMULATIVE_ENERGY_EXPORT_KVAH = 0x02,
    DLP_CUMULATIVE_ENERGY_IMPORT_KWH = 0x03,
    DLP_CUMULATIVE_ENERGY_IMPORT_KVAH = 0x04
};

enum BLP_Data_index
{
    BLP_RTC = 0x00,
    BLP_AVERAGE_VOLTAGE = 0x01,
    BLP_BLOCK_ENERGY_IMPORT_KWH = 0x02,
    BLP_BLOCK_ENERGY_IMPORT_KVAH = 0x03,
    BLP_BLOCK_ENERGY_EXPORT_KWH = 0x04,
    BLP_BLOCK_ENERGY_EXPORT_KVAH = 0x05,
    BLP_AVERAGE_CURRENT = 0x06
};

enum BHP_Data_index
{
    BHP_BILLING_DATE_IMPORT_MODE = 0x00,
    BHP_AVERAGE_POWER_FACTOR_FOR_BILLING_PERIOD = 0x01,
    BHP_CUMULATIVE_ENERGY_IMPORT_KWH = 0x02,
    BHP_CUMULATIVE_ENERGY_TZ1_KWH = 0x03,
    BHP_CUMULATIVE_ENERGY_TZ2_KWH = 0x04,
    BHP_CUMULATIVE_ENERGY_TZ3_KWH = 0x05,
    BHP_CUMULATIVE_ENERGY_TZ4_KWH = 0x06,
    BHP_CUMULATIVE_ENERGY_TZ5_KWH = 0x07,
    BHP_CUMULATIVE_ENERGY_TZ6_KWH = 0x08,
    BHP_CUMULATIVE_ENERGY_TZ7_KWH = 0x09,
    BHP_CUMULATIVE_ENERGY_TZ8_KWH = 0x0A,
    BHP_CUMULATIVE_ENERGY_IMPORT_KVAH = 0x0B,
    BHP_CUMULATIVE_ENERGY_TZ1_KVAH = 0x0C,
    BHP_CUMULATIVE_ENERGY_TZ2_KVAH = 0x0D,
    BHP_CUMULATIVE_ENERGY_TZ3_KVAH = 0x0E,
    BHP_CUMULATIVE_ENERGY_TZ4_KVAH = 0x0F,
    BHP_CUMULATIVE_ENERGY_TZ5_KVAH = 0x10,
    BHP_CUMULATIVE_ENERGY_TZ6_KVAH = 0x11,
    BHP_CUMULATIVE_ENERGY_TZ7_KVAH = 0x12,
    BHP_CUMULATIVE_ENERGY_TZ8_KVAH = 0x13,
    BHP_MD_KW = 0x14,
    BHP_MD_KW_DATE_AND_TIME = 0x15,
    BHP_MD_KVA = 0x16,
    BHP_MD_KVA_DATE_AND_TIME = 0x17,
    BHP_BILLING_POWER_ON_DURATION = 0x18,
    BHP_CUMULATIVE_ENERGY_EXPORT_KWH = 0x19,
    BHP_CUMULATIVE_ENERGY_EXPORT_KVAH = 0x1A
};

enum EVENT_type
{
    EVENT_ALL = 0x00,
    EVENT_VOLTAGE = 0x01,
    EVENT_CURRENT = 0x02,
    EVENT_POWER = 0x03,
    EVENT_TRANSACTIONAL = 0x04,
    EVENT_OTHER = 0x05,
    EVENT_NON_ROLL_OVER = 0x06,
    EVENT_CONTROL = 0x07
};

enum EVENT_data_index
{
    EVENT_DATA_INDEX_RTC = 0x00,
    EVENT_DATA_INDEX_EVENT_CODE = 0x01,
    EVENT_DATA_INDEX_CURRENT = 0x02,
    EVENT_DATA_INDEX_VOLTAGE = 0x03,
    EVENT_DATA_INDEX_SIGNED_POWER_FACTOR = 0x04,
    EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH = 0x05,
    EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT = 0x06
};

enum EVENT_DATA_INDEX_FIELDS
{
    EVENT_DATA_INDEX_FIELD_INDEX = 0,
    EVENT_DATA_INDEX_FIELD_NO_OF_RECORDS = 1,
    EVENT_DATA_INDEX_FIELD_CAPTURE_PERIOD = 2,
    EVENT_DATA_INDEX_FIELD_SORT_METHOD = 3,
    EVENT_DATA_INDEX_FIELD_CURRENT_ENTRIES = 4,
    EVENT_DATA_INDEX_FIELD_MAX_RECORDS = 5
};

enum SINGLE_OBIS_type
{
    SINGLE_OBIS_RTC = 0x02,
    SINGLE_OBIS_INTEGRATION_OR_CAPTURE_PERIOD = 0x01,
    SINGLE_OBIS_LOAD_LIMIT = 0x08,
    SINGLE_OBIS_LOAD_STATUS = 0x09,
    SINGLE_OBIS_ACTION_SCHEDULE = 0x07
};

enum DownloadDataType
{
    DATA_TYPE_NP = 0,
    DATA_TYPE_IP = 1,
    DATA_TYPE_BHP = 2,
    DATA_TYPE_DLP = 3,
    DATA_TYPE_BLP = 4,
    DATA_TYPE_ALL_EVENTS = 5,
    DATA_TYPE_VOLTAGE_EVENTS = 6,
    DATA_TYPE_CURRENT_EVENTS = 7,
    DATA_TYPE_POWER_EVENTS = 8,
    DATA_TYPE_TRANSACTIONAL_EVENTS = 9,
    DATA_TYPE_OTHER_EVENTS = 10,
    DATA_TYPE_NON_ROLL_OVER_EVENTS = 11,
    DATA_TYPE_CONTROL_EVENTS = 12,
    DATA_TYPE_PING_NODE = 13,
    DATA_TYPE_PING_METER = 14,
    DATA_TYPE_RTC_READ = 15,
    DATA_TYPE_RTC_WRITE = 16,
    DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ = 17,
    DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ_WRITE = 18,
    DATA_TYPE_CAPTURE_PERIOD_READ = 19,
    DATA_TYPE_CAPTURE_PERIOD_READ_WRITE = 20,
    DATA_TYPE_LOAD_LIMIT_READ = 21,
    DATA_TYPE_LOAD_LIMIT_WRITE = 22,
    DATA_TYPE_LOAD_STATUS_READ = 23,
    DATA_TYPE_LOAD_STATUS_WRITE = 24,
    DATA_TYPE_ACTION_SCHEDULER_READ = 25,
    DATA_TYPE_ACTIVITY_CALENDAR_READ = 26,
    DATA_TYPE_RF_FIRMWARE_UPGRADE = 27,
    DATA_TYPE_METER_FIRMWARE_UPGRADE = 28,
    DATA_TYPE_METER_FIRMWARE_VERSION_READ = 29,
    DATA_TYPE_RF_FIRMWARE_VERSION_READ = 30,
    DATA_TYPE_MD_RESET = 31,
    DATA_TYPE_CANCEL_ODM = 32,
    DATA_TYPE_CANCEL_FUOTA = 33
};

// Structured profile for Name Plate
struct ODM_NamePlateProfile
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_InstantaneousProfile
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_DailyLoadProfile
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_BlockLoadProfile
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_BillingProfile
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_VoltageEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_CurrentEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_PowerEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_TransactionalEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_OtherEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_NonRollOverEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ODM_ControlEvent
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

struct ScalarProfile_init
{
    std::unordered_map<int8_t, std::vector<DLMSValue>> scalar; // index -> [scalar, unit]
    std::unordered_map<int8_t, std::vector<DLMSValue>> scalar_type;
    std::unordered_map<int8_t, std::vector<DLMSValue>> unit;
    std::unordered_map<int8_t, std::vector<DLMSValue>> unit_type;
    std::unordered_map<int8_t, std::string> obis_codes; // index -> OBIS string
};

struct ODM_SingleObisData
{
    // RTC Read (0x02)
    std::vector<uint32_t> rtc_timestamp;
    std::string rtc_formatted;

    // Integration/Profile Capture Period (0x01)
    std::vector<uint16_t> period_seconds;

    // Load Limit (0x08)
    std::vector<uint16_t> load_limit;

    // Load Disconnect/Reconnect Status (0x09)(PING METER)
    std::vector<uint8_t> load_status;
    std::string load_status_str;

    // Single Action Scheduler (0x07)
    std::vector<uint32_t> scheduler_timestamp;
    std::string scheduler_formatted;

    // Command type read or write (0 = read, 1 = write)
    std::vector<uint8_t> command;

    // Sub-command
    std::vector<uint8_t> sub_command;
};

struct EventDataIndex
{
    std::unordered_map<uint8_t, std::vector<DLMSValue>> data;
};

// Structure holding parameters required for DB operations related to a single ODM request
struct DBparameters
{
    // Unique request ID from the on-demand / ODM table (primary reference for this transaction)
    size_t req_id;

    // Identifier of the GATEWAY (Data Concentrator Unit) handling this request
    std::string gateway_id;

    // MAC address of the target meter in the RF/mesh network
    std::string meter_mac_address;

    // Serial number of the target meter (used for billing / audit mapping)
    std::string meter_serial_no;

    uint8_t command; // read = 0 or write = 1

    // Current status of the request (e.g., pending, success, failed – application-specific enum/values)
    uint8_t status;

    // Timestamp string of the last successful download/interaction with the meter
    std::string last_download_time;

    uint8_t push_alaram; // Pull = 0 && Push = 1
};

struct Pingnode_details
{
    std::string gateway_id;
    std::string meter_serial_no;
    std::string target_mac_address;
    std::vector<uint8_t> gateway_type;
    std::vector<uint8_t> download_data_type;
    std::string req_command;
    size_t ping_count; // single counter for pings
    std::vector<size_t> request_id;
    std::chrono::time_point<std::chrono::steady_clock> Time_tx;
    std::chrono::time_point<std::chrono::steady_clock> Time_rx;
    uint64_t time_duration; // duration in milliseconds
};

// Main ODMProfiles class
class ODMProfiles
{
 public:
    // Structured NP profiles
    ODM_NamePlateProfile name_plate;
    // Structured IP profile
    ODM_InstantaneousProfile IP;
    // Structured DLP profile
    ODM_DailyLoadProfile DLP;
    // Structured BLP profile
    ODM_BlockLoadProfile BLP;
    // Structured BH profile
    ODM_BillingProfile BH;
    // Structured voltage event profile
    ODM_VoltageEvent voltage_event;
    // Structured current event profile
    ODM_CurrentEvent current_event;
    // Structured power event profile
    ODM_PowerEvent power_event;
    // Structured transaction event profile
    ODM_TransactionalEvent transaction_event;
    // Structured other event profile
    ODM_OtherEvent other_event;
    // Structured nonrollover event profile
    ODM_NonRollOverEvent non_roll_over_event;
    // Structured control event profile
    ODM_ControlEvent control_event;
    // Event Indexes Summary
    EventDataIndex event_data;
    // Structured SingleObisData profile
    ODM_SingleObisData singleObisData;
    // structure for DB parameters
    DBparameters DB_parameter;
    // structure for ping node details
    Pingnode_details pingnode_detail;
};

struct DLMSValue
{
    DLMSDataType type;
    std::string typeName;

    // Union to hold different data types
    union {
        bool boolValue;
        int8_t int8Value;
        int16_t int16Value;
        int32_t int32Value;
        int64_t int64Value;
        uint8_t uint8Value;
        uint16_t uint16Value;
        uint32_t uint32Value;
        uint64_t uint64Value;
        float float32Value;
        double float64Value;
    };

    std::vector<uint8_t> octetString;

    DLMSValue() : type(DLMS_DATA_TYPE_NONE), typeName("NONE")
    {
        uint64Value = 0;
    }

    DLMSValue(const DLMSValue &other) : type(other.type), typeName(other.typeName), octetString(other.octetString)
    {
        copyUnion(other);
    }

    DLMSValue &operator=(const DLMSValue &other)
    {
        if (this != &other)
        {
            type = other.type;
            typeName = other.typeName;
            octetString = other.octetString;
            copyUnion(other);
        }
        return *this;
    }

    ~DLMSValue() {}

    void setBool(bool val)
    {
        type = DLMS_DATA_TYPE_BOOLEAN;
        boolValue = val;
    }
    void setInt8(int8_t val)
    {
        type = DLMS_DATA_TYPE_INT8;
        int8Value = val;
    }
    void setInt16(int16_t val)
    {
        type = DLMS_DATA_TYPE_INT16;
        int16Value = val;
    }
    void setInt32(int32_t val)
    {
        type = DLMS_DATA_TYPE_INT32;
        int32Value = val;
    }
    void setInt64(int64_t val)
    {
        type = DLMS_DATA_TYPE_INT64;
        int64Value = val;
    }
    void setUint8(uint8_t val)
    {
        type = DLMS_DATA_TYPE_UINT8;
        uint8Value = val;
    }
    void setUint16(uint16_t val)
    {
        type = DLMS_DATA_TYPE_UINT16;
        uint16Value = val;
    }
    void setUint32(uint32_t val)
    {
        type = DLMS_DATA_TYPE_UINT32;
        uint32Value = val;
    }
    void setUint64(uint64_t val)
    {
        type = DLMS_DATA_TYPE_UINT64;
        uint64Value = val;
    }
    void setFloat32(float val)
    {
        type = DLMS_DATA_TYPE_FLOAT32;
        float32Value = val;
    }
    void setFloat64(double val)
    {
        type = DLMS_DATA_TYPE_FLOAT64;
        float64Value = val;
    }
    void setOctetString(const std::vector<uint8_t> &val)
    {
        type = DLMS_DATA_TYPE_OCTET_STRING;
        octetString = val;
    }
    void setString(const std::string &val)
    {
        type = DLMS_DATA_TYPE_STRING;
        octetString.assign(val.begin(), val.end());
    }

    std::string to_string() const
    {
        switch (type)
        {
            case DLMS_DATA_TYPE_BOOLEAN:
                return boolValue ? "true" : "false";
            case DLMS_DATA_TYPE_INT8:
            case DLMS_DATA_TYPE_NONE:
            case DLMS_DATA_TYPE_ENUM:
                return std::to_string(int8Value);
            case DLMS_DATA_TYPE_INT16:
                return std::to_string(int16Value);
            case DLMS_DATA_TYPE_INT32:
                return std::to_string(int32Value);
            case DLMS_DATA_TYPE_INT64:
                return std::to_string(int64Value);
            case DLMS_DATA_TYPE_UINT8:
                return std::to_string(uint8Value);
            case DLMS_DATA_TYPE_UINT16:
                return std::to_string(uint16Value);
            case DLMS_DATA_TYPE_UINT32:
                return std::to_string(uint32Value);
            case DLMS_DATA_TYPE_UINT64:
                return std::to_string(uint64Value);
            case DLMS_DATA_TYPE_FLOAT32:
                return std::to_string(float32Value);
            case DLMS_DATA_TYPE_FLOAT64:
                return std::to_string(float64Value);
            case DLMS_DATA_TYPE_OCTET_STRING: {
                std::string s;
                for (auto b : octetString)
                    s += std::to_string((int)b) + " ";
                return s;
            }

            case DLMS_DATA_TYPE_STRING:
                return std::string(octetString.begin(), octetString.end());
            default:
                return "0";
        }
    }

    // Generic getters to avoid direct union access
    float getAsFloat(double scale = 1.0) const
    {
        switch (type)
        {
            case DLMS_DATA_TYPE_BOOLEAN:
                return boolValue ? scale : 0.0f;
            case DLMS_DATA_TYPE_INT8:
                return static_cast<float>(int8Value) * scale;
            case DLMS_DATA_TYPE_INT16:
                return static_cast<float>(int16Value) * scale;
            case DLMS_DATA_TYPE_INT32:
                return static_cast<float>(int32Value) * scale;
            case DLMS_DATA_TYPE_INT64:
                return static_cast<float>(int64Value) * scale;
            case DLMS_DATA_TYPE_UINT8:
                return static_cast<float>(uint8Value) * scale;
            case DLMS_DATA_TYPE_UINT16:
                return static_cast<float>(uint16Value) * scale;
            case DLMS_DATA_TYPE_UINT32:
                return static_cast<float>(uint32Value) * scale;
            case DLMS_DATA_TYPE_UINT64:
                return static_cast<float>(uint64Value) * scale;
            case DLMS_DATA_TYPE_FLOAT32:
                return float32Value * scale;
            case DLMS_DATA_TYPE_FLOAT64:
                return static_cast<float>(float64Value * scale);
            default:
                return 0.0f;
        }
    }

    double getAsDouble(double scale = 1.0) const
    {
        switch (type)
        {
            case DLMS_DATA_TYPE_BOOLEAN:
                return boolValue ? scale : 0.0;
            case DLMS_DATA_TYPE_INT8:
                return static_cast<double>(int8Value) * scale;
            case DLMS_DATA_TYPE_INT16:
                return static_cast<double>(int16Value) * scale;
            case DLMS_DATA_TYPE_INT32:
                return static_cast<double>(int32Value) * scale;
            case DLMS_DATA_TYPE_INT64:
                return static_cast<double>(int64Value) * scale;
            case DLMS_DATA_TYPE_UINT8:
                return static_cast<double>(uint8Value) * scale;
            case DLMS_DATA_TYPE_UINT16:
                return static_cast<double>(uint16Value) * scale;
            case DLMS_DATA_TYPE_UINT32:
                return static_cast<double>(uint32Value) * scale;
            case DLMS_DATA_TYPE_UINT64:
                return static_cast<double>(uint64Value) * scale;
            case DLMS_DATA_TYPE_FLOAT32:
                return static_cast<double>(float32Value) * scale;
            case DLMS_DATA_TYPE_FLOAT64:
                return float64Value * scale;
            default:
                return 0.0;
        }
    }

    std::string getAsString() const
    {
        if (type == DLMS_DATA_TYPE_STRING || type == DLMS_DATA_TYPE_STRING_UTF8)
            return std::string(octetString.begin(), octetString.end());
        return to_string(); // fallback to string representation
    }

    const std::vector<uint8_t> &getOctetString() const
    {
        return octetString;
    }

    bool getAsBool() const
    {
        if (type == DLMS_DATA_TYPE_BOOLEAN)
            return boolValue;
        return false;
    }

 private:
    void copyUnion(const DLMSValue &other)
    {
        switch (other.type)
        {
            case DLMS_DATA_TYPE_BOOLEAN:
                boolValue = other.boolValue;
                break;
            case DLMS_DATA_TYPE_INT8:
                int8Value = other.int8Value;
                break;
            case DLMS_DATA_TYPE_INT16:
                int16Value = other.int16Value;
                break;
            case DLMS_DATA_TYPE_INT32:
                int32Value = other.int32Value;
                break;
            case DLMS_DATA_TYPE_INT64:
                int64Value = other.int64Value;
                break;
            case DLMS_DATA_TYPE_UINT8:
                uint8Value = other.uint8Value;
                break;
            case DLMS_DATA_TYPE_UINT16:
                uint16Value = other.uint16Value;
                break;
            case DLMS_DATA_TYPE_UINT32:
            case DLMS_DATA_TYPE_DATETIME:
                uint32Value = other.uint32Value;
                break;
            case DLMS_DATA_TYPE_UINT64:
                uint64Value = other.uint64Value;
                break;
            case DLMS_DATA_TYPE_FLOAT32:
                float32Value = other.float32Value;
                break;
            case DLMS_DATA_TYPE_FLOAT64:
                float64Value = other.float64Value;
                break;
            default:
                uint64Value = 0;
                break;
        }
    }
};

struct Pmeshcmdstructure
{
    uint8_t start_byte;
    uint8_t length;
    uint8_t cmd_type;
    uint8_t panid[4];
    uint8_t src_addr[4];
    uint8_t router_index;
    uint8_t no_of_routers;
    uint8_t data[];
};

struct OtaAtCmdResponse
{
    uint8_t start_byte;
    uint8_t length;
    uint8_t packet_type;
    uint8_t pan_id[4];
    uint8_t gateway_mac_address[4];
    uint8_t node_mac_address[4];
    uint8_t command;
    uint8_t status;
    uint8_t data[];
};

//(added by Supritha K P)
struct DLMSQuerycmd
{
    uint8_t start_byte;
    uint8_t length;
    uint8_t page_index;
    uint8_t frame_id;
    uint8_t cmd_id;
    uint8_t sub_cmd_id;
    uint8_t data_index;
    uint8_t crc;
};
struct UnsilenceQuerycmd
{
    uint8_t start_byte;
    uint8_t cmd;
};

struct PMESHQuerycmd
{
    uint8_t start_byte;
    uint8_t length;
    uint8_t cmd_type;
    uint8_t panid[4];
    uint8_t src_addr[4];
    uint8_t router_index;
    uint8_t no_of_routers;
    uint8_t data[128];
};
#pragma pack(1)
struct OtaCmdResponse
{
    uint8_t start_byte;
    uint8_t length;
    uint8_t packet_type;
    uint8_t pan_id[4];
    uint8_t gateway_mac_address[4];
    uint8_t node_mac_address[4];
    uint8_t remaining_pkt_cnt;
    uint8_t current_pkt_index;
    uint8_t data[];
};
#pragma pack()

#pragma pack(1)
struct dlms_rxd_dlms_obis_payload
{
    int8_t index_byte;
    int8_t obis_code[6];
    int8_t scalar_status_byte;
    int8_t scalar_data_type;
    int8_t scalar_value;
    int8_t unit_status_byte;
    int8_t unit_data_type;
    int8_t unit_value;
};
#pragma pack()

class InitProfile
{
 public:
    // Structured NP profiles
    ScalarProfile_init name_plate;
    // Structured IP profile
    ScalarProfile_init IP; // Instantaneous Profile
    ScalarProfile_init BLP;
    ScalarProfile_init DLP;
    ScalarProfile_init BH;
};

struct OtaFlashState
{
    uint8_t unsilence;
    uint8_t read_flash;
    uint8_t flash_write;
    uint8_t exit;
};

class StateTransition
{
 public:
    uint8_t targetState = 0;
    uint8_t currentState = 0;
    uint8_t timeoutState = 0;
};

struct HesCycleState
{
    int last_hour = -1;
    int current_cycle_id = -1;
    uint8_t done_mask = 0;
};

class Client : public MQTTClient, public virtual BaseLogger, public ODMProfiles, public virtual PushData, public virtual PullData
{
 private:
    int client_socket = -1;
    // StateTransition stateInfo;
    bool comm_init_status = true;
    bool gatewayid_logging_enabled = true;

    int current_ip_cycle = -1;
    //==============================Added by LHK===================//
    std::unique_ptr<MySqlDatabase> db;
    std::unique_ptr<Fuota> fuota;

 public:
    Client();
    ~Client();

    pollfd pfd[3];
    int polltimeout = 0;
    char gateway_id[17] = {0};
    unsigned char rx_buffer[BUFFER_SIZE] = {0};
    StateTransition stateInfo;

    std::string time_str; // For storing formatted time string(added by Amith KN)
    bool gatewayStatus = Status::CONNECTED;
    bool duplicate_gateway = false;
    uint8_t tx_page_index = 0;          // to track page index of received packet
    uint32_t pp_write_payload_tail = 0; // last 4 bytes of programmable-parameter write frame

    ODMProfiles odmProfiles; // access all ODM profiles

    uint8_t src_addr_check_buffer[4]; // Buffer to hold source address for validation(added by Amith KN)
    bool need_to_validate_src_addr;

    uint8_t client_rx_buffer[MAX_CLIENT_RX_BUFFER]; // Buffer for receiving data
    uint32_t buffer_rx_length;
    uint8_t retry = 1; // Retry counter (added by Amith KN)
    std::vector<uint8_t> Pmesh_header;

    // // Added by Amith KN (23/12/25)
    // //  Store scalar values per manufacturer
    // std::unordered_map<std::string, ManufacturerScalarData> manufacturer_scalar_map;

    // // Store mapping: meter_serial_no -> (manufacturer, index)
    // std::unordered_map<std::string, MeterInfo> meter_info_map; // meter_serial_no -> MeterInfo

    // Manufacturer details map: {attr_id, index} -> scalar_value
    std::map<std::pair<std::string, uint8_t>, int32_t> manufacture_details;

    // PMESH header structure(added by Amith KN)
    struct PMeshHeader
    {
        uint8_t start_byte;
        uint8_t length; // total packet length (header + command + checksum)
        uint8_t packet_type;
        uint8_t pan_id[4];
        uint8_t source_address[4];
        uint8_t router_index;
        uint8_t hop_count;
        std::vector<uint8_t> dest_address; // hop_count * 4 bytes
    };

    // Added by Supritha K P
    char Source_ID[9] = {0};
    char PAN_ID[5] = {0};
    char meter_manufacture_name[65] = {0};
    char meter_fw_version[65] = {0};
    uint8_t meter_phase = 0; // Assigning value while accessing meter details to insert in meter supported attributes
    InitProfile Profiles;    // access all Init profiles
    // to check rx packet is for correct node
    uint8_t frame_id = 0;
    uint8_t command_id = 0;
    uint8_t unsilence_cmd = 0;

    // to validate remainign packet count and page index of received packet
    uint8_t remaining_pkt_cnt = 0;
    uint8_t page_index_count = 0;
    char pgwid[65] = {0};
    uint8_t val1 = 0, val2 = 0, val3 = 0;
    // Name plate profile accessible across functions
    ODM_NamePlateProfile name_plate;

    unsigned char PMESH_RESP_HEADER[30] = {0X2E, 0X0E, 0X0A, 0X01, 0X02, 0X03, 0X04, 0X01, 0X02, 0X03, 0X04, 0X01, 0X02, 0X03, 0X04};
    unsigned char PMESH_CMD_HEADER[30] = {0X2E, 0X0C, 0X09, 0X01, 0X02, 0X03, 0X04, 0X01, 0X02, 0X03, 0X04, 0X00, 0X00, 0X01, 0X02, 0X03, 0X04};
    unsigned char nameplate_cmd[8] = {0x2B, 0x07, 01, 0x0E, 0x00, 0x00, 0x00, 0x40};
    struct dlms_rxd_dlms_obis_payload *dlms_obis_payload;
    // Added by Supritha K P

    void set_client_socket(int client_socket);
    int get_client_socket(void);
    bool register_client(char *gateway_id, Client *new_client);
    void unregister_client(char *gateway_id, Client *current_client);
    int set_recv_timeout_for_client(uint32_t time_in_sec);
    void set_poll_timeout(int timeout_in_sec);
    int initCommunication(uint8_t *buffer, ssize_t length);
    void create_mqtt_client(void);
    void create_gateway_log_file(void);
    ssize_t receive_data(uint8_t *buff, size_t buffSize);
    int client_received_data(uint8_t *buffer, ssize_t length);
    void poll_timeout_handler(void);
    void process_ondemand_request(void);
    int receive_data_and_validate_response(void);
    int validate_received_data(uint8_t *buffer, ssize_t length);
    int is_valid_packet(uint8_t *buff, int length);
    int client_process_data(uint8_t *buff, ssize_t length);
    std::vector<std::string> split(const std::string &str, char delim);                                                 // Utility: Split string by delimiter(added by Amith KN)
    int write_to_client(uint8_t *buf, size_t length);                                                                   // Write data to client socket(added by Amith KN)
    static void client_get_time(std::string &time_str, int format_type);                                                // Get current time as string(added by Amith KN)
                                                                                                                        // Function to frame PMESH packet
    std::vector<uint8_t> build_odm_pmesh_packet(const PMeshHeader &header, const uint8_t *command, size_t command_len); //(added by Amith KN)
    std::vector<uint8_t> build_pmesh_frame(const std::vector<std::string> &parts);                                      //(added by Amith KN)
    bool has_pending_cancel();                                                                                          //(added by Amith KN)
    void get_destination_address(uint8_t *buf);                                                                         //(added by Amith KN)
    void print_data_in_hex(const uint8_t *data, uint32_t length);                                                       //(added by Amith KN)
    int validate_source_address(uint8_t *data);                                                                         //(added by Amith KN)
    void Process_ODM_request(uint8_t *buf, size_t length, int request_id, uint8_t command);
    int log_dlms_data_type(uint8_t data_type, uint8_t *offset, uint8_t *data_offset, uint8_t *field_length, const uint8_t *data); //(added by Amith KN)
    bool handle_success(int request_id, uint8_t command, uint8_t *buf);                                                           //(added by Amith KN)
    bool resend_packet(uint8_t *buf, size_t length, int request_id);                                                              //(added by Amith KN)
    bool handle_connection_failed(int request_id);                                                                                //(added by Amith KN)
    bool handle_next_page(uint8_t *buf, size_t &length, int request_id, uint8_t command, uint8_t &page_index);                    //(added by Amith KN)
    void parse_nameplate_profile(const uint8_t *data, size_t length);                                                             //(added by Amith KN)
    void parse_instantaneous_profile(const uint8_t *data, size_t length);                                                         //(added by Amith KN)
    void parse_daily_load_profile(const uint8_t *data, size_t length);                                                            //(added by Amith KN)
    void parse_block_load_profile(const uint8_t *data, size_t length);                                                            //(added by Amith KN)
    void parse_billing_history_profile(const uint8_t *data, size_t length);                                                       //(added by Amith KN)
    void parse_Events_profile(const uint8_t *data, size_t length);                                                                //(added by Amith KN)
    std::string format_timestamp(uint32_t raw_timestamp);                                                                         //(added by Amith KN)
    int handle_single_obis_read(const OtaCmdResponse *response, uint16_t dlms_len);                                               //(added by Amith KN)
    bool validate_response_frame(uint8_t *buf);                                                                                   //(added by Amith KN)
    uint32_t extract_u32(const OtaCmdResponse *response, uint8_t offset, uint8_t data_off);                                       //(added by Amith KN)
    uint16_t extract_u16(const OtaCmdResponse *response, uint8_t offset, uint8_t data_off);                                       //(added by Amith KN)
    std::string parse_dlms_datetime(const uint8_t *data);                                                                         //(added by Amith KN)
    std::string bytes_to_hex_string(const uint8_t *data, size_t len) const;                                                       //(added by Amith KN)
    DLMSValue extract_dlms_value(uint8_t data_type, const uint8_t *data, uint8_t field_len);                                      //(added by Amith KN)
    bool update_odm_db(uint8_t download_data_type);                                                                               //(added by Amith KN)
    void recalculate_dlms_checksum(uint8_t *buf, size_t total_length);                                                            //(added by Amith KN)
    void clear_profile_for_type(uint8_t download_data_type);
    void Insert_receive_data_offset();                                                         //(added by Amith KN)
    void Update_Insert_Ping_request(uint8_t download_type, uint8_t status);                    //(added by Amith KN)
    void ping_node_details(const std::vector<std::string> &parts, DBparameters &DB_parameter); //(added by Amith KN)
    void validate_NP_for_db();                                                                 //(added by Amith KN)
    void validate_IP_for_db();                                                                 //(added by Amith KN)
    void validate_DLP_for_db();                                                                //(added by Amith KN)
    void validate_BLP_for_db();                                                                //(added by Amith KN)
    void validate_BH_for_db();                                                                 //(added by Amith KN)
    // Ensure common DB string parameters are safe for DB usage
    void ensure_db_parameters();                                                                     //(added helper)
    void validate_voltage_event_for_db();                                                            //(added by Amith KN)
    void validate_current_event_for_db();                                                            //(added by Amith KN)
    void validate_power_event_for_db();                                                              //(added by Amith KN)
    void validate_transaction_event_for_db();                                                        //(added by Amith KN)
    void validate_other_event_for_db();                                                              //(added by Amith KN)
    void validate_non_rollover_event_for_db();                                                       //(added by Amith KN)
    void validate_control_event_for_db();                                                            //(added by Amith KN)
    uint32_t extract_u32_reversed(const OtaCmdResponse *response, uint8_t offset, uint8_t data_off); //(added by Amith KN)
    void addManufacturer(const std::string &manufacturer, const std::map<std::pair<std::string, uint8_t>, int32_t> &values);
    void addMeter(const std::string &meter_serial_no, const std::string &manufacturer, const std::string &firmware_version);

    int processPacketType(uint8_t *rx_buf, int length);
    bool process_NP_case();
    bool process_IP_case();
    bool process_BHP_case(uint8_t download_data_type);
    bool process_DLP_case(uint8_t download_data_type);
    bool process_BLP_case(uint8_t download_data_type);
    bool process_AllEvents_case(uint8_t download_data_type);
    //(added by Supritha K P)
    int init_connection(int val1, int val2, int val3);
    uint8_t *frame_NP_cmd_to_pull_data(uint8_t *path_record, uint8_t hop_count);
    int transmit_command_and_validate_response(uint8_t *buf, size_t length, uint32_t maxRetries);
    int pull_NP_at_init_con();
    int parse_instantaneous_scalar_profile(const OtaCmdResponse *response);
    int parse_billing_scalar_profile(const OtaCmdResponse *response);
    int parse_daily_load_scalar_profile(const OtaCmdResponse *response);
    int parse_block_load_scalar_profile(const OtaCmdResponse *response);
    int pull_scalar_profile();
    uint8_t *frame_Scalar_Profile_cmd_to_pull(uint8_t *path_record, uint8_t hop_count, uint8_t profile);
    uint8_t *frame_unsilence_cmd(uint8_t *path_record, uint8_t hop_count);
    bool unsilence_network();
    int validate_response_buffer(uint8_t *tx_buffer, uint8_t *rx_buffer);
    void store_scalar_profile_db(uint8_t profile_name, const ScalarProfile_init &profile);

    // int insert_name_plate_db();

    /* PULL */ // Added by Puneeth
    NodeInfo *currentNode = nullptr;
    HesCycleState hes_state{};

    void mark_hes_cycle_done(void);
    int get_cycle_id_from_minute(int minute);
    int client_hes_cycle_schedule(void);
    int write_to_client_vector(std::vector<uint8_t> &buff);
    int transmit_command_and_validate_response(std::vector<uint8_t> &buff, uint8_t maxRetries);
    int hes_start_cycle_activiy(const char *gateway_id);
    void update_gateway_details(const char *gateway_id);
    void build_node_list_from_db(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id);
    void add_gateway_node_to_map_list(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id);
    void get_alternate_path_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id);
    void get_missing_info_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id);
    int pull_missing_info_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);
    int try_paths_for_profile_pull(NodeInfo &node, std::vector<uint8_t> (Client::*frame_fn)(uint8_t), uint8_t &page_index, const char *profile_name);
    int pull_profile_pages_on_path(const PathInfo &path, std::vector<uint8_t> (Client::*frame_fn)(uint8_t), uint8_t &page_index, NodeInfo &node);
    void load_scalar_values_from_db(std::array<uint8_t, 8> meter_sl_number);
    void load_scalar_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);

    std::vector<uint8_t> frame_pmesh_command_packet(uint8_t packet_type, PathInfo const &path_info, const std::vector<uint8_t> &cmd);
    int transmit_command_on_path_pmesh(uint8_t packet_type, const std::vector<uint8_t> &cmd, PathInfo const &path, int timeout_retries);
    int flash_save(NodeInfo &node, PathInfo const &path);
    int soft_reset(NodeInfo &node, PathInfo const &path);
    int perform_flash_and_reset(NodeInfo &node, PathInfo const &path);

    int attempt_dlms_reconnect(const PathInfo &path);
    int attempt_dlms_reconnect_all_paths(NodeInfo &node, const PathInfo &failed_path);

    // Unsilence / Fuota status and disable
    int unsilence_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);
    int check_fuota_status(NodeInfo &node);
    int fuota_disable_for_a_node(NodeInfo &node);

    // Nameplate pull
    std::vector<uint8_t> frame_dlms_nameplate_command_packet(uint8_t page_index);
    int pull_missing_nameplate_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);
    int pull_nameplate_for_a_node(NodeInfo &node);

    // IFV
    int pull_internal_firmware_version_for_a_node(NodeInfo &node);
    int pull_missing_internal_firmware_version_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);

    // IP
    std::vector<uint8_t> frame_dlms_ip_command_packet(uint8_t page_index);
    int pull_instantaneous_profile_for_cycle(NodeInfo &node, int cycle);
    int pull_missing_instantaneous_for_a_node(NodeInfo &node);
    int pull_missing_ip_profile_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);

    // DLP pull
    std::vector<uint8_t> frame_dlms_dlp_command_packet(uint8_t page_index);
    int pull_daily_load_profile_for_a_node(NodeInfo &node);
    int pull_missing_daily_load_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);

    // BLP pull
    std::vector<uint8_t> frame_dlms_blp_command_packet(uint8_t page_index);
    int pull_block_load_profile_for_a_node(NodeInfo &node);
    int pull_missing_block_load_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);

    // BHP
    std::vector<uint8_t> frame_dlms_bhp_command_packet(uint8_t page_index);
    int pull_billing_history_profile_for_a_node(NodeInfo &node);
    int pull_missing_billing_history_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info);
};

#endif // __CLIENT_H__