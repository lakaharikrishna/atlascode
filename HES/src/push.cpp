#include "../inc/push.h"

std::array<uint8_t, 8> PushData::to_mac_array(const uint8_t mac[4])
{
    std::array<uint8_t, 8> arr{};
    memcpy(&arr[0], proprietary_mac_address, 4);
    memcpy(&arr[4], mac, 4);
    return arr;
}

uint8_t PushData::calculate_checksum(const uint8_t *data, size_t len)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    uint16_t sum = 0;

    for (size_t i = 0; i < len; i++)
    {
        sum += data[i];
    }

    return static_cast<uint8_t>(sum & 0xFF);
}

void PushData::cleanup_push_profiles(void)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto now = std::chrono::system_clock::now();
    const auto timeout = std::chrono::minutes(2);

    for (auto &pair : this->push_node_info)
    {
        PushNodeInfo &node = pair.second;
        const std::string mac_str = Utility::mac_to_string(pair.first.data());

        // ----- Instantaneous Profile -----
        if (node.instantaneous_profile.total_packets_received > 0 &&
            now - node.instantaneous_profile.last_packet_time > timeout)
        {
            node.instantaneous_profile.clear();
            this->print_and_log("Cleared Instantaneous Profile for node: %s\n", mac_str.c_str());
        }

        // ----- Daily Load Profile -----
        if (node.daily_load_profile.total_packets_received > 0 &&
            now - node.daily_load_profile.last_packet_time > timeout)
        {
            node.daily_load_profile.clear();
            this->print_and_log("Cleared Daily Load Profile for node: %s\n", mac_str.c_str());
        }

        // ----- Block Load Profile -----
        if (node.block_load_profile.total_packets_received > 0 &&
            now - node.block_load_profile.last_packet_time > timeout)
        {
            node.block_load_profile.clear();
            this->print_and_log("Cleared Block Load Profile for node: %s\n", mac_str.c_str());
        }

        // ----- Billing History Profile -----
        if (node.billing_history.total_packets_received > 0 &&
            now - node.billing_history.last_packet_time > timeout)
        {
            node.billing_history.clear();
            this->print_and_log("Cleared Billing History Profile for node: %s\n", mac_str.c_str());
        }

        // ----- Events Power On Profile -----
        if (node.power_on_event.total_packets_received > 0 &&
            now - node.power_on_event.last_packet_time > timeout)
        {
            node.power_on_event.clear();
            this->print_and_log("Cleared Power On Events Profile for node: %s\n", mac_str.c_str());
        }

        // ----- Events Power Off Profile -----
        if (node.power_off_event.total_packets_received > 0 &&
            now - node.power_off_event.last_packet_time > timeout)
        {
            node.power_off_event.clear();
            this->print_and_log("Cleared Power Off Events Profile for node: %s\n", mac_str.c_str());
        }
    }
}

void PushData::process_push_data(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);

    switch (resp->dlms.frame_id)
    {
        case FI_INSTANT_DATA: {
            switch (resp->dlms.command)
            {
                case COMMAND_IP_PROFILE: {
                    this->process_IP_push_data(buff, length, gateway_id);
                    break;
                }

                case COMMAND_BLOCK_LOAD_PROFILE: {
                    this->process_BLP_push_data(buff, length, gateway_id);
                    break;
                }

                case COMMAND_DAILY_LOAD_PROFILE: {
                    this->process_DLP_push_data(buff, length, gateway_id);
                    break;
                }

                case COMMAND_BILLING_PROFILE: {
                    this->process_BHP_push_data(buff, length, gateway_id);
                    break;
                }

                default:
                    break;
            }
            break;
        }

        case FI_INSTANT_EVENT_OBJECT_READ: {
            this->process_power_on_event(buff, length, gateway_id);
            break;
        }

        case FI_INSTANT_POWERFAIL_OBJECT_READ: {
            this->process_power_off_event(buff, length, gateway_id);
            break;
        }

        default: {
            this->print_and_log("Invalid frame ID in push data\n");
            break;
        }
    }
}

int PushData::parseDLMSRecords(const std::vector<uint8_t> &data, const uint8_t &number_of_records, PacketBuffer<DlmsRecordMap> &records)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    const size_t pmesh_header_size = sizeof(PmeshPushResponse);
    const size_t dlms_header_size = 9;

    int received_records = 0;

    if (data.size() < pmesh_header_size + dlms_header_size)
    {
        this->print_and_log("Packet too small: size=%u expected_min=%u\n", (unsigned)data.size(), (unsigned)(pmesh_header_size + dlms_header_size));

        return FAILURE;
    }

    size_t offset = pmesh_header_size + dlms_header_size;

    while (offset < data.size() - 1)
    {
        if (offset + 3 > data.size())
        {
            this->print_and_log("Record header overflow at offset=%zu\n", offset);
            break;
        }

        uint8_t record_id = data[offset++];
        uint8_t delimiter = data[offset++];

        if (delimiter != 0x00)
        {
            this->print_and_log("Missing record delimiter for id=0x%02X at offset=%zu\n", record_id, offset - 1);
            break;
        }

        DLMSDataType dtype = static_cast<DLMSDataType>(data[offset++]);

        DLMSValueStruct value;
        if (!this->parseByType(data, offset, dtype, value))
        {
            this->print_and_log("Failed to parse record id=0x%02X at offset=%zu\n", record_id, offset);
            break;
        }

        records.profile_data[record_id] = value;
        received_records++;

        this->print_and_log("Parsed record id=0x%02X type=%u total_parsed=%u\n", record_id, (unsigned)dtype, received_records);
    }

    if (received_records == number_of_records)
    {
        this->print_and_log("SUCCESS expected=%u received=%u\n", number_of_records, received_records);
        return SUCCESS;
    }

    this->print_and_log("FAILURE expected=%u received=%u\n", number_of_records, received_records);
    return FAILURE;
}

int PushData::parseBlockLoadDLMSRecords(const std::vector<uint8_t> &data, const uint8_t &number_of_records, PacketBufferBlockLoad &records)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    const size_t pmesh_header_size = sizeof(PmeshPushResponse);
    const size_t dlms_header_size = 9;

    int received_records = 0;

    if (data.size() < pmesh_header_size + dlms_header_size)
    {
        this->print_and_log("Packet too small: size=%u expected_min=%u\n", (unsigned)data.size(), (unsigned)(pmesh_header_size + dlms_header_size));

        return FAILURE;
    }

    size_t offset = pmesh_header_size + dlms_header_size;

    while (offset < data.size() - 1)
    {
        if (offset + 3 > data.size())
        {
            this->print_and_log("Record header overflow at offset=%zu\n", offset);
            break;
        }

        uint8_t record_id = data[offset++];
        uint8_t delimiter = data[offset++];

        if (delimiter != 0x00)
        {
            this->print_and_log("Missing record delimiter for id=0x%02X at offset=%zu\n", record_id, offset - 1);
            break;
        }

        DLMSDataType dtype = static_cast<DLMSDataType>(data[offset++]);

        DLMSValueStruct value;
        if (!this->parseByType(data, offset, dtype, value))
        {
            this->print_and_log("Failed to parse record id=0x%02X at offset=%zu\n", record_id, offset);
            break;
        }

        records.partial_profile[record_id] = value;

        if (record_id == 0x06)
        {
            records.profiles_data.push_back(records.partial_profile);
            records.partial_profile.clear();
        }
        received_records++;

        this->print_and_log("Parsed record id=0x%02X type=%u total_parsed=%u\n", record_id, (unsigned)dtype, received_records);
    }

    if (received_records == number_of_records)
    {
        this->print_and_log("SUCCESS expected=%u received=%u\n", number_of_records, received_records);
        return SUCCESS;
    }

    this->print_and_log("FAILURE expected=%u received=%u\n", number_of_records, received_records);
    return FAILURE;
}

void PushData::process_IP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];

    this->print_and_log("[InstantaneousProfile] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node.instantaneous_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[InstantaneousProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node.instantaneous_profile.last_packet_time = std::chrono::system_clock::now();
        node.instantaneous_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[InstantaneousProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
    }

    // if (resp->pmesh.remaining_pkt_count == 0x00)
    if (resp->dlms.next_page_status == 0x00)
    {
        int received = node.instantaneous_profile.total_packets_received;
        // int expected = resp->pmesh.current_pkt_count;
        int expected = resp->dlms.current_page_index + 1;

        this->print_and_log("[InstantaneousProfile] Node %s → Packets Received: %d, Expected: %d\n", Utility::mac_to_string(mac.data()).c_str(), received, expected);

        if (received == expected)
        {
            this->printRecords(node.instantaneous_profile.profile_data);
            this->print_and_log("[InstantaneousProfile] Node %s → OK (Updating DB)\n", Utility::mac_to_string(mac.data()).c_str());
            this->insert_instantaneous_parameters_profile_data(mac, gateway_id, calculate_cycle_id(0), node.instantaneous_profile, 1);
            node.instantaneous_profile.clear();
            this->print_and_log("[InstantaneousProfile] Node %s → Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            node.instantaneous_profile.clear();
            this->print_and_log("[InstantaneousProfile] Node %s →  Corrupted/Partial -> Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

void PushData::process_DLP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);

    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }

    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];

    this->print_and_log("[DailyLoadProfile] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node.daily_load_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[DailyLoadProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node.daily_load_profile.last_packet_time = std::chrono::system_clock::now();
        node.daily_load_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[DailyLoadProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
    }

    // if (resp->pmesh.remaining_pkt_count == 0x00)
    if (resp->dlms.next_page_status == 0x00)
    {
        int received = node.daily_load_profile.total_packets_received;
        // int expected = resp->pmesh.current_pkt_count;
        int expected = resp->dlms.current_page_index + 1;

        this->print_and_log("[DailyLoadProfile] Node %s → Packets Received: %d, Expected: %d\n", Utility::mac_to_string(mac.data()).c_str(), received, expected);

        if (received == expected)
        {
            this->printRecords(node.daily_load_profile.profile_data);
            this->print_and_log("[DailyLoadProfile] Node %s → OK (Updating DB)\n", Utility::mac_to_string(mac.data()).c_str());
            this->insert_daily_load_profile_data(mac, gateway_id, node.daily_load_profile, 1);
            node.daily_load_profile.clear();
            this->print_and_log("[DailyLoadProfile] Node %s → Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            node.daily_load_profile.clear();
            this->print_and_log("[DailyLoadProfile] Node %s →  Corrupted/Partial -> Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

void PushData::process_BLP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];

    this->print_and_log("[BlockLoadProfile] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseBlockLoadDLMSRecords(v, resp->dlms.no_of_records, node.block_load_profile);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[BlockLoadProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node.block_load_profile.last_packet_time = std::chrono::system_clock::now();
        node.block_load_profile.total_packets_received++;
    }
    else
    {
        this->print_and_log("[BlockLoadProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
    }

    // if (resp->pmesh.remaining_pkt_count == 0x00)
    if (resp->dlms.next_page_status == 0x00)
    {
        int received = node.block_load_profile.total_packets_received;
        // int expected = resp->pmesh.current_pkt_count;
        int expected = resp->dlms.current_page_index + 1;

        this->print_and_log("[BlockLoadProfile] Node %s → Packets Received: %d, Expected: %d\n", Utility::mac_to_string(mac.data()).c_str(), received, expected);

        if (received == expected)
        {
            for (size_t i = 0; i < node.block_load_profile.profiles_data.size(); i++)
            {
                this->print_and_log("----- BLP RECORD %zu -----\n", i + 1);
                this->printRecords(node.block_load_profile.profiles_data[i]);
            }
            this->print_and_log("[BlockLoadProfile] Node %s → OK (Updating DB)\n", Utility::mac_to_string(mac.data()).c_str());
            int cycle_id = this->calculate_cycle_id_for_block_load();
            this->insert_block_load_profile_data(mac, gateway_id, cycle_id, node.block_load_profile, 1);
            node.block_load_profile.clear();
            this->print_and_log("[BlockLoadProfile] Node %s → Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            node.block_load_profile.clear();
            this->print_and_log("[BlockLoadProfile] Node %s →  Corrupted/Partial -> Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

void PushData::process_BHP_push_data(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];

    this->print_and_log("[BillingProfile] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node.billing_history);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[BillingProfile] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node.billing_history.last_packet_time = std::chrono::system_clock::now();
        node.billing_history.total_packets_received++;
    }
    else
    {
        this->print_and_log("[BillingProfile] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
    }

    // if (resp->pmesh.remaining_pkt_count == 0x00)
    if (resp->dlms.next_page_status == 0x00)
    {
        int received = node.billing_history.total_packets_received;
        // int expected = resp->pmesh.current_pkt_count;
        int expected = resp->dlms.current_page_index + 1;

        this->print_and_log("[BillingProfile] Node %s → Packets Received: %d, Expected: %d\n", Utility::mac_to_string(mac.data()).c_str(), received, expected);

        if (received == expected)
        {
            this->printRecords(node.billing_history.profile_data);
            this->print_and_log("[BillingProfile] Node %s → OK (Updating DB)\n", Utility::mac_to_string(mac.data()).c_str());
            this->insert_billing_history_profile_data(mac, gateway_id, node.billing_history, 1);
            node.billing_history.clear();
            this->print_and_log("[BillingProfile] Node %s → Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            node.billing_history.clear();
            this->print_and_log("[BillingProfile] Node %s → Corrupted/partial -> Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

#if PUSH_EVENT_DETAILED_PARSING

void PushData::process_power_on_event(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];
    node.power_on_event.last_packet_time = std::chrono::system_clock::now();
    node.power_on_event.total_packets_received++;

    this->print_and_log("[PowerOnEvent] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    const uint8_t *p = resp->dlms.data;
    int total_number_of_records = resp->dlms.no_of_records;
    int current_number_of_record = 0;

    // ordinal index
    node.power_on_event.profile_data.ordinal_index_of_all_elements = *p;
    p += 1;

    // RTC
    {
        this->print_and_log("Record RTC\n");
        EventDlmsDataType<EventDlmsDataTypeOctetString> dt;
        dt.status = p[0];
        dt.data_type = p[1];
        dt.data.parse(p);
        node.power_on_event.profile_data.real_time_clock = dt;
        if (dt.data.length == 0)
        {
            this->print_and_log("The length of the record is zero\n");
            return;
        }
        this->print_and_log("RTC: ");
        for (int i = 0; i < dt.data.length; i++)
            this->print_and_log("%02X ", dt.data.data[i]);
        this->print_and_log("\n");
        p += dt.data.total_size();
        current_number_of_record++;
    }

    // Meter Serial Number
    {
        this->print_and_log("Record Meter serial number\n");
        EventDlmsDataType<EventDlmsDataTypeOctetString> dt;
        dt.status = p[0];
        dt.data_type = p[1];
        dt.data.parse(p);
        node.power_on_event.profile_data.meter_sl_number = dt;
        if (dt.data.length == 0)
        {
            this->print_and_log("The length of the record is zero\n");
            return;
        }
        this->print_and_log("Meter serial number: ");
        for (int i = 0; i < dt.data.length; i++)
            this->print_and_log("%02X ", dt.data.data[i]);
        this->print_and_log("\n");
        p += dt.data.total_size();
        current_number_of_record++;
    }

    // Device ID
    {
        this->print_and_log("Record Device ID\n");
        EventDlmsDataType<EventDlmsDataTypeOctetString> dt;
        dt.status = p[0];
        dt.data_type = p[1];
        dt.data.parse(p);
        node.power_on_event.profile_data.device_id = dt;
        if (dt.data.length == 0)
        {
            this->print_and_log("The length of the record is zero\n");
            return;
        }
        this->print_and_log("Device ID: ");
        for (int i = 0; i < dt.data.length; i++)
            this->print_and_log("%02X ", dt.data.data[i]);
        this->print_and_log("\n");
        p += dt.data.total_size();
        current_number_of_record++;
    }

    // Event Status Word
    {
        this->print_and_log("Record Event status word\n");
        EventDlmsDataType<EventDlmsDataTypeOctetString> dt;
        dt.status = p[0];
        dt.data_type = p[1];
        dt.data.parse(p);
        node.power_on_event.profile_data.event_status_word = dt;
        if (dt.data.length == 0)
        {
            this->print_and_log("The length of the record is zero\n");
            return;
        }
        this->print_and_log("Event status word: ");
        for (int i = 0; i < dt.data.length; i++)
            this->print_and_log("%02X ", dt.data.data[i]);
        this->print_and_log("\n");
        p += dt.data.total_size();
        current_number_of_record++;
    }

    // Voltage
    {
        this->print_and_log("Record Voltage\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.voltage = dt;
        this->print_and_log("Voltage: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    // Current
    {
        this->print_and_log("Record Current\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.current = dt;
        this->print_and_log("Current: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    // Power
    {
        this->print_and_log("Record Power\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.power = dt;
        this->print_and_log("Power: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    // Transaction
    {
        this->print_and_log("Record Transaction\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.transaction = dt;
        this->print_and_log("Transaction: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    // Other Event
    {
        this->print_and_log("Record Other Event\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.other_event = dt;
        this->print_and_log("Other Event: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    // Non-roll over
    {
        this->print_and_log("Record Non-roll over\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.non_roll_over = dt;
        this->print_and_log("Non-roll over: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    // Control
    {
        this->print_and_log("Record Control\n");
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_on_event.profile_data.control = dt;
        this->print_and_log("Control: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    this->print_and_log("Current number of records: %d - Total number of records: %d\n", current_number_of_record, total_number_of_records);
    if (current_number_of_record == total_number_of_records)
    {
        this->insert_power_on_event_to_db(mac, gateway_id, node.power_on_event);
        node.power_on_event.clear();
    }
}

void PushData::process_power_off_event(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];
    node.power_off_event.last_packet_time = std::chrono::system_clock::now();
    node.power_off_event.total_packets_received++;

    this->print_and_log("[PowerOffEvent] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    const uint8_t *p = resp->dlms.data;
    int total_number_of_records = resp->dlms.no_of_records;
    int current_number_of_record = 0;

    // ordinal index
    node.power_off_event.profile_data.ordinal_index_of_all_elements = *p;
    p += 1;

    // Meter Serial Number
    {
        EventDlmsDataType<EventDlmsDataTypeOctetString> dt;
        dt.status = p[0];
        dt.data_type = p[1];
        dt.data.parse(p);
        node.power_off_event.profile_data.meter_sl_number = dt;
        this->print_and_log("Meter serial number: ");
        for (int i = 0; i < dt.data.length; i++)
            this->print_and_log("%02X ", dt.data.data[i]);
        this->print_and_log("\n");
        p += dt.data.total_size();
        current_number_of_record++;
    }

    // Device ID
    {
        EventDlmsDataType<EventDlmsDataTypeOctetString> dt;
        dt.status = p[0];
        dt.data_type = p[1];
        dt.data.parse(p);
        node.power_off_event.profile_data.device_id = dt;
        this->print_and_log("Device ID: ");
        for (int i = 0; i < dt.data.length; i++)
            this->print_and_log("%02X ", dt.data.data[i]);
        this->print_and_log("\n");
        p += dt.data.total_size();
        current_number_of_record++;
    }

    // Power
    {
        EventDlmsDataType<uint16_t> dt;
        dt.parse(p);
        node.power_off_event.profile_data.power = dt;
        this->print_and_log("Power: %X\n", dt.data);
        p += dt.total_size();
        current_number_of_record++;
    }

    this->print_and_log("Current number of records: %d - Total number of records: %d\n", current_number_of_record, total_number_of_records);
    if (current_number_of_record == total_number_of_records)
    {
        this->insert_power_off_event_to_db(mac, gateway_id, node.power_off_event);
        node.power_off_event.clear();
    }
}

#else // PUSH_EVENT_DETAILED_PARSING

void PushData::process_power_on_event(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];

    this->print_and_log("[PowerOnEvent] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node.power_on_event);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[PowerOnEvent] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node.power_on_event.last_packet_time = std::chrono::system_clock::now();
        node.power_on_event.total_packets_received++;
    }
    else
    {
        this->print_and_log("[PowerOnEvent] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
    }

    if (resp->dlms.next_page_status == 0x00)
    {
        int received = node.power_on_event.total_packets_received;
        int expected = resp->dlms.current_page_index + 1;

        this->print_and_log("[PowerOnEvent] Node %s → Packets Received: %d, Expected: %d\n", Utility::mac_to_string(mac.data()).c_str(), received, expected);

        if (received == expected)
        {
            this->printRecords(node.power_on_event.profile_data);
            this->print_and_log("[PowerOnEvent] Node %s → OK (Updating DB)\n", Utility::mac_to_string(mac.data()).c_str());
            this->insert_power_on_event_to_db(mac, gateway_id, node.power_on_event);
            node.power_on_event.clear();
            this->print_and_log("[PowerOnEvent] Node %s → Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            node.power_on_event.clear();
            this->print_and_log("[PowerOnEvent] Node %s →  Corrupted/Partial -> Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

void PushData::process_power_off_event(uint8_t *buff, ssize_t length, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    auto *resp = reinterpret_cast<const PushDataResponse *>(buff);
    uint16_t dlms_data_length = ((resp->dlms.length[0] << 8) | (resp->dlms.length[1]));
    uint8_t received_checksum = buff[resp->pmesh.length];
    uint8_t calculated_checksum = this->calculate_checksum(buff + sizeof(PmeshPushResponse), dlms_data_length);
    this->print_and_log("Received Checksum: 0x%X - Calculated Checksum: 0x%X\n", received_checksum, calculated_checksum);
    if (received_checksum != calculated_checksum)
    {
        this->print_and_log("Checksum error\n");
        return;
    }
    std::array<uint8_t, 8> mac = to_mac_array(resp->pmesh.destination_addr);
    PushNodeInfo &node = this->push_node_info[mac];

    this->print_and_log("[PowerOffEvent] Node %s\n", Utility::mac_to_string(mac.data()).c_str());

    std::vector<uint8_t> v(buff, buff + length);
    int ret_val = this->parseDLMSRecords(v, resp->dlms.no_of_records, node.power_off_event);

    if (ret_val == SUCCESS)
    {
        this->print_and_log("[PowerOffEvent] Parsing SUCCESS for node %s\n", Utility::mac_to_string(mac.data()).c_str());
        node.power_off_event.last_packet_time = std::chrono::system_clock::now();
        node.power_off_event.total_packets_received++;
    }
    else
    {
        this->print_and_log("[PowerOffEvent] Parsing FAILED for node %s. Ignoring this packet.\n", Utility::mac_to_string(mac.data()).c_str());
    }

    if (resp->dlms.next_page_status == 0x00)
    {
        int received = node.power_off_event.total_packets_received;
        int expected = resp->dlms.current_page_index + 1;

        this->print_and_log("[PowerOffEvent] Node %s → Packets Received: %d, Expected: %d\n", Utility::mac_to_string(mac.data()).c_str(), received, expected);

        if (received == expected)
        {
            this->printRecords(node.power_off_event.profile_data);
            this->print_and_log("[PowerOffEvent] Node %s → OK (Updating DB)\n", Utility::mac_to_string(mac.data()).c_str());
            this->insert_power_off_event_to_db(mac, gateway_id, node.power_off_event);
            node.power_off_event.clear();
            this->print_and_log("[PowerOffEvent] Node %s → Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
        else
        {
            node.power_off_event.clear();
            this->print_and_log("[PowerOffEvent] Node %s →  Corrupted/Partial -> Cleared\n", Utility::mac_to_string(mac.data()).c_str());
        }
    }
}

#endif // #if PUSH_EVENT_DETAILED_PARSING
