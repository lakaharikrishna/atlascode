#include "../inc/mqtt.h"
#include "../inc/String_functions.h"
#include "../inc/client.h"

// Helper for deque lookup (O(n) but n<=2 → fast)
bool MQTTClient::contains(const std::deque<std::string> &dq, const std::string &id)
{
    return std::find(dq.begin(), dq.end(), id) != dq.end();
}

MQTTClient::MQTTClient()
{
    std::cout << "MQTT constructor called" << std::endl;
}

MQTTClient::~MQTTClient()
{
    std::cout << "MQTT destructor called" << std::endl;
    if (this->mosq)
    {
        mosquitto_disconnect(this->mosq);
        mosquitto_loop_stop(this->mosq, true);
        mosquitto_destroy(this->mosq);
        this->mosq = nullptr;
    }
}

void MQTTClient::set_mqtt_socket(int mqtt_socket)
{
    this->mqtt_socket = mqtt_socket;
}

int MQTTClient::get_mqtt_socket(void)
{
    return this->mqtt_socket;
}

void MQTTClient::set_mqtt_topic_and_client_id(const char *gateway_id)
{
    snprintf(this->MqttTopic, sizeof(this->MqttTopic), "%.*s/ONDEMAND_REQUEST", 16, gateway_id);
    snprintf(this->ClientID, sizeof(this->ClientID), "%.*s/CLIENT_ID", 16, gateway_id);

    this->print_and_log("Mqtt topic: %s\n", this->MqttTopic);
    this->print_and_log("Mqtt client ID: %s\n", this->ClientID);
}

void MQTTClient::load_mqtt_config_from_file(void)
{
    try
    {
        this->mqtt_host = Utility::readConfig<std::string>("MQTT.host");
        this->mqtt_port = Utility::readConfig<int>("MQTT.port");
    }

    catch (const std::exception &e)
    {
        this->print_and_log("Error: %s\n", e.what());
    }

    this->print_and_log("mqtt_port: %d\n", this->mqtt_port);
    this->print_and_log("mqtt_server_ip_address: %s\n", this->mqtt_host.c_str());
}

bool MQTTClient::connect(const char *host, int port, int keepalive)
{
    this->mosq = mosquitto_new(this->ClientID, true, this);

    mosquitto_reconnect_delay_set(this->mosq, 30, 60, false);
    mosquitto_connect_callback_set(this->mosq, on_connect);
    mosquitto_disconnect_callback_set(this->mosq, on_disconnect);
    mosquitto_message_callback_set(this->mosq, on_message);

    if (mosquitto_connect_async(this->mosq, host, port, keepalive) != MOSQ_ERR_SUCCESS)
    {
        this->print_and_log("Could not connect to broker.\n");
        return false;
    }

    mosquitto_loop_start(this->mosq);

    return true;
}

void MQTTClient::on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    MQTTClient *client = static_cast<MQTTClient *>(obj);
    if (!client)
        return;

    if (rc == 0)
    {
        mosquitto_subscribe(mosq, nullptr, client->MqttTopic, 2);
        // update dlms_mqtt_connection_log
        Client *clientobj = static_cast<Client *>(client);
        client->print_and_log("[✅ MQTT CONNECTED SUCCESSFULLY][%s]\n", clientobj->gateway_id);
        clientobj->update_dlms_mqtt_info(clientobj->gateway_id, 1);
    }
    else
    {
        client->print_and_log("MQTT Connection failed. Error code: %d (%s)", rc, mosquitto_strerror(rc));
    }
}

void MQTTClient::on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    MQTTClient *client = static_cast<MQTTClient *>(obj);
    if (!client)
        return;

    client->print_and_log("MQTT disconnected: rc = %d (%s)\n", rc, mosquitto_strerror(rc));

    if (rc != 0)
    {
        if (client->allow_reconnect == true)
        {
            client->print_and_log("Reconnecting MQTT...\n");
            mosquitto_reconnect_async(mosq);
        }
        else
        {
            mosquitto_loop_stop(client->mosq, true);
            client->print_and_log("MQTT Reconnect disabled duplicate gateway case.\n");
        }
    }
}

void MQTTClient::on_message(mosquitto *mosq, void *obj, const mosquitto_message *msg)
{
    (void)mosq; // Unused: Reserved for possible future use or compliance with callback signature

    // Safely cast and check client object passed by callback.
    MQTTClient *client = static_cast<MQTTClient *>(obj);
    if (!client)
        return;

    /* =======================
    Store payload safely
    ======================= */
    client->ondemand_data.clear();
    client->ondemand_data.resize(msg->payloadlen + 1); // +1 for '\0'

    memcpy(client->ondemand_data.data(), msg->payload, msg->payloadlen);
    client->ondemand_data[msg->payloadlen] = '\0'; // null-terminate

    // client->ondemand_data_len = msg->payloadlen;

    /* =======================
       Timestamp generation
       ======================= */
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    localtime_r(&time_t, &tm); // localtime_s(&time_t, &tm) on Windows
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "."
        << std::setfill('0') << std::setw(3) << ms;
    std::string time_str = oss.str();

    /* =======================
   Debug log
   ======================= */
    client->print_and_log("[%s] Received message on topic : %s : %s\n", time_str.c_str(), msg->topic, client->ondemand_data.data());

    /* =======================
    Validate request IDs
    ======================= */
    client->validate_request_ids(client->ondemand_data.data(), client->gatewayIdStr.c_str(), client);

    /* =======================
    Notify main loop
    ======================= */
    eventfd_write(client->mqtt_socket, 1);
}

// Enqueue successful On-Demand (ODM) command*+
void MQTTClient::enqueue_odm(const std::string &cmd)
{
    std::vector<uint8_t> cmd_bytes(cmd.begin(), cmd.end());
    if (ODM.size() >= MAX_QUEUE_SIZE)
        ODM.pop(); // Drop oldest if full
    ODM.emplace(std::move(cmd_bytes));
}

// Enqueue failed commands
void MQTTClient::enqueue_failed(const std::string &cmd)
{
    std::vector<uint8_t> cmd_bytes(cmd.begin(), cmd.end());
    if (Failed.size() >= MAX_QUEUE_SIZE)
        Failed.pop();
    Failed.emplace(std::move(cmd_bytes));
}

// Enqueue RF & Meter FUOTA commands
void MQTTClient::enqueue_fuota(const std::string &cmd)
{
    std::vector<uint8_t> cmd_bytes(cmd.begin(), cmd.end());
    if (RF_Meter_FUOTA.size() >= MAX_QUEUE_SIZE)
        RF_Meter_FUOTA.pop();
    RF_Meter_FUOTA.emplace(std::move(cmd_bytes));
}

void MQTTClient::remove_commands_by_req_id(std::queue<std::vector<uint8_t>> &queue,
                                           const std::unordered_set<std::string> &cancel_req_ids)
{
    // Temporary queue to hold commands that are not cancelled
    std::queue<std::vector<uint8_t>> temp_queue;

    // Process each command in the original queue
    while (!queue.empty())
    {
        // Reference to the front command bytes in the queue
        const std::vector<uint8_t> &cmd_bytes = queue.front();

        // Convert command bytes to a string for parsing
        std::string cmd_str(cmd_bytes.begin(), cmd_bytes.end());

        // Split the command string by ':' delimiter to extract parts
        std::vector<std::string> cmd_parts = explode(cmd_str, ':');

        // If command is empty or request ID is not in cancel list, keep the command
        if (cmd_parts.empty() || cancel_req_ids.find(cmd_parts[0]) == cancel_req_ids.end())
        {
            temp_queue.emplace(cmd_bytes);
        }
        else
        {
            // Log the cancellation for traceability with the request ID
            this->print_and_log("Cancelling queued command with Request ID: %s\n", cmd_parts[0].c_str());
        }

        // Remove the processed command from the original queue
        queue.pop();
    }

    // Replace the original queue with the filtered queue containing only non-cancelled commands
    queue = std::move(temp_queue);
}

// Validate request IDs from a message payload.
// IDs are hyphen-separated and that each ID should be 16 characters.
// Returns true if all IDs are valid, false otherwise.
void MQTTClient::validate_request_ids(const char *message, const char *gateway_id, MQTTClient *client)
{
    // Convert input message into std::string for easy manipulation.
    std::string msg_str(message);

    if (msg_str.find('-') != std::string::npos)
    {
        msg_str = msg_str.substr(msg_str.find(':') + 1); // remove group ID
        this->print_and_log("[WITHOUT GROUP ID] = %s\n", msg_str.c_str());
    }
    // Split multiple commands separated by hyphen '-'.
    std::vector<std::string> commands = explode(msg_str, '-');
    if (commands.empty())
    {
        client->print_and_log("❌ NO COMMANDS - EXIT\n");
        return;
    }

    // Allowed command codes, used to validate commands' type in parts[4].
    static const std::set<std::string> allowed_cmds{
        "00", "01", "02", "03", "04", "05", "06", "07", "08", "09",
        "10", "11", "12", "13", "14", "15", "16", "17", "18", "19",
        "20", "21", "22", "23", "24", "25", "26", "27", "28", "29", "30", "31"};

    // Process each command one by one
    for (const auto &cmd : commands)
    {
        // Divide command into parts delimited by ':'
        std::vector<std::string> parts = explode(cmd, ':');

        // Special Handling: Check if command is a CANCEL request
        if (!parts.empty() && parts[0] == "CANCEL")
        {
            client->print_and_log("✅ CANCEL CMD - SKIPPING VALIDATION\n");
            // Collect all request IDs to be cancelled
            std::unordered_set<std::string> cancel_req_ids(parts.begin() + 1, parts.end());

            // Remove matching commands from ODM and FUOTA queues
            remove_commands_by_req_id(client->ODM, cancel_req_ids);
            remove_commands_by_req_id(client->RF_Meter_FUOTA, cancel_req_ids);

            // Store cancelled request IDs for DB update in a thread-safe manner
            std::lock_guard<std::mutex> lock(client->cancelled_mutex);
            for (const auto &id : cancel_req_ids)
            {
                client->cancelled_requests.push(id);
            }
            client->print_and_log("CANCEL: Queued %zu request IDs for DB update\n", cancel_req_ids.size());
            // After cancellation, skip further validation for this command
            continue;
        }

        // Reject and enqueue as failed if command parts are empty
        if (parts.empty())
        {
            client->print_and_log("❌ EMPTY PARTS → FAILED QUEUE\n");
            client->enqueue_failed(cmd);
            continue;
        }

        // Accept only commands with 7 or 8 parts, else treat as invalid
        if (!(parts.size() == 6 || parts.size() == 7 || parts.size() == 8))
        {
            client->print_and_log("❌ INVALID PARTS COUNT (%zu) → FAILED QUEUE\n", parts.size());
            client->enqueue_failed(cmd);
            continue;
        }
        if (parts.size() == 6)
        {
            if (parts[4] == "13")
            {
                if (parts.size() != 8)
                {
                    client->print_and_log("❌ INVALID PARTS COUNT FOR PING NODE (%zu) → FAILED QUEUE\n", parts.size());
                    client->enqueue_failed(cmd);
                    continue;
                }
            }
        }
        client->print_and_log("[PARTS SIZE] = %d\n", parts.size() - 1);

        // Assume command is valid initially
        bool command_valid = true;

        // Validate each part against expected rules
        for (size_t idx = 0; idx < parts.size() && command_valid; ++idx)
        {
            switch (idx)
            {
                case REQUEST_ID: {
                    client->print_and_log("[idx] = %d, REQ_ID = %s\n", idx, parts[REQUEST_ID].c_str());

                    // LEVEL 1: Basic numeric validation
                    if (!std::all_of(parts[REQUEST_ID].begin(), parts[REQUEST_ID].end(), ::isdigit))
                    {
                        client->print_and_log("Enqueueing command to Failed queue (REQ_ID not numeric): %s\n", cmd.c_str());
                        command_valid = false;
                        break;
                    }

                    // LEVEL 2: GLOBAL DUPLICATE CHECK
                    {
                        std::lock_guard<std::mutex> lock(global_id_mutex);

                        // 🔥 DEBUG: Print BEFORE check
                        client->print_and_log("🔍 BEFORE CHECK - OTHER_REQ_IDS size=%zu: [", global_seen_req_ids.size());
                        for (const auto &id : global_seen_req_ids)
                            client->print_and_log("%s ", id.c_str());
                        client->print_and_log("]\n");

                        bool is_duplicate = false;
                        std::deque<std::string> *target_set = nullptr;

                        if (parts[DOWNLOAD_DATA_TYPE] == "13" || parts[DOWNLOAD_DATA_TYPE] == "14")
                        {
                            target_set = &special_req_ids;
                            is_duplicate = contains(*target_set, parts[REQUEST_ID]);
                        }
                        else if (parts[DOWNLOAD_DATA_TYPE] == "27")
                        {
                            target_set = &FUOTA_req_ids;
                            is_duplicate = contains(*target_set, parts[REQUEST_ID]);
                        }
                        else
                        {
                            target_set = &global_seen_req_ids;
                            is_duplicate = contains(*target_set, parts[REQUEST_ID]);
                        }
                        client->print_and_log("🔍 CHECK '%s' → %s\n", parts[REQUEST_ID].c_str(),
                                              is_duplicate ? "DUPLICATE (REJECT)" : "OK");

                        if (is_duplicate)
                        {
                            client->print_and_log("🚫 GLOBAL DUPLICATE REQ_ID '%s' - REJECTED!\n", parts[REQUEST_ID].c_str());
                            command_valid = false;
                            break;
                        }

                        // ✅ APPROVED - Add to END (newest)
                        target_set->push_back(parts[REQUEST_ID]);
                        client->print_and_log("✅ INSERTED '%s' → size=%zu\n", parts[REQUEST_ID].c_str(), target_set->size());

                        // Cleanup: Keep only LAST 2 (remove oldest from front)
                        while (target_set->size() > 2)
                        {
                            target_set->pop_front();
                            client->print_and_log("🧹 Removed OLDEST → size=%zu\n", target_set->size());
                        }

                        // 🔥 DEBUG: Print AFTER cleanup
                        client->print_and_log("🔍 DEBUG AFTER CLEANUP - OTHER_REQ_IDS: [");
                        for (const auto &id : global_seen_req_ids)
                            client->print_and_log("%s ", id.c_str());
                        client->print_and_log("]\n");
                    }
                    break;
                }

                case GATEWAY_ID:
                    client->print_and_log("[idx] = %d, GATEWAY_ID = %s\n", idx, parts[GATEWAY_ID].c_str());
                    // Payload ID must have length 16 and match GATEWAY ID
                    if (parts[GATEWAY_ID].length() != 16 || parts[GATEWAY_ID].substr(0, 16) != gateway_id)
                    {
                        client->print_and_log("Enqueueing command to Failed queue (Wrong Gateway ID): %s\n", cmd.c_str());
                        command_valid = false;
                    }
                    break;
                case HOP_COUNT:
                    client->print_and_log("[idx] = %d, HOP_COUNT = %s\n", idx, parts[HOP_COUNT].c_str());
                    break;
                case DEST_ADDR: {
                    client->print_and_log("[idx] = %d, DEST_ADDRESS = %s\n", idx, parts[DEST_ADDR].c_str());
                    // Hop count integer check and address validation
                    int hop_count;
                    try
                    {
                        hop_count = std::stoi(parts[HOP_COUNT]);
                    }
                    catch (...)
                    {
                        client->print_and_log("🚫 HOP COUNT INVALID | CMD='%s' | HOPS:'%s' → FAILED\n", cmd.c_str(), parts[HOP_COUNT].c_str());
                        command_valid = false;
                        break;
                    }
                    size_t expected_len = 16 * (hop_count + 1);

                    if (hop_count == 0)
                    {
                        if (parts[GATEWAY_ID] != parts[DEST_ADDR])
                        {
                            client->print_and_log("🚫 DEST PATH MISMATCH | CMD='%s' | "
                                                  "REQ:'%s' | GW:'%s' | DEST:'%s'(len=%zu) | "
                                                  "HOPS:0 → FAILED QUEUE\n",
                                                  cmd.c_str(),
                                                  parts[0].c_str(),               // Request ID ✓
                                                  parts[1].c_str(),               // Gateway ID ✓
                                                  parts[3].substr(0, 16).c_str(), // Dest preview ✓
                                                  parts[3].length());             // Dest length ✓
                            command_valid = false;
                        }
                    }
                    else
                    {
                        if ((parts[DEST_ADDR].length() != expected_len) || (parts[DEST_ADDR].substr(0, 16) != gateway_id))
                        {
                            client->print_and_log("🚫 DEST PATH INVALID | CMD='%s' | "
                                                  "REQ:'%s' | GW:'%s' | DEST:'%s'(len=%zu/%zu) | "
                                                  "HOPS:%d → FAILED QUEUE\n",
                                                  cmd.c_str(),
                                                  parts[0].c_str(),               // Request ID ✓
                                                  gateway_id,                     // Expected GW ✓
                                                  parts[3].substr(0, 16).c_str(), // Got GW preview ✓
                                                  parts[3].length(),              // Actual len ✓
                                                  expected_len,                   // Expected len ✓
                                                  hop_count);                     // Hops ✓
                            command_valid = false;
                        }
                    }
                    break;
                }

                case DOWNLOAD_DATA_TYPE:
                    client->print_and_log("[idx] = %d, DOWNLOAD_DATA_TYPE = %s\n", idx, parts[DOWNLOAD_DATA_TYPE].c_str());
                    // Data type must be one of allowed commands
                    if (allowed_cmds.find(parts[DOWNLOAD_DATA_TYPE]) == allowed_cmds.end())
                    {
                        client->print_and_log("🚫[INVALID DOWNLOAD DATA TYPE]: %s\n", parts[DOWNLOAD_DATA_TYPE].c_str());
                        command_valid = false;
                    }
                    break;

                case COMMAND:
                    // Only validate this part for commands which actually have 6 or 7 parts AND require validation here

                    client->print_and_log("[idx] = %d, COMMAND = %s\n", idx, parts[COMMAND].c_str());
                    if (Validate_command(parts) == false)
                    {
                        client->print_and_log("[🚫 INVALID COMMAND]: %s\n", cmd.c_str());
                        command_valid = false;
                    }
                    break;
                case PING_COUNT:

                    if (parts[DOWNLOAD_DATA_TYPE] == "13") // Example: Ping Node command
                    {
                        client->print_and_log("[idx] = %d, PING_COUNT = %s\n", idx, parts[PING_COUNT].c_str());
                        try
                        {
                            int ping_count = std::stoi(parts[PING_COUNT]);
                            if (ping_count < 1 || ping_count > 3)
                            {
                                client->print_and_log("Enqueueing command to Failed queue (Ping count out of range): %s\n", cmd.c_str());
                                command_valid = false;
                            }
                        }
                        catch (...)
                        {
                            client->print_and_log("Enqueueing command to Failed queue (Invalid ping count): %s\n", cmd.c_str());
                            command_valid = false;
                        }
                    }
                    else
                    {
                        client->print_and_log("[idx] = %d, PING_COUNT = %s\n", idx, parts[PING_COUNT].c_str());
                        break;
                    }
                    break;
                case PING_INTERVAL:
                    // Only validate if parts.size() > 6

                    if (parts[DOWNLOAD_DATA_TYPE] == "13") // Ping Node command
                    {
                        client->print_and_log("[idx] = %d, PING_INTERVAL = %s\n", idx, parts[PING_INTERVAL].c_str());
                        try
                        {
                            int ping_interval = std::stoi(parts[PING_INTERVAL]);
                            if (ping_interval < 1 || ping_interval > 3)
                            {
                                client->print_and_log("Enqueueing command to Failed queue (Ping interval out of range): %s\n", cmd.c_str());
                                command_valid = false;
                            }
                        }
                        catch (...)
                        {
                            client->print_and_log("Enqueueing command to Failed queue (Invalid ping interval): %s\n", cmd.c_str());
                            command_valid = false;
                        }
                    }
                    else
                    {
                        break;
                        // Other command-specific validations for parts[6]
                    }
                    break;

                default:
                    client->print_and_log("[idx] = %d\n", idx);
                    client->print_and_log("wrong command format\n");
                    // command_valid = false;
                    break;
            }

            if (!command_valid)
                break; // Exit validation on first failure
        }
        // Log the received message with timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
        std::tm tm{};
        localtime_r(&time_t, &tm); // localtime_s(&time_t, &tm) on Windows
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "."
            << std::setfill('0') << std::setw(3) << ms;
        std::string time_str = oss.str();
        // Enqueue and signal the appropriate queue based on the final validation status and command type
        if (command_valid)
        {
            if (parts[DOWNLOAD_DATA_TYPE] == "27" || parts[DOWNLOAD_DATA_TYPE] == "28") // 27 is RF FUOTA, 28 is Meter FUOTA
            {
                client->print_and_log("[%s][Enqueueing command to FUOTA queue] (success): %s\n", time_str.c_str(), cmd.c_str());
                client->enqueue_fuota(cmd); // FUOTA Queue
            }
            else
            {
                client->print_and_log("[%s][Enqueueing command to ODM queue] (success): %s\n", time_str.c_str(), cmd.c_str());
                client->enqueue_odm(cmd); // ODM Queue
            }
        }
        else
        {
            client->print_and_log("[%s][Enqueueing command to Failed queue] (invalid): %s\n", time_str.c_str(), cmd.c_str());
            client->enqueue_failed(cmd); // Failed Queue
        }
    }
    client->ODM_Flag = 1;
    this->print_and_log("[ODM Flag] =%d\n", client->ODM_Flag);
}

bool MQTTClient::hex_to_bytes(const std::string &hex, std::vector<uint8_t> &out)
{
    if (hex.size() % 2 != 0)
        return false;

    out.clear();
    out.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2)
    {
        const std::string byteStr = hex.substr(i, 2);
        uint8_t b = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16)); // base 16
        out.push_back(b);
    }
    return true;
}

bool MQTTClient::Validate_command(const std::vector<std::string> &parts)
{
    uint8_t download_data_type = static_cast<uint8_t>(std::stoi(parts[DOWNLOAD_DATA_TYPE]));
    // 2) Get command hex string from parts[5]
    const std::string &cmd_hex = parts[COMMAND];

    std::vector<uint8_t> cmd_bytes;
    if (!hex_to_bytes(cmd_hex, cmd_bytes))
    {
        print_and_log("❌ Invalid hex in command: %s\n", cmd_hex.c_str());
        return false;
    }

    // Need at least 8 bytes
    if (cmd_bytes.size() < 8)
    {
        print_and_log("❌ Command too short: len=%zu (need >=8)\n", cmd_bytes.size());
        return false;
    }
    uint8_t command = cmd_bytes[4];
    uint8_t sub_command = cmd_bytes[5];
    uint8_t packet_type = cmd_bytes[2];

    switch (download_data_type)
    {
        case DATA_TYPE_NP:
        case DATA_TYPE_IP:
        case DATA_TYPE_BHP:
        case DATA_TYPE_DLP:
        case DATA_TYPE_BLP: { // Read Scalar List
            if (command != download_data_type)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, TYPE=%02X\n", command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_ALL_EVENTS: {
            if (command != 0x08 && sub_command != 0x00)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_VOLTAGE_EVENTS: {
            if (command != 0x08 && sub_command != 0x01)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_CURRENT_EVENTS: {
            if (command != 0x08 && sub_command != 0x02)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_POWER_EVENTS: {
            if (command != 0x08 && sub_command != 0x03)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_TRANSACTIONAL_EVENTS: {
            if (command != 0x08 && sub_command != 0x04)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_OTHER_EVENTS: {
            if (command != 0x08 && sub_command != 0x05)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_NON_ROLL_OVER_EVENTS: {
            if (command != 0x08 && sub_command != 0x06)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_CONTROL_EVENTS: {
            if (command != 0x08 && sub_command != 0x07)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_PING_NODE: {
            if (packet_type != 0x0D)
            {
                print_and_log("❌ [PACKET TYPE NOT MATCHING DOWNLOAD DATA TYPE] PACKET_TYPE=%02X, TYPE=%02X\n", packet_type, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_RTC_READ: {
            if (command != 0x00 && sub_command != 0x02)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_RTC_WRITE: {
            if (command != 0x01 && sub_command != 0x02)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ:
        case DATA_TYPE_CAPTURE_PERIOD_READ: {
            if (command != 0x00 && sub_command != 0x01)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ_WRITE:
        case DATA_TYPE_CAPTURE_PERIOD_READ_WRITE: {
            if (command != 0x01 && sub_command != 0x01)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_LOAD_LIMIT_READ: {
            if (command != 0x00 && sub_command != 0x08)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_LOAD_LIMIT_WRITE: {
            if (command != 0x01 && sub_command != 0x08)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_LOAD_STATUS_READ:
        case DATA_TYPE_PING_METER: {
            if (command != 0x00 && sub_command != 0x09)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_LOAD_STATUS_WRITE: {
            if (command != 0x01 && sub_command != 0x09)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        case DATA_TYPE_ACTION_SCHEDULER_READ: {
            if (command != 0x01 && sub_command != 0x07)
            {
                print_and_log("❌ [COMMAND NOT MATCHING DOWNLOAD DATA TYPE] CMD=%02X, SUBCMD=%02X, TYPE=%02X\n", command, sub_command, download_data_type);
                return false;
            }
            return true;
        }
        default:
            print_and_log("❌ Unknown download data type: %02X\n", download_data_type);
            return false;
    }
}

bool MQTTClient::check_rf_fuota_queue_empty()
{
    return RF_Meter_FUOTA.empty();
}
// Dequeue FUOTA command
bool MQTTClient::dequeue_fuota(std::vector<uint8_t> &out_cmd)
{
    this->print_and_log("(%s) -> %s Start, Dequeuing FUOTA command\n", this->dcuIdStr.c_str(), __FUNCTION__);
    std::lock_guard<std::mutex> lock(fuota_queue_mtx);

    if (RF_Meter_FUOTA.empty())
    {
        this->print_and_log("Empty queue\n");
        return false;
    }

    out_cmd = std::move(RF_Meter_FUOTA.front());

    RF_Meter_FUOTA.pop();
    return true;
}

void MQTTClient::resume_fuota(const std::string &cmd)
{
    this->print_and_log("(%s) -> resume_fuota called, enqueueing pending FUOTA (len=%zu)\n", this->dcuIdStr.c_str(), cmd.size());
    // std::cout << "Enqueueing FUOTA command: " << cmd << std::endl;
    std::lock_guard<std::mutex> lock(fuota_queue_mtx);

    std::vector<uint8_t> cmd_bytes(cmd.begin(), cmd.end());

    // Maintain max queue capacity (drop oldest)
    if (RFMeterFUOTA.size() >= MAX_QUEUE_SIZE)
    {
        this->print_and_log("Queue is full\n");
        RFMeterFUOTA.pop();
    }

    RFMeterFUOTA.emplace(std::move(cmd_bytes));
}

bool MQTTClient::dequeue_pending_fuota(std::vector<uint8_t> &out_cmd)
{
    this->print_and_log("(%s) -> %s Start\n", this->dcuIdStr.c_str(), __FUNCTION__);
    std::lock_guard<std::mutex> lock(fuota_queue_mtx);

    if (RFMeterFUOTA.empty())
    {
        this->print_and_log("Empty queue\n");
        return false;
    }

    out_cmd = std::move(RFMeterFUOTA.front());
    this->print_and_log("(%s) -> dequeue_pending_fuota: dequeued command len=%zu\n", this->dcuIdStr.c_str(), out_cmd.size());

    RFMeterFUOTA.pop();
    return true;
}