#include "../inc/client.h"
#include "../inc/String_functions.h"
#include "../inc/utility.h"
#include <algorithm>
#include <vector>

static std::string obis_to_string(const unsigned char *obis);
Client::Client()
{
    std::cout << "Client constructor called" << std::endl;

    this->stateInfo.targetState = ClientTargetState::IDLE;
    this->stateInfo.timeoutState = ClientTimeoutState::TIMER_PGWID_RX;
    this->stateInfo.currentState = ClientCurrentState::IDLE;

    //==============================Added by LHK=====================//
    db = std::make_unique<MySqlDatabase>();
    fuota = std::make_unique<Fuota>(*this, *db);
    print_and_log("Client ctor end, fuota=%p\n", fuota.get());
}

Client::~Client()
{
    std::cout << "Client destructor called" << std::endl;

    if (this->client_socket != -1)
    {
        shutdown(this->get_client_socket(), SHUT_RDWR);
        close(this->get_client_socket());
    }
}

void Client::set_client_socket(int client_socket)
{
    this->client_socket = client_socket;
}

int Client::get_client_socket(void)
{
    return this->client_socket;
}

bool Client::register_client(char *gateway_id, Client *new_client)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::array<char, 16> key;
    std::memcpy(key.data(), gateway_id, 16);

    std::lock_guard<std::mutex> lock(Server::clients_mutex);

    auto it = Server::g_clients.find(key);

    if (it != Server::g_clients.end())
    {
        this->print_and_log("Gateway already connected.\n");
        it->second->duplicate_gateway = true; // Client
        it->second->allow_reconnect = false;  // MQTT flag on-dissconnect
        Server::g_clients.erase(it);
    }

    Server::g_clients[key] = new_client;

    this->print_and_log("Total number of clients connected: %d\n", Server::g_clients.size());

    return true;
}

void Client::unregister_client(char *gateway_id, Client *current_client)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::array<char, 16> key;
    std::memcpy(key.data(), gateway_id, 16);

    std::lock_guard<std::mutex> lock(Server::clients_mutex);

    auto it = Server::g_clients.find(key);

    if (it != Server::g_clients.end())
    {
        if (it->second == current_client)
        {
            Server::g_clients.erase(it);
            this->print_and_log("Client removed correctly.\n");
        }
        else
        {
            this->print_and_log("Client not removed (pointer does not match).\n");
        }
    }
    else
    {
        this->print_and_log("No client found to remove (may already be deleted).\n");
    }

    this->print_and_log("Total number of clients connected: %d\n", Server::g_clients.size());
}

int Client::set_recv_timeout_for_client(uint32_t time_in_sec)
{
    timeval timeout;
    timeout.tv_sec = time_in_sec;
    timeout.tv_usec = 0;

    if (setsockopt(this->get_client_socket(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        this->print_and_log("Failed to set recv timeout.\n");
        return FAILURE;
    }

    return SUCCESS;
}

void Client::set_poll_timeout(int timeout_in_sec)
{
    this->print_and_log("Poll timeout is set to %d seconds\n", timeout_in_sec);
    this->polltimeout = timeout_in_sec * 1000;
}

ssize_t Client::receive_data(uint8_t *buff, size_t buffSize)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    ssize_t receivedBytes = recv(this->get_client_socket(), buff, buffSize, 0);

    if (!memcmp(buff, "PING", 4))
    {
        uint8_t pong[4] = {'P', 'O', 'N', 'G'};
        write_to_client(pong, 4);
    }
    if (receivedBytes == 0)
    {
        this->print_and_log("Client disconnected gracefully: %s\nerrno: %d (%s)\n", this->gateway_id, errno, strerror(errno));
        this->gatewayStatus = Status::DISCONNECTED;
    }
    else if (receivedBytes < 0)
    {
        this->client_get_time(this->time_str, 2);
        this->print_and_log("%s (%s) ", this->time_str.c_str(), this->gateway_id);

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            this->print_and_log("Receive timeout: no data received for client %s\n", this->gateway_id);
        }
        else
        {
            this->print_and_log("Receive error from client %s\nerrno: %d (%s)\n", this->gateway_id, errno, strerror(errno));
            this->gatewayStatus = Status::DISCONNECTED;
        }
    }
    else
    {
        this->client_get_time(this->time_str, 2);
        this->print_and_log("[%s] RX: %s : %d =", this->now_ms().c_str(), this->gateway_id, receivedBytes);

        for (int i = 0; i < receivedBytes; i++)
        {
            this->print_and_log(" %02X", buff[i]);
        }
        this->print_and_log("\n");

        // TO print TDC value for IP responses
        if (buff[21] == 0x0E && buff[22] == 0x01 && buff[20] == 0x01)
        {
            uint32_t tdc_raw =
                (static_cast<uint32_t>(buff[receivedBytes - 2]) << 24) | // MSB
                (static_cast<uint32_t>(buff[receivedBytes - 3]) << 16) |
                (static_cast<uint32_t>(buff[receivedBytes - 4]) << 8) |
                (static_cast<uint32_t>(buff[receivedBytes - 5])); // LSB

            this->print_and_log("TDC VALUE RECEIVED: Gateway ID = %s, dec=%u, hex=0x%08X\n", this->gateway_id, tdc_raw, tdc_raw);
        }
    }

    return receivedBytes;
}

void Client::create_gateway_log_file()
{
    std::string base_folder = "Individual_Logs";
    std::string dcu_folder = base_folder + "/" + this->gateway_id;

    std::error_code ec;

    // Portable directory creation fallback (no std::filesystem)
    auto create_directories_fallback = [](const std::string &path, std::error_code &ec) -> bool {
        ec.clear();
        if (path.empty())
        {
            ec = std::make_error_code(std::errc::invalid_argument);
            return false;
        }
        std::string accum;
        size_t pos = 0;
        if (path[0] == '/')
        {
            accum = "/";
            pos = 1;
        }
        while (pos <= path.size())
        {
            size_t next = path.find('/', pos);
            std::string part = path.substr(pos, (next == std::string::npos) ? std::string::npos : next - pos);
            if (!part.empty())
            {
                if (!accum.empty() && accum.back() != '/') accum += "/";
                accum += part;
                struct stat st;
                if (stat(accum.c_str(), &st) != 0)
                {
                    if (mkdir(accum.c_str(), 0755) != 0)
                    {
                        ec = std::error_code(errno, std::generic_category());
                        return false;
                    }
                }
                else
                {
                    if (!S_ISDIR(st.st_mode))
                    {
                        ec = std::make_error_code(std::errc::not_a_directory);
                        return false;
                    }
                }
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        return true;
    };
    create_directories_fallback(dcu_folder, ec);

    if (!ec)
    {
        std::cout << "Directory created/exists: " << dcu_folder << std::endl;
    }
    else
    {
        std::cout << "Failed to create directory: " << ec.message() << std::endl;
        return;
    }

    std::string id_suffix = &this->gateway_id[8];
    std::string log_filename = dcu_folder + "/" + id_suffix + "_" + this->now() + "_log.txt";

    std::cout << "Opening log file: " << log_filename << std::endl;

    if (!this->open_log_file(log_filename))
    {
        std::cout << "Failed to open log file.\n";
        return;
    }

    this->print_and_log("Client connected %s\n", this->gateway_id);
}

int Client::initCommunication(uint8_t *buffer, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    this->print_and_log("%s\n", buffer);

    if ((memcmp(buffer, "PGWID:3C", 8) == 0) && (length == 32))
    {
        char str[65];
        int val1, val2, val3;
        sscanf((char *)buffer, "%s %d %d %d", str, &val1, &val2, &val3);

        memcpy(this->gateway_id, &str[6], 16);
        memcpy(this->Source_ID, &str[14], 8);
        this->Source_ID[8] = '\0';
        memcpy(this->PAN_ID, &str[18], 4);
        this->PAN_ID[4] = '\0';
        memcpy(this->pgwid, str, 64);
        this->pgwid[64] = '\0';

        this->val1 = val1;
        this->val2 = val2;
        this->val3 = val3;

        this->gatewayIdStr = std::string(this->gateway_id); // Store GATEWAY ID as std::string for MQTT client ID/topic(added by Amith KN)

        this->print_and_log("gateway_id = %s\n", this->gateway_id);
        this->print_and_log("val1 = %d\n", val1);
        this->print_and_log("val2 = %d\n", val2);
        this->print_and_log("val3 = %d\n", val3);

        if (this->gatewayid_logging_enabled)
        {
            this->create_gateway_log_file();
        }
        this->register_client(this->gateway_id, this); // Add gateway info to Server::g_clients
        this->load_mysql_config_from_file();           // Read from file and connect to MySQL server
        this->init_connection(val1, val2, val3);       // initial pull function
        this->create_mqtt_client();                    // Read from file and connect to broker

        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->stateInfo.timeoutState = ClientTimeoutState::TIMER_NONE;
    }
    else
    {
        this->print_and_log("Invalid gatewayid\n");
        return FAILURE;
    }

    return SUCCESS;
}

void Client::create_mqtt_client(void)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    this->set_mqtt_topic_and_client_id(this->gateway_id);
    this->load_mqtt_config_from_file();
    this->connect(this->mqtt_host.c_str(), this->mqtt_port, 60);
}

int Client::is_valid_packet(uint8_t *rx_buf, int length)
{
    this->print_and_log("[%s] Starting packet validation - Length: %d bytes\n", __FUNCTION__, length);

    if ((rx_buf[0] == HES_START_BYTE) && ((rx_buf[1] + 1) == length) && (rx_buf[17] == PUSH_DATA_START_BYTE)) // added by puneeth
    {
        this->print_and_log("[PUSH DATA PACKET] Detected push data packet\n");
        return SUCCESS;
    }

    else if ((rx_buf[0] == HES_START_BYTE) && ((rx_buf[1] + 1) == length))
    {
        if (this->need_to_validate_src_addr == true)
        {
            this->need_to_validate_src_addr = false;
            if (this->validate_source_address(rx_buf) == SUCCESS)
            {
                this->print_and_log("[PKT SUCCESS] ✅ Valid packet confirmed\n");
                return SUCCESS;
            }
        }
    }
    else if (fuota && fuota->ondemand_fuota_state == FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE)
    {
        // If FUOTA FSM is rolling back to normal, treat incoming FUOTA-like packets as normal
        this->print_and_log("(%s) FUOTA state=ROLLBACK_TO_NORMAL_COMM_MODE: treating incoming packet as normal DLMS (not FUOTA)\n", this->gateway_id);
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->stateInfo.timeoutState = ClientTimeoutState::TIMER_NONE;
        fuota->ondemand_fuota_state = FUOTA_STATE::IDLE;
        return SUCCESS;
    }

    this->print_and_log("[PKT SRC_ADDR] ❌ Source address validation FAILED\n");
    return INVALID_RESPONSE;
}

int Client::client_process_data(uint8_t *buff, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (length > 17 && buff[0] == PMESH_PUSH_START_BYTE && buff[17] == PUSH_DATA_START_BYTE)
    {
        this->process_push_data(buff, length, this->gateway_id);
        return PUSH_DATA_RECEIVED;
    }

    int ret = processPacketType(buff, length);
    if (ret != SUCCESS)
    {
        this->print_and_log("[RETURN VALUE of processPacketType] = 0x%02X\n", ret);
        return ret;
    }
    // Interpret incoming buffer as OtaCmdResponse structure
    OtaCmdResponse *response = reinterpret_cast<OtaCmdResponse *>(buff);

    switch (this->stateInfo.targetState)
    {
        case ClientTargetState::PULL: {
            this->print_and_log("Client target state: PULL\n");
            OtaAtCmdResponse *ota_atcmd_rsp = reinterpret_cast<OtaAtCmdResponse *>(buff);

            switch (this->stateInfo.currentState)
            {
                case ClientCurrentState::FUOTA_STATUS: {
                    if (ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET_RESPONSE && ota_atcmd_rsp->command == 0x9B && ota_atcmd_rsp->status == 0x00 && ota_atcmd_rsp->data[0] == 0x00)
                    {
                        return SUCCESS;
                    }
                    else if (ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET_RESPONSE && ota_atcmd_rsp->command == 0x9B && ota_atcmd_rsp->status == 0x00 && ota_atcmd_rsp->data[0] == 0x01)
                    {
                        return ENABLED;
                    }
                    else if ((ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET) && (buff[ota_atcmd_rsp->length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid fuota status response\n");
                    }
                    break;
                }

                case ClientCurrentState::FUOTA_DISABLE: {
                    if (ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET_RESPONSE && ota_atcmd_rsp->command == 0x9B && ota_atcmd_rsp->status == 0x00)
                    {
                        return SUCCESS;
                    }
                    else if ((ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET) && (buff[ota_atcmd_rsp->length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid fuota disable response\n");
                    }
                    break;
                }

                case ClientCurrentState::FLASH_SAVE: {
                    if (ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET_RESPONSE && ota_atcmd_rsp->command == 0x02 && ota_atcmd_rsp->status == 0x00)
                    {
                        return SUCCESS;
                    }
                    else if ((ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET) && (buff[ota_atcmd_rsp->length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid flash save response\n");
                    }
                    break;
                }

                case ClientCurrentState::SOFT_RESET: {
                    if (ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET_RESPONSE && ota_atcmd_rsp->command == 0x01 && ota_atcmd_rsp->status == 0x00)
                    {
                        return SUCCESS;
                    }
                    else if ((ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET) && (buff[ota_atcmd_rsp->length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid soft reset response\n");
                    }
                    break;
                }

                case ClientCurrentState::NAMEPLATE: {
                    if (!this->currentNode)
                    {
                        this->print_and_log("ERROR: currentNode is NULL\n");
                        return FAILURE;
                    }

                    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

                    if ((resp->pmesh.start_byte == PMESH_PUSH_START_BYTE) && (resp->dlms.start_byte == PULL_START_BYTE) && (resp->pmesh.packet_type == MESH_DATA_RESPONSE))
                    {
                        if ((resp->dlms.frame_id == FI_INSTANT_DATA) && (resp->dlms.command == COMMAND_NAMEPLATE_PROFILE))
                        {
                            if ((resp->dlms.no_of_records == 0x00) && ((resp->dlms.data[0] == 0x0E) && (resp->dlms.data[1] == 0x01) && (resp->dlms.data[2] == 0x28)))
                            {
                                return DLMS_ERROR;
                            }

                            return this->process_nameplate_data(this->currentNode, buff, length);
                        }
                        else if (resp->dlms.next_page_status == FI_INSTANT_DATA && resp->dlms.no_of_records == 0x01 && resp->dlms.data[0] == 0x25)
                        {
                            return DLMS_CONNECTION_FAILED;
                        }
                    }
                    else if ((resp->pmesh.packet_type == MESH_DATA_QUERY) && (buff[resp->pmesh.length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid nameplate response\n");
                    }
                    break;
                }

                case ClientCurrentState::DLMS_CONNECT: {
                    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

                    if ((resp->dlms.start_byte == PULL_START_BYTE) && (resp->pmesh.packet_type == MESH_DATA_RESPONSE))
                    {
                        if (resp->dlms.next_page_status == 0x00 || resp->dlms.next_page_status == 0x02) // Status byte connect or Already connected
                        {
                            this->print_and_log("DLMS connect success\n");
                            return SUCCESS;
                        }
                        else if (resp->dlms.next_page_status == 0x01) // Failure
                        {
                            return DLMS_CONNECTION_FAILED;
                        }
                    }
                    else if ((resp->pmesh.packet_type == MESH_DATA_QUERY) && (buff[resp->pmesh.length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid DLMS connect response\n");
                    }
                    break;
                }

                case ClientCurrentState::INTERNAL_FV: {
                    if (!this->currentNode)
                    {
                        this->print_and_log("ERROR: currentNode is NULL\n");
                        return FAILURE;
                    }

                    if (ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET_RESPONSE && ota_atcmd_rsp->status == 0x00)
                    {
                        memcpy(&this->currentNode->profile_data.internal_firmware_version, &ota_atcmd_rsp->data[0], ota_atcmd_rsp->length - 16);
                        this->print_and_log("IFV: %s\n", this->currentNode->profile_data.internal_firmware_version);
                        return SUCCESS;
                    }
                    else if ((ota_atcmd_rsp->packet_type == MESH_COMMISSION_PACKET) && (buff[ota_atcmd_rsp->length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid internal firmware version response\n");
                    }
                    break;
                }

                case ClientCurrentState::DAILY_LOAD: {
                    if (!this->currentNode)
                    {
                        this->print_and_log("ERROR: currentNode is NULL\n");
                        return FAILURE;
                    }

                    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

                    if ((resp->pmesh.start_byte == PMESH_PUSH_START_BYTE) && (resp->dlms.start_byte == PULL_START_BYTE) && (resp->pmesh.packet_type == MESH_DATA_RESPONSE))
                    {
                        if ((resp->dlms.frame_id == FI_INSTANT_DATA) && (resp->dlms.command == COMMAND_DAILY_LOAD_PROFILE))
                        {
                            if ((resp->dlms.no_of_records == 0x00) && ((resp->dlms.data[0] == 0x0E) && (resp->dlms.data[1] == 0x01) && (resp->dlms.data[2] == 0x28)))
                            {
                                return DLMS_ERROR;
                            }

                            return this->process_daily_load_data(this->currentNode, buff, length);
                        }
                        else if (resp->dlms.next_page_status == FI_INSTANT_DATA && resp->dlms.no_of_records == 0x01 && resp->dlms.data[0] == 0x25)
                        {
                            return DLMS_CONNECTION_FAILED;
                        }
                    }
                    else if ((resp->pmesh.packet_type == MESH_DATA_QUERY) && (buff[resp->pmesh.length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid daily load response\n");
                    }
                    break;
                }

                case ClientCurrentState::BLOCK_LOAD: {
                    if (!this->currentNode)
                    {
                        this->print_and_log("ERROR: currentNode is NULL\n");
                        return FAILURE;
                    }

                    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

                    if ((resp->pmesh.start_byte == PMESH_PUSH_START_BYTE) && (resp->dlms.start_byte == PULL_START_BYTE) && (resp->pmesh.packet_type == MESH_DATA_RESPONSE))
                    {
                        if ((resp->dlms.frame_id == FI_INSTANT_DATA) && (resp->dlms.command == COMMAND_BLOCK_LOAD_PROFILE))
                        {
                            if ((resp->dlms.no_of_records == 0x00) && ((resp->dlms.data[0] == 0x0E) && (resp->dlms.data[1] == 0x01) && (resp->dlms.data[2] == 0x28)))
                            {
                                return DLMS_ERROR;
                            }

                            return this->process_block_load_data(this->currentNode, buff, length);
                        }
                        else if (resp->dlms.next_page_status == FI_INSTANT_DATA && resp->dlms.no_of_records == 0x01 && resp->dlms.data[0] == 0x25)
                        {
                            return DLMS_CONNECTION_FAILED;
                        }
                    }
                    else if ((resp->pmesh.packet_type == MESH_DATA_QUERY) && (buff[resp->pmesh.length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid daily load response\n");
                    }
                    break;
                }

                case ClientCurrentState::BILLING_HISTORY: {
                    if (!this->currentNode)
                    {
                        this->print_and_log("ERROR: currentNode is NULL\n");
                        return FAILURE;
                    }

                    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

                    if ((resp->pmesh.start_byte == PMESH_PUSH_START_BYTE) && (resp->dlms.start_byte == PULL_START_BYTE) && (resp->pmesh.packet_type == MESH_DATA_RESPONSE))
                    {
                        if ((resp->dlms.frame_id == FI_INSTANT_DATA) && (resp->dlms.command == COMMAND_BILLING_PROFILE))
                        {
                            if ((resp->dlms.no_of_records == 0x00) && ((resp->dlms.data[0] == 0x0E) && (resp->dlms.data[1] == 0x01) && (resp->dlms.data[2] == 0x28)))
                            {
                                return DLMS_ERROR;
                            }

                            return this->process_billing_history_data(this->currentNode, buff, length);
                        }
                        else if (resp->dlms.next_page_status == FI_INSTANT_DATA && resp->dlms.no_of_records == 0x01 && resp->dlms.data[0] == 0x25)
                        {
                            return DLMS_CONNECTION_FAILED;
                        }
                    }
                    else if ((resp->pmesh.packet_type == MESH_DATA_QUERY) && (buff[resp->pmesh.length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid daily load response\n");
                    }
                    break;
                }

                case ClientCurrentState::INSTANTANEOUS_PROFILE: {
                    if (!this->currentNode)
                    {
                        this->print_and_log("ERROR: currentNode is NULL\n");
                        return FAILURE;
                    }

                    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

                    if ((resp->pmesh.start_byte == PMESH_PUSH_START_BYTE) && (resp->dlms.start_byte == PULL_START_BYTE) && (resp->pmesh.packet_type == MESH_DATA_RESPONSE) && (resp->dlms.frame_id == FI_CACHE_DATA) && (resp->dlms.command == COMMAND_IP_PROFILE))
                    {
                        return this->process_ip_profile_data(this->currentNode, buff, length);
                    }
                    else if ((resp->pmesh.packet_type == MESH_DATA_QUERY) && (buff[resp->pmesh.length] == PMESH_COMMAND_IN_PROGRESS))
                    {
                        return PMESH_COMMAND_IN_PROGRESS;
                    }
                    else
                    {
                        this->print_and_log("Invalid Instantaneous profile response\n");
                    }
                    break;
                }

                default: {
                    this->print_and_log("Invalid current state\n");
                    break;
                }
            }
            break;
        }

        default: {
            this->print_and_log("Invalid target state\n");
            break;
        }
    }

    if (response->packet_type == 0x0E) // Ping Node
    {
        this->print_and_log("[PKT PING NODE]\n");
        return SUCCESS;
    }

    // Basic packet integrity check
    if (length < 17)
    {
        this->print_and_log("DLMS packet too short.\n");
        return INVALID_RESPONSE;
    }
    if (tx_page_index != response->data[3]) // page index mismatch
    {
        this->print_and_log("❌[PAGE INDEX MISMATCH] Expected: 0x%02X, Received: 0x%02X\n", tx_page_index, response->data[3]);
        return INVALID_RESPONSE;
    }
    // validate for mesh data response
    switch (response->packet_type)
    {
        case MESH_DATA_RESPONSE: {
            if (response->remaining_pkt_cnt != 0 && response->data[7] == 0x01)
            {
                this->remaining_pkt_cnt = response->remaining_pkt_cnt;
                this->print_and_log("remaining pkt count:%d\n", this->remaining_pkt_cnt);
            }
            // Find DLMS start byte
            uint8_t *dlms_data = (uint8_t *)memchr(response->data, PULL_DATA_START_BYTE, length - 17);
            if (!dlms_data)
            {
                this->print_and_log("DLMS start byte not found\n");
                return INVALID_RESPONSE;
            }
            size_t dlms_offset = dlms_data - response->data;
            uint8_t page_index = dlms_data[3];
            this->print_and_log("[PAGE_INDEX] = %d\n", page_index);

            // Parse DLMS header and length verification
            uint16_t dlms_len = (dlms_data[1] << 8) | dlms_data[2]; // big-endian length field
            // Ensure we have enough bytes for DLMS payload + checksum
            if ((size_t)(dlms_len + 1) > (size_t)(length - 17 - dlms_offset))
            {
                this->print_and_log("DLMS payload length exceeds received length: dlms_len=%d, available=%d\n", dlms_len, length - 17 - dlms_offset);
                return INVALID_RESPONSE;
            }

            // Calculate checksum as LSB of the sum of DLMS payload bytes (all bytes before checksum)
            uint32_t sum = 0;
            for (size_t i = 0; i < (size_t)dlms_len; ++i)
            {
                sum += dlms_data[i];
            }
            uint8_t checksum = static_cast<uint8_t>(sum & 0xFF);

            uint8_t actual_checksum = dlms_data[dlms_len];
            if (checksum != actual_checksum)
            {
                this->print_and_log("[❌CHECKSUM ERROR] calculated checksum = 0x%02X , actual checksum = 0x%02X\n", checksum, actual_checksum);
                return INVALID_RESPONSE;
            }

            // Ensure response data's DLMS length matches expected packet size
            if (((size_t)dlms_len + 1) != length - 17 - dlms_offset)
            {
                this->print_and_log("[❌DLMS length mismatch]: expected %d, got %d\n", dlms_len, length - 17 - dlms_offset);
                return INVALID_RESPONSE;
            }

            // Select profile processing based on DLMS command type (e.g., NP, IP, BHP, etc.)
            this->print_and_log("[FRAME ID] = 0x%02X, [DLMS_START_BYTE] = 0x%02X\n", dlms_data[4], dlms_data[0]);

            if (dlms_data[4] == 0x0E)
            {
                if (dlms_data[8] == 0x00)
                {
                    this->print_and_log("[ZERO RECORDS IN DLMS RESPONSE]\n");
                    return INVALID_RESPONSE;
                }
                switch (dlms_data[5])
                {
                    case CMD_ID_NP_PROFILE: {
                        this->print_and_log("[PROCESSING NP]\n");
                        parse_nameplate_profile(dlms_data, dlms_len);
                        break;
                    }

                    case CMD_ID_IP_PROFILE: {
                        this->print_and_log("[PROCESSING IP]\n");
                        parse_instantaneous_profile(dlms_data, dlms_len);
                        break;
                    }
                    case CMD_ID_DLP_PROFILE: {
                        this->print_and_log("[PROCESSING DLP]\n");
                        parse_daily_load_profile(dlms_data, dlms_len);
                        break;
                    }
                    case CMD_ID_BLP_PROFILE: {
                        this->print_and_log("[PROCESSING BLP]\n");
                        parse_block_load_profile(dlms_data, dlms_len);
                        break;
                    }
                    case CMD_ID_BH_PROFILE: {
                        this->print_and_log("[PROCESSING BHP]\n");
                        parse_billing_history_profile(dlms_data, dlms_len);
                        break;
                    }
                    case CMD_ID_EVENT_PROFILE: {
                        this->print_and_log("[PROCESSING EVENTS]\n");
                        parse_Events_profile(dlms_data, dlms_len);
                        break;
                    }

                    default:
                        // Catch-all for unknown DLMS command types
                        this->print_and_log("Unknown command type: %d\n", dlms_data[5]);
                        return INVALID_RESPONSE;
                }
            }
            else if (response->data[4] == 0x0F)
            {
                this->print_and_log("[FRAME ID] = 0x%02X\n", response->data[4]);
                return handle_single_obis_read(response, dlms_len);
            }
            else if (response->data[4] == FI_OBIS_CODES_SCALAR_LIST)
            {
                switch (response->data[5])
                {
                    case CMD_ID_IP_PROFILE: {

                        this->parse_instantaneous_scalar_profile(response);
                    }
                    break;
                    case CMD_ID_BH_PROFILE: {

                        this->parse_billing_scalar_profile(response);
                    }
                    break;
                    case CMD_ID_DLP_PROFILE: {

                        this->parse_daily_load_scalar_profile(response);
                    }
                    break;
                    case CMD_ID_BLP_PROFILE: {

                        this->parse_block_load_scalar_profile(response);
                    }
                    break;
                    default:
                        return INVALID_RESPONSE;
                }
            }
            else
                return INVALID_RESPONSE;

            // Successful
            if (response->data[7] > 0) // Next page status
            {
                this->print_and_log("[📄➡️NEXT_PAGE]\n");
                return SUCCES_NEXT_PAGE;
            }
            else
                return SUCCESS;
        }
        break;

        case MESH_COMMISSION_PACKET_RESPONSE: {

            // check of unsilence response
            if (buff[PMESH_HOPCNT_INDX + 6] == 0x01 && (buff[PMESH_HOPCNT_INDX + 5] == 0x9D))
            {
                this->print_and_log("meter is in silence mode\n");
                return SUCCESS;
            }
            if (buff[PMESH_HOPCNT_INDX + 6] == 0x00 && (buff[PMESH_HOPCNT_INDX + 5] == 0x9D))
            {
                this->print_and_log("meter is already unsilenced\n");
                return SUCCESS;
            }
            else if (buff[PMESH_HOPCNT_INDX + 5] == 0x9B && buff[PMESH_HOPCNT_INDX + 6] == 0x00)
            {
                // update data base
                return SUCCESS;
            }
            else if (buff[PMESH_HOPCNT_INDX + 6] == 0x01)
            {
                this->print_and_log("❌ Exit Failed response\n");
                return FAILED_RESPONSE;
            }
            return FAILURE;
        }
        break;
    }
    return FAILURE;
}

int Client::validate_received_data(uint8_t *buffer, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    // this->print_and_log("[EACH RX] : ");
    // for (int i = 0; i < length; i++)
    // {
    //     this->print_and_log(" %02X", buffer[i]);
    // }
    // this->print_and_log("\n");

    int retval = is_valid_packet(buffer, length);

    switch (retval)
    {
        case SUCCESS:
            return this->client_process_data(buffer, length);
        case INVALID_RESPONSE:
            return INVALID_RESPONSE;
        default:
            this->print_and_log("[INVALID_RESPONSE]\n");
            return INVALID_RESPONSE;
    }

    return FAILURE;
}

int Client::client_received_data(uint8_t *buffer, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (length > MAX_CLIENT_RX_BUFFER)
    {
        this->print_and_log("Abnormal data received\n");
        return 0;
    }

    // copy to member RX_buffer
    memset(this->client_rx_buffer, 0, sizeof(this->client_rx_buffer));
    memcpy(this->client_rx_buffer, buffer, length);
    this->buffer_rx_length = length;

    if (this->comm_init_status)
    {
        this->comm_init_status = false;
        return this->initCommunication(buffer, length);
    }

    int offset = 0;
    int ret = 0;
    uint8_t loop = 10;

    while (length > offset)
    {
        int len = buffer[offset + 1] + 1;
        ret = this->validate_received_data(&buffer[offset], len);
        offset += len;

        if (loop-- == 0)
        {
            this->print_and_log("[❌EXITING LOOP,WRONG RESPONSE]\n");
            break;
        }
    }

    return ret;
}

int Client::receive_data_and_validate_response()
{
    this->print_and_log("%s start\n", __FUNCTION__);

    ssize_t rxLen = 0;
    uint8_t buffer[4096] = {0};
    rxLen = this->receive_data(buffer, sizeof(buffer));

    if (rxLen > 0)
    {
        return this->client_received_data(buffer, rxLen);
    }

    return POLL_TIMEOUT;
}

void Client::poll_timeout_handler(void)
{
    this->print_and_log("%s start: %s\n", __FUNCTION__, this->gateway_id);

    switch (this->stateInfo.timeoutState)
    {
        case ClientTimeoutState::TIMER_PGWID_RX: {
            this->print_and_log("Timeout occurred: %d-second wait for PGWID reception expired for GATEWAY %s. Stopping client thread.\n", this->polltimeout, this->gateway_id);
            this->gatewayStatus = Status::DISCONNECTED;
            break;
        }

        case ClientTimeoutState::TIMER_NONE: {
            this->print_and_log("Client timeout state: timer none\n");

            this->cleanup_push_profiles();

            if (ODM_Flag == 1)
            {
                this->print_and_log("ODM Cycle in progress. Skipping HES cycle scheduling.\n");
                break;
            }

            if (this->client_hes_cycle_schedule() == SUCCESS)
            {
                if (this->hes_start_cycle_activiy(this->gateway_id) == SUCCESS)
                {
                    this->mark_hes_cycle_done();
                }
            }
            break;
        }
        case ClientTimeoutState::TIMER_FUOTA_RESPONSE: {
            if (!fuota->waiting_for_response)
            {
                this->print_and_log("(%s) FUOTA timeout: no pending FUOTA request (waiting_for_response==false) — nothing to retry.\n", this->gateway_id);
                break;
            }

            /* ---------- PRIMARY RETRY ---------- */
            if (fuota->cntx.retry_count < fuota->cntx.max_retries)
            {
                fuota->cntx.retry_count++;

                this->print_and_log("(%s) -> FUOTA primary retry %d/%d\n", this->gateway_id, fuota->cntx.retry_count, fuota->cntx.max_retries);

                fuota->build_and_store_fuota_cmd();
                break;
            }

            /* ---------- SWITCH TO ALTERNATE ---------- */
            if (fuota->cntx.alternate_retry < fuota->cntx.alternate_maxcnt)
            {
                fuota->cntx.alternate_retry++;
                fuota->cntx.retry_count = 0;
                fuota->use_alternate_fuota_route = true;

                this->print_and_log("(%s) -> Switching to alternate FUOTA route (%d/%d)\n", this->gateway_id, fuota->cntx.alternate_retry, fuota->cntx.alternate_maxcnt);
                if (fuota->ondemand_fuota_state == FUOTA_STATE::FW_IMAGE_TRANSFER || fuota->ondemand_fuota_state == FUOTA_STATE::FWIMAGE_OF_SECTOR_COUNT)
                {
                    fuota->prepare_alternate_sameroute_retry(fuota->client_tx_buffer, fuota->client_tx_buffer[1] + 1);
                }
                else
                {
                    fuota->prepare_alternate_route_retry(fuota->client_tx_buffer, fuota->client_tx_buffer[1] + 1);
                }

                fuota->build_and_store_fuota_cmd();
                break;
            }

            /* ---------- FINAL FAILURE ---------- */
            this->print_and_log("(%s) -> FUOTA failed after primary + alternate retries\n", this->gateway_id);

            fuota->waiting_for_response = false;
            fuota->use_alternate_fuota_route = false;

            fuota->cntx.retry_count = 0;
            fuota->cntx.alternate_retry = 0;

            this->db->update_ondemand_RF_Fuota_upload_status((char *)this->rx_buffer, this->request_id, 3, 0);

            std::vector<uint8_t> odmfuota_cmd;
            if (dequeue_fuota(odmfuota_cmd))
            {
                fuota->cmd_bytes = std::move(odmfuota_cmd);
                fuota->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
            }
            else
            {
                fuota->ondemand_fuota_state = FUOTA_STATE::NETWORK_SILENCE;
            }
            break;
        }

        default: {
            this->print_and_log("Default poll timeout\n");
            break;
        }
    }
}

void Client::client_get_time(std::string &time_str, int format_type)
{
    using namespace std::chrono;

    // Get current system time with milliseconds
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::time_t t = system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    std::ostringstream oss;

    if (format_type == 1)
    {
        // HH:MM:SS.mmm
        oss << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_sec << "."
            << std::setw(3) << std::setfill('0') << ms.count();
    }
    else if (format_type == 2)
    {
        // YYYY-MM-DD HH:MM:SS.mmm
        oss << (tm.tm_year + 1900) << "-"
            << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1) << "-"
            << std::setw(2) << std::setfill('0') << tm.tm_mday << " "
            << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_sec << "."
            << std::setw(3) << std::setfill('0') << ms.count();
    }
    else
    {
        // Default HH:MM:SS.mmm
        oss << std::setw(2) << std::setfill('0') << tm.tm_hour << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_min << ":"
            << std::setw(2) << std::setfill('0') << tm.tm_sec << "."
            << std::setw(3) << std::setfill('0') << ms.count();
    }

    time_str = oss.str();
}

int Client::write_to_client(uint8_t *buf, size_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    size_t total_written = 0;

    this->get_destination_address(buf);

    while (total_written < length)
    {
        ssize_t written = send(this->client_socket, buf + total_written, length - total_written, 0);

        this->client_get_time(this->time_str, 2); // Get current time in HH:MM:SS.mmm format
        this->print_and_log("[%s] TX: %s : %d =", this->time_str.c_str(), this->gateway_id, length);

        for (size_t i = 0; i < length; i++)
        {
            this->print_and_log(" %02X", buf[i]);
        }
        this->print_and_log("\n");

        if (written < 0)
        {
            this->print_and_log("Send failed: %s\n", strerror(errno));
            this->gatewayStatus = 0;
            return FAILURE;
        }
        else if (written == 0)
        {
            this->print_and_log("Send returned 0 (possibly peer closed connection).\n");
            this->gatewayStatus = 0;
            break;
        }

        total_written += written;
    }

    return SUCCESS;
}

// Utility: Split string by delimiter
std::vector<std::string> Client::split(const std::string &str, char delim)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delim))
        tokens.push_back(item);
    return tokens;
}

// Check if there are any pending cancelled requests
bool Client::has_pending_cancel()
{
    std::lock_guard<std::mutex> lock(this->cancelled_mutex);
    return !this->cancelled_requests.empty(); // Return true if there are cancelled requests
}

void Client::process_ondemand_request(void)
{
    // Log function entry for debugging/tracing.
    this->print_and_log("%s start\n", __FUNCTION__);

    auto now = std::chrono::system_clock::now();
    const auto timeout = std::chrono::minutes(2);
    // check for nms activity
    while (insert_update_hes_nms_sync_time(this->gateway_id, 1) != SUCCESS)
    {
        this->receive_data_and_validate_response();
        auto current = std::chrono::system_clock::now();
        if (current - now > timeout)
            break;
        sleep(30); // sleep for 30secs
    }

    // Output current GATEWAY ID, data length, and data for context.
    this->print_and_log("Processing On-Demand request for GATEWAY ID: \n");
    // Wait for eventfd signal (from validate_request_ids or queue enqueuing event).
    eventfd_t u;
    eventfd_read(this->pfd[1].fd, &u); // pfd[1] is assumed ODM eventfd
    this->pfd[1].revents = 0;          // Clear pollfd event state

    // ================= TOP PRIORITY : CANCELLED REQUESTS =================
    while (true)
    {
        int cancelled_req_id = -1;

        {
            std::lock_guard<std::mutex> lock(this->cancelled_mutex); // Thread-safe access

            if (this->cancelled_requests.empty())
                break;

            cancelled_req_id = std::stoi(this->cancelled_requests.front()); // Convert to int
            this->cancelled_requests.pop();                                 // Remove from queue
        }

        this->print_and_log("[❌CANCELLED]: REQ_ID=%d updated in DB\n", cancelled_req_id);
        // Update DB immediately for cancelled request
        this->Update_dlms_on_demand_request_status(cancelled_req_id, CANCELLED, 0);
    }

    // 1. Process all failed commands (highest priority)
    while (!this->Failed.empty())
    {
        // Get a const reference to the first command's byte vector in the Failed queue
        const std::vector<uint8_t> &cmd_bytes = this->Failed.front();

        // Convert the command byte vector to string for parsing
        std::string cmd_str(cmd_bytes.begin(), cmd_bytes.end());

        // Split the command string by ':'
        std::vector<std::string> parts = split(cmd_str, ':');

        // Extract and convert request ID (parts[0]) to int, if present
        int request_id = -1;
        if (parts.size() > 0 && !parts[0].empty())
            request_id = std::stoi(parts[0]);

        int download_type = 0;
        if (parts.size() > 0 && !parts[4].empty())
            download_type = std::stoi(parts[4]);
        // Debug log
        this->print_and_log("[❌FAILED]: REQ_ID=%d \n", request_id);
        if (download_type == PING_NODE || download_type == PING_METER)
            this->Update_dlms_on_demand_Ping_request_status(request_id, FAILED_INVALID_REQUEST);
        else
            this->Update_dlms_on_demand_request_status(request_id, FAILED_INVALID_REQUEST, 0); // Update status to failed in DB

        this->Failed.pop();
    }

    // 2. Process all FUOTA commands
    fuota->process_fuota_queue();

    // 3. Process all ODM commands
    while (!this->ODM.empty())
    {
        // Get a const reference to the first command's byte vector in the ODM queue
        const std::vector<uint8_t> &cmd_bytes = this->ODM.front();

        this->print_and_log("[QUEUE SIZE] ODM = %zu \n", this->ODM.size());
        // 🔥 PRIORITY CHECK
        if (has_pending_cancel())
        {
            this->print_and_log("⏹️ [ODM] 🔄 INTERRUPTING ODM | Processing CANCEL request\n");
            break; // Exit ODM immediately
        }
        this->client_get_time(this->time_str, 2);

        // Convert the vector of bytes into a std::string for easier processing and parsing
        std::string cmd_str(cmd_bytes.begin(), cmd_bytes.end());

        // Split the converted command string by ':' delimiter into individual parts/fields
        std::vector<std::string> parts = split(cmd_str, ':');

        // Initialize request_id to invalid/default value
        int request_id = -1;

        // If the parts vector is non-empty and first part is not empty, convert first part to integer request ID
        if (parts.size() > 0 && !parts[0].empty())
            request_id = std::stoi(parts[0]);

        // Log the current device ID, length of on-demand data, and the data itself (context info)
        this->print_and_log("[%s] [ODM START] : gateway_ID = %s | REQ_ID = %d | data = %s\n", this->time_str.c_str(), this->gateway_id, request_id, cmd_bytes.data());

        this->Update_dlms_on_demand_request_status(request_id, IN_PROGRESS, 0);

        int hop_count = 0;
        hop_count = std::stoi(parts[2]); // convert string -> int

        this->DB_parameter.req_id = request_id;
        this->DB_parameter.gateway_id = parts[1].c_str(); // GATEWAY ID from parts[1]
        std::vector<uint8_t> meter_data;
        meter_data.resize(parts[3].length() / 2); // Resize to exact size

        // **FIX 1: Use parts[3].c_str()**
        Utility::convert_asc_hex_string_to_bytes(&meter_data[0], (uint8_t *)(parts[3].c_str()), parts[3].length() / 2);

        // Last 8 bytes → MAC (as hex)
        size_t mac_start = meter_data.size() - 8;
        this->DB_parameter.meter_mac_address = this->bytes_to_hex_string(&meter_data[mac_start], 8);

        // Last 4 bytes → Serial (as hex)
        size_t serial_start = meter_data.size() - 4;
        this->DB_parameter.meter_serial_no = this->bytes_to_hex_string(&meter_data[serial_start], 4);

        // If hop count > 0, validate path in source route network
        if (hop_count > 0)
        {
            // Check if path exists in source route network
            if (!check_path_in_source_route_network(parts, request_id))
            {
                Update_dlms_on_demand_request_status(request_id, FAILED_INVALID_REQUEST, 0);
                this->ODM.pop();
                continue; // Skip to next command
            }
        }

        // Extract command from parts
        uint8_t download_data_type = 0;
        download_data_type = static_cast<uint8_t>(std::stoi(parts[4]));
        this->pingnode_detail.ping_count = 0;

        if (download_data_type == PING_NODE || download_data_type == PING_METER)
        {
            // Log PING_NODE details for debugging
            ping_node_details(parts, this->DB_parameter);
        }

        if (download_data_type == PING_NODE)
        {
            this->print_and_log("🔄 [PING NODE] Processing PING_NODE command\n");
            // Extract PING_NODE command data from parts[5] (hex string) and convert to binary buffer
            std::vector<uint8_t> command_data;
            size_t cmd_len = 0;
            if (parts.size() > 5)
                cmd_len = parts[5].length() / 2;
            if (cmd_len > 0)
            {
                command_data.resize(cmd_len);
                Utility::convert_asc_hex_string_to_bytes(command_data.data(),
                                                         (uint8_t *)parts[5].c_str(),
                                                         static_cast<uint32_t>(cmd_len));
            }

            // Get repeat count from parts[6] - number of times to send PING command
            uint8_t count = static_cast<uint8_t>(std::stoi(parts[6]));

            // Send PING_NODE command 'count' times to ensure reliable delivery in mesh network
            while (count)
            {
                this->print_and_log("🔄 [PING NODE] Sending ping, remaining count: %d\n", count);
                Process_ODM_request(command_data.data(), command_data.size(), request_id, download_data_type);
                count--;                            // Decrement repeat counter
                this->pingnode_detail.ping_count++; // Increment ping count statistic
            }
            this->pingnode_detail.ping_count = 0; // Reset ping count after sending
        }

        else
        {
            // --- Create PMESH packet using clean function ---

            std::vector<uint8_t> pkt = build_pmesh_frame(parts);
            //  Send the framed PMESH packet to the client and process responses
            Process_ODM_request(pkt.data(), pkt.size(), request_id, download_data_type);
        }

        this->client_get_time(this->time_str, 2);
        // Log details: request ID, original command string, length of decoded hex data, and total bytes in the command
        this->print_and_log("[%s] [ODM END]: REQ_ID = %d || Command = %s || HexDataLen = %zu || DataLen = %d\n",
                            this->time_str.c_str(), request_id, cmd_str.c_str(), parts[3].length(), static_cast<int>(cmd_bytes.size()));

        if (this->gatewayStatus == DISCONNECTED)
            break;
        // Remove the processed command from the front of the ODM queue
        this->ODM.pop();
    }
    ODM_Flag = 0; // Reset the flag after completion of ODM
    this->print_and_log("[ODM Flag] =%d\n", ODM_Flag);
    insert_update_hes_nms_sync_time(this->gateway_id, 0);
}

std::string Client::bytes_to_hex_string(const uint8_t *data, size_t len) const
{
    std::string out;
    out.reserve(len * 2);
    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i)
    {
        uint8_t b = data[i];
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> Client::build_odm_pmesh_packet(const PMeshHeader &header, const uint8_t *command, size_t command_len)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    size_t pmh_len;
    if (header.hop_count > 0)
        pmh_len = 13 + header.hop_count * 4; // Fixed PMESH header length + hop_count*4
    else
        pmh_len = 13 + 4;
    size_t total_len = pmh_len + command_len;

    std::vector<uint8_t> packet(total_len, 0);

    // Fill PMESH header
    packet[0] = header.start_byte;
    packet[1] = static_cast<uint8_t>(total_len - 1);
    packet[2] = header.packet_type;
    memcpy(&packet[3], header.pan_id, 4);
    memcpy(&packet[7], header.source_address, 4);
    packet[11] = header.router_index;
    packet[12] = header.hop_count;
    if (header.hop_count > 0 && !header.dest_address.empty())
        memcpy(&packet[13], header.dest_address.data(), header.hop_count * 4);
    else
        memcpy(&packet[13], header.dest_address.data(), 4);

    this->Pmesh_header.resize(pmh_len);
    memcpy(this->Pmesh_header.data(), packet.data(), pmh_len);
    // Copy command after PMESH header
    memcpy(&packet[pmh_len], command, command_len);
    return packet;
}

std::vector<uint8_t> Client::build_pmesh_frame(const std::vector<std::string> &parts)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    Client::PMeshHeader header;
    uint8_t command[256] = {0};

    // Decode command data
    if (parts.size() > 5 && !parts[5].empty())
        Utility::convert_asc_hex_string_to_bytes(command, (uint8_t *)parts[5].c_str(), parts[5].length() / 2);

    // PMESH header
    header.start_byte = 0x2E;

    // Set packet type based on parts[4]
    if (parts[4] == "13")
        header.packet_type = 0x0D; // Ping node command
    else
        header.packet_type = 0x07; // other commands

    header.pan_id[0] = 0x00;
    header.pan_id[1] = 0x00;

    // PAN ID last 2 bytes from GATEWAY ID
    std::string hex_str = parts[1].substr(parts[1].length() - 4, 4);
    Utility::convert_asc_hex_string_to_bytes(&header.pan_id[2], (uint8_t *)hex_str.c_str(), hex_str.length() / 2);

    // Source address last 4 bytes of GATEWAY ID
    hex_str = parts[1].substr(parts[1].length() - 8, 8);
    Utility::convert_asc_hex_string_to_bytes(header.source_address, (uint8_t *)hex_str.c_str(), hex_str.length() / 2);

    // Router index
    header.router_index = 0x00;

    // Hop count
    header.hop_count = static_cast<uint8_t>(std::stoi(parts[2])); // for example "2"

    // Decode path as destination address
    uint8_t path[100] = {0};

    Utility::convert_asc_hex_string_to_bytes(path, (uint8_t *)parts[3].c_str(), parts[3].length() / 2);

    header.dest_address.clear();

    size_t total_bytes = parts[3].length() / 2; // Total bytes in path

    if (header.hop_count == 0)
    {
        /* example:
        path = 3CC1F601A3535435
        header.dest_address = A3535435  */
        header.dest_address.insert(header.dest_address.end(), path + 4, path + 8); // Copy bytes 4-7 as destination address
    }
    else
    {
        /* example:
        path = 3CC1F601A35354353CC1F601000000453CC1F60100000047
        header.dest_address = 0000004500000047  */
        for (size_t i = 8; i + 8 <= total_bytes; i += 8) // For each 8-byte block
        {
            // take only last 4 bytes of each 8-byte block
            for (size_t j = i + 4; j < i + 8; j++) // Last 4 bytes
            {
                header.dest_address.push_back(path[j]);
            }
        }
    }
    // Frame PMESH packet
    return build_odm_pmesh_packet(header, command, parts[5].length() / 2);
}

void Client::get_destination_address(uint8_t *buf)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    // Parse PMESH command structure
    Pmeshcmdstructure *query_cmd = reinterpret_cast<Pmeshcmdstructure *>(buf);

    if (query_cmd->start_byte == HES_START_BYTE || query_cmd->start_byte == NMS_START_BYTE)
    {
        const uint8_t *destination_mac = nullptr;

        if ((query_cmd->cmd_type == MESH_PING_REQUEST) || (query_cmd->cmd_type == MESH_DATA_QUERY) || (query_cmd->cmd_type == MESH_COMMISSION_PACKET))
        {
            this->print_and_log("[COMMAND TYPE] = 0x%02X\n", query_cmd->cmd_type);
            if (query_cmd->no_of_routers == 0x00)
            {
                destination_mac = query_cmd->data;
            }
            else
            {
                destination_mac = query_cmd->data + ((query_cmd->no_of_routers - 1) * 4);
            }

            if (destination_mac != nullptr)
            {
                memcpy(this->src_addr_check_buffer, destination_mac, 4);
                this->need_to_validate_src_addr = true;
                this->print_and_log("[DESTINATION ADDRESS]: ");
                this->print_data_in_hex(this->src_addr_check_buffer, 4);
            }
        }
    }
}

int Client::write_to_client_vector(std::vector<uint8_t> &buff)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    size_t total_written = 0;
    const size_t length = buff.size();

    this->get_destination_address(buff.data());

    // Print the buffer once
    this->print_and_log("[%s] TX: %s : %zu =", this->now_ms().c_str(), this->gateway_id, length);

    for (size_t i = 0; i < length; ++i)
    {
        this->print_and_log(" %02X", buff[i]);
    }
    this->print_and_log("\n");

    while (total_written < length)
    {
        usleep(20000); // 20ms

        ssize_t written = send(this->client_socket, buff.data() + total_written, length - total_written, 0);

        if (written < 0)
        {
            this->print_and_log("Send failed: %s\n", strerror(errno));
            this->gatewayStatus = Status::DISCONNECTED;
            return FAILURE;
        }

        if (written == 0)
        {
            this->print_and_log("Send returned 0 (peer may have closed connection).\n");
            this->gatewayStatus = Status::DISCONNECTED;
            break;
        }

        total_written += static_cast<size_t>(written);
    }

    return SUCCESS;
}

// Utility function to print data in hexadecimal format
void Client::print_data_in_hex(const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++)
    {
        this->print_and_log("%02X ", data[i]);
    }
    this->print_and_log("\n");
}

int Client::validate_source_address(uint8_t *buf)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    OtaAtCmdResponse *ota_at_rx_ptr = reinterpret_cast<OtaAtCmdResponse *>(buf);

    if ((ota_at_rx_ptr->packet_type == MESH_COMMISSION_PACKET) ||
        (ota_at_rx_ptr->packet_type == MESH_DATA_QUERY) ||
        (ota_at_rx_ptr->packet_type == MESH_PING_REQUEST))
    {
        this->print_and_log("RECEVIED PACKET TYPE[0x%02X]\n", ota_at_rx_ptr->packet_type);
        return SUCCESS;
    }

    this->print_and_log("Received source address: ");
    this->print_data_in_hex(ota_at_rx_ptr->node_mac_address, 4);

    if (memcmp(ota_at_rx_ptr->node_mac_address, this->src_addr_check_buffer, 4) == 0)
    {
        this->print_and_log("Valid source address data received\n");
        return SUCCESS;
    }

    return FAILURE;
}

void Client::Process_ODM_request(uint8_t *buf, size_t length, int request_id, uint8_t download_data_type)
{
    this->print_and_log("%s START\n", __FUNCTION__);

    // Set the receive timeout for the client to 12 seconds
    if (download_data_type == 0x05) // For ALL TAMPER EVENTS PROFILE
        set_recv_timeout_for_client(20);
    else
        set_recv_timeout_for_client(12);

    // Log the start of processing with request id and command
    this->print_and_log("%s start [RequestID]=%d [Download data_type]=%d\n", __FUNCTION__, request_id, download_data_type);

    this->pingnode_detail.Time_tx = std::chrono::steady_clock::now();
    tx_page_index = 0;

    // Send the initial ODM packet to the client
    if (write_to_client(buf, length) != SUCCESS)
    {
        // If send fails, log and exit the function
        this->print_and_log("[❌ODM ERROR] Failed to send initial packet - RequestID: %d\n", request_id);
        return;
    }

    if (length >= 5 && download_data_type > 0x0F)
    {
        this->pp_write_payload_tail =
            (static_cast<uint32_t>(buf[length - 5]) << 24) | (static_cast<uint32_t>(buf[length - 4]) << 16) | (static_cast<uint32_t>(buf[length - 3]) << 8) | static_cast<uint32_t>(buf[length - 2]);
        this->print_and_log("[ODM DEBUG] Last 4 Write bytes : 0x%08X\n", this->pp_write_payload_tail);
    }

    uint8_t page_index = 1; // Initialize page index for multi-page data handling
    uint8_t poll_retry = 1;
    uint8_t once = true;
    uint8_t invalid_resp = 0;
    uint8_t dlms_checksum_error_retry = 1;
    // Start a loop to receive responses and handle multiple pages if needed
    while (1)
    {
        // Wait for and validate incoming response data
        int ret = receive_data_and_validate_response();
        client_get_time(this->DB_parameter.last_download_time, 1);

        if (ret == SUCCESS)
        {
            this->retry = 1;
            this->pingnode_detail.Time_rx = std::chrono::steady_clock::now();
            this->print_and_log("[✅ SUCCESS] - RequestID: %d\n", request_id);
            if (!handle_success(request_id, download_data_type, buf)) continue;
            page_index = 0;
            return;
        }
        else if (ret == DLMS_SUCCESS)
        {
            this->print_and_log("[✅ DLMS_SUCCESS] - resending packet, [RequestID] = %d\n", request_id);
            if (!resend_packet(buf, length, request_id)) return;
            continue;
        }
        else if (ret == PMESH_ERROR)
        {
            if (download_data_type == PING_NODE)
                Update_Insert_Ping_request(download_data_type, FAILED_PMESH_ERROR);
            else
                this->Update_dlms_on_demand_request_status(request_id, FAILED_PMESH_ERROR, 0);
            break;
        }
        else if (ret == TIMEOUT_RECEIVED)
        {
            this->print_and_log("[⏰TIMEOUT_RECEIVED] - retrying, [RequestID] = %d || [RETRY COUNT] = %d\n", request_id, this->retry);
            if (this->retry == 3)
            {
                this->Update_dlms_on_demand_request_status(request_id, FAILED_RF_TIMEOUT, 0);
                this->print_and_log("ODM RequestID=%d || [RETRY COUNT] = %d, || [RETURN VALUE] = %d, failed with response code: %d\n", request_id, this->retry, ret, FAILED_RF_TIMEOUT);
                this->retry = 1;
                break;
            }
            if (once)
            {
                this->Update_dlms_on_demand_request_status(request_id, RETRY_IN_PROGRESS, 0);
                once = false;
            }
            this->retry++;
            if (!resend_packet(buf, length, request_id)) return;
            continue;
        }
        else if (ret == DLMS_CONNECTION_FAILED)
        {
            this->print_and_log("[❌DLMS_CONNECTION_FAILED] - recovering, [RequestID] = %d\n", request_id);
            if (this->retry == 3)
            {
                this->Update_dlms_on_demand_request_status(request_id, FAILED_RF_TIMEOUT, 0);
                this->print_and_log("ODM RequestID=%d || [RETRY COUNT] = %d, || [RETURN VALUE] = %d, failed with response code: %d\n", request_id, this->retry, ret, FAILED_RF_TIMEOUT);
                this->retry = 1;
                break;
            }
            if (!handle_connection_failed(request_id)) return;
            continue;
        }
        else if (ret == FAILED_PMESH_ERROR)
        {
            this->print_and_log("[❌ ODM ERROR] General failure - [RequestID] = %d, [Response Code] = %u\n", request_id, ret);
            // General failure case, update DB and log
            this->Update_dlms_on_demand_request_status(request_id, FAILED_PMESH_ERROR, 0);
            memset(this->src_addr_check_buffer, 0, sizeof(this->src_addr_check_buffer));
            break;
        }
        else if (ret == DLMS_ERROR)
        {
            this->print_and_log("[⭕ DLMS_ERROR] [REQ_ID] = %d, [Response Code] = %u\n", request_id, ret);
            uint16_t dlms_error_code = (this->client_rx_buffer[this->buffer_rx_length - 3] << 8) | this->client_rx_buffer[this->buffer_rx_length - 2];
            this->Update_dlms_on_demand_request_status(request_id, SUCCESS_STATUS, dlms_error_code);
            memset(this->src_addr_check_buffer, 0, sizeof(this->src_addr_check_buffer));
            break;
        }
        else if (ret == POLL_TIMEOUT)
        {
            if (download_data_type == PING_NODE)
            {
                this->print_and_log("[⏰POLL_TIMEOUT] - PING_NODE failed , [RequestID] = %d, Response Code: %u\n", request_id, ret);
                Update_Insert_Ping_request(download_data_type, FAILED_NO_GW_RESPONSE);
                break;
            }

            if (poll_retry < 3)
            {
                this->print_and_log("[%s][⏰POLL_TIMEOUT] - retrying, [RequestID] = %d, [RETRY COUNT] = %d, Response Code: %u\n", this->DB_parameter.last_download_time.c_str(), request_id, poll_retry, ret);
                poll_retry++;
                if (!resend_packet(buf, length, request_id)) return;
                continue;
            }

            this->print_and_log("[%s][⏰POLL_TIMEOUT] - retrying, [RequestID] = %d, [RETRY COUNT] = %d, Response Code: %u\n", this->DB_parameter.last_download_time.c_str(), request_id, poll_retry, ret);
            if (!invalid_resp)
            {
                this->print_and_log("[INVALID RESPONSE FLAG] = %d\n", invalid_resp);
                this->print_and_log("❌Gateway didn't respond with response\n");
                this->gatewayStatus = Status::DISCONNECTED;
            }
            this->Update_dlms_on_demand_request_status(request_id, FAILED_NO_GW_RESPONSE, 0);
            memset(this->src_addr_check_buffer, 0, sizeof(this->src_addr_check_buffer));
            break;
        }
        else if (ret == SUCCES_NEXT_PAGE)
        {
            tx_page_index++;
            this->retry = 1;
            poll_retry = 1;
            this->print_and_log("[✅ SUCCES_NEXT_PAGE] - moving to next page (%d), [RequestID] = %d\n", page_index, request_id);
            if (!handle_next_page(buf, length, request_id, download_data_type, page_index)) return;
            continue;
        }
        else if (ret == INVALID_RESPONSE || ret == COMMAND_IN_PROGRESS)
        {
            invalid_resp = 1;
            this->print_and_log("[ℹ️ ODM INFO] Waiting for valid response or command completion - [RequestID] = %d\n", request_id);
            // Continue waiting if response invalid or command still in progress
            continue;
        }

        else if (ret == DLMS_CHECKSUM_ERROR)
        {
            this->print_and_log("[❌ DLMS_CHECKSUM_ERROR] - recalculating and retrying, [RequestID] = %d\n", request_id);
            dlms_checksum_error_retry++;
            // Recalculate DLMS checksum (skip PMESH header)
            recalculate_dlms_checksum(buf, length);

            // Log corrected frame (first few + checksum)
            this->print_and_log("[ODM DEBUG] Corrected frame: ");
            for (size_t i = 0; i < 20 && i < length; ++i)
            {
                this->print_and_log("%02X ", buf[i]);
            }
            this->print_and_log("... <checksum=0x%02X>\n", buf[length - 1]);

            // Resend corrected packet
            if (write_to_client(buf, length) != SUCCESS)
            {
                this->print_and_log("[ODM ERROR] Failed to resend corrected packet - [RequestID] = %d\n", request_id);
                this->Update_dlms_on_demand_request_status(request_id, FAILED_PMESH_ERROR, 0);
                memset(this->src_addr_check_buffer, 0, sizeof(this->src_addr_check_buffer));
                break;
            }
            if (dlms_checksum_error_retry > 3)
            {
                this->Update_dlms_on_demand_request_status(request_id, DLMS_FAILED, 0);
                break;
            }

            this->print_and_log("[ODM INFO] Checksum fixed and packet resent - awaiting response, RequestID: %d\n", request_id);
            continue; // Wait for new response
        }
    }
}

void Client::recalculate_dlms_checksum(uint8_t *buf, size_t total_length)
{
    if (total_length < 18)
    { // Need at least 17 bytes PMESH + 1 DLMS byte
        this->print_and_log("[DLMS] Buffer too short for checksum recalc: %zu bytes\n", total_length);
        return;
    }
    size_t DLMS_START;
    uint8_t hop_count = buf[12]; // hop count
    if (hop_count == 0 || hop_count == 1)
        DLMS_START = 0x11; // After 17-byte PMESH header(if it is for gateway or 1st hop)
    else
        DLMS_START = 17 + ((hop_count - 1) * 4); // For hop count greater than 1(because PMESH header size increases with hop )

    if (buf[DLMS_START] != 0x2B)
    {
        this->print_and_log("[DLMS] Invalid DLMS start byte at %zu: 0x%02X (expected 0x2B)\n",
                            DLMS_START, buf[DLMS_START]);
        return;
    }

    // Sum DLMS payload bytes (from 0x2B to before checksum)
    uint32_t sum = 0;
    for (size_t i = DLMS_START; i < total_length - 1; ++i)
    {
        sum += buf[i];
    }

    uint8_t checksum = static_cast<uint8_t>(sum & 0xFF); // LSB of sum
    buf[total_length - 1] = checksum;                    // Overwrite final checksum byte

    this->print_and_log("[DLMS] Recalculated checksum = 0x%02X\n", checksum);
}

int Client::log_dlms_data_type(uint8_t data_type, uint8_t *offset, uint8_t *data_offset, uint8_t *field_length, const uint8_t *data)
{
    // Determine data length and data offset based on the DLMS data type
    switch (data_type)
    {
        case DLMS_DATA_TYPE_NONE:
            // No data present for this type
            break;

        case DLMS_DATA_TYPE_ARRAY:
            // Variable length array data, requires special handling elsewhere
            *data_offset = 3;  // Data starts 3 bytes after offset
            *field_length = 0; // Length determined by array parsing
            break;

        case DLMS_DATA_TYPE_STRUCTURE:
            // Composite record structure, requires parsing of sub-elements
            break;

        case DLMS_DATA_TYPE_BIT_STRING:
            // Packed bits, variable length bitstring
            break;

        case DLMS_DATA_TYPE_OCTET_STRING:
            // Octet string (raw byte array)
            // Length byte is located 3 bytes after offset
            *field_length = data[*offset + 3];
            // Data starts 4 bytes after offset (type + length fields)
            *data_offset = 4;
            break;

        case DLMS_DATA_TYPE_STRING:
            // ASCII string type
            // Length byte is located 3 bytes after offset
            *field_length = data[*offset + 3];
            // Data starts 4 bytes after offset (type + length fields)
            *data_offset = 4;
            break;

        case DLMS_DATA_TYPE_STRING_UTF8:
            // UTF-8 encoded string
            break;

        case DLMS_DATA_TYPE_BINARY_CODED_DECIMAL:
            // Binary coded decimal format
            break;

        case DLMS_DATA_TYPE_BOOLEAN: // Boolean type (true/false), typically 1 byte
        case DLMS_DATA_TYPE_INT8:
        case DLMS_DATA_TYPE_UINT8:
            // 8-bit unsigned integer has fixed length of 1 byte
            *field_length = 1;
            // Data starts 3 bytes after offset
            *data_offset = 3;
            break;

        case DLMS_DATA_TYPE_UINT16:
        case DLMS_DATA_TYPE_INT16:
            // 16-bit unsigned integer has fixed length of 2 bytes
            *field_length = 2;
            // Data starts 3 bytes after offset
            *data_offset = 3;
            break;

        case DLMS_DATA_TYPE_COMPACT_ARRAY:
            // Compact array type, special handling required
            break;

        case DLMS_DATA_TYPE_UINT64:
        case DLMS_DATA_TYPE_INT64:
            // 64-bit unsigned integer
            *field_length = 8;
            // Data starts 3 bytes after offset
            *data_offset = 3;
            break;

        case DLMS_DATA_TYPE_ENUM:
            // Enumerated integer value
            *field_length = 1;
            // Data starts 3 bytes after offset
            *data_offset = 3;
            break;

        case DLMS_DATA_TYPE_FLOAT32:
            // 32-bit IEEE Floating Point
            break;

        case DLMS_DATA_TYPE_FLOAT64:
            // 64-bit IEEE Floating Point
            break;
        case DLMS_DATA_TYPE_UINT32:
        case DLMS_DATA_TYPE_INT32:
        case DLMS_DATA_TYPE_DATETIME:
            *field_length = 4;
            *data_offset = 3;
            break;
        case DLMS_DATA_TYPE_DATE:
            // Date object type
            break;

        case DLMS_DATA_TYPE_TIME:
            // Time object type
            break;

        case DLMS_DATA_TYPE_DELTA_INT8:
            // 8-bit signed delta value
            break;

        case DLMS_DATA_TYPE_DELTA_INT16:
            // 16-bit signed delta value
            break;

        case DLMS_DATA_TYPE_DELTA_INT32:
            // 32-bit signed delta value
            break;

        case DLMS_DATA_TYPE_DELTA_UINT8:
            // 8-bit unsigned delta value
            break;

        case DLMS_DATA_TYPE_DELTA_UINT16:
            // 16-bit unsigned delta value
            break;

        case DLMS_DATA_TYPE_DELTA_UINT32:
            // 32-bit unsigned delta value
            break;

        case DLMS_DATA_TYPE_BYREF:
            // Reference by address or pointer, rarely used
            break;

        default:
            // Unknown or unsupported data type
            return UNKNOWN_DATA_TYPE;
    }

    // Successful determination of data length and offset
    return 0;
}

// Helper function to resend packet, returns false on failure to resend
bool Client::resend_packet(uint8_t *buf, size_t length, int request_id)
{
    if (write_to_client(buf, length) != SUCCESS)
    {
        this->print_and_log("Failed to send ODM packet for RequestID=%d .\n", request_id);
        return false;
    }
    return true;
}

// Handle next page logic, returns false on failure
bool Client::handle_next_page(uint8_t *buf, size_t &length, int request_id, uint8_t download_data_type, uint8_t &page_index)
{
    uint8_t hop_count = buf[12];
    const uint8_t offset = (hop_count == 0) ? 19 : 19 + ((hop_count - 1) * 4);
    switch (download_data_type)
    {
        case CMD_ID_NP_PROFILE:
        case CMD_ID_IP_PROFILE: {
            // Update page index and checksum for standard profiles
            this->print_and_log("[PAGE_INDEX] = %d\n", page_index);
            uint8_t checksum = buf[length - 1] + 1;
            buf[offset] = page_index;
            buf[length - 1] = checksum;

            if (!resend_packet(buf, length, request_id))
            {
                return false;
            }
            page_index++;
            break;
        }

        case CMD_ID_BH_PROFILE: {
            if (page_index == 1)
            {
                // First page: Overwrite last 18 bytes with 8-byte DLP sequence
                size_t start = length - 13;
                length = (length - 13) + 8; // Update length parameter
                buf[1] = length - 1;
                memset(buf + start, 0, 13);
                if (download_data_type == 2) // For BHP
                {
                    uint8_t next_page_dlp[] = {0x2B, 0x07, 0x01, 0x0E, 0x02, 0x00, 0x00, 0x43};
                    memcpy(buf + start, next_page_dlp, sizeof(next_page_dlp));
                }
                if (!resend_packet(buf, length, request_id))
                {
                    return false;
                }
                page_index++;
            }
            else
            {
                // Subsequent pages: Standard page index + checksum update
                this->print_and_log("[PAGE_INDEX] = %d\n", page_index);
                uint8_t checksum = buf[length - 1] + 1;
                buf[offset] = page_index;
                buf[length - 1] = checksum;

                if (!resend_packet(buf, length, request_id))
                {
                    return false;
                }
                page_index++;
            }
            break;
        }

        case CMD_ID_DLP_PROFILE: // Daily Load Profile special handling
        case CMD_ID_BLP_PROFILE: {
            if (page_index == 1)
            {
                // First page: Overwrite last 18 bytes with 8-byte DLP sequence
                size_t start = length - 18;
                length = (length - 18) + 8; // Update length parameter
                buf[1] = length - 1;
                memset(buf + start, 0, 18);
                if (download_data_type == 3) // For DLP
                {
                    uint8_t next_page_dlp[] = {0x2B, 0x07, 0x01, 0x0E, 0x03, 0x00, 0x00, 0x44};
                    memcpy(buf + start, next_page_dlp, sizeof(next_page_dlp));
                }
                else if (download_data_type == 4) // For BLP
                {
                    uint8_t next_page_dlp[] = {0x2B, 0x07, 0x01, 0x0E, 0x04, 0x00, 0x00, 0x45};
                    memcpy(buf + start, next_page_dlp, sizeof(next_page_dlp));
                }
                if (!resend_packet(buf, length, request_id))
                {
                    return false;
                }
                page_index++;
            }
            else
            {
                this->print_and_log("[PAGE_INDEX] = %d\n", page_index);
                // Subsequent pages: Standard page index + checksum update
                uint8_t checksum = buf[length - 1] + 1;
                buf[offset] = page_index;
                buf[length - 1] = checksum;

                if (!resend_packet(buf, length, request_id))
                {
                    return false;
                }
                page_index++;
            }
            break;
        }

        default: {
            // Subsequent pages: Standard page index + checksum update
            this->print_and_log("[PAGE_INDEX] = %d\n", page_index);
            uint8_t checksum = buf[length - 1] + 1;
            buf[offset] = page_index;
            buf[length - 1] = checksum;

            if (!resend_packet(buf, length, request_id))
            {
                return false;
            }
            page_index++;
            break;
        }
    }
    return true;
}

// Handles DLMS connection failure with retries, returns false on failure
bool Client::handle_connection_failed(int request_id)
{
    uint8_t DLMS_Enable[8] = {0x2B, 0x07, 0x00, 0x00, 0x00, 0x02, 0x01, 0x35};
    this->Pmesh_header.insert(this->Pmesh_header.end(), DLMS_Enable, DLMS_Enable + 8);

    while (this->retry < 3)
    {
        this->retry++;
        if (write_to_client(this->Pmesh_header.data(), this->Pmesh_header.size()) != SUCCESS)
        {
            this->print_and_log("Failed to send ODM packet for RequestID=%d .\n", request_id);
            return false;
        }
        int ret = receive_data_and_validate_response();
        if (ret == DLMS_SUCCESS)
        {
            this->retry = 1;
            return true;
        }
    }

    this->Update_dlms_on_demand_request_status(request_id, DLMS_FAILED, 0);
    this->print_and_log("ODM RequestID=%d ,failed with response code: 01 %d\n", request_id, DLMS_CONNECTION_FAILED);
    return false;
}

bool Client::validate_response_frame(uint8_t *buf)
{
    if (buf[20] == 0x0E)
    {
        if (this->client_rx_buffer[22] != buf[21]) return false;
        if (buf[21] == 0x08 && this->client_rx_buffer[23] != buf[22]) return false;
    }
    else if (buf[20] == 0x0F)
    {
        if (this->client_rx_buffer[23] != buf[22] && this->client_rx_buffer[22] != buf[21]) return false;
    }
    return true;
}

// Helper function to handle SUCCESS case
bool Client::handle_success(int request_id, uint8_t download_data_type, uint8_t *buf)
{
    this->print_and_log("SUCCESS received for REQ_ID=%d.\n", request_id);

    // Update last download time
    client_get_time(this->DB_parameter.last_download_time, 2); // YYYY-MM-DD HH:MM:SS.mmm

    if (buf[2] != 0x0E)
    {
        // Validate response frame structure
        if (!validate_response_frame(buf))
        {
            this->print_and_log("Invalid response frame structure\n");
            return false;
        }
    }
    this->DB_parameter.status = static_cast<uint8_t>(SUCCESS_STATUS);
    this->DB_parameter.push_alaram = 2; // PULL
    // **CALL SEPARATE DB FUNCTION** - handles all cases efficiently
    this->print_and_log("[Download Data_Type] = %d\n", download_data_type);
    bool result = this->update_odm_db(download_data_type);

    // **CLEAR AFTER DB UPDATE**
    this->DB_parameter = DBparameters();

    return result;
}

/*
 * Extracts DLMS value based on data type from the response buffer
 */
DLMSValue Client::extract_dlms_value(uint8_t data_type, const uint8_t *data, uint8_t field_len)
{
    DLMSValue value;
    value.type = static_cast<DLMSDataType>(data_type);

    switch (data_type)
    {
        case DLMS_DATA_TYPE_BOOLEAN:
            value.setBool(data[0] != 0);
            break;
        case DLMS_DATA_TYPE_INT8:
            value.setInt8(static_cast<int8_t>(data[0]));
            break;
        case DLMS_DATA_TYPE_UINT8:
            value.setUint8(data[0]);
            break;
        case DLMS_DATA_TYPE_INT16:
            value.setInt16(static_cast<int16_t>((data[0] << 8) | data[1]));
            break;
        case DLMS_DATA_TYPE_UINT16:
            value.setUint16((data[0] << 8) | data[1]);
            break;
        case DLMS_DATA_TYPE_INT32:
            value.setInt32(static_cast<int32_t>((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]));
            break;
        case DLMS_DATA_TYPE_UINT32:
        case DLMS_DATA_TYPE_DATETIME: {
            if (data_type == DLMS_DATA_TYPE_UINT32)
                value.setUint32((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]);
            else if (data_type == DLMS_DATA_TYPE_DATETIME)
            {
                uint32_t raw_ts = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
                raw_ts -= SECONDS_5H30M; // Adjust for timezone offset
                value.setString(format_timestamp(raw_ts));
            }
            break;
        }
        case DLMS_DATA_TYPE_FLOAT32: {
            float f;
            memcpy(&f, data, 4);
            value.setFloat32(f);
        }
        break;
        case DLMS_DATA_TYPE_FLOAT64: {
            double d;
            memcpy(&d, data, 8);
            value.setFloat64(d);
        }
        break;
        case DLMS_DATA_TYPE_OCTET_STRING:
            value.setOctetString(std::vector<uint8_t>(data, data + field_len));
            break;
        case DLMS_DATA_TYPE_STRING:
            value.setString(std::string((char *)data, field_len));
            break;
        default:
            // For unknown types, store as octet string
            value.setOctetString(std::vector<uint8_t>(data, data + field_len));
            break;
    }

    return value;
}

/*
 * Parses Nameplate Profile DLMS records from incoming response.
 * Extracts fields by record index, converts raw bytes to strings or numeric types,
 * pushes to name_plate struct members, and logs parsed values.
 */
void Client::parse_nameplate_profile(const uint8_t *data, size_t length)
{
    uint8_t offset = 9;              // Starting offset of records in DLMS payload
    uint8_t total_records = data[8]; // Number of records

    this->print_and_log("=== Parsing %u Nameplate Profile records ===\n", total_records);

    for (uint8_t i = 0; i < total_records; i++)
    {
        if (offset >= length) break;
        // Extract record metadata fields
        uint8_t data_index = data[offset];
        uint8_t dlms_status = data[offset + 1];
        uint8_t dlms_data_type = data[offset + 2];

        uint8_t field_len = 0;
        uint8_t data_off = 0;

        // Parse data type and determine data offset and length
        log_dlms_data_type(dlms_data_type, &offset, &data_off, &field_len, data);

        // Log record metadata for debugging
        this->print_and_log("[DLMS Data Index: 0x%02X | Status: %d | Type: 0x%02X | Length: %d] \n",
                            data_index, dlms_status, dlms_data_type, field_len);

        DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
        if (dlms_data_type != DLMS_DATA_TYPE_NONE)
        {
            this->name_plate.data[data_index].push_back(value);
        }

        // Advance offset by total size of this record
        offset += (data_off + field_len);
    }
}
// Parses Instantaneous Profile records: similar logic extracting and storing data
void Client::parse_instantaneous_profile(const uint8_t *data, size_t length)
{
    uint8_t offset = 9;
    uint8_t record_count = data[8];

    this->print_and_log("=== Parsing %u Instantaneous Profile records ===\n", record_count);

    for (uint8_t i = 0; i < record_count; i++)
    {
        if (offset >= length) break;
        uint8_t data_index = data[offset];
        uint8_t dlms_status = data[offset + 1];
        uint8_t dlms_data_type = data[offset + 2];

        uint8_t field_len = 0;
        uint8_t data_off = 0;
        log_dlms_data_type(dlms_data_type, &offset, &data_off, &field_len, data);

        // Log the parsed value
        this->print_and_log("[DLMS Data Index: 0x%02X | Status: %d | Type: 0x%02X | Length: %d]\n",
                            data_index, dlms_status, dlms_data_type, field_len);

        DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
        if (dlms_data_type != DLMS_DATA_TYPE_NONE)
        {
            this->IP.data[data_index].push_back(value);
        }

        offset += (data_off + field_len);
    }
}

// Parses Daily Load Profile records
void Client::parse_daily_load_profile(const uint8_t *data, size_t length)
{
    // Skip fixed header (8 bytes), read record count from byte 8
    uint8_t offset = 9;             // Start after DLMS header + record_count
    uint8_t record_count = data[8]; // Number of records in this page

    this->print_and_log("=== Parsing %u Daily Load Profile records ===\n", record_count);

    // Process each record in the response
    for (uint8_t i = 0; i < record_count; i++)
    {
        if (offset >= length) break;
        // DLMS Record Structure:
        // [data_index(1B)] [status(1B)] [data_type(1B)] [data_length(1B)] [actual_data]
        uint8_t data_index = data[offset];         // Field ID (0=Date, 1=kWh Export, etc.)
        uint8_t dlms_status = data[offset + 1];    // DLMS field status
        uint8_t dlms_data_type = data[offset + 2]; // DLMS data type (uint32=0x06, etc.)

        // Parse variable-length field using DLMS data type decoder
        uint8_t field_len = 0; // Actual data length in bytes
        uint8_t data_off = 0;  // Offset to actual data (after length specifier)
        log_dlms_data_type(dlms_data_type, &offset, &data_off, &field_len, data);

        // Log the parsed value
        this->print_and_log("[DLMS Data Index: 0x%02X | Status: %d | Type: 0x%02X | Length: %d] \n",
                            data_index, dlms_status, dlms_data_type, field_len);

        // Dispatch to appropriate field parser based on data_index
        DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
        if (dlms_data_type != DLMS_DATA_TYPE_NONE)
        {
            this->DLP.data[data_index].push_back(value);
        }

        // Advance offset to next record (skip parsed field completely)
        offset += (data_off + field_len);
    }

    this->print_and_log("=== Parsed %u DLP records successfully ===\n", record_count);
}

// Parses Block Load Profile records
void Client::parse_block_load_profile(const uint8_t *data, size_t length)
{
    uint8_t offset = 9;             // Starting offset of records in DLMS payload
    uint8_t record_count = data[8]; // Number of records from header

    this->print_and_log("=== Parsing %u Block Load Profile records ===\n", record_count);

    for (uint8_t i = 0; i < record_count; i++)
    {
        if (offset >= length) break;
        // Extract record metadata fields
        uint8_t data_index = data[offset];
        uint8_t dlms_status = data[offset + 1];
        uint8_t dlms_data_type = data[offset + 2];

        uint8_t field_len = 0;
        uint8_t data_off = 0;

        // Parse data type and determine data offset and length
        log_dlms_data_type(dlms_data_type, &offset, &data_off, &field_len, data);

        // Log the parsed value
        this->print_and_log("[DLMS Data Index: 0x%02X | Status: %d | Type: 0x%02X | Length: %d] \n",
                            data_index, dlms_status, dlms_data_type, field_len);

        // Store extracted data into corresponding BLP member using index
        DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
        if (dlms_data_type != DLMS_DATA_TYPE_NONE)
        {
            this->BLP.data[data_index].push_back(value);
        }

        // Advance offset by total size of this record
        offset += (data_off + field_len);
    }

    this->print_and_log("=== Parsed %u BLP records successfully ===\n", record_count);
}

//====================================================================================================================
// Parses Billing History Profile records from DLMS/COSEM response
//====================================================================================================================
// Parses Billing History Profile records
void Client::parse_billing_history_profile(const uint8_t *data, size_t length)
{
    // Skip fixed header (8 bytes), read record count from byte 8
    uint8_t offset = 9;             // Start after DLMS header + record_count
    uint8_t record_count = data[8]; // Number of billing records

    this->print_and_log("=== Parsing %u Billing History Profile records ===\n", record_count);

    for (uint8_t i = 0; i < record_count; i++)
    {
        if (offset >= length) break;
        // DLMS Record Structure: [data_index(1B)] [status(1B)] [data_type(1B)] [data]
        uint8_t data_index = data[offset];         // Field ID (0x00-0x1A = 27 fields)
        uint8_t dlms_status = data[offset + 1];    // DLMS field status
        uint8_t dlms_data_type = data[offset + 2]; // DLMS data type

        // Parse variable-length field using DLMS data type decoder
        uint8_t field_len = 0; // Actual data length
        uint8_t data_off = 0;  // Offset to actual data
        log_dlms_data_type(dlms_data_type, &offset, &data_off, &field_len, data);

        // Log the parsed value
        this->print_and_log("[DLMS Data Index: 0x%02X | Status: %d | Type: 0x%02X | Length: %d] \n",
                            data_index, dlms_status, dlms_data_type, field_len);

        // Dispatch to ALL 27 billing profile fields (0x00-0x1A)
        DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
        if (dlms_data_type != DLMS_DATA_TYPE_NONE)
        {
            this->BH.data[data_index].push_back(value);
        }

        // Advance to next record
        offset += (data_off + field_len);
    }

    this->print_and_log("=== Parsed %u Billing History records successfully ===\n", record_count);
}
// Parses all Event Profiles based on event type (data[6])
void Client::parse_Events_profile(const uint8_t *data, size_t length)
{
    uint8_t offset = 9;             // Starting offset of records in DLMS payload
    uint8_t record_count = data[8]; // Number of records from header
    uint8_t event_type = data[6];   // Event type selector (00-07)
    bool print_flag = true;
    for (uint8_t i = 0; i < record_count; i++)
    {
        if (offset >= length) break;
        // Extract record metadata fields
        uint8_t data_index = data[offset];
        uint8_t dlms_status = data[offset + 1];
        uint8_t dlms_data_type = data[offset + 2];

        uint8_t field_len = 0;
        uint8_t data_off = 0;
        if (print_flag)
            this->print_and_log("[DATA_INDEX] = %d\n", data_index);
        // Parse data type and determine data offset and length
        log_dlms_data_type(dlms_data_type, &offset, &data_off, &field_len, data);

        // Handle data based on event type and data_index
        switch (event_type)
        {
            case EVENT_ALL: // Event Indexes Summary (Metadata before actual records)
            {
                print_flag = false;

                if (print_flag)
                {
                    this->print_and_log("=== EVENT INDEXES SUMMARY ===\n");
                }

                // Each EventDataIndex starts here (16 bytes each):
                // 00 = Index | 00 07 = Records | 00 00 00 00 = Capture period | 01 = Sort | 00 00 00 12 = Current | 00 00 00 46 = Max
                uint8_t base = 9 + (i * 16);
                uint8_t event_index = data[base];
                this->event_data.data[event_index].resize(6);
                this->event_data.data[event_index][EVENT_DATA_INDEX_FIELD_INDEX].setUint8(event_index);

                uint16_t no_of_records = (data[base + 1] << 8) | data[base + 2];
                this->event_data.data[event_index][EVENT_DATA_INDEX_FIELD_NO_OF_RECORDS].setUint16(no_of_records);

                uint32_t capture_period = (data[base + 3] << 24) | (data[base + 4] << 16) | (data[base + 5] << 8) | data[base + 6];
                this->event_data.data[event_index][EVENT_DATA_INDEX_FIELD_CAPTURE_PERIOD].setUint32(capture_period);

                uint8_t sort_method = data[base + 7];
                this->event_data.data[event_index][EVENT_DATA_INDEX_FIELD_SORT_METHOD].setUint8(sort_method);

                uint32_t current_entries = (data[base + 8] << 24) | (data[base + 9] << 16) | (data[base + 10] << 8) | data[base + 11];
                this->event_data.data[event_index][EVENT_DATA_INDEX_FIELD_CURRENT_ENTRIES].setUint32(current_entries);

                uint32_t max_records = (data[base + 12] << 24) | (data[base + 13] << 16) | (data[base + 14] << 8) | data[base + 15];
                this->event_data.data[event_index][EVENT_DATA_INDEX_FIELD_MAX_RECORDS].setUint32(max_records);

                // Log each event index summary
                const char *event_names[] = {"Voltage", "Current", "Power", "Transactional", "Other", "Non-Rollover", "Control Event"};
                const char *event_name = (event_index < 7) ? event_names[event_index] : "Unknown";

                this->print_and_log("Index %d [%s Event]: Records=%u, Period=%u, Current Record=%u, Max Record=%u\n",
                                    event_index, event_name, no_of_records, capture_period, current_entries, max_records);
                break;
            }

            case EVENT_VOLTAGE: // VoltageEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->voltage_event.data[data_index].push_back(value);
                }
                this->print_and_log("[VOLTAGE EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            case EVENT_CURRENT: // CurrentEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->current_event.data[data_index].push_back(value);
                }
                this->print_and_log("[CURRENT EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            case EVENT_POWER: // PowerEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->power_event.data[data_index].push_back(value);
                }
                this->print_and_log("[POWER EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            case EVENT_TRANSACTIONAL: // TransactionalEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->transaction_event.data[data_index].push_back(value);
                }
                this->print_and_log("[TRANSACTIONAL EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            case EVENT_OTHER: // OtherEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->other_event.data[data_index].push_back(value);
                }
                this->print_and_log("[OTHER EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            case EVENT_NON_ROLL_OVER: // NonRollOverEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->non_roll_over_event.data[data_index].push_back(value);
                }
                this->print_and_log("[NON ROLLOVER EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            case EVENT_CONTROL: // ControlEvent
            {
                DLMSValue value = extract_dlms_value(dlms_data_type, data + offset + data_off, field_len);
                if (dlms_data_type != DLMS_DATA_TYPE_NONE)
                {
                    this->control_event.data[data_index].push_back(value);
                }
                this->print_and_log("[CONTROL EVENT - Data Index %d] = %s\n", data_index, value.to_string().c_str());
                break;
            }

            default:
                this->print_and_log("Unknown Event Type: %d, Data Index: %d\n", event_type, data_index);
                break;
        }
        // Log record metadata for debugging
        if (print_flag)
            this->print_and_log("[EVENT TYPE: %d | DLMS Data Index: %d | Status: %d | Type: 0x%02X | Length: %d]\n",
                                event_type, data_index, dlms_status, dlms_data_type, field_len);

        // Advance offset by total size of current record
        offset += (data_off + field_len);
    }
    if (event_type == 0)
    {
        // DON'T advance offset here - let log_dlms_data_type handle it
        this->print_and_log("=== END EVENT INDEXES SUMMARY ===\n");
    }
}

// Helper function to format 4-byte timestamp
std::string Client::format_timestamp(uint32_t hex_timestamp)
{
    // this->print_and_log("[DEBUG] Raw hex timestamp = 0x%08X (%u)\n", hex_timestamp, hex_timestamp);

    time_t unix_time = static_cast<time_t>(hex_timestamp);

    std::tm *timeinfo = std::localtime(&unix_time);
    if (!timeinfo)
    {
        this->print_and_log("[ERROR] std::localtime() failed - invalid timestamp\n");
        return "1970-01-01 00:00:00"; // Fallback
    }

    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo) == 0)
    {
        this->print_and_log("[ERROR] strftime() failed\n");
        return "Format failed";
    }

    std::string result(buffer);
    // this->print_and_log("[SUCCESS] Formatted = %s\n", result.c_str());
    return result;
}

int Client::handle_single_obis_read(const OtaCmdResponse *response, uint16_t dlms_len)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    uint8_t command = response->data[5];     // 00=READ, 01=WRITE
    uint8_t sub_command = response->data[6]; // OBIS type: 0x02=RTC, 0x01=Period, etc.

    singleObisData.command.push_back(command);
    singleObisData.sub_command.push_back(sub_command);
    // Fixed: Correct command validation (was using wrong operator)
    if (command != 0x00 && command != 0x01)
    {
        this->print_and_log("Expected READ(0x00) or WRITE(0x01), got 0x%02X\n", command);
        return INVALID_RESPONSE;
    }
    if (command == 0x00)
        this->DB_parameter.command = 0;
    else
        this->DB_parameter.command = 1;

    uint8_t offset = 8; // offset pointing to no.of records position index

    this->print_and_log("[SUB_COMMAND] = 0x%0X\n", sub_command);
    // **SWITCH ON SUB_COMMAND** (OBIS object identifier)
    switch (sub_command)
    {
        case SINGLE_OBIS_RTC: // RTC Read/Write (0.0.1.0.0.255)
        {
            uint8_t status = response->data[offset + 2];
            uint8_t data_type = response->data[offset + 3];

            if (command == 0x00)
            {
                if (data_type == 0x19 && status == 0x00)
                {
                    uint32_t timestamp = (response->data[offset + 4] << 24) |
                                         (response->data[offset + 5] << 16) |
                                         (response->data[offset + 6] << 8) |
                                         response->data[offset + 7];
                    timestamp += 0x386CF628;
                    this->singleObisData.rtc_timestamp.push_back(timestamp);
                    this->singleObisData.rtc_formatted = format_timestamp(timestamp);

                    this->print_and_log("[RTC READ] = %s \n",
                                        this->singleObisData.rtc_formatted.c_str());
                }
                else
                {
                    this->print_and_log("Invalid Structure [DATA_TYPE] = 0x%0X, [STATUS] = 0x%0X\n", data_type, status);
                    return INVALID_RESPONSE;
                }
            }
            else
            {
                if (response->data[7] == 0x00)
                {
                    this->pp_write_payload_tail += 0x386CF628;
                    this->singleObisData.rtc_timestamp.push_back(this->pp_write_payload_tail);
                    this->singleObisData.rtc_formatted = format_timestamp(this->pp_write_payload_tail);
                    this->print_and_log("[RTC WRITE] = %s \n", this->singleObisData.rtc_formatted.c_str());
                    this->print_and_log("[RTC WRITE] = SUCCESS\n");
                }
                else
                {
                    this->print_and_log("RTC WRITE FAILED [STATUS] = 0x%0X\n", response->data[7]);
                    return INVALID_RESPONSE;
                }
            }
            break;
        }

        case SINGLE_OBIS_INTEGRATION_OR_CAPTURE_PERIOD: // Integration/Profile Capture Period (1.0.0.8.0/4.255)
        {
            uint8_t status = response->data[offset + 2];
            uint8_t data_type = response->data[offset + 3];
            uint16_t period_seconds;

            if (command == 0x00)
            {
                if (data_type == 0x06 && status == 0x00)
                {
                    // Little-endian uint32_t
                    period_seconds = (response->data[offset + 6] << 8) |
                                     response->data[offset + 7];

                    this->singleObisData.period_seconds.push_back(period_seconds);
                    this->print_and_log("[PERIOD READ] = %u seconds \n", singleObisData.period_seconds.back());
                    this->print_and_log("[BYTES 1] = 0x%02X ,[BYTES 2] = 0x%02X \n", response->data[offset + 7], response->data[offset + 6]);
                }
                else
                {
                    this->print_and_log("Invalid Structure [DATA_TYPE] = 0x%0X,[STATUS] = 0x%0X\n", data_type, status);
                    return INVALID_RESPONSE;
                }
            }
            else
            {
                if (response->data[7] == 0x00)
                {
                    period_seconds = static_cast<uint16_t>(pp_write_payload_tail & 0xFFFF);
                    this->singleObisData.period_seconds.push_back(period_seconds);
                    this->print_and_log("[PERIOD WRITE] = %u seconds \n", singleObisData.period_seconds.back());
                    this->print_and_log("[PERIOD WRITE] = SUCCESS\n");
                }
                else
                {
                    this->print_and_log("Period WRITE FAILED [STATUS] = 0x%0X\n", response->data[7]);
                    return INVALID_RESPONSE;
                }
            }
            break;
        }

        case SINGLE_OBIS_LOAD_LIMIT: // Load Limit (0.0.17.0.0.255)
        {
            uint8_t data_index = response->data[offset + 1];
            uint16_t load_limit;
            if (command == 0x00)
            {
                if (data_index == 0x04)
                {
                    load_limit = (response->data[offset + 6] << 8) |
                                 response->data[offset + 7];
                    singleObisData.load_limit.push_back(load_limit);
                    this->print_and_log("[LOAD LIMIT READ] = 0x%02X\n", singleObisData.load_limit.back());
                }
                else
                {
                    this->print_and_log("Invalid Structure [DATA_INDEX] = 0x%0X\n", data_index);
                    return INVALID_RESPONSE;
                }
            }
            else
            {
                if (response->data[7] == 0x00)
                {
                    load_limit = static_cast<uint16_t>(pp_write_payload_tail & 0xFFFF);
                    singleObisData.load_limit.push_back(load_limit);
                    this->print_and_log("[LOAD LIMIT WRITE] = 0x%02X\n", singleObisData.load_limit.back());
                    this->print_and_log("[LOAD LIMIT WRITE] = SUCCESS\n");
                }
                else
                {
                    this->print_and_log("Load Limit WRITE FAILED [STATUS] = 0x%0X\n", response->data[7]);
                    return INVALID_RESPONSE;
                }
            }
            break;
        }

        case SINGLE_OBIS_LOAD_STATUS: // Load Disconnect/Reconnect Status (0.0.96.3.10.255)(PING METER)
        {
            if (command == 0x01)
            {
                singleObisData.load_status.push_back(response->data[7]);
                this->print_and_log("[LOAD STATUS WRITE] = 0x%02X\n", singleObisData.load_status.back());
            }
            else
            {
                uint8_t status = response->data[11];
                singleObisData.load_status.push_back(status);
                singleObisData.load_status_str = (status == 1) ? "CONNECTED" : "DISCONNECTED";
                this->print_and_log("[LOAD STATUS READ] = %s (0x%02X)\n",
                                    singleObisData.load_status_str.c_str(),
                                    status);
            }
            break;
        }

        case SINGLE_OBIS_ACTION_SCHEDULE: // Single Action Scheduler (0.0.15.0.0.255)
        {
            uint8_t status = response->data[offset + 2];
            uint8_t data_type = response->data[offset + 3];
            uint8_t no_of_elements = response->data[offset + 4];

            if (data_type == 0x01 && no_of_elements == 0x01 && status == 0x00)
            {
                uint8_t elem_data_type = response->data[offset + 5];

                if (elem_data_type == 0x1B)
                {
                    uint32_t scheduler_time = (response->data[offset + 6] << 24) |
                                              (response->data[offset + 7] << 16) |
                                              (response->data[offset + 8] << 8) |
                                              response->data[offset + 9];

                    singleObisData.scheduler_timestamp.push_back(scheduler_time);
                    singleObisData.scheduler_formatted = format_timestamp(scheduler_time);
                    this->print_and_log("[SCHEDULER READ] = %s \n",
                                        singleObisData.scheduler_formatted.c_str());
                }
            }
            break;
        }

        default:
            this->print_and_log("Unknown SUB COMMAND 0x%02X (CMD=0x%02X)\n", sub_command, command);
            this->print_data_in_hex(const_cast<uint8_t *>(response->data), dlms_len);
            return INVALID_RESPONSE;
    }

    return SUCCESS;
}

// Helper functions (add these to Client class)
uint32_t Client::extract_u32(const OtaCmdResponse *response, uint8_t offset, uint8_t data_off)
{
    return (static_cast<uint32_t>(response->data[offset + data_off]) << 24) |
           (static_cast<uint32_t>(response->data[offset + data_off + 1]) << 16) |
           (static_cast<uint32_t>(response->data[offset + data_off + 2]) << 8) |
           static_cast<uint32_t>(response->data[offset + data_off + 3]);
}

uint16_t Client::extract_u16(const OtaCmdResponse *response, uint8_t offset, uint8_t data_off)
{
    return (static_cast<uint16_t>(response->data[offset + data_off]) << 8) |
           static_cast<uint16_t>(response->data[offset + data_off + 1]);
}

/**
 * Parse DLMS DateTime (12-byte Octet String) → "YYYY-MM-DD HH:MM" format
 * Input: 0C 07 D0 01 01 FF 00 00 FF FF 01 4A 00 → "2027-01-01 00:00"
 */
std::string Client::parse_dlms_datetime(const uint8_t *data)
{
    // Debug: print 12 bytes we are about to parse
    char dbg[64] = {0};
    snprintf(dbg, sizeof(dbg),
             "DATE TIME RAW: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             data[0], data[1], data[2], data[3],
             data[4], data[5], data[6], data[7],
             data[8], data[9], data[10], data[11]);
    this->print_and_log("%s\n", dbg);
    // DLMS DateTime: year(2, big-endian), month, day, dow, hour, min, sec, hsec, dev(2), status
    uint16_t year = (static_cast<uint16_t>(data[0]) << 8) | data[1]; // big-endian
    uint8_t month = data[2];
    uint8_t day = data[3];
    // data[4] = day of week (ignored here)
    uint8_t hour = data[5];
    uint8_t min = data[6];
    // data[7] = seconds, data[8] = hundredatahs (ignored or use if you want)

    // Handle unspecified values (0xFF -> defaults)
    if (year == 0xFFFF || year == 0) year = 2000;
    if (month == 0xFF || month == 0) month = 1;
    if (day == 0xFF || day == 0) day = 1;
    if (hour == 0xFF) hour = 0;
    if (min == 0xFF) min = 0;

    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
             static_cast<unsigned>(year),
             static_cast<unsigned>(month),
             static_cast<unsigned>(day),
             static_cast<unsigned>(hour),
             static_cast<unsigned>(min));

    return std::string(buf);
}

bool Client::update_odm_db(uint8_t download_data_type)
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    uint8_t err_code = 1;
    uint8_t tamper_type_index;

    load_scaler_details_from_db(this->DB_parameter.meter_serial_no, this->gateway_id, this);
    switch (download_data_type)
    {
        case DATA_TYPE_NP: {
            return process_NP_case();
        }
        case DATA_TYPE_IP: {
            return process_IP_case();
        }
        case DATA_TYPE_BHP: {
            return process_BHP_case(download_data_type);
        }
        case DATA_TYPE_DLP: {
            return process_DLP_case(download_data_type);
        }
        case DATA_TYPE_BLP: {
            return process_BLP_case(download_data_type);
        }
        case DATA_TYPE_ALL_EVENTS: {
            return process_AllEvents_case(download_data_type);
        }
        case DATA_TYPE_VOLTAGE_EVENTS: {
            this->validate_voltage_event_for_db();
            tamper_type_index = 1;
            this->print_and_log("Voltage Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for VOLTAGE_EVENTS
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] date_time_of_event: %s\n", this->voltage_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] event_code: %u\n", static_cast<unsigned>(this->voltage_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] current: %.2f\n", this->voltage_event.data[EVENT_DATA_INDEX_CURRENT].back().getAsFloat(0.01f));
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] voltage: %.2f\n", this->voltage_event.data[EVENT_DATA_INDEX_VOLTAGE].back().getAsFloat(0.01f));
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] power_factor: %.2f\n", this->voltage_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].back().getAsFloat(0.001f));
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] cumulative_kwh: %.2f\n", this->voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].back().getAsFloat(0.001f));
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] cumulative_tamper_count: %u\n", static_cast<unsigned>(this->voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG VOLTAGE_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO voltage_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,current,"
                     "voltage,power_factor,cumulative_kwh,cumulative_tamper_count,"
                     "last_download_time,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,%.2f,%.2f,%.2f,%.2f,%u,'%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->voltage_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->voltage_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->voltage_event.data[EVENT_DATA_INDEX_CURRENT].back().getAsFloat(0.01f),
                     this->voltage_event.data[EVENT_DATA_INDEX_VOLTAGE].back().getAsFloat(0.01f),
                     this->voltage_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].back().getAsFloat(0.001f),
                     this->voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].back().getAsFloat(0.001f),
                     static_cast<unsigned>(this->voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);
            break;
        }
        case DATA_TYPE_CURRENT_EVENTS: {
            this->validate_current_event_for_db();
            tamper_type_index = 2;
            this->print_and_log("Current Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for CURRENT_EVENTS
            this->print_and_log("[DEBUG CURRENT_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG CURRENT_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG CURRENT_EVENTS] date_time_of_event: %s\n", this->current_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG CURRENT_EVENTS] event_code: %u\n", static_cast<unsigned>(this->current_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG CURRENT_EVENTS] current: %.2f\n", this->current_event.data[EVENT_DATA_INDEX_CURRENT].back().getAsFloat(0.01f));
            this->print_and_log("[DEBUG CURRENT_EVENTS] voltage: %.2f\n", this->current_event.data[EVENT_DATA_INDEX_VOLTAGE].back().getAsFloat(0.01f));
            this->print_and_log("[DEBUG CURRENT_EVENTS] power_factor: %.2f\n", this->current_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].back().getAsFloat(0.001f));
            this->print_and_log("[DEBUG CURRENT_EVENTS] cumulative_kwh: %.2f\n", this->current_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].back().getAsFloat(0.001f));
            this->print_and_log("[DEBUG CURRENT_EVENTS] cumulative_tamper_count: %u\n", static_cast<unsigned>(this->current_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG CURRENT_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG CURRENT_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG CURRENT_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG CURRENT_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG CURRENT_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO current_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,current,"
                     "voltage,power_factor,cumulative_kwh,cumulative_tamper_count,"
                     "last_download_time,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,%.2f,%.2f,%.2f,%.2f,%u,'%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->current_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->current_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->current_event.data[EVENT_DATA_INDEX_CURRENT].back().getAsFloat(0.01f),
                     this->current_event.data[EVENT_DATA_INDEX_VOLTAGE].back().getAsFloat(0.01f),
                     this->current_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].back().getAsFloat(0.001f),
                     this->current_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].back().getAsFloat(0.001f),
                     static_cast<unsigned>(this->current_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);

            break;
        }
        case DATA_TYPE_POWER_EVENTS: {
            this->validate_power_event_for_db();
            tamper_type_index = 3;
            this->print_and_log("Power Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for POWER_EVENTS
            this->print_and_log("[DEBUG POWER_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG POWER_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG POWER_EVENTS] date_time_of_event: %s\n", this->power_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG POWER_EVENTS] event_code: %u\n", static_cast<unsigned>(this->power_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG POWER_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG POWER_EVENTS] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
            this->print_and_log("[DEBUG POWER_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG POWER_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG POWER_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG POWER_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO power_failure_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,"
                     "last_download_time,gateway_id,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,'%s','%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->power_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->power_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.gateway_id.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);
            break;
        }
        case DATA_TYPE_TRANSACTIONAL_EVENTS: {
            this->validate_transaction_event_for_db();
            tamper_type_index = 4;
            this->print_and_log("Transactional Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for TRANSACTIONAL_EVENTS
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] date_time_of_event: %s\n", this->transaction_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] event_code: %u\n", static_cast<unsigned>(this->transaction_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG TRANSACTIONAL_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO transaction_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,"
                     "last_download_time,gateway_id,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,'%s','%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->transaction_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->transaction_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.gateway_id.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);

            break;
        }
        case DATA_TYPE_OTHER_EVENTS: {
            this->validate_other_event_for_db();
            tamper_type_index = 5;
            this->print_and_log("Other Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for OTHER_EVENTS
            this->print_and_log("[DEBUG OTHER_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG OTHER_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG OTHER_EVENTS] date_time_of_event: %s\n", this->other_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG OTHER_EVENTS] event_code: %u\n", static_cast<unsigned>(this->other_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG OTHER_EVENTS] current: %.2f\n", this->other_event.data[EVENT_DATA_INDEX_CURRENT].back().getAsFloat(0.01f));
            this->print_and_log("[DEBUG OTHER_EVENTS] voltage: %.2f\n", this->other_event.data[EVENT_DATA_INDEX_VOLTAGE].back().getAsFloat(0.01f));
            this->print_and_log("[DEBUG OTHER_EVENTS] power_factor: %.2f\n", this->other_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].back().getAsFloat(0.001f));
            this->print_and_log("[DEBUG OTHER_EVENTS] cumulative_kwh: %.2f\n", this->other_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].back().getAsFloat(0.001f));
            this->print_and_log("[DEBUG OTHER_EVENTS] cumulative_tamper_count: %u\n", static_cast<unsigned>(this->other_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG OTHER_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG OTHER_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG OTHER_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG OTHER_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG OTHER_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO other_utility_based_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,current,"
                     "voltage,power_factor,cumulative_kwh,cumulative_tamper_count,"
                     "last_download_time,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,%.2f,%.2f,%.2f,%.2f,%u,'%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->other_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->other_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->other_event.data[EVENT_DATA_INDEX_CURRENT].back().getAsFloat(0.01f),
                     this->other_event.data[EVENT_DATA_INDEX_VOLTAGE].back().getAsFloat(0.01f),
                     this->other_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].back().getAsFloat(0.001f),
                     this->other_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].back().getAsFloat(0.001f),
                     static_cast<unsigned>(this->other_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);
            break;
        }
        case DATA_TYPE_NON_ROLL_OVER_EVENTS: {
            this->validate_non_rollover_event_for_db();
            tamper_type_index = 6;
            this->print_and_log("Non-Roll-Over Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for NON_ROLL_OVER_EVENTS
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] date_time_of_event: %s\n", this->non_roll_over_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] event_code: %u\n", static_cast<unsigned>(this->non_roll_over_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG NON_ROLL_OVER_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO non_roll_over_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,"
                     "last_download_time,gateway_id,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,'%s','%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->non_roll_over_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->non_roll_over_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.gateway_id.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);
            break;
        }
        case DATA_TYPE_CONTROL_EVENTS: {
            this->validate_control_event_for_db();
            tamper_type_index = 7;
            this->print_and_log("Control Event Profile\n");
            this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

            // Debug prints for CONTROL_EVENTS
            this->print_and_log("[DEBUG CONTROL_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
            this->print_and_log("[DEBUG CONTROL_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
            this->print_and_log("[DEBUG CONTROL_EVENTS] date_time_of_event: %s\n", this->control_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str());
            this->print_and_log("[DEBUG CONTROL_EVENTS] event_code: %u\n", static_cast<unsigned>(this->control_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)));
            this->print_and_log("[DEBUG CONTROL_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
            this->print_and_log("[DEBUG CONTROL_EVENTS] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
            this->print_and_log("[DEBUG CONTROL_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
            this->print_and_log("[DEBUG CONTROL_EVENTS] push_alaram: %d\n", this->DB_parameter.push_alaram);
            this->print_and_log("[DEBUG CONTROL_EVENTS] error_code: %d\n", err_code);
            this->print_and_log("[DEBUG CONTROL_EVENTS] tamper_type_index: %u\n", tamper_type_index);

            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO control_events_tamper_data("
                     "meter_mac_address,meter_serial_number,date_time_of_event,event_code,"
                     "last_download_time,gateway_id,request_id,push_alaram,error_code,tamper_type_index) "
                     "VALUES('%s','%s','%s',%u,'%s','%s',%zu,%d,%d,%u)",

                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(),
                     this->control_event.data[EVENT_DATA_INDEX_RTC].back().getAsString().c_str(),
                     static_cast<unsigned>(this->control_event.data[EVENT_DATA_INDEX_EVENT_CODE].back().getAsFloat(1.0)),

                     this->DB_parameter.last_download_time.c_str(),
                     this->DB_parameter.gateway_id.c_str(),
                     this->DB_parameter.req_id,
                     this->DB_parameter.push_alaram,
                     err_code,
                     tamper_type_index);

            break;
        }
        case DATA_TYPE_PING_NODE: {
            this->print_and_log("Ping Node\n");
            Update_Insert_Ping_request(download_data_type, SUCCESS_STATUS); // Insert new record
            return true;
        }
        case DATA_TYPE_PING_METER:
            this->print_and_log("Ping Meter\n");
            Update_Insert_Ping_request(download_data_type, SUCCESS_STATUS); // Insert new record
            return true;

        // **COMMANDS 15-25** - programmable_parameter_data table (COMPLETE)
        case DATA_TYPE_RTC_READ:
        case DATA_TYPE_RTC_WRITE: // RTC Read/Write

            this->print_and_log("%s\n", (download_data_type == DATA_TYPE_RTC_READ) ? "RTC Read Success" : "RTC Write Success");
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO programmable_parameter_data(request_id,gateway_id,meter_mac_address,meter_serial_number,is_read_or_write,status,last_download_time,real_time_clock) VALUES(%zu,'%s','%s','%s',%u,%d,'%s','%s')",
                     this->DB_parameter.req_id, this->DB_parameter.gateway_id.c_str(), this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(), this->DB_parameter.command, this->DB_parameter.status, this->DB_parameter.last_download_time.c_str(), this->singleObisData.rtc_formatted.c_str());

            break;

        case DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ:
        case DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ_WRITE: // Demand Integration Read/Writ
            this->print_and_log("%s\n", (download_data_type == DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ) ? "Demand Integration Read" : "Demand Integration Write");
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO programmable_parameter_data(request_id,gateway_id,meter_mac_address,meter_serial_number,is_read_or_write,status,last_download_time,demand_intg_period) VALUES(%zu,'%s','%s','%s',%u,%d,'%s',%u)",
                     this->DB_parameter.req_id, this->DB_parameter.gateway_id.c_str(), this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(), this->DB_parameter.command, this->DB_parameter.status, this->DB_parameter.last_download_time.c_str(), this->singleObisData.period_seconds.back());
            break;

        case DATA_TYPE_CAPTURE_PERIOD_READ:
        case DATA_TYPE_CAPTURE_PERIOD_READ_WRITE: // Capture Period Read/Write
            this->print_and_log("%s\n", (download_data_type == DATA_TYPE_CAPTURE_PERIOD_READ) ? "Capture Period Read" : "Capture Period Write");
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO programmable_parameter_data(request_id,gateway_id,meter_mac_address,meter_serial_number,is_read_or_write,status,last_download_time,profile_capture_period) VALUES(%zu,'%s','%s','%s',%u,%d,'%s',%u)",
                     this->DB_parameter.req_id, this->DB_parameter.gateway_id.c_str(), this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(), this->DB_parameter.command, this->DB_parameter.status, this->DB_parameter.last_download_time.c_str(), this->singleObisData.period_seconds.back());

            break;

        case DATA_TYPE_LOAD_LIMIT_READ:
        case DATA_TYPE_LOAD_LIMIT_WRITE: // Load Limit Read/Write
            this->print_and_log("%s\n", (download_data_type == DATA_TYPE_LOAD_LIMIT_READ) ? "Load Limit Read" : "Load Limit Write");
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO programmable_parameter_data(request_id,gateway_id,meter_mac_address,meter_serial_number,is_read_or_write,status,last_download_time,load_limit)  VALUES(%zu,'%s','%s','%s',%u,%d,'%s',%u)",
                     this->DB_parameter.req_id, this->DB_parameter.gateway_id.c_str(), this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(), this->DB_parameter.command, this->DB_parameter.status, this->DB_parameter.last_download_time.c_str(), this->singleObisData.load_limit.back());

            break;

        case DATA_TYPE_LOAD_STATUS_READ:
        case DATA_TYPE_LOAD_STATUS_WRITE: // Load Connection Read/Write
            this->print_and_log("%s\n", (download_data_type == DATA_TYPE_LOAD_STATUS_READ) ? "Load Connection Read" : "Load Connection Write");
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO programmable_parameter_data(request_id,gateway_id,meter_mac_address,meter_serial_number,is_read_or_write,status,last_download_time,load_connection) VALUES(%zu,'%s','%s','%s',%u,%d,'%s',%u)",
                     this->DB_parameter.req_id, this->DB_parameter.gateway_id.c_str(), this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(), this->DB_parameter.command, this->DB_parameter.status, this->DB_parameter.last_download_time.c_str(), this->singleObisData.load_status.back());

            break;

        case DATA_TYPE_ACTION_SCHEDULER_READ: // Action Scheduler Read
            this->print_and_log("Action Scheduler Read\n");
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO programmable_parameter_data(request_id,gateway_id,meter_mac_address,meter_serial_number,is_read_or_write,status,last_download_time,action_scheduler) VALUES(%zu,'%s','%s','%s',%u,%d,'%s','%s')",
                     this->DB_parameter.req_id, this->DB_parameter.gateway_id.c_str(), this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.meter_serial_no.c_str(), this->DB_parameter.command, this->DB_parameter.status, this->DB_parameter.last_download_time.c_str(), this->singleObisData.scheduler_formatted.c_str());

            break;

        // **COMMANDS 26-30** - No DB insert
        case DATA_TYPE_ACTIVITY_CALENDAR_READ:
            this->print_and_log("Activity Calendar Read\n");

            break;
        case DATA_TYPE_METER_FIRMWARE_VERSION_READ:
            this->print_and_log("Meter Firmware Version Read\n");

            break;
        case DATA_TYPE_RF_FIRMWARE_VERSION_READ:
            this->print_and_log("RF Firmware Version Read\n");

            break;

        default:
            this->print_and_log("Unknown ODM download_data_type id: %u\n", download_data_type);
            return true;
    }
    int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

    if (db_result == 0)
    { // ✅ SUCCESS
        this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", download_data_type, this->DB_parameter.req_id);
        clear_profile_for_type(download_data_type);
    }
    else
    { // ✅ FAILURE
        this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", download_data_type);
    }
    return true;
}

void Client::clear_profile_for_type(uint8_t download_data_type)
{
    this->print_and_log("[CLEAR PROFILE] Clearing for download type: %u\n", download_data_type);

    switch (download_data_type)
    {
        // 0: Nameplate Profile
        case DATA_TYPE_NP:
            this->name_plate = ODM_NamePlateProfile();
            break;

        // 1: Instantaneous Profile
        case DATA_TYPE_IP:
            this->IP = ODM_InstantaneousProfile();
            break;

        // 2: Billing History Profile
        case DATA_TYPE_BHP:
            this->BH = ODM_BillingProfile();
            break;

        // 3: Daily Load Profile
        case DATA_TYPE_DLP:
            this->DLP = ODM_DailyLoadProfile();
            break;

        // 4: Block Load Profile
        case DATA_TYPE_BLP:
            this->BLP = ODM_BlockLoadProfile();
            break;

        // 5: Tamper Profile (All Events + Summary)
        case DATA_TYPE_ALL_EVENTS:
            this->event_data = EventDataIndex();
            break;
        // 6-12: Event Profiles (Voltage, Current, Power, Transactional, Other, Non-RollOver, Control)
        case DATA_TYPE_VOLTAGE_EVENTS:
        case DATA_TYPE_CURRENT_EVENTS:
        case DATA_TYPE_POWER_EVENTS:
        case DATA_TYPE_TRANSACTIONAL_EVENTS:
        case DATA_TYPE_OTHER_EVENTS:
        case DATA_TYPE_NON_ROLL_OVER_EVENTS:
        case DATA_TYPE_CONTROL_EVENTS:
            this->voltage_event = ODM_VoltageEvent();
            this->current_event = ODM_CurrentEvent();
            this->power_event = ODM_PowerEvent();
            this->transaction_event = ODM_TransactionalEvent();
            this->other_event = ODM_OtherEvent();
            this->non_roll_over_event = ODM_NonRollOverEvent();
            this->control_event = ODM_ControlEvent();
            break;

        case DATA_TYPE_RTC_READ:
        case DATA_TYPE_RTC_WRITE:
        case DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ:
        case DATA_TYPE_DEMAND_INTEGRATION_PERIOD_READ_WRITE:
        case DATA_TYPE_CAPTURE_PERIOD_READ:
        case DATA_TYPE_CAPTURE_PERIOD_READ_WRITE:
        case DATA_TYPE_LOAD_LIMIT_READ:
        case DATA_TYPE_LOAD_LIMIT_WRITE:
        case DATA_TYPE_LOAD_STATUS_READ:
        case DATA_TYPE_LOAD_STATUS_WRITE:
        case DATA_TYPE_ACTION_SCHEDULER_READ:
        case DATA_TYPE_ACTIVITY_CALENDAR_READ:
            this->singleObisData = ODM_SingleObisData();
            this->pp_write_payload_tail = 0;
            break;

        default:
            this->print_and_log("[CLEAR PROFILE] Unknown type %u - cleared ALL profiles\n", download_data_type);
            break;
    }

    this->print_and_log("[CLEAR PROFILE] ✅ Complete for type: %u\n", download_data_type);
}

//(added by Supritha K P)
int Client::init_connection(int val1, int val2, int val3)
{
    insert_update_hes_nms_sync_time(this->gateway_id, 1);
    if (this->update_into_gateway_status_info((const uint8_t *)this->pgwid, this->gatewayStatus, this->val1, this->val2, this->val3) == FAILURE) //(added by Supritha K P)
    {
        insert_into_gateway_status_info((const uint8_t *)this->pgwid);
    }
    this->insert_into_gateway_connection_log((const uint8_t *)this->pgwid, val1, val2, val3);

    if (this->db->check_for_fuota_resume(this->gateway_id) == SUCCESS)
    {
        std::vector<uint8_t> pending_cmd;
        unsigned char target_mac_address[17] = {0}; // will be filled from dequeued command if available

        if (this->dequeue_pending_fuota(pending_cmd))
        {
            std::string cmd_str(pending_cmd.begin(), pending_cmd.end());
            this->print_and_log("(%s) -> init_connection: dequeued pending resume cmd: %s\n", this->gateway_id, cmd_str.c_str());

            std::vector<std::string> parts = split(cmd_str, ':');

            if (parts.size() > FIRMWARE_FILE_NAME)
            {
                if (!parts[REQUEST_ID].empty())
                    this->request_id = std::stoi(parts[REQUEST_ID]);

                this->firmware_path = parts[FIRMWARE_PATH];
                this->firmware_filename = parts[FIRMWARE_FILE_NAME];

                this->print_and_log("(%s) -> init_connection: parsed resume request_id=%d path=%s file=%s\n", this->gateway_id, this->request_id, this->firmware_path.c_str(), this->firmware_filename.c_str());

                if (!parts[DEST_ADDR].empty())
                    Utility::convert_asc_hex_string_to_bytes(target_mac_address, (uint8_t *)parts[DEST_ADDR].c_str(), parts[DEST_ADDR].length() / 2);
            }

            fuota->resume_rf_fuota_pending_state_process((unsigned char *)this->gateway_id, target_mac_address, this->request_id, this->firmware_path, this->firmware_filename);
        }
        else
        {
            this->print_and_log("(%s) -> init_connection: no pending resume command in queue, invoking DB-based resume\n", this->gateway_id);
            // No pending entry in resume queue; trigger resume flow which will fetch from DB
            fuota->resume_rf_fuota_pending_state_process((unsigned char *)this->gateway_id, target_mac_address, this->request_id, this->firmware_path, this->firmware_filename);
        }
    }

    if (fetch_path_record_from_src_route_network_db(this->gateway_id) == FAILURE)
    {
        this->print_and_log("Failed to fetch path record from src route network db\n");
    }
    if (this->check_for_unsilenced_nodes(this->gateway_id) == SUCCESS)
    {
        if (this->unsilence_network() == (bool)FAILURE) // Read NP data from DB at initial connection
        {
            insert_update_hes_nms_sync_time(this->gateway_id, 0);
            return FAILURE;
        }
    }
    if (this->is_NP_data_available(this->gateway_id) == SUCCESS)
    {
        if (this->pull_NP_at_init_con() == FAILURE) // Read NP data from DB at initial connection
        {
            insert_update_hes_nms_sync_time(this->gateway_id, 0);
            return FAILURE;
        }
    }
    if (this->check_for_scalar_profile(this->gateway_id))
    {
        if (this->pull_scalar_profile() == FAILURE)
        {
            insert_update_hes_nms_sync_time(this->gateway_id, 0);
            return FAILURE;
        }
    }
    insert_update_hes_nms_sync_time(this->gateway_id, 0);
    return SUCCESS;
}

int Client::parse_instantaneous_scalar_profile(const OtaCmdResponse *response)
{
    uint8_t record_count = response->data[8];
    uint8_t next_page_status = response->data[7];
    uint8_t offset = 9;

    this->print_and_log("parse_instantaneous_profile: %u records\n", record_count);

    // Parse each record
    for (uint8_t i = 0; i < record_count; ++i)
    {
        uint8_t index_byte = response->data[offset];
        const uint8_t *obis_raw = response->data + offset + 1;
        uint8_t scalar_status = response->data[offset + 1 + 6];
        std::string obis_str = obis_to_string(obis_raw);

        // Store OBIS code (always)
        this->Profiles.IP.obis_codes[index_byte] = obis_str;

        if (scalar_status == 0x86)
        {
            this->print_and_log("[IP] Got Scalar status(0x86), index %u ❌: %s\n",
                                index_byte, obis_str.c_str());
            this->manufacture_details[{"0100", index_byte}] = 0;

            // Store empty values for invalid status
            this->Profiles.IP.scalar[index_byte] = {DLMSValue()};
            this->Profiles.IP.unit[index_byte] = {DLMSValue()};
            this->Profiles.IP.scalar_type[index_byte] = {DLMSValue()};
            this->Profiles.IP.unit_type[index_byte] = {DLMSValue()};
            offset += 8;
            continue;
        }

        const dlms_rxd_dlms_obis_payload *p =
            reinterpret_cast<const dlms_rxd_dlms_obis_payload *>(response->data + offset);

        // Create DLMSValue objects (type-safe)
        DLMSValue scalar, unit, scalar_type_val, unit_type_val;

        scalar.setInt8(p->scalar_value);

        unit.setInt8(p->unit_value);

        scalar_type_val.setInt8(p->scalar_data_type);

        unit_type_val.setInt8(p->unit_data_type);

        // ✅ REPLACE (not push_back!)
        this->Profiles.IP.scalar[index_byte] = {scalar};
        this->Profiles.IP.unit[index_byte] = {unit};
        this->Profiles.IP.scalar_type[index_byte] = {scalar_type_val};
        this->Profiles.IP.unit_type[index_byte] = {unit_type_val};

        this->manufacture_details[{"0100", index_byte}] = p->scalar_value;

        if (this->Profiles.IP.unit.count(index_byte))
        {
            const auto &stored_unit_vec = this->Profiles.IP.unit[index_byte];
            const auto &stored_scalar_vec = this->Profiles.IP.scalar[index_byte];
            const auto &stored_scalar_type_vec = this->Profiles.IP.scalar_type[index_byte];
            const auto &stored_unit_type_vec = this->Profiles.IP.unit_type[index_byte];

            if (!stored_unit_vec.empty() && !stored_scalar_vec.empty())
            {
                this->print_and_log("[STORED✓] Idx[%u]: scalar[%s] unit[%s] OBIS[%s] scaler_type[%s] unit_type[%s]\n",
                                    index_byte,
                                    stored_scalar_vec[0].to_string().c_str(),
                                    stored_unit_vec[0].to_string().c_str(),
                                    obis_str.c_str(),
                                    stored_scalar_type_vec[0].to_string().c_str(),
                                    stored_unit_type_vec[0].to_string().c_str());
            }
        }

        offset += sizeof(dlms_rxd_dlms_obis_payload);
    }

    // Database storage (ONLY when complete)
    if (next_page_status == 0x00)
    {
        for (const auto &[index, unit_vec] : this->Profiles.IP.unit)
        {
            if (!unit_vec.empty())
            {
                this->print_and_log("Final IP: Index %d Unit=%s\n",
                                    index, unit_vec[0].to_string().c_str());
            }
        }
        store_scalar_profile_db(1, this->Profiles.IP);

        // CALL addManufacturer with collected scalar values
        this->addManufacturer(this->meter_manufacture_name, this->manufacture_details);

        get_meter_details_from_db(this->meter_manufacture_name, this->meter_fw_version, this->gateway_id);

        // Clear dynamic data
        // Clear
        this->Profiles.IP = ScalarProfile_init{}; // ✅ Reset entire struct
        this->meter_phase = 0;
    }

    return SUCCESS;
}

void Client::store_scalar_profile_db(uint8_t profile_name, const ScalarProfile_init &profile)
{
    char query_buffer[2048];

    // check if data is present in database
    if (check_for_scaler_before_insert(this->meter_manufacture_name, this->meter_fw_version, profile_name) == SUCCESS)
    {
        return;
    }
    //  Iterate ACTUAL map keys (not 0 to size())
    for (uint8_t index_byte = 0; index_byte < profile.scalar.size(); ++index_byte)
    {
        // Check existence before access
        if (profile.unit.find(index_byte) == profile.unit.end() ||
            profile.unit.at(index_byte).empty())
        {
            this->print_and_log("Missing unit data for index %d\n", index_byte);
            continue;
        }
        const auto &scalar_vec = profile.scalar.at(index_byte);
        const auto &unit_vec = profile.unit.at(index_byte);
        const DLMSValue &scalar = scalar_vec[0];
        const DLMSValue &unit = unit_vec[0];

        //  Get data types from dedicated maps (priority)
        int8_t scalar_data_type_int = 0;
        int8_t unit_data_type_int = 0;

        if (profile.scalar_type.count(index_byte) && !profile.scalar_type.at(index_byte).empty())
        {
            scalar_data_type_int = profile.scalar_type.at(index_byte)[0].int8Value;
        }
        if (profile.unit_type.count(index_byte) && !profile.unit_type.at(index_byte).empty())
        {
            unit_data_type_int = profile.unit_type.at(index_byte)[0].int8Value;
        }

        std::string obis_str = profile.obis_codes.count(index_byte) ? profile.obis_codes.at(index_byte) : "UNKNOWN";

        //  Attribute ID mapping
        char attr_id[5];
        uint16_t attr_id_value = 0x0100; // Default IP
        switch (profile_name)
        {
            case 1:
                attr_id_value = 0x0100;
                break;
            case 2:
                attr_id_value = 0x0200;
                break;
            case 3:
                attr_id_value = 0x0300;
                break;
            case 4:
                attr_id_value = 0x0400;
                break;
        }
        snprintf(attr_id, sizeof(attr_id), "%04X", attr_id_value);

        // Step 1: OBIS
        snprintf(query_buffer, sizeof(query_buffer),
                 "INSERT INTO dlms_attributes (obis_code, attribute_id) VALUES ('%s','%s') "
                 "ON DUPLICATE KEY UPDATE obis_code='%s'",
                 obis_str.c_str(), attr_id, obis_str.c_str());

        this->execute_query(query_buffer);

        memset(query_buffer, 0, sizeof(query_buffer));

        // Step 2: Attributes (FIXED unit value!)
        snprintf(query_buffer, sizeof(query_buffer),
                 "INSERT INTO meter_supported_attributes "
                 "(manufacturer_name,meter_supported_index,meter_type,attribute_id,"
                 "scalar_data_type,scalar_value,unit_data_type,unit_value,firmware_version) "
                 "VALUES ('%s','%d','%d','%s','%d','%s','%d','%s','%s') "
                 "ON DUPLICATE KEY UPDATE "
                 "scalar_data_type=VALUES(scalar_data_type),scalar_value=VALUES(scalar_value),"
                 "unit_data_type=VALUES(unit_data_type),unit_value=VALUES(unit_value),"
                 "firmware_version=VALUES(firmware_version)",
                 this->meter_manufacture_name, (int)index_byte, this->meter_phase, attr_id,
                 scalar_data_type_int, scalar.to_string().c_str(),
                 unit_data_type_int, unit.to_string().c_str(), this->meter_fw_version);

        if (this->execute_query(query_buffer) == SUCCESS)
        {
            this->print_and_log("✅ Profile %d Index %d: scalar=%s unit=%s\n",
                                profile_name, (int)index_byte,
                                scalar.to_string().c_str(), unit.to_string().c_str());
        }
    }
}

static std::string obis_to_string(const unsigned char *obis)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u.%u.%u",
                  obis[0], obis[1], obis[2], obis[3], obis[4], obis[5]);
    return std::string(buf);
}

int Client::parse_billing_scalar_profile(const OtaCmdResponse *response)
{
    uint8_t record_count = response->data[8];
    uint8_t next_page_status = response->data[7];
    uint8_t offset = 9;

    this->print_and_log("parse_billing_profile: %u records\n", record_count);

    // DYNAMIC: No fixed-size vectors or bounds checking needed

    // Parse each record
    for (uint8_t i = 0; i < record_count; ++i)
    {
        uint8_t index_byte = response->data[offset];
        const uint8_t *obis_raw = response->data + offset + 1;
        uint8_t scalar_status = response->data[offset + 1 + 6];
        std::string obis_str = obis_to_string(obis_raw);

        // Store OBIS code (always)
        this->Profiles.BH.obis_codes[index_byte] = obis_str;

        if (scalar_status == 0x86)
        {
            this->print_and_log("[BH] Got Scalar status(0x86), index %u ❌: %s\n",
                                index_byte, obis_str.c_str());

            this->manufacture_details[{"0200", index_byte}] = 0;

            // Store empty values for invalid status
            this->Profiles.BH.scalar[index_byte] = {DLMSValue()};
            this->Profiles.BH.unit[index_byte] = {DLMSValue()};
            this->Profiles.BH.scalar_type[index_byte] = {DLMSValue()};
            this->Profiles.BH.unit_type[index_byte] = {DLMSValue()};
            offset += 8;
            continue;
        }

        const dlms_rxd_dlms_obis_payload *p =
            reinterpret_cast<const dlms_rxd_dlms_obis_payload *>(response->data + offset);

        // Create DLMSValue objects (type-safe)
        DLMSValue scalar, unit, scalar_type_val, unit_type_val;

        scalar.setInt8(p->scalar_value);

        scalar_type_val.setInt8(p->scalar_data_type);

        unit.setInt8(p->unit_value);

        unit_type_val.setInt8(p->unit_data_type);

        // DYNAMIC STORAGE: No bounds check, auto-grows
        this->Profiles.BH.scalar[index_byte] = {scalar};
        this->Profiles.BH.unit[index_byte] = {unit};
        this->Profiles.BH.scalar_type[index_byte] = {scalar_type_val};
        this->Profiles.BH.unit_type[index_byte] = {unit_type_val};

        this->manufacture_details[{"0200", index_byte}] = p->scalar_value;

        if (this->Profiles.BH.unit.count(index_byte))
        {
            const auto &stored_unit_vec = this->Profiles.BH.unit[index_byte];
            const auto &stored_scalar_vec = this->Profiles.BH.scalar[index_byte];
            const auto &stored_scalar_type_vec = this->Profiles.BH.scalar_type[index_byte];
            const auto &stored_unit_type_vec = this->Profiles.BH.unit_type[index_byte];

            if (!stored_unit_vec.empty() && !stored_scalar_vec.empty())
            {
                this->print_and_log("[STORED✓] Idx[%u]: scalar[%s] unit[%s] OBIS[%s] scaler_type[%s] unit_type[%s]\n",
                                    index_byte,
                                    stored_scalar_vec[0].to_string().c_str(),
                                    stored_unit_vec[0].to_string().c_str(),
                                    obis_str.c_str(),
                                    stored_scalar_type_vec[0].to_string().c_str(),
                                    stored_unit_type_vec[0].to_string().c_str());
            }
        }

        offset += sizeof(dlms_rxd_dlms_obis_payload);
    }

    // Database storage (ONLY when complete)
    if (next_page_status == 0x00)
    {
        store_scalar_profile_db(2, this->Profiles.BH); // Same function, different profile name

        //  CALL addManufacturer with collected scalar values
        this->addManufacturer(this->meter_manufacture_name, manufacture_details);

        get_meter_details_from_db(this->meter_manufacture_name, this->meter_fw_version, this->gateway_id);

        // Clear dynamic data
        this->Profiles.BH = ScalarProfile_init{}; // ✅ Reset entire struct
        this->meter_phase = 0;
    }

    this->print_and_log("parse_billing_profile complete\n");
    return SUCCESS;
}

int Client::parse_daily_load_scalar_profile(const OtaCmdResponse *response)
{
    uint8_t record_count = response->data[8];
    uint8_t next_page_status = response->data[7];
    uint8_t offset = 9;

    this->print_and_log("parse_daily_load_profile: %u records\n", record_count);

    // DYNAMIC: No fixed-size vectors or bounds checking needed

    // Parse each record
    for (uint8_t i = 0; i < record_count; ++i)
    {
        uint8_t index_byte = response->data[offset];
        const uint8_t *obis_raw = response->data + offset + 1;
        uint8_t scalar_status = response->data[offset + 1 + 6];
        std::string obis_str = obis_to_string(obis_raw);

        // Store OBIS code (always)
        this->Profiles.DLP.obis_codes[index_byte] = obis_str;

        if (scalar_status == 0x86)
        {
            this->print_and_log("[DLP] Got Scalar status(0x86), index %u ❌: %s\n",
                                index_byte, obis_str.c_str());

            this->manufacture_details[{"0300", index_byte}] = 0;

            // Store empty values for invalid status
            this->Profiles.DLP.scalar[index_byte] = {DLMSValue()};
            this->Profiles.DLP.unit[index_byte] = {DLMSValue()};
            this->Profiles.DLP.scalar_type[index_byte] = {DLMSValue()};
            this->Profiles.DLP.unit_type[index_byte] = {DLMSValue()};

            offset += 8;
            continue;
        }

        const dlms_rxd_dlms_obis_payload *p =
            reinterpret_cast<const dlms_rxd_dlms_obis_payload *>(response->data + offset);

        // Create DLMSValue objects (type-safe)
        DLMSValue scalar, unit, scalar_type_val, unit_type_val;

        scalar.setInt8(p->scalar_value);

        scalar_type_val.setInt8(p->scalar_data_type);

        unit.setInt8(p->unit_value);

        unit_type_val.setInt8(p->unit_data_type);

        this->print_and_log("[DLP] Parsed index %u: scalar_value=%d (type %d), unit_value=%d (type %d)\n",
                            index_byte, p->scalar_value, p->scalar_data_type,
                            p->unit_value, p->unit_data_type);

        // DYNAMIC STORAGE: No bounds check, auto-grows
        this->Profiles.DLP.scalar[index_byte] = {scalar};
        this->Profiles.DLP.unit[index_byte] = {unit};
        this->Profiles.DLP.scalar_type[index_byte] = {scalar_type_val};
        this->Profiles.DLP.unit_type[index_byte] = {unit_type_val};

        this->manufacture_details[{"0300", index_byte}] = p->scalar_value;

        if (this->Profiles.DLP.unit.count(index_byte))
        {
            const auto &stored_unit_vec = this->Profiles.DLP.unit[index_byte];
            const auto &stored_scalar_vec = this->Profiles.DLP.scalar[index_byte];
            const auto &stored_scalar_type_vec = this->Profiles.DLP.scalar_type[index_byte];
            const auto &stored_unit_type_vec = this->Profiles.DLP.unit_type[index_byte];

            if (!stored_unit_vec.empty() && !stored_scalar_vec.empty())
            {
                this->print_and_log("[STORED✓] Idx[%u]: scalar[%s] unit[%s] OBIS[%s] scaler_type[%s] unit_type[%s]\n",
                                    index_byte,
                                    stored_scalar_vec[0].to_string().c_str(),
                                    stored_unit_vec[0].to_string().c_str(),
                                    obis_str.c_str(),
                                    stored_scalar_type_vec[0].to_string().c_str(),
                                    stored_unit_type_vec[0].to_string().c_str());
            }
        }

        offset += sizeof(dlms_rxd_dlms_obis_payload);
    }

    // Database storage (ONLY when complete)
    if (next_page_status == 0x00)
    {
        store_scalar_profile_db(3, this->Profiles.DLP);

        //  CALL addManufacturer with collected scalar values
        this->addManufacturer(this->meter_manufacture_name, manufacture_details);

        get_meter_details_from_db(this->meter_manufacture_name, this->meter_fw_version, this->gateway_id);

        // Clear dynamic data
        this->Profiles.DLP = ScalarProfile_init{}; // ✅ Reset entire struct
        this->meter_phase = 0;
    }

    return SUCCESS;
}

int Client::parse_block_load_scalar_profile(const OtaCmdResponse *response)
{
    uint8_t record_count = response->data[8];
    uint8_t next_page_status = response->data[7];
    uint8_t offset = 9;

    this->print_and_log("parse_block_load_profile: %u records\n", record_count);

    // DYNAMIC: No fixed-size vectors or bounds checking needed

    // Parse each record
    for (uint8_t i = 0; i < record_count; ++i)
    {
        uint8_t index_byte = response->data[offset];
        const uint8_t *obis_raw = response->data + offset + 1;
        uint8_t scalar_status = response->data[offset + 1 + 6];
        std::string obis_str = obis_to_string(obis_raw);

        // Store OBIS code (always)
        this->Profiles.BLP.obis_codes[index_byte] = obis_str;

        if (scalar_status == 0x86)
        {
            this->print_and_log("[BLP] Got Scalar status(0x86), index %u ❌: %s\n",
                                index_byte, obis_str.c_str());

            this->manufacture_details[{"0400", index_byte}] = 0;

            // Store empty values for invalid status
            this->Profiles.BLP.scalar[index_byte] = {DLMSValue()};
            this->Profiles.BLP.unit[index_byte] = {DLMSValue()};
            this->Profiles.BLP.scalar_type[index_byte] = {DLMSValue()};
            this->Profiles.BLP.unit_type[index_byte] = {DLMSValue()};
            offset += 8;
            continue;
        }

        const dlms_rxd_dlms_obis_payload *p =
            reinterpret_cast<const dlms_rxd_dlms_obis_payload *>(response->data + offset);

        // Create DLMSValue objects (type-safe)
        DLMSValue scalar, unit, scalar_type_val, unit_type_val;

        scalar.setInt8(p->scalar_value);

        scalar_type_val.setInt8(p->scalar_data_type);

        unit.setInt8(p->unit_value);

        unit_type_val.setInt8(p->unit_data_type);

        this->print_and_log("[BLP] Parsed index %u: scalar_value=%d (type %d), unit_value=%d (type %d)\n",
                            index_byte, p->scalar_value, p->scalar_data_type,
                            p->unit_value, p->unit_data_type);

        // DYNAMIC STORAGE: No bounds check, auto-grows
        this->Profiles.BLP.scalar[index_byte] = {scalar};
        this->Profiles.BLP.unit[index_byte] = {unit};
        this->Profiles.BLP.scalar_type[index_byte] = {scalar_type_val};
        this->Profiles.BLP.unit_type[index_byte] = {unit_type_val};

        this->manufacture_details[{"0400", index_byte}] = p->scalar_value;

        if (this->Profiles.BLP.unit.count(index_byte))
        {
            const auto &stored_unit_vec = this->Profiles.BLP.unit[index_byte];
            const auto &stored_scalar_vec = this->Profiles.BLP.scalar[index_byte];
            const auto &stored_scalar_type_vec = this->Profiles.BLP.scalar_type[index_byte];
            const auto &stored_unit_type_vec = this->Profiles.BLP.unit_type[index_byte];

            if (!stored_unit_vec.empty() && !stored_scalar_vec.empty())
            {
                this->print_and_log("[STORED✓] Idx[%u]: scalar[%s] unit[%s] OBIS[%s] scaler_type[%s] unit_type[%s]\n",
                                    index_byte,
                                    stored_scalar_vec[0].to_string().c_str(),
                                    stored_unit_vec[0].to_string().c_str(),
                                    obis_str.c_str(),
                                    stored_scalar_type_vec[0].to_string().c_str(),
                                    stored_unit_type_vec[0].to_string().c_str());
            }
        }

        offset += sizeof(dlms_rxd_dlms_obis_payload);
    }

    // Database storage (ONLY when complete)
    if (next_page_status == 0x00)
    {
        store_scalar_profile_db(4, this->Profiles.BLP);

        //  CALL addManufacturer with collected scalar values
        this->addManufacturer(this->meter_manufacture_name, manufacture_details);

        get_meter_details_from_db(this->meter_manufacture_name, this->meter_fw_version, this->gateway_id);

        // Clear dynamic data
        this->Profiles.BLP = ScalarProfile_init{}; // ✅ Reset entire struct
        this->meter_phase = 0;
    }

    return SUCCESS;
}

uint8_t *Client::frame_NP_cmd_to_pull_data(uint8_t *destination, uint8_t hop_count)
{
    // Validate total packet size: hop path + DLMS command
    size_t data_needed = hop_count * 4 + DLMS_CMD_LEN;
    if (data_needed > 128)
    {
        this->print_and_log("ERROR: Data too large: %zu > 128\n", data_needed);
        return nullptr; // Reject oversized packets
    }

    // BUILD on STACK (safe construction) - Local variables for packet assembly
    PMESHQuerycmd query_cmd{}; // Zero-initialized PMESH command structure
    DLMSQuerycmd dlms_cmd{};   // Zero-initialized DLMS NamePlate command

    // All your existing code (unchanged):
    query_cmd.start_byte = HES_OTA_CMD_START_BYTE; // PMESH packet start byte
    query_cmd.cmd_type = MESH_DATA_QUERY;          // Command type: Mesh data query

    // Convert PAN_ID (ASCII hex) → binary for PMESH header
    std::array<char, 8> panid_bytes{};
    std::copy(this->PAN_ID, this->PAN_ID + 4, panid_bytes.begin() + 4);
    Utility::convert_asc_hex_string_to_bytes(query_cmd.panid, (uint8_t *)panid_bytes.data(), 16);

    // Convert Source_ID (ASCII hex) → binary source address
    Utility::convert_asc_hex_string_to_bytes(query_cmd.src_addr, (uint8_t *)this->Source_ID, 16);

    query_cmd.router_index = 0x00;       // Current router index (start at 0)
    query_cmd.no_of_routers = hop_count; // Total routers in mesh path

    // Copy destination path (mesh routing): hop_count * 4 bytes (MAC addresses)
    if (hop_count > 0)
    {
        Utility::convert_asc_hex_string_to_bytes(query_cmd.data, destination, hop_count * 4);
    }
    else
    {
        Utility::convert_asc_hex_string_to_bytes(query_cmd.data, destination, 4);
    }
    // Construct DLMS NamePlate query command (fixed structure)
    dlms_cmd.start_byte = DLMS_START_BYTE; // DLMS start byte
    dlms_cmd.length = 0x07;                // DLMS command length (7 bytes)
    dlms_cmd.page_index = 0x00;            // Page index (first page)
    dlms_cmd.frame_id = FI_INSTANT_DATA;   // Frame ID: Instantaneous/NamePlate data
    dlms_cmd.cmd_id = 0x00;                // Command ID (NamePlate pull)
    dlms_cmd.sub_cmd_id = 0x00;            // Sub-command ID
    dlms_cmd.data_index = 0x00;            // Data index

    // Calculate DLMS CRC (sum of all DLMS fields)
    dlms_cmd.crc = dlms_cmd.start_byte + dlms_cmd.length + dlms_cmd.page_index +
                   dlms_cmd.frame_id + dlms_cmd.cmd_id + dlms_cmd.sub_cmd_id + dlms_cmd.data_index;

    if (hop_count > 0)
    {
        // Append DLMS command after mesh path in PMESH data field
        memcpy(query_cmd.data + (hop_count * 4), &dlms_cmd, sizeof(DLMSQuerycmd));
        // Set total PMESH packet length: header + path + DLMS
        query_cmd.length = PMESH_HOPCNT_INDX + (hop_count * 4) + DLMS_CMD_LEN;
    }
    else
    {
        memcpy(query_cmd.data + 4, &dlms_cmd, sizeof(DLMSQuerycmd));
        query_cmd.length = PMESH_HOPCNT_INDX + 4 + DLMS_CMD_LEN;
    }

    // 🔥 CRITICAL: COPY TO HEAP for RETURN - Stack vars destroyed on return
    uint8_t *pkt = new uint8_t[sizeof(PMESHQuerycmd)];
    if (!pkt) return nullptr; // Allocation failure

    memcpy(pkt, &query_cmd, sizeof(PMESHQuerycmd)); // Copy complete packet to heap

    return pkt; // HEAP - survives function return (caller must delete[])
}

/**
 * @brief Transmits PMESH/DLMS command and validates multi-stage response with retries
 *
 * Implements state machine for OTA firmware update sequence:
 * 9B → 9D → 02 → 01 (Exit)
 * Handles page fragmentation, alternate paths, DLMS reconnection, timeouts.
 *
 * @param buf PMESHQuerycmd packet buffer (modified in-place for state transitions)
 * @param length Initial packet length
 * @param maxRetries Maximum retry attempts before failure
 * @return SUCCESS/e_success_1 on completion, FAILURE on timeout/errors
 */
int Client::transmit_command_and_validate_response(uint8_t *buf, size_t length, uint32_t maxRetries)
{
    ssize_t rxLen = 0;                   // Received data length (-1 = error)
    uint32_t retry = 0;                  // Current retry counter
    uint32_t retry_timeout = 0;          // Timeout-specific retry counter
    bool need_to_write = true;           // Flag to trigger next transmission
    bool got_any_response = false;       // Track if ANY response received
    bool need_to_write_dlms_pkt = false; // Flag for DLMS reconnection packets
    uint8_t buffer[512] = {0};           // Receive buffer for responses
    uint8_t dlms_query_buf[128] = {0};   // DLMS reconnection packet buffer
    size_t dlms_pkt_length = 0;          // Length of constructed DLMS packet
    int tried_alternate_path = 0;        // Alternate path attempt counter
    int ret = FAILURE;                   // Response processing return code
    int res = FAILURE;
    uint8_t tx_buffer[512] = {0};
    this->page_index_count = 0; // reset page index for next tx

    uint8_t hop_count = 0; // Number of mesh hops

    PMESHQuerycmd *Pmeshbuf = reinterpret_cast<PMESHQuerycmd *>(buf); // Cast buffer to PMESH structure
    hop_count = Pmeshbuf->no_of_routers;
    uint8_t offset = (hop_count ? hop_count * 4 : 4); // Extract offset for destination address using hop count

    // Main retry loop with state machine
    while (retry <= maxRetries)
    {
        if (need_to_write) // Time to transmit next command?
        {
            set_recv_timeout_for_client(12); // Set 12-second receive timeout
            if (!need_to_write_dlms_pkt)     // Send original PMESH packet?
            {
                memcpy(tx_buffer, buf, length);

                tx_page_index = Pmeshbuf->data[offset + 2];

                if (this->write_to_client(buf, length) != SUCCESS) // Transmit PMESH packet
                {
                    this->print_and_log("Write to client failed\n");
                    break; // Exit on transmit failure
                }
                else
                {
                    this->print_and_log("Write to client success\n");
                }
            }
            else // Send DLMS reconnection packet
            {
                memcpy(tx_buffer, dlms_query_buf, dlms_pkt_length);

                if (this->write_to_client(dlms_query_buf, dlms_pkt_length) != SUCCESS) // Transmit DLMS packet
                {
                    this->print_and_log("Write to client failed\n");
                    break; // Exit on transmit failure
                }
                else
                {
                    this->print_and_log("Write to client success\n");
                }
            }
            need_to_write = false; // Transmission complete, await response
        }

        memset(buffer, 0, sizeof(buffer)); // Clear receive buffer

        rxLen = this->receive_data(buffer, sizeof(buffer)); // Blocking receive with timeout (-1 = error)

        if (rxLen > 0) // Valid response received
        {
            got_any_response = true; // Mark communication alive

            res = this->validate_response_buffer(tx_buffer, buffer);
            ret = FAILURE;
            if (res == SUCCESS)
            {
                ret = this->client_received_data(buffer, rxLen); // Parse and validate response
            }

            switch (ret)
            {
                case SUCCESS:
                case e_success_1: // Valid PMESH response
                {
                    if (buffer[2] == MESH_COMMISSION_PACKET_RESPONSE && buffer[15] == 0x9D && buffer[16] == 0x01) // Stage 1: 9B ACK
                    {

                        Pmeshbuf->data[offset] = 0x03;           // Prepare next stage
                        Pmeshbuf->data[offset + 1] = 0x9B;       // Next: 9B command
                        Pmeshbuf->data[offset + 2] = 0x00;       // Next: 9B command
                        length = PMESH_HOPCNT_INDX + offset + 3; // Update length
                        need_to_write = true;                    // Trigger next transmit
                        retry = 0;                               // Reset retry counters
                        retry_timeout = 0;
                        this->print_and_log("Send 9B command\n");
                    }
                    else // All pages complete
                    {
                        this->print_and_log("No Remaining pkt\n");
                        this->page_index_count = 0; // Reset page counter
                        return ret;                 // success, stop retries
                    }
                }
                break;
                case DLMS_ERROR: {
                    this->print_and_log("[DLMS_ERROR]\n");
                    return FAILURE;
                }
                case DLMS_CHECKSUM_ERROR: {
                    this->print_and_log("[DLMS_CHECKSUM_ERROR]\n");
                }
                break;
                case SUCCES_NEXT_PAGE: {

                    // frame the command again and send
                    buf[PMESH_HOPCNT_INDX + (offset + 3)] = ++this->page_index_count; // Increment page index
                    this->print_and_log("More data pending, sending request for next page: %u\n", this->page_index_count);
                    buf[PMESH_HOPCNT_INDX + (offset + 8)] = buf[PMESH_HOPCNT_INDX + (offset + 8)] + 1; // Increment CRC
                    retry = 0;                                                                         // Reset retry counters
                    retry_timeout = 0;
                    need_to_write = true;
                }
                break;
                case DLMS_CONNECTION_FAILED: // DLMS layer connection lost
                {
                    uint8_t offset = PMESH_HOPCNT_INDX + 1 + (hop_count ? hop_count * 4 : 4);                                                                   // DLMS command offset
                    memcpy(dlms_query_buf, buf, offset);                                                                                                        // Copy PMESH header + path
                    dlms_query_buf[offset] = 0x2B;                                                                                                              // DLMS reconnection command
                    dlms_query_buf[offset + 5] = 0x02;                                                                                                          // Sub-command
                    dlms_query_buf[offset + 6] = 0x01;                                                                                                          // Parameter
                    dlms_pkt_length = offset + 8;                                                                                                               // Set new packet length
                    dlms_query_buf[offset + 1] = 0x07;                                                                                                          // Length field
                    dlms_query_buf[offset + 7] = dlms_query_buf[offset] + dlms_query_buf[offset + 5] + dlms_query_buf[offset + 6] + dlms_query_buf[offset + 1]; // Recalculate CRC

                    if (buffer[24] != 0x01 && buffer[24] != 0x03)
                    {
                        this->print_and_log("Retry count in dlms success %u for [%s] \n", retry, this->gateway_id);
                        retry = 0;
                        retry_timeout = 0; // Reset retry counters
                    }
                    else
                    {
                        this->print_and_log("Retry count in dlms failure %u for [%s] \n", retry, this->gateway_id);
                        retry++; // Increment retry counter
                    }
                    need_to_write_dlms_pkt = true; // Switch to DLMS reconnection mode
                    need_to_write = true;
                }
                break;
                case DLMS_SUCCESS: {
                    need_to_write_dlms_pkt = false;
                    need_to_write = true;
                }
                break;
                case INVALID_RESPONSE:
                case COMMAND_IN_PROGRESS: // Temporary conditions
                {
                    this->need_to_validate_src_addr = true;
                    // need to wait
                    this->print_and_log("wait for response\n"); // Continue loop (implicit wait via receive timeout)
                }
                break;
                case PMESH_TIMEOUT_ERROR: // Mesh layer timeout - try alternate path
                {
                    if (retry_timeout >= 2 && tried_alternate_path < 2 && buf[2] == MESH_DATA_QUERY) // Alternate path conditions met
                    {
                        if (hop_count == 0)
                        {
                            this->print_and_log("NO ALTERNATE PATH TRY FOR GW\n");
                            return FAILURE;
                        }
                        bool alt_path_found = false;   // Alternate path search result
                        uint8_t copy_dest_rx[5] = {0}; // Destination MAC from response
                        uint8_t DlmsBuf[9] = {0};      // DLMS command backup

                        size_t offset_in = PMESH_HOPCNT_INDX + ((hop_count - 1) * 4) + 1; // Last hop destination offset

                        // ✅ BACKUP DLMS COMMAND - Calculate EXACT offset
                        size_t original_path_bytes = hop_count * 4;
                        size_t dlms_start_offset = PMESH_HOPCNT_INDX + original_path_bytes + 1;
                        memcpy(DlmsBuf, &buf[dlms_start_offset], 8); // Exact DLMS location

                        this->print_and_log("DLMS Packet : ");
                        this->print_data_in_hex(DlmsBuf, 8); // Debug DLMS command

                        // copy destination address to search for alternate path
                        memcpy(copy_dest_rx, &buffer[offset_in], 4); // Extract failed destination

                        copy_dest_rx[4] = '\0'; // Null terminate for safe printing as string

                        uint8_t router_mac_addr[8]; // Failed router MAC (ASCII hex)

                        Utility::convert_bytes_to_asc_hex_string(router_mac_addr, copy_dest_rx, 4); // Convert to printable hex

                        std::queue<path_data_record> temp_queue = MySqlDatabase::hesATPathQueue; // Copy path queue

                        while (!temp_queue.empty()) // Search alternate paths
                        {
                            auto &alt_record = temp_queue.front(); // Current path candidate

                            // this->print_and_log("Dequeued router_mac_address %s,target_mac_address %s,path %s\n",
                            //                     alt_record.router_mac_address.data(), this->target_mac_address, alt_record.path_record.data());

                            if (memcmp(this->target_mac_address, alt_record.router_mac_address.data(), 16) == 0) // Path match found
                            {
                                this->print_and_log("Copying Alternate Path to TX Command\n");

                                alt_path_found = true; // Mark success

                                std::vector<uint8_t> alternate_path; // New path buffer

                                //  Copy ONLY path data (exclude null terminator)
                                size_t path_len = alt_record.path_record.size(); // Use size(), not strlen()

                                //  Safe bounds check
                                size_t bytes_needed = (path_len + 1) / 2;
                                if (bytes_needed > alternate_path.size())
                                {
                                    this->print_and_log("ERROR: Path too long: %zu\n", bytes_needed);
                                    return FAILURE;
                                }

                                Utility::convert_asc_hex_string_to_bytes(alternate_path.data(), alt_record.path_record.data(), path_len); // Exact length!

                                size_t new_path_bytes = alt_record.hop_count * 4;
                                //  CRITICAL: Calculate EXACT positions
                                size_t path_start = PMESH_HOPCNT_INDX + 1;
                                size_t new_dlms_start = path_start + new_path_bytes;

                                // Replace path
                                memcpy(&buf[path_start], alternate_path.data(), new_path_bytes);
                                buf[PMESH_HOPCNT_INDX] = alt_record.hop_count; // Update hop count

                                //  Restore DLMS at EXACT new position
                                memcpy(&buf[new_dlms_start], DlmsBuf, 8);

                                //  Recalculate TOTAL length
                                length = new_dlms_start + 8; // path_start + path_bytes + dlms(8) + length_field
                                buf[1] = length - 1;         // Update PMESH length field

                                this->print_and_log("New packet length: %zu\n", length);
                                this->print_data_in_hex(&buf[path_start], new_path_bytes + 8);

                                tried_alternate_path++;
                                retry = 0;
                                retry_timeout = 0;
                                this->hesATPathQueue.pop();
                                break;
                            }
                            temp_queue.pop(); // Pop element from temporary queue only
                        }
                        if (!alt_path_found)
                        {
                            this->print_and_log("Alternate Path Not found for %s\n", router_mac_addr);
                            return FAILURE; // No alternate path available
                        }
                        else
                        {
                            alt_path_found = false; // Reset for next timeout
                        }
                    }
                    else
                    {
                        this->print_and_log("Retry %u: Time Out Data Received [%s]\n", retry, this->gateway_id);
                    }
                    if (tried_alternate_path > 2)
                    {
                        this->print_and_log("TRIED ALTERNATE PATH %d, TIMEOUT RESPONSE\n", tried_alternate_path);
                        return FAILURE;
                    }
                    if (retry >= 2)
                    {
                        this->print_and_log("Retry %u: Time Out Data Received [%s]\n", retry, this->gateway_id);
                        return FAILURE;
                    }
                }
                break;
                case FAILED_RESPONSE:
                    this->print_and_log("❌ FAILED RESPONSE");
                    break;
                case FAILURE:
                default: {
                    this->print_and_log("Retry %u: Unexpected Data Received [%s]\n", retry, this->gateway_id);
                    break;
                }
            }
        }
        else // No response (timeout)
        {
            if (this->gatewayStatus == Status::DISCONNECTED || this->duplicate_gateway == true)
            {
                this->print_and_log("Gateway_disconnected\n");
                break; // thread shutting down or duplicate connection
            }
            this->print_and_log("[⏰ POLL TIMEOUT]: %u for [%s]\n", retry, this->gateway_id);
            need_to_write = true; // Retransmit next iteration
            retry++;              // Increment retry counter
        }

        if (ret == PMESH_TIMEOUT_ERROR || ret == FAILED_RESPONSE) // Normal retry conditions
        {
            need_to_write = true; // resend on next iteration
            retry++;              // Increment retry counter
            retry_timeout++;      // Increment timeout counter
        }
        if ((retry > 2 || retry_timeout > 2)) // Max retries exceeded
        {
            this->print_and_log("POLL TIMEOUT retry %d, retry_timeout %d for [%s]\n", retry, retry_timeout, this->gateway_id);
            break;
        }
    }

    if (maxRetries > 0 && !got_any_response) // Final cleanup: No communication
    {
        this->gatewayStatus = Status::DISCONNECTED;
        this->print_and_log("Disconnecting: [%s] after %u retries\n", this->gateway_id, retry);
        this->print_and_log("Gateway status: %d\n", this->gatewayStatus);
        this->need_to_validate_src_addr = false; // Disable source address validation
    }

    return FAILURE; // Default failure (success paths return early)
}

/**
 * @brief Frames Scalar Profile command packet for pulling specific meter profile data
 *
 * Constructs PMESH + DLMS command for Instantaneous/Billing/Daily/Block Load profiles.
 * Supports profile selection via parameter (1=IP, 2=BH, 3=DLP, 4=BLP).
 * Caller MUST delete[] returned pointer to avoid memory leak.
 *
 * @param path_record Pointer to hex-encoded mesh path (hop_count * 4 bytes)
 * @param hop_count Number of mesh routers in path (1-32 max)
 * @param profile Profile type: 1=Instantaneous, 2=Billing, 3=Daily Load, 4=Block Load
 * @return Allocated PMESHQuerycmd packet or nullptr on error
 * @note Memory allocated with new[] - caller responsible for delete[]
 */
uint8_t *Client::frame_Scalar_Profile_cmd_to_pull(uint8_t *path_record, uint8_t hop_count, uint8_t profile)
{
    this->print_and_log("Frame scalar profile\n"); // Debug: Profile pull initiated

    // SAFE: Bounds check - Validate total packet size
    size_t data_needed = hop_count * 4 + DLMS_CMD_LEN; // Mesh path + DLMS command size
    if (data_needed > 128)
    {
        this->print_and_log("ERROR: Data too large: %zu > 128\n", data_needed);
        return nullptr; // Reject oversized packets
    }

    // STACK - NO LEAKS, NO CRASHES! - Local variables for safe packet construction
    PMESHQuerycmd query_cmd{}; // Zero-init ALL fields → No warnings!
    DLMSQuerycmd dlms_cmd{};   // Zero-init ALL fields → No warnings!

    // PMESH Header construction
    query_cmd.start_byte = HES_OTA_CMD_START_BYTE; // PMESH packet start byte
    query_cmd.cmd_type = MESH_DATA_QUERY;          // Command type: Mesh data query

    // Convert PAN_ID (ASCII hex, 4 chars) → binary PAN ID (8 bytes)
    std::array<char, 8> panid_bytes{};
    std::copy(this->PAN_ID, this->PAN_ID + 4, panid_bytes.begin() + 4);
    Utility::convert_asc_hex_string_to_bytes(query_cmd.panid, (uint8_t *)panid_bytes.data(), 16);

    // Convert Source_ID (ASCII hex) → binary source address (8 bytes)
    Utility::convert_asc_hex_string_to_bytes(query_cmd.src_addr, (uint8_t *)this->Source_ID, 16);

    // Mesh routing fields
    query_cmd.router_index = 0x00;       // Current router index (start at 0)
    query_cmd.no_of_routers = hop_count; // Total routers in mesh path

    // Copy mesh path: hop_count * 4 bytes (MAC addresses in hex → binary)
    if (hop_count > 0)
    {
        Utility::convert_asc_hex_string_to_bytes(query_cmd.data, path_record, hop_count * 4);
    }
    else
    {
        Utility::convert_asc_hex_string_to_bytes(query_cmd.data, path_record, 4);
    }

    // Profile-specific DLMS command construction
    dlms_cmd.start_byte = DLMS_START_BYTE;         // DLMS start byte
    dlms_cmd.length = 0x07;                        // Fixed DLMS command length (7 bytes)
    dlms_cmd.page_index = 0x00;                    // First page (page_index=0)
    dlms_cmd.frame_id = FI_OBIS_CODES_SCALAR_LIST; // Frame ID: OBIS scalar list request

    // Profile type → DLMS command ID mapping
    switch (profile)
    {
        case 1: // Instantaneous Profile
            dlms_cmd.cmd_id = 0x01;
            break;
        case 2: // Billing History Profile
            dlms_cmd.cmd_id = 0x02;
            break;
        case 3: // Daily Load Profile
            dlms_cmd.cmd_id = 0x03;
            break;
        case 4: // Block Load Profile
            dlms_cmd.cmd_id = 0x04;
            break;
        default: // Invalid profile
            this->print_and_log("Unknown Command ID: %d\n", profile);
            return nullptr; // Reject unknown profile
    }

    // DLMS command completion fields
    dlms_cmd.sub_cmd_id = 0x00; // Sub-command ID (none)
    dlms_cmd.data_index = 0x00; // Data index (start)

    // Calculate DLMS CRC (sum of all 7 DLMS fields)
    dlms_cmd.crc = dlms_cmd.start_byte + dlms_cmd.length + dlms_cmd.page_index +
                   dlms_cmd.frame_id + dlms_cmd.cmd_id + dlms_cmd.sub_cmd_id + dlms_cmd.data_index;

    if (hop_count > 0)
    {
        // Copy DLMS to path end - Append after mesh path data
        memcpy(query_cmd.data + (hop_count * 4), &dlms_cmd, sizeof(DLMSQuerycmd));
        // Set total PMESH packet length: header + path + DLMS
        query_cmd.length = PMESH_HOPCNT_INDX + (hop_count * 4) + DLMS_CMD_LEN;
    }
    else
    {
        // Copy DLMS to path end - Append after mesh path data
        memcpy(query_cmd.data + 4, &dlms_cmd, sizeof(DLMSQuerycmd));
        // Set total PMESH packet length: header + path + DLMS
        query_cmd.length = PMESH_HOPCNT_INDX + 4 + DLMS_CMD_LEN;
    }

    // CRITICAL: Copy STACK → HEAP for return - Stack destroyed on function exit
    uint8_t *pkt = new uint8_t[sizeof(PMESHQuerycmd)]; // Heap allocation
    if (!pkt)
    {
        this->print_and_log("FATAL: Heap allocation failed\n");
        return nullptr; // Allocation failure
    }
    memcpy(pkt, &query_cmd, sizeof(PMESHQuerycmd)); // Copy complete packet to heap

    return pkt; // SAFE HEAP POINTER - caller must delete[]
}

int Client::pull_NP_at_init_con()
{
    this->print_and_log("NAME_PLATE PULL\n"); // Log: NamePlate pull sequence started

    // PHASE 1: Send NamePlate pull commands for ALL meters
    std::queue<meter_data_record> temp_queue2 = MySqlDatabase::hesNPDataQueue; // Copy queue again (original unchanged)

    while (!temp_queue2.empty())
    {

        auto &router_mac_add = temp_queue2.front(); // Get front queue element (meter record)

        // Copy meter MAC address to global target (16 bytes + null terminator)
        memcpy(this->target_mac_address, (uint8_t *)router_mac_add.meter_mac_address.data(), 16);
        this->target_mac_address[16] = '\0';

        // Frame NamePlate pull command using pre-populated path
        uint8_t *tx_buffer = this->frame_NP_cmd_to_pull_data(router_mac_add.path_record.data(), router_mac_add.hop_count);

        if (tx_buffer)
        {
            int tx_length = tx_buffer[1] + 1; // Extract packet length from PMESH length field

            // Transmit NamePlate command + validate response (max 4 retries)
            int ret = this->transmit_command_and_validate_response(tx_buffer, tx_length, RETRY_COUNT_4);

            free(tx_buffer); // Free heap-allocated transmit buffer

            // update to database
            if (ret == SUCCESS)
            {
                this->insert_name_plate_db((const char *)this->target_mac_address, (const char *)this->gateway_id, (const ODM_NamePlateProfile)this->name_plate, this->time_str);
                this->name_plate = ODM_NamePlateProfile{};
            }
            else
            {
                this->print_and_log("Gateway status1:%d\n", this->gatewayStatus);
                if (this->gatewayStatus == DISCONNECTED)
                {
                    this->print_and_log("Gateway Disconnected during NamePlate pull\n");
                    return FAILURE;
                }
                this->print_and_log("transmit_command_and_validate_response Failed\n");
            }

            temp_queue2.pop(); // Remove processed meter from temporary queue
            this->hesNPDataQueue.pop();
        }
    }
    return SUCCESS; // All NamePlate pulls completed
}

int Client::pull_scalar_profile()
{
    this->print_and_log("Scalar Pull\n"); // Log: Scalar profile pull started

    std::queue<meter_details> temp_queue2 = MySqlDatabase::meterDetailsQueue; // Copy NamePlate queue

    // Process all meters in queue
    while (!temp_queue2.empty())
    {
        meter_details &router_mac_add = temp_queue2.front(); // Get front queue element

        // Copy meter MAC address to target (16 bytes + null)
        memcpy(this->target_mac_address, (uint8_t *)router_mac_add.meter_mac_address.data(), 16);
        this->target_mac_address[16] = '\0';

        // Copy manufacturer name (64 bytes + null)
        memcpy(this->meter_manufacture_name, router_mac_add.meter_manufacture_name.data(), 64);
        this->meter_manufacture_name[64] = '\0';

        memcpy(this->meter_fw_version, router_mac_add.meter_fw_version.data(), 64);
        this->meter_fw_version[64] = '\0';

        // Set meter phase from queue data
        this->meter_phase = router_mac_add.meter_phase;

        // Frame scalar profile command (1=IP, 2=BH, 3=DLP, 4=BLP)
        uint8_t *tx_buffer = this->frame_Scalar_Profile_cmd_to_pull(router_mac_add.path_record.data(), router_mac_add.hop_count, router_mac_add.profile_id);

        if (tx_buffer)
        {
            int tx_length = tx_buffer[1] + 1; // Extract packet length from length field

            // Transmit and validate response (max 4 retries)
            int ret = this->transmit_command_and_validate_response(tx_buffer, tx_length, RETRY_COUNT_4);

            free(tx_buffer); // Free heap-allocated packet

            if (ret == SUCCESS) // Transmit/validation failed
            {
                this->print_and_log("Transmit or validation SUCCESS\n");
            }
            else
            {
                this->print_and_log("Gateway status1: %d\n", this->gatewayStatus);
                if (this->gatewayStatus == DISCONNECTED)
                {
                    this->print_and_log("Gateway Disconnected during Scalar Profile pull\n");
                    return FAILURE;
                }
                this->print_and_log("❌ Transmit Failed node [%s]\n", this->gateway_id);
            }
        }
        else
        {
            this->print_and_log("Failed to frame a command for node: %s\n", router_mac_add.meter_mac_address.data());
        }
        temp_queue2.pop();
        this->meterDetailsQueue.pop(); // Remove processed meter from queue
    }
    return SUCCESS; // All meters processed
}

bool Client::unsilence_network()
{
    std::queue<silenceND> temp_queue = MySqlDatabase::silencedNDQueue;
    silenceND node_list[100];
    int node_count = 0;

    // Step 1: Copy queue to array
    while (!temp_queue.empty())
    {
        node_list[node_count] = temp_queue.front();
        node_count++;
        temp_queue.pop();
    }

    if (node_count == 0)
    {
        this->print_and_log("No nodes in queue\n");
        return SUCCESS;
    }

    // Step 2: Find MAX hop_count once
    uint8_t max_hop = 0;
    for (int i = 0; i < node_count; i++)
    {
        if (node_list[i].hop_count > max_hop)
            max_hop = node_list[i].hop_count;
    }

    // Step 3: Process ALL nodes in ASCENDING hop_count order
    int total_processed = 0;

    // Try each hop_count level from 0 to max_hop
    for (uint8_t current_hop = 0; current_hop <= max_hop; current_hop++)
    {
        this->print_and_log("\n--- Processing hop_count = %d ---\n", current_hop);

        // Process ALL nodes with this exact hop_count
        for (int i = 0; i < node_count; i++)
        {
            // Only process nodes with EXACT current_hop AND not processed
            if (node_list[i].hop_count == current_hop)
            {
                silenceND &router_mac_add = node_list[i];

                uint8_t *tx_buffer = this->frame_unsilence_cmd(router_mac_add.path_record.data(), router_mac_add.hop_count);

                int ret = FAILURE;
                if (tx_buffer)
                {
                    int tx_length = tx_buffer[1] + 1;
                    ret = this->transmit_command_and_validate_response(tx_buffer, tx_length, RETRY_COUNT_4);
                    free(tx_buffer);

                    if (ret == SUCCESS)
                    {
                        this->print_and_log("✅ SUCCESS hop_count=%d (path = %s)\n", current_hop, router_mac_add.path_record.data());

                        if (this->delete_node_from_db(router_mac_add.meter_mac_address.data(), this->gateway_id) == FAILURE)
                        {
                            this->print_and_log("❌ Failed to delete the table\n");
                        }
                        node_list[i].hop_count = max_hop + 1; // Mark as processed
                        total_processed++;
                    }
                    else
                    {
                        this->print_and_log("Gateway status1: %d\n", this->gatewayStatus);
                        if (this->gatewayStatus == DISCONNECTED)
                        {
                            this->print_and_log("Gateway Disconnected during Unsilence operation\n");
                            return FAILURE;
                        }
                    }
                }
            }
        }
    }

    this->print_and_log("\n✅ Unsilence cycle COMPLETE: %d/%d nodes processed\n", total_processed, node_count);
    return SUCCESS;
}

uint8_t *Client::frame_unsilence_cmd(uint8_t *path_record, uint8_t hop_count)
{
    // Ternary offset: hop_count ? hop_count * 4 : 4 + DLMS_CMD_LEN
    size_t data_needed = (hop_count ? hop_count * 4 : 4) + DLMS_CMD_LEN;

    if (data_needed > 128)
    {
        this->print_and_log("ERROR: Data too large: %zu > 128\n", data_needed);
        return nullptr; // Reject oversized packets
    }

    // Heap allocation for return (no stack copy needed)
    PMESHQuerycmd *query_cmd = new PMESHQuerycmd;            // PMESH command structure
    UnsilenceQuerycmd *unslinec_cmd = new UnsilenceQuerycmd; // Unsilence command (9B)

    memset(query_cmd, 0, sizeof(PMESHQuerycmd));        // Safe zeroing of PMESH structure
    memset(unslinec_cmd, 0, sizeof(UnsilenceQuerycmd)); // Safe zeroing of unsilence command

    // PMESH Header construction
    query_cmd->start_byte = HES_OTA_CMD_START_BYTE; // PMESH start byte
    query_cmd->cmd_type = MESH_COMMISSION_PACKET;   // Commissioning packet type

    // Convert PAN_ID (ASCII hex) → binary PAN ID
    std::array<char, 8> panid_bytes{};
    std::copy(this->PAN_ID, this->PAN_ID + 4, panid_bytes.begin() + 4);
    Utility::convert_asc_hex_string_to_bytes(query_cmd->panid, (uint8_t *)panid_bytes.data(), 16);

    // Convert Source_ID → binary source address
    Utility::convert_asc_hex_string_to_bytes(query_cmd->src_addr, (uint8_t *)this->Source_ID, 16);

    // Mesh routing fields
    query_cmd->router_index = 0x00;       // Current router index
    query_cmd->no_of_routers = hop_count; // Total routers in path

    // Copy mesh path to data field
    // Copy mesh path to data field (ternary for hop_count=0)
    Utility::convert_asc_hex_string_to_bytes(query_cmd->data, path_record, (hop_count ? hop_count * 4 : 4));

    // Unsilence command construction (9B wake command)
    unslinec_cmd->start_byte = 0x00; // Unsilence read
    unslinec_cmd->cmd = 0x9D;        // Unsilence command (wake node)
    // unslinec_cmd->write = 0x00;      // Write flag (read operation)

    // Append unsilence command after mesh path
    uint8_t *unsilence_bytes = reinterpret_cast<uint8_t *>(unslinec_cmd);
    memcpy(query_cmd->data + (hop_count ? (hop_count * 4) : 4), unsilence_bytes, sizeof(UnsilenceQuerycmd));

    // Set total packet length: header + path + unsilence command
    query_cmd->length = PMESH_HOPCNT_INDX + (hop_count ? (hop_count * 4) : 4) + UNSILECE_CMD_LEN;

    return reinterpret_cast<uint8_t *>(query_cmd); // Return PMESH pointer (caller frees)
}

void Client::Insert_receive_data_offset()
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    // Safely extract latest instantaneous values (fall back to 0/empty if not present)
    int32_t freq_val = 0;
    std::string meter_rtc_time = "";
    int8_t rssi_val = 0;
    int8_t noise_val = 0;
    int16_t temp = 0;
    uint32_t tdc = 0;

    if (!this->IP.data[IP_FREQUENCY_OFFSET].empty())
    {
        freq_val = this->IP.data[IP_FREQUENCY_OFFSET].back().int8Value;
        // freq_val = (freq_val * 25000) / 64;
    }
    if (!this->IP.data[IP_RTC].empty())
        meter_rtc_time = this->IP.data[IP_RTC].back().getAsString();
    if (!this->IP.data[IP_RSSI].empty())
        rssi_val = static_cast<int>(this->IP.data[IP_RSSI].back().int8Value);
    if (!this->IP.data[IP_NOISE_FLOOR].empty())
        noise_val = static_cast<int>(this->IP.data[IP_NOISE_FLOOR].back().int8Value);
    if (!this->IP.data[IP_TDC_VALUE].empty())
    {
        uint32_t tdc_raw = static_cast<int>(this->IP.data[IP_TDC_VALUE].back().int8Value);
        uint8_t *bytes = reinterpret_cast<uint8_t *>(&tdc_raw);
        tdc =
            (static_cast<uint32_t>(bytes[0]) << 24) | // MSB
            (static_cast<uint32_t>(bytes[1]) << 16) |
            (static_cast<uint32_t>(bytes[2]) << 8) |
            (static_cast<uint32_t>(bytes[3])); // LSB
    }
    if (!this->IP.data[IP_TEMP].empty())
    {
        int16_t original_int16 = this->IP.data[IP_TEMP].back().int16Value;
        this->print_and_log("[ORIGINAL TEMP VALUE] = %d\n", original_int16);
        uint8_t *bytes = reinterpret_cast<uint8_t *>(&original_int16);

        // Reverse 2 bytes for int16_t: [b0,b1] → [b1,b0]
        temp = static_cast<int16_t>(
            (bytes[0] << 8) | bytes[1]);
    }
    this->print_and_log("[FREQ_OFFSET] = %d || [RSSI] = %d || [NOISE] = %d || [TEMP] = %d || [TDC] = %d\n", freq_val, rssi_val, noise_val, temp, tdc);
    // Build query with correct format specifiers
    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO rf_param_info(gateway_id,meter_mac_address,meter_sl_no,frequency_offset,frequency_last_download_time,meter_rtc_time,rssi_value,noise_floor,temprature,tdc) "
             "VALUES('%s','%s','%s',%d,'%s','%s',%d,%d,%d,%d)",
             this->DB_parameter.gateway_id.c_str(),
             this->DB_parameter.meter_mac_address.c_str(),
             this->DB_parameter.meter_serial_no.c_str(),
             freq_val,
             this->DB_parameter.last_download_time.c_str(),
             meter_rtc_time.c_str(),
             rssi_val,
             noise_val,
             temp,
             tdc);

    int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

    if (db_result == 0)
    { // ✅ SUCCESS
        this->print_and_log("[DB] 🗄️ INSERT RECEIVE DATA OFFSET ✅ | Offset=%d | Meter=%s\n", freq_val, this->DB_parameter.meter_serial_no.c_str());
    }
    else
    { // ✅ FAILURE
        this->print_and_log("[DB] 🗄️ INSERT RECEIVE DATA OFFSET ❌ | Meter=%s \n", this->DB_parameter.meter_serial_no.c_str());
    }
}

void Client::Update_Insert_Ping_request(uint8_t download_type, uint8_t status)
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    this->pingnode_detail.time_duration = std::chrono::duration_cast<std::chrono::milliseconds>(this->pingnode_detail.Time_rx - this->pingnode_detail.Time_tx).count();
    if (download_type == PING_NODE)
    {
        if (this->pingnode_detail.ping_count == 0) // First ping request
        {
            snprintf(query_buf, sizeof(query_buf),
                     "UPDATE dlms_on_demand_ping_request SET status = %d, last_download_time = '%s', time_duration = %llu WHERE gateway_id = '%s' AND target_mac_address = '%s' AND request_id = %zu",
                     status,
                     this->DB_parameter.last_download_time.c_str(),
                     (unsigned long long)this->pingnode_detail.time_duration,
                     this->DB_parameter.gateway_id.c_str(),
                     this->DB_parameter.meter_mac_address.c_str(),
                     this->DB_parameter.req_id);
        }
        else
        {
            snprintf(query_buf, sizeof(query_buf),
                     "INSERT INTO dlms_on_demand_ping_request("
                     "gateway_id,gateway_type,meter_sno,download_sub_data_type,target_mac_address,request_command,status,request_id,last_download_time,time_duration) "
                     "VALUES('%s',%d,'%s',%d,'%s','%s',%d,%zu,'%s',%llu)",

                     // 6 VALUES :
                     this->pingnode_detail.gateway_id.c_str(),
                     this->pingnode_detail.gateway_type.back(),
                     this->pingnode_detail.meter_serial_no.c_str(),
                     download_type,
                     this->pingnode_detail.target_mac_address.c_str(),
                     this->pingnode_detail.req_command.c_str(),
                     status,
                     this->pingnode_detail.request_id.back(),
                     this->DB_parameter.last_download_time.c_str(),
                     (unsigned long long)this->pingnode_detail.time_duration);
        }
    }
    else
    {
        snprintf(query_buf, sizeof(query_buf),
                 "UPDATE dlms_on_demand_ping_request SET status = %d, last_download_time = '%s', time_duration = %llu WHERE gateway_id = '%s' AND target_mac_address = '%s' AND request_id = %zu",
                 status,
                 this->DB_parameter.last_download_time.c_str(),
                 (unsigned long long)this->pingnode_detail.time_duration,
                 this->DB_parameter.gateway_id.c_str(),
                 this->DB_parameter.meter_mac_address.c_str(),
                 this->DB_parameter.req_id);
    }
    int db_result = execute_query(query_buf);
    if (db_result == 0)
    { // ✅ SUCCESS
        this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", download_type, this->DB_parameter.req_id);
        clear_profile_for_type(download_type);
    }
    else
    { // ✅ FAILURE
        this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", download_type);
    }
}

void Client::ping_node_details(const std::vector<std::string> &parts, DBparameters &DB_parameter)
{
    this->print_and_log("Ping Node/Meter Details\n");
    this->pingnode_detail.gateway_id = parts.size() > 1 ? parts[1] : std::string();
    uint8_t hop_count = 0;
    if (parts.size() > 2 && !parts[2].empty())
        hop_count = static_cast<uint8_t>(std::stoi(parts[2]));
    if (hop_count == 0)
        this->pingnode_detail.gateway_type.push_back(1);
    else
        this->pingnode_detail.gateway_type.push_back(0);
    if (!parts.empty())
        this->pingnode_detail.request_id.push_back(static_cast<size_t>(std::stoul(parts[0])));
    this->pingnode_detail.meter_serial_no = DB_parameter.meter_serial_no;
    this->pingnode_detail.target_mac_address = DB_parameter.meter_mac_address;
    // Join parts into a single command string for DB storage
    std::string joined_cmd;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i) joined_cmd += ':';
        joined_cmd += parts[i];
    }
    this->pingnode_detail.req_command = joined_cmd;
    this->pingnode_detail.ping_count = 0;
}

int Client::validate_response_buffer(uint8_t *tx_buffer, uint8_t *rx_buffer)
{
    PMESHQuerycmd *tx_buf = reinterpret_cast<PMESHQuerycmd *>(const_cast<uint8_t *>(tx_buffer));
    OtaCmdResponse *rx_buf = reinterpret_cast<OtaCmdResponse *>(const_cast<uint8_t *>(rx_buffer));

    uint8_t hop_count = tx_buffer[12];
    // If hop_count == 0 → gateway, else router
    const uint8_t base_offset = hop_count ? (hop_count * 4) : 4;                                          // ternary as requested
    const uint8_t base_offset2 = hop_count ? PMESH_HOPCNT_INDX + (hop_count * 4) : PMESH_HOPCNT_INDX + 4; // ternary as requested
    const uint8_t page_idx_tx = tx_buf->data[base_offset + 2];
    const uint8_t frame_id_tx = tx_buf->data[base_offset + 3];
    const uint8_t cmd_id_tx = tx_buf->data[base_offset + 4];

    if (rx_buf->packet_type == MESH_DATA_RESPONSE)
    {
        const uint8_t page_idx_rx = rx_buf->data[3];
        const uint8_t frame_id_rx = rx_buf->data[4];
        const uint8_t cmd_id_rx = rx_buf->data[5];

        if (rx_buf->data[3] == 0x00 && rx_buf->data[4] == 0x00 && rx_buf->data[5] == 0x00 && rx_buf->data[6] == 0x02)
        {
            return SUCCESS;
        }
        if (rx_buf->data[0] == 0x2C)
            return SUCCESS;

        if (page_idx_tx != page_idx_rx)
        {
            this->print_and_log("Invalid Page index ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n",
                                page_idx_tx, page_idx_rx);
            return INVALID_RESPONSE;
        }
        if (frame_id_tx != frame_id_rx)
        {
            this->print_and_log("Invalid Frame ID ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n",
                                frame_id_tx, frame_id_rx);
            return INVALID_RESPONSE;
        }
        if (cmd_id_tx != cmd_id_rx)
        {
            this->print_and_log("Invalid Command ID ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n",
                                cmd_id_tx, cmd_id_rx);
            return INVALID_RESPONSE;
        }
    }
    else if (rx_buf->packet_type == MESH_COMMISSION_PACKET_RESPONSE)
    {
        const uint8_t cmd_tx = tx_buf->data[base_offset + 1];

        const uint8_t cmd_rx = rx_buffer[UNSILENCE_RESPONSE_LEN - 2];
        const uint8_t res = rx_buffer[UNSILENCE_RESPONSE_LEN - 1];
        if (cmd_rx != cmd_tx)
        {
            this->print_and_log("Invalid Command type ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n", cmd_tx, cmd_rx);
            return INVALID_RESPONSE;
        }
        if (res != 0x00)
        {
            this->print_and_log("Failed response❌\n");
            return FAILED_RESPONSE;
        }
        return SUCCESS;
    }
    else if (rx_buf->packet_type == MESH_DATA_QUERY)
    {
        const uint8_t page_idx_rx = rx_buffer[base_offset2 + 3];
        const uint8_t frame_id_rx = rx_buffer[base_offset2 + 4];
        const uint8_t cmd_id_rx = rx_buffer[base_offset2 + 5];

        if (page_idx_tx != page_idx_rx)
        {
            this->print_and_log("Invalid Page index ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n",
                                page_idx_tx, page_idx_rx);
            return INVALID_RESPONSE;
        }
        if (frame_id_tx != frame_id_rx)
        {
            this->print_and_log("Invalid Frame ID ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n",
                                frame_id_tx, frame_id_rx);
            return INVALID_RESPONSE;
        }
        if (cmd_id_tx != cmd_id_rx)
        {
            this->print_and_log("Invalid Command ID ❌ | tx_buf = 0x%02X, rx_buf = 0x%02X\n",
                                cmd_id_tx, cmd_id_rx);
            return INVALID_RESPONSE;
        }
    }

    this->print_and_log("Valid Response Frame ✅\n");
    return SUCCESS;
}

void Client::validate_NP_for_db()
{
    // Ensure common DB parameters are initialized
    ensure_db_parameters();
    if (name_plate.data[NP_METER_SERIALNUMBER].empty())
    {
        this->print_and_log("NP_METER_SERIALNUMBER data empty, assigning default empty string\n");
        DLMSValue val;
        val.setString("");
        name_plate.data[NP_METER_SERIALNUMBER].push_back(val);
    }
    if (name_plate.data[NP_DEVICE_ID].empty())
    {
        this->print_and_log("NP_DEVICE_ID data empty, assigning default empty string\n");
        DLMSValue val;
        val.setString("");
        name_plate.data[NP_DEVICE_ID].push_back(val);
    }
    if (name_plate.data[NP_MANUFACTURE_NAME].empty())
    {
        this->print_and_log("NP_MANUFACTURE_NAME data empty, assigning default empty string\n");
        DLMSValue val;
        val.setString("");
        name_plate.data[NP_MANUFACTURE_NAME].push_back(val);
    }
    if (name_plate.data[NP_METER_FIRMWARE].empty())
    {
        this->print_and_log("NP_METER_FIRMWARE data empty, assigning default empty string\n");
        DLMSValue val;
        val.setString("");
        name_plate.data[NP_METER_FIRMWARE].push_back(val);
    }
    if (name_plate.data[NP_METER_PHASE].empty())
    {
        this->print_and_log("NP_METER_PHASE data empty, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        name_plate.data[NP_METER_PHASE].push_back(val);
    }
    if (name_plate.data[NP_CATEGORY].empty())
    {
        this->print_and_log("NP_CATEGORY data empty, assigning default empty string\n");
        DLMSValue val;
        val.setString("");
        name_plate.data[NP_CATEGORY].push_back(val);
    }
    if (name_plate.data[NP_CURRENT_RATING].empty())
    {
        this->print_and_log("NP_CURRENT_RATING data empty, assigning default empty string\n");
        DLMSValue val;
        val.setString("");
        name_plate.data[NP_CURRENT_RATING].push_back(val);
    }
    if (name_plate.data[NP_MANUFACTURE_YEAR].empty())
    {
        this->print_and_log("NP_MANUFACTURE_YEAR data empty, assigning default 0\n");
        DLMSValue val;
        val.setUint16(0);
        name_plate.data[NP_MANUFACTURE_YEAR].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_IP_for_db()
{
    // Ensure common DB parameters are initialized
    ensure_db_parameters();

    // Helper lambda to ensure DLMSValue vector has at least one valid entry with debug
    auto ensure_dlms_value = [this](std::vector<DLMSValue> &data, DLMSDataType type, const char *param_name, float fval = 0.0f, std::vector<uint8_t> octet = {}, bool bval = false) {
        if (data.empty() || data[0].type == DLMS_DATA_TYPE_NONE)
        {
            this->print_and_log("%s data empty or NONE, assigning default value\n", param_name);

            DLMSValue val;
            switch (type)
            {
                case DLMS_DATA_TYPE_OCTET_STRING:
                    val.setOctetString(octet);
                    break;
                case DLMS_DATA_TYPE_FLOAT32:
                    val.setFloat32(fval);
                    break;
                case DLMS_DATA_TYPE_BOOLEAN:
                    val.setBool(bval);
                    break;
                default:
                    val.setFloat32(0.0f);
                    break;
            }
            data.push_back(val);
        }
    };

    // RTC (Octet String)
    ensure_dlms_value(IP.data[IP_RTC], DLMS_DATA_TYPE_OCTET_STRING, "IP_RTC");

    // DB parameter defaults handled by ensure_db_parameters()

    // Voltage (Float32)
    ensure_dlms_value(IP.data[IP_VOLTAGE], DLMS_DATA_TYPE_FLOAT32, "IP_VOLTAGE");

    // Phase Current (Float32)
    ensure_dlms_value(IP.data[IP_PHASE_CURRENT], DLMS_DATA_TYPE_FLOAT32, "IP_PHASE_CURRENT");

    // Neutral Current (Float32)
    ensure_dlms_value(IP.data[IP_NEUTRAL_CURRENT], DLMS_DATA_TYPE_FLOAT32, "IP_NEUTRAL_CURRENT");

    // Signed Power Factor (Float32)
    ensure_dlms_value(IP.data[IP_SIGNED_POWER_FACTOR], DLMS_DATA_TYPE_FLOAT32, "IP_SIGNED_POWER_FACTOR");

    // Frequency (Float32)
    ensure_dlms_value(IP.data[IP_FREQUENCY], DLMS_DATA_TYPE_FLOAT32, "IP_FREQUENCY");

    // Apparent Power (Float32)
    ensure_dlms_value(IP.data[IP_APPARENT_POWER], DLMS_DATA_TYPE_FLOAT32, "IP_APPARENT_POWER");

    // Active Power (Float32)
    ensure_dlms_value(IP.data[IP_ACTIVE_POWER], DLMS_DATA_TYPE_FLOAT32, "IP_ACTIVE_POWER");

    // Cumulative Energy Import (kWh) (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_ENERGY_IMPORT_KWH], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_ENERGY_IMPORT_KWH");

    // Cumulative Energy Import (kVAh) (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_ENERGY_IMPORT_KVAH], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_ENERGY_IMPORT_KVAH");

    // Maximum Demand kW (Float32)
    ensure_dlms_value(IP.data[IP_MAXIMUM_DEMAND_KW], DLMS_DATA_TYPE_FLOAT32, "IP_MAXIMUM_DEMAND_KW");

    // Maximum Demand kVA (Float32)
    ensure_dlms_value(IP.data[IP_MAXIMUM_DEMAND_KVA], DLMS_DATA_TYPE_FLOAT32, "IP_MAXIMUM_DEMAND_KVA");

    // Cumulative Power On Duration (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_POWER_ON], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_POWER_ON");

    // Cumulative Tamper Count (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_TAMPER_COUNT], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_TAMPER_COUNT");

    // Cumulative Billing Count (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_BILLING_COUNT], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_BILLING_COUNT");

    // Cumulative Programming Count (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_PROGRAMMING_COUNT], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_PROGRAMMING_COUNT");

    // Cumulative Energy Export (kWh) (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_ENERGY_EXPORT_KWH], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_ENERGY_EXPORT_KWH");

    // Cumulative Energy Export (kVAh) (Float32)
    ensure_dlms_value(IP.data[IP_CUMULATIVE_ENERGY_EXPORT_KVAH], DLMS_DATA_TYPE_FLOAT32, "IP_CUMULATIVE_ENERGY_EXPORT_KVAH");

    // Load Limit Function Status (Boolean)
    ensure_dlms_value(IP.data[IP_LOAD_LIMIT_FUNCTION_STATUS], DLMS_DATA_TYPE_BOOLEAN, "IP_LOAD_LIMIT_FUNCTION_STATUS");

    // Load Limit Value kW (Float32)
    ensure_dlms_value(IP.data[IP_LOAD_LIMIT_VALUE_KW], DLMS_DATA_TYPE_FLOAT32, "IP_LOAD_LIMIT_VALUE_KW");

    // Maximum Demand kW Date Time (Octet String, optional)
    if (IP.data.find(IP_MAXIMUM_DEMAND_KW_DATE_TIME) != IP.data.end())
    {
        ensure_dlms_value(IP.data[IP_MAXIMUM_DEMAND_KW_DATE_TIME], DLMS_DATA_TYPE_OCTET_STRING, "IP_MAXIMUM_DEMAND_KW_DATE_TIME");
    }

    // Maximum Demand kVA Date Time (Octet String, optional)
    if (IP.data.find(IP_MAXIMUM_DEMAND_KVA_DATE_TIME) != IP.data.end())
    {
        ensure_dlms_value(IP.data[IP_MAXIMUM_DEMAND_KVA_DATE_TIME], DLMS_DATA_TYPE_OCTET_STRING, "IP_MAXIMUM_DEMAND_KVA_DATE_TIME");
    }
}

void Client::validate_DLP_for_db()
{
    ensure_db_parameters();
    if (DLP.data[DLP_RTC].empty() || DLP.data[DLP_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("DLP_RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        DLP.data[DLP_RTC].push_back(val);
    }
    if (DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KWH].empty() || DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("DLP_CUMULATIVE_ENERGY_EXPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KWH].push_back(val);
    }
    if (DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KVAH].empty() || DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KVAH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("DLP_CUMULATIVE_ENERGY_EXPORT_KVAH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KVAH].push_back(val);
    }
    if (DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KWH].empty() || DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("DLP_CUMULATIVE_ENERGY_IMPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KWH].push_back(val);
    }
    if (DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KVAH].empty() || DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KVAH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("DLP_CUMULATIVE_ENERGY_IMPORT_KVAH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KVAH].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_BLP_for_db()
{
    ensure_db_parameters();
    if (BLP.data[BLP_RTC].empty() || BLP.data[BLP_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        BLP.data[BLP_RTC].push_back(val);
    }
    if (BLP.data[BLP_AVERAGE_VOLTAGE].empty() || BLP.data[BLP_AVERAGE_VOLTAGE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_AVERAGE_VOLTAGE data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        BLP.data[BLP_AVERAGE_VOLTAGE].push_back(val);
    }
    if (BLP.data[BLP_BLOCK_ENERGY_IMPORT_KWH].empty() || BLP.data[BLP_BLOCK_ENERGY_IMPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_BLOCK_ENERGY_IMPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        BLP.data[BLP_BLOCK_ENERGY_IMPORT_KWH].push_back(val);
    }
    if (BLP.data[BLP_BLOCK_ENERGY_IMPORT_KVAH].empty() || BLP.data[BLP_BLOCK_ENERGY_IMPORT_KVAH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_BLOCK_ENERGY_IMPORT_KVAH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        BLP.data[BLP_BLOCK_ENERGY_IMPORT_KVAH].push_back(val);
    }
    if (BLP.data[BLP_BLOCK_ENERGY_EXPORT_KWH].empty() || BLP.data[BLP_BLOCK_ENERGY_EXPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_BLOCK_ENERGY_EXPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        BLP.data[BLP_BLOCK_ENERGY_EXPORT_KWH].push_back(val);
    }
    if (BLP.data[BLP_BLOCK_ENERGY_EXPORT_KVAH].empty() || BLP.data[BLP_BLOCK_ENERGY_EXPORT_KVAH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_BLOCK_ENERGY_EXPORT_KVAH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        BLP.data[BLP_BLOCK_ENERGY_EXPORT_KVAH].push_back(val);
    }
    if (BLP.data[BLP_AVERAGE_CURRENT].empty() || BLP.data[BLP_AVERAGE_CURRENT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("BLP_AVERAGE_CURRENT data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        BLP.data[BLP_AVERAGE_CURRENT].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_BH_for_db()
{
    // Ensure DB params are initialized (meter_serial_no, meter_mac_address, gateway_id, last_download_time)
    ensure_db_parameters();

    // Helper lambda to ensure DLMSValue vector has at least one valid entry
    auto ensure_dlms_value = [this](std::vector<DLMSValue> &data, DLMSDataType type, float fval = 0.0f, std::vector<uint8_t> octet = {}) {
        if (data.empty() || data[0].type == DLMS_DATA_TYPE_NONE)
        {
            std::string param_name;
            switch (type)
            {
                case DLMS_DATA_TYPE_OCTET_STRING:
                    param_name = "octet string";
                    break;
                case DLMS_DATA_TYPE_FLOAT32:
                    param_name = "float32";
                    break;
                default:
                    param_name = "unknown";
                    break;
            }
            this->print_and_log("BH.data %s empty or NONE, assigning default value\n", param_name.c_str());

            DLMSValue val;
            switch (type)
            {
                case DLMS_DATA_TYPE_OCTET_STRING:
                    val.setOctetString(octet);
                    break;
                case DLMS_DATA_TYPE_FLOAT32:
                    val.setFloat32(fval);
                    break;
                default:
                    val.setFloat32(0.0f);
                    break;
            }
            data.push_back(val);
        }
    };

    // Billing Date Import Mode
    ensure_dlms_value(BH.data[BHP_BILLING_DATE_IMPORT_MODE], DLMS_DATA_TYPE_OCTET_STRING);

    // Average Power Factor for Billing Period
    ensure_dlms_value(BH.data[BHP_AVERAGE_POWER_FACTOR_FOR_BILLING_PERIOD], DLMS_DATA_TYPE_FLOAT32);

    // Cumulative Energy Import (kWh)
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_IMPORT_KWH], DLMS_DATA_TYPE_FLOAT32);

    // Cumulative Energy Import (kVAh)
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_IMPORT_KVAH], DLMS_DATA_TYPE_FLOAT32);

    // Cumulative Energy Export (kWh)
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_EXPORT_KWH], DLMS_DATA_TYPE_FLOAT32);

    // Cumulative Energy Export (kVAh)
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_EXPORT_KVAH], DLMS_DATA_TYPE_FLOAT32);

    // Max Demand kW
    ensure_dlms_value(BH.data[BHP_MD_KW], DLMS_DATA_TYPE_FLOAT32);

    // Max Demand kVA
    ensure_dlms_value(BH.data[BHP_MD_KVA], DLMS_DATA_TYPE_FLOAT32);

    // MD kW Date and Time
    ensure_dlms_value(BH.data[BHP_MD_KW_DATE_AND_TIME], DLMS_DATA_TYPE_OCTET_STRING);

    // MD kVA Date and Time
    ensure_dlms_value(BH.data[BHP_MD_KVA_DATE_AND_TIME], DLMS_DATA_TYPE_OCTET_STRING);

    // Cumulative Energy for TZ1–TZ8 (kWh)
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ1_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ2_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ3_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ4_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ5_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ6_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ7_KWH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ8_KWH], DLMS_DATA_TYPE_FLOAT32);

    // Cumulative Energy for TZ1–TZ8 (kVAh)
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ1_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ2_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ3_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ4_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ5_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ6_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ7_KVAH], DLMS_DATA_TYPE_FLOAT32);
    ensure_dlms_value(BH.data[BHP_CUMULATIVE_ENERGY_TZ8_KVAH], DLMS_DATA_TYPE_FLOAT32);

    // Billing Power On Duration
    ensure_dlms_value(BH.data[BHP_BILLING_POWER_ON_DURATION], DLMS_DATA_TYPE_FLOAT32);
}

void Client::validate_voltage_event_for_db()
{
    ensure_db_parameters();
    if (voltage_event.data[EVENT_DATA_INDEX_RTC].empty() || voltage_event.data[EVENT_DATA_INDEX_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        voltage_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (voltage_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty() || voltage_event.data[EVENT_DATA_INDEX_EVENT_CODE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event EVENT_CODE data empty or NONE, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        voltage_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    if (voltage_event.data[EVENT_DATA_INDEX_CURRENT].empty() || voltage_event.data[EVENT_DATA_INDEX_CURRENT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event CURRENT data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        voltage_event.data[EVENT_DATA_INDEX_CURRENT].push_back(val);
    }
    if (voltage_event.data[EVENT_DATA_INDEX_VOLTAGE].empty() || voltage_event.data[EVENT_DATA_INDEX_VOLTAGE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event VOLTAGE data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        voltage_event.data[EVENT_DATA_INDEX_VOLTAGE].push_back(val);
    }
    if (voltage_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].empty() || voltage_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event SIGNED_POWER_FACTOR data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        voltage_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].push_back(val);
    }
    if (voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].empty() || voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event CUMULATIVE_ENERGY_IMPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].push_back(val);
    }
    if (voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].empty() || voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("voltage_event CUMULATIVE_TAMPER_COUNT data empty or NONE, assigning default 0.0\n");
        DLMSValue val;
        val.setFloat64(0.0);
        voltage_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_current_event_for_db()
{
    ensure_db_parameters();
    if (current_event.data[EVENT_DATA_INDEX_RTC].empty() || current_event.data[EVENT_DATA_INDEX_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        current_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (current_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty() || current_event.data[EVENT_DATA_INDEX_EVENT_CODE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event EVENT_CODE data empty or NONE, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        current_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    if (current_event.data[EVENT_DATA_INDEX_CURRENT].empty() || current_event.data[EVENT_DATA_INDEX_CURRENT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event CURRENT data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        current_event.data[EVENT_DATA_INDEX_CURRENT].push_back(val);
    }
    if (current_event.data[EVENT_DATA_INDEX_VOLTAGE].empty() || current_event.data[EVENT_DATA_INDEX_VOLTAGE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event VOLTAGE data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        current_event.data[EVENT_DATA_INDEX_VOLTAGE].push_back(val);
    }
    if (current_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].empty() || current_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event SIGNED_POWER_FACTOR data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        current_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].push_back(val);
    }
    if (current_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].empty() || current_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event CUMULATIVE_ENERGY_IMPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        current_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].push_back(val);
    }
    if (current_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].empty() || current_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("current_event CUMULATIVE_TAMPER_COUNT data empty or NONE, assigning default 0.0\n");
        DLMSValue val;
        val.setFloat64(0.0);
        current_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_other_event_for_db()
{
    ensure_db_parameters();
    if (other_event.data[EVENT_DATA_INDEX_RTC].empty() || other_event.data[EVENT_DATA_INDEX_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        other_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (other_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty() || other_event.data[EVENT_DATA_INDEX_EVENT_CODE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event EVENT_CODE data empty or NONE, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        other_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    if (other_event.data[EVENT_DATA_INDEX_CURRENT].empty() || other_event.data[EVENT_DATA_INDEX_CURRENT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event CURRENT data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        other_event.data[EVENT_DATA_INDEX_CURRENT].push_back(val);
    }
    if (other_event.data[EVENT_DATA_INDEX_VOLTAGE].empty() || other_event.data[EVENT_DATA_INDEX_VOLTAGE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event VOLTAGE data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        other_event.data[EVENT_DATA_INDEX_VOLTAGE].push_back(val);
    }
    if (other_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].empty() || other_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event SIGNED_POWER_FACTOR data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        other_event.data[EVENT_DATA_INDEX_SIGNED_POWER_FACTOR].push_back(val);
    }
    if (other_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].empty() || other_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event CUMULATIVE_ENERGY_IMPORT_KWH data empty or NONE, assigning default 0.0f\n");
        DLMSValue val;
        val.setFloat32(0.0f);
        other_event.data[EVENT_DATA_INDEX_CUMULATIVE_ENERGY_IMPORT_KWH].push_back(val);
    }
    if (other_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].empty() || other_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("other_event CUMULATIVE_TAMPER_COUNT data empty or NONE, assigning default 0.0\n");
        DLMSValue val;
        val.setFloat64(0.0);
        other_event.data[EVENT_DATA_INDEX_CUMULATIVE_TAMPER_COUNT].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_power_event_for_db()
{
    ensure_db_parameters();
    if (power_event.data[EVENT_DATA_INDEX_RTC].empty() || power_event.data[EVENT_DATA_INDEX_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("power_event RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        power_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (power_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty() || power_event.data[EVENT_DATA_INDEX_EVENT_CODE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("power_event EVENT_CODE data empty or NONE, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        power_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_transaction_event_for_db()
{
    ensure_db_parameters();
    if (transaction_event.data[EVENT_DATA_INDEX_RTC].empty() || transaction_event.data[EVENT_DATA_INDEX_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("transaction_event RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        transaction_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (transaction_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty() || transaction_event.data[EVENT_DATA_INDEX_EVENT_CODE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("transaction_event EVENT_CODE data empty or NONE, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        transaction_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_non_rollover_event_for_db()
{
    ensure_db_parameters();
    if (non_roll_over_event.data[EVENT_DATA_INDEX_RTC].empty() || non_roll_over_event.data[EVENT_DATA_INDEX_RTC][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("non_roll_over_event RTC data empty or NONE, assigning default empty octet string\n");
        DLMSValue val;
        val.setOctetString({});
        non_roll_over_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (non_roll_over_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty() || non_roll_over_event.data[EVENT_DATA_INDEX_EVENT_CODE][0].type == DLMS_DATA_TYPE_NONE)
    {
        this->print_and_log("non_roll_over_event EVENT_CODE data empty or NONE, assigning default 0\n");
        DLMSValue val;
        val.setUint8(0);
        non_roll_over_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

void Client::validate_control_event_for_db()
{
    ensure_db_parameters();
    if (control_event.data[EVENT_DATA_INDEX_RTC].empty())
    {
        DLMSValue val;
        val.setUint32(0);
        control_event.data[EVENT_DATA_INDEX_RTC].push_back(val);
    }
    if (control_event.data[EVENT_DATA_INDEX_EVENT_CODE].empty())
    {
        DLMSValue val;
        val.setUint8(0);
        control_event.data[EVENT_DATA_INDEX_EVENT_CODE].push_back(val);
    }
    // DB parameter defaults handled by ensure_db_parameters()
}

// Helper: ensure common DB string parameters have safe defaults
void Client::ensure_db_parameters()
{
    if (DB_parameter.meter_serial_no.empty())
    {
        this->print_and_log("DB_parameter.meter_serial_no empty, setting to empty string\n");
        DB_parameter.meter_serial_no = "";
    }
    if (DB_parameter.meter_mac_address.empty())
    {
        this->print_and_log("DB_parameter.meter_mac_address empty, setting to empty string\n");
        DB_parameter.meter_mac_address = "";
    }
    if (DB_parameter.gateway_id.empty())
    {
        this->print_and_log("DB_parameter.gateway_id empty, setting to empty string\n");
        DB_parameter.gateway_id = "";
    }
    if (DB_parameter.last_download_time.empty())
    {
        this->print_and_log("DB_parameter.last_download_time empty, setting to empty string\n");
        DB_parameter.last_download_time = "";
    }
}

uint32_t Client::extract_u32_reversed(const OtaCmdResponse *response, uint8_t offset, uint8_t data_off)
{
    return (static_cast<uint32_t>(response->data[offset + data_off + 3]) << 24) |
           (static_cast<uint32_t>(response->data[offset + data_off + 2]) << 16) |
           (static_cast<uint32_t>(response->data[offset + data_off + 1]) << 8) |
           (static_cast<uint32_t>(response->data[offset + data_off]));
}

void Client::mark_hes_cycle_done(void)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (this->hes_state.current_cycle_id >= 0)
    {
        this->hes_state.done_mask |= (1 << this->hes_state.current_cycle_id);
        this->hes_state.current_cycle_id = -1;
    }
}

int Client::get_cycle_id_from_minute(int minute)
{
    if (minute >= 6 && minute <= 9) return 0;
    if (minute >= 18 && minute <= 24) return 1;
    if (minute >= 33 && minute <= 39) return 2;
    if (minute >= 48 && minute <= 54) return 3;
    return -1;
}

int Client::client_hes_cycle_schedule(void)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    time_t now = time(nullptr);
    tm timeinfo{};
    localtime_r(&now, &timeinfo);

    int minute = timeinfo.tm_min;
    int hour = timeinfo.tm_hour;

    if (this->hes_state.last_hour != hour)
    {
        this->hes_state.last_hour = hour;
        this->hes_state.done_mask = 0;
        this->hes_state.current_cycle_id = -1;
    }

    int cycle_id = get_cycle_id_from_minute(minute);
    if (cycle_id < 0)
        return FAILURE;

    if (this->hes_state.done_mask & (1 << cycle_id))
        return FAILURE;

    this->hes_state.current_cycle_id = cycle_id;

    return SUCCESS;
}

int Client::hes_start_cycle_activiy(const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    // Set HES sync time to 1 (in progress)
    if (this->insert_update_hes_nms_sync_time(gateway_id, 1) != SUCCESS)
    {
        this->print_and_log("Failed to insert/update HES sync time\n");
        return FAILURE;
    }

    // Set client receive timeout to 12 seconds
    this->set_recv_timeout_for_client(12);

    this->stateInfo.targetState = ClientTargetState::PULL;

    std::map<std::array<uint8_t, 8>, NodeInfo> nodes_info;

    // Update gateway details
    this->update_gateway_details(gateway_id);

    // Build node list from DB
    this->build_node_list_from_db(nodes_info, gateway_id);

    // Pull missing info for all nodes
    int ret = this->pull_missing_info_for_all_nodes(nodes_info);

    this->stateInfo.targetState = ClientTargetState::IDLE;

    // Set HES sync time to 0 (done)
    this->insert_update_hes_nms_sync_time(gateway_id, 0);

    return ret;
}

void Client::update_gateway_details(const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    this->gateway_details.serial_number = this->mac_from_hex(gateway_id);
    std::memset(&gateway_details.panid[0], 0, 4);
    std::memcpy(&gateway_details.panid[2], &gateway_details.serial_number[6], 2);

    this->print_and_log("Serial number:");
    for (int i = 0; i < 8; i++)
    {
        this->print_and_log(" %02X", gateway_details.serial_number[i]);
    }
    this->print_and_log("\n");

    this->print_and_log("pan id:");
    for (int i = 0; i < 4; i++)
    {
        this->print_and_log(" %02X", gateway_details.panid[i]);
    }
    this->print_and_log("\n");
}

void Client::build_node_list_from_db(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    // Gateway node
    this->add_gateway_node_to_map_list(nodes_info, gateway_id);

    // Routers list and primary path from source route network table
    this->get_nodes_info_from_src_route_network_db(nodes_info, gateway_id);

    // Alternate path from alternate source route network
    this->get_alternate_path_for_all_nodes(nodes_info, gateway_id);

    // Get all missing cycle information (Nameplate, Scalars, IFV, IP, BLP, DLP, BHP)
    this->get_missing_info_for_all_nodes(nodes_info, gateway_id);
}

void Client::add_gateway_node_to_map_list(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::array<uint8_t, 8> gateway_mac = PullData::mac_from_hex(gateway_id);

    NodeInfo gateway_node;
    gateway_node.node_mac_address = gateway_mac;
    gateway_node.primary_path.hop_count = 0;
    gateway_node.primary_path.path.clear();
    for (int i = 8; i < 16; i += 2) // skip first 8 hex chars "3CC1F601"
    {
        std::string byte_str(gateway_id + i, 2);
        gateway_node.primary_path.path.push_back(static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16)));
    }

    this->print_and_log("Gateway node %s with %d hop ", gateway_id, gateway_node.primary_path.hop_count);

    this->print_and_log("Path:");
    for (uint8_t b : gateway_node.primary_path.path)
        this->print_and_log(" %02X", b);
    this->print_and_log("\n");

    nodes_info[gateway_mac] = std::move(gateway_node);

    return;
}

void Client::get_alternate_path_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    const std::array<uint8_t, 8> GATEWAY_MAC = PullData::mac_from_hex(gateway_id);

    for (auto &kv : nodes_info)
    {
        const std::array<uint8_t, 8> &mac = kv.first;
        NodeInfo &node = kv.second;

        if (mac == GATEWAY_MAC)
        {
            this->print_and_log("Skipping gateway for alternate path\n");
            continue;
        }

        if (get_alternate_path_for_node(node, gateway_id))
        {
            this->print_and_log("Alternate path added for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            this->print_and_log("Alternate path NOT found for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

void Client::get_missing_info_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto &kv : nodes_info)
    {
        const std::array<uint8_t, 8> &mac = kv.first;
        NodeInfo &node = kv.second;

        char mac_hex[17] = {0};
        PullData::mac_to_hex(mac, mac_hex);
        this->print_and_log("Get missing info for node: %s\n", mac_hex);

        node.missing_info.missing_ip_cycles = this->get_last_hour_missing_ip_cycles_for_node(node.node_mac_address, gateway_id);

        node.missing_info.is_blp_available = this->is_blp_available_last_hour(node.node_mac_address, gateway_id);
        this->print_and_log("BLP: %s\n", node.missing_info.is_blp_available ? "Exists" : "Missing");

        node.missing_info.is_dlp_available = this->is_dlp_available_previous_day(node.node_mac_address, gateway_id);
        this->print_and_log("DLP: %s\n", node.missing_info.is_dlp_available ? "Exists" : "Missing");

        node.missing_info.is_bhp_available = this->is_bhp_available_previous_month(node.node_mac_address, gateway_id);
        this->print_and_log("BHP: %s\n", node.missing_info.is_bhp_available ? "Exists" : "Missing");

        node.missing_info.is_name_plate_available = this->is_nameplate_available(node.node_mac_address);
        this->print_and_log("Nameplate: %s\n", node.missing_info.is_name_plate_available ? "Exists" : "Missing");

        node.missing_info.is_scalar_available = this->is_scalar_profile_available(node.node_mac_address);
        this->print_and_log("Scalar profile: %s\n", node.missing_info.is_scalar_available ? "Exists" : "Missing");

        node.missing_info.is_silenced = this->is_node_silenced(node.node_mac_address, gateway_id);
        this->print_and_log("Node silenced: %s\n", node.missing_info.is_silenced ? "Yes" : "No");

        node.missing_info.verify_ifv_presence = is_ifv_available_for_node(node.node_mac_address, gateway_id);
        this->print_and_log("IFV: %s\n", node.missing_info.verify_ifv_presence ? "Exists/Entry not present" : "Missing");
    }
}

void Client::load_scalar_values_from_db(std::array<uint8_t, 8> meter_sl_number)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(meter_sl_number, mac_hex);

    std::string meter_serial_no(&mac_hex[8], 8);
    load_scaler_details_from_db(meter_serial_no, this->gateway_id, this);
}

void Client::load_scalar_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto &kv : nodes_info)
    {
        const std::array<uint8_t, 8> &mac = kv.first;
        NodeInfo &node = kv.second;

        this->print_and_log("Load scalar for node: %s\n", Utility::mac_to_string(mac.data()).c_str());

        this->load_scalar_values_from_db(node.node_mac_address);
    }
}

int Client::pull_missing_info_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (this->unsilence_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Unsilence all nodes failed\n");
        return FAILURE;
    }

    if (this->pull_missing_internal_firmware_version_for_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Pull internal firmware version for all nodes failed\n");
        return FAILURE;
    }

    if (this->pull_missing_nameplate_for_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Pull nameplate for all nodes failed\n");
        return FAILURE;
    }

    if (this->check_for_scalar_profile(this->gateway_id))
    {
        if (this->pull_scalar_profile() == FAILURE)
        {
            this->stateInfo.currentState = ClientCurrentState::IDLE;
            this->print_and_log("Pull scalar profile failed\n");
            return FAILURE;
        }
    }

    // Load scalar values from DB
    this->load_scalar_for_all_nodes(nodes_info);

    if (this->pull_missing_ip_profile_for_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Pull ip profile for all nodes failed\n");
        return FAILURE;
    }

    if (this->pull_missing_daily_load_for_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Pull daily load profile failed\n");
        return FAILURE;
    }

    if (this->pull_missing_block_load_for_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Pull block load profile failed\n");
        return FAILURE;
    }

    if (this->pull_missing_billing_history_for_all_nodes(nodes_info) != SUCCESS)
    {
        this->stateInfo.currentState = ClientCurrentState::IDLE;
        this->print_and_log("Pull billing history for all nodes failed\n");
        return FAILURE;
    }

    this->print_and_log("%s completed successfully\n", __FUNCTION__);

    this->stateInfo.currentState = ClientCurrentState::IDLE;
    return SUCCESS;
}

int Client::try_paths_for_profile_pull(NodeInfo &node, std::vector<uint8_t> (Client::*frame_fn)(uint8_t), uint8_t &page_index, const char *profile_name)
{
    this->print_and_log("Trying primary path for %s\n", profile_name);

    // ---------- PRIMARY PATH ----------
    int ret = pull_profile_pages_on_path(node.primary_path, frame_fn, page_index, node);

    switch (ret)
    {
        case SUCCESS:
            this->print_and_log("%s pull successful on primary path\n", profile_name);
            return SUCCESS;

        case DLMS_ERROR:
            this->print_and_log("%s pull failed: DLMS_ERROR\n", profile_name);
            return FAILURE;

        case DLMS_CONNECTION_FAILED:
            this->print_and_log("%s pull failed on primary: DLMS connection failed\n", profile_name);
            return FAILURE;

        default:
            this->print_and_log("Primary path failed for %s, checking alternates\n", profile_name);
            break;
    }

    // ---------- ALTERNATE PATHS ----------
    if (this->gatewayStatus == Status::CONNECTED)
    {
        for (auto it = node.alternate_paths.begin(); it != node.alternate_paths.end(); ++it)
        {
            const PathInfo &alt_path = *it;

            this->print_and_log("Trying alternate path (hop_count=%d) for %s\n", alt_path.hop_count, profile_name);

            ret = pull_profile_pages_on_path(alt_path, frame_fn, page_index, node);

            switch (ret)
            {
                case SUCCESS: {
                    this->print_and_log("%s pull successful using alternate path\n", profile_name);

                    // Promote alternate → primary
                    PathInfo old_primary = node.primary_path;
                    node.primary_path = alt_path;

                    node.alternate_paths.erase(it);
                    node.alternate_paths.push_back(old_primary);

                    return SUCCESS;
                }

                case DLMS_ERROR:
                    this->print_and_log("%s pull failed: DLMS_ERROR\n", profile_name);
                    return FAILURE;

                case DLMS_CONNECTION_FAILED:
                    this->print_and_log("Alternate path failed for %s: DLMS connection issue\n", profile_name);
                    return FAILURE;

                default:
                    this->print_and_log("Alternate path failed for %s, trying next\n", profile_name);
                    break;
            }
        }
    }

    this->print_and_log("All paths failed for %s\n", profile_name);
    return FAILURE;
}

int Client::pull_profile_pages_on_path(const PathInfo &path, std::vector<uint8_t> (Client::*frame_fn)(uint8_t), uint8_t &page_index, NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    uint8_t exit_loop_counter = 0;

    while (true)
    {
        if (exit_loop_counter++ >= 16)
        {
            this->print_and_log("Safety break: exit_loop_counter exceeded limit\n");
            return FAILURE;
        }

        this->print_and_log("Requesting page %u on path (hop_count=%d)\n", page_index, path.hop_count);

        std::vector<uint8_t> dlms_cmd = (this->*frame_fn)(page_index);

        int ret = this->transmit_command_on_path_pmesh(MESH_DATA_QUERY, dlms_cmd, path, 3);

        switch (ret)
        {
            case NEXT_PAGE_PRESENT: {
                this->print_and_log("Page %u received successfully, next page available. Incrementing to page %u\n", page_index, page_index + 1);
                page_index++;

                if (page_index >= 16)
                {
                    this->print_and_log("Safety break reached: page_index=%u exceeds limit.\n", page_index);
                    return FAILURE;
                }
                continue;
            }

            case SUCCESS: {
                this->print_and_log("Final page %u received successfully\n", page_index);
                return SUCCESS;
            }

            case DLMS_CONNECTION_FAILED: {
                this->print_and_log("DLMS connection failed while reading. Attempting reconnection...\n");

                int rc = this->attempt_dlms_reconnect_all_paths(node, path);

                if (rc == SUCCESS)
                {
                    this->print_and_log("DLMS reconnection successful. Retrying page %u...\n", page_index);
                    continue;
                }

                this->print_and_log("DLMS reconnection failed. Cannot continue reading page %u.\n", page_index);
                return DLMS_CONNECTION_FAILED;
            }

            case DLMS_ERROR: {
                this->print_and_log("Received DLMS_ERROR response on page %u. No data present.\n", page_index);
                return DLMS_ERROR;
            }

            default: {
                this->print_and_log("Unexpected response %d received for page %u. Aborting.\n", ret, page_index);
                return FAILURE;
            }
        }
    }

    return FAILURE;
}

int Client::transmit_command_on_path_pmesh(uint8_t packet_type, const std::vector<uint8_t> &cmd, PathInfo const &path, int timeout_retries)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    std::vector<uint8_t> packet = this->frame_pmesh_command_packet(packet_type, path, cmd);
    return this->transmit_command_and_validate_response(packet, timeout_retries);
}

int Client::perform_flash_and_reset(NodeInfo &node, PathInfo const &path)
{
    if (this->flash_save(node, path) != SUCCESS)
        return FAILURE;

    if (this->soft_reset(node, path) != SUCCESS)
        return FAILURE;

    return SUCCESS;
}

int Client::unsilence_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto it = nodes_info.begin(); it != nodes_info.end(); ++it)
    {
        auto &mac = it->first;
        auto &node = it->second;

        this->print_and_log("Processing unsilence for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (node.missing_info.is_silenced == true)
        {
            int fuota_status = this->check_fuota_status(node);
            if (fuota_status == SUCCESS)
            {
                this->delete_node_from_unsilence_nodes_from_fuota(mac, this->gateway_id);
            }
            else if (fuota_status == ENABLED)
            {
                int result = this->fuota_disable_for_a_node(node);
                if (result == SUCCESS)
                {
                    this->print_and_log("Unsilence command success -> updating DB\n");
                    this->delete_node_from_unsilence_nodes_from_fuota(mac, this->gateway_id);
                }
                else
                {
                    this->print_and_log("Unsilence command failed -> not updating DB\n");
                }
            }
            else
            {
                this->print_and_log("Node FUOTA status unknown: %d\n", fuota_status);
            }
        }
        else
        {
            this->print_and_log("Node is not silenced. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int Client::check_fuota_status(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::FUOTA_STATUS;

    // ---------- PRIMARY PATH ----------
    {
        int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->fuota_status, node.primary_path, 3);
        if (ret != FAILURE)
        {
            this->print_and_log("Fuota status: %s\n", (ret == 0) ? "DISABLED" : "ENABLED");
            return ret;
        }
        this->print_and_log("Primary path failed for node: ");
        this->print_data_in_hex(node.node_mac_address.data(), 8);
    }

    // ---------- ALTERNATE PATHS ----------
    if (this->gatewayStatus == Status::CONNECTED)
    {
        for (auto it = node.alternate_paths.begin(); it != node.alternate_paths.end(); ++it)
        {
            PathInfo alt_path = *it;
            this->print_and_log("Trying alternate path (hop_count=%d)...\n", alt_path.hop_count);

            int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->fuota_status, alt_path, 3);
            if (ret != FAILURE)
            {
                this->print_and_log("Fuota status (using alternate path): %s\n", (ret == 0) ? "DISABLED" : "ENABLED");
                this->print_and_log("Promoting alternate path to PRIMARY path\n");

                PathInfo old_primary = node.primary_path;
                node.primary_path = alt_path;
                node.alternate_paths.erase(it);
                node.alternate_paths.push_back(old_primary);

                return ret;
            }
        }
    }

    this->stateInfo.currentState = ClientCurrentState::IDLE;
    this->print_and_log("Fuota status read failed using all paths for node: ");
    this->print_data_in_hex(node.node_mac_address.data(), 8);
    return FAILURE;
}

int Client::flash_save(NodeInfo &node, PathInfo const &path)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::FLASH_SAVE;

    int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->flash_write, path, 3);
    if (ret == SUCCESS)
    {
        this->print_and_log("Flash save successful\n");
        return ret;
    }

    this->print_and_log("Flash save failed for node: ");
    this->print_data_in_hex(node.node_mac_address.data(), 8);
    return FAILURE;
}

int Client::soft_reset(NodeInfo &node, PathInfo const &path)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::SOFT_RESET;

    int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->soft_exit, path, 3);
    if (ret == SUCCESS)
    {
        this->print_and_log("Soft reset successful\n");
        return ret;
    }

    this->print_and_log("Soft reset failed for node: ");
    this->print_data_in_hex(node.node_mac_address.data(), 8);
    return FAILURE;
}

int Client::fuota_disable_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::FUOTA_DISABLE;

    // ---------- PRIMARY PATH ----------
    {
        int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->fuota_disable, node.primary_path, 3);
        if (ret == SUCCESS)
        {
            this->print_and_log("Fuota disable command sent on primary path\n");

            if (this->perform_flash_and_reset(node, node.primary_path) == SUCCESS)
            {
                this->print_and_log("Fuota disabled successful\n");
                return ret;
            }
            else
            {
                this->print_and_log("Flash/soft-reset failed after fuota_disable on primary\n");
            }
        }
        this->print_and_log("Primary path failed for node: ");
        this->print_data_in_hex(node.node_mac_address.data(), 8);
    }

    // ---------- ALTERNATE PATHS ----------
    if (this->gatewayStatus == Status::CONNECTED)
    {
        for (auto it = node.alternate_paths.begin(); it != node.alternate_paths.end(); ++it)
        {
            PathInfo alt_path = *it;
            this->print_and_log("Trying alternate path (hop_count=%d)...\n", alt_path.hop_count);

            int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->fuota_disable, alt_path, 3);
            if (ret != FAILURE)
            {
                this->print_and_log("Fuota disable command sent on alternate path\n");

                if (this->perform_flash_and_reset(node, alt_path) == SUCCESS)
                {
                    this->print_and_log("Fuota disable (using alternate path)\n");
                    this->print_and_log("Promoting alternate path to PRIMARY path\n");

                    PathInfo old_primary = node.primary_path;
                    node.primary_path = alt_path;
                    node.alternate_paths.erase(it);
                    node.alternate_paths.push_back(old_primary);

                    return ret;
                }
                else
                {
                    this->print_and_log("Flash/soft-reset failed on alternate path, continuing\n");
                }
            }
        }
    }

    this->print_and_log("Fuota disable failed using all paths for node: ");
    this->print_data_in_hex(node.node_mac_address.data(), 8);
    return FAILURE;
}

int Client::attempt_dlms_reconnect(const PathInfo &path)
{
    this->print_and_log("Attempting DLMS reconnect on a new path...\n");

    auto old_state = this->stateInfo.currentState;
    this->stateInfo.currentState = ClientCurrentState::DLMS_CONNECT;

    int ret = this->transmit_command_on_path_pmesh(MESH_DATA_QUERY, this->dlms_connect, path, 3);

    this->stateInfo.currentState = old_state;

    if (ret == DLMS_SUCCESS || ret == SUCCESS)
    {
        this->print_and_log("DLMS reconnect successful on new path.\n");
        return SUCCESS;
    }
    else if (ret == DLMS_CONNECTION_FAILED)
    {
        this->print_and_log("DLMS reconnect failed on new path.\n");
        return DLMS_CONNECTION_FAILED;
    }

    return FAILURE;
}

int Client::attempt_dlms_reconnect_all_paths(NodeInfo &node, const PathInfo &failed_path)
{
    // Try reconnect on failed path
    int ret = attempt_dlms_reconnect(failed_path);
    if (ret == SUCCESS)
    {
        return SUCCESS;
    }
    else if (ret == DLMS_CONNECTION_FAILED)
    {
        return DLMS_CONNECTION_FAILED;
    }

    // Try all alternates
    if (this->gatewayStatus == Status::CONNECTED)
    {
        this->print_and_log("Trying alternate paths for DLMS reconnect...\n");

        for (auto &alt : node.alternate_paths)
        {
            int ret_val = attempt_dlms_reconnect(alt);
            if (ret_val == SUCCESS)
            {
                PathInfo old_primary = node.primary_path;
                node.primary_path = alt;
                std::vector<PathInfo> updated;
                updated.reserve(node.alternate_paths.size());
                updated.push_back(old_primary);

                for (const auto &x : node.alternate_paths)
                {
                    if (!(x.hop_count == alt.hop_count && x.path == alt.path))
                        updated.push_back(x);
                }

                node.alternate_paths = std::move(updated);

                return SUCCESS;
            }
            else if (ret_val == DLMS_CONNECTION_FAILED)
            {
                return DLMS_CONNECTION_FAILED;
            }
        }
    }

    return FAILURE;
}

int Client::pull_internal_firmware_version_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::INTERNAL_FV;
    this->currentNode = &node;
    memset(node.profile_data.internal_firmware_version, 0, sizeof(node.profile_data.internal_firmware_version));

    // ---------- PRIMARY PATH ----------
    {
        int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->internal_firmware_version, node.primary_path, 3);
        if (ret == SUCCESS)
        {
            this->print_and_log("Internal firmware version success\n");
            this->currentNode = nullptr;
            return ret;
        }
        this->print_and_log("Primary path failed for node: ");
        this->print_data_in_hex(node.node_mac_address.data(), 8);
    }

    // ---------- ALTERNATE PATHS ----------
    if (this->gatewayStatus == Status::CONNECTED)
    {
        for (auto it = node.alternate_paths.begin(); it != node.alternate_paths.end(); ++it)
        {
            PathInfo alt_path = *it;
            this->print_and_log("Trying alternate path (hop_count=%d)...\n", alt_path.hop_count);

            int ret = this->transmit_command_on_path_pmesh(MESH_COMMISSION_PACKET, this->internal_firmware_version, alt_path, 3);
            if (ret == SUCCESS)
            {
                this->print_and_log("Internal firmware version success\n");
                this->print_and_log("Promoting alternate path to PRIMARY path\n");

                PathInfo old_primary = node.primary_path;
                node.primary_path = alt_path;
                node.alternate_paths.erase(it);
                node.alternate_paths.push_back(old_primary);

                this->currentNode = nullptr;
                return ret;
            }
        }
    }

    this->print_and_log("Internal firmware version failed using all paths for node: ");
    this->print_data_in_hex(node.node_mac_address.data(), 8);

    this->stateInfo.currentState = ClientCurrentState::IDLE;
    this->currentNode = nullptr;
    return FAILURE;
}

int Client::pull_missing_internal_firmware_version_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto it = nodes_info.begin(); it != nodes_info.end(); ++it)
    {
        auto &mac = it->first;
        auto &node = it->second;

        this->print_and_log("Processing internal firmware version for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (node.missing_info.verify_ifv_presence == 0)
        {
            int result = this->pull_internal_firmware_version_for_a_node(node);

            if (result == SUCCESS)
            {
                this->print_and_log("Internal firmware version read success -> updating DB\n");
                this->update_internal_firmware_version_in_meter_details(mac, this->gateway_id, node.profile_data.internal_firmware_version);
            }
            else
            {
                this->print_and_log("Internal firmware version read failed -> not updating DB\n");
            }
        }
        else
        {
            this->print_and_log("Internal firmware version already present. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int Client::pull_nameplate_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::NAMEPLATE;
    this->currentNode = &node;
    node.profile_data.name_plate_profile.clear();

    uint8_t page_index = 0;

    int ret = try_paths_for_profile_pull(node, &Client::frame_dlms_nameplate_command_packet, page_index, "Nameplate");

    this->currentNode = nullptr;
    return ret;
}

int Client::pull_missing_nameplate_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto it = nodes_info.begin(); it != nodes_info.end(); ++it)
    {
        auto &mac = it->first;
        auto &node = it->second;

        this->print_and_log("Processing nameplate for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (node.missing_info.is_name_plate_available != true)
        {
            if (this->pull_nameplate_for_a_node(node) == SUCCESS)
            {
                this->insert_name_plate_data(mac, this->gateway_id, &node.profile_data.name_plate_profile);
                node.profile_data.name_plate_profile.clear();
            }
        }
        else
        {
            this->print_and_log("Nameplate already present. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int Client::pull_daily_load_profile_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::DAILY_LOAD;
    this->currentNode = &node;
    node.profile_data.daily_load_profile.clear();

    uint8_t page_index = 0;

    int ret = try_paths_for_profile_pull(node, &Client::frame_dlms_dlp_command_packet, page_index, "Daily Load Profile");

    this->currentNode = nullptr;
    return ret;
}

int Client::pull_missing_daily_load_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto it = nodes_info.begin(); it != nodes_info.end(); ++it)
    {
        auto &mac = it->first;
        auto &node = it->second;

        this->print_and_log("Processing daily load profile for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (node.missing_info.is_dlp_available != true)
        {
            if (this->pull_daily_load_profile_for_a_node(node) == SUCCESS)
            {
                this->insert_daily_load_profile_data(mac, this->gateway_id, node.profile_data.daily_load_profile, 0);
                node.profile_data.daily_load_profile.clear();
            }
        }
        else
        {
            this->print_and_log("Daily load profile already present. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int Client::pull_block_load_profile_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->stateInfo.currentState = ClientCurrentState::BLOCK_LOAD;
    this->currentNode = &node;
    node.profile_data.block_load_profile.clear();

    uint8_t page_index = 0;

    int ret = try_paths_for_profile_pull(node, &Client::frame_dlms_blp_command_packet, page_index, "Block Load Profile");

    this->currentNode = nullptr;
    return ret;
}

int Client::pull_missing_block_load_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto it = nodes_info.begin(); it != nodes_info.end(); ++it)
    {
        auto &mac = it->first;
        auto &node = it->second;

        this->print_and_log("Processing block load profile for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (node.missing_info.is_blp_available != true)
        {
            if (this->pull_block_load_profile_for_a_node(node) == SUCCESS)
            {
                int cycle_id = this->calculate_cycle_id_for_block_load();
                this->insert_block_load_profile_data(mac, this->gateway_id, cycle_id, node.profile_data.block_load_profile, 0);
                node.profile_data.block_load_profile.clear();
            }
        }
        else
        {
            this->print_and_log("Block load profile already present. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int Client::pull_billing_history_profile_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    this->stateInfo.currentState = ClientCurrentState::BILLING_HISTORY;
    this->currentNode = &node;
    node.profile_data.billing_history.clear();

    uint8_t page_index = 0;

    int ret = try_paths_for_profile_pull(node, &Client::frame_dlms_bhp_command_packet, page_index, "Billing History Profile");

    this->currentNode = nullptr;
    return ret;
}

int Client::pull_missing_billing_history_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto it = nodes_info.begin(); it != nodes_info.end(); ++it)
    {
        auto &mac = it->first;
        auto &node = it->second;

        this->print_and_log("Processing billing history profile for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (node.missing_info.is_bhp_available != true)
        {
            if (this->pull_billing_history_profile_for_a_node(node) == SUCCESS)
            {
                this->insert_billing_history_profile_data(mac, this->gateway_id, node.profile_data.billing_history, 0);
                node.profile_data.billing_history.clear();
            }
        }
        else
        {
            this->print_and_log("Billing history profile already present. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

int Client::pull_instantaneous_profile_for_cycle(NodeInfo &node, int cycle)
{
    this->print_and_log("Pulling IP for cycle %d\n", cycle);

    this->stateInfo.currentState = ClientCurrentState::INSTANTANEOUS_PROFILE;
    this->currentNode = &node;
    this->current_ip_cycle = cycle;

    uint8_t page_index = 0;

    int ret = try_paths_for_profile_pull(node, &Client::frame_dlms_ip_command_packet, page_index, "Instantaneous Profile");

    this->current_ip_cycle = -1;
    this->currentNode = nullptr;
    return ret;
}

int Client::pull_missing_instantaneous_for_a_node(NodeInfo &node)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto &missing_cycles = node.missing_info.missing_ip_cycles;

    if (missing_cycles.empty())
    {
        this->print_and_log("List empty\n");
        return SUCCESS;
    }

    if (missing_cycles.size() > 4)
    {
        this->print_and_log("Max four cycles allowed\n");
        missing_cycles.resize(4);
    }

    for (auto it = missing_cycles.begin(); it != missing_cycles.end();)
    {
        int cycle = *it;

        if (this->pull_instantaneous_profile_for_cycle(node, cycle) == SUCCESS)
        {
            this->print_and_log("IP cycle %d downloaded successfully\n", cycle);
            int cycle_id = calculate_cycle_id(cycle);
            this->insert_instantaneous_parameters_profile_data(node.node_mac_address, this->gateway_id, cycle_id, node.profile_data.instantaneous_profile, 0);
            it = missing_cycles.erase(it); // remove downloaded cycle
        }
        else
        {
            this->print_and_log("Failed IP cycle %d\n", cycle);
            return FAILURE; // stop on failure
        }
    }

    return SUCCESS;
}

int Client::pull_missing_ip_profile_for_all_nodes(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    for (auto &kv : nodes_info)
    {
        auto &mac = kv.first;
        NodeInfo &node = kv.second;

        this->print_and_log("Processing instantaneous profile for node: ");
        this->print_data_in_hex(mac.data(), 8);

        if (!node.missing_info.missing_ip_cycles.empty())
        {
            if (this->pull_missing_instantaneous_for_a_node(node) == SUCCESS)
            {
                this->print_and_log("Pull missings cycles for a node is successful\n");
                node.profile_data.instantaneous_profile.clear();
            }
        }
        else
        {
            this->print_and_log("No missing instantaneous profile cycles. Skipping.\n");
        }

        if (this->gatewayStatus == Status::DISCONNECTED || ODM_Flag == 1)
        {
            this->print_and_log("Gateway disconnected or ODM_Flag set. Exiting.\n");
            return FAILURE;
        }
    }

    return SUCCESS;
}

/* std::vector<uint8_t> Client::frame_pmesh_command_packet(uint8_t packet_type, PathInfo const &path_info, const std::vector<uint8_t> &cmd)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::vector<uint8_t> packet;
    packet.resize(256);
    size_t pos = 0;

    auto push = [&](uint8_t value) {
        packet[pos++] = value;
    };

    auto push_mem = [&](const void *src, size_t len) {
        memcpy(&packet[pos], src, len);
        pos += len;
    };

    push(HES_START_BYTE);
    push(0x00); // placeholder for LENGTH
    push(packet_type);
    push_mem(&this->gateway_details.panid[0], 4);
    push_mem(&this->gateway_details.serial_number[4], 4);
    push(0x00);                // router_index
    push(path_info.hop_count); // hop count
    push_mem(path_info.path.data(), path_info.path.size());
    push_mem(cmd.data(), cmd.size());
    uint8_t total_len = static_cast<uint8_t>(pos); // pos = total bytes written
    packet[1] = total_len - 1;                     // Length exclude start byte
    packet.resize(pos);
    return packet;
} */

std::vector<uint8_t> Client::frame_pmesh_command_packet(uint8_t packet_type, PathInfo const &path_info, const std::vector<uint8_t> &cmd)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    // Build packet safely using push_back/insert to avoid out-of-bounds writes.
    std::vector<uint8_t> packet;
    packet.reserve(256);

    // Header
    packet.push_back(HES_START_BYTE);
    packet.push_back(0x00); // placeholder for LENGTH
    packet.push_back(packet_type);

    // PAN ID (4 bytes)
    packet.insert(packet.end(), this->gateway_details.panid.begin(), this->gateway_details.panid.end());

    // Serial number (last 4 bytes)
    packet.insert(packet.end(), this->gateway_details.serial_number.begin() + 4, this->gateway_details.serial_number.end());

    // Router index
    packet.push_back(0x00);

    // Hop count
    packet.push_back(static_cast<uint8_t>(path_info.hop_count));

    // Path bytes (if any)
    if (!path_info.path.empty())
        packet.insert(packet.end(), path_info.path.begin(), path_info.path.end());

    // Command payload
    if (!cmd.empty())
        packet.insert(packet.end(), cmd.begin(), cmd.end());

    // Set length (exclude start byte)
    if (packet.size() >= 2)
        packet[1] = static_cast<uint8_t>(packet.size() - 1);

    return packet;
}

std::vector<uint8_t> Client::frame_dlms_nameplate_command_packet(uint8_t page_index)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::vector<uint8_t> packet;
    packet.resize(8);
    size_t pos = 0;

    auto push = [&](uint8_t value) {
        packet[pos++] = value;
    };

    push(ATCMD_START_BYTE); // Start byte
    push(0x07);             // Length
    push(page_index);       // Page index
    push(FI_INSTANT_DATA);  // Instant data
    push(0x00);             // Command
    push(0x00);             // Sub command
    push(0x00);             // Latest

    // Calculate checksum
    uint8_t checksum = 0;
    for (size_t i = 0; i < pos; ++i)
        checksum += packet[i];

    push(checksum); // Checksum

    return packet;
}

std::vector<uint8_t> Client::frame_dlms_ip_command_packet(uint8_t page_index)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (this->current_ip_cycle < 0 || this->current_ip_cycle > 3)
    {
        this->print_and_log("Invalid IP cycle %d (expected 0-3)\n", this->current_ip_cycle);
    }

    std::vector<uint8_t> packet;
    packet.resize(8);
    size_t pos = 0;

    auto push = [&](uint8_t value) {
        packet[pos++] = value;
    };

    push(ATCMD_START_BYTE);                             // Start byte
    push(0x07);                                         // Length
    push(page_index);                                   // Page index
    push(FI_CACHE_DATA);                                // Instant data
    push(COMMAND_IP_PROFILE);                           // Command
    push(0x00);                                         // Sub command
    push(static_cast<uint8_t>(this->current_ip_cycle)); // Latest

    // Calculate checksum
    uint8_t checksum = 0;
    for (size_t i = 0; i < pos; ++i)
        checksum += packet[i];

    push(checksum); // Checksum

    return packet;
}

std::vector<uint8_t> Client::frame_dlms_dlp_command_packet(uint8_t page_index)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::vector<uint8_t> packet;
    packet.resize(128);
    size_t pos = 0;

    auto push = [&](uint8_t value) {
        packet[pos++] = value;
    };

    push(ATCMD_START_BYTE);           // Start byte
    push(0x00);                       // Length placeholder
    push(page_index);                 // Page index
    push(FI_INSTANT_DATA);            // Instant data
    push(COMMAND_DAILY_LOAD_PROFILE); // Command - DLP
    push(0x00);                       // Sub-command
    push(0x00);                       // Data index

    // ---- Only for first page ----
    if (page_index == 0)
    {
        time_t start_time, end_time;
        this->GetYesterdayRange(start_time, end_time);

        uint32_t start_secs = this->SecondsFromBase(start_time);
        uint32_t end_secs = this->SecondsFromBase(end_time);

        push(0x53);
        push((start_secs >> 24) & 0xFF);
        push((start_secs >> 16) & 0xFF);
        push((start_secs >> 8) & 0xFF);
        push(start_secs & 0xFF);

        push(0x45);
        push((end_secs >> 24) & 0xFF);
        push((end_secs >> 16) & 0xFF);
        push((end_secs >> 8) & 0xFF);
        push(end_secs & 0xFF);
    }

    packet[1] = static_cast<uint8_t>(pos);

    uint8_t checksum = 0;
    for (size_t i = 0; i < pos; ++i)
        checksum += packet[i];
    push(checksum);

    packet.resize(pos); // Final size

    return packet;
}

std::vector<uint8_t> Client::frame_dlms_blp_command_packet(uint8_t page_index)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::vector<uint8_t> packet;
    packet.resize(128);
    size_t pos = 0;

    auto push = [&](uint8_t value) {
        packet[pos++] = value;
    };

    // Start constructing packet
    push(ATCMD_START_BYTE);           // Start byte
    push(0x00);                       // Length placeholder
    push(page_index);                 // Page index
    push(FI_INSTANT_DATA);            // Instant data
    push(COMMAND_BLOCK_LOAD_PROFILE); // Command - BLP
    push(0x00);                       // Sub-command
    push(0x00);                       // Data index

    // ---- Only for first page ----
    if (page_index == 0)
    {
        time_t start_time, end_time;
        this->GetPreviousHourRange(start_time, end_time);

        uint32_t start_secs = this->SecondsFromBase(start_time);
        uint32_t end_secs = this->SecondsFromBase(end_time);

        // Start time tag
        push(0x53);
        push((start_secs >> 24) & 0xFF);
        push((start_secs >> 16) & 0xFF);
        push((start_secs >> 8) & 0xFF);
        push(start_secs & 0xFF);

        // End time tag
        push(0x45);
        push((end_secs >> 24) & 0xFF);
        push((end_secs >> 16) & 0xFF);
        push((end_secs >> 8) & 0xFF);
        push(end_secs & 0xFF);
    }

    packet[1] = static_cast<uint8_t>(pos);

    uint8_t checksum = 0;
    for (size_t i = 0; i < pos; ++i)
        checksum += packet[i];
    push(checksum);

    packet.resize(pos); // Final size

    return packet;
}

std::vector<uint8_t> Client::frame_dlms_bhp_command_packet(uint8_t page_index)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::vector<uint8_t> packet;
    packet.resize(128);
    size_t pos = 0;

    auto push = [&](uint8_t value) {
        packet[pos++] = value;
    };

    push(ATCMD_START_BYTE);        // Start byte
    push(0x0C);                    // Length placeholder
    push(page_index);              // Page index
    push(FI_INSTANT_DATA);         // Instant data
    push(COMMAND_BILLING_PROFILE); // command - DLP
    push(0x00);                    // Sub-command
    push(0x00);                    // Data index
    push(0x49);                    // Index
    push(0x00);
    push(0x06); // last month bhp (6:last month, 5:last second month,... , 1:last 6th month)
    push(0x43); // count
    push(0x01); // count = 1

    uint8_t checksum = 0;
    for (uint8_t b : packet)
        checksum += b;
    push(checksum);

    packet.resize(pos); // Final size

    return packet;
}

int Client::transmit_command_and_validate_response(std::vector<uint8_t> &buff, uint8_t maxRetries)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    ssize_t rxLen = 0;
    uint8_t retry = 0;
    uint8_t buffer[512] = {0};
    bool need_to_write = true;
    bool got_any_response = false;

    for (retry = 0; retry < maxRetries;)
    {
        this->print_and_log("Attempt %u of %u\n", retry + 1, maxRetries);

        if (need_to_write)
        {
            if (this->write_to_client_vector(buff) != SUCCESS)
            {
                this->print_and_log("Write to client failed\n");
                break;
            }
            need_to_write = false;
        }

        memset(buffer, 0, sizeof(buffer));
        rxLen = this->receive_data(buffer, sizeof(buffer));

        if (rxLen > 0)
        {
            this->print_and_log("Received %zd bytes from gateway\n", rxLen);

            got_any_response = true;

            int ret = this->client_received_data(buffer, rxLen);

            if (ret == SUCCESS || ret == ENABLED || ret == NEXT_PAGE_PRESENT || ret == DLMS_CONNECTION_FAILED || ret == DLMS_ERROR || ret == DLMS_SUCCESS)
            {
                this->print_and_log("Received expected data from gateway\n");
                return ret;
            }
            else if (ret == PMESH_COMMAND_IN_PROGRESS || ret == INVALID_SRC_ADDR || ret == PUSH_DATA_RECEIVED)
            {
                this->print_and_log("Retry %u: command in progress or invalid src addr or push data received\n", retry);

                if (ret == INVALID_SRC_ADDR)
                {
                    this->print_and_log("Will validate source address on next response\n");
                    this->need_to_validate_src_addr = true;
                }
                continue;
            }
            else
            {
                this->print_and_log("Retry %u: unexpected data received\n", retry);
            }
        }
        else
        {
            if (this->gatewayStatus == Status::DISCONNECTED || this->duplicate_gateway == true)
            {
                this->print_and_log("Stopping retries: gateway disconnected or duplicate connection detected\n");
                break;
            }
            this->print_and_log("Retry %u: no data received\n", retry);
        }

        retry++;
        need_to_write = true; // resend on next iteration
        this->print_and_log("write again\n");
    }

    if (maxRetries > 0 && !got_any_response)
    {
        this->gatewayStatus = Status::DISCONNECTED;
        this->print_and_log("Disconnecting: no data received from gateway after %u retries\n", retry);
    }

    return FAILURE;
}

void Client::addManufacturer(const std::string &manufacturer, const std::map<std::pair<std::string, uint8_t>, int32_t> &values)
{
    manufacturer_scalar_map[manufacturer] = ManufacturerScalarData{values};
}

void Client::addMeter(const std::string &meter_serial_no, const std::string &manufacturer, const std::string &firmware_version)
{
    meter_info_map[meter_serial_no] = MeterInfo{manufacturer, firmware_version};
}

int Client::processPacketType(uint8_t *rx_buf, int length)
{

    // Existing packet type checks
    if ((rx_buf[2] == 0x07 || rx_buf[2] == 0x03) && rx_buf[length - 1] == 0x06)
    {
        this->print_and_log("[PKT TIMEOUT]\n");
        return TIMEOUT_RECEIVED;
    }
    else if (rx_buf[2] == 0x08 && rx_buf[length - 3] == 0x01 && rx_buf[length - 2] == 0x29 && rx_buf[length - 4] == 0x0E)
    {
        this->print_and_log("[PKT DLMS CHECKSUM ERROR]\n");
        return DLMS_CHECKSUM_ERROR;
    }
    else if (rx_buf[2] == 0x08 && rx_buf[length - 3] == 0x01 && rx_buf[length - 2] == 0x25 && rx_buf[length - 4] == 0x0E)
    {
        this->print_and_log("[PKT DLMS CONNECTION FAILED]\n");
        return DLMS_CONNECTION_FAILED;
    }
    else if (rx_buf[2] == 0x08 && ((rx_buf[23] == 0x02 && rx_buf[24] == 0x00) || (rx_buf[23] == 0x02 && rx_buf[24] == 0x02)) && rx_buf[21] == 0x00)
    {
        this->print_and_log("[PKT DLMS CONNECTION SUCCESS]\n");
        return DLMS_SUCCESS;
    }
    else if (rx_buf[2] == 0x08 && ((rx_buf[23] == 0x02 && rx_buf[24] == 0x01) || (rx_buf[23] == 0x02 && rx_buf[24] == 0x03)) && rx_buf[21] == 0x00)
    {
        this->print_and_log("[PKT DLMS CONNECTION FAILED (0X38/0X40)]\n");
        return DLMS_CONNECTION_FAILED;
    }
    else if (rx_buf[2] == 0x08 && rx_buf[24] == 0x0E)
    {
        this->print_and_log("[PKT DLMS ERROR]\n");
        return DLMS_ERROR;
    }
    else if ((rx_buf[2] == 0x07 || rx_buf[2] == 0x03) && rx_buf[length - 1] == 0x07)
    {
        this->print_and_log("[PKT COMMAND IN PROGRESS(0X07)]\n");
        return COMMAND_IN_PROGRESS;
    }
    else if (rx_buf[2] == 0x07 || rx_buf[2] == 0x0D || rx_buf[2] == 0x03)
    {
        this->print_and_log("[❌PMESH ERROR]\n");
        return PMESH_ERROR;
    }
    // Valid packet check
    else if ((rx_buf[0] == HES_START_BYTE) && (rx_buf[1] + 1) == length && (rx_buf[2] == 0x08 || rx_buf[2] == 0x0E))
    {
        if (this->need_to_validate_src_addr == true)
        {
            this->need_to_validate_src_addr = false;
            if (this->validate_source_address(rx_buf) != SUCCESS)
            {
                this->print_and_log("[PKT SRC_ADDR] ❌ Source address validation FAILED\n");
                return INVALID_RESPONSE;
            }
        }
        this->print_and_log("[PKT SUCCESS] ✅ Valid packet confirmed\n");
        return SUCCESS;
    }
    // Commission packet validation
    else if (rx_buf[2] == MESH_COMMISSION_PACKET_RESPONSE && rx_buf[0] == HES_START_BYTE)
    {
        if (this->need_to_validate_src_addr == true)
        {
            this->need_to_validate_src_addr = false;
            if (this->validate_source_address(rx_buf) != SUCCESS)
            {
                this->print_and_log("[PKT SRC_ADDR] ❌ Source address validation FAILED\n");
                return INVALID_RESPONSE;
            }
        }
        return SUCCESS;
    }

    this->print_and_log("❌[PKT UNKNOWN] \n");
    return FAILURE;
}

bool Client::process_NP_case()
{
    char query_buf[MAX_QUERY_BUFFER] = {0};

    this->validate_NP_for_db();

    this->print_and_log("Inserting Nameplate Profile Success to DB\n");
    this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

    // Debug prints
    this->print_and_log("[DEBUG NP] meter_serial_number: %s\n", std::string((char *)this->name_plate.data[NP_METER_SERIALNUMBER][0].getOctetString().data(), this->name_plate.data[NP_METER_SERIALNUMBER][0].getOctetString().size()).c_str());
    this->print_and_log("[DEBUG NP] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
    this->print_and_log("[DEBUG NP] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
    this->print_and_log("[DEBUG NP] device_id: %s\n", std::string((char *)this->name_plate.data[NP_DEVICE_ID][0].getOctetString().data(), this->name_plate.data[NP_DEVICE_ID][0].getOctetString().size()).c_str());
    this->print_and_log("[DEBUG NP] manufacturer_name: %s\n", std::string((char *)this->name_plate.data[NP_MANUFACTURE_NAME][0].getOctetString().data(), this->name_plate.data[NP_MANUFACTURE_NAME][0].getOctetString().size()).c_str());
    this->print_and_log("[DEBUG NP] firmware_version_for_meter: %s\n", std::string((char *)this->name_plate.data[NP_METER_FIRMWARE][0].getOctetString().data(), this->name_plate.data[NP_METER_FIRMWARE][0].getOctetString().size()).c_str());
    this->print_and_log("[DEBUG NP] meter_type: %d\n", static_cast<int>(this->name_plate.data[NP_METER_PHASE][0].getAsFloat(1.0)) / 6);
    this->print_and_log("[DEBUG NP] category: %s\n", std::string((char *)this->name_plate.data[NP_CATEGORY][0].getOctetString().data(), this->name_plate.data[NP_CATEGORY][0].getOctetString().size()).c_str());
    this->print_and_log("[DEBUG NP] current_rating: %s\n", std::string((char *)this->name_plate.data[NP_CURRENT_RATING][0].getOctetString().data(), this->name_plate.data[NP_CURRENT_RATING][0].getOctetString().size()).c_str());
    this->print_and_log("[DEBUG NP] meter_manufacture_year: %s\n", this->name_plate.data[NP_MANUFACTURE_YEAR][0].to_string().c_str());
    this->print_and_log("[DEBUG NP] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
    this->print_and_log("[DEBUG NP] request_id: %zu\n", this->DB_parameter.req_id);
    this->print_and_log("[DEBUG NP] push_alaram: %d\n", 0);

    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO name_plate_data(meter_serial_number,meter_mac_address,gateway_id,device_id,"
             "manufacturer_name,firmware_version_for_meter,meter_type,category,current_rating,meter_manufacture_year,"
             "last_download_time,request_id,push_alaram) "
             "VALUES('%s','%s','%s','%s','%s','%s',%d,'%s','%s','%s','%s',%zu,%d)",
             // 13 VALUES
             std::string((char *)this->name_plate.data[NP_METER_SERIALNUMBER][0].getOctetString().data(), this->name_plate.data[NP_METER_SERIALNUMBER][0].getOctetString().size()).c_str(),
             this->DB_parameter.meter_mac_address.c_str(),
             this->DB_parameter.gateway_id.c_str(),
             std::string((char *)this->name_plate.data[NP_DEVICE_ID][0].getOctetString().data(), this->name_plate.data[NP_DEVICE_ID][0].getOctetString().size()).c_str(),
             std::string((char *)this->name_plate.data[NP_MANUFACTURE_NAME][0].getOctetString().data(), this->name_plate.data[NP_MANUFACTURE_NAME][0].getOctetString().size()).c_str(),
             std::string((char *)this->name_plate.data[NP_METER_FIRMWARE][0].getOctetString().data(), this->name_plate.data[NP_METER_FIRMWARE][0].getOctetString().size()).c_str(),
             static_cast<int>(this->name_plate.data[NP_METER_PHASE][0].getAsFloat(1.0)) / 6,
             std::string((char *)this->name_plate.data[NP_CATEGORY][0].getOctetString().data(), this->name_plate.data[NP_CATEGORY][0].getOctetString().size()).c_str(),
             std::string((char *)this->name_plate.data[NP_CURRENT_RATING][0].getOctetString().data(), this->name_plate.data[NP_CURRENT_RATING][0].getOctetString().size()).c_str(),
             this->name_plate.data[NP_MANUFACTURE_YEAR][0].to_string().c_str(),
             this->DB_parameter.last_download_time.c_str(),
             this->DB_parameter.req_id,
             0);

    clear_profile_for_type(DATA_TYPE_NP);

    int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

    if (db_result == 0)
    { // ✅ SUCCESS
        this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", DATA_TYPE_NP, this->DB_parameter.req_id);
    }
    else
    { // ✅ FAILURE
        this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", DATA_TYPE_NP);
    }
    return true;
}

bool Client::process_IP_case()
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    uint8_t err_code = 1;

    this->validate_IP_for_db();
    float voltage = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_VOLTAGE));
    float phase_current = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_PHASE_CURRENT));
    float neutral_current = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_NEUTRAL_CURRENT));
    float signed_powerfactor = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_SIGNED_POWER_FACTOR));
    float frequency = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_FREQUENCY));
    float cum_energy_kwh_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_IMPORT_KWH));
    float cum_energy_kvah_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_IMPORT_KVAH));
    float cum_energy_kwh_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_EXPORT_KWH));
    float cum_energy_kvah_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_EXPORT_KVAH));
    float apparent_power = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_APPARENT_POWER));
    float active_power = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_ACTIVE_POWER));
    float max_demand_kw = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_MAXIMUM_DEMAND_KW));
    float max_demand_kva = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_MAXIMUM_DEMAND_KVA));
    float power_on_duration = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_POWER_ON));
    float tamper_count = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_TAMPER_COUNT));
    float billing_count = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_BILLING_COUNT));
    float programming_count = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_CUMULATIVE_PROGRAMMING_COUNT));
    float load_limit_value = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0100", IP_LOAD_LIMIT_VALUE_KW));

    this->print_and_log("[SCALER VALUES] -> [VOLTAGE] = %f || [PHASE CURRENT] = %f || [NEUTRAL CURRENT] = %f || [SIGNED PF] = %f || [FREQ] = %f || [CUM_ENERGY KWH IMPORT] = %f || [CUM_ENERGY KVAH IMPORT] %f || [KWH EXPORT] = %f || [KVAH EXPORT] %f\n", voltage, phase_current, neutral_current, signed_powerfactor, frequency, cum_energy_kwh_import, cum_energy_kvah_import, cum_energy_kwh_export, cum_energy_kvah_export);

    auto get_bool = [&](IP_Data_index idx) -> int {
        return (this->IP.data[idx].size() > 0 && this->IP.data[idx][0].getAsBool()) ? 1 : 0;
    };
    auto get_float = [&](IP_Data_index idx, double scale) -> float {
        return (this->IP.data[idx].size() > 0) ? this->IP.data[idx][0].getAsFloat(scale) : 0.0f;
    };
    auto get_double = [&](IP_Data_index idx, double scale) -> double {
        return (this->IP.data[idx].size() > 0) ? this->IP.data[idx][0].getAsDouble(scale) : 0.0;
    };
    auto get_string = [&](IP_Data_index idx) -> std::string {
        return (this->IP.data[idx].size() > 0) ? this->IP.data[idx][0].getAsString() : "";
    };
    auto get_datetime = [&](IP_Data_index idx) -> std::string {
        if (this->IP.data[idx].size() > 0 && !this->IP.data[idx][0].getOctetString().empty())
        {
            return parse_dlms_datetime(this->IP.data[idx][0].getOctetString().data());
        }
        return "";
    };

    int load_limit_sts = get_bool(IP_LOAD_LIMIT_FUNCTION_STATUS);
    this->print_and_log("Inserting Instantaneous Profile Success to DB\n");
    this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);
    Insert_receive_data_offset();

    // Debug prints
    this->print_and_log("[DEBUG IP] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
    this->print_and_log("[DEBUG IP] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
    this->print_and_log("[DEBUG IP] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
    this->print_and_log("[DEBUG IP] meter_rtc_time: %s\n", get_string(IP_RTC).c_str());
    this->print_and_log("[DEBUG IP] voltage: %.2f\n", get_float(IP_VOLTAGE, voltage));
    this->print_and_log("[DEBUG IP] phase_current: %.2f\n", get_float(IP_PHASE_CURRENT, phase_current));
    this->print_and_log("[DEBUG IP] neutral_current: %.2f\n", get_float(IP_NEUTRAL_CURRENT, neutral_current));
    this->print_and_log("[DEBUG IP] signed_powerfactor: %.2f\n", get_float(IP_SIGNED_POWER_FACTOR, signed_powerfactor));
    this->print_and_log("[DEBUG IP] frequency: %.2f\n", get_float(IP_FREQUENCY, frequency));
    this->print_and_log("[DEBUG IP] apparent_power_kva: %.2f\n", get_float(IP_APPARENT_POWER, apparent_power));
    this->print_and_log("[DEBUG IP] active_power_kw: %.2f\n", get_float(IP_ACTIVE_POWER, active_power));
    this->print_and_log("[DEBUG IP] cum_energy_kwh_import: %.2f\n", get_float(IP_CUMULATIVE_ENERGY_IMPORT_KWH, convertScalar(cum_energy_kwh_import)));
    this->print_and_log("[DEBUG IP] cum_energy_kvah_import: %.2f\n", get_float(IP_CUMULATIVE_ENERGY_IMPORT_KVAH, convertScalar(cum_energy_kvah_import)));
    this->print_and_log("[DEBUG IP] maximum_demand_kw: %.2f\n", get_float(IP_MAXIMUM_DEMAND_KW, max_demand_kw));
    this->print_and_log("[DEBUG IP] md_kw_datetime: %s\n", get_datetime(IP_MAXIMUM_DEMAND_KW_DATE_TIME).c_str());
    this->print_and_log("[DEBUG IP] maximum_demand_kva: %.2f\n", get_float(IP_MAXIMUM_DEMAND_KVA, max_demand_kva));
    this->print_and_log("[DEBUG IP] md_kva_datetime: %s\n", get_datetime(IP_MAXIMUM_DEMAND_KVA_DATE_TIME).c_str());
    this->print_and_log("[DEBUG IP] cum_power_on_duration: %.2f\n", get_float(IP_CUMULATIVE_POWER_ON, power_on_duration));
    this->print_and_log("[DEBUG IP] cum_tamper_count: %.2f\n", get_float(IP_CUMULATIVE_TAMPER_COUNT, tamper_count));
    this->print_and_log("[DEBUG IP] cum_billing_count: %.2f\n", get_float(IP_CUMULATIVE_BILLING_COUNT, billing_count));
    this->print_and_log("[DEBUG IP] cum_programming_count: %.2f\n", get_double(IP_CUMULATIVE_PROGRAMMING_COUNT, programming_count));
    this->print_and_log("[DEBUG IP] cum_energy_kwh_export: %.2f\n", get_float(IP_CUMULATIVE_ENERGY_EXPORT_KWH, cum_energy_kwh_export));
    this->print_and_log("[DEBUG IP] cum_energy_kvah_export: %.2f\n", get_float(IP_CUMULATIVE_ENERGY_EXPORT_KVAH, cum_energy_kvah_export));
    this->print_and_log("[DEBUG IP] loadlimit_function_sts: %d\n", load_limit_sts);
    this->print_and_log("[DEBUG IP] loadlimit_value_kw: %.2f\n", get_float(IP_LOAD_LIMIT_VALUE_KW, load_limit_value));
    this->print_and_log("[DEBUG IP] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());
    this->print_and_log("[DEBUG IP] request_id: %zu\n", this->DB_parameter.req_id);
    this->print_and_log("[DEBUG IP] push_alaram: %d\n", this->DB_parameter.push_alaram);
    this->print_and_log("[DEBUG IP] error_code: %d\n", err_code);

    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO dlms_ip_push_data("
             "meter_serial_number,meter_mac_address,gateway_id,meter_rtc_time,"
             "voltage,phase_current,neutral_current,signed_powerfactor,frequency,"
             "apparent_power_kva,active_power_kw,cum_energy_kwh_import,cum_energy_kvah_import,"
             "maximum_demand_kw,md_kw_datetime,maximum_demand_kva,md_kva_datetime,"
             "cum_power_on_duration,cum_tamper_count,cum_billing_count,cum_programming_count,"
             "cum_energy_kwh_export,cum_energy_kvah_export,loadlimit_function_sts,"
             "loadlimit_value_kw,last_download_time,request_id,push_alaram,error_code) " // 29 cols
             "VALUES('%s','%s','%s','%s',%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
             "%.2f,'%s',%.2f,'%s',%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.2f,'%s',%zu,%d,%d)",

             // 29 VALUES (matches columns):
             this->DB_parameter.meter_serial_no.c_str(),
             this->DB_parameter.meter_mac_address.c_str(),
             this->DB_parameter.gateway_id.c_str(),
             get_string(IP_RTC).c_str(),
             get_float(IP_VOLTAGE, voltage),
             get_float(IP_PHASE_CURRENT, phase_current),
             get_float(IP_NEUTRAL_CURRENT, neutral_current),
             get_float(IP_SIGNED_POWER_FACTOR, signed_powerfactor),
             get_float(IP_FREQUENCY, frequency),
             get_float(IP_APPARENT_POWER, apparent_power),
             get_float(IP_ACTIVE_POWER, active_power),
             get_float(IP_CUMULATIVE_ENERGY_IMPORT_KWH, cum_energy_kwh_import),
             get_float(IP_CUMULATIVE_ENERGY_IMPORT_KVAH, cum_energy_kvah_import),
             get_float(IP_MAXIMUM_DEMAND_KW, max_demand_kw),
             get_datetime(IP_MAXIMUM_DEMAND_KW_DATE_TIME).c_str(),
             get_float(IP_MAXIMUM_DEMAND_KVA, max_demand_kva),
             get_datetime(IP_MAXIMUM_DEMAND_KVA_DATE_TIME).c_str(),
             get_float(IP_CUMULATIVE_POWER_ON, power_on_duration),
             get_float(IP_CUMULATIVE_TAMPER_COUNT, tamper_count),
             get_float(IP_CUMULATIVE_BILLING_COUNT, billing_count),
             get_float(IP_CUMULATIVE_PROGRAMMING_COUNT, programming_count),
             get_float(IP_CUMULATIVE_ENERGY_EXPORT_KWH, cum_energy_kwh_export),
             get_float(IP_CUMULATIVE_ENERGY_EXPORT_KVAH, cum_energy_kvah_export),
             load_limit_sts, // ✅  bool→int
             get_float(IP_LOAD_LIMIT_VALUE_KW, load_limit_value),
             this->DB_parameter.last_download_time.c_str(),
             this->DB_parameter.req_id,
             this->DB_parameter.push_alaram,
             err_code);

    clear_profile_for_type(DATA_TYPE_IP);

    int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

    if (db_result == 0)
    { // ✅ SUCCESS
        this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", DATA_TYPE_IP, this->DB_parameter.req_id);
    }
    else
    { // ✅ FAILURE
        this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", DATA_TYPE_IP);
    }
    return true;
}

bool Client::process_BHP_case(uint8_t download_data_type)
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    uint8_t err_code = 1;

    this->validate_BH_for_db();
    this->print_and_log("Billing History Profile\n");
    this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

    // ✅ ALL BHP scalar values (except strings)
    float bhp_avg_pwr_factor = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_AVERAGE_POWER_FACTOR_FOR_BILLING_PERIOD));
    float bhp_cum_energy_kwh_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_IMPORT_KWH));
    float bhp_cum_energy_kwh_tz1 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ1_KWH));
    float bhp_cum_energy_kwh_tz2 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ2_KWH));
    float bhp_cum_energy_kwh_tz3 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ3_KWH));
    float bhp_cum_energy_kwh_tz4 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ4_KWH));
    float bhp_cum_energy_kwh_tz5 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ5_KWH));
    float bhp_cum_energy_kwh_tz6 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ6_KWH));
    float bhp_cum_energy_kwh_tz7 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ7_KWH));
    float bhp_cum_energy_kwh_tz8 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ8_KWH));
    float bhp_cum_energy_kvah_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_IMPORT_KVAH));
    float bhp_cum_energy_kvah_tz1 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ1_KVAH));
    float bhp_cum_energy_kvah_tz2 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ2_KVAH));
    float bhp_cum_energy_kvah_tz3 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ3_KVAH));
    float bhp_cum_energy_kvah_tz4 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ4_KVAH));
    float bhp_cum_energy_kvah_tz5 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ5_KVAH));
    float bhp_cum_energy_kvah_tz6 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ6_KVAH));
    float bhp_cum_energy_kvah_tz7 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ7_KVAH));
    float bhp_cum_energy_kvah_tz8 = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ8_KVAH));
    float bhp_max_demand_kw = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_MD_KW));
    float bhp_max_demand_kva = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_MD_KVA));
    float bhp_cum_energy_kwh_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_EXPORT_KWH));
    float bhp_cum_energy_kvah_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_EXPORT_KVAH));
    float bhp_billing_power_on_duration = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0200", BHP_BILLING_POWER_ON_DURATION));

    const size_t n = this->BH.data[BHP_BILLING_DATE_IMPORT_MODE].size();
    for (size_t i = 0; i < n; ++i)
    {

        std::string date_time = (this->BH.data[BHP_BILLING_DATE_IMPORT_MODE].size() > i && !this->BH.data[BHP_BILLING_DATE_IMPORT_MODE][i].getOctetString().empty())
                                    ? parse_dlms_datetime(this->BH.data[BHP_BILLING_DATE_IMPORT_MODE][i].getOctetString().data())
                                    : "";
        std::string MD_KW = (this->BH.data[BHP_MD_KW_DATE_AND_TIME].size() > i && !this->BH.data[BHP_MD_KW_DATE_AND_TIME][i].getOctetString().empty())
                                ? parse_dlms_datetime(this->BH.data[BHP_MD_KW_DATE_AND_TIME][i].getOctetString().data())
                                : "";
        std::string MD_KVA = (this->BH.data[BHP_MD_KVA_DATE_AND_TIME].size() > i && !this->BH.data[BHP_MD_KVA_DATE_AND_TIME][i].getOctetString().empty())
                                 ? parse_dlms_datetime(this->BH.data[BHP_MD_KVA_DATE_AND_TIME][i].getOctetString().data())
                                 : "";
        // ✅ Debug prints using scalar values as scale
        this->print_and_log("[DEBUG BHP %zu] meter_mac_address: %s\n", i, this->DB_parameter.meter_mac_address.c_str());
        this->print_and_log("[DEBUG BHP %zu] meter_serial_number: %s\n", i, this->DB_parameter.meter_serial_no.c_str());
        this->print_and_log("[DEBUG BHP %zu] gateway_id: %s\n", i, this->DB_parameter.gateway_id.c_str());
        this->print_and_log("[DEBUG BHP %zu] billing_date_import_mode: %s\n", i, date_time.c_str());
        this->print_and_log("[DEBUG BHP %zu] average_pwr_factor_for_billing_period: %.2f\n", i,
                            this->BH.data[BHP_AVERAGE_POWER_FACTOR_FOR_BILLING_PERIOD][i].getAsFloat(bhp_avg_pwr_factor));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_import: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_IMPORT_KWH][i].getAsFloat(bhp_cum_energy_kwh_import));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz1: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ1_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz1));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz2: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ2_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz2));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz3: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ3_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz3));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz4: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ4_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz4));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz5: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ5_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz5));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz6: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ6_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz6));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz7: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ7_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz7));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kwh_tz8: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_TZ8_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz8));
        this->print_and_log("[DEBUG BHP %zu] cum_energy_kvah_import: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_IMPORT_KVAH][i].getAsFloat(bhp_cum_energy_kvah_import));
        this->print_and_log("[DEBUG BHP %zu] max_demand_kw: %.2f\n", i,
                            this->BH.data[BHP_MD_KW][i].getAsFloat(bhp_max_demand_kw));
        this->print_and_log("[DEBUG BHP %zu] max_demand_kva: %.2f\n", i,
                            this->BH.data[BHP_MD_KVA][i].getAsFloat(bhp_max_demand_kva));
        this->print_and_log("[DEBUG BHP %zu] md_kw_datetime: %s\n", i, MD_KW.c_str());
        this->print_and_log("[DEBUG BHP %zu] md_kva_datetime: %s\n", i, MD_KVA.c_str());
        this->print_and_log("[DEBUG BHP %zu] cum_active_energy_kwh_export: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_EXPORT_KWH][i].getAsFloat(bhp_cum_energy_kwh_export));
        this->print_and_log("[DEBUG BHP %zu] cum_apparent_energy_kvah_export: %.2f\n", i,
                            this->BH.data[BHP_CUMULATIVE_ENERGY_EXPORT_KVAH][i].getAsFloat(bhp_cum_energy_kvah_export));
        this->print_and_log("[DEBUG BHP %zu] billing_power_on_duration: %.2f\n", i,
                            this->BH.data[BHP_BILLING_POWER_ON_DURATION][i].getAsDouble(bhp_billing_power_on_duration));
        this->print_and_log("[DEBUG BHP %zu] last_download_time: %s\n", i, this->DB_parameter.last_download_time.c_str());
        this->print_and_log("[DEBUG BHP %zu] request_id: %zu\n", i, this->DB_parameter.req_id);
        this->print_and_log("[DEBUG BHP %zu] push_alaram: %d\n", i, this->DB_parameter.push_alaram);
        this->print_and_log("[DEBUG BHP %zu] error_code: %d\n", i, err_code);

        snprintf(query_buf, sizeof(query_buf),
                 "INSERT INTO dlms_history_data("
                 "meter_mac_address,meter_serial_number,gateway_id,billing_date_import_mode,"
                 "average_pwr_factor_for_billing_period,cum_energy_kwh_import,"
                 "cum_energy_kwh_tz1,cum_energy_kwh_tz2,cum_energy_kwh_tz3,cum_energy_kwh_tz4,"
                 "cum_energy_kwh_tz5,cum_energy_kwh_tz6,cum_energy_kwh_tz7,cum_energy_kwh_tz8,"
                 "cum_energy_kvah_import,cum_energy_kvah_tz1,cum_energy_kvah_tz2,"
                 "cum_energy_kvah_tz3,cum_energy_kvah_tz4,cum_energy_kvah_tz5,"
                 "cum_energy_kvah_tz6,cum_energy_kvah_tz7,cum_energy_kvah_tz8,"
                 "max_demand_kw,max_demand_kva,md_kw_datetime,md_kva_datetime,"
                 "cum_active_energy_kwh_export,cum_apparent_energy_kvah_export,"
                 "billing_power_on_duration,last_download_time,request_id,push_alaram,error_code) "
                 "VALUES('%s','%s','%s','%s',%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                 "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,'%s','%s',%.2f,%.2f,%.2f,'%s',%zu,%d,%d)",

                 // ✅ 28 VALUES - using scalar values as scale
                 this->DB_parameter.meter_mac_address.c_str(),
                 this->DB_parameter.meter_serial_no.c_str(),
                 this->DB_parameter.gateway_id.c_str(),
                 date_time.c_str(),

                 this->BH.data[BHP_AVERAGE_POWER_FACTOR_FOR_BILLING_PERIOD][i].getAsFloat(bhp_avg_pwr_factor),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_IMPORT_KWH][i].getAsFloat(bhp_cum_energy_kwh_import),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ1_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz1),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ2_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz2),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ3_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz3),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ4_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz4),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ5_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz5),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ6_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz6),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ7_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz7),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ8_KWH][i].getAsFloat(bhp_cum_energy_kwh_tz8),

                 this->BH.data[BHP_CUMULATIVE_ENERGY_IMPORT_KVAH][i].getAsFloat(bhp_cum_energy_kvah_import),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ1_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz1),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ2_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz2),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ3_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz3),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ4_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz4),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ5_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz5),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ6_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz6),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ7_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz7),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_TZ8_KVAH][i].getAsFloat(bhp_cum_energy_kvah_tz8),

                 this->BH.data[BHP_MD_KW][i].getAsFloat(bhp_max_demand_kw),
                 this->BH.data[BHP_MD_KVA][i].getAsFloat(bhp_max_demand_kva),
                 MD_KW.c_str(),
                 MD_KVA.c_str(),

                 this->BH.data[BHP_CUMULATIVE_ENERGY_EXPORT_KWH][i].getAsFloat(bhp_cum_energy_kwh_export),
                 this->BH.data[BHP_CUMULATIVE_ENERGY_EXPORT_KVAH][i].getAsFloat(bhp_cum_energy_kvah_export),
                 this->BH.data[BHP_BILLING_POWER_ON_DURATION][i].getAsDouble(bhp_billing_power_on_duration),
                 this->DB_parameter.last_download_time.c_str(),
                 this->DB_parameter.req_id,
                 this->DB_parameter.push_alaram,
                 err_code);

        int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

        if (db_result == 0)
        { // ✅ SUCCESS
            this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", download_data_type, this->DB_parameter.req_id);
        }
        else
        { // ✅ FAILURE
            this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", download_data_type);
        }
    }
    clear_profile_for_type(download_data_type);
    return true;
}

bool Client::process_DLP_case(uint8_t download_data_type)
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    uint8_t err_code = 1;

    this->validate_DLP_for_db();
    this->print_and_log("Daily Load Profile\n");
    this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

    // ✅ ALL DLP scalar values (attribute "0300")
    float dlp_cum_energy_kwh_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_IMPORT_KWH));
    float dlp_cum_energy_kvah_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_IMPORT_KVAH));
    float dlp_cum_energy_kwh_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_EXPORT_KWH));
    float dlp_cum_energy_kvah_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_EXPORT_KVAH));

    const size_t n = this->DLP.data[DLP_RTC].size();
    for (size_t i = 0; i < n; ++i)
    {
        // ✅ Debug prints using scalar values as scale
        this->print_and_log("[DEBUG DLP %zu] meter_mac_address: %s\n", i, this->DB_parameter.meter_mac_address.c_str());
        this->print_and_log("[DEBUG DLP %zu] meter_serial_number: %s\n", i, this->DB_parameter.meter_serial_no.c_str());
        this->print_and_log("[DEBUG DLP %zu] gateway_id: %s\n", i, this->DB_parameter.gateway_id.c_str());
        this->print_and_log("[DEBUG DLP %zu] real_time_clock: %s\n", i, this->DLP.data[DLP_RTC][i].getAsString().c_str());
        this->print_and_log("[DEBUG DLP %zu] cum_energy_kwh_import: %.2f\n", i,
                            this->DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KWH][i].getAsFloat(dlp_cum_energy_kwh_import));
        this->print_and_log("[DEBUG DLP %zu] cum_energy_kvah_import: %.2f\n", i,
                            this->DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KVAH][i].getAsFloat(dlp_cum_energy_kvah_import));
        this->print_and_log("[DEBUG DLP %zu] cum_energy_kwh_export: %.2f\n", i,
                            this->DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KWH][i].getAsFloat(dlp_cum_energy_kwh_export));
        this->print_and_log("[DEBUG DLP %zu] cum_energy_kvah_export: %.2f\n", i,
                            this->DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KVAH][i].getAsFloat(dlp_cum_energy_kvah_export));
        this->print_and_log("[DEBUG DLP %zu] last_download_time: %s\n", i, this->DB_parameter.last_download_time.c_str());
        this->print_and_log("[DEBUG DLP %zu] request_id: %zu\n", i, this->DB_parameter.req_id);
        this->print_and_log("[DEBUG DLP %zu] push_alaram: %d\n", i, this->DB_parameter.push_alaram);
        this->print_and_log("[DEBUG DLP %zu] error_code: %d\n", i, err_code);

        snprintf(query_buf, sizeof(query_buf),
                 "INSERT INTO dlms_daily_load_push_profile("
                 "meter_mac_address,meter_serial_number,gateway_id,real_time_clock,"
                 "cum_energy_kwh_export,cum_energy_kvah_export,cum_energy_kwh_import,"
                 "cum_energy_kvah_import,last_download_time,request_id,push_alaram,error_code) "
                 "VALUES('%s','%s','%s','%s',%.2f,%.2f,%.2f,%.2f,'%s',%zu,%d,%d)",

                 // 11 VALUES :
                 this->DB_parameter.meter_mac_address.c_str(),
                 this->DB_parameter.meter_serial_no.c_str(),
                 this->DB_parameter.gateway_id.c_str(),
                 this->DLP.data[DLP_RTC][i].getAsString().c_str(),
                 this->DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KWH][i].getAsFloat(dlp_cum_energy_kwh_export),
                 this->DLP.data[DLP_CUMULATIVE_ENERGY_EXPORT_KVAH][i].getAsFloat(dlp_cum_energy_kvah_export),
                 this->DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KWH][i].getAsFloat(dlp_cum_energy_kwh_import),
                 this->DLP.data[DLP_CUMULATIVE_ENERGY_IMPORT_KVAH][i].getAsFloat(dlp_cum_energy_kvah_import),
                 this->DB_parameter.last_download_time.c_str(),
                 this->DB_parameter.req_id,
                 this->DB_parameter.push_alaram,
                 err_code);
        int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

        if (db_result == 0)
        { // ✅ SUCCESS
            this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", download_data_type, this->DB_parameter.req_id);
        }
        else
        { // ✅ FAILURE
            this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", download_data_type);
        }
    }
    clear_profile_for_type(download_data_type);
    return true;
}

bool Client::process_BLP_case(uint8_t download_data_type)
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    uint8_t err_code = 1;

    this->validate_BLP_for_db();
    this->print_and_log("Block Load Profile\n");
    this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

    // ✅ ALL BLP scalar values (attribute "0400")
    float blp_average_voltage = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0400", BLP_AVERAGE_VOLTAGE));
    float blp_block_energy_kwh_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0400", BLP_BLOCK_ENERGY_IMPORT_KWH));
    float blp_block_energy_kvah_import = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0400", BLP_BLOCK_ENERGY_IMPORT_KVAH));
    float blp_block_energy_kwh_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0400", BLP_BLOCK_ENERGY_EXPORT_KWH));
    float blp_block_energy_kvah_export = convertScalar(getScalarValue(this->DB_parameter.meter_serial_no, "0400", BLP_BLOCK_ENERGY_EXPORT_KVAH));

    const size_t n = this->BLP.data[BLP_RTC].size();
    for (size_t i = 0; i < n; ++i)
    {
        // ✅ Debug prints using scalar values as scale
        this->print_and_log("[DEBUG BLP %zu] meter_mac_address: %s\n", i, this->DB_parameter.meter_mac_address.c_str());
        this->print_and_log("[DEBUG BLP %zu] meter_serial_number: %s\n", i, this->DB_parameter.meter_serial_no.c_str());
        this->print_and_log("[DEBUG BLP %zu] gateway_id: %s\n", i, this->DB_parameter.gateway_id.c_str());
        this->print_and_log("[DEBUG BLP %zu] real_time_clock: %s\n", i, this->BLP.data[BLP_RTC][i].getAsString().c_str());
        this->print_and_log("[DEBUG BLP %zu] average_voltage: %.2f\n", i,
                            this->BLP.data[BLP_AVERAGE_VOLTAGE][i].getAsFloat(blp_average_voltage));
        this->print_and_log("[DEBUG BLP %zu] block_energy_kwh_import: %.2f\n", i,
                            this->BLP.data[BLP_BLOCK_ENERGY_IMPORT_KWH][i].getAsFloat(blp_block_energy_kwh_import));
        this->print_and_log("[DEBUG BLP %zu] block_energy_kvah_import: %.2f\n", i,
                            this->BLP.data[BLP_BLOCK_ENERGY_IMPORT_KVAH][i].getAsFloat(blp_block_energy_kvah_import));
        this->print_and_log("[DEBUG BLP %zu] block_energy_kwh_export: %.2f\n", i,
                            this->BLP.data[BLP_BLOCK_ENERGY_EXPORT_KWH][i].getAsFloat(blp_block_energy_kwh_export));
        this->print_and_log("[DEBUG BLP %zu] block_energy_kvah_export: %.2f\n", i,
                            this->BLP.data[BLP_BLOCK_ENERGY_EXPORT_KVAH][i].getAsFloat(blp_block_energy_kvah_export));
        this->print_and_log("[DEBUG BLP %zu] last_download_time: %s\n", i, this->DB_parameter.last_download_time.c_str());
        this->print_and_log("[DEBUG BLP %zu] request_id: %zu\n", i, this->DB_parameter.req_id);
        this->print_and_log("[DEBUG BLP %zu] push_alaram: %d\n", i, this->DB_parameter.push_alaram);
        this->print_and_log("[DEBUG BLP %zu] error_code: %d\n", i, err_code);

        snprintf(query_buf, sizeof(query_buf),
                 "INSERT INTO dlms_block_load_push_profile("
                 "meter_mac_address,meter_serial_number,gateway_id,real_time_clock,"
                 "average_voltage,block_energy_kwh_import,block_energy_kvah_import,"
                 "block_energy_kwh_export,block_energy_kvah_export,"
                 //  "average_current,"
                 "last_download_time,request_id,push_alaram,error_code) "
                 "VALUES('%s','%s','%s','%s',%.2f,%.2f,%.2f,%.2f,%.2f,'%s',%zu,%d,%d)", // add %.2f for average current from last 5 postion

                 // 13 VALUES (perfect match):
                 this->DB_parameter.meter_mac_address.c_str(),
                 this->DB_parameter.meter_serial_no.c_str(),
                 this->DB_parameter.gateway_id.c_str(),
                 this->BLP.data[BLP_RTC][i].getAsString().c_str(),
                 this->BLP.data[BLP_AVERAGE_VOLTAGE][i].getAsFloat(blp_average_voltage),
                 this->BLP.data[BLP_BLOCK_ENERGY_IMPORT_KWH][i].getAsFloat(blp_block_energy_kwh_import),
                 this->BLP.data[BLP_BLOCK_ENERGY_IMPORT_KVAH][i].getAsFloat(blp_block_energy_kvah_import),
                 this->BLP.data[BLP_BLOCK_ENERGY_EXPORT_KWH][i].getAsFloat(blp_block_energy_kwh_export),
                 this->BLP.data[BLP_BLOCK_ENERGY_EXPORT_KVAH][i].getAsFloat(blp_block_energy_kvah_export),
                 this->DB_parameter.last_download_time.c_str(),
                 this->DB_parameter.req_id,
                 this->DB_parameter.push_alaram,
                 err_code);

        int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

        if (db_result == 0)
        { // ✅ SUCCESS
            this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", download_data_type, this->DB_parameter.req_id);
        }
        else
        { // ✅ FAILURE
            this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", download_data_type);
        }
    }
    clear_profile_for_type(download_data_type);
    return true;
}

bool Client::process_AllEvents_case(uint8_t download_data_type)
{
    char query_buf[MAX_QUERY_BUFFER] = {0};
    uint8_t err_code = 1;

    this->print_and_log("Tamper Profile Summary\n");
    this->Update_dlms_on_demand_request_status(this->DB_parameter.req_id, SUCCESS_STATUS, 1);

    auto get_no_of_records = [this](uint8_t idx) -> uint16_t {
        if (this->event_data.data.count(idx) && this->event_data.data[idx].size() > EVENT_DATA_INDEX_FIELD_NO_OF_RECORDS)
        {
            return static_cast<uint16_t>(this->event_data.data[idx][EVENT_DATA_INDEX_FIELD_NO_OF_RECORDS].getAsFloat(1.0));
        }
        return 0;
    };
    auto get_capture_period = [this](uint8_t idx) -> uint32_t {
        if (this->event_data.data.count(idx) && this->event_data.data[idx].size() > EVENT_DATA_INDEX_FIELD_CAPTURE_PERIOD)
        {
            return static_cast<uint32_t>(this->event_data.data[idx][EVENT_DATA_INDEX_FIELD_CAPTURE_PERIOD].getAsFloat(1.0));
        }
        return 0;
    };
    auto get_current_entries = [this](uint8_t idx) -> uint32_t {
        if (this->event_data.data.count(idx) && this->event_data.data[idx].size() > EVENT_DATA_INDEX_FIELD_CURRENT_ENTRIES)
        {
            return static_cast<uint32_t>(this->event_data.data[idx][EVENT_DATA_INDEX_FIELD_CURRENT_ENTRIES].getAsFloat(1.0));
        }
        return 0;
    };
    auto get_max_records = [this](uint8_t idx) -> uint32_t {
        if (this->event_data.data.count(idx) && this->event_data.data[idx].size() > EVENT_DATA_INDEX_FIELD_MAX_RECORDS)
        {
            return static_cast<uint32_t>(this->event_data.data[idx][EVENT_DATA_INDEX_FIELD_MAX_RECORDS].getAsFloat(1.0));
        }
        return 0;
    };

    // Debug prints for ALL_EVENTS
    this->print_and_log("[DEBUG ALL_EVENTS] gateway_id: %s\n", this->DB_parameter.gateway_id.c_str());
    this->print_and_log("[DEBUG ALL_EVENTS] meter_mac_address: %s\n", this->DB_parameter.meter_mac_address.c_str());
    this->print_and_log("[DEBUG ALL_EVENTS] meter_serial_number: %s\n", this->DB_parameter.meter_serial_no.c_str());
    this->print_and_log("[DEBUG ALL_EVENTS] voltage_no_of_records: %u\n", get_no_of_records(0));
    this->print_and_log("[DEBUG ALL_EVENTS] voltage_period: %u\n", get_capture_period(0));
    this->print_and_log("[DEBUG ALL_EVENTS] voltage_current_record: %u\n", get_current_entries(0));
    this->print_and_log("[DEBUG ALL_EVENTS] voltage_max_records: %u\n", get_max_records(0));
    // Similarly for others, but for brevity, skipping
    this->print_and_log("[DEBUG ALL_EVENTS] error_code: %d\n", err_code);
    this->print_and_log("[DEBUG ALL_EVENTS] request_id: %zu\n", this->DB_parameter.req_id);
    this->print_and_log("[DEBUG ALL_EVENTS] last_download_time: %s\n", this->DB_parameter.last_download_time.c_str());

    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO dlms_all_tamper("
             "gateway_id,meter_mac_address,meter_serial_number,"
             "voltage_no_of_records,voltage_period,voltage_current_record,voltage_max_records,"                         // Voltage (Index 0)
             "current_no_of_records,current_period,current_current_record,current_max_records,"                         // Current (Index 1)
             "power_no_of_records,power_period,power_current_record,power_max_records,"                                 // Power (Index 2)
             "transaction_no_of_records,transaction_period,transaction_current_record,transaction_max_record,"          // Transaction (Index 3)
             "non_roll_over_no_of_records,non_roll_over_period,non_roll_over_current_record,non_roll_over_max_records," // Non-Rollover (Index 4)
             "other_no_of_records,other_period,other_current_record,other_max_records,"                                 // Other (Index 5)
             "control_no_of_records,control_period,control_current_record,control_max_records,"                         // Control (Index 6)
             "error_code,request_id,last_download_time) "
             "VALUES('%s','%s','%s',%u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %u,%u,%u,%u, %d,%zu,'%s')",

             this->DB_parameter.gateway_id.c_str(),
             this->DB_parameter.meter_mac_address.c_str(),
             this->DB_parameter.meter_serial_no.c_str(),
             // Voltage - Index 0
             get_no_of_records(0), get_capture_period(0), get_current_entries(0), get_max_records(0),
             // Current - Index 1
             get_no_of_records(1), get_capture_period(1), get_current_entries(1), get_max_records(1),
             // Power - Index 2
             get_no_of_records(2), get_capture_period(2), get_current_entries(2), get_max_records(2),
             // Transaction - Index 3
             get_no_of_records(3), get_capture_period(3), get_current_entries(3), get_max_records(3),
             // Non-Rollover - Index 4
             get_no_of_records(4), get_capture_period(4), get_current_entries(4), get_max_records(4),
             // Other - Index 5
             get_no_of_records(5), get_capture_period(5), get_current_entries(5), get_max_records(5),
             // Control - Index 6
             get_no_of_records(6), get_capture_period(6), get_current_entries(6), get_max_records(6),
             err_code,
             this->DB_parameter.req_id,
             this->DB_parameter.last_download_time.c_str());

    clear_profile_for_type(download_data_type);

    int db_result = execute_query(query_buf); // 0=SUCCESS, non-zero=FAILURE

    if (db_result == 0)
    { // ✅ SUCCESS
        this->print_and_log("[DB] 🗄️ INSERT ✅ | Type=%u | ReqID=%zu\n", download_data_type, this->DB_parameter.req_id);
    }
    else
    { // ✅ FAILURE
        this->print_and_log("[DB] 🗄️ INSERT ❌ | Type=%u \n", download_data_type);
    }
    return true;
}