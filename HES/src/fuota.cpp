#ifndef __FUOTA_CPP__
#define __FUOTA_CPP__

#include "../inc/fuota.h"
#include "../inc/client.h"
#include "../inc/mqtt.h"

#define PHY_LAYER_PKT_SIZE 128
#define FUOTA_CMD_LEN      8 // Header (7 Byte) + Checksum (1 Byte)
// #define FLASH_SECTOR_SIZE  4096

Fuota::Fuota(Client &c, MySqlDatabase &d) : client(c), db(d)
{
    print_and_log("Fuota constructed, this=%p\n", this);
    gate_node = new gateway_details();
    gate_node->meter_list = nullptr; // explicit
}

Fuota::~Fuota()
{
    print_and_log("Fuota destructed, this=%p\n", this);

    if (gate_node)
    {
        if (gate_node->meter_list)
        {
            for (auto *m : *gate_node->meter_list)
            {
                delete m;
            }
            delete gate_node->meter_list;
        }
        delete gate_node;
    }
}

// not using
void Fuota::delete_meter_list(void)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    if (this->gate_node->meter_list != NULL)
    {
        for (vector<struct meter_vital_info *>::iterator pointer1 = this->gate_node->meter_list->begin(); pointer1 != this->gate_node->meter_list->end(); ++pointer1)
        {
            delete (struct meter_vital_info *)(*pointer1);
        }
        this->gate_node->meter_list->clear();

        if (this->gate_node->meter_list)
        {
            delete this->gate_node->meter_list;
            this->gate_node->meter_list = nullptr;
        }
    }
}

void Fuota::process_fuota_queue()
{
    print_and_log("(%s) -> Processing FUOTA queue...\n", client.gateway_id);

    // If a previous status update marked a terminal completion, handle dequeuing here
    if (this->pending_terminal_complete.load())
    {
        int pid = this->pending_terminal_request_id.load();
        if (pid >= 0 && !client.RF_Meter_FUOTA.empty())
        {
            const std::vector<uint8_t> &front_bytes = client.RF_Meter_FUOTA.front();
            std::string front_cmd(front_bytes.begin(), front_bytes.end());
            auto parts = client.split(front_cmd, ':');
            if (!parts.empty())
            {
                try
                {
                    int front_req_id = std::stoi(parts[0]);
                    if (front_req_id == pid)
                    {
                        this->print_and_log("(%s) [FUOTA] Dequeuing completed request %d as instructed by status update.\n", client.gateway_id, pid);
                        client.RF_Meter_FUOTA.pop();
                    }
                }
                catch (...)
                {
                }
            }
        }
        this->pending_terminal_complete.store(false);
        this->pending_terminal_request_id.store(-1);
    }

    if (client.RF_Meter_FUOTA.empty())
    {
        print_and_log("[FUOTA] No pending FUOTA requests in queue — switching to ROLLBACK_TO_NORMAL_COMM_MODE (normal communications).\n");
        ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
        return;
    }

    // 🔥 Dequeue ONLY ONE command
    if (!client.dequeue_fuota(this->cmd_bytes))
        return;

    std::string cmd(this->cmd_bytes.begin(), this->cmd_bytes.end());
    auto parts = client.split(cmd, ':');

    if (parts.size() < 7)
    {
        print_and_log("Invalid FUOTA command format\n");
        return;
    }

    client.request_id = std::stoi(parts[0]);
    client.hop_count = std::stoi(parts[2]);
    this->firmware_path = parts[5];
    this->firmware_file = parts[6];

    print_and_log("[FUOTA] Dequeued REQ=%d FW=%s\n", client.request_id, this->firmware_file.c_str());
    ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
    this->cansend_fuota_next_command();
}

// This returns the directory where the executable is located
std::string get_app_base_path()
{
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    std::string full(result, (count > 0) ? count : 0);
    return full.substr(0, full.find_last_of('/'));
}

// This returns the directory where the program is running, even when started via systemd, service, script, or terminal.
std::string Fuota::get_app_base_path()
{
    this->print_and_log("(%s) -> Function:%s\n", client.gateway_id, __FUNCTION__);
    namespace fs = std::experimental::filesystem;
    return fs::current_path().string();
}

void Fuota::open_requested_firmware(const std::string &base_dir, const std::string &requested_filename, std::string &out_filepath)
{
    this->print_and_log("(%s) -> Function:%s\n", client.gateway_id, __FUNCTION__);
    namespace fs = std::experimental::filesystem;

    print_and_log("(%s) -> Base path: %s\n", client.gateway_id, base_dir.c_str());

    if (requested_filename.empty())
    {
        print_and_log("[ERROR] Requested firmware filename is empty\n");
        this->ondemand_fuota_update_status(2, 1 /*success*/, client.request_id);
        return;
    }

    // Build DCU-specific FUOTA directory
    this->dcu_folder = base_dir + "/FUOTA/RF/" + client.gateway_id;
    print_and_log("FUOTA folder: %s\n", dcu_folder.c_str());

    // Build full firmware path
    this->firmware_path = dcu_folder + "/" + requested_filename;
    print_and_log("Requested firmware: %s\n", this->firmware_path.c_str());

    // Validate existence
    if (!fs::exists(this->firmware_path))
    {
        print_and_log("[ERROR] Firmware not found: %s\n", this->firmware_path.c_str());
        this->ondemand_fuota_update_status(0, 1 /*success*/, client.request_id);
        return;
    }

    // Close previous file if any
    if (fouta_read_fd)
    {
        fclose(fouta_read_fd);
        fouta_read_fd = nullptr;
    }

    // Open firmware
    fouta_read_fd = fopen(this->firmware_path.c_str(), "rb");
    if (!fouta_read_fd)
    {
        print_and_log("[ERROR] fopen failed: %s\n", strerror(errno));
        this->ondemand_fuota_update_status(1, 1, client.request_id);
        return;
    }

    // Store state
    out_filepath = this->firmware_path;
    client.firmware_path = this->firmware_path;
    client.firmware_filename = fs::path(this->firmware_path).filename().string();

    this->set_latest_firmware_path(this->firmware_path);

    total_firmwaresize = this->get_fw_size(this->firmware_path);

    print_and_log("Firmware ready for FUOTA: %s (%ld bytes)\n", this->firmware_path.c_str(), total_firmwaresize);
    ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_SILENCE;
    this->cansend_fuota_next_command();
}

void Fuota::set_latest_firmware_path(const std::string &path)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    this->latest_firmware_path = path;
}

int Fuota::dynamic_fuota_calculate()
{
    this->print_and_log("(%s) -> sector divide, state:%d\n", client.gateway_id, this->ondemand_fuota_state);
    this->print_and_log("(%s) -> Firmware Path: %s\n", client.gateway_id, this->dcu_folder.c_str());

    if (this->dcu_folder.empty())
    {
        return e_failure;
    }

    this->ondemand_fuota_update_status(8, 1 /*success*/, client.request_id);
    int path_count = 0;
    this->print_and_log("(%s) -> Router Path Count: %d\n", client.gateway_id, path_count);

    session.header_len = this->calculate_dynamic_header_len(path_count);

    session.file = client.firmware_path;
    this->print_and_log("(%s) -> Firmware file found: %s\n", client.gateway_id, session.file.c_str());

    if (session.file.empty())
    {
        this->print_and_log("(%s) -> [ERROR] No firmware file available at runtime\n", client.gateway_id);
        return e_failure;
    }
    else
    {
        // Close old file handle
        if (this->fouta_read_fd)
        {
            fclose(this->fouta_read_fd);
            this->fouta_read_fd = nullptr;
        }

        // Open .bin
        session.file_ptr = fopen(session.file.c_str(), "rb");
        if (!session.file_ptr)
        {
            this->print_and_log("(%s) -> [ERROR]  Unable to open firmware:%d\n", client.gateway_id, strerror(errno));
            return e_failure;
        }

        this->set_latest_firmware_path(session.file);
        this->fouta_read_fd = session.file_ptr;

        // Get firmware size
        session.firmware_size = this->get_fw_size(session.file);
        this->total_firmwaresize = session.firmware_size;

        session.flash_page_count = this->get_max_page_count(session.firmware_size);
        session.flash_subpage_count = this->get_max_sub_page_count(session.header_len);

        flash_page = session.flash_page_count;
        flash_sub_page = session.flash_subpage_count;

        this->print_and_log("(%s) -> Header Len:%d, FW Size:%ld, Pages:%d, SubPages:%d\n", client.gateway_id, session.header_len, session.firmware_size, flash_page, flash_sub_page);
    }
    return e_success_1;
}

long Fuota::get_fw_size(const std::string &file_path)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    struct stat st;
    if (stat(file_path.c_str(), &st) == 0)
        return st.st_size;

    perror("stat failed");
    return -1;
}

unsigned int Fuota::get_max_page_count(long fw_size)
{
    auto pages = static_cast<unsigned int>(fw_size / this->sector_value);
    print_and_log("(%s) -> mxpgcount: %u\n", client.gateway_id, pages);
    return pages;
}

int Fuota::calculate_dynamic_header_len(uint8_t routerpath_count)
{
    int base_len = 13; // Default base header size (no router i.e Gateway node)
    if (routerpath_count == 1 || routerpath_count == 0)
    {
        print_and_log("(%s) -> Path_count: %d,  headersize: %d, hop size:%d\n", client.gateway_id, routerpath_count, base_len, (routerpath_count * 4));
        // routerpath_count = 0; // 30092025
        return base_len + 4;
    }
    else
    {
        print_and_log("(%s) -> Pathcount: %d,  headersize: %d, hop size:%d\n", client.gateway_id, routerpath_count, base_len, (routerpath_count * 4));

        return base_len + (routerpath_count * 4);
    }
}

int Fuota::get_max_payload_size(int header_len)
{
    int hopdevice_headerlen = (PHY_LAYER_PKT_SIZE - header_len - FUOTA_CMD_LEN);

    print_and_log("(%s) -> Header size = %d\n", client.gateway_id, hopdevice_headerlen);
    if (hopdevice_headerlen < 0)
    {
        print_and_log("(%s) -> header_len: %d\n", client.gateway_id, header_len);
        return (PHY_LAYER_PKT_SIZE - 25);
    }
    return (PHY_LAYER_PKT_SIZE - header_len - FUOTA_CMD_LEN);
}

int Fuota::get_max_sub_page_count(int header_len)
{
    int subpgcount = ((this->sector_value / this->get_max_payload_size(header_len)) + 1);
    print_and_log("(%s) -> subpgcount: %d\n", client.gateway_id, subpgcount);
    if (subpgcount < 0)
    {
        print_and_log("(%s) -> get_max_payload_size(header_len): %d\n", client.gateway_id, this->get_max_payload_size(header_len));
        return e_failure;
    }
    return ((this->sector_value / this->get_max_payload_size(header_len)) + 1);
}

int Fuota::get_min_payload_size(int header_len)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    int max_payload = this->get_max_payload_size(header_len);
    int subpage_count = this->get_max_sub_page_count(header_len);
    int min_payload = (this->sector_value - ((subpage_count - 1) * max_payload));

    print_and_log("(%s) -> minpayloadsize: %d\n", client.gateway_id, min_payload);
    return min_payload;
}

int Fuota::client_get_time(char *time_str)
{
    char rxd_time[26] = {0};
    int millisec;
    struct tm newtime;
    time_t ltime;
    struct timeval tv;
    const char *fmt = "%Y-%m-%d %X";
    gettimeofday(&tv, NULL);

    millisec = lrint(tv.tv_usec / 1000.0); // Round to nearest millisec
    if (millisec >= 1000)
    {
        millisec -= 1000;
        tv.tv_sec++;
    }
    ltime = time(&ltime);
    localtime_r(&ltime, &newtime);

    if (strftime(rxd_time, sizeof(rxd_time), fmt, &newtime) == 0)
    {
        fprintf(stderr, "strftime returned 0");
        return -1;
    }
    sprintf(time_str, "%s", rxd_time);
    return millisec;
}

int Fuota::frame_pmesh_ota_cmd_header(unsigned char packet_type, unsigned char *cmd_data, int cmd_len)
{
    // this->print_and_log("I'm in the frame pmesh_ota_cmd_header");
    print_and_log(" -> Requested cmd len (%d) is: ", cmd_len);
    for (int i = 0; i < cmd_len; i++)
    {
        print_and_log("%02X ", cmd_data[i]);
    }
    print_and_log("\n");

    // this->print_and_log("cmd_len:%d\n", cmd_len);

    // struct data_query_header *ptr;
    struct common_header *c;
    unsigned char *pkt_buffer, *t;

    this->client_tx_buffer[0] = HES_START_BYTE;

    pkt_buffer = &this->client_tx_buffer[1];
    c = (struct common_header *)pkt_buffer;

    // cout << "Common header size: " << sizeof(struct common_header) << endl;

    print_and_log("(%s) -> Hop count: %d ,Hop array size: %d ", client.gateway_id, this->router_path.hop_count, (this->router_path.hop_count * 4));
    print_and_log(" , Cmd length: %d\n", cmd_len);

    print_and_log("Packet length: %d", c->pkt_length);
    print_and_log(", Router Count: %d\n", this->router_path.hop_count);

    Utility::extract_ids((const char *)client.gateway_id, this->panid, this->dcu_short_addr);

    // Copy PAN ID
    memcpy(&c->panid[0], this->panid, 4);
    print_and_log("Panid: ");
    for (int i = 0; i < 4; i++)
        print_and_log("%02X ", ((unsigned char *)&c->panid)[i]);
    print_and_log("\n");

    c->flags = packet_type;
    t = pkt_buffer + sizeof(struct common_header);
    req_header = (struct data_query_header *)t;

    memcpy(req_header->source_address, this->dcu_short_addr, 4); // Source address is last 4 bytes of DCU ID
    print_and_log("Source Address: ");
    for (int i = 0; i < 4; i++)
        print_and_log("%02X ", ((unsigned char *)&req_header->source_address)[i]);
    print_and_log("\n");

    print_and_log("Destination Address: ");
    for (int i = 0; i < 4; i++)
        print_and_log("%02X ", ((unsigned char *)&req_header->addr_array[0])[i]);
    print_and_log("\n");

    this->print_and_log("------------------Arranging Pmeshheader------------\n");

    // Fill router data

    req_header->num_routers = this->router_path.hop_count;
    req_header->router_index = 0;

    if (this->router_path.hop_count == 0)
    {
        this->print_and_log("Gateway is direct node\n");
        memcpy(&req_header->addr_array[0], req_header->source_address, 4);
    }
    else
    {
        this->print_and_log("Gateway is under node\n");
        memcpy(&req_header->addr_array[0], &this->router_path.paths[0], this->router_path.hop_count * 4);
    }

    // Calculate packet length
    if (this->router_path.hop_count == 0)
    {
        c->pkt_length = sizeof(struct common_header) + 6 /*source + router Index + num of router*/ + /*dest*/ 4 + cmd_len;
    }
    else
    {
        c->pkt_length = sizeof(struct common_header) + 6 + (this->router_path.hop_count * 4) + cmd_len;
    }

    // Append command data
    if (cmd_len != 0)
    {
        if (this->router_path.hop_count == 0)
        {
            memcpy(&req_header->addr_array[4], &cmd_data[0], cmd_len);
        }
        else
        {
            memcpy(&req_header->addr_array[this->router_path.hop_count * 4], &cmd_data[0], cmd_len);
        }
    }

    // Finalize buffer for output
    // this->print_and_log("OTA request_cmd is: ", sizeof(this->client_tx_buffer), this->client_tx_buffer);

    // printf("OTA Arranged cmd Header: ");
    // for (int i = 0; i < c->pkt_length + 1; i++)
    // {
    //     printf("%02X ", this->client_tx_buffer[i]);
    // }
    // printf("\n");

    return e_success_0;
}

LastSectorStatus Fuota::rffuota_read_lastsector_page_and_subpage_status()
{
    this->print_and_log("(%s)-> %s \n", client.gateway_id, __FUNCTION__);
    LastSectorStatus st;
    this->ondemand_fuota_update_status(12, 1, client.request_id);
    if (this->current_fuota_target_mac != nullptr)
    {
        this->print_and_log("(%s) -> RF FUOTA Target node at lastsector read:", client.gateway_id);
        for (int i = 0; i < 8; i++)
        {
            print_and_log(" %02X", this->current_fuota_target_mac[i]);
        }
        this->print_and_log("\n");
    }
    else
    {
        this->print_and_log("(%s) -> RF FUOTA current_fuota_targetmac is NULL\n", client.gateway_id);
    }

    // Prepare a query command into a local buffer ota for RF FUOTA
    // this->frame_pmesh_ota_cmd_header(FUOTA_DATA_QUERY, this->lastsector_page_read, sizeof(this->lastsector_page_read));

    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;

    this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, this->lastsector_page_read, sizeof(this->lastsector_page_read), mac, route);

    int length = this->client_tx_buffer[1] + 1;

    if (length <= 0)
    {
        this->print_and_log("(%s) -> frame headerbuild failed\n", client.gateway_id);
        return st;
    }

    const int MAX_RETRY = 3;
    for (int attempt = 1; attempt <= MAX_RETRY; ++attempt)
    {

        this->print_and_log("(%s) -> Attempt %d to read lastsector page\n", client.gateway_id, attempt);
        // --- TX ---
        ssize_t sent = write(client.get_client_socket(), this->client_tx_buffer, this->client_tx_buffer[1] + 1);
        if (sent < 0)
        {

            this->print_and_log("(%s) -> Error sending lastSector_page read failed:%d \n", client.gateway_id, strerror(errno));
            continue; // retry
        }

        char txdtime[26] = {0};
        int millisec = this->client_get_time(txdtime);
        print_and_log("TX:[%s] -> [%s.%03d] = ", client.gateway_id, txdtime, millisec);
        for (int i = 0; i < sent; ++i)
            print_and_log("%02X ", this->client_tx_buffer[i]);
        print_and_log("\n");

        // --- RX ---
        uint8_t resp[512] = {0};
        ssize_t rlen = read(client.get_client_socket(), resp, sizeof(resp));

        if (rlen <= 0)
        {
            this->print_and_log("(%s) -> Read failed or timedout:%d resplen:%d (attempt:%d)\n", client.gateway_id, strerror(errno), rlen, attempt);
            continue; // retry up to MAX_RETRY
        }

        char rxdtime[26] = {0};
        millisec = this->client_get_time(rxdtime);
        print_and_log("RX [%s] -> [%s.%03d] = ", client.gateway_id, rxdtime, millisec);
        for (ssize_t i = 0; i < rlen; i++)
            print_and_log("%02X ", resp[i]);
        print_and_log("\n");

        // this->print_and_log("RX", rxdtime, millisec, resp, rlen);

        // --- Check content validity ---
        if (resp[2] == 0x0A && (resp[rlen - 1] != 0x06 && resp[rlen - 1] != 0x07))
        {
            // skip - invalid response
            this->print_and_log("(%s) ->valid response (0x0A)\n", client.gateway_id);
        }
        else if (resp[2] == 0x09 && (resp[rlen - 1] == 0x06 || resp[rlen - 1] == 0x07))
        {
            this->print_and_log("(%s) -> Retry-type response received - retrying...\n", client.gateway_id);

            // client.set_poll_timeout(12);
            this->wait_after_flash(12, this->thread_run); // 02012026
            continue;
        }

        // --- Parse valid response ---
        int expected_hdr_len = this->update_fuota_response_buff(); // 2F 6E 06 01 17 00 0A
        ssize_t idx_subpage = expected_hdr_len + 7;
        ssize_t idx_page_hi = expected_hdr_len + 5;
        ssize_t idx_page_lo = expected_hdr_len + 6;

        if (rlen > idx_page_lo)
        {
            this->print_and_log("--------------valid responses----------\n");
            st.subpage = resp[idx_subpage];
            this->print_and_log("(%s) -> subPage:%d\n", client.gateway_id, st.subpage);

            st.page = (resp[idx_page_hi] << 8) | resp[idx_page_lo];

            this->print_and_log("(%s) -> Page:%d\n", client.gateway_id, st.page);
            st.valid = true;
            break; // success
        }
        else
        {
            // cout << "(" << client.gateway_id << ") = FUOTA page mis-match framing the response (len too small)\n";
            this->print_and_log("FUOTA page mis-match framing the response\n");

            if (rlen >= 6)
            {
                st.subpage = resp[rlen - 2];
                st.page = resp[rlen - 3];
                st.valid = true;
            }
            break; // we still consider it valid enough to continue
        }
    }

    if (!st.valid)
    {
        // cout << "(" << client.gateway_id << ") = Failed to read lastsector page after: " << MAX_RETRY << " attempts.\n";
        this->print_and_log("Failed to read lastsector page after 3 retries\n");
    }

    return st;
}

void Fuota::insert_alternate_path(const unsigned char *mac_address, const unsigned char *route_path_full, unsigned char hop_count)
{
    this->print_and_log("%s Function: \n", __FUNCTION__);
    auto new_path = std::make_unique<alternate_path_info>();

    // Copy target meter MAC (8 bytes)
    memcpy(new_path->meter_mac_address, mac_address, 8);

    // Each hop is 8 bytes in DB → we store last 4 bytes only
    size_t total_full_bytes = (size_t)hop_count * 8; // DB format
                                                     // size_t total_compressed = (size_t)hop_count * 4; // Our stored format

    // Clear buffer first
    memset(new_path->route_path, 0, sizeof(new_path->route_path));

    for (int i = 0; i < hop_count; ++i)
    {
        size_t src_offset = (size_t)i * 8 + 4; // LAST 4 bytes of 8-byte hop
        size_t dst_offset = (size_t)i * 4;

        if (src_offset + 4 <= total_full_bytes &&
            dst_offset + 4 <= sizeof(new_path->route_path))
        {
            memcpy(&new_path->route_path[dst_offset], &route_path_full[src_offset], 4);
        }
        else
        {
            print_and_log("(%s) ->  Warning: invalid hop offset while compressing route!", client.gateway_id);
            memset(&new_path->route_path[dst_offset], 0, 4);
        }
    }
    new_path->hop_count = hop_count;

    // Link into list
    if (!this->alternatepaths)
    {
        this->alternatepaths = std::move(new_path);
    }
    else
    {
        alternate_path_info *cur = this->alternatepaths.get();
        while (cur->next)
            cur = cur->next.get();
        cur->next = std::move(new_path);
    }
}

void Fuota::print_alternate_paths()
{
    this->print_and_log("Function:%s \n", __FUNCTION__);
    const alternate_path_info *cur = this->alternatepaths.get();
    int index = 1;

    while (cur)
    {
        print_and_log("Alternate Path #%d:\n", index++);

        print_and_log("  MAC: ");
        for (int i = 0; i < 8; i++)
            print_and_log("%02X", cur->meter_mac_address[i]);
        print_and_log("\n");

        print_and_log("  Hop Count: %u\n", cur->hop_count);

        print_and_log("  ROUTE PATH: ");
        for (int i = 0; i < cur->hop_count * 4; i++)
            print_and_log("%02X", cur->route_path[i]);
        print_and_log("\n\n");

        cur = cur->next.get();
    }
}

int Fuota::frame_pmesh_fuota_cmd_header_for_target_node(unsigned char packet_type, unsigned char *cmd_data, int cmd_len, unsigned char *target_mac, router_path_t *target_route)
{
    (void)target_mac;
    (void)packet_type;
    (void)cmd_len;
    (void)cmd_data;

    this->print_and_log("(%s): ", client.gateway_id, "\n", __FUNCTION__);

    if (!target_route)
    {
        std::cout << "(" << client.gateway_id << ") =  target_route is NULL" << std::endl;
        return e_failure;
    }
    if (!cmd_data || cmd_len <= 0)
    {
        std::cout << "(" << client.gateway_id << ") = Invalid command data or length" << std::endl;
        return e_failure;
    }

    // basic header init
    this->client_tx_buffer[0] = HES_START_BYTE;
    unsigned char *pkt_buffer = &this->client_tx_buffer[1];

    struct common_header *c = (struct common_header *)pkt_buffer;
    unsigned char *t = pkt_buffer + sizeof(struct common_header);
    struct data_query_header *ptr = (struct data_query_header *)t;

    // Top get the panid and gateway serial number from the gateway id
    Utility::extract_ids((const char *)client.gateway_id, this->panid, this->dcu_short_addr);

    // PAN ID & flags — ensure correct copy (panid is 4 bytes)
    memcpy(&c->panid[0], this->panid, 4);
    print_and_log("PANID: ");
    for (int i = 0; i < 4; i++)
    {
        print_and_log("%02X ", ((unsigned char *)&c->panid)[i]);
    }

    // Source address — ensure correct copy (source_address is 4 bytes)
    memcpy(&ptr->source_address, this->dcu_short_addr, 4);
    print_and_log("\nSource Address: ");
    for (int i = 0; i < 4; i++)
    {
        print_and_log("%02X ", ((unsigned char *)&ptr->source_address)[i]);
    }
    c->flags = packet_type;

    // *** DO NOT overwrite target_route->hop_count here ***
    // If you need to limit hops due to ondemand_request->no_alternate_path,
    // compute a local limited_hop_count variable instead (do not overwrite the route).
    target_route->hop_count = client.hop_count;
    print_and_log("Target hop count: %d\n", (int)target_route->hop_count);
    int actual_hop_count = (int)target_route->hop_count;
    if (actual_hop_count < 0)
        actual_hop_count = 0;
    if (actual_hop_count > 32)
    {
        print_and_log("(%s) -> Invalid hop_count: %d\n", client.gateway_id, actual_hop_count);
        return e_failure;
    }

    // Router Info
    ptr->num_routers = (unsigned char)actual_hop_count;
    ptr->router_index = 0;

    // Fill route path (addr_array holds hop_count*4 bytes). If hop_count == 0,
    // set the addr_array[0..3] to source address (as earlier code attempted)
    if (actual_hop_count == 0)
    {
        print_and_log("(%s) -> Gateway Only device bcz hop count zero\n", client.gateway_id);
        memcpy(&ptr->addr_array[0], &ptr->source_address[0], 4);
        // for (int i = 0; i < 4; i++)
        // {
        //     printf("%02X ", ptr->addr_array[i]);
        // }
        // cout << endl;
    }
    else
    {
        for (int i = 0; i < actual_hop_count; ++i)
        {
            memcpy(&ptr->addr_array[i * 4], target_route->paths[i], 4);
        }
        for (int i = 0; i < 4; i++)
        {
            print_and_log("%02X ", ptr->addr_array[i]);
        }
        print_and_log("\n");
    }

    // compute base len: common_header + data_query_header (6 bytes) + hops
    // NOTE: keep consistent with your earlier base_len formula
    int base_len = sizeof(struct common_header) + 6 + (actual_hop_count * 4);

    if (base_len + cmd_len > CLIENT_TX_BUFFER_SIZE)
    {
        print_and_log("(%s) -> Packet size exceeds client_tx_buffer capacity\n", client.gateway_id);
        return e_failure;
    }

    if (actual_hop_count > 0)
    {
        print_and_log("(%s) -> Not a self node,hop count greater than zero\n", client.gateway_id);
        memcpy(&ptr->addr_array[4], cmd_data, cmd_len);
        c->pkt_length = base_len + cmd_len;
    }
    else
    {
        // cout << "(" << client.gateway_id << ") = No hops, inserting cmd_data after addr_array[4] :";

        print_and_log("(%s) -> cmd_data: ", client.gateway_id);
        for (int i = 0; i < cmd_len; i++)
        {
            print_and_log("%02X ", cmd_data[i]);
        }
        print_and_log("\n");
        memcpy(&ptr->addr_array[4], cmd_data, cmd_len);
        c->pkt_length = base_len + 4 + cmd_len;
    }

    // debug: print hops, next-hop and final-hop
    print_and_log("(%s) -> hop_count -> %d\n", client.gateway_id, actual_hop_count);

    if (actual_hop_count > 0)
    {
        print_and_log(" Next-hop (wire-destination) = %02X:%02X:%02X:%02X\n",
                      ptr->addr_array[0], ptr->addr_array[1], ptr->addr_array[2], ptr->addr_array[3]);
        print_and_log(" Final-hop = %02X:%02X:%02X:%02X\n",
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 0],
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 1],
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 2],
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 3]);
    }
    else
    {
        print_and_log(" No hops, addr_array[0..3] = %02X:%02X:%02X:%02X\n",
                      ptr->addr_array[0], ptr->addr_array[1], ptr->addr_array[2], ptr->addr_array[3]);
    }

    // print final packet
    print_and_log("Base length: %d, Command length: %d\n", base_len, cmd_len);
    // print_and_log("(%s) -> OTA Cmd Packet: ", client.gateway_id);
    // for (int i = 0; i < c->pkt_length + 1; i++)
    //     print_and_log("%02X ", this->client_tx_buffer[i]);
    // print_and_log("\n");

    return e_success_0;
}

int Fuota::frame_fuota_cmdheader_for_network(unsigned char packet_type, unsigned char *cmd_data, int cmd_len, unsigned char *target_mac,
                                             router_path_t *target_route)
{
    (void)target_mac;
    (void)packet_type;
    (void)cmd_data;
    (void)cmd_len;
    print_and_log("(%s) -> client: To Frame Network Fuota OTAHeader\n", client.gateway_id);

    if (!target_route)
    {
        print_and_log("(%s) -> target_route is NULL\n", client.gateway_id);
        return e_failure;
    }
    if (!cmd_data || cmd_len <= 0)
    {
        print_and_log("(%s) -> Invalid command data or length\n", client.gateway_id);

        return e_failure;
    }

    // basic header init
    this->client_tx_buffer[0] = 0x2E;
    unsigned char *pkt_buffer = &this->client_tx_buffer[1];
    struct common_header *c = (struct common_header *)pkt_buffer;
    unsigned char *t = pkt_buffer + sizeof(struct common_header);
    struct data_query_header *ptr = (struct data_query_header *)t;

    // PAN ID & flags
    Utility::extract_ids((const char *)client.gateway_id, this->panid, this->dcu_short_addr);

    // PAN ID & flags — ensure correct copy (panid is 4 bytes)
    memcpy(&c->panid[0], this->panid, 4);
    // Source address — ensure correct copy (source_address is 4 bytes)
    memcpy(&ptr->source_address, this->dcu_short_addr, 4);
    c->flags = packet_type;

    // *** DO NOT overwrite target_route->hop_count here ***
    // If you need to limit hops due to ondemand_request->no_alternate_path,
    // compute a local limited_hop_count variable instead (do not overwrite the route).
    int actual_hop_count = (int)target_route->hop_count;
    if (actual_hop_count < 0)
        actual_hop_count = 0;
    if (actual_hop_count > 32)
    {
        print_and_log("(%s) -> Invalid hop_count:%d\n", client.gateway_id, actual_hop_count);
        return e_failure;
    }

    // Router Info
    ptr->num_routers = (unsigned char)actual_hop_count;
    ptr->router_index = 0;

    // Fill route path (addr_array holds hop_count*4 bytes). If hop_count == 0,
    // set the addr_array[0..3] to source address (as earlier code attempted)
    if (actual_hop_count == 0)
    {
        memcpy(&ptr->addr_array[0], &ptr->source_address[0], 4);
    }
    else
    {
        for (int i = 0; i < actual_hop_count; ++i)
        {
            memcpy(&ptr->addr_array[i * 4], target_route->paths[i], 4);
        }
    }

    // compute base len: common_header + data_query_header (6 bytes) + hops
    // NOTE: keep consistent with your earlier base_len formula
    int base_len = sizeof(struct common_header) + 6 + (actual_hop_count * 4);

    if (base_len + cmd_len > CLIENT_TX_BUFFER_SIZE)
    {
        print_and_log("(%s) -> Packet size exceeds client_tx_buffer capacity\n", client.gateway_id);
        return e_failure;
    }

    // append the command payload after the route bytes
    memcpy(&ptr->addr_array[actual_hop_count * 4], cmd_data, cmd_len);
    c->pkt_length = base_len + cmd_len;

    // debug: print hops, next-hop and final-hop
    print_and_log("(%s) -> Debug: hop_count:%d\n", client.gateway_id, actual_hop_count);

    if (actual_hop_count > 0)
    {
        print_and_log(" Next-hop (wire-destination) = %02X:%02X:%02X:%02X\n",
                      ptr->addr_array[0], ptr->addr_array[1], ptr->addr_array[2], ptr->addr_array[3]);
        print_and_log(" Final-hop = %02X:%02X:%02X:%02X\n",
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 0],
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 1],
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 2],
                      ptr->addr_array[(actual_hop_count - 1) * 4 + 3]);
    }
    else
    {
        print_and_log(" No hops, addr_array[0..3] = %02X:%02X:%02X:%02X\n",
                      ptr->addr_array[0], ptr->addr_array[1], ptr->addr_array[2], ptr->addr_array[3]);
    }

    // print final packet
    print_and_log("Base length: %d, Command length: %d\n", base_len, cmd_len);
    // print_and_log("Network OTACmd Packet: ");
    // for (int i = 0; i < c->pkt_length + 1; i++)
    //     print_and_log("%02X ", this->client_tx_buffer[i]);
    // print_and_log("\n");

    return e_success_0;
}

void Fuota::start_fuota_response_timer(int timeout_sec)
{
    // std::cout << "(" << client.gateway_id << ") = Starting FUOTA response timer: " << timeout_sec << " seconds\n";
    int ret = client.set_recv_timeout_for_client(timeout_sec); // epoll / timerfd / poll
    if (ret != e_success_1)
    {
        // std::cout << "(" << client.gateway_id << ") = Failed to set FUOTA response timer\n";
        client.stateInfo.timeoutState = ClientTimeoutState::TIMER_FUOTA_RESPONSE; // added by LHK
    }
}

// step 1: Create a single helper to build and copy the command into buff
int Fuota::build_and_store_fuota_cmd()
{
    this->print_and_log("(%s) -> build_and_store_fuota_cmd attempt=%d route=%s\n", client.gateway_id, cntx.retry_count + 1, use_alternate_fuota_route ? "ALTERNATE" : "PRIMARY");

    //====================== 31122025===================
    if (this->use_alternate_fuota_route == true)
    {
        switch (this->network_silence_state)
        {
            case PATH_SILENCE_STATE::AT_FUOTA_ENABLE: {
                this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, fuota_enable, sizeof(fuota_enable), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
            }
            break;
            case PATH_SILENCE_STATE::AT_FUOTA_MODE_ENTRY: {
                this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, fuota_updatemode, sizeof(fuota_updatemode), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
            }
            break;
            case PATH_SILENCE_STATE::AT_ENABLE_FLASHSAVE: {
                this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, flash_write, sizeof(flash_write), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
            }
            break;
            case PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT: {
                this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, flash_exit, sizeof(flash_exit), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
            }
            break;
            default:
                break;
        }
    }
    else
    {
        if (use_alternate_fuotaroute_disable == true)
        {
            switch (this->network_silence_state)
            {
                case PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE: {
                    this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, fuota_disable_cmd, sizeof(fuota_disable_cmd), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
                }
                break;
                case PATH_UNSILENCE_STATE::AT_FUOTA_MODE_ENTRY_DISABLE: {
                    this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, fuota_modeentry_disable_cmd, sizeof(fuota_modeentry_disable_cmd), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
                }
                break;
                case PATH_UNSILENCE_STATE::AT_DISABLE_FLASHSAVE: {
                    this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, flash_write, sizeof(flash_write), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
                }
                break;
                case PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT: {
                    this->frame_fuota_cmdheader_for_network(MESH_COMMISSION_PACKET, flash_exit, sizeof(flash_exit), this->current_fuota_target_mac, (router_path_t *)this->current_fuota_route);
                }
                break;
                default:
                    break;
            }
        }
    }

    ssize_t sent = client.write_to_client(this->client_tx_buffer, this->client_tx_buffer[1] + 1);

    if (sent < 0)
    {
        this->print_and_log("Error sending FUOTA command: %s\n", strerror(errno));
        return e_failure;
    }

    waiting_for_response = true;
    this->start_fuota_response_timer(FUOTA_TIMEOUT_SEC);
    return e_success_1;
}

// step 2: command building for gateway node silence
int Fuota::prepare_gateway_silence_cmd()
{
    this->print_and_log("(%s)", client.gateway_id, ": \n", __FUNCTION__);

    if (this->ondemand_fuota_state == FUOTA_STATE::GATEWAY_PATH_SILENCE)
    {
        switch (this->fuota_gateway_silence_state)
        {
            case PATH_SILENCE_STATE::AT_FUOTA_ENABLE:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, fuota_enable, sizeof(fuota_enable));
                break;

            case PATH_SILENCE_STATE::AT_FUOTA_MODE_ENTRY:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, fuota_updatemode, sizeof(fuota_updatemode));
                break;

            case PATH_SILENCE_STATE::AT_ENABLE_FLASHSAVE:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, flash_write, sizeof(flash_write));
                break;

            case PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, flash_exit, sizeof(flash_exit));
                break;
            default:
                return e_failure;
        }
        return this->build_and_store_fuota_cmd();
    }
    // Update silence sub-state
    if (this->fuota_gateway_silence_state < PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT)
    {
        this->fuota_gateway_silence_state++;
    }
    else
    {
        // Finished gateway silence → move to next main FUOTA state
        this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_SILENCE;
        this->fuota_gateway_silence_state = PATH_SILENCE_STATE::AT_FUOTA_ENABLE; // reset
    }
    return e_success_1;
}

// step 3: command building for  target node silence
int Fuota::prepare_targetnode_silence_cmd()
{
    this->print_and_log("(%s)", client.gateway_id, ": \n", __FUNCTION__);
    // this->print_and_log("Preparing Target Node Silence Command");
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;

    if (this->ondemand_fuota_state == FUOTA_STATE::TARGET_NODE_SILENCE)
    {

        switch (this->fuota_targetnode_silence_state)
        {
            case PATH_SILENCE_STATE::AT_FUOTA_ENABLE:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, fuota_enable,
                    sizeof(fuota_enable), mac, route);
                break;

            case PATH_SILENCE_STATE::AT_FUOTA_MODE_ENTRY:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, fuota_updatemode,
                    sizeof(fuota_updatemode), mac, route);
                break;

            case PATH_SILENCE_STATE::AT_ENABLE_FLASHSAVE:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, flash_write,
                    sizeof(flash_write), mac, route);
                break;

            case PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, flash_exit,
                    sizeof(flash_exit), mac, route);
                break;
            default:
                return e_failure;
        }
        return this->build_and_store_fuota_cmd();
    }
    // Update silence sub-state
    if (this->fuota_targetnode_silence_state < PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT)
    {
        this->fuota_targetnode_silence_state++;
    }
    else
    {
        // Finished gateway silence → move to next main FUOTA state
        this->ondemand_fuota_state = FUOTA_STATE::NETWORK_SILENCE;
        this->fuota_targetnode_silence_state = PATH_SILENCE_STATE::AT_FUOTA_ENABLE; // reset
    }
    return e_success_1;
}

int Fuota::prepare_silence_the_entire_network_for_fuota()
{
    this->print_and_log("(%s) -> Silencing the entire path for FUOTA\n", client.gateway_id);

    switch (this->ondemand_fuota_state)
    {
        case FUOTA_STATE::NETWORK_SILENCE: {
            this->print_and_log("Network Silence State - Preparing FUOTA command\n");

            Fuota *pan_list_ptr = this;
            this->db.get_the_gateway_undernodes_details_for_fuota((char *)client_query_buffer, (unsigned char *)client.gateway_id, pan_list_ptr);

            this->gateway_route_info = new std::vector<route_entry *>();

            for (auto &meter : *(pan_list_ptr->gate_node->meter_list))
            {
                route_entry *entry = new route_entry();
                memset(entry, 0, sizeof(route_entry));
                print_and_log("(%s) -> Network silence of current hop count:%d\n", client.gateway_id, meter->hop_count);
                entry->hop_count = meter->hop_count;
                memcpy(entry->target_mac, meter->meter_mac_address, 8);
                memcpy(entry->route_path, meter->route_path, meter->hop_count * 4);

                this->gateway_route_info->push_back(entry);
            }
            this->client_tx_buffer[2] = MESH_COMMISSION_PACKET_TYPE;
            print_and_log("(%s) -> Passing of On-demand mac as: ", client.gateway_id);
            for (int i = 0; i < 16; i++)
            {
                print_and_log("%02X ", this->ondemand_mac_addr[i]);
            }
            print_and_log("\n");
            return process_rffuota_enable_sequence_refactored(this->ondemand_mac_addr);
        }

        default:
            return e_failure;
    }
}

int Fuota::prepare_get_sector_read_for_target_node()
{
    this->print_and_log("(%s) -> Preparing Get Sector Read Command for Target Node\n", client.gateway_id);
    this->wait_after_flash(30, this->thread_run);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::SECTOR_READ)
    {
        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, sector_read, sizeof(sector_read), mac, route);
    }
    return this->build_and_store_fuota_cmd();

    // response need to fetch and calculate the header length
}

int Fuota::prepare_fwimage_of_sector_count_cmd()
{
    this->print_and_log("(%s) -> Preparing FW Image of Sector Count Command", client.gateway_id);

    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::FWIMAGE_OF_SECTOR_COUNT)
    {
        initiate_fw_info[4] = this->total_firmwaresize >> 24;
        initiate_fw_info[5] = this->total_firmwaresize >> 16;
        initiate_fw_info[6] = this->total_firmwaresize >> 8;
        initiate_fw_info[7] = this->total_firmwaresize;
        initiate_fw_info[8] = this->get_max_page_count(this->total_firmwaresize) >> 8;
        initiate_fw_info[9] = this->get_max_page_count(this->total_firmwaresize);
        initiate_fw_info[10] = this->get_max_sub_page_count(session.header_len);
        this->command_length = initiate_fw_info[1] + 1;
        initiate_fw_info[this->command_length - 1] = this->calculate_checksum(initiate_fw_info, initiate_fw_info[1]);

        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, initiate_fw_info, sizeof(initiate_fw_info), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_flash_erase_command_to_target_node()
{
    this->print_and_log("(%s) -> Preparing FW Image of Flash Erase Command", client.gateway_id);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::ERASE_FLASH)
    {
        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, Erase_flash_area, sizeof(Erase_flash_area), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_and_get_the_endof_image_transfer()
{
    this->print_and_log("(%s) -> Preparing of Endof FW Image status Command\n", client.gateway_id);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER)
    {
        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, Endof_image_transfer, sizeof(Endof_image_transfer), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_and_calculate_and_verify_crc_value()
{
    print_and_log("(%s)-> %s\n", client.gateway_id, __FUNCTION__);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::NEW_FWIMAGE_CRC_CALCULATE)
    {
        this->calculate_crc_for_target_node();
        Verify_image[4] = (this->CRC >> 8) & 0xFF;
        Verify_image[5] = (this->CRC) & 0xFF;

        unsigned char checksum = Fuota::calculate_checksum(Verify_image, 6);
        Verify_image[6] = checksum;

        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, Verify_image, sizeof(Verify_image), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_and_get_the_firmware_activate_command()
{
    print_and_log("(%s)->%s\n", client.gateway_id, __FUNCTION__);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::ACTIVATE_NEW_FWIMAGE)
    {
        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, activate_read, sizeof(activate_read), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_and_get_the_firmware_activate_status_command()
{
    print_and_log("(%s) -> %s\n", client.gateway_id, __FUNCTION__);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::SEND_ACTIVATE_CMD)
    {
        this->frame_pmesh_fuota_cmd_header_for_target_node(FUOTA_DATA_QUERY, activate_status_read, sizeof(activate_status_read), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_to_read_the_firmware_version_for_comparision()
{
    print_and_log("(%s)-> %s\n", client.gateway_id, __FUNCTION__);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::READ_AND_COMPARE_FW_VERSION)
    {
        this->frame_pmesh_fuota_cmd_header_for_target_node(MESH_COMMISSION_PACKET_TYPE, rf_internal_fw, sizeof(rf_internal_fw), mac, route);
    }
    return this->build_and_store_fuota_cmd();
}

int Fuota::prepare_commands_to_Unsilence_the_network_for_fuota()
{
    this->print_and_log("(%s) -> Silencing the entire path for FUOTA\n", client.gateway_id);
    switch (this->ondemand_fuota_state)
    {
        case FUOTA_STATE::NETWORK_UNSILENCE: {
            this->print_and_log("Network UNSilence State - Preparing FUOTA command\n");

            Fuota *pan_list_ptr = this;
            this->db.get_the_gateway_undernodes_details_for_fuota((char *)client_query_buffer, (unsigned char *)client.gateway_id, pan_list_ptr);

            this->gateway_route_info = new std::vector<route_entry *>();

            for (auto &meter : *(pan_list_ptr->gate_node->meter_list))
            {
                route_entry *entry = new route_entry();
                memset(entry, 0, sizeof(route_entry));

                entry->hop_count = meter->hop_count;
                memcpy(entry->target_mac, meter->meter_mac_address, 8);
                memcpy(entry->route_path, meter->route_path, meter->hop_count * 4);

                this->gateway_route_info->push_back(entry);
            }
            this->client_tx_buffer[2] = MESH_COMMISSION_PACKET_TYPE;
            // 03012025 to now the ondemand mac to exclude to unsilience
            return process_rffuota_disable_sequence(this->current_fuota_target_mac /*this->ondemand_mac_addr*/);
        }

        default:
            return e_failure;
    }
    // after complete leaf node unsilence done go for targetnode unsilence state
    this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_UNSILENCE;
    this->fuota_targetnode_Unsilence_state = PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE;
    return e_success_1;
}

int Fuota::prepare_to_Unsilence_the_target_node()
{
    this->print_and_log("(%s) -> Preparing Target Node UnSilence Command\n", client.gateway_id);
    auto mac = this->current_fuota_target_mac;
    auto route = (router_path_t *)this->current_fuota_route;
    if (this->ondemand_fuota_state == FUOTA_STATE::TARGET_NODE_UNSILENCE)
    {
        switch (this->fuota_targetnode_Unsilence_state)
        {
            case PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, fuota_disable_cmd,
                    sizeof(fuota_disable_cmd), mac, route);
                break;

            case PATH_UNSILENCE_STATE::AT_FUOTA_MODE_ENTRY_DISABLE:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, fuota_modeentry_disable_cmd,
                    sizeof(fuota_modeentry_disable_cmd), mac, route);
                break;

            case PATH_UNSILENCE_STATE::AT_DISABLE_FLASHSAVE:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, flash_write,
                    sizeof(flash_write), mac, route);
                break;

            case PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT:
                this->frame_pmesh_fuota_cmd_header_for_target_node(
                    MESH_COMMISSION_PACKET_TYPE, flash_exit,
                    sizeof(flash_exit), mac, route);
                break;
            default:
                return e_failure;
        }
        return this->build_and_store_fuota_cmd();
    }

    // Update unsilence sub-state
    if (this->fuota_targetnode_Unsilence_state < PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT)
    {
        this->fuota_targetnode_Unsilence_state++;
    }
    else
    {
        // Finished gateway unsilence → move to next main FUOTA state
        this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_UNSILENCE;
        this->fuota_targetnode_Unsilence_state = PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE; // reset
    }
    return e_success_1;
}

int Fuota::prepare_to_Unsilence_the_gateway_node()
{
    this->print_and_log("(%s) -> Preparing Gateway UnSilence Command\n", client.gateway_id);
    if (this->ondemand_fuota_state == FUOTA_STATE::GATEWAY_PATH_UNSILENCE)
    {
        switch (this->fuota_gateway_Unsilence_state)
        {
            case PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, fuota_disable_cmd, sizeof(fuota_disable_cmd));
                break;

            case PATH_UNSILENCE_STATE::AT_FUOTA_MODE_ENTRY_DISABLE:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, fuota_modeentry_disable_cmd, sizeof(fuota_modeentry_disable_cmd));
                break;

            case PATH_UNSILENCE_STATE::AT_DISABLE_FLASHSAVE:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, flash_write, sizeof(flash_write));
                break;

            case PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT:
                this->frame_pmesh_ota_cmd_header(MESH_COMMISSION_PACKET_TYPE, flash_exit, sizeof(flash_exit));
                break;
            default:
                return e_failure;
        }
        return this->build_and_store_fuota_cmd();
    }

    // Update silence sub-state
    if (this->fuota_gateway_Unsilence_state < PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT)
    {
        this->fuota_gateway_Unsilence_state++;
    }
    else
    {
        // Finished gateway silence → move to next main FUOTA state
        this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_UNSILENCE;             // need to decide next state
        this->fuota_gateway_Unsilence_state = PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE; // reset
    }
    return e_success_1;
}

int Fuota::prepare_rollback_to_normal_state()
{
    this->print_and_log("(%s) [FUOTA] prepare_rollback_to_normal_state: setting state to ROLLBACK_TO_NORMAL_COMM_MODE\n", client.gateway_id);
    this->ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
    return this->ondemand_fuota_state;
}

FuotaTimeoutResult Fuota::decide_timeout_action()
{
    this->print_and_log("Function: %s \n", __FUNCTION__);
    if (!cntx_active)
    {
        this->print_and_log("[FUOTA][FATAL] Retry context inactive — aborting\n");
        return FuotaTimeoutResult::TERMINATE;
    }

    if (cntx.retry_count < cntx.max_retries)
    {
        cntx.retry_count++;
        return FuotaTimeoutResult::RETRY_SAME_ROUTE;
    }

    if (cntx.alternate_retry < cntx.alternate_maxcnt)
    {
        cntx.alternate_retry++;
        return FuotaTimeoutResult::RETRY_ALTERNATE_ROUTE;
    }

    return FuotaTimeoutResult::NEXT_FROM_QUEUE;
}

void Fuota::prepare_alternate_route_retry(uint8_t *payload, uint16_t payload_len)
{
    this->print_and_log("Function: %s \n", __FUNCTION__);
    if (!payload || payload_len == 0)
    {
        this->print_and_log("ERROR: Alternate retry with empty payload\n");
        return;
    }

    cntx.retry_count = 0;
    cntx.alternate_retry++;
    this->ondemand_fuota_update_status(18, 1, client.request_id);

    uint8_t router_count = this->client_tx_buffer[12];

    if (router_count == 0)
    {
        this->print_and_log("(%s) -> No routers present\n", client.gateway_id);
        return;
    }

    size_t offset = 13; // Start of router MAC list
    unsigned char last_router_mac[4] = {0};

    for (uint8_t i = 0; i < router_count; i++)
    {
        memcpy(last_router_mac, &this->client_tx_buffer[offset], 4);

        this->print_and_log("(%s) -> Router[%d] MAC = %02X%02X%02X%02X\n", client.gateway_id, i, last_router_mac[0], last_router_mac[1], last_router_mac[2], last_router_mac[3]);

        offset += 4;
    }
    unsigned char ondemand_mac_addr[8] = {0};

    /* Standard ID */
    Utility::ascii_hex_to_bin(ondemand_mac_addr, "3CC1F601", 8);

    /* Append source router MAC (4 bytes) */
    memcpy(&ondemand_mac_addr[4], last_router_mac, 4);

    this->print_and_log("(%s) -> Ondemand MAC = %02X%02X%02X%02X%02X%02X%02X%02X\n", client.gateway_id, ondemand_mac_addr[0], ondemand_mac_addr[1], ondemand_mac_addr[2], ondemand_mac_addr[3], ondemand_mac_addr[4], ondemand_mac_addr[5], ondemand_mac_addr[6], ondemand_mac_addr[7]);

    /* Fetch alternate route */
    bool status = this->build_alternate_route(ondemand_mac_addr);
    if (status == false)
    {
        cntx.alternate_retry = cntx.alternate_maxcnt;
        return;
    }
    else
    {
        /* Rebuild client TX buffer using SAME payload */
        int length = this->update_client_tx_buffer_with_route(this->client_tx_buffer, payload, payload_len);

        /* Persist as current FUOTA command */
        memcpy(this->current_fuota_command, this->client_tx_buffer, length);
        this->print_and_log("Prepared FUOTA alternate route retry (len=%d, alt_retry=%d)\n", length, cntx.alternate_retry);
    }
}

void Fuota::prepare_same_route_retry()
{
    this->print_and_log("Function: %s \n", __FUNCTION__);
    cntx.retry_count++;

    // this->current_fuota_command = &this->client_tx_buffer[0];
    print_and_log("Before send Tx:");
    for (int i = 0; i < this->client_tx_buffer[1] + 1; i++)
    {
        print_and_log("%02X ", this->client_tx_buffer[i]);
    }
    print_and_log("\n");
    this->build_and_store_fuota_cmd();

    this->print_and_log("Retrying FUOTA on same route\n");
}

bool Fuota::prepare_next_fuota_from_queue()
{
    if (client.dequeue_fuota(this->odmfuota_cmd))
    {
        this->print_and_log("Dequeued next FUOTA command: %s\n", this->odmfuota_cmd);
        return true;
    }

    return false;
}

void Fuota::start_retry_context()
{
    cntx = Fuota_retry_contex{}; // value reset
    cntx_active = true;

    this->print_and_log("[FUOTA] Retry context initialized (max=%d alt=%d)\n", cntx.max_retries, cntx.alternate_maxcnt);
}

void Fuota::stop_retry_context()
{
    this->print_and_log("Function: %s \n", __FUNCTION__);
    cntx_active = false;
}

int Fuota::command_response_timeout_at_fuota()
{
    this->print_and_log("(%s) -> %s\n", client.gateway_id, __FUNCTION__);

    auto action = decide_timeout_action();

    switch (action)
    {
        case FuotaTimeoutResult::RETRY_SAME_ROUTE: {
            this->print_and_log("[FUOTA] Retry same route (%d/%d)\n", cntx.retry_count, cntx.max_retries);

            this->build_and_store_fuota_cmd();
            return client.set_recv_timeout_for_client(12);
        }

        case FuotaTimeoutResult::RETRY_ALTERNATE_ROUTE: {
            this->print_and_log("[FUOTA] Retry via alternate route(%d/%d)\n", cntx.alternate_retry, cntx.alternate_maxcnt);
            if (this->current_fuota_command == NULL)
            {
                printf("null this->current_fuota_command\n");
            }
            this->prepare_alternate_route_retry(this->current_fuota_command, this->current_fuota_command[1] + 1);

            this->build_and_store_fuota_cmd();
            return client.set_recv_timeout_for_client(12);
        }

        case FuotaTimeoutResult::NEXT_FROM_QUEUE: {
            this->print_and_log("[FUOTA] Retries exhausted — next queue entry\n");

            this->stop_retry_context();

            if (client.dequeue_fuota(this->odmfuota_cmd))
            {
                this->cmd_bytes = std::move(odmfuota_cmd);
                this->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
                return SUCCESS;
            }
            // changed from rllback to netwk unsilience state 05012026
            this->ondemand_fuota_state = FUOTA_STATE::NETWORK_UNSILENCE;
            return SUCCESS;
        }

        case FuotaTimeoutResult::TERMINATE:
        default: {
            this->print_and_log("[FUOTA] Fatal timeout — aborting FUOTA\n");

            this->stop_retry_context();
            this->ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
            return e_failure;
        }
    }
}

// not using as of now
int Fuota::frame_a_retry_comamnd_for_pmesh_routine()
{
    this->print_and_log("(%s) -> Function: Retry command for pmesh Routine\n", client.gateway_id);
    unsigned char last_command[1024] = {0};
    char txd_time[26] = {0};
    int send_req_count = 0;
    int l_len = 0;
    int l_written = 0;
    memset(last_command, 0, sizeof(last_command));

    // Store the original command to resend on retry
    memcpy(last_command, this->client_tx_buffer, sizeof(this->client_tx_buffer));
    int last_command_length = this->client_tx_buffer[1] + 2; // Length is stored in byte 1

    do
    {
        if (send_req_count < cntx.max_retries)
        {
            l_written = send(client.get_client_socket(), last_command, last_command_length - 1, 0);

            gettimeofday(&this->startTime, nullptr);
            // page_ctx.current_page++;

            this->print_and_log("l_written ", l_written);
            this->print_and_log("this->current_fuotacommand[] ", sizeof(this->current_fuota_command), (unsigned char *)&this->current_fuota_command[0]);

            if (l_written == -1) // socket error
            {
                std::cout << "send() " << strerror(errno) << std::endl;
                // this->thread_run = 0;
                return e_failure;
            }
            if (errno == EPIPE)
            {
                std::cout << "send()epipe " << strerror(errno) << std::endl;
                // this->thread_run = 0;
                return e_failure;
            }

            int millisec = this->client_get_time(txd_time);
            int ret = clock_gettime(CLOCK_MONOTONIC, &this->start_time);
            if (ret == -1)
            {
                int err_number = errno;
                std::cout << "error_val: " << err_number << ", clockget_time return val: " << strerror(errno) << std::endl;
            }
            this->print_and_log("l_written ", l_written);
            this->print_and_log("Tx Retry Command:");

            printf("(%s) ->[%s.%03d] = ", client.gateway_id, txd_time, millisec);

            this->print_and_log("client.gateway_id ", client.gateway_id);

            for (int l_index = 0; l_index < l_written; l_index++)
            {
                printf("%02X ", this->current_fuota_command[l_index]);
            }
            std::cout << std::endl;

            if (l_written == l_len)
            {
                break;
            }
            else
            {
                l_len = l_len - l_written;
            }
            if (l_len < 0) // this is just to be at the safe side
            {
                this->print_and_log("Negative valueof l_len: ", client.gateway_id);
                break;
            }
        }
        else
        {
            std::cout << "(" << client.gateway_id << "): Retry write Exceeded ,Aborting the Gateway Conncetion" << std::endl;
            // this->thread_run = 0;
            return e_failure;
        }
    } while (cntx.retry_count < cntx.max_retries);

    return client.set_recv_timeout_for_client(12);
}

int Fuota::update_client_tx_buffer_with_route(uint8_t *client_tx_buffer, const uint8_t *payload, int payload_len)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    if (!client_tx_buffer || !payload || !this->current_fuota_route)
        return -1;

    int idx = 0;
    client_tx_buffer[idx++] = 0x2E; // Start byte

    int len_index = idx;
    client_tx_buffer[idx++] = 0x00; // length placeholder

    client_tx_buffer[idx++] = 0x03; // Packet type

    // IDs
    Utility::extract_ids((const char *)client.gateway_id, this->panid, this->dcu_short_addr);

    // PAN ID
    memcpy(&client_tx_buffer[idx], this->panid, 4);
    idx += 4;

    // SRC addr
    memcpy(&client_tx_buffer[idx], this->dcu_short_addr, 4);
    idx += 4;

    // router index
    client_tx_buffer[idx++] = 0x00;

    // hop count
    router_path_t *route = (router_path_t *)this->current_fuota_route;
    client_tx_buffer[idx++] = route->hop_count;

    // hop paths
    for (int i = 0; i < route->hop_count; i++)
    {
        if (idx + 4 > CLIENT_TX_BUFFER_SIZE) // safety check
        {
            this->print_and_log("(%s) -> ERROR: buffer overflow at hop paths\n", client.gateway_id);
            this->ondemand_fuota_update_status(22, 0, client.request_id);
            return -1;
        }
        memcpy(&client_tx_buffer[idx], route->paths[i], 4);
        idx += 4;
    }

    // payload
    if (idx + payload_len > CLIENT_TX_BUFFER_SIZE)
    {
        this->print_and_log("(%s) -> ERROR: payload overflow, payload_len=%d\n", client.gateway_id, payload_len);
        return -1;
    }
    memcpy(&client_tx_buffer[idx], payload, payload_len);
    idx += payload_len;

    client_tx_buffer[len_index] = idx - 1; // length
    return idx;
}

int Fuota::cansend_fuota_next_command()
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    switch (this->ondemand_fuota_state)
    {
        case FUOTA_STATE::OPEN_FILE: {
            this->print_and_log("(%s) -> Get the Firmware file path and Name - Preparing FUOTA command\n", client.gateway_id);
            print_and_log("[FUOTA][ASSERT] Active cmd_bytes = %s\n", std::string(cmd_bytes.begin(), cmd_bytes.end()).c_str());

            if (this->resume_fuota_flag == true)
            {
                this->print_and_log("(%s) -> Resumption of Pending Fuota Request\n", client.gateway_id);
                this->open_requested_firmware(this->get_app_base_path(), this->firmware_file, this->firmware_path);
                this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_SILENCE;
            }
            else
            {
                this->open_requested_firmware(this->get_app_base_path(), this->firmware_file, this->firmware_path);

                this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_SILENCE;
            }
            return e_success_0;
        }
        break;

        case FUOTA_STATE::GATEWAY_PATH_SILENCE:
            this->print_and_log("(%s) -> Gateway Path Silence State - Preparing FUOTA command\n", client.gateway_id);
            return prepare_gateway_silence_cmd();

        case FUOTA_STATE::TARGET_NODE_SILENCE:
            this->print_and_log("(%s) -> Target Node Path Silence State - Preparing FUOTA command\n", client.gateway_id);
            return prepare_targetnode_silence_cmd();

        case FUOTA_STATE::NETWORK_SILENCE: {
            this->print_and_log("(%s)-> Network Silence State - Preparing FUOTA command\n", client.gateway_id);
            int status = this->db.check_gateway_nodelist_empty_or_not((unsigned char *)client.gateway_id);
            if (status > 0)
            {
                this->print_and_log("Gateway node list is empty,proceed with Gateway Only FUOTA\n");

                memcpy(&this->current_fuota_target_mac[4], this->dcu_short_addr, 4);
                for (int i = 0; i < 4; i++)
                {
                    print_and_log("%02X ", this->current_fuota_target_mac[i]);
                }
                print_and_log("\n");
                this->ondemand_fuota_state = FUOTA_STATE::SECTOR_READ; // skip silencing if no under nodes
                return this->prepare_get_sector_read_for_target_node();
            }
            return this->prepare_silence_the_entire_network_for_fuota();
        }

        case FUOTA_STATE::SECTOR_READ:
            this->print_and_log("(%s) -> Get sector Read State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_get_sector_read_for_target_node();

        case FUOTA_STATE::FWIMAGE_OF_SECTOR_COUNT:
            this->print_and_log("(%s) -> FW Image of Sector Count State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_fwimage_of_sector_count_cmd();

        case FUOTA_STATE::ERASE_FLASH:
            this->print_and_log("(%s) -> Flash Erase Area State - Preparing FUOTA command\n", client.gateway_id);
            return prepare_flash_erase_command_to_target_node();

        case FUOTA_STATE::FW_IMAGE_TRANSFER:
            this->print_and_log("(%s) -> Flash Image Transfer State - Preparing FUOTA command\n", client.gateway_id);
            return this->fuota_uploading_process();

            // case FUOTA_STATE::LAST_UPDATE_FWIMAGE_OF_SECTORCOUNT: // think not require
            //     this->print_and_log("Flash Image Transfer State - Preparing FUOTA command");
            //     return fuota_uploading_process();

        case FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER: //
            this->print_and_log("(%s) -> Endof page Transfer State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_and_get_the_endof_image_transfer();

        case FUOTA_STATE::NEW_FWIMAGE_CRC_CALCULATE:
            this->print_and_log("(%s) -> Calculate crc State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_and_calculate_and_verify_crc_value();

        case FUOTA_STATE::ACTIVATE_NEW_FWIMAGE: // write
            this->print_and_log("(%s) -> Activate image State - Preparing FUOTA command\n", client.gateway_id);
            // Basedon prev cmd status wait and send next cmd decision
            return this->prepare_and_get_the_firmware_activate_command();

        case FUOTA_STATE::SEND_ACTIVATE_CMD: // status read
            this->print_and_log("(%s) -> Activate image status read confirmation State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_and_get_the_firmware_activate_status_command();

        case FUOTA_STATE::READ_AND_COMPARE_FW_VERSION:
            this->print_and_log("(%s) -> Final comparision of Firmware Version - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_to_read_the_firmware_version_for_comparision();

        case FUOTA_STATE::NETWORK_UNSILENCE: {
            this->print_and_log("(%s) -> Network UnSilence State - Preparing FUOTA command\n", client.gateway_id);
            int status = this->db.check_gateway_nodelist_empty_or_not((unsigned char *)client.gateway_id);
            if (status > 0)
            {
                this->print_and_log("Gateway node list is empty, Move to gateway Path Un-silence Only\n");
                memcpy(&this->current_fuota_target_mac[4], this->dcu_short_addr, 4);
                for (int i = 0; i < 4; i++)
                {
                    print_and_log("%02X ", this->current_fuota_target_mac[i]);
                }
                print_and_log("\n");
                // ---------skip Uns-silencing if no under nodes---------
                this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_UNSILENCE;
                return this->prepare_to_Unsilence_the_gateway_node();
            }
            return this->prepare_commands_to_Unsilence_the_network_for_fuota();
        }

        case FUOTA_STATE::TARGET_NODE_UNSILENCE:
            this->print_and_log("(%s) -> Target Node Path UnSilence State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_to_Unsilence_the_target_node();

        case FUOTA_STATE::GATEWAY_PATH_UNSILENCE:
            this->print_and_log("(%s) -> Gateway Path UNSilence State - Preparing FUOTA command\n", client.gateway_id);
            return this->prepare_to_Unsilence_the_gateway_node();

        case FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE:
            return this->prepare_rollback_to_normal_state();

        default:
            return e_failure;
    }
    return e_success_1;
}

/*
 * For invalid state taking no action and No alternate path chcek for this state
 */
int Fuota::handling_of_pmesh_error_routines_for_retry_at_fuota()
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    if (cntx.retry_count >= cntx.max_retries)
    {
        return e_failure;
    }
    else
    {
        return this->cansend_fuota_next_command();
    }
}

int Fuota::validate_the_fuota_response_basedon_packet_type(unsigned char *data, unsigned int len)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    int error_code = data[len - 1];

    /*
     *Silence and Unsilence the network and include firmware read  the pkt type as 0x03
     *Fuota sequence of commands pkt type as 0x09
     */
    if ((data[2] == 0x03 || data[2] == 0x09))
    {
        switch (error_code)
        {
            case 0x00: {
                this->print_and_log("Start Byte Error state - Fuota validation of Response\n");
                return e_success_1; // retry 3 include previous count go for next request if queue not empty
            }
            break;

            case 0x01: {
                this->print_and_log("Lengthe Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x02: {
                this->print_and_log("Packet Type Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x04: {
                this->print_and_log("Source address Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x05: {
                this->print_and_log("router index Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x0A: {
                this->print_and_log("Source and Destination Address Matching Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x0B: {
                this->print_and_log("Fuota Mode Disable Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x0C: {
                this->print_and_log("Fuota Mode Enable Error state - Fuota validation of Response\n");
                return e_success_1;
            }
            break;

            case 0x06: {
                this->print_and_log("Fuota Timeout state - Fuota validation of Response\n");
                cntx.retry_count++;
                return e_success_2;
            }
            break;

            case 0x07: {
                this->print_and_log("Fuota command in progress state - Fuota validation of Response\n");
                return e_success_3;
            }
            break;

            default:
                break;
        }
    }
    return e_success_1; // for success
}

/*
 * Dequeuing of next request and getinto respective file path
 */
void Fuota::handle_fuota_completion()
{
    this->print_and_log("(%s) -> Function:%s\n", client.gateway_id, __FUNCTION__);
    std::vector<uint8_t> next_cmd;

    if (client.dequeue_fuota(next_cmd))
    {
        // Safe debug print
        std::string cmd_str(next_cmd.begin(), next_cmd.end());
        print_and_log("(%s) -> FUOTA next command dequeued: %s\n", client.gateway_id, cmd_str.c_str());

        cmd_bytes = std::move(next_cmd); // Move ownership
                                         /* 🔑 CRITICAL FIX */
        if (!parse_and_update_fuota_request(cmd_str))
        {
            print_and_log("(%s) -> FUOTA command parse failed\n", client.gateway_id);
            return;
        }

        // reset_crc(crc);
        waiting_for_response = false;
        fuota_retry_count = 0;

        print_and_log("(%s) -> FUOTA After dequeued command: %s\n", client.gateway_id, cmd_str.c_str());
        print_and_log("[FUOTA] Dequeued REQ = %d FWFILE = %s\n", client.request_id, client.firmware_filename.c_str());
        ondemand_fuota_state = FUOTA_STATE::OPEN_FILE; // Continue FSM
    }
    else
    {
        print_and_log("(%s) -> FUOTA queue empty, unsilencing network\n", client.gateway_id);

        this->ondemand_fuota_state = FUOTA_STATE::NETWORK_UNSILENCE;
        cansend_fuota_next_command();
    }
}

bool Fuota::wait_after_flash(int wait_sec, std::atomic<bool> &thread_run)
{
    std::mutex mtx;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mtx);

    return cv.wait_for(lock, std::chrono::seconds(wait_sec), [&thread_run]() { return !thread_run.load(std::memory_order_acquire); });
}

bool Fuota::parse_and_update_fuota_request(const std::string &cmd)
{
    print_and_log("(%s) -> Function:%s\n", client.gateway_id, __FUNCTION__);

    std::vector<std::string> parts = client.split(cmd, ':');

    if (parts.size() < 7)
    {
        print_and_log("[FUOTA][ERROR] Invalid FUOTA cmd, parts=%zu, cmd=%s\n", parts.size(), cmd.c_str());
        return false;
    }

    client.request_id = std::stoi(parts[0]);
    client.firmware_path = parts[5];
    client.firmware_filename = parts[6];

    /* 🔑 CRITICAL */
    this->firmware_file = client.firmware_filename;

    print_and_log("[FUOTA] Parsed REQ=%d FWFILE=%s\n", client.request_id, this->firmware_file.c_str());

    return true;
}

int Fuota::client_process_rf_fuota_data(unsigned char *data, int len)
{
    this->print_and_log("(%s) -> Function:%s, next state:%d\n", client.gateway_id, __FUNCTION__, this->ondemand_fuota_state);

    switch (this->ondemand_fuota_state)
    {
        case FUOTA_STATE::OPEN_FILE: {
            if (this->resume_fuota_flag == true)
            {
                this->print_and_log("(%s) -> Resumption of Pending Fuota_Request\n", client.gateway_id);
                this->open_requested_firmware(this->get_app_base_path(), this->resume_file_name, this->resume_fwpath);
            }
            else
            {
                this->open_requested_firmware(this->get_app_base_path(), this->firmware_file, this->firmware_path);
            }
            this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_SILENCE;
            this->fuota_gateway_silence_state = PATH_SILENCE_STATE::AT_FUOTA_ENABLE;
        }
        break;

        case FUOTA_STATE::GATEWAY_PATH_SILENCE: {
            this->print_and_log("(%s) -> Processing Gateway Path Silence Response\n", client.gateway_id);

            this->fuota_resp_status = data[2];
            if (this->fuota_resp_status == COMMISSION_RESP_PACKET_TYPE)
            {
                this->ondemand_fuota_update_status(6, 1, client.request_id);
                if (data[len - 1] == 0x00 && (data[len - 2] == 0x9B || data[len - 2] == 0x9D || data[len - 2] == 0x02 || data[len - 2] == 0x01))
                {
                    if (this->fuota_gateway_silence_state < PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT)
                    {
                        this->fuota_gateway_silence_state++;
                    }
                    else
                    {
                        db.insert_update_fuota_Unsilenced_meter_details_in_db((char *)client_query_buffer, (unsigned char *)client.gateway_id, (unsigned char *)client.gateway_id, 1, 0);

                        // After flash exit wait for particular period of time then move forward
                        this->wait_after_flash(30, this->thread_run); // 02012026
                        if (this->router_path.hop_count > 0)
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_SILENCE;
                        }
                        else
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::NETWORK_SILENCE;
                        }
                        this->fuota_gateway_silence_state = PATH_SILENCE_STATE::AT_FUOTA_ENABLE;
                    }
                }
                else
                {
                    // go for retry
                    this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_SILENCE;
                }
            }
            else
            {
                // go for retry
                this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_SILENCE;
                cntx.retry_count++;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->command_response_timeout_at_fuota();
            }
        }
        break;

        case FUOTA_STATE::TARGET_NODE_SILENCE: {
            this->print_and_log("(%s) -> Processing Target Node Silence Response\n", client.gateway_id);

            if (data[2] == 0x04)
            {
                if (this->fuota_targetnode_silence_state < PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT)
                {
                    this->fuota_targetnode_silence_state++;
                }
                else
                {
                    // After flash exit wait for particular period of time then move forward
                    // client.set_poll_timeout(30);
                    this->wait_after_flash(30, this->thread_run); // 02012026
                    this->ondemand_fuota_state = FUOTA_STATE::NETWORK_SILENCE;
                    this->fuota_targetnode_silence_state = PATH_SILENCE_STATE::AT_FUOTA_ENABLE;
                }
            }
            else
            {
                // go for retry
                this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_SILENCE;
                cntx.retry_count++;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->command_response_timeout_at_fuota();
            }
        }
        break;

        case FUOTA_STATE::NETWORK_SILENCE: {
            // After flash exit wait for particular period of time then move forward
            this->print_and_log("(%s) ->  Network Silence Response Processing\n", client.gateway_id);

            if (data[2] == 0x04)
            {
                if (this->network_silence_state < PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT)
                {
                    this->network_silence_state++;
                }
                else
                {
                    // After flash exit wait for particular period of time then move forward
                    // client.set_poll_timeout(30);
                    this->wait_after_flash(30, this->thread_run); // 02012026
                    this->ondemand_fuota_state = FUOTA_STATE::SECTOR_READ;
                }
            }
            else
            {
                // go for retry
                this->ondemand_fuota_state = FUOTA_STATE::NETWORK_SILENCE;
                cntx.retry_count++;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->command_response_timeout_at_fuota();
            }
        }
        break;

        case FUOTA_STATE::SECTOR_READ: {
            if (data[len - 2] == 0x00 && data[2] == 0x0A)
            {
                // Shift the high byte 8 bits to the left and use bitwise OR to add the low byte
                this->sector_value = ((data[len - 3] << 8) | data[len - 2]);
                this->ondemand_fuota_update_status(7, 1 /*success*/, client.request_id);
                this->print_and_log("(%s) -> Sector value  :%d\n", client.gateway_id, this->sector_value);

                this->waiting_for_response = false;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;

                int ret_value = this->dynamic_fuota_calculate();
                if (ret_value == e_failure)
                {
                    if (session.file.empty() || this->total_firmwaresize <= 0)
                    {
                        this->ondemand_fuota_update_status(2, 1, client.request_id);
                        this->ondemand_fuota_state = FUOTA_STATE::NETWORK_UNSILENCE;
                        // return e_success_1;
                    }
                }
                else
                {
                    this->ondemand_fuota_state = FUOTA_STATE::FWIMAGE_OF_SECTOR_COUNT;
                }
            }
            else
            {
                this->print_and_log("Retry at sector read\n");
                // go for retry
                if (cntx.retry_count < 3)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    cntx.alternate_retry = 0;
                    use_alternate_fuota_route = false;
                    this->ondemand_fuota_state = FUOTA_STATE::SECTOR_READ;
                }
                else
                {
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::FWIMAGE_OF_SECTOR_COUNT: {
            this->print_and_log("(%s) -> Processing Firmware size\n", client.gateway_id);

            if (data[len - 2] == 0x00 && data[2] == 0x0A)
            {
                this->ondemand_fuota_update_status(9, 1 /*success*/, client.request_id);
                waiting_for_response = false;

                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                stop_retry_context();
                this->ondemand_fuota_state = FUOTA_STATE::ERASE_FLASH;
            }
            else
            {
                // go for retry
                if (cntx.retry_count < 3)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::FWIMAGE_OF_SECTOR_COUNT;
                }
                else
                {
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::ERASE_FLASH: {
            this->print_and_log("(%s) -> Processing at Erased Old Firmware\n", client.gateway_id);

            if (data[len - 2] == 0x00 && data[2] == 0x0A)
            {
                this->ondemand_fuota_update_status(10, 1 /*success*/, client.request_id);
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                // this->ondemand_fuota_state = FUOTA_STATE::NEW_FWIMAGE_CRC_CALCULATE;
                this->ondemand_fuota_state = FUOTA_STATE::FW_IMAGE_TRANSFER;
            }
            else
            {
                // go for retry
                if (cntx.retry_count < 3)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::ERASE_FLASH;
                }
                else
                {
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::FW_IMAGE_TRANSFER: {
            this->print_and_log("(%s) -> Processing of Image tarnsfer\n", client.gateway_id);
            this->ondemand_fuota_update_status(11, 1 /*success*/, client.request_id);
            this->ondemand_fuota_state = FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER;
        }
        break;

        case FUOTA_STATE::LAST_UPDATE_FWIMAGE_OF_SECTORCOUNT: {
            this->print_and_log("(%s) -> Processing of the last sector page read\n", client.gateway_id);
            this->ondemand_fuota_update_status(12, 1 /*success*/, client.request_id);
            if (data[len - 2] == 0x00 && data[2] == 0x0A)
            {
                this->waiting_for_response = false;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->ondemand_fuota_state = FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER;
            }
            else
            {
                // go for retry
                this->ondemand_fuota_state = FUOTA_STATE::LAST_UPDATE_FWIMAGE_OF_SECTORCOUNT;
            }
        }
        break;

        case FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER: { // 2F 04 08 01 3C
            this->print_and_log("(%s) -> Processingof the endOf page transfer\n", client.gateway_id);

            if (data[2] == 0x0A && (data[len - 2] == 0x00) && data[len - 3] == 0x01)
            {
                this->ondemand_fuota_update_status(13, 1 /*success*/, client.request_id);
                this->waiting_for_response = false;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->ondemand_fuota_state = FUOTA_STATE::NEW_FWIMAGE_CRC_CALCULATE;
            }
            else
            {
                this->print_and_log("(%s) -> Endof image Retry state\n", client.gateway_id);

                // go for retry
                if (cntx.retry_count < 3)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER;
                }
                else
                {
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::NEW_FWIMAGE_CRC_CALCULATE: {
            this->print_and_log("(%s) -> caculating of the firmware of latest upload\n", client.gateway_id);

            if ((data[len - 4] == 0x00 && data[len - 6] == 0x0B) && data[2] == 0x0A)
            {
                this->ondemand_fuota_update_status(14, 1 /*success*/, client.request_id);
                this->reset_crc(this->crc); // To reset the crc upon success
                this->waiting_for_response = false;
                cntx.retry_count = 0;
                this->rf_fuota_crc_calculated = false;

                this->ondemand_fuota_state = FUOTA_STATE::ACTIVATE_NEW_FWIMAGE;
                // wait for 30sec
            }
            else
            {
                this->print_and_log("(%s) -> Retry of crc verify status\n", client.gateway_id);

                // go for retry
                if (cntx.retry_count < 2)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::NEW_FWIMAGE_CRC_CALCULATE;
                }
                else
                {
                    this->ondemand_fuota_update_status(14, 0 /*Failure*/, client.request_id);
                    this->reset_crc(this->crc);
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::ACTIVATE_NEW_FWIMAGE: { // 2F 04 0E 01 42
            this->print_and_log("(%s) -> Processing of newly activated firmware image status\n", client.gateway_id);

            if ((data[len - 2] == 0x00 || data[len - 2] == 0x01) && data[len - 4] == 0x0F && data[2] == 0x0A)
            {
                // success 01 failure ?
                // client.set_poll_timeout(30);
                this->ondemand_fuota_update_status(15, 1 /*success*/, client.request_id);
                this->wait_after_flash(30, this->thread_run); // 02012026
                this->waiting_for_response = false;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->ondemand_fuota_state = FUOTA_STATE::SEND_ACTIVATE_CMD;
            }
            else
            {
                this->print_and_log("(%s) -> Retry of activate image status:%d\n", client.gateway_id, cntx.retry_count);

                // go for retry
                if (cntx.retry_count < 3)
                {
                    this->ondemand_fuota_update_status(16, 1 /*success*/, client.request_id);
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::ACTIVATE_NEW_FWIMAGE;
                }
                else
                {
                    this->ondemand_fuota_update_status(16, 0 /*Failure*/, client.request_id);
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::SEND_ACTIVATE_CMD: { // 2F 04 10 01 44
            this->print_and_log("(%s) -> Processing at activate firmware conformation state\n", client.gateway_id);

            if ((data[len - 2] == 0x00 || data[len - 2] == 0x01) && data[2] == 0x0A)
            {
                this->ondemand_fuota_update_status(17, 1 /*success*/, client.request_id);
                this->waiting_for_response = false;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->wait_after_flash(30, this->thread_run); // 02012026
                this->ondemand_fuota_state = FUOTA_STATE::READ_AND_COMPARE_FW_VERSION;
            }
            else
            {
                // go for retry at activate image read state
                if (cntx.retry_count < 3)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::ACTIVATE_NEW_FWIMAGE;
                }
                else
                {
                    this->ondemand_fuota_update_status(17, 0 /*Failure*/, client.request_id);
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;
                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::READ_AND_COMPARE_FW_VERSION: {
            this->print_and_log("(%s) -> Processing of the firmware comparision\n", client.gateway_id);

            if (data[2] == 0x04)
            {
                this->ondemand_fuota_update_status(18, 1 /*success*/, client.request_id);
                this->waiting_for_response = false;
                cntx.retry_count = 0;
                cntx.alternate_retry = 0;
                use_alternate_fuota_route = false;
                this->comparision_of_firmware_from_meterdetails_and_from_latest_uploaded_firmware(data);

                this->handle_fuota_completion();
            }
            else
            {

                // go for retry
                if (cntx.retry_count < 3)
                {
                    this->waiting_for_response = true;
                    cntx.retry_count++;
                    this->ondemand_fuota_state = FUOTA_STATE::READ_AND_COMPARE_FW_VERSION;
                }
                else
                {
                    this->ondemand_fuota_update_status(18, 0 /*failure*/, client.request_id);
                    this->waiting_for_response = false;
                    cntx.retry_count = 0;

                    this->handle_fuota_completion();
                }
            }
        }
        break;

        case FUOTA_STATE::NETWORK_UNSILENCE: {
            this->print_and_log("(%s) ->  Processing of the network unsilence state\n", client.gateway_id);

            this->fuota_resp_status = data[2];
            if (this->fuota_resp_status == COMMISSION_RESP_PACKET_TYPE)
            {
                this->ondemand_fuota_update_status(20, 1 /*success*/, client.request_id);
                if (data[len - 1] == 0x00 && (data[len - 2] == 0x9B || data[len - 2] == 0x9D || data[len - 2] == 0x02))
                {
                    if (this->fuota_gateway_Unsilence_state < PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT)
                    {
                        this->fuota_gateway_Unsilence_state++;
                    }
                    else
                    {
                        // After flash exit wait for particular period of time then move forward
                        // client.set_poll_timeout(30);
                        this->wait_after_flash(30, this->thread_run); // 02012026
                        if (this->router_path.hop_count > 0)
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_UNSILENCE;
                        }
                        else
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::NETWORK_UNSILENCE;
                        }
                        this->fuota_gateway_Unsilence_state = PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE;
                    }
                }
                else if (data[len - 1] == 0x00 && data[len - 2] == 0x01)
                {
                    // FUOTA already enabled state move to next state
                    if (this->fuota_gateway_Unsilence_state < PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT)
                    {
                        this->fuota_gateway_Unsilence_state++;
                    }
                    else
                    {
                        // After flash exit wait for particular period of time then move forward
                        // client.set_poll_timeout(30);
                        this->wait_after_flash(30, this->thread_run); // 02012026
                        if (this->router_path.hop_count > 0)
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_UNSILENCE;
                        }
                        else
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::NETWORK_UNSILENCE;
                        }
                        this->fuota_gateway_Unsilence_state = PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE;
                    }
                }
                else
                {
                    this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_UNSILENCE;
                }
            }
            else if (this->fuota_resp_status == MESH_COMMISSION_PACKET_TYPE)
            {
                this->ondemand_fuota_update_status(20, 0 /*Failure*/, client.request_id);
                // failure response retry state of appropriate state
            }
            else
            { // success move to next state
                this->ondemand_fuota_state = FUOTA_STATE::TARGET_NODE_UNSILENCE;
            }
        }
        break;

        case FUOTA_STATE::TARGET_NODE_UNSILENCE: {
            this->print_and_log("(%s) -> Processing of the target node unsilence state\n", client.gateway_id);

            this->fuota_resp_status = data[2];
            if (this->fuota_resp_status == COMMISSION_RESP_PACKET_TYPE)
            {
                if (data[len - 1] == 0x00 && (data[len - 2] == 0x9B || data[len - 2] == 0x9D || data[len - 2] == 0x02 || data[len - 2] == 0x01))
                {
                    if (this->fuota_gateway_Unsilence_state < PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT)
                    {
                        this->fuota_gateway_Unsilence_state++;
                    }
                    else
                    {
                        // After flash exit wait for particular period of time then move forward
                        // client.set_poll_timeout(30);
                        this->wait_after_flash(30, this->thread_run); // 02012026
                        if (this->router_path.hop_count > 0)
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_UNSILENCE;
                        }
                        else
                        {
                            this->ondemand_fuota_state = FUOTA_STATE::NETWORK_UNSILENCE;
                        }
                        this->fuota_gateway_Unsilence_state = PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE;
                    }
                }
                else
                {
                    // move to next state
                    this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_UNSILENCE;
                }
            }
            else
            {
                // failure or  invalid response
                this->ondemand_fuota_state = FUOTA_STATE::FUOTA_STATE::TARGET_NODE_UNSILENCE;
            }
        }
        break;

        case FUOTA_STATE::GATEWAY_PATH_UNSILENCE: {
            this->print_and_log("(%s) ->Processing of the gateway network unsilence state\n", client.gateway_id);

            this->fuota_resp_status = data[2];
            if (this->fuota_resp_status == COMMISSION_RESP_PACKET_TYPE)
            {
                if (data[len - 1] == 0x00 && (data[len - 2] == 0x9B || data[len - 2] == 0x9D || data[len - 2] == 0x02 || data[len - 2] == 0x01))
                {
                    if (this->fuota_gateway_Unsilence_state < PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT)
                    {
                        this->fuota_gateway_Unsilence_state++;
                    }
                    else
                    {
                        db.insert_update_fuota_Unsilenced_meter_details_in_db((char *)client_query_buffer, (unsigned char *)client.gateway_id, (unsigned char *)client.gateway_id, 0, 0);

                        // After flash exit wait for particular period of time then move forward
                        // After flash exit, wait 30 sec before moving forward
                        this->wait_after_flash(30, this->thread_run); // 02012026

                        // Proceed next stage correct state only
                        bool dequeued = client.dequeue_fuota(this->odmfuota_cmd);
                        if (dequeued)
                        {
                            // queue not empty dequeue the request and continue the process
                            this->print_and_log("(%s) -> [FUOTA] Command dequeued, moving to OPEN_FILE state\n", client.gateway_id);
                            this->cmd_bytes = std::move(odmfuota_cmd);
                            this->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
                        }
                        else
                        {
                            this->print_and_log("(%s) -> [FUOTA] No pending command, rollback to normal mode\n", client.gateway_id);
                            this->ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
                        }
                    }
                }
                else
                {
                    // move to next state before that verify queue empty or not?
                    bool dequeued = client.dequeue_fuota(this->odmfuota_cmd);
                    if (dequeued)
                    {
                        // queue not empty dequeue the request and continue the process
                        this->print_and_log("(%s) -> [FUOTA] Command dequeued, moving to OPEN_FILE state\n", client.gateway_id);
                        this->cmd_bytes = std::move(odmfuota_cmd);
                        this->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
                    }
                    else
                    {
                        this->print_and_log("(%s) -> [FUOTA] No pending command, rollback to normal mode\n", client.gateway_id);
                        this->ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
                    }
                }
            }
            else if (this->fuota_resp_status == MESH_COMMISSION_PACKET_TYPE)
            {
                // timeout/command in progress invoke
            }
            else
            {
                // move to next state before that verify queue empty or not?
                bool dequeued = client.dequeue_fuota(this->odmfuota_cmd);
                if (dequeued)
                {
                    // queue not empty dequeue the request and continue the process

                    this->print_and_log("(%s) -> FUOTA Command dequeued, moving to OPEN_FILE state\n", client.gateway_id);
                    this->cmd_bytes = std::move(odmfuota_cmd);
                    this->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
                }
                else
                {
                    this->print_and_log("(%s) -> FUOTA No pending command, rollback to normal mode\n", client.gateway_id);
                    this->ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
                }
            }
        }
        break;

        case FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE: {
            this->print_and_log("(%s) -> Back to NOrmal communication state\n", client.gateway_id);
            bool dequeued = client.dequeue_fuota(this->odmfuota_cmd);
            if (dequeued)
            {
                // queue not empty dequeue the request and continue the process
                this->print_and_log("(%s) -> FUOTA Command dequeued, moving to OPEN_FILE state\n", client.gateway_id);

                this->cmd_bytes = std::move(odmfuota_cmd);
                this->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE;
            }
            else
            {
                // exit from fuota state close all the fd's,requestid's,db updation of silence/un-silnce nodes list,
                // firmware version in np
                this->print_and_log("(%s) \n----------------Fuota done/list empty--------------\n", client.gateway_id);
                client.stateInfo.currentState = ClientCurrentState::IDLE;
                client.stateInfo.timeoutState = ClientTimeoutState::TIMER_NONE;
                return e_success_0; // back to normal state
            }
        }
        break;

        default:
            break;
    }

    this->cansend_fuota_next_command();
    return SUCCESS;
}

void Fuota::reset_crc(unsigned short int &crc)
{
    crc = 0x0000;
    this->print_and_log("(%s) -> [reset_crc] CRC reset to 0x0000\n", client.gateway_id);
}

uint16_t Fuota::crc16_update(const uint8_t *data, uint32_t len, uint16_t crc)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    for (uint32_t i = 0; i < len; i++)
    {
        crc = (crc >> 8) ^ crc_tab16[(crc ^ data[i]) & 0xFF];
    }
    this->print_and_log("(%s) -> Final CRC: %02X\n", client.gateway_id, crc);
    return crc;
}

void Fuota::init_crc16_tab(bool &crc_tab16_init, unsigned short int *crc_tab16)
{
    unsigned short int i, j, c, k;

    for (i = 0; i < 256; i++)
    {
        k = 0;
        c = i;
        for (j = 0; j < 8; j++)
        {

            if ((k ^ c) & 0x0001)
                k = (k >> 1) ^ 0xA001;
            else
                k = k >> 1;

            c = c >> 1;
        }

        crc_tab16[i] = k;
    }

    crc_tab16_init = true;

} /* init_crc16_tab */

void Fuota::ensure_crc_table()
{
    static bool crc_tab16_init = false;

    if (!crc_tab16_init)
    {
        init_crc16_tab(crc_tab16_init, crc_tab16);
        print_and_log("[CRC] CRC16 table initialized\n");
    }
}

void Fuota::calculate_crc_for_target_node()
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    ensure_crc_table();
    /* ---------- REUSE CRC IF AVAILABLE ---------- */
    if (this->rf_fuota_crc_calculated)
    {
        this->CRC = this->rf_fuota_crc_value;
        this->print_and_log("[CRC] Reusing cached CRC = 0x%04X\n", this->CRC);
        return;
    }

    /* ---------- VALIDATION ---------- */
    if (!this->fouta_read_fd || this->total_firmwaresize == 0)
    {
        this->print_and_log("[CRC][ERROR] Invalid file or size\n");
        return;
    }

    fseek(this->fouta_read_fd, 0, SEEK_SET);

    uint32_t remaining = this->total_firmwaresize;
    uint8_t buffer[this->sector_value];

    this->print_and_log("[CRC] Start FW size=%u sector=%u\n", remaining, this->sector_value);

    while (remaining > 0)
    {
        uint32_t chunk = (remaining > this->sector_value) ? this->sector_value : remaining;

        size_t readBytes = fread(buffer, 1, chunk, this->fouta_read_fd);

        if (readBytes == 0)
        {
            this->print_and_log("[CRC][ERROR] fread failed: %s\n", strerror(errno));
            return;
        }

        crc = crc16_update(buffer, readBytes, crc);
        this->print_and_log("[CRC][DBG] chunk=%u crc=0x%04X\n", readBytes, crc);
        remaining -= readBytes;
    }

    /* ---------- STORE FOR RETRY / RESUME ---------- */
    this->CRC = crc;
    this->rf_fuota_crc_value = crc;
    this->rf_fuota_crc_calculated = true;

    this->print_and_log("[CRC][SUCCESS] RF FUOTA CRC = 0x%04X\n", this->CRC);
}

uint8_t Fuota::update_fuota_response_buff()
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    int index = 0;

    // Copy the fixed header (first 11 bytes)
    memcpy(this->rf_fuota_expected_resp_header, this->client_tx_buffer, 11);
    index = 11;

    // Read hop count from byte 12
    unsigned char hop_count = this->client_tx_buffer[12];

    // Copy the appropriate MAC
    if (hop_count == 0)
    {
        // Direct path — copy 4 bytes from offset 13
        memcpy(&this->rf_fuota_expected_resp_header[index], &this->client_tx_buffer[13], 4);
        index += 4;
    }
    else
    {
        // Multi-hop — copy last hop's 4 bytes
        int last_hop_offset = 13 + (hop_count - 1) * 4;
        memcpy(&this->rf_fuota_expected_resp_header[index], &this->client_tx_buffer[last_hop_offset], 4);
        index += 4;
    }

    this->print_and_log("(%s) -> -> Response data before append payload: ", client.gateway_id);
    for (int i = 0; i < index; i++)
        print_and_log("%02X", this->rf_fuota_expected_resp_header[i]);
    print_and_log("\n");

    return index;
}

uint8_t Fuota::update_fuota_image_transfer_tx_buff()
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    int index = 0;
    memcpy((unsigned char *)&this->image_tf_command[index], this->client_tx_buffer, 13);
    index += 13;
    int dest_len = this->client_tx_buffer[12] * 4;

    if (dest_len == 0)
    {
        memcpy((unsigned char *)&this->image_tf_command[index], &this->client_tx_buffer[13], 4);
        index += 4;
    }
    else
    {
        memcpy((unsigned char *)&this->image_tf_command[index], &this->client_tx_buffer[13], this->client_tx_buffer[12] * 4);
        index += dest_len;
    }

    return index;
}

void Fuota::print_data_in_hex(unsigned char *buffer, unsigned int length)
{
    unsigned char index;
    printf("%d : ", length);
    for (index = 0; index < length; index++)
        printf("%02X ", buffer[index]);
    printf("\n\n");
}

unsigned char Fuota::calculate_checksum(unsigned char input_buff[], unsigned short buff_len)
{
    // this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    unsigned short check_sum_value = 0;

    for (unsigned short i = 0; i < buff_len; i++)
    {
        check_sum_value += input_buff[i];
    }
    return (unsigned char)check_sum_value;
}

/*
 *Reading of last sector/sub page status basedon that response frmae next page command
 */
void Fuota::fix_page_subpage_and_seek()
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    int max_sub = this->get_max_sub_page_count(this->FUOTA_Cmd_Header_Len);

    cout << "(" << client.gateway_id << ") -> max_sub: " << max_sub << ", subpage count: " << (int)subpage_count << endl;

    if (subpage_count >= max_sub)
    {
        subpage_count = 0;
        page_count++;
    }
    else
    {
        cout << "(" << client.gateway_id << ") -> subpage_count: " << (int)subpage_count << ", page: " << page_count << endl;
    }

    long sector = this->sector_value;
    long payload = this->get_max_payload_size(this->FUOTA_Cmd_Header_Len);
    long long offset = (long long)page_count * sector + (long long)subpage_count * payload;

    if (this->fouta_read_fd)
    {
        if (fseeko(this->fouta_read_fd, offset, SEEK_SET) != 0)
        {
            cout << "(" << client.gateway_id << ") -> fseeko failed: " << strerror(errno) << " offset: " << offset << endl;
            // fallback: abort or continue with previous file pointer depending on your policy
        }
        else
        {
            cout << "(" << client.gateway_id << ") -> Gateway Seeked firmware file to offset: " << offset << " (page " << page_count << ", subpage " << (int)subpage_count << ")\n";
        }
    }
    else
    {
        cout << "(" << client.gateway_id << ") = Firmware filepointer null while trying to adjust after mismatch\n";
    }

    this->image_tf_command[FUOTA_Cmd_Header_Len + 4] = subpage_count;
    this->image_tf_command[FUOTA_Cmd_Header_Len + 5] = (page_count >> 8);
    this->image_tf_command[FUOTA_Cmd_Header_Len + 6] = (page_count & 0xFF);

    this->rf_fuota_expected_resp_header[FUOTA_Resp_Header_Len + 4] = subpage_count;
    this->rf_fuota_expected_resp_header[FUOTA_Resp_Header_Len + 5] = (page_count >> 8);
    this->rf_fuota_expected_resp_header[FUOTA_Resp_Header_Len + 6] = (page_count & 0xFF);

    memcpy(rf_fuota_expected_resp_header, temp_expect_buf, response_length);
}

/*
 * In the state of Fuota read failed exit from fuota state with updation of respective database statuses
 * While in the command in progress and timeout retry
 * During the page/subpage mismatch state read the last sector/subpage status and take appropriate action
 */
int Fuota::handle_fuota_post_read(int bytes_read)
{
    this->print_and_log("(%s) -> Function:%s\n", client.gateway_id, __FUNCTION__);

    /* ================= SUCCESS ================= */
    if (memcmp(this->fuota_rbuf, this->rf_fuota_expected_resp_header, this->response_length) == 0)
    {
        print_and_log("[RX] Response matched\n");
        cntx.alternate_retry = 0;
        this->fuota_imagetf_retry_count = 0;
        this->fuotaimagetf_alternate_count = 0;
        this->using_alternate_path = false;

        return FuotaStatus::FUOTA_SUCCESS;
    }

    /* ================= READ FAILURE ================= */
    if (bytes_read <= 0)
    {
        this->print_and_log("(%s) -> Read Failed: %s\n", client.gateway_id, strerror(errno));

        // try alternate path
        this->fuota_imagetf_retry_count++;
        this->ondemand_fuota_update_status(4, 0, client.request_id);

        return (this->fuota_imagetf_retry_count >= this->MAX_FUOTA_RETRIES) ? FuotaStatus::FUOTA_ABORT : FuotaStatus::FUOTA_RETRY;
    }

    /* ================= ERROR CLASSIFICATION ================= */
    bool timeout = (this->fuota_rbuf[2] == 0x09 &&
                    this->fuota_rbuf[this->response_length - 1] == 0x06);

    bool cmd_progress = (this->fuota_rbuf[2] == 0x09 &&
                         this->fuota_rbuf[this->response_length - 1] == 0x07);

    bool subpage_mismatch = (this->fuota_rbuf[2] == 0x0A &&
                             this->fuota_rbuf[this->no_of_bytes_read_serial - 2] == 0x02);

    bool page_mismatch = (this->fuota_rbuf[2] == 0x0A &&
                          this->fuota_rbuf[this->no_of_bytes_read_serial - 2] == 0x03);

    /* ================= JUMP CASES (NO RETRY COUNT) ================= */
    if (page_mismatch)
    {
        this->print_and_log("(%s) -> Page Mis-matched:%s\n", client.gateway_id, page_mismatch);
        LastSectorStatus st = this->rffuota_read_lastsector_page_and_subpage_status();

        if (!st.valid)
            return FuotaStatus::FUOTA_RETRY;

        page_count = st.page;
        subpage_count = page_mismatch ? st.subpage + 1 : st.subpage;

        this->fix_page_subpage_and_seek();

        this->fuota_imagetf_retry_count = 0;
        this->fuotaimagetf_alternate_count = 0;

        return FuotaStatus::FUOTA_JUMP;
    }

    if (subpage_mismatch)
    {
        this->print_and_log("(%s) -> Subpage Mis-matched:%d\n", client.gateway_id, subpage_mismatch);
        LastSectorStatus st = this->rffuota_read_lastsector_page_and_subpage_status();

        if (!st.valid)
            return FuotaStatus::FUOTA_RETRY;

        page_count = st.page;
        subpage_count = st.subpage;

        this->fix_page_subpage_and_seek();

        this->fuota_imagetf_retry_count = 0;
        this->fuotaimagetf_alternate_count = 0;

        return FuotaStatus::FUOTA_JUMP;
    }

    /* ================= RETRY HANDLING ================= */

    /* -------- PRIMARY PATH -------- */
    if (!this->using_alternate_path)
    {
        this->fuota_imagetf_retry_count++;

        this->print_and_log("(%s) -> Primary path retry %d/3 (timeout=%d, progress=%d)\n", client.gateway_id, this->fuota_imagetf_retry_count, timeout, cmd_progress);

        if (this->fuota_imagetf_retry_count < 3)
        {
            memcpy(this->rf_fuota_expected_resp_header, this->temp_expect_buf, this->response_length);

            return FuotaStatus::FUOTA_RETRY;
        }

        /* Switch to alternate */
        this->print_and_log("(%s) -> Primary path exhausted → switching to alternate path\n", client.gateway_id);

        this->using_alternate_path = true;
        this->fuotaimagetf_alternate_count = 0;

        this->prepare_alternate_sameroute_retry(&this->image_tf_command[BASE_HDR_LEN], this->image_tf_command[BASE_HDR_LEN + 1]);

        return FuotaStatus::FUOTA_RETRY;
    }

    /* -------- ALTERNATE PATH -------- */
    this->fuotaimagetf_alternate_count++;

    this->print_and_log("(%s) -> Alternate path retry %d/3\n", client.gateway_id, this->fuotaimagetf_alternate_count);

    if (this->fuotaimagetf_alternate_count < 3)
    {
        memcpy(this->rf_fuota_expected_resp_header, this->temp_expect_buf, this->response_length);

        return FuotaStatus::FUOTA_RETRY;
    }

    /* ================= FINAL ABORT ================= */
    this->print_and_log("(%s) -> Alternate path exhausted → FUOTA ABORT\n", client.gateway_id);

    return FuotaStatus::FUOTA_ABORT;
}

/*
 * Fetch and Transfering the file content to the Target device
 * Based on the responses move forward
 */
unsigned int Fuota::fuota_uploading_process()
{
    this->print_and_log("\n========== RF FUOTA IMAGE UPLOAD START ==========\n");

    char txd_time[26]{}, rxd_time[26]{};
    int millisec = 0;

    this->page_count = 0;
    this->subpage_count = 0;
    this->fuota_imagetf_retry_count = 0;

    /* ---------- BUILD RX HEADER TEMPLATE ---------- */
    this->FUOTA_Resp_Header_Len = update_fuota_response_buff();

    /* ---------- BUILD TX HEADER TEMPLATE ---------- */
    this->FUOTA_Cmd_Header_Len = update_fuota_image_transfer_tx_buff();
    this->BASE_HDR_LEN = this->FUOTA_Cmd_Header_Len;
    millisec = client_get_time(rxd_time);

    this->print_and_log("[HDR] TX Header Len=%d RX Header Len=%d\n", BASE_HDR_LEN, FUOTA_Resp_Header_Len);
    this->print_and_log("Expected Resp: (%s.%03d) ", rxd_time, millisec);

    for (int i = 0; i < this->rf_fuota_expected_resp_header[1] + 1; i++)
        this->print_and_log("%02X", this->rf_fuota_expected_resp_header[i]);

    this->print_and_log("\n");
    memcpy(&this->image_tf_command[BASE_HDR_LEN], this->image_transfer, sizeof(this->image_transfer));

    this->image_tf_command[1] += sizeof(this->image_transfer);

    fseek(this->fouta_read_fd, 0, SEEK_SET);

    /* ========================================================= */
    /* ===================== MAIN LOOP ========================= */
    /* ========================================================= */
    while (page_count < get_max_page_count(this->total_firmwaresize))
    {
        /* ---------- RESET TX LENGTH ---------- */
        this->image_tf_command[0] = 0x2E;
        this->image_tf_command[1] = BASE_HDR_LEN - 1;

        /* ---------- FILE READ ---------- */
        bool last_subpage = (subpage_count == get_max_sub_page_count(BASE_HDR_LEN) - 1);
        // std::cout << "bool subpage count: " << subpage_count << ",last_subpage = " << last_subpage << endl;

        payload_len = fread(ffile_rxdata, 1, last_subpage ? get_min_payload_size(BASE_HDR_LEN) : get_max_payload_size(BASE_HDR_LEN), fouta_read_fd);

        if (payload_len == 0)
        {
            print_and_log("[FILE] EOF reached\n");
            break;
        }

        print_and_log("[FILE] Read %d bytes (Page = %d Sub = %d)\n", payload_len, page_count, subpage_count);

        /* ---------- SET PAGE / SUBPAGE ---------- */
        this->image_tf_command[BASE_HDR_LEN + 4] = subpage_count;
        this->image_tf_command[BASE_HDR_LEN + 6] = page_count;
        this->image_tf_command[BASE_HDR_LEN + 5] = page_count >> 8;

        /* ---------- APPEND PAYLOAD ---------- */
        memcpy(&this->image_tf_command[BASE_HDR_LEN + 7], ffile_rxdata, payload_len);
        this->image_tf_command[1] += payload_len;
        this->image_tf_command[BASE_HDR_LEN + 1] = (payload_len + 7);

        /* ---------- CHECKSUM TX ---------- */
        this->image_tf_command[BASE_HDR_LEN + 7 + payload_len] = this->calculate_checksum(&this->image_tf_command[BASE_HDR_LEN], this->image_tf_command[BASE_HDR_LEN + 1]);

        uint16_t total_len =
            2 +                  // start + len
            (BASE_HDR_LEN - 1) + // actual header
            7 +                  // image tf command
            payload_len +        // payload
            1;                   // checksum

        // print_and_log("this->image_tf_command[1] = %02X,total_len = %02X\n", this->image_tf_command[1], total_len);
        this->image_tf_command[1] = total_len - 2; // LEN excludes start+len
        this->command_length = total_len - 1;      // exclude checksum byte

        print_and_log("[CHK] TX checksum = %02X len = %d\n", this->image_tf_command[command_length - 1], command_length);

        /* ---------- BUILD EXPECTED RX ---------- */
        uint16_t exp_rx_len = this->build_image_tf_expected_rx(this->rf_fuota_expected_resp_header, FUOTA_Resp_Header_Len, page_count, subpage_count, last_subpage);

        if (exp_rx_len == 0)
        {
            print_and_log("Expected response len invalid\n");
        }
        this->print_and_log("(%s) -> Expected Response for Node: ", client.gateway_id);

        for (int i = 0; i < exp_rx_len; i++)
            this->print_and_log("%02X ", this->rf_fuota_expected_resp_header[i]);

        this->print_and_log("\n");

        /* ================================================= */
        /* ================= SEND + RETRY ================== */
        /* ================================================= */
        this->fuota_imagetf_retry_count = 0;
        this->ondemand_fuota_update_status(11, 1, client.request_id);
        while (this->fuota_imagetf_retry_count < MAX_FUOTA_RETRIES)
        {
            millisec = this->client_get_time(txd_time);

            print_and_log("TX(%s)->(%s.%03d) P = %d S = %d LEN = %d RETRY = %d\n", client.gateway_id, txd_time, millisec, page_count, subpage_count, command_length, this->fuota_imagetf_retry_count);

            this->print_and_log("TX(%s) -> [%s.%03d] = ", client.gateway_id, txd_time, millisec);

            for (int i = 0; i < command_length; i++)
                this->print_and_log("%02X ", this->image_tf_command[i]);

            this->print_and_log("\n");

            int write_ret = write(client.get_client_socket(), this->image_tf_command, command_length);
            if (write_ret < 0)
            {
                this->print_and_log("WriteFailed:%d\n", (unsigned char *)strerror(errno));
                this->ondemand_fuota_update_status(1, 0, client.request_id);
                return e_failure;
            }
            struct pollfd pfd{client.get_client_socket(), POLLIN, 0};
            int poll_ret = poll(&pfd, 1, 12000);

            if (poll_ret == 0)
            {
                print_and_log("[RETRY] Timeout\n");
                this->fuota_imagetf_retry_count++;
                continue;
            }

            no_of_bytes_read_serial = read(client.get_client_socket(), this->fuota_rbuf, sizeof(this->fuota_rbuf));

            if (no_of_bytes_read_serial <= 0)
            {
                print_and_log("[RETRY] Read error\n");
                this->fuota_imagetf_retry_count++;
                continue;
            }

            millisec = this->client_get_time(rxd_time);
            print_and_log("[RX] (%s.%03d) LEN = %d\n", rxd_time, millisec, no_of_bytes_read_serial);
            this->print_and_log("RX(%s -> %s.%03d) = ", client.gateway_id, rxd_time, millisec);

            for (unsigned int i = 0; i < no_of_bytes_read_serial; i++)
                this->print_and_log("%02X ", this->fuota_rbuf[i]);

            this->print_and_log("\n");
            response_length = this->rf_fuota_expected_resp_header[1] + 1;

            /*
             * Going for response validation & retry
             */

            int decision = this->handle_fuota_post_read(no_of_bytes_read_serial);

            if (decision == FuotaStatus::FUOTA_SUCCESS)
                break;
            if (decision == FuotaStatus::FUOTA_RETRY)
                continue;

            if (decision == FuotaStatus::FUOTA_JUMP)
                break; // page/subpage already fixed

            if (decision == FuotaStatus::FUOTA_ABORT)
            {
                this->ondemand_fuota_update_status(11, 0, client.request_id);
                this->handle_fuota_completion();
                return e_success_1;
            }
        }

        if (this->fuota_imagetf_retry_count == MAX_FUOTA_RETRIES)
        {
            print_and_log("[ABORT] Max retries reached\n");
            using_alternate_path = true;
            fuotaimagetf_alternate_count = cntx.alternate_retry;
            if (fuotaimagetf_alternate_count < 3)
            {
                this->handle_fuota_completion();
                return e_success_1;
            }
        }

        /* ---------- ADVANCE PAGE / SUBPAGE ---------- */
        if (!last_subpage)
            subpage_count++;
        else
        {
            subpage_count = 0;
            page_count++;
        }

        print_and_log("[PAGE] Next Page = %d Sub = %d\n", page_count, subpage_count);
    }

    print_and_log("\n========== FUOTA IMAGE UPLOAD COMPLETED ==========\n");
    print_and_log("(%s) -> Fuota next state: ", client.gateway_id, this->ondemand_fuota_state);
    if (this->ondemand_fuota_state == FUOTA_STATE::FW_IMAGE_TRANSFER)
    {
        this->ondemand_fuota_state = FUOTA_STATE::FW_IMAGE_ENDOF_PAGE_TRANSFER;
    }
    // return e_success_1;

    return this->prepare_and_get_the_endof_image_transfer();
}

uint16_t Fuota::build_image_tf_expected_rx(uint8_t *exp, uint16_t base_hdr_len, uint16_t page, uint8_t subpage, bool last_subpage)
{
    this->print_and_log("Function: %s\n", __FUNCTION__);
    uint16_t idx = 0;

    /* ---------- Mesh header (NO checksum) ---------- */
    exp[idx++] = 0x2E;
    exp[idx++] = 0x00; // LEN placeholder
    exp[idx++] = 0x0A; // Response type

    memcpy(&exp[idx], this->rf_fuota_expected_resp_header + 3, base_hdr_len - 3);
    idx += (base_hdr_len - 3);

    printf("index: %02X, exp[idx]: %02X\n", idx, exp[idx]);
    uint16_t dlms_start = idx; // <-- CRITICAL

    /* ---------- DLMS Image TF response ---------- */
    exp[idx++] = 0x2D;
    exp[idx++] = 0x08;
    exp[idx++] = 0x07;
    exp[idx++] = 0x01;
    exp[idx++] = subpage;
    exp[idx++] = page >> 8;
    exp[idx++] = page & 0xFF;
    printf("page: %02X, subpage: %02X, last_subpage:%02X\n", page, subpage, last_subpage);
    exp[idx++] = last_subpage ? 0x00 : 0x01;
    printf("page: %02X, subpage: %02X, last_subpage:%02X\n", page, subpage, last_subpage);

    /* ---------- Final length ---------- */
    exp[1] = (idx - 2) + 1;

    /* ---------- CHECKSUM ONLY OVER DLMS ---------- */
    {
        uint16_t len_for_checksum = static_cast<uint16_t>(idx - dlms_start);
        exp[idx++] = calculate_checksum(&exp[dlms_start], len_for_checksum);
    }
    exp[1] = 0x17;
    return idx;
}

#if defined(__FUOTA_CPP__)

/**
 * Fetch meter list from existing DB API which populates a pan_list_ptr with
gate_node->meter_list(vector<meter_vital_info *>)
*We copy entries into a vector<meter_vital_info> and free original pointers and
*the temporary pan_list_ptr to avoid leaks.
*/

bool Fuota::fetch_and_copy_meter_list_from_db(std::vector<meter_vital_info> &out_meters)
{
    this->print_and_log("Function: %s\n", __FUNCTION__);
    // Create temporary Fuota to getr DB API
    Fuota *pan_list_ptr = this;
    if (!pan_list_ptr)
    {
        this->print_and_log("fetch_and_copy_meter_list_from_db: new Fuota failed\n");
        return false;
    }

    // call existing DB function that populates pan_list_ptr->gate_node->meter_list
    if (this->db.get_the_gateway_undernodes_details_for_fuota((char *)client_query_buffer,
                                                              (unsigned char *)client.gateway_id, pan_list_ptr) != e_success_0)
    {
        this->print_and_log("DB call failed inside fetch_and_copy_meter_list_from_db\n");
        // cleanup pan_list_ptr if it contains allocated meter_list
        if (pan_list_ptr->gate_node && pan_list_ptr->gate_node->meter_list)
        {
            for (auto p : *(pan_list_ptr->gate_node->meter_list))
            {
                delete p;
            }
            delete pan_list_ptr->gate_node->meter_list;
            pan_list_ptr->gate_node->meter_list = nullptr;
        }
        delete pan_list_ptr;
        return false;
    }

    // If DB filled the meter_list, copy them into out_meters and free originals
    if (!pan_list_ptr->gate_node || !pan_list_ptr->gate_node->meter_list)
    {
        this->print_and_log("No meter list returned by DB for DCU: %s\n", client.gateway_id);
        delete pan_list_ptr;
        return false;
    }

    for (auto pnode : *(pan_list_ptr->gate_node->meter_list))
    {
        if (!pnode)
            continue;

        // Copy struct contents to local vector (value copy)
        meter_vital_info local;
        memset(&local, 0, sizeof(local));
        memcpy(&local, pnode, sizeof(meter_vital_info)); // shallow copy; ensure struct has POD fields
        out_meters.push_back(local);

        //  original pointer
        delete pnode;
    }

    this->print_and_log("Fetched and copied %zu meter entries from DB\n", out_meters.size());
    return true;
}

/**
 * Build gateway_route_info from vector<meter_vital_info>.
 * Allocates route_entry objects, stores pointers in this->gateway_route_info.
 * Caller must call freegateway_route_info() to release memory.
 */
bool Fuota::build_gateway_route_info_from_meters(const std::vector<meter_vital_info> &meters)
{
    // free previous if any
    // if (this->gateway_route_info)
    // {
    //     this->free_gateway_route_info();
    // }
    this->print_and_log("Function: %s\n", __FUNCTION__);
    this->gateway_route_info = new std::vector<route_entry *>();
    if (!this->gateway_route_info)
    {
        this->print_and_log("Failed to allocate gateway_route_info vector\n");
        return false;
    }

    for (const auto &m : meters)
    {
        route_entry *entry = new (std::nothrow) route_entry();
        if (!entry)
        {
            this->print_and_log("Failed to allocate route_entry for meter\n");
            // cleanup already created entries
            this->free_gateway_route_info();
            return false;
        }
        memset(entry, 0, sizeof(route_entry));
        entry->hop_count = m.hop_count;
        memcpy(entry->target_mac, m.meter_mac_address, 8);
        // copy route path up to hop_count*4 bytes, ensure bounds
        if (m.hop_count > 0)
        {
            int bytes = m.hop_count * 4;
            if (bytes > (int)sizeof(entry->route_path))
                bytes = sizeof(entry->route_path);
            memcpy(entry->route_path, m.route_path, bytes);
        }
        this->gateway_route_info->push_back(entry);
    }

    this->print_and_log("Built gateway_route_info with %zu entries\n", this->gateway_route_info->size());
    return true;
}

/**
 * Free the gateway_route_info vector and its route_entry allocations.
 */
void Fuota::free_gateway_route_info()
{
    this->print_and_log("Function: %s\n", __FUNCTION__);
    if (!this->gateway_route_info)
        return;
    for (auto e : *(this->gateway_route_info))
    {
        delete e;
    }
    delete this->gateway_route_info;
    this->gateway_route_info = nullptr;
}

/**
 * Identify leaf nodes (not referenced as a hop by any other node).
 * Input: meters (value copies). Return: vector of indices into meters that are leafs.
 */
std::vector<size_t> Fuota::detect_leaf_nodes(const std::vector<meter_vital_info> &meters)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    std::vector<size_t> leaf_indices;
    const size_t n = meters.size();

    for (size_t i = 0; i < n; ++i)
    {
        const auto &node = meters[i];
        bool is_used_as_hop = false;

        unsigned char *node_last4 = (unsigned char *)node.meter_mac_address + 4;

        for (size_t j = 0; j < n; ++j)
        {
            if (i == j)
                continue;
            const auto &other = meters[j];
            int route_len = other.hop_count * 4;
            for (int k = 0; k < route_len; k += 4)
            {
                if (memcmp(node_last4, other.route_path + k, 4) == 0)
                {
                    is_used_as_hop = true;
                    break;
                }
            }
            if (is_used_as_hop)
                break;
        }

        if (!is_used_as_hop)
        {
            leaf_indices.push_back(i);
        }
    }

    this->print_and_log("Detected %zu leaf nodes\n", leaf_indices.size());
    return leaf_indices;
}

/**
 * Filter leaf indices to exclude the on-demand (target) MAC.
 */
std::vector<size_t> Fuota::filter_out_on_demand(const std::vector<size_t> &leaf_indices, const std::vector<meter_vital_info> &meters, const unsigned char *ondemand_mac)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    std::vector<size_t> filtered;
    for (auto idx : leaf_indices)
    {
        if (memcmp(meters[idx].meter_mac_address, ondemand_mac, 8) != 0)
        {
            filtered.push_back(idx);
        }
        else
        {
            this->print_and_log("Skipping on-demand node from leaf list: %s\n", Utility::mac_to_string(ondemand_mac).c_str());
        }
    }
    this->print_and_log("Filtered leaf nodes count (excluding on-demand): %zu\n", filtered.size());
    return filtered;
}

bool Fuota::compute_router_path(unsigned char *target_mac, router_path_t *out_path)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    // Fuota *pan_list_ptr;
    if (client.gateway_id == NULL)
    {
        std::cout << "client.gateway_id empty!" << endl;
    }
    // out_path->hop_count = 0;//commented on 30122025

    if (!this->gateway_route_info || this->gateway_route_info->empty())
    {
        std::cout << "(" << client.gateway_id << ") = gateway_route_info is empty!" << std::endl;
        return false;
    }

    std::cout << "(" << client.gateway_id << ") = gateway_route_info size: " << this->gateway_route_info->size() << std::endl;
    for (auto &entry : *(this->gateway_route_info))
    {
        std::cout << "Route to MAC: ";
        for (int i = 0; i < 8; ++i)
            printf("%02X ", entry->target_mac[i]);
        std::cout << " | Hop Count: " << (int)entry->hop_count << std::endl;
    }

    for (auto &entry : *(this->gateway_route_info))
    {
        printf("Checking route entry: ");
        for (int i = 0; i < 8; ++i)
            printf("%02X ", entry->target_mac[i]);
        printf("\t");

        printf("Comparing to target_mac: ");
        for (int i = 0; i < 8; ++i)
            printf("%02X ", target_mac[i]);
        printf("\n\n");

        // Try full 8-byte comparison (if entry.target_mac is 8 bytes)
        if (memcmp(entry->target_mac, target_mac, 8) == 0)
        {
            std::cout << "Comparision done try to silence the node" << (int)entry->hop_count << endl;
            // int hop_bytes = entry->hop_count * 4;
            for (int i = 0; i < entry->hop_count; i++)
            {
                memcpy(out_path->paths[i], &entry->route_path[i * 4], 4);
            }
            std::cout << "Selected path out: ";
            for (int i = 0; i < 8; i++)
            {
                printf("%s", out_path->paths[i]);
            }
            cout << endl;
            out_path->hop_count = entry->hop_count;
            return true;
        }
    }

    std::cout << "(" << client.gateway_id << ") = Route not found for MAC: ";
    for (int i = 0; i < 8; ++i)
        printf("%02X ", target_mac[i]);
    std::cout << std::endl;

    return false;
}

/**
 * Per-leaf operation:
 *  - set current target mac
 *  - compute router path into current_fuota_route
 *  - set use_alternate_fuota_route, call execute_fuotasequence()
 * Returns true if FUOTA ENABLE succeeded for this leaf.
 */
bool Fuota::send_fuota_enable_to_leaf(const meter_vital_info &leaf)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    // copy mac into current target
    memcpy(this->current_fuota_target_mac, leaf.meter_mac_address, 8);

    // compute router path (uses this->gateway_route_info internally)
    router_path_t tmp_route;
    memset(&tmp_route, 0, sizeof(tmp_route));

    if (!this->compute_router_path(this->current_fuota_target_mac, &tmp_route))
    {
        this->print_and_log("Route not found for leaf node: %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
        return false;
    }

    // copy computed route into current_fuota_route for downstream functions that expect it
    memset(this->current_fuota_route, 0, sizeof(*((router_path_t *)this->current_fuota_route))); // ensure zeroing
    for (int i = 0; i < tmp_route.hop_count && i < MAX_ROUTE_HOPS; ++i)
    {
        memcpy(((router_path_t *)this->current_fuota_route)->paths[i], tmp_route.paths[i], 4);
    }
    ((router_path_t *)this->current_fuota_route)->hop_count = tmp_route.hop_count;

    // enable alternate route usage for leaf execution (preserves your previous behaviour)
    this->use_alternate_fuota_route = true;

    bool success = false;
    int attempt_ret = this->execute_fuotasequence_to_silence_network(); // existing function handles the enable sequence
    success = (attempt_ret == e_success_1);

    if (!success)
    {
        this->print_and_log("FUOTA ENABLE failed for leaf node: %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
    }
    else
    {
        this->print_and_log("FUOTA ENABLE succeeded for leaf node: %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
    }

    // restore flag
    this->use_alternate_fuota_route = false;
    return success;
}

// ---------- Refactored sequence function --------------- //

int Fuota::process_rffuota_enable_sequence_refactored(unsigned char *ondemand_mac)
{
    std::string readable_mac = Utility::mac_to_string(ondemand_mac);
    print_and_log("\n(%s) -> cclient: MultiHop RF FUOTA ENABLE (refactored)\n", client.gateway_id);
    print_and_log("On-demand FUOTA TargetMAC For Silence: %s\n", readable_mac.c_str());

    // 1) Fetch and copy meters from DB
    std::vector<meter_vital_info> meters;
    if (!this->fetch_and_copy_meter_list_from_db(meters))
    {
        this->print_and_log("Failed to fetch meter list from DB\n");
        return e_failure;
    }
    if (meters.empty())
    {
        this->print_and_log("PAN not found or meter list is empty for DCU: %s\n", client.gateway_id);
        return e_failure;
    }

    // 2) Build gateway route info (allocates this->gateway_route_info)
    if (!this->build_gateway_route_info_from_meters(meters))
    {
        this->print_and_log("Failed to build gateway_route_info\n");
        return e_failure;
    }

    // 3) Detect leaf nodes
    auto leaf_indices = this->detect_leaf_nodes(meters);

    // 4) Filter out on-demand target MAC (don't treat it as leaf)
    auto filtered_leaf_indices = this->filter_out_on_demand(leaf_indices, meters, ondemand_mac);

    // 5) Send FUOTA ENABLE to each filtered leaf
    for (auto idx : filtered_leaf_indices)
    {
        const auto &leaf = meters[idx];

        // attempt per-leaf operation (this will compute route and call execute_fuotasequence)
        bool res = this->send_fuota_enable_to_leaf(leaf);

        // On success, you can optionally update silenced_nodes_for_fuota table here:
        if (res)
        {
            // Insert into DB table silenced_nodes_for_fuota: meter_serial_number,gateway_id,meter_mac_address,hop_count

            this->db.insert_update_fuota_Unsilenced_meter_details_in_db((char *)client_query_buffer, (unsigned char *)leaf.meter_mac_address, /*leaf.meter_serial_number,*/ (unsigned char *)client.gateway_id, 1, leaf.hop_count);

            this->print_and_log("Update silenced_nodes_for_fuota for %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
        }
        else
        {
            this->print_and_log("Leaf FUOTA enable failed for %s (will continue with next leaf)", Utility::mac_to_string(leaf.meter_mac_address).c_str());

            this->db.insert_update_fuota_Unsilenced_meter_details_in_db((char *)client_query_buffer, (unsigned char *)leaf.meter_mac_address, (unsigned char *)client.gateway_id, 0, leaf.hop_count);
        }
    }
    print_and_log("\n(%s) = RF FUOTA enable sequence (refactored) completed successfully\n", client.gateway_id);

    // 6) After finishing leaf enabling, prepare to transfer image to on-demand node
    print_and_log("\n Starting FUOTA imagetransfer state of on-demand node: %s\n", readable_mac.c_str());
    this->ondemand_fuota_state = FUOTA_STATE::SECTOR_READ;
    cansend_fuota_next_command(); // added on 05012026 for immediate start
    // 7) Clean up gateway_route_info memory
    // this->free_gateway_route_info();

    return e_success_1;
}

/*
 * Perform the Operations basedon the best routing path such as primary fails go for via alternate path of same hop count
 */

int Fuota::perform_fuota_sequence_with_paths(unsigned char *target_mac)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    if (!target_mac)
    {
        this->print_and_log("perform_fuotasequence_with_paths: invalid arguments\n");
        return e_failure;
    }

    this->print_and_log("Starting FUOTA sequence (primary path) for %s\n", Utility::mac_to_string(target_mac).c_str());

    // 1) Build primary route and attempt
    if (this->build_primary_route(target_mac))
    {
        int rc = this->execute_fuotasequence_to_silence_network();
        if (rc == e_success_1)
        {
            this->print_and_log("FUOTA succeeded on PRIMARY path for %s\n", Utility::mac_to_string(target_mac).c_str());
            return e_success_1;
        }
        else
        {
            this->print_and_log("PRIMARY path attempt failed for %s (rc = %d)\n", Utility::mac_to_string(target_mac).c_str(), rc);
        }
    }
    else
    {
        this->print_and_log("Failed to build PRIMARY route for %s\n", Utility::mac_to_string(target_mac).c_str());
    }

    // 2) Try alternate path(s) of same hopcount fetched from DB
    this->print_and_log("Attempting ALTERNATE routes for %s\n", Utility::mac_to_string(target_mac).c_str());
    if (this->build_alternate_route(target_mac))
    {
        int rc_alt = this->execute_fuotasequence_to_silence_network();
        if (rc_alt == e_success_1)
        {
            this->print_and_log("FUOTA succeeded on ALTERNATE path for %s\n", Utility::mac_to_string(target_mac).c_str());
            return e_success_1;
        }
        else
        {
            this->print_and_log("ALTERNATE path attempt failed for %s (rc = %d)\n", Utility::mac_to_string(target_mac).c_str(), rc_alt);
        }
    }
    else
    {
        this->print_and_log("No ALTERNATE routes found for %s\n", Utility::mac_to_string(target_mac).c_str());
    }

    this->print_and_log("FUOTA failed on both PRIMARY and ALTERNATE paths for %s\n", Utility::mac_to_string(target_mac).c_str());
    return e_failure;
}

// ---------- buildprimaryroute ----------
bool Fuota::build_primary_route(unsigned char *target_mac)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    if (!target_mac)
        return false;

    // This function must populate gateway_route_info / current_fuota_route such that compute_routerpath() works.
    char qbuf[512] = {0};
    Fuota *tmp_pan = this; // used only to call DB function
    if (!tmp_pan)
        return false;

    int rc = this->db.get_the_gateway_undernodes_details_for_fuota(qbuf, (unsigned char *)client.gateway_id, tmp_pan);
    if (rc != e_success_0 && rc != 0)
    {
        std::cout << "DB call failed!" << endl;
        // DB call failed
        delete tmp_pan;
        return false;
    }

    // Build gateway_route_info from tmp_pan->gate_node->meter_list
    // We'll reuse existing logic by copying entries into `this->gateway_route_info`.
    //====================comment on 30122025==============
    // if (this->gateway_route_info)
    // {
    //     // free previous
    //     this->gateway_route_info->clear();
    //     delete this->gateway_route_info;
    //     this->gateway_route_info = nullptr;
    // }

    this->gateway_route_info = new std::vector<route_entry *>();

    if (!tmp_pan->gate_node || !tmp_pan->gate_node->meter_list || !this->gateway_route_info)
    {
        delete tmp_pan;
        return false;
    }

    for (auto &m_ptr : *(tmp_pan->gate_node->meter_list))
    {
        if (!m_ptr)
            continue;
        route_entry *entry = new (std::nothrow) route_entry();
        if (!entry)
            continue;
        memset(entry, 0, sizeof(route_entry));
        entry->hop_count = m_ptr->hop_count;
        memcpy(entry->target_mac, m_ptr->meter_mac_address, 8);
        int bytes = std::min((int)sizeof(entry->route_path), m_ptr->hop_count * 4);
        if (bytes > 0)
            memcpy(entry->route_path, m_ptr->route_path, bytes);
        this->gateway_route_info->push_back(entry);
    }

    // Clean up tmp_pan memory (as your DB allocator pattern used new)
    for (auto &m_ptr : *(tmp_pan->gate_node->meter_list))
    {
        delete m_ptr;
    }
    delete tmp_pan->gate_node->meter_list;
    tmp_pan->gate_node->meter_list = nullptr;
    delete tmp_pan;

    // Now compute route for target_mac into this->current_fuota_route using your compute_routerpath()
    if (!this->compute_router_path(target_mac, (router_path_t *)this->current_fuota_route))
    {
        // No route found
        this->print_and_log("build_primaryroute: computerouter_path failed for %s\n", Utility::mac_to_string(target_mac).c_str());
        return false;
    }

    // copy target mac to current target
    memcpy(this->current_fuota_target_mac, target_mac, 8);

    this->print_and_log("build_primaryroute: primary route built for %s\n", Utility::mac_to_string(target_mac).c_str());
    return true;
}

// ---------- build_alternateroute ----------
bool Fuota::build_alternate_route(unsigned char *target_mac)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    if (!target_mac)
    {
        this->print_and_log("target_mac is NULL!");
        return false;
    }

    char qbuf[1024] = {0};
    Fuota *pan_list_ptr = this;
    if (!pan_list_ptr)
    {
        this->print_and_log("Failed to allocate pmesh_pan!\n");
        return false;
    }

    int rc = this->db.get_alternate_source_route_network_from_db(qbuf, (unsigned char *)client.gateway_id, target_mac, pan_list_ptr);

    if (rc != SUCCESS && rc != e_success_0 && rc != 0)
    {
        this->print_and_log("DB alternate path fetch failed!\n");
        // delete pan_list_ptr;
        return false;
    }

    if (!pan_list_ptr->alternatepaths)
    {
        this->print_and_log("No alternate paths found!\n");
        // delete pan_list_ptr;
        return false;
    }
    else
    {

        // -----------------------------
        // Step 1: Print all alternate paths
        // -----------------------------
        cout << "Printing DB Alternate Paths " << endl;
        pan_list_ptr->print_alternate_paths();

        // -----------------------------
        // Step 2: Determine primary hop_count
        // -----------------------------
        int primary_hop_count = 0;

        if (this->gateway_route_info && !this->gateway_route_info->empty())
        {
            for (auto &entry : *gateway_route_info)
            {
                if (memcmp(entry->target_mac, target_mac, 8) == 0)
                {
                    primary_hop_count = entry->hop_count;
                    break;
                }
            }
        }

        cout << "Primary hop_count = " << primary_hop_count << endl;

        // -----------------------------
        // Step 3: Select alternate path
        // -----------------------------
        alternate_path_info *cur = pan_list_ptr->alternatepaths.get();
        alternate_path_info *chosen = nullptr;

        while (cur)
        {
            if ((int)cur->hop_count == primary_hop_count)
            {
                chosen = cur;
                break;
            }
            cur = cur->next.get();
        }

        // If none matched hop_count → pick first one
        if (!chosen)
            chosen = pan_list_ptr->alternatepaths.get();

        if (!chosen)
        {
            cout << "ERROR: No valid alternate path chosen!" << endl;
            // delete pan_list_ptr;
            return false;
        }

        // -----------------------------
        // Step 4: Copy selected alternate path into current_fuota_route
        // -----------------------------
        router_path_t *dst = (router_path_t *)this->current_fuota_route;
        memset(dst, 0, sizeof(router_path_t));

        size_t bytes_to_copy = chosen->hop_count * 4;
        if (bytes_to_copy > sizeof(dst->paths))
            bytes_to_copy = sizeof(dst->paths);

        memcpy(dst->paths, chosen->route_path, bytes_to_copy);

        dst->hop_count = chosen->hop_count;

        memcpy(this->current_fuota_target_mac, chosen->meter_mac_address, 8);

        // -----------------------------
        // Step 5: Debug print selected route
        // -----------------------------
        cout << "Chosen alternate path (hop_count = " << (int)dst->hop_count << "): ";
        for (int i = 0; i < dst->hop_count; i++)
        {
            printf("[%02X%02X%02X%02X] ", dst->paths[i][0], dst->paths[i][1], dst->paths[i][2], dst->paths[i][3]);
        }
        cout << endl;

        // delete pan_list_ptr;
        return true;
    }
}

int Fuota::write_fuota_sequnce_of_commands()
{
    this->print_and_log("Writing FUOTA Sequence of command to Silence Network client socket\n");

    memcpy(this->nodeinfo_tx, this->client_tx_buffer, this->client_tx_buffer[1] + 1);
    ssize_t sent = client.write_to_client(this->nodeinfo_tx, this->nodeinfo_tx[1] + 1);
    if (sent < 0)
    {
        this->print_and_log("Error sending FUOTA command: %s\n", strerror(errno));
        return e_failure;
    }
    return e_success_1;
}

// ---------- execute_fuotasequence (refined, uses wait_for_socket_response()) ----------
int Fuota::execute_fuotasequence_to_silence_network()
{
    printf("(%s) -> Execute FUOTA Sequence Started\n", client.gateway_id);

    // Sequence states (substates to be sent)
    std::vector<int> fuota_states{
        PATH_SILENCE_STATE::AT_FUOTA_ENABLE,
        PATH_SILENCE_STATE::AT_FUOTA_MODE_ENTRY,
        PATH_SILENCE_STATE::AT_ENABLE_FLASHSAVE,
        PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT};

    for (auto state : fuota_states)
    {
        this->network_silence_state = state;
        int retry_count = 0;
        bool success = false;
        std::cout << "Network silience of current state as " << this->network_silence_state << endl;
        while (retry_count < 3 && !success)
        {
            printf("(%s) -> Sending command for FUOTA state %d (Attempt %d)\n", client.gateway_id, state, retry_count + 1);

            if (this->build_and_store_fuota_cmd() == -1)
            {
                printf("(%s) -> Write failed for state %d, retrying...\n", client.gateway_id, state);
                retry_count++;
                // small backoff
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            // Wait for valid response
            if (!this->wait_for_socket_response(15))
            {
                printf("(%s) -> Timeout/invalid response for Silience state %d, retrying...\n", client.gateway_id, state);
                retry_count++;
                // small backoff before retrying
                // std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            printf("(%s) -> FUOTA step %d completed successfully\n", client.gateway_id, state);
            success = true;
        }

        if (!success)
        {
            printf("(%s) -> FUOTA step %d failed after 3 retries.\n", client.gateway_id, state);
            return e_failure;
        }
    }

    printf("(%s) -> Execute FUOTA Sequence Completed Successfully\n", client.gateway_id);
    return e_success_1;
}

// ---------- wait_for_socketresponse (robust frame handling) ----------
bool Fuota::wait_for_socket_response(int timeout_secs)
{
    int sd = client.get_client_socket();
    if (sd < 0)
        return false;

    this->print_and_log("(%s) -> Waiting for socket response (timeout %d seconds)\n", client.gateway_id, timeout_secs);

    const int RFMAX_RETRIES = 3;
    int rf_retry_count = 0;

    if (this->current_fuota_command == nullptr)
    {
        this->print_and_log("this->current_fuota_command null\n");
    }

    std::vector<unsigned char> recvbuf;
    recvbuf.reserve(2048);

    while (rf_retry_count < RFMAX_RETRIES)
    {
        fd_set read_fds;
        struct timeval timeout;
        FD_ZERO(&read_fds);
        FD_SET(sd, &read_fds);

        timeout.tv_sec = timeout_secs;
        timeout.tv_usec = 0;

        int sel = select(sd + 1, &read_fds, NULL, NULL, &timeout);
        if (sel < 0)
        {
            perror("select");
            return false;
        }
        else if (sel == 0)
        {
            std::string start_time;
            client.client_get_time(start_time, 1);
            // timeout - resend previous command if present
            rf_retry_count++;
            this->print_and_log("(%s) -> Socket select timeout (%d/%d). Re-sending previous command if present\n", client.gateway_id, rf_retry_count, RFMAX_RETRIES);

            if (this->client_tx_buffer)
            {
                ssize_t tosend = this->client_tx_buffer[1] + 1;
                if (tosend > 0)
                {
                    ssize_t s = write(sd, this->client_tx_buffer, tosend);
                    if (s < 0)
                    {
                        this->print_and_log("(%s) -> Error re-sending after select timeout: %s\n", client.gateway_id, strerror(errno));
                        return false;
                    }
                }
            }
            continue;
        }

        if (!FD_ISSET(sd, &read_fds))
            continue;

        unsigned char tmp[1024];
        ssize_t len = recv(sd, tmp, sizeof(tmp), 0);
        if (len <= 0)
        {
            if (len == 0)
                this->print_and_log("Peer closed socket\n");
            else
                this->print_and_log("recv error: %s\n", strerror(errno));
            return false;
        }
        std::string time_str;
        client.client_get_time(time_str, 1);

        print_and_log("Rx (%s -> %s): ", client.gateway_id, time_str.c_str());
        for (int i = 0; i < tmp[1] + 1; i++)
        {
            print_and_log("%02X ", tmp[i]);
        }
        print_and_log("\n");

        // append to buffer
        recvbuf.insert(recvbuf.end(), tmp, tmp + len);

        // Attempt to parse one or more frames. Protocol: total_length = recvbuf[1] + 1
        while (recvbuf.size() >= 2)
        {
            std::string start_time;
            client.client_get_time(start_time, 1);
            size_t frame_len = static_cast<size_t>(recvbuf[1]) + 1;
            if (frame_len == 0 || frame_len > recvbuf.size())
                break; // wait for more data

            // we have a full frame in recvbuf[0..frame_len-1]
            unsigned char *frame = recvbuf.data();
            unsigned char code = frame[2];
            unsigned char status = frame[frame_len - 1];

            this->print_and_log("(%s) -> Pkt Type:%02X , Status/Failure Byte:%02X\n", client.gateway_id, code, status);

            // print_and_log("Tx (%s -> %s): ", client.gateway_id, start_time.c_str());
            // for (int i = 0; i < this->client_tx_buffer[1] + 1; i++)
            // {
            //     print_and_log("%02X ", this->client_tx_buffer[i]);
            // }
            // print_and_log("\n");

            // handle special in-progress/timeouts
            if ((code == 0x09 || code == 0x03) && status == 0x06)
            {
                rf_retry_count++;
                this->print_and_log("Received timeout (0x06). Retry %d/%d\n", rf_retry_count, RFMAX_RETRIES);
                // resend previous
                if (this->client_tx_buffer)
                {
                    ssize_t tosend = this->client_tx_buffer[1] + 1;
                    if (tosend > 0)
                    {
                        ssize_t s = write(sd, this->client_tx_buffer, tosend);
                        if (s < 0)
                        {
                            this->print_and_log("Error re-sending: %s\n", strerror(errno));
                            return false;
                        }
                    }
                }
                // consume frame
                recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                continue;
            }
            else if ((code == 0x09 || code == 0x03) && status == 0x07)
            {
                rf_retry_count++;
                this->print_and_log("Received in-progress (0x07) - wait 12s before retry. Retry %d/%d\n", rf_retry_count, RFMAX_RETRIES);

                this->wait_after_flash(12, this->thread_run); // 02012026
                if (this->client_tx_buffer)
                {
                    ssize_t tosend = this->client_tx_buffer[1] + 1;
                    if (tosend > 0)
                    {
                        ssize_t s = write(sd, this->client_tx_buffer, tosend);
                        if (s < 0)
                        {
                            this->print_and_log("Error re-sending: %s\n", strerror(errno));
                            return false;
                        }
                    }
                }
                recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                continue;
            }
            else if (code == 0x04 || code == 0x82)
            {
                this->print_and_log("success byte validation state, framelen:%d\n", frame_len);
                // success-like response. Validate based on last byte and original command opcode

                int cmd_len = 0;
                if (this->client_tx_buffer)
                    cmd_len = this->client_tx_buffer[1] + 1;

                int frame_len_int = static_cast<int>(frame_len);

                if (frame_len_int >= 1 && cmd_len >= 2)
                {
                    unsigned char last_byte = frame[frame_len - 1];
                    //-------command opcode match for enable & entry
                    unsigned char cmd_opcode = this->client_tx_buffer[cmd_len - 2];
                    //-------response opcode match search
                    unsigned char resp_opcode = frame[frame_len - 2];
                    //-------command opcode match
                    unsigned char serialcmdopcode = this->client_tx_buffer[cmd_len - 1];

                    this->print_and_log("(%s) -> lastbyte:%02X , cmd_opcode Byte:%02X\n", client.gateway_id, last_byte, cmd_opcode);
                    // Success cases (adapt if your opcodes differ)
                    if (last_byte == 0x00 && (cmd_opcode == 0x9B || cmd_opcode == 0x9D))
                    {
                        // consume frame and return success
                        recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                        return true;
                    }
                    else if (last_byte == 0x00 && (resp_opcode == 0x02 || resp_opcode == 0x01) && (serialcmdopcode == 0x01 || serialcmdopcode == 0x02))
                    {
                        // consume frame and return success
                        recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                        return true;
                    }
                    else
                    {
                        // If not matching expected success, treat as failure for this command
                        this->print_and_log("(%s) -> Received response but validation failed (last = 0x%02X, cmd = 0x%02X)\n", client.gateway_id, last_byte, cmd_opcode);
                        recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                        return false;
                    }
                }
                else
                {
                    // malformed frame
                    recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                    return false;
                }
            }
            else
            {
                this->print_and_log("(%s) -> Invalid/unexpected response code (0x%02X). Treat as failure\n", client.gateway_id, code);
                recvbuf.erase(recvbuf.begin(), recvbuf.begin() + frame_len);
                return false;
            }
        } // end while parse frames
    } // end while retry loop

    this->print_and_log("Max retries reached in wait_for_socket_response()\n");
    return false;
}

#endif

// ---------- execute_fuota Unsilence sequence (refined, uses wait_for_socket_response()) ----------
int Fuota::execute_fuota_sequence_for_unsilence_network()
{

    print_and_log("(%s) -> Execute FUOTA Sequence For Unsilence Started\n", client.gateway_id);

    // Sequence states (substates to be sent)
    std::vector<int> fuota_states{
        PATH_UNSILENCE_STATE::AT_FUOTA_DISABLE,
        PATH_UNSILENCE_STATE::AT_FUOTA_MODE_ENTRY_DISABLE,
        PATH_UNSILENCE_STATE::AT_DISABLE_FLASHSAVE,
        PATH_UNSILENCE_STATE::AT_DISABLE_FLASHEXIT};

    for (auto state : fuota_states)
    {
        this->network_silence_state = state;
        int retry_count = 0;
        bool success = false;

        while (retry_count < 3 && !success)
        {
            print_and_log("(%s) -> Sending command for FUOTA state %d (Attempt %d)\n",
                          client.gateway_id, state, retry_count + 1);

            if (this->build_and_store_fuota_cmd() == -1)
            {
                print_and_log("(%s) -> Write failed for state %d, retrying...\n", client.gateway_id, state);
                retry_count++;
                // small backoff
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            // Wait for valid response
            if (!this->wait_for_socket_response(15))
            {
                print_and_log("(%s) -> Timeout/invalid response for Unsilnce state %d, retrying...\n", client.gateway_id, state);
                retry_count++;
                // small backoff before retrying
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            printf("(%s) -> FUOTA step %d completed successfully\n", client.gateway_id, state);
            success = true;
        }

        if (!success)
        {
            printf("(%s) -> FUOTA step %d failed after 3 retries.\n", client.gateway_id, state);
            return e_failure;
        }
    }

    printf("(%s) -> Execute FUOTA Sequence For Unsilnce Network Completed Successfully\n", client.gateway_id);
    return e_success_1;
}

/*
 * Unsilence the Network based on the max hop to least leaf node exclude intermediate nodes
 */
int Fuota::process_rffuota_disable_sequence(unsigned char *ondemand_mac)
{
    std::string readable_mac = Utility::mac_to_string(ondemand_mac);
    print_and_log("\n(%s) -> cclient: MultiHop RF FUOTA DISABLE (refactored)\n", client.gateway_id);
    print_and_log("On-demand FUOTA Target MAC: %s\n", readable_mac.c_str());

    // 1) Fetch and copy meters from DB
    std::vector<meter_vital_info> meters;
    if (!this->fetch_and_copy_meter_list_from_db(meters))
    {
        this->print_and_log("Failed to fetch meter list from DB\n");
        return e_failure;
    }
    if (meters.empty())
    {
        this->print_and_log("PAN not found or meter list is empty for DCU: %s\n", client.gateway_id);
        return e_failure;
    }

    // 2) Build gateway route info (allocates this->gateway_route_info)
    if (!this->build_gateway_route_info_from_meters(meters))
    {
        this->print_and_log("Failed to build gateway_route_info\n");
        return e_failure;
    }

    // 3) Detect leaf nodes
    auto leaf_indices = this->detect_leaf_nodes(meters);

    // 4) Filter out on-demand target MAC (don't treat it as leaf)
    auto filtered_leaf_indices = this->filter_out_on_demand(leaf_indices, meters, ondemand_mac);

    // 5) Send FUOTA DISABLE to each filtered leaf
    for (auto idx : filtered_leaf_indices)
    {
        const auto &leaf = meters[idx];

        // attempt per-leaf operation (this will compute route and call execute fuota_sequence_for_Unsilence)
        bool res = this->send_fuota_disable_to_leaf_nodes(leaf);

        // On success, you can optionally update silenced_nodes_for_fuota table here:
        if (res)
        {
            // Insert into DB table unsilenced_nodes_forfuota: meter_serial_number,gateway_id,meter_mac_address,hop_count

            this->db.insert_update_fuota_Unsilenced_meter_details_in_db((char *)client_query_buffer, (unsigned char *)leaf.meter_mac_address, /*leaf.meter_serial_number,*/ (unsigned char *)client.gateway_id, 0, leaf.hop_count);

            this->print_and_log("Update Unsilenced_nodes_for_fuota for %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
        }
        else
        {
            this->print_and_log("Leaf FUOTA Disable failed for %s (will continue with next leaf)\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());

            this->db.insert_update_fuota_Unsilenced_meter_details_in_db((char *)client_query_buffer, (unsigned char *)leaf.meter_mac_address, (unsigned char *)client.gateway_id, 1, leaf.hop_count);
        }
    }

    // 6) After finishing leaf nodes disabling, prepare to unsilnce the on-demand node
    print_and_log("\n Starting FUOTA DISABLE state of on-demand node: %s\n", readable_mac.c_str());
    this->ondemand_fuota_state = FUOTA_STATE::GATEWAY_PATH_UNSILENCE;

    // 7) Clean up gateway_route_info memory
    // this->free_gateway_route_info();

    print_and_log("\n(%s) -> RF FUOTA disable sequence (refactored) completed successfully.\n", client.gateway_id);
    return e_success_1;
}

/**
 * Per-leaf operation:
 *  - set current target mac
 *  - compute router path into current_fuota_route
 *  - set use_alternate_fuota_route, call executefuota_sequence_Unsilence()
 * Returns true if FUOTA DISABLE succeeded for this leaf.
 */
bool Fuota::send_fuota_disable_to_leaf_nodes(const meter_vital_info &leaf)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    // copy mac into current target
    memcpy(this->current_fuota_target_mac, leaf.meter_mac_address, 8);

    // compute router path (uses this->gateway_route_info internally)
    router_path_t tmp_route;
    memset(&tmp_route, 0, sizeof(tmp_route));

    if (!this->compute_router_path(this->current_fuota_target_mac, &tmp_route))
    {
        this->print_and_log("Route not found for leaf node: %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
        return false;
    }

    // copy computed route into current_fuota_route for downstream functions that expect it
    memset(this->current_fuota_route, 0, sizeof(*((router_path_t *)this->current_fuota_route))); // ensure zeroing
    for (int i = 0; i < tmp_route.hop_count && i < MAX_ROUTE_HOPS; ++i)
    {
        memcpy(((router_path_t *)this->current_fuota_route)->paths[i], tmp_route.paths[i], 4);
    }
    ((router_path_t *)this->current_fuota_route)->hop_count = tmp_route.hop_count;

    // enable alternate route usage for leaf execution (preserves your previous behaviour)
    this->use_alternate_fuotaroute_disable = true; // 31122025

    bool success = false;
    int attempt_ret = this->execute_fuota_sequence_for_unsilence_network(); // existing function handles the enable sequence
    success = (attempt_ret == e_success_1);

    if (!success)
    {
        this->print_and_log("FUOTA DISABLE failed for leaf node: %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
    }
    else
    {
        this->print_and_log("FUOTA DISABLE succeeded for leaf node: %s\n", Utility::mac_to_string(leaf.meter_mac_address).c_str());
    }

    // restore flag
    this->use_alternate_fuotaroute_disable = false;
    return success;
}

int Fuota::client_process_ondemand_data(void)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    this->ondemand_data_download_completed = 0;

    memset(&this->router_path, 0, sizeof(path_details));

    while (!client.RF_Meter_FUOTA.empty())
    {
        this->cmd_bytes = client.RF_Meter_FUOTA.front();

        std::string cmd_str(this->cmd_bytes.begin(), this->cmd_bytes.end());
        std::vector<std::string> parts = client.split(cmd_str, ':');

        int request_id = -1;
        if (parts.size() > 0 && !parts[0].empty())
            request_id = std::stoi(parts[0]);

        std::vector<uint8_t> hex_data;
        if (parts.size() > 5 && !parts[5].empty())
        {
            std::string hex_field = parts[5];
            for (size_t i = 0; i + 1 < hex_field.length(); i += 2)
            {
                std::string byte_str = hex_field.substr(i, 2);
                hex_data.push_back(static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16)));
            }
        }

        this->print_and_log("FUOTA: RequestID = %d HexDataLen = %zu\n", request_id, hex_data.size());

        // Custom FUOTA handling...
        client.RF_Meter_FUOTA.pop();
    }

    // this->prev_state_timeout_timer_id = this->timeout_timer_id;
    this->prev_client_current_state = this->ondemand_fuota_state;
    this->ondemand_fuota_state = FUOTA_STATE::IDLE;

    this->prev_pmesh_download_data_type = this->pmesh_download_data_type;
    this->prev_download_mode = this->fuotadownload_mode;

    this->print_and_log("Prev data download type:\n", this->pmesh_download_data_type);
    this->print_and_log("this->download_mode: \n", this->fuotadownload_mode);

    return e_success_0;
}

/*
 * Fuota updation sequence statuses updating into database
 */
int Fuota::ondemand_fuota_update_status(int request_status, int error_code, int ondemand_request_id)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    // Keep last status and error for each request_id (static map, persists across calls)
    static std::unordered_map<int, std::pair<int, int>> last_status_map; // id -> (status, error_code)

    int last_error = -1;
    auto it = last_status_map.find(ondemand_request_id);
    if (it != last_status_map.end())
    {
        last_error = it->second.second;
    }

    // If previously this request rolled back (error_code==0), treat as final and skip further updates
    if (last_error == 0)
    {
        this->print_and_log("(%s) = Previous update for request %d indicates final rollback (error_code=0). Skipping further updates.\n", client.gateway_id, ondemand_request_id);
        return e_success_0;
    }

    // ====== Continue with update ======

    this->print_and_log("gateway_id: %s, ondemand fuotastatus: %d,status error_code: %d\n", client.gateway_id, request_status, error_code);

    bzero(this->client_query_buffer, sizeof this->client_query_buffer);

    // Map certain transient updates to final state when appropriate
    int db_status = request_status;
    int db_error = error_code;

    // If stage 17 (Get Activation Status) succeeded (error_code==1), treat as final success (21)
    if (request_status == 17 && error_code == 1)
    {
        db_status = 21; // Firmware Upgrade Success
        db_error = 1;
        this->print_and_log("(%s) [FUOTA] Stage 17 reported success -> marking request %d as final status 21 (Success)\n", client.gateway_id, ondemand_request_id);
    }

    // Update DB with mapped status/error
    this->db.update_ondemand_RF_Fuota_upload_status((char *)this->client_query_buffer, ondemand_request_id, db_status, db_error);

    // Save last status + error
    last_status_map[ondemand_request_id] = std::make_pair(db_status, db_error);

    // If this is a terminal result (final success or explicit rollback/final failure (error_code==0)),
    // mark it for the FUOTA FSM/queue processor to handle (dequeue + start next) rather than doing it here.
    bool is_terminal = (db_status == 21 && db_error == 1) || (error_code == 0);
    if (is_terminal)
    {
        this->pending_terminal_request_id.store(ondemand_request_id);
        this->pending_terminal_complete.store(true);
        this->print_and_log("(%s) [FUOTA] Request %d reached terminal state (status=%d,error=%d) — deferring dequeue to FUOTA queue processor.\n", client.gateway_id, ondemand_request_id, db_status, db_error);
    }

    return e_success_0;
}

/*
 * Resume Fuota after Gateway Reconnection
 */
int Fuota::resume_rf_fuota_pending_state_process(unsigned char *dcuid, unsigned char *target_mac, int request_id, const std::string &resume_filepath, std::string &filename)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    int req_id = -1;

    (void)target_mac;
    (void)request_id;
    (void)resume_filepath;
    (void)filename;

    this->print_and_log("(%s) -> resume_rf_fuota_pending_state_process: fetching pending resume entry from DB for DCU %s\n", client.gateway_id, dcuid);

    int rc = this->db.fetch_pending_fuota_targetnode_path((char *)client_query_buffer, dcuid, this->resumed_target_mac, this->resume_fwpath, req_id, this->resume_file_name);

    this->print_and_log("(%s) -> resume_rf_fuota_pending_state_process: DB fetch returned rc=%d, resumed_target_mac=%s, resume_fwpath=%s, resume_file=%s, req_id=%d\n", client.gateway_id, rc, this->resumed_target_mac, this->resume_fwpath.c_str(), this->resume_file_name.c_str(), req_id);

    if (rc == e_success_0)
    {
        this->print_and_log(" (%s) Trigger FUOTA → MAC = %s req_id = %d\n ", client.gateway_id, this->resumed_target_mac, req_id);

        this->resume_fuota_flag = true;
        int ret = this->get_resumefuota_for_requestedfirmware_filepath(this->resumed_target_mac, this->resume_fwpath, req_id);
        if (ret == e_success_0)
        {
            // Set FSM entry state and immediately kick the FUOTA FSM to start processing the resumed job
            this->ondemand_fuota_state = FUOTA_STATE::OPEN_FILE; //'ll pass the path,filename
            this->print_and_log("(%s) -> resume fetch succeeded, starting FUOTA FSM (state OPEN_FILE)\n", client.gateway_id);
            this->cansend_fuota_next_command();
        }
        else
        {
            this->print_and_log("(%s) -> Resume fetch failed, will not start FSM\n", client.gateway_id);
            this->resume_fuota_flag = false;
        }
    }
    else if (rc == e_failure)
    {
        this->print_and_log(" (%s) = No pending FUOTA tasks\n", client.gateway_id);
        this->resume_fuota_flag = false;
    }
    else
    {
        this->print_and_log(" (%s) = DB error!\n", client.gateway_id);
        this->resume_fuota_flag = false;
    }
    return 0;
}

/*
 * Resume fuota invocation
 */
int Fuota::get_resumefuota_for_requestedfirmware_filepath(unsigned char *target_node, const std::string &fw_path, int reqid)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    (void)target_node;
    (void)reqid;

    std::string file_name = this->resume_file_name; // from DB
    if (file_name.empty())
    {
        this->print_and_log("No filename returned from DB!%s\n", client.gateway_id);
        return e_failure;
    }

    // Construct correct target firmware file path
    std::string full_file_path = fw_path + "/" + file_name;

    // Validate file exist
    if (access(full_file_path.c_str(), F_OK) != 0)
    {
        this->print_and_log("Firmware file not found:%s ->%d\n ", full_file_path, strerror(errno));
        return e_failure;
    }

    // Open BIN file
    this->fouta_read_fd = fopen(full_file_path.c_str(), "rb");
    if (!this->fouta_read_fd)
    {
        this->print_and_log("Failed to open file:%d\n ", strerror(errno));
        return e_failure;
    }

    this->set_latest_firmware_path(full_file_path);
    this->total_firmwaresize = this->get_fw_size(full_file_path);

    this->print_and_log("Resume FUOTA %s → File Ready: %d bytes\n ", full_file_path, this->total_firmwaresize);

    return e_success_0;
}

int Fuota::comparision_of_firmware_from_meterdetails_and_from_latest_uploaded_firmware(unsigned char *data)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    unsigned char query_buff[1024] = {0};
    unsigned char old_firmware_read[200] = {0};
    unsigned char latest_firmware_read[200] = {0};

    print_and_log("target Mac address: ");
    for (int i = 0; i < 8; i++)
    {
        print_and_log("%02X ", this->current_fuota_target_mac[i]);
    }
    print_and_log("\n");

    memset(&old_firmware_read[0], 0, sizeof(old_firmware_read));
    this->db.get_rf_internal_firmware_version_from_meter_details((char *)query_buff, (unsigned char *)client.gateway_id, this->current_fuota_target_mac, (char *)old_firmware_read);
    int length = data[1];
    if (length < 0)
    {
        return -1;
    }
    if (client.hop_count == 0)
    {
        client.hop_count = 1; // for self node hop index 00
    }

    int offset = 10 /*st,len,pkttype,src*/ + (client.hop_count * 4) + 2 /*success byte*/;

    int fw_newlen = length - (offset - 1);
    print_and_log("fw_newlen: %d\n", fw_newlen);
    if (fw_newlen <= 0)
    {
        return -1;
    }
    memcpy(&latest_firmware_read[0], data + offset, fw_newlen);

    int j = 0;
    for (int i = 0; i < fw_newlen; i++)
    {
        if (isprint(data[offset + i]))
        {
            latest_firmware_read[j++] = data[offset + i];
        }
    }
    latest_firmware_read[j] = '\0';
    if (strcmp((char *)old_firmware_read, (char *)latest_firmware_read) != 0)
    {
        print_and_log("(%s) -> Uploaded Firmware is Different\n", client.gateway_id);
    }
    print_and_log("(%s) -> NEW FIRMWARE: %s\n", client.gateway_id, latest_firmware_read);
    return 0;
}

/*
 * Trying of alternate route with same hop at fuota image transfer state
 */

void Fuota::prepare_alternate_sameroute_retry(uint8_t *payload, uint16_t payload_len)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    if (!payload || payload_len == 0)
    {
        this->print_and_log("(%s) -> ERROR: Empty payload for alternate retry\n", client.gateway_id);
        return;
    }

    cntx.retry_count = 0;
    cntx.alternate_retry++;

    this->ondemand_fuota_update_status(22, 1, client.request_id);

    /* Extract router count from current TX frame */
    uint8_t router_count = this->client_tx_buffer[12];

    if (router_count == 0)
    {
        this->print_and_log("(%s) -> No routers in current path, skipping alternate retry\n", client.gateway_id);
        return;
    }

    /* Extract LAST router MAC (4 bytes) */
    size_t offset = 13;
    unsigned char last_router_mac[4] = {0};

    for (uint8_t i = 0; i < router_count; i++)
    {
        memcpy(last_router_mac, &this->client_tx_buffer[offset], 4);
        offset += 4;
    }

    this->print_and_log("(%s) -> Last Router MAC = %02X%02X%02X%02X\n", client.gateway_id, last_router_mac[0], last_router_mac[1], last_router_mac[2], last_router_mac[3]);

    /* Build target MAC (DCU + last router) */
    unsigned char ondemand_mac_addr[8] = {0};
    Utility::ascii_hex_to_bin(ondemand_mac_addr, "3CC1F601", 8);
    memcpy(&ondemand_mac_addr[4], last_router_mac, 4);

    this->print_and_log("(%s) -> Alternate target MAC = %02X%02X%02X%02X%02X%02X%02X%02X\n",
                        client.gateway_id,
                        ondemand_mac_addr[0], ondemand_mac_addr[1],
                        ondemand_mac_addr[2], ondemand_mac_addr[3],
                        ondemand_mac_addr[4], ondemand_mac_addr[5],
                        ondemand_mac_addr[6], ondemand_mac_addr[7]);

    /* Build alternate route ONLY if same hop_count exists */
    if (!this->build_alternateroute_image_transfer(ondemand_mac_addr, router_count))
    {
        this->print_and_log("(%s) -> Alternate route not found, exhausting retries\n", client.gateway_id);
        cntx.alternate_retry = cntx.max_retries;
        return;
    }

    /* Rebuild TX buffer using SAME payload */
    int len = this->update_client_tx_buffer_with_route(this->client_tx_buffer, payload, payload_len);

    // memcpy(this->current_fuota_command, this->client_tx_buffer, len);
    for (int i = 0; i < len; i++)
    {
        printf("%02X ", this->client_tx_buffer[i]);
    }
    printf("\n");

    this->print_and_log("(%s) -> Alternate FUOTA route prepared (len = %d, alt_retry = %d)\n", client.gateway_id, len, cntx.alternate_retry);
}

bool Fuota::build_alternateroute_image_transfer(unsigned char *target_mac, uint8_t router_count)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);

    if (!target_mac)
        return false;

    char qbuf[1024] = {0};
    this->ondemand_fuota_update_status(22, 1 /*success*/, client.request_id);
    int rc = this->db.get_alternate_source_route_network_from_db(qbuf, (unsigned char *)client.gateway_id, target_mac, this);

    if (rc != SUCCESS && rc != e_success_0 && rc != 0)
        return false;

    if (!this->alternatepaths)
    {
        this->print_and_log("(%s) -> No alternate paths in DB\n", client.gateway_id);
        return false;
    }

    /* Select alternate path with SAME hop_count */
    alternate_path_info *cur = this->alternatepaths.get();
    alternate_path_info *chosen = nullptr;

    while (cur)
    {
        if (cur->hop_count == router_count)
        {
            chosen = cur;
            break;
        }
        cur = cur->next.get();
    }

    if (!chosen)
    {
        this->print_and_log("(%s) -> No alternate path with hop_count=%d\n", client.gateway_id, router_count);
        return false;
    }

    /* Copy chosen route */
    router_path_t *dst = (router_path_t *)this->current_fuota_route;
    memset(dst, 0, sizeof(router_path_t));

    memcpy(dst->paths, chosen->route_path, chosen->hop_count * 4);
    dst->hop_count = chosen->hop_count;

    memcpy(this->current_fuota_target_mac, chosen->meter_mac_address, 8);

    /* Debug print */
    this->print_and_log("(%s) -> Selected alternate path: ", client.gateway_id);
    for (int i = 0; i < dst->hop_count; i++)
    {
        printf("[%02X%02X%02X%02X] ",
               dst->paths[i][0],
               dst->paths[i][1],
               dst->paths[i][2],
               dst->paths[i][3]);
    }
    printf("\n");

    return true;
}

//---------not using to decide silenceing nodes 70% then only fuota update else drop out the request--

StepStat Fuota::execute_silence_state_for_all_nodes(int state)
{
    this->print_and_log("(%s) -> Function: %s\n", client.gateway_id, __FUNCTION__);
    StepStat stat{};
    stat.total = node_list.size();

    for (auto &node : node_list)
    {
        (void)node;
        bool node_ok = false;

        for (int attempt = 1; attempt <= MAX_NODE_RETRIES; attempt++)
        {
            // set_current_target_node(node);
            network_silence_state = state;

            //  print_and_log("(%s) State %d → Node %s (Attempt %d)\n", client.gateway_id, state, node.node_id.c_str(), attempt);

            if (build_and_store_fuota_cmd() < 0)
                continue;

            if (wait_for_socket_response(15))
            {
                node_ok = true;
                break;
            }
        }

        if (node_ok)
            stat.success++;
    }

    return stat;
}

int Fuota::execute_fuotasequence_to_silencenetwork()
{
    print_and_log("(%s) → Execute FUOTA Silence Sequence START\n", client.gateway_id);

    std::vector<int> fuota_states =
        {
            PATH_SILENCE_STATE::AT_FUOTA_ENABLE,
            PATH_SILENCE_STATE::AT_FUOTA_MODE_ENTRY,
            PATH_SILENCE_STATE::AT_ENABLE_FLASHSAVE,
            PATH_SILENCE_STATE::AT_ENABLE_FLASHEXIT};

    for (auto state : fuota_states)
    {
        print_and_log("[FUOTA] Executing silence state %d\n", state);

        StepStat stat = execute_silence_state_for_all_nodes(state);

        float percent = (stat.total == 0) ? 0.0f : (stat.success * 100.0f) / stat.total;

        print_and_log("[FUOTA] State %d result: %d/%d (%.2f%%)\n", state, stat.success, stat.total, percent);

        if (percent < MIN_SUCCESS_PERCENT)
        {
            print_and_log("[FUOTA][ABORT] State %d failed (%.2f%% < %.2f%%)\n", state, percent, MIN_SUCCESS_PERCENT);

            ondemand_fuota_state = FUOTA_STATE::ROLLBACK_TO_NORMAL_COMM_MODE;
            return e_failure;
        }

        print_and_log("[FUOTA] State %d PASSED — continuing\n", state);
    }

    print_and_log("(%s) → FUOTA Silence Sequence COMPLETED\n", client.gateway_id);

    return e_success_1;
}

#endif // __FUOTA_CPP__