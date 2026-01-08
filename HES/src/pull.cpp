#include "../inc/pull.h"
#include "../inc/utility.h"

std::array<uint8_t, 8> PullData::mac_from_hex(const char *hex)
{
    std::array<uint8_t, 8> mac{};
    for (int i = 0; i < 8; ++i)
    {
        std::string byte_str(hex + (i * 2), 2);
        mac[i] = static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16));
    }
    return mac;
}

void PullData::mac_to_hex(const std::array<uint8_t, 8> &mac, char *out16)
{
    // out16 must have space for 16 hex chars + null = 17 bytes
    for (int i = 0; i < 8; ++i)
    {
        sprintf(out16 + (i * 2), "%02X", mac[i]);
    }
    out16[16] = '\0'; // Null terminate
}

bool PullData::extract_path(const std::string &hex_path, int hop_count, std::vector<uint8_t> &out)
{
    out.clear();

    const int kChunkHexLen = 8;              // 8 hex chars
    const int kExpectedLen = hop_count * 16; // each hop = 16 hex chars (skip+take)

    // Validate total hex length
    if (hex_path.size() != static_cast<size_t>(kExpectedLen))
    {
        return false;
    }

    bool take = false; // first 8 chars = skip

    for (int pos = 0; pos + kChunkHexLen <= kExpectedLen; pos += kChunkHexLen)
    {
        if (take)
        {
            // Convert 8 hex chars → 4 bytes
            for (int i = 0; i < kChunkHexLen; i += 2)
            {
                uint8_t byte = static_cast<uint8_t>(std::stoi(hex_path.substr(pos + i, 2), nullptr, 16));
                out.push_back(byte);
            }
        }
        take = !take; // flip skip/take
    }

    return true;
}

void PullData::GetYesterdayRange(time_t &start, time_t &end)
{
    std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);

    tm_now.tm_mday -= 1;
    std::mktime(&tm_now);

    // Start 00:00:00
    std::tm tm_start = tm_now;
    tm_start.tm_hour = 0;
    tm_start.tm_min = 0;
    tm_start.tm_sec = 0;
    start = std::mktime(&tm_start);

    // End 23:59:59
    std::tm tm_end = tm_now;
    tm_end.tm_hour = 23;
    tm_end.tm_min = 59;
    tm_end.tm_sec = 59;
    end = std::mktime(&tm_end);
}

void PullData::GetPreviousHourRange(time_t &start, time_t &end)
{
    time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);

    // Move one hour back
    tm_now.tm_hour -= 1;
    std::mktime(&tm_now); // Normalize

    // Start of previous hour
    std::tm tm_start = tm_now;
    tm_start.tm_min = 0;
    tm_start.tm_sec = 0;
    start = std::mktime(&tm_start);

    // End of previous hour
    std::tm tm_end = tm_now;
    tm_end.tm_min = 59;
    tm_end.tm_sec = 59;
    end = std::mktime(&tm_end);
}

uint32_t PullData::SecondsFromBase(time_t t)
{
    std::tm base_tm{};
    base_tm.tm_year = 100;
    base_tm.tm_mon = 0;
    base_tm.tm_mday = 1;
    time_t base = std::mktime(&base_tm);
    return static_cast<uint32_t>(std::difftime(t, base));
}

uint8_t PullData::calculate_checksum(const uint8_t *data, size_t len)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    uint16_t sum = 0;

    for (size_t i = 0; i < len; i++)
    {
        sum += data[i];
    }

    return static_cast<uint8_t>(sum & 0xFF);
}

std::array<uint8_t, 8> PullData::to_mac_array(const uint8_t mac[4])
{
    std::array<uint8_t, 8> arr{};
    memcpy(&arr[0], proprietary_mac_address, 4);
    memcpy(&arr[4], mac, 4);
    return arr;
}

int PullData::process_nameplate_data(NodeInfo *node, uint8_t *buff, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (!node)
    {
        this->print_and_log("Node is nullptr\n");
        return FAILURE;
    }

    if (length <= 0 || buff == nullptr)
    {
        this->print_and_log("Invalid buffer/length\n");
        return FAILURE;
    }

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return FAILURE;
    }
    std::array<uint8_t, 8> mac = this->to_mac_array(resp->pmesh.destination_addr);

    if (mac != node->node_mac_address)
    {
        this->print_and_log("Invalid source address\n");
        return FAILURE;
    }

    this->print_and_log("[NameplateProfile] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node->profile_data.name_plate_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[NameplateProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node->profile_data.name_plate_profile.last_packet_time = std::chrono::system_clock::now();
        node->profile_data.name_plate_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[NameplateProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
        return FAILURE;
    }

    if (resp->dlms.next_page_status == 0)
    {
        return SUCCESS;
    }
    else if (resp->dlms.next_page_status == 1)
    {
        return NEXT_PAGE_PRESENT;
    }

    this->print_and_log("Invalid next page status\n");
    return FAILURE;
}

int PullData::process_daily_load_data(NodeInfo *node, uint8_t *buff, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (!node)
    {
        this->print_and_log("Node is nullptr\n");
        return FAILURE;
    }

    if (length <= 0 || buff == nullptr)
    {
        this->print_and_log("Invalid buffer/length\n");
        return FAILURE;
    }

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return FAILURE;
    }
    std::array<uint8_t, 8> mac = this->to_mac_array(resp->pmesh.destination_addr);

    if (mac != node->node_mac_address)
    {
        this->print_and_log("Invalid source address\n");
        return FAILURE;
    }

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node->profile_data.daily_load_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[DailyLoadProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node->profile_data.daily_load_profile.last_packet_time = std::chrono::system_clock::now();
        node->profile_data.daily_load_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[DailyLoadProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
        return FAILURE;
    }

    if (resp->dlms.next_page_status == 0)
    {
        return SUCCESS;
    }
    else if (resp->dlms.next_page_status == 1)
    {
        return NEXT_PAGE_PRESENT;
    }

    this->print_and_log("Invalid next page status\n");
    return FAILURE;
}

int PullData::process_block_load_data(NodeInfo *node, uint8_t *buff, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (!node)
    {
        this->print_and_log("Node is nullptr\n");
        return FAILURE;
    }

    if (length <= 0 || buff == nullptr)
    {
        this->print_and_log("Invalid buffer/length\n");
        return FAILURE;
    }

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return FAILURE;
    }
    std::array<uint8_t, 8> mac = this->to_mac_array(resp->pmesh.destination_addr);

    if (mac != node->node_mac_address)
    {
        this->print_and_log("Invalid source address\n");
        return FAILURE;
    }

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseBlockLoadDLMSRecords(v, resp->dlms.no_of_records, node->profile_data.block_load_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[BlockLoadProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node->profile_data.block_load_profile.last_packet_time = std::chrono::system_clock::now();
        node->profile_data.block_load_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[BlockLoadProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
        return FAILURE;
    }

    if (resp->dlms.next_page_status == 0)
    {
        return SUCCESS;
    }
    else if (resp->dlms.next_page_status == 1)
    {
        return NEXT_PAGE_PRESENT;
    }

    this->print_and_log("Invalid next page status\n");
    return FAILURE;
}

int PullData::process_billing_history_data(NodeInfo *node, uint8_t *buff, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (!node)
    {
        this->print_and_log("Node is nullptr\n");
        return FAILURE;
    }

    if (length <= 0 || buff == nullptr)
    {
        this->print_and_log("Invalid buffer/length\n");
        return FAILURE;
    }

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return FAILURE;
    }
    std::array<uint8_t, 8> mac = this->to_mac_array(resp->pmesh.destination_addr);

    if (mac != node->node_mac_address)
    {
        this->print_and_log("Invalid source address\n");
        return FAILURE;
    }

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node->profile_data.billing_history);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[BillingProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node->profile_data.billing_history.last_packet_time = std::chrono::system_clock::now();
        node->profile_data.billing_history.total_packets_received++;
    }
    else
    {
        this->print_and_log("[BillingProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
        return FAILURE;
    }

    if (resp->dlms.next_page_status == 0)
    {
        return SUCCESS;
    }
    else if (resp->dlms.next_page_status == 1)
    {
        return NEXT_PAGE_PRESENT;
    }

    this->print_and_log("Invalid next page status\n");
    return FAILURE;
}

int PullData::process_ip_profile_data(NodeInfo *node, uint8_t *buff, ssize_t length)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    if (!node)
    {
        this->print_and_log("Node is nullptr\n");
        return FAILURE;
    }

    if (length <= 0 || buff == nullptr)
    {
        this->print_and_log("Invalid buffer/length\n");
        return FAILURE;
    }

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return FAILURE;
    }
    std::array<uint8_t, 8> mac = this->to_mac_array(resp->pmesh.destination_addr);

    if (mac != node->node_mac_address)
    {
        this->print_and_log("Invalid source address\n");
        return FAILURE;
    }

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node->profile_data.instantaneous_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[InstantaneousProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node->profile_data.instantaneous_profile.last_packet_time = std::chrono::system_clock::now();
        node->profile_data.instantaneous_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[InstantaneousProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
        return FAILURE;
    }

    if (resp->dlms.next_page_status == 0)
    {
        return SUCCESS;
    }
    else if (resp->dlms.next_page_status == 1)
    {
        return NEXT_PAGE_PRESENT;
    }

    this->print_and_log("Invalid next page status\n");
    return FAILURE;
}
