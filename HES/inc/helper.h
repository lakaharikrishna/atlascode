#ifndef __HELPER_H__
#define __HELPER_H__

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

enum DLMSDataType
{
    DLMS_DATA_TYPE_NONE = 0x00,
    DLMS_DATA_TYPE_ARRAY = 0x01,
    DLMS_DATA_TYPE_STRUCTURE = 0x02,
    DLMS_DATA_TYPE_BOOLEAN = 0x03,
    DLMS_DATA_TYPE_BIT_STRING = 0x04,
    DLMS_DATA_TYPE_INT32 = 0x05,
    DLMS_DATA_TYPE_UINT32 = 0x06,
    DLMS_DATA_TYPE_OCTET_STRING = 0x09,
    DLMS_DATA_TYPE_STRING = 0x0A,
    DLMS_DATA_TYPE_STRING_UTF8 = 0x0C,
    DLMS_DATA_TYPE_BINARY_CODED_DECIMAL = 0x0D,
    DLMS_DATA_TYPE_INT8 = 0x0F,
    DLMS_DATA_TYPE_INT16 = 0x10,
    DLMS_DATA_TYPE_UINT8 = 0x11,
    DLMS_DATA_TYPE_UINT16 = 0x12,
    DLMS_DATA_TYPE_COMPACT_ARRAY = 0x13,
    DLMS_DATA_TYPE_INT64 = 0x14,
    DLMS_DATA_TYPE_UINT64 = 0x15,
    DLMS_DATA_TYPE_ENUM = 0x16,
    DLMS_DATA_TYPE_FLOAT32 = 0x17,
    DLMS_DATA_TYPE_FLOAT64 = 0x18,
    DLMS_DATA_TYPE_DATETIME = 0x19,
    DLMS_DATA_TYPE_DATE = 0x1A,
    DLMS_DATA_TYPE_TIME = 0x1B,
    DLMS_DATA_TYPE_DELTA_INT8 = 0x1C,
    DLMS_DATA_TYPE_DELTA_INT16 = 0x1D,
    DLMS_DATA_TYPE_DELTA_INT32 = 0x1E,
    DLMS_DATA_TYPE_DELTA_UINT8 = 0x1F,
    DLMS_DATA_TYPE_DELTA_UINT16 = 0x20,
    DLMS_DATA_TYPE_DELTA_UINT32 = 0x21,
    DLMS_DATA_TYPE_BYREF = 0x80,
    UNKNOWN_DATA_TYPE = 0x22
};

struct DLMSValueStruct
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

    DLMSValueStruct() : type(DLMS_DATA_TYPE_NONE), typeName("NONE")
    {
        uint64Value = 0;
    }
};

using DlmsRecordMap = std::map<uint8_t, DLMSValueStruct>;

class DlmsValueExtractor
{
 public:
    // Get numeric value based on type field (returns as double for flexibility)
    static std::optional<double> get_numeric(const DlmsRecordMap &records, uint8_t tag)
    {
        auto it = records.find(tag);
        if (it == records.end())
        {
            return std::nullopt;
        }

        const DLMSValueStruct &val = it->second;

        switch (val.type)
        {
            /* Because of simulator issue */
            case DLMS_DATA_TYPE_ARRAY:
                return std::nullopt;

            case DLMS_DATA_TYPE_BOOLEAN:
                return val.boolValue ? 1.0 : 0.0;

            case DLMS_DATA_TYPE_INT8:
            case DLMS_DATA_TYPE_DELTA_INT8:
                return static_cast<double>(val.int8Value);

            case DLMS_DATA_TYPE_UINT8:
            case DLMS_DATA_TYPE_DELTA_UINT8:
            case DLMS_DATA_TYPE_ENUM:
                return static_cast<double>(val.uint8Value);

            case DLMS_DATA_TYPE_INT16:
            case DLMS_DATA_TYPE_DELTA_INT16:
                return static_cast<double>(val.int16Value);

            case DLMS_DATA_TYPE_UINT16:
            case DLMS_DATA_TYPE_DELTA_UINT16:
                return static_cast<double>(val.uint16Value);

            case DLMS_DATA_TYPE_INT32:
            case DLMS_DATA_TYPE_DELTA_INT32:
                return static_cast<double>(val.int32Value);

            case DLMS_DATA_TYPE_UINT32:
            case DLMS_DATA_TYPE_DELTA_UINT32:
            case DLMS_DATA_TYPE_DATETIME:
                return static_cast<double>(val.uint32Value);

            case DLMS_DATA_TYPE_INT64:
                return static_cast<double>(val.int64Value);

            case DLMS_DATA_TYPE_UINT64:
                return static_cast<double>(val.uint64Value);

            case DLMS_DATA_TYPE_FLOAT32:
                return static_cast<double>(val.float32Value);

            case DLMS_DATA_TYPE_FLOAT64:
                return val.float64Value;

            default:
                return std::nullopt;
        }
    }

    // Get numeric with default value
    static double get_numeric_or_default(const DlmsRecordMap &records, uint8_t tag, double default_value = 0.0)
    {
        return get_numeric(records, tag).value_or(default_value);
    }

    // Apply lambda to numeric value (preserves original type)
    template <typename Func>
    static auto apply_to_numeric(const DlmsRecordMap &records, uint8_t tag, Func func) -> decltype(func(std::declval<uint32_t>()))
    {
        auto it = records.find(tag);
        if (it == records.end())
        {
            return func(0);
        }

        const DLMSValueStruct &val = it->second;

        switch (val.type)
        {
            /* Because of simulator issue */
            case DLMS_DATA_TYPE_ARRAY:
                return func(0);

            case DLMS_DATA_TYPE_BOOLEAN:
                return func(val.boolValue);

            case DLMS_DATA_TYPE_INT8:
            case DLMS_DATA_TYPE_DELTA_INT8:
                return func(val.int8Value);

            case DLMS_DATA_TYPE_UINT8:
            case DLMS_DATA_TYPE_DELTA_UINT8:
            case DLMS_DATA_TYPE_ENUM:
                return func(val.uint8Value);

            case DLMS_DATA_TYPE_INT16:
            case DLMS_DATA_TYPE_DELTA_INT16:
                return func(val.int16Value);

            case DLMS_DATA_TYPE_UINT16:
            case DLMS_DATA_TYPE_DELTA_UINT16:
                return func(val.uint16Value);

            case DLMS_DATA_TYPE_INT32:
            case DLMS_DATA_TYPE_DELTA_INT32:
                return func(val.int32Value);

            case DLMS_DATA_TYPE_UINT32:
            case DLMS_DATA_TYPE_DELTA_UINT32:
                return func(val.uint32Value);

            case DLMS_DATA_TYPE_INT64:
                return func(val.int64Value);

            case DLMS_DATA_TYPE_UINT64:
                return func(val.uint64Value);

            case DLMS_DATA_TYPE_FLOAT32:
                return func(val.float32Value);

            case DLMS_DATA_TYPE_FLOAT64:
                return func(val.float64Value);

            default:
                return func(0);
        }
    }

    // Get bool value
    static bool get_bool(const DlmsRecordMap &records, uint8_t tag, bool default_value = false)
    {
        auto it = records.find(tag);
        if (it == records.end())
        {
            return default_value;
        }

        const DLMSValueStruct &val = it->second;

        if (val.type == DLMS_DATA_TYPE_BOOLEAN)
        {
            return val.boolValue;
        }

        // Try to interpret numeric as bool
        auto num = get_numeric(records, tag);
        if (num.has_value())
        {
            return num.value() != 0.0;
        }

        return default_value;
    }

    // Get OctetString
    static std::vector<uint8_t> get_octet_string(const DlmsRecordMap &records, uint8_t tag)
    {
        auto it = records.find(tag);
        if (it == records.end())
        {
            return {};
        }

        const DLMSValueStruct &val = it->second;

        if (val.type == DLMS_DATA_TYPE_OCTET_STRING || val.type == DLMS_DATA_TYPE_BIT_STRING || val.type == DLMS_DATA_TYPE_STRING || val.type == DLMS_DATA_TYPE_STRING_UTF8)
        {
            return val.octetString;
        }

        return {};
    }

    // Get the entire DLMSValueStruct struct
    static std::optional<DLMSValueStruct> get_value(const DlmsRecordMap &records, uint8_t tag)
    {
        auto it = records.find(tag);
        if (it != records.end())
        {
            return it->second;
        }
        return std::nullopt;
    }

    // Check if tag exists
    static bool has_tag(const DlmsRecordMap &records, uint8_t tag)
    {
        return records.find(tag) != records.end();
    }

    // Get type name
    static std::string get_type_name(const DlmsRecordMap &records, uint8_t tag)
    {
        auto it = records.find(tag);
        if (it != records.end())
        {
            return it->second.typeName;
        }
        return "NOT_FOUND";
    }
};

#endif // __HELPER_H__