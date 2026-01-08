#ifndef __MQTT_H__
#define __MQTT_H__

#include <mosquitto.h>
#include <sys/eventfd.h>

#include <cstdint>
#include <deque>
#include <iostream>
#include <queue>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "utility.h"

#define REQUEST_ID         0
#define GATEWAY_ID         1
#define HOP_COUNT          2
#define DEST_ADDR          3
#define DOWNLOAD_DATA_TYPE 4
#define COMMAND            5
#define PING_COUNT         6
#define PING_INTERVAL      7

#define FIRMWARE_PATH      5
#define FIRMWARE_FILE_NAME 6
/*
------------------------
Class Description:
------------------------
MQTTClient encapsulates all logic for MQTT messaging in an embedded or desktop application context.
It implements message validation compatible with your protocol's command format, handles dynamic topic/client ID assignment, manages connection state, supports config reload, and offers thread-safety primitives for concurrency.
Use 'validate_command_format()' to check incoming MQTT message payloads against your command pattern before further processing.
The class provides logging and error reporting facilities through BaseLogger for debugging and production usage.
For multi-threaded scenarios, use the std::mutex as shown for thread-safe method implementations.
*/
class MQTTClient : public virtual BaseLogger
{
 private:
    mosquitto *mosq = nullptr;
    int mqtt_socket;
    char MqttTopic[64] = {0};
    char ClientID[64] = {0};

    //(added by hari)
    // External ODM related members
    int failed_flag;
    int failed_fd;
    std::string g_previous_req_id;
    std::string allowed_cmds;
    std::string g_previous_group_id;
    std::mutex fuota_queue_mtx;

    //(added by hari ends here)
 protected:
    bool allow_reconnect = true;
    std::string mqtt_host;
    int mqtt_port = 1883;
    std::vector<char> ondemand_data;
    // uint16_t ondemand_data_len{};

 public:
    // Added: string version of gateway_id
    std::string gatewayIdStr; //(added by Amith KN)
    uint8_t ODM_Flag = 0;     // to identify ODM in progress

    // Queues for each processing type
    std::queue<std::vector<uint8_t>> ODM;            // On-demand commands
    std::queue<std::vector<uint8_t>> Failed;         // Failed commands
    std::queue<std::vector<uint8_t>> RF_Meter_FUOTA; // FUOTA commands
    std::queue<std::vector<uint8_t>> RFMeterFUOTA;
    std::queue<std::string> cancelled_requests; // Cancelled request IDs
    std::mutex cancelled_mutex;                 // Mutex for thread-safe access to cancelled_requests

    static const size_t MAX_QUEUE_SIZE = 500;
    MQTTClient();
    ~MQTTClient();

    void set_mqtt_socket(int mqtt_socket);
    int get_mqtt_socket(void);
    void set_mqtt_topic_and_client_id(const char *gateway_id);
    void load_mqtt_config_from_file(void);
    bool connect(const char *host, int port, int keepalive);
    static void on_message(mosquitto *mosq, void *obj, const mosquitto_message *msg);
    static void on_connect(struct mosquitto *mosq, void *obj, int rc);
    static void on_disconnect(struct mosquitto *mosq, void *obj, int rc);
    //(added by Amith KN)
    void enqueue_odm(const std::string &cmd);
    void enqueue_failed(const std::string &cmd);
    void enqueue_fuota(const std::string &cmd);
    void validate_request_ids(const char *message, const char *gateway_id, MQTTClient *client);
    void remove_commands_by_req_id(std::queue<std::vector<uint8_t>> &queue,
                                   const std::unordered_set<std::string> &cancel_req_ids);
    bool hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out);
    bool Validate_command(const std::vector<std::string> &parts);
    bool contains(const std::deque<std::string> &dq, const std::string &id);

    //(added by Hari)
    bool check_rf_fuota_queue_empty();
    bool dequeue_fuota(std::vector<uint8_t> &out_cmd);
    void resume_fuota(const std::string &cmd);
    bool dequeue_pending_fuota(std::vector<uint8_t> &out_cmd);
    // ==================Additional methods can be declared here==================//
    int request_id = 0;
    int hop_count = 0;
    int Download_type = 0;
    std::string firmware_filename;
    std::string firmware_path;
    std::string dcuIdStr;
    std::mutex global_id_mutex;
    std::deque<std::string> global_seen_req_ids;
    std::deque<std::string> special_req_ids; // For download types 13,14
    std::deque<std::string> FUOTA_req_ids;   // For download type 27: FUOTA
};

#endif // __MQTT_H__