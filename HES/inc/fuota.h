

#ifndef __FUOTA_H__
#define __FUOTA_H__

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <experimental/filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

// #include "database.h"
// #include "mqtt.h"
// #include "server.h"
#include "utility.h"

using namespace std;

#define FUOTA_DATA_QUERY            0x9
#define FUOTA_RESP_PACKET_TYPE      0xA
#define MESH_COMMISSION_PACKET_TYPE 0x3
#define COMMISSION_RESP_PACKET_TYPE 0x4
#define HES_START_BYTE              0x2E
#define MAXI_PATHS                  15
#define CLIENT_TX_BUFFER_SIZE       1024
#define MAX_ROUTE_HOPS              10

class Client;        // forward declare
class MySqlDatabase; // forward declare

#pragma pack(1)
struct fuota_context
{
    std::string firmware_path;
    FILE *firmware_fp = nullptr;
    size_t firmware_size = 0;
    int flash_pages = 0;
    int flash_subpages = 0;
    size_t header_len = 0;
    bool valid = false;
};
#pragma pack()

#pragma pack(1)
struct route_entry
{
    unsigned char target_mac[8];
    unsigned char route_path[128]; // Multiple 4-byte MACs
    unsigned char hop_count;
};
#pragma pack()

#pragma pack(1)
struct alternate_path_info
{
    unsigned char meter_mac_address[8];
    unsigned char route_path[128];
    unsigned char hop_count;
    std::unique_ptr<alternate_path_info> next;
};
#pragma pack()

class FUOTA_REQ_STATUS
{
 public:
    enum
    {
        /*.................FUOTA UPLOAD STATE STATUS INFO..........*/
        FUOTA_REQUEST_FAILED = 0,
        FUOTA_SUCCESS = 14,
        FUOAT_FAIL,

        SILENCE_PATH_TO_GATEWAY_NODE = 42,
        SILENCE_THE_TARGET_NODE,
        NETWORK_SILENCE,

        FIRMWARE_SECTOR_READ,
        FW_SECTOR_DIVIDE_TF_TO_TARGET_NODE,
        ERASE_FLASH_AREA,
        FW_IMAGE_TF_TO_TARGET_NODE,
        FW_IMAGE_ENDOF_PAGETF_STATUS,
        NEWLY_LOADED_FW_IMAGE_CRC_CALCULATE,
        NEWLY_LAODED_FWIMAGE_SECTOR_FRAME_STATUS,
        FIRMWARE_ACTIVATE_IMAGE_STATUS,
        SEND_FIRMWARE_ACTIVATE_CMD,
        READ_AND_COMPARE_FIRMWARE_VERSION,

        NETWORK_UNSILENCE,
        UNSILENCE_GATEWAY_NODE,
        PMESH_FUOTADONE_DISABLE_FUOTA_4_GATEWAY,
        GATEWAY_PATH_SILENCE_DISABLE_4_FUOTA,
        GATEWAY_ROLLBACKING_TO_NORMAL_COMM_MODE = 53,
        FIRMWARE_RESP_MISMATCH,

        FUOTA_PAGE_RETRY_STATUS,
        FUOTA_FILE_NOT_FOUND,
        FUOTA_FILE_SIZE_ZERO,
        FUOTA_FILE_SIZE_MISMATCH,
        FUOTA_FILE_READ_ERROR,
        FUOTA_FILE_OPEN_ERROR,
        FUOTA_FILE_CLOSE_ERROR,
        FUOTA_FILE_READ_SUCCESS,
        FUOTA_FILE_WRITE_SUCCESS,
        FUOTA_FILE_WRITE_ERROR,
        FUOTA_SESSION_INITIATED,
        FUOTA_SESSION_COMPLETED,
        FUOTA_SESSION_FAILED,
        FUOTA_SESSION_ABORTED,
        FUOTA_PATH_SILENCE_DISABLE_FAILED,
        FUOTA_PATH_SILENCE_DISABLE_DONE,
        METER_FUOTA_FIRMWARE_READ_FAILED,
        METER_FWREAD_DONE_ENTERINTO_TARGET_ENABLE,
        FUOTA_PATH_SILENCE_BEGIN,
        GATEWAY_NOT_RESPONDING_READ,
        GATEWAY_NOT_RESPONDING_WRITE,
        COMMAND_RESP_TIMEOUT,

        /*.................FUOTA UPLOAD STATE STATUS INFO..........*/
    };
};

typedef enum
{
    e_success_0 = 0,
    e_success_1,
    e_success_2, // resp timeout
    e_success_3, // command in prog
    e_success_4,
    e_waiting,
    e_failure = -1,
    e_not_found = -2,
    YES = 1,
    NO,
    e_pending_wait = -99

} status;

struct LastSectorStatus
{
    int page;
    int subpage;
    bool valid;
    LastSectorStatus() : page(0), subpage(0), valid(false) {}
};

struct OndemandFuotaDetails
{
    uint8_t gateway_id[16];
    uint8_t target_mac[16];
    uint8_t hop_count;
    uint8_t path[128];
    uint8_t previous_fw_version[128];
    uint8_t new_fw_version[128];
};

struct FuotaDetails
{
    uint8_t gateway_id[8];
    uint8_t target_mac[8];
    uint8_t hop_count;
    uint8_t path[128];
    uint8_t previous_fw_version[128];
    uint8_t new_fw_version[128];
};

typedef struct fuota_route_path_details
{
    unsigned char paths[32][4]; // Each path is a 4-byte MAC
    unsigned char hop_count;
} router_path_t;

// appnd common header
#pragma pack(1)
struct common_header
{
    unsigned char pkt_length; // Includes the header as well
    unsigned char flags;
    unsigned char panid[4];
};
#pragma pack()

#pragma pack(1)
struct data_query_header
{
    unsigned char source_address[4];
    unsigned char router_index;
    unsigned char num_routers;
    unsigned char addr_array[1]; /* Variable size array of 4 byte routers*/
};
#pragma pack()

typedef struct pmesh_path_details
{
    unsigned char paths[128];
    unsigned char hop_count;

} path_details;

#pragma pack(1)
struct gateway_details
{
    vector<struct meter_vital_info *> *meter_list;
    short int total_meter_under_gatway;
    unsigned char Firmware_version_read[120];
    unsigned char Old_Fw_version_read[120];
};
#pragma pack()

#pragma pack(1)
struct meter_vital_info
{
    unsigned char meter_mac_address[8];
    unsigned char meter_serial_number[8];
    unsigned char route_path[100];
    unsigned char hop_count;
    int count;
    int download_data_sts;
    unsigned char panid[4];
    unsigned char serial_no[16];
    bool panid_serial_loaded;
};
#pragma pack()

struct fuota_session_t
{
    std::string firmware_path;
    std::string file;
    FILE *file_ptr;
    long firmware_size;
    int flash_page_count;
    int flash_subpage_count;
    int header_len;
    int max_payload_size;
};

struct alternate_paths
{
    unsigned char paths[256]; // in future it should be link list
    unsigned char hop_count;
};

typedef struct on_deman_request_rf
{
    char meter_serialnumber[9];
    char gateway_id[17];
    unsigned char no_alternate_path;
    unsigned char paths_track_index;
    struct alternate_paths paths_index[MAXI_PATHS];
    unsigned int data_type;
    unsigned char cmd[128];
    unsigned char cmd_len;
    int ping_id;
    int no_of_ping;
    int ping_duration;
} rf_request_details;

typedef struct condemandrequest
{
    unsigned char nic_type; // 0-Gateway,1-Router
    union {
        rf_request_details *rf_ondemand_ptr;
    };
} condemandrequest;

struct Fuota_retry_contex
{
    int retry_count = 0;
    int max_retries = 3;
    int alternate_retry = 0;
    int alternate_maxcnt = 2;
};

class FUOTA_MODE
{
 public:
    enum
    {
        FUOTA_UPDATE,
        FUOTA_DISABLE
    };
};

class FUOTA_STATE
{
 public:
    enum
    {
        OPEN_FILE = 0,
        GATEWAY_PATH_SILENCE,
        TARGET_NODE_SILENCE,
        NETWORK_SILENCE,
        SECTOR_READ,
        FWIMAGE_OF_SECTOR_COUNT,
        ERASE_FLASH,
        FW_IMAGE_TRANSFER,
        LAST_UPDATE_FWIMAGE_OF_SECTORCOUNT,
        FW_IMAGE_ENDOF_PAGE_TRANSFER,
        NEW_FWIMAGE_CRC_CALCULATE,
        ACTIVATE_NEW_FWIMAGE,
        SEND_ACTIVATE_CMD,
        READ_AND_COMPARE_FW_VERSION,
        NETWORK_UNSILENCE,
        TARGET_NODE_UNSILENCE,
        GATEWAY_PATH_UNSILENCE,
        ROLLBACK_TO_NORMAL_COMM_MODE,
        IDLE
    };
};

class PATH_SILENCE_STATE
{
 public:
    enum
    {
        AT_FUOTA_ENABLE = 0,
        AT_FUOTA_MODE_ENTRY,
        AT_ENABLE_FLASHSAVE,
        AT_ENABLE_FLASHEXIT
    };
};

class PATH_UNSILENCE_STATE
{
 public:
    enum
    {
        AT_FUOTA_DISABLE = 0,
        AT_FUOTA_MODE_ENTRY_DISABLE,
        AT_DISABLE_FLASHSAVE,
        AT_DISABLE_FLASHEXIT
    };
};

enum class FuotaTimeoutResult
{
    RETRY_SAME_ROUTE,
    RETRY_ALTERNATE_ROUTE,
    NEXT_FROM_QUEUE,
    TERMINATE
};

class FuotaStatus
{
 public:
    enum Type
    {
        FUOTA_SUCCESS = 0,
        FUOTA_RETRY,
        FUOTA_JUMP,
        FUOTA_ABORT,
    };
};

// enum class FuotaStatus : uint8_t
// {
//     FUOTA_SUCCESS = 0,
//     FUOTA_RETRY,
//     FUOTA_JUMP,
//     FUOTA_ABORT
// };

struct StepStat
{
    int total = 0;
    int success = 0;
};

class Fuota : public virtual BaseLogger
{
 private:
    Client &client;    // reference, NOT pointer
    MySqlDatabase &db; // reference
    static constexpr int MAX_NODE_RETRIES = 3;
    static constexpr float MIN_SUCCESS_PERCENT = 70.0f;

 public:
    Fuota(Client &client, MySqlDatabase &db);
    ~Fuota();
    FILE *fouta_read_fd = NULL;
    std::string firmware_path;
    std::string firmware_file;
    std::string dcu_folder;
    std::string fullpath;
    std::vector<uint8_t> cmd_bytes;
    std::vector<gateway_details> node_list; // all meters under this gateway
    std::atomic<bool> thread_run{true};

    std::vector<uint8_t> odmfuota_cmd;
    struct timeval startTime;
    struct data_query_header *req_header;
    Fuota_retry_contex cntx;
    bool cntx_active = false;
    struct timespec start_time;

    std::unique_ptr<alternate_path_info> alternatepaths = nullptr;
    path_details router_path;
    struct gateway_details *gate_node;
    struct fuota_session_t session;
    condemandrequest *ondemand_request;
    std::vector<struct route_entry *> *gateway_route_info;
    //............Resume fuota ...........//
    bool resume_fuota_flag = false;
    std::string resume_file_name;
    std::string resume_fwpath;
    unsigned char resumed_target_mac[20] = {0};

    // retry upon no response
    int fuota_retry_count = 0;
    static constexpr int MAX_FUOTA_RETRIES = 3;
    static constexpr int FUOTA_TIMEOUT_SEC = 12;
    bool waiting_for_response = false;

    // Pending terminal completion reported via status updates; FSM should dequeue/process next
    std::atomic<bool> pending_terminal_complete{false};
    std::atomic<int> pending_terminal_request_id{-1};

    /*
     *..................FUOTA Related commands.................
     */
    unsigned char image_tf_command[128];
    unsigned char image_transfer[7] = {0x2F, 0x06, 0x06, 0x01, 0x00, 0x00, 0x00}; // image transfer
    unsigned char rf_fuota_expected_resp_header[30] = {0x2D, 0x08, 0x07, 0x01};   // for rf fuota
    unsigned char read_rf_fw[6] = {0x2B, 0x05, 0x02, 0x00, 0x00, 0x0F};
    unsigned char rf_internal_fw[3] = {0x00, 0x49, 0x46};

    unsigned char fuota_enable[3] = {0x01, 0x9B, 0x01};
    unsigned char fuota_enable_resp[2] = {0x9B, 0x00};

    unsigned char fuota_updatemode[3] = {0x01, 0x9D, 0x01};
    unsigned char fuota_updatemode_resp[2] = {0x9B, 0x00};

    unsigned char fuota_disable_cmd[3] = {0x01, 0x9B, 0x00};
    unsigned char fuota_disable_resp[2] = {0x9B, 0x00};

    unsigned char fuota_modeentry_disable_cmd[3] = {0x01, 0x9D, 0x00};
    unsigned char fuota_mode_entrydisable_resp[2] = {0x9D, 0x00};

    unsigned char flash_write[2] = {0x00, 0x02};
    unsigned char flashwrite_resp[2] = {0x02, 0x00};

    unsigned char flash_exit[2] = {0x00, 0x01};
    unsigned char flashexit_resp[2] = {0x01, 0x00};

    unsigned char sector_read[4] = {0x2F, 0x03, 0x00, 0x32}; // sector read command
    unsigned char getsector_resp[6] = {0x2D, 0x05, 0x01, 0x10, 0x00, 0x43};

    unsigned char initiate_fw_info[12] = {0x2F, 0x0B, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    unsigned char sectorcount_resp[5] = {0x2D, 0x04, 0x03, 0x00, 0x34}; // 2D 04 03 00 34

    unsigned char Erase_flash_area[5] = {0x2F, 0x04, 0x04, 0x01, 0x38};    // erase block
    unsigned char eraseblk_resp[6] = {0x2D, 0x05, 0x05, 0x01, 0x00, 0x38}; // 2D 05 05 01 00 38

    unsigned char Endof_image_transfer[5] = {0x2F, 0x04, 0x08, 0x01, 0x3C};     // endof image transfer 2F 04 08 01 3C
    unsigned char Verify_image[7] = {0x2F, 0x06, 0x0A, 0x01, 0x00, 0x00, 0x00}; // verify image
    unsigned char verifyimg_response[6] = {0x2D, 0x05, 0x0B, 0x01, 0x00, 0x3E};

    unsigned char lastsector_page_read[5] = {0x2F, 0x04, 0x0C, 0x01, 0x40}; // lastupdate of f/w sector pgcount
    unsigned char activate_read[5] = {0x2F, 0x04, 0x0E, 0x01, 0x42};        // activate image
    unsigned char activate_status_read[5] = {0x2F, 0x04, 0x10, 0x01, 0x44}; // 2F 04 10 01 44

    /*
     *.............FUOTA Related Members.................
     */
    unsigned char client_tx_buffer[1024] = {0};
    unsigned char client_rx_buffer[8192] = {0};
    unsigned char client_query_buffer[86500] = {0};
    unsigned char nodeinfo_tx[1024] = {0};

    char serial_no[16] = {0};
    int fuota_resp_status;

    bool use_alternate_fuota_route = false;
    bool use_alternate_fuotaroute_disable = false;

    unsigned int ondemand_fuota_state;
    unsigned int fuota_gateway_silence_state = 0;
    unsigned int fuota_targetnode_silence_state = 0;
    unsigned int fuota_targetnode_Unsilence_state;
    unsigned int fuota_gateway_Unsilence_state;
    unsigned int network_silence_state;
    unsigned int network_Unsilence_state;
    unsigned char current_fuota_target_mac[8] = {0};
    unsigned char *current_fuota_command; // current_command
    unsigned int fuotadownload_mode;
    unsigned char current_fuota_route[128];
    unsigned char ond_mac[8] = {0};
    unsigned char ondemand_mac_addr[16];
    unsigned int silence_retrycount;
    unsigned int ondemand_request_id;
    unsigned int fuota_download_type;

    std::string latest_firmware_path;
    unsigned int total_firmwaresize;
    unsigned int sector_value = 0;
    unsigned char flash_sub_page;
    unsigned short int flash_page;

    bool rf_fuota_crc_calculated = false;
    unsigned short rf_fuota_crc_value = 0;
    unsigned short int crc_tab16[256];
    uint16_t crc = 0;
    unsigned short int CRC = 0x0000;

    unsigned char FUOTA_Resp_Header_Len;
    unsigned char FUOTA_Cmd_Header_Len;
    unsigned char command_length, response_length;
    unsigned char fuota_imagetf_retry_count, fuota_maxretry;
    int fuotaimagetf_alternate_count = 0;
    bool using_alternate_path = false;
    uint8_t BASE_HDR_LEN = 0;

    unsigned int page_count;
    unsigned char subpage_count = 0;
    bool is_rf_skip = 0;
    unsigned int no_of_bytes_read_file;
    unsigned int no_of_bytes_read_serial;
    unsigned char ffile_rxdata[128] = {0};
    unsigned char fuota_rbuf[130] = {0};
    unsigned char temp_expect_buf[64] = {0};
    uint16_t payload_len = 0;

    //.........................ONDEMAND REQUEST VARIABLES
    unsigned int ondemand_data_download_completed;
    unsigned int prev_download_mode;
    unsigned int prev_client_current_state;
    unsigned int prev_dcu_comm_state;
    unsigned int prev_pmesh_download_data_type;
    unsigned int pmesh_download_data_type;
    unsigned char dcu_short_addr[4] = {0};
    unsigned char panid[4] = {0};
    //...................................................
    uint8_t last_payload[256];
    uint16_t last_payload_len = 0;

    /*
     *.......................FUOTA Function ProtoTypes.........
     */
    void delete_meter_list(void);
    void print_alternate_paths();
    void process_fuota_queue();
    void handle_fuota_completion();
    void open_requested_firmware(const std::string &base_dir, const std::string &requested_filename, std::string &out_filepath);
    bool wait_after_flash(int wait_sec, std::atomic<bool> &thread_run);
    bool parse_and_update_fuota_request(const std::string &cmd);

    void insert_alternate_path(const unsigned char *mac_address, const unsigned char *route_path_full, unsigned char hop_count);
    LastSectorStatus rffuota_read_lastsector_page_and_subpage_status();

    //...............Primary path functions for FUOTA..................//
    int frame_pmesh_ota_cmd_header(unsigned char packet_type, unsigned char *cmd_data, int cmd_len);
    int client_get_time(char *time_str);

    //..............Utility Functions for FUOTA..................//
    std::string get_latest_firmware_file(std::string &directory_path);
    std::string get_latest_firmware_file_by_mtime(const std::string &directory_path);
    void get_latestfirmware_filepath(const std::string &base_dir, const std::string &requested_filename, std::string &out_filepath);
    std::string get_app_base_path();

    void set_latest_firmware_path(const std::string &path);
    int dynamic_fuota_calculate();
    int calculate_dynamic_header_len(uint8_t router_path_count);
    long get_fw_size(const std::string &file_path);
    unsigned int get_max_page_count(long fw_size);
    int get_max_payload_size(int header_len);
    int get_max_sub_page_count(int header_len);
    int get_min_payload_size(int header_len);

    //..............primary path function for target node..........//
    int frame_pmesh_fuota_cmd_header_for_target_node(unsigned char packet_type, unsigned char *cmd_data, int cmd_len, unsigned char *target_mac, router_path_t *target_route);
    //---------------For network silence & unsilience framing header
    int frame_fuota_cmdheader_for_network(unsigned char packet_type, unsigned char *cmd_data, int cmd_len, unsigned char *target_mac, router_path_t *target_route);

    int cansend_fuota_next_command();

    int build_and_store_fuota_cmd();
    int prepare_gateway_silence_cmd();
    int prepare_targetnode_silence_cmd();
    int prepare_silence_the_entire_network_for_fuota();
    int prepare_get_sector_read_for_target_node();
    int prepare_fwimage_of_sector_count_cmd();
    int prepare_flash_erase_command_to_target_node();
    // int prepare_to_fileFetch_and_firmware_transfer_to_target_node();

    void calculate_crc_for_target_node();
    uint16_t crc16_update(const uint8_t *data, uint32_t len, uint16_t crc);
    void init_crc16_tab(bool &crc_tab16_init, unsigned short int *crc_tab16);
    void ensure_crc_table();
    void reset_crc(unsigned short int &crc);

    uint8_t update_fuota_response_buff();
    uint8_t update_fuota_image_transfer_tx_buff();
    unsigned int fuota_uploading_process();
    void print_data_in_hex(unsigned char *buffer, unsigned int length);
    unsigned char calculate_checksum(unsigned char input_buff[], unsigned short buff_len);
    int handle_fuota_post_read(int bytes_read);
    void fix_page_subpage_and_seek();

    int prepare_and_get_the_endof_image_transfer();
    int prepare_and_calculate_and_verify_crc_value();
    int prepare_and_get_the_firmware_activate_command();
    int prepare_and_get_the_firmware_activate_status_command();
    int prepare_to_read_the_firmware_version_for_comparision();
    int prepare_commands_to_Unsilence_the_network_for_fuota();
    int prepare_to_Unsilence_the_target_node();
    int prepare_to_Unsilence_the_gateway_node();
    int prepare_rollback_to_normal_state();

    //..............FUOTA SEQUENCE WRAPPER FUNCTIONS...........//
    int perform_fuota_sequence_with_paths(unsigned char *target_mac);
    bool build_primary_route(unsigned char *target_mac);
    bool build_alternate_route(unsigned char *target_mac);
    bool fetch_and_copy_meter_list_from_db(std::vector<meter_vital_info> &out_meters);
    bool build_gateway_route_info_from_meters(const std::vector<meter_vital_info> &meters);
    std::vector<size_t> detect_leaf_nodes(const std::vector<meter_vital_info> &meters);
    std::vector<size_t> filter_out_on_demand(const std::vector<size_t> &leaf_indices, const std::vector<meter_vital_info> &meters, const unsigned char *ondemand_mac);
    bool send_fuota_enable_to_leaf(const meter_vital_info &leaf);
    void free_gateway_route_info();
    int write_fuota_sequnce_of_commands();
    int process_rffuota_enable_sequence_refactored(unsigned char *ondemand_mac);
    //.........................................................//

    //...............FUOTA Unsilence.................//
    int execute_fuota_sequence_for_unsilence_network();
    int process_rffuota_disable_sequence(unsigned char *ondemand_mac);
    bool send_fuota_disable_to_leaf_nodes(const meter_vital_info &leaf);
    //...............................................//

    //..............Validate and Process of Fuota data............//
    int validate_the_fuota_response_basedon_packet_type(unsigned char *data, unsigned int len);
    int client_process_rf_fuota_data(unsigned char *data, int len);
    int handling_of_pmesh_error_routines_for_retry_at_fuota();
    int frame_a_retry_comamnd_for_pmesh_routine();
    int command_response_timeout_at_fuota();
    int client_process_ondemand_data(void);
    int update_client_tx_buffer_with_route(unsigned char *client_tx_buffer, const unsigned char *payload, int payload_len);
    int comparision_of_firmware_from_meterdetails_and_from_latest_uploaded_firmware(unsigned char *data);
    FuotaTimeoutResult decide_timeout_action();
    void prepare_alternate_route_retry(uint8_t *payload, uint16_t payload_len);
    void prepare_same_route_retry();
    bool prepare_next_fuota_from_queue();
    void start_retry_context();
    void stop_retry_context();
    StepStat execute_silence_state_for_all_nodes(int state);
    //............................................................//

    int write_fuota_command_to_client_socket(unsigned char *cmd, size_t cmd_len);
    bool compute_router_path(unsigned char *target_mac, router_path_t *out_path);
    bool wait_for_socket_response(int timeout_secs);
    int execute_fuotasequence_to_silence_network();
    int process_rffuota_enable_sequence(Fuota *pan_list_ptr, unsigned char *ondemand_mac);
    int execute_fuotasequence_to_silencenetwork(); // not using to decide silenceing nodes 70% then only fuota update else drop out the request
    //......................Resume Fuota.............................//
    int resume_rf_fuota_pending_state_process(unsigned char *dcuid, unsigned char *target_mac, int request_id, const std::string &resume_filepath, std::string &filename);

    //.....................Fuota request progress updation................//
    int ondemand_fuota_update_status(int request_status, int error_code, int ondemand_request_id);

    //.................resume fuota...
    int get_resumefuota_for_requestedfirmware_filepath(unsigned char *target_node, const std::string &path, int reqid);
    void start_fuota_response_timer(int timeout_sec);
    uint16_t build_image_tf_expected_rx(uint8_t *rx_exp, uint16_t base_resp_hdr_len, uint16_t page, uint8_t subpage, bool last_subpage);

    //-----------------alternate path at image transfer
    void prepare_alternate_sameroute_retry(uint8_t *payload, uint16_t payload_len);
    bool build_alternateroute_image_transfer(unsigned char *target_mac, uint8_t router_count);
};

#endif // __FUOTA_H__