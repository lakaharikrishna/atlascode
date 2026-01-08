#ifndef __PULL_H__
#define __PULL_H__

#include "database.h"
#include "push.h"
#include "utility.h"

class Client;

#define HES_START_BYTE   0x2E
#define ATCMD_START_BYTE 0x2B

#define INVALID_SRC_ADDR      INVALID_RESPONSE
#define PING_COMMAND_RECEIVED 0x50
#define NEXT_PAGE_PRESENT     0x52
#define PUSH_DATA_RECEIVED    0x54

/* Mesh Tx Packet Types */
#define MESH_COMMISSION_PACKET        0x03
#define MESH_PARENT_SET               0x05
#define MESH_DATA_QUERY               0x07
#define MESH_DATA_RESPONSE            0x08
#define MESH_OTHER_PAN_NEIGHBOUR_SCAN 0x0B
#define MESH_SAME_PAN_NEIGHBOUR_SCAN  0x0C
#define MESH_PING_REQUEST             0x0D

/* Mesh Rx Packet Types */
#define MESH_COMMISSION_PACKET_RESPONSE        0x04
#define MESH_PARENT_SET_RESPONSE               0x06
#define MESH_SAME_PAN_NEIGHBOUR_SCAN_RESPONSE  0x0F
#define MESH_OTHER_PAN_NEIGHBOUR_SCAN_RESPONSE 0x10
#define MESH_PING_RESPONSE                     0x0E

struct RemoteNeighbourQueryCmd
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

class PullCommands
{
 public:
    std::vector<uint8_t> fuota_status = {0x00, 0x9B};
    std::vector<uint8_t> fuota_disable = {0x01, 0x9B, 0x00};
    std::vector<uint8_t> flash_write = {0x00, 0x02};
    std::vector<uint8_t> soft_exit = {0x00, 0x01};
    std::vector<uint8_t> dlms_connect = {0x2B, 0x07, 0x00, 0x00, 0x00, 0x02, 0x01, 0x35};
    std::vector<uint8_t> internal_firmware_version = {0x00, 0x49, 0x46};
};

struct MissingCycleInfo
{
    std::vector<int> missing_ip_cycles{};
    bool is_blp_available = false;
    bool is_dlp_available = false;
    bool is_bhp_available = false;
    bool is_name_plate_available = false;
    bool is_scalar_available = false;
    bool is_silenced = false;
    int verify_ifv_presence = false;
};

struct PathInfo
{
    int hop_count{};
    std::vector<uint8_t> path{};
};

struct NodeProfileData
{
    PacketBuffer<DlmsRecordMap> name_plate_profile{};
    PacketBuffer<DlmsRecordMap> scalar_profile{};
    uint8_t internal_firmware_version[256]{};
    PacketBuffer<DlmsRecordMap> instantaneous_profile{};
    PacketBufferBlockLoad block_load_profile{};
    PacketBuffer<DlmsRecordMap> daily_load_profile{};
    PacketBuffer<DlmsRecordMap> billing_history{};
};

struct NodeInfo
{
    std::array<uint8_t, 8> node_mac_address{};
    MissingCycleInfo missing_info{};
    PathInfo primary_path{};
    std::vector<PathInfo> alternate_paths{};
    NodeProfileData profile_data{};
};

struct GatewayDetails
{
    std::array<uint8_t, 8> serial_number{};
    std::array<uint8_t, 4> panid{};
};

class PullData : public virtual BaseLogger, public virtual MySqlDatabase, public PullCommands, public virtual PushData
{
 public:
    GatewayDetails gateway_details{};

    static std::array<uint8_t, 8> mac_from_hex(const char *hex);
    static void mac_to_hex(const std::array<uint8_t, 8> &mac, char *out16);
    static bool extract_path(const std::string &hex_path, int hop_count, std::vector<uint8_t> &out);

    uint8_t calculate_checksum(const uint8_t *buff, size_t length);
    std::array<uint8_t, 8> to_mac_array(const uint8_t mac[4]);

    int process_nameplate_data(NodeInfo *node, uint8_t *buff, ssize_t length);
    int process_ip_profile_data(NodeInfo *node, uint8_t *buff, ssize_t length);
    int process_daily_load_data(NodeInfo *node, uint8_t *buff, ssize_t length);
    int process_block_load_data(NodeInfo *node, uint8_t *buff, ssize_t length);
    int process_billing_history_data(NodeInfo *node, uint8_t *buff, ssize_t length);

    uint32_t SecondsFromBase(time_t t);
    void GetYesterdayRange(time_t &start, time_t &end);
    void GetPreviousHourRange(time_t &start, time_t &end);
};

#endif // __PULL_H__