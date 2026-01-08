#ifndef __MYSQL_DATABASE_H__
#define __MYSQL_DATABASE_H__

#include <mysql/mysql.h>

#include <unistd.h>

#include <array>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "String_functions.h"
// #include "fuota.h"
#include "helper.h"
#include "packet_buffer.h"
#include "utility.h"

class Fuota;

#define attribute_id_ip  0x0100
#define attribute_id_dlp 0x0300
#define attribute_id_blp 0x0400
#define attribute_id_bh  0x0200

#define MAX_QUERY_BUFFER 86500

#define PUSH_EVENT_DETAILED_PARSING 0

// Forward declarations to avoid circular include with client.h
class Client;

struct MySQLCredentials
{
    uint16_t port{};
    std::string host;
    std::string username;
    std::string password;
    std::string database;
};

struct meter_data_record
{
    std::vector<uint8_t> meter_mac_address; // router MAC address
    std::vector<uint8_t> path_record;       // your existing vector of uint8_t
    uint8_t hop_count = 0;                  // hop count

    meter_data_record(uint8_t *data, uint8_t len, uint8_t *path, uint8_t length, uint8_t hops)
        : meter_mac_address(data, data + len), path_record(path, path + length), hop_count(hops) {}

    // Default constructor
    meter_data_record() = default;
};

struct path_data_record
{
    std::vector<uint8_t> router_mac_address; // router MAC address
    std::vector<uint8_t> path_record;        // your existing vector of uint8_t
    uint8_t hop_count = 0;                   // hop count

    path_data_record(uint8_t *data, size_t len, uint8_t *path, uint8_t length, uint8_t hops)
        : router_mac_address(data, data + len), path_record(path, path + length), hop_count(hops) {}

    // Default constructor
    path_data_record() = default;
};

struct meter_details
{
    std::vector<uint8_t> meter_manufacture_name; // manufacture name
    std::vector<uint8_t> meter_fw_version;       // manufacture name
    std::vector<uint8_t> meter_mac_address;      // meter_mac_address
    std::vector<uint8_t> path_record;            // path
    uint8_t profile_id = 0;
    uint8_t hop_count = 0;
    uint8_t meter_phase = 0;

    meter_details(uint8_t *data, size_t len, uint8_t *meter_fw_v, uint8_t len_fw, uint8_t *mac_address, uint8_t length, uint8_t *path, uint8_t path_len, uint8_t profile, uint8_t hop, uint8_t meter_type)
        : meter_manufacture_name(data, data + len), meter_fw_version(meter_fw_v, meter_fw_v + len_fw), meter_mac_address(mac_address, mac_address + length), path_record(path, path + path_len), profile_id(profile), hop_count(hop), meter_phase(meter_type) {}

    // Default constructor
    meter_details() = default;
};

struct silenceND
{
    std::vector<uint8_t> meter_mac_address; // meter_mac_address
    std::vector<uint8_t> path_record;       // path
    uint8_t hop_count = 0;

    silenceND(uint8_t *mac_address, size_t length, uint8_t *path, uint8_t path_len, uint8_t hop)
        : meter_mac_address(mac_address, mac_address + length), path_record(path, path + path_len), hop_count(hop) {}

    // Default constructor
    silenceND() = default;
};

// Forward declarations
class EventPowerFailureRestoration;
class EventPowerFailureOccurance;
struct NodeInfo;
class PacketBufferBlockLoad;
struct ODM_NamePlateProfile;

struct ManufacturerScalarData
{
    std::map<std::pair<std::string, uint8_t>, int32_t> scalar_values; // (attribute_id, index) -> signed scalar_value
};

struct MeterInfo
{
    std::string manufacturer;
    std::string firmware_version;
};

class MySqlDatabase : public virtual BaseLogger
{
 private:
    MYSQL *mysql = nullptr;
    MySQLCredentials creds{};
    // Protects access to the MYSQL* handle - mysql client is not safe for
    // concurrent use from multiple threads using the same connection.
    std::mutex mysql_mutex;

 public:
    MySqlDatabase();
    ~MySqlDatabase();

    // Added by Amith KN (23/12/25)
    //  Store scalar values per manufacturer
    std::unordered_map<std::string, ManufacturerScalarData> manufacturer_scalar_map;

    // Store mapping: meter_serial_no -> (manufacturer, index)
    std::unordered_map<std::string, MeterInfo> meter_info_map; // meter_serial_no -> MeterInfo

    char target_mac_address[17] = {0};
    // std::queue<std::vector<uint8_t>> hesNPDataQueue;
    std::vector<uint8_t> dest_address;
    std::queue<path_data_record> hesATPathQueue;
    std::queue<meter_data_record> hesNPDataQueue;
    std::queue<meter_details> meterDetailsQueue;
    std::queue<silenceND> silencedNDQueue;
    std::queue<silenceND> pathSrcRouteQueue;

    void load_mysql_config_from_file(void);
    bool connect_to_mysql();
    bool check_and_reconnect();
    int execute_query(char *query);

    int Update_dlms_on_demand_request_status(unsigned int req_id, RequestStatus status, uint16_t err_code); //(added by Amith KN)
    int Update_dlms_on_demand_Ping_request_status(unsigned int req_id, RequestStatus status);               //(added by Amith KN)
    bool check_path_in_source_route_network(const std::vector<std::string> &parts, int request_id);         //(added by Amith KN)
    float convertScalar(int32_t raw);                                                                       //(added by Amith KN)

    //(added by Supritha K P)
    int update_into_gateway_status_info(const uint8_t *str, bool gatewayStatus, uint8_t val1, uint8_t val2, uint8_t val3);
    int insert_into_gateway_status_info(const uint8_t *str);
    bool insert_into_gateway_connection_log(const uint8_t *str, int val1, int val2, int val3);
    int is_NP_data_available(char *gateway_id);
    int get_alternate_source_route_network_from_db(uint8_t *router_mac_address, char *gateway_id);
    int check_for_scalar_profile(char *gateway_id);
    bool enqueue_info(uint8_t *meter_manufacture_name, uint16_t attribute_id, uint8_t *meter_mac_address, uint8_t meter_phase, char *gateway_id, uint8_t *meter_fw_version);
    int check_for_unsilenced_nodes(char *gateway_id);
    int check_for_fuota_resume(char *gateway_id);
    int compare_schedule_time(const char *schedule_time_str, std::string current_time_str);
    int delete_node_from_db(uint8_t *meter_mac_address, char *gateway_id);
    int delete_dlms_fuota_table(uint8_t *meter_mac_address, char *gateway_id);
    int insert_name_plate_db(const char *target_mac_address, const char *gateway_id, const ODM_NamePlateProfile name_plate, std::string &time_str);
    bool update_dlms_mqtt_info(char *gateway_id, uint8_t status);
    int get_meter_details_from_db(const std::string &manufacturer, const std::string &meter_firmware_version, char *gateway_id);
    void load_scaler_details_from_db(const std::string &meter_serial_no, char *gateway_id, Client *Client);
    int check_for_scaler_before_insert(char *meter_manufacture_name, char *meter_fw_version, int profile_name);
    int32_t getScalarValue(const std::string &meter_serial_no, const std::string &attribute_id, uint8_t index);
    int fetch_path_record_from_src_route_network_db(char *gateway_id);
    //(added by Supritha K P)

    /* PULL */ // Added by Puneeth
    int get_nodes_info_from_src_route_network_db(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id);
    int get_alternate_path_for_node(NodeInfo &node, const char *gateway_id);
    std::vector<int> get_last_hour_missing_ip_cycles_for_node(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    bool is_blp_available_last_hour(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    bool is_dlp_available_previous_day(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    bool is_bhp_available_previous_month(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    bool is_nameplate_available(std::array<uint8_t, 8> node_mac_address);
    bool is_scalar_profile_available(std::array<uint8_t, 8> node_mac_address);
    bool is_node_silenced(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    int is_ifv_available_for_node(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    int delete_node_from_unsilence_nodes_from_fuota(std::array<uint8_t, 8> node_mac_address, const char *gateway_id);
    int insert_update_hes_nms_sync_time(const char *gateway_mac, int status);

    int calculate_cycle_id(int subtract_cycles = 0);
    int calculate_cycle_id_for_block_load(void);
    std::string format_timestamp(uint32_t hex_timestamp);
    std::string parse_dlms_date_time(const uint8_t *data, size_t len);
    std::string get_event_code_string_from_event_code(uint16_t event_code);

    int insert_name_plate_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> *name_plate_data);
    int update_internal_firmware_version_in_meter_details(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, uint8_t *internal_firmware_version);
    int insert_instantaneous_parameters_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, int cycle, PacketBuffer<DlmsRecordMap> &instantaneous_profile, bool push_status);
    int insert_daily_load_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &daily_load_profile, bool push_status);
    int insert_block_load_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, int cycle_id, PacketBufferBlockLoad &block_load_profile, bool push_status);
    int insert_billing_history_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &billing_history, bool push_status);
#if PUSH_EVENT_DETAILED_PARSING
    int insert_power_on_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<EventPowerFailureRestoration> &power_on_event);
    int insert_power_off_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<EventPowerFailureOccurance> &power_off_event);
#else
    int insert_power_on_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &power_on_event);
    int insert_power_off_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &power_off_event);
#endif // PUSH_EVENT_DETAILED_PARSING

    /*.................................Added BY LHK on 20112025......
     *........FUOTA Dependent Functions..............................*/

    int get_alternate_source_route_network_from_db(char *query_buffer, unsigned char *dcuid, unsigned char *target_mac, Fuota *pan_list_ptr);
    int get_rf_internal_firmware_version_from_meter_details(char *query_buffer, unsigned char *dcuid, unsigned char *meter_mac_addreess, char *fw_version_out);
    int get_serial_number_panid_for_gateway(char *query_buffer, unsigned char *dcuid, unsigned char *meter_mac_address, char *panid, char *serial_number);
    int Arrange_route_path(unsigned char *source, unsigned char *dest, short int hop_count);
    int get_the_gateway_undernodes_details_for_fuota(char *query_buffer, unsigned char *dcuid, Fuota *pan_list_ptr);

    int insert_update_fuota_silenced_meter_details_in_db(char *query_buffer, unsigned char *meter_mac_address, unsigned char *dcuid, int status, int hopCount);
    int insert_update_fuota_Unsilenced_meter_details_in_db(char *query_buffer, unsigned char *meter_mac, unsigned char *dcuid, int status, int hopCount);

    int update_ondemand_RF_Fuota_upload_status(char *query_buffer, unsigned int req_id, int status, int error_code);

    int update_fuota_upload_sync_status(char *query_buffer, unsigned char *gateway_id);

    int fetch_pending_fuota_targetnode_path(char *query_buffer, const unsigned char *gateway_id, unsigned char *target_mac, char *file_path, int &request_id, char *file_name);

    int check_gateway_nodelist_empty_or_not(unsigned char *dcuid);
    int fetch_pending_fuota_targetnode_path(char *query_buffer, const unsigned char *gateway_id, unsigned char *target_mac, std::string &file_path, int &request_id, std::string &file_name);
    //...........................................................//
};

#endif // __MYSQL_DATABASE_H__