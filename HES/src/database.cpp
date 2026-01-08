#include "../inc/database.h"
#include "../inc/client.h"
#include <ctime>
#include <iomanip>
#include <mutex>
#include <sstream>

MySqlDatabase::MySqlDatabase()
{
    std::cout << "MySqlDatabase constructor called" << std::endl;

    mysql_thread_init();
}

// Thread-local RAII guard: ensures mysql_thread_init() is called once per
// thread that touches the MySQL C API and is paired with mysql_thread_end().
// This avoids crashes when multiple threads (e.g., mosquitto callbacks)
// invoke database methods.
namespace
{
thread_local bool mysql_thread_initialized = false;

struct MysqlThreadGuard
{
    bool owner{false};
    MysqlThreadGuard()
    {
        if (!mysql_thread_initialized)
        {
            // mysql_thread_init();
            mysql_thread_initialized = true;
            owner = true;
        }
    }
    ~MysqlThreadGuard()
    {
        if (owner)
        {
            // mysql_thread_end();
            mysql_thread_initialized = false;
        }
    }
};
} // namespace

MySqlDatabase::~MySqlDatabase()
{
    std::cout << "MySqlDatabase destructor called" << std::endl;

    if (this->mysql)
    {
        mysql_close(this->mysql);
        this->mysql = nullptr;
    }

    mysql_thread_end();
}

void MySqlDatabase::load_mysql_config_from_file(void)
{
    try
    {
        this->creds.host = Utility::readConfig<std::string>("MYSQL.connection.host");
        this->creds.port = Utility::readConfig<unsigned short>("MYSQL.connection.port");
        this->creds.database = Utility::readConfig<std::string>("MYSQL.credentials.database");
        this->creds.username = Utility::readConfig<std::string>("MYSQL.credentials.username");
        this->creds.password = Utility::readConfig<std::string>("MYSQL.credentials.password");
    }

    catch (const std::exception &e)
    {
        this->print_and_log("Error: %s\n", e.what());
    }

    this->print_and_log("mysql_port: %d\n", this->creds.port);
    this->print_and_log("mysql_server_ip_address: %s\n", this->creds.host.c_str());
    this->print_and_log("mysql_server_database_name: %s\n", this->creds.database.c_str());
    this->print_and_log("mysql_server_username: %s\n", this->creds.username.c_str());
    this->print_and_log("mysql_server_password: %s\n", this->creds.password.c_str());

    this->connect_to_mysql();
}

bool MySqlDatabase::connect_to_mysql()
{
    MysqlThreadGuard guard;
    std::lock_guard<std::mutex> lock(this->mysql_mutex);
    this->print_and_log("Connecting to MySQL server at %s:%d ...\n", this->creds.host.c_str(), this->creds.port);

    if (this->mysql)
    {
        mysql_close(this->mysql);
    }

    this->mysql = mysql_init(nullptr);
    if (!this->mysql)
    {
        this->print_and_log("MySQL init failed\n");
        return false;
    }

    int value = 1;
    uint32_t timeout_val = 10;
    mysql_options(this->mysql, MYSQL_OPT_SSL_MODE, &value);
    mysql_options(this->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_val);
    mysql_options(this->mysql, MYSQL_OPT_READ_TIMEOUT, &timeout_val);
    mysql_options(this->mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout_val);

    if (!mysql_real_connect(this->mysql, this->creds.host.c_str(), this->creds.username.c_str(), this->creds.password.c_str(), this->creds.database.c_str(), this->creds.port, nullptr, 0))
    {
        this->print_and_log("MySQL connection failed: %s\n", mysql_error(this->mysql));
        mysql_close(this->mysql);
        this->mysql = nullptr;
        return false;
    }

    this->print_and_log("MySQL connection established\n");
    return true;
}

bool MySqlDatabase::check_and_reconnect()
{
    int max_retries = 3;

    MysqlThreadGuard guard;

    bool need_reconnect = false;
    {
        std::lock_guard<std::mutex> lock(this->mysql_mutex);
        need_reconnect = (!this->mysql || mysql_ping(this->mysql));
    }

    if (need_reconnect)
    {
        this->print_and_log("MySQL connection lost. Attempting to reconnect...\n");

        for (int attempt = 1; attempt <= max_retries; attempt++)
        {
            this->print_and_log("Reconnection attempt %d of %d ...\n", attempt, max_retries);

            // Ensure we don't leave a dangling MYSQL* open
            {
                std::lock_guard<std::mutex> lock(this->mysql_mutex);
                if (this->mysql)
                {
                    mysql_close(this->mysql);
                    this->mysql = nullptr;
                }
            }

            sleep(attempt);

            if (connect_to_mysql())
            {
                this->print_and_log("Reconnection successful!\n");
                return true;
            }
        }

        this->print_and_log("Max reconnection attempts reached. Giving up!\n");
        return false;
    }
    return true;
}

int MySqlDatabase::execute_query(char *query)
{
    this->print_and_log("Query: %s\n", query);
    MysqlThreadGuard guard;

    if (!check_and_reconnect())
    {
        this->print_and_log("MySQL not connected. Query cannot be executed.\n");
        return FAILURE;
    }

    {
        std::lock_guard<std::mutex> lock(this->mysql_mutex);
        if (mysql_query(this->mysql, query) == 0)
        {
            this->print_and_log("Query executed successfully.\n");
            return SUCCESS;
        }

        uint32_t err = mysql_errno(this->mysql);
        this->print_and_log("Query failed (Error %u): %s\n", err, mysql_error(this->mysql));

        if (err == CR_SERVER_LOST || err == CR_SERVER_GONE_ERROR)
        {
            this->print_and_log("Connection lost. Attempting to reconnect...\n");
        }
    }

    // If we get here and the error was server lost, try to reconnect and re-execute
    if (check_and_reconnect())
    {
        std::lock_guard<std::mutex> lock(this->mysql_mutex);
        if (mysql_query(this->mysql, query) == 0)
        {
            this->print_and_log("Query re-executed successfully after reconnect.\n");
            return SUCCESS;
        }
    }

    this->print_and_log("Reconnection or re-execution failed.\n");
    return FAILURE;
}

int MySqlDatabase::Update_dlms_on_demand_request_status(unsigned int req_id, RequestStatus status, uint16_t err_code)
{
    char query[128] = {0};
    std::string download_time;
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    download_time = oss.str();
    // Construct SQL UPDATE query to set the status for the given request_id
    snprintf(query, sizeof(query),
             "UPDATE dlms_on_demand_request SET status='%d',error_code = %d,download_time = '%s' WHERE request_id=%u;",
             static_cast<int>(status), err_code, download_time.c_str(), req_id);

    // Execute the update query
    bool ret = this->execute_query(query);

    // Log the execution result
    this->print_and_log("Update_dlms_on_demand_request_status Result: %s\n", (ret == SUCCESS ? "SUCCESS" : "FAILURE"));

    return ret;
}

int MySqlDatabase::Update_dlms_on_demand_Ping_request_status(unsigned int req_id, RequestStatus status)
{
    char query[128] = {0};
    std::string download_time;
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    download_time = oss.str();
    // Construct SQL UPDATE query to set the status for the given request_id
    snprintf(query, sizeof(query),
             "UPDATE dlms_on_demand_ping_request SET status='%d',last_download_time = '%s' WHERE request_id=%u;",
             static_cast<int>(status), download_time.c_str(), req_id);

    // Execute the update query
    bool ret = this->execute_query(query);

    // Log the execution result
    this->print_and_log("Update_dlms_on_demand_Ping_request_status Result: %s\n", (ret == SUCCESS ? "SUCCESS" : "FAILURE"));

    return ret;
}

bool MySqlDatabase::check_path_in_source_route_network(const std::vector<std::string> &parts, int request_id)
{
    if (parts[3].length() < 32)
    {
        this->print_and_log("❌ PATH TOO SHORT: '%s' (len=%zu) < 32\n", parts[3].c_str(), parts[3].length());
        this->Update_dlms_on_demand_request_status(request_id, FAILED_PMESH_ERROR, 0);
        return false;
    }
    // Extract substring skipping first 16 bytes (32 hex chars)
    std::string path_hex = parts[3].substr(16);

    this->print_and_log("🔍 FULL_PATH='%s' → PATH='%s' (len=%zu)\n",
                        parts[3].c_str(), path_hex.c_str(), path_hex.length());

    // Convert to uppercase for consistent DB query(string to uppercase hex format)
    std::transform(path_hex.begin(), path_hex.end(), path_hex.begin(), ::toupper);

    // Build and log query
    char check_query[128];
    snprintf(check_query, sizeof(check_query),
             "SELECT path from source_route_network where disconnected_from_gateway = '0';");

    int query_result = MySqlDatabase::execute_query(check_query);

    if (query_result == FAILURE)
    {
        this->print_and_log("[EXECUTE QUERY FAILED]\n");
        this->Update_dlms_on_demand_request_status(request_id, FAILED_PMESH_ERROR, 0);
        return false;
    }
    MYSQL_ROW column;
    MYSQL_RES *result = mysql_store_result(this->mysql);
    while ((column = mysql_fetch_row(result)) != nullptr)
    {
        if (!memcmp(path_hex.c_str(), column[0], path_hex.length()))
        {
            this->print_and_log(" PATH FOUND | REQ_ID=%d | PATH='%s'\n", request_id, path_hex.c_str());
            return true;
        }
    }
    this->print_and_log("❌ PATH NOT FOUND | REQ_ID=%d | PATH='%s'\n", request_id, path_hex.c_str());
    return false;
}

/**
 * @brief Checks if NamePlate (NP) data is available for GATEWAY and queues missing meters
 *
 * Queries source_route_network → cross-checks name_plate_data → queues missing NPs to hesNPDataQueue
 * Extracts mesh path by skipping every other 8 bytes from path_data (copy 8/skip 8 pattern).
 *
 * @param gateway_id GATEWAY identifier string
 * @return 1=NP queued for pull, 0=no new data needed, FAILURE=DB error
 */
int MySqlDatabase::is_NP_data_available(char *gateway_id)
{
    this->print_and_log("%s Start\n", __FUNCTION__); // Function entry log

    if (!gateway_id) // Validate GATEWAY ID parameter
    {
        this->print_and_log("%s: null gateway_id\n", __FUNCTION__);
        return FAILURE; // Null GATEWAY ID → early exit
    }

    char query_buffer[256];       // Query 1 buffer: source routes
    char query_buffer2[256];      // Query 2 buffer: nameplate check
    MYSQL_RES *result = nullptr;  // Result set 1: source_route_network
    MYSQL_RES *result2 = nullptr; // Result set 2: name_plate_data
    // MYSQL_ROW row2;               // Row iterator for nameplate data

    // Query 1: fetch nomber of name plate count for gateway
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT COUNT(*) FROM name_plate_data WHERE gateway_id='%s' and meter_mac_address='%s';",
             gateway_id, gateway_id);

    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Query executed Failed.\n");
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);
    if (result && mysql_num_rows(result) > 0)
    {
        MYSQL_ROW row = mysql_fetch_row(result);
        int row_count = atoi(row[0]);
        mysql_free_result(result);

        if (row_count > 0)
        {
            this->print_and_log("✅ NamePlate data EXISTS for gateway = %s\n", gateway_id);
        }
        else
        {
            this->print_and_log("❌ No NamePlate data found for Gateway \n");

            // Fetch last 8 bytes of gateway and enqueue
            dest_address.clear(); // Clear global destination address buffer

            size_t total_length = 16; // Total raw path length in bytes

            size_t copy_start = 8; // Skip initial 8 bytes (likely source MAC/header)

            while (copy_start + 8 <= total_length)
            {
                // Copy next 8 bytes after skipping previous 8
                for (size_t i = 0; i < 8; i++)
                {
                    dest_address.push_back(gateway_id[copy_start + i]);
                }
                copy_start += 16; // Skip next 8 bytes after copying 8 bytes
            }
            dest_address.push_back(0); // Null terminate extracted path

            // Enqueue Gateway details to pull
            auto qsize = this->hesNPDataQueue.size(); // Check NamePlate queue size

            if (qsize >= 50) // Queue full (max 10) → drop oldest entry
            {
                this->print_and_log("HES data queue full, dropping oldest packet\n");
                this->hesNPDataQueue.pop();
            }

            // Queue for NamePlate pull: MAC(16), path_len, path, hop_count
            this->hesNPDataQueue.emplace((uint8_t *)gateway_id, 16, (uint8_t *)dest_address.data(), 8, 0);
        }
    }

    std::queue<silenceND> temp_queue = MySqlDatabase::pathSrcRouteQueue; // Copy queue again (original unchanged)

    while (!temp_queue.empty())
    {

        silenceND &router_mac_add = temp_queue.front(); // Get front queue element (meter record)

        // Prepare null-terminated MAC string from queued binary/vector data
        std::string queued_mac_str(reinterpret_cast<const char *>(router_mac_add.meter_mac_address.data()), router_mac_add.meter_mac_address.size());

        // Query 2: Check if NamePlate data EXISTS for this GATEWAY
        snprintf(query_buffer2, sizeof(query_buffer2), "select COUNT(*) from name_plate_data where gateway_id ='%s' and meter_mac_address = '%s';", gateway_id, queued_mac_str.c_str());
        if (this->execute_query(query_buffer2) == FAILURE)
        {
            this->print_and_log("Query execution Failed.\n");
            continue;
        }

        result2 = mysql_store_result(this->mysql); // Store nameplate results
        if (result2 != nullptr)
        {
            MYSQL_ROW row = mysql_fetch_row(result2);
            int row_count = atoi(row[0]);
            mysql_free_result(result2);

            if (row_count > 0)
            {
                this->print_and_log("✅ NP data found for (%.*s)\n",
                                    static_cast<int>(router_mac_add.meter_mac_address.size()), (char *)router_mac_add.meter_mac_address.data());
            }
            else
            {
                this->print_and_log("❌ NP data not found\n");

                auto qsize = this->hesNPDataQueue.size(); // Check NamePlate queue size

                if (qsize >= 50) // Queue full (max 10) → drop oldest entry
                {
                    this->print_and_log("HES data queue full, dropping oldest packet\n");
                    this->hesNPDataQueue.pop();
                }

                // Queue for NamePlate pull: MAC(16), path_len, path, hop_count
                this->hesNPDataQueue.emplace(router_mac_add.meter_mac_address.data(), 16, router_mac_add.path_record.data(), router_mac_add.hop_count * 8, router_mac_add.hop_count);

                // Print safely using explicit lengths because these fields may contain non-null-terminated/binary data
                this->print_and_log("Queued [%.*s], path[%.*s] hop count[%d]\n",
                                    16, (char *)router_mac_add.meter_mac_address.data(),
                                    static_cast<int>(router_mac_add.path_record.size()), (char *)router_mac_add.path_record.data(),
                                    router_mac_add.hop_count);
            }
        }
        temp_queue.pop(); // Remove processed queue element
    }
    return SUCCESS; // 1=queued, 0=no new data, FAILURE=error
}

int MySqlDatabase::get_meter_details_from_db(const std::string &manufacturer, const std::string &meter_firmware_version, char *gateway_id)
{
    this->print_and_log("%s Start\n", __FUNCTION__); // Function entry log

    char query_buffer[512] = {0}; // Query buffer

    // Query meter_details for given MAC + GATEWAY
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT meter_address FROM meter_details WHERE meter_manufacture_name = '%s' AND meter_firmware_version = '%s' and gateway_id ='%s';",
             manufacturer.c_str(),
             meter_firmware_version.c_str(), gateway_id);

    if (this->execute_query(query_buffer) == FAILURE) // Execute query (non-zero = FAILURE)
    {
        return FAILURE;
    }

    MYSQL_ROW row;     // Row iterator
    MYSQL_RES *result; // Query result set

    result = mysql_store_result(this->mysql);

    if (result != nullptr)
    {
        if (mysql_num_rows(result) > 0)
        {
            Client *caller = dynamic_cast<Client *>(this);
            while ((row = mysql_fetch_row(result)) != nullptr) // Fetch rows
            {
                this->print_and_log("meter_serial_number [%s],manufacturer [%s], firmware_version [%s]\n", row[0], manufacturer.c_str(), meter_firmware_version.c_str());
                if (caller)
                {
                    caller->addMeter(row[0], manufacturer.c_str(), meter_firmware_version.c_str());
                }
                else
                {
                    this->print_and_log("get_meter_details_from_db: not called on a Client instance, skipping addMeter\n");
                }
            }

            mysql_free_result(result); // Cleanup result set
            return SUCCESS;            // Meter details found
        }
        mysql_free_result(result); // Cleanup result set
    }

    return FAILURE; // Meter details not found
}

int MySqlDatabase::get_alternate_source_route_network_from_db(uint8_t *router_mac_address, char *gateway_id)
{
    router_mac_address[16] = '\0'; // Ensure input MAC is null-terminated
    this->print_and_log("Get Alternate Path for (%s)\n", router_mac_address);

    int num_row = 0;   // Number of alternate route records found
    MYSQL_ROW row;     // Row iterator
    MYSQL_RES *result; // Query result set

    int return_value = FAILURE;    // Default: no alternate path available
    char query_buffer[4096] = {0}; // Large buffer for path-containing query

    // check if already there in queue
    std::queue<path_data_record> temp_queue = MySqlDatabase::hesATPathQueue; // Copy queue

    bool already_queued = false;

    while (!temp_queue.empty())
    {
        auto &queued_record = temp_queue.front();

        // Find actual string lengths first
        size_t md_len = strnlen((char *)router_mac_address, 64);
        // size_t queued_len = queued_record.meter_manufacture_name.size();

        if (memcmp(queued_record.router_mac_address.data(), router_mac_address, md_len) == 0)
        {
            this->print_and_log("alter path for '%s' already queued - SKIPPING\n", router_mac_address);
            already_queued = true;
            break;
        }
        temp_queue.pop();
    }

    if (already_queued)
    {
        return SUCCESS; // Skip to next meter
    }

    // Query alternate routes matching router MAC + GATEWAY
    sprintf(query_buffer, "select gateway_id, target_mac_address, hop_count, path from alternate_source_route_network where target_mac_address='%s' and gateway_id = '%s' LIMIT 2;", router_mac_address, gateway_id);

    if (this->execute_query(query_buffer)) // Execute query (non-zero = FAILURE)
    {
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);

    if (result != nullptr)
    {
        num_row = mysql_num_rows(result); // Count matching alternate routes

        if (num_row > 0)
        {
            while ((row = mysql_fetch_row(result)) != nullptr)
            {
                uint8_t gateway_id[17] = {0};         // GATEWAY ID from alternate route record
                uint8_t target_mac_address[17] = {0}; // Target MAC from record
                uint8_t hop_count = 0;                // Hop count from record
                uint8_t path_data[512] = {0};         // Raw path data from record

                if (!(row[0]) || !(row[1])) // Skip incomplete records
                {
                    continue;
                }
                hop_count = static_cast<uint8_t>(atoi(row[2])); // Parse hop count (col 2)
                memcpy(&gateway_id[0], (uint8_t *)row[0], 16);  // Copy GATEWAY ID (col 0)
                gateway_id[16] = '\0';

                memcpy(&target_mac_address[0], (uint8_t *)row[1], 16); // Copy target MAC (col 1)
                target_mac_address[16] = '\0';

                memcpy(&path_data[0], (uint8_t *)row[3], hop_count * 16); // Copy path data (col 3)
                path_data[hop_count * 16] = '\0';

                unsigned long *lengths = mysql_fetch_lengths(result);
                size_t path_bytes = (lengths && lengths[3]) ? static_cast<size_t>(lengths[3]) : 0;

                dest_address.clear(); // Clear destination path buffer

                size_t total_length = hop_count * 16; // Total raw path length

                size_t copy_start = 8; // Skip first 8 bytes

                // Path extraction: copy 8 bytes, skip 8 bytes pattern
                while (copy_start + 8 <= total_length)
                {
                    // Copy next 8 bytes after skipping previous 8
                    for (size_t i = 0; i < 8; i++)
                    {
                        dest_address.push_back(path_data[copy_start + i]);
                    }
                    copy_start += 16; // Advance by 16 (8 copy + 8 skip)
                }

                dest_address.push_back(0); // Null terminate extracted path

                if (path_bytes > 16) // Valid path length detected
                {
                    auto qsize = this->hesATPathQueue.size(); // Check alternate path queue

                    if (qsize >= 10) // Queue full → drop oldest
                    {
                        this->print_and_log("HES data queue full, dropping oldest packet\n");
                        this->hesATPathQueue.pop();
                    }

                    // Queue alternate path: MAC(16), path(16B/hop), hops
                    this->hesATPathQueue.emplace(target_mac_address, 16, (uint8_t *)dest_address.data(), hop_count * 16, hop_count);

                    this->print_and_log("Queued alternate path for target_mac_address: %s, path: %s, hop count: %d\n", target_mac_address, (uint8_t *)dest_address.data(), hop_count);
                    return_value = 1; // Success: alternate path queued
                }
            }
        }
        else
        {
            this->print_and_log("No Alternate Path Found For (%s)\n", router_mac_address);
            return FAILURE;
        }

        mysql_free_result(result); // Cleanup result set
    }

    return return_value;
}

int MySqlDatabase::insert_name_plate_db(const char *target_mac_address, const char *gateway_id, const ODM_NamePlateProfile name_plate, std::string &time_str)
{
    char query_buffer[2048] = {0}; // Larger buffer for safety

    // PHASE 1: Insert/Update name_plate_data (ALWAYS insert)
    snprintf(query_buffer, sizeof(query_buffer),
             "INSERT INTO name_plate_data "
             "(meter_mac_address, gateway_id, meter_serial_number, device_id, "
             "manufacturer_name, firmware_version_for_meter, meter_type, "
             "category, current_rating, meter_manufacture_year, "
             "last_download_time, push_alaram) "
             "VALUES ('%s','%s','%s','%s','%s','%s','%d','%s','%s','%s','%s','%d')",
             target_mac_address, gateway_id,
             std::string((char *)name_plate.data.at(NP_METER_SERIALNUMBER)[0].getOctetString().data(), name_plate.data.at(NP_METER_SERIALNUMBER)[0].getOctetString().size()).c_str(),
             std::string((char *)name_plate.data.at(NP_DEVICE_ID)[0].getOctetString().data(), name_plate.data.at(NP_DEVICE_ID)[0].getOctetString().size()).c_str(),
             std::string((char *)name_plate.data.at(NP_MANUFACTURE_NAME)[0].getOctetString().data(), name_plate.data.at(NP_MANUFACTURE_NAME)[0].getOctetString().size()).c_str(),
             std::string((char *)name_plate.data.at(NP_METER_FIRMWARE)[0].getOctetString().data(), name_plate.data.at(NP_METER_FIRMWARE)[0].getOctetString().size()).c_str(),
             static_cast<int>(name_plate.data.at(NP_METER_PHASE)[0].getAsFloat(1.0)) / 6,
             std::string((char *)name_plate.data.at(NP_CATEGORY)[0].getOctetString().data(), name_plate.data.at(NP_CATEGORY)[0].getOctetString().size()).c_str(),
             std::string((char *)name_plate.data.at(NP_CURRENT_RATING)[0].getOctetString().data(), name_plate.data.at(NP_CURRENT_RATING)[0].getOctetString().size()).c_str(),
             name_plate.data.at(NP_MANUFACTURE_YEAR)[0].to_string().c_str(),
             time_str.c_str(), 0);

    if (execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("❌ Failed to execute INSERT INTO name_plate_data\n");
        return FAILURE;
    }
    memset(query_buffer, 0, sizeof(query_buffer));

    // PHASE 2: Check meter_details existence
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT COUNT(*) FROM meter_details WHERE gateway_id='%s' AND meter_mac_address='%s';", gateway_id, target_mac_address);

    if (execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("❌ Failed to execute meter_details COUNT query\n");
        return FAILURE;
    }

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (result != nullptr)
    {
        MYSQL_ROW row = mysql_fetch_row(result);
        int row_count = atoi(row[0]);
        mysql_free_result(result);

        if (row_count > 0)
        {
            // UPDATE existing meter_details
            this->print_and_log("🔄 Updating meter_details for MAC: %s\n", target_mac_address);
            snprintf(query_buffer, sizeof(query_buffer),
                     "UPDATE meter_details SET "
                     "gateway_id='%s', meter_mac_address='%s', device_id='%s', "
                     "meter_manufacture_name='%s', meter_firmware_version='%s', "
                     "meter_phase='%d', category='%s', current_rating='%s', "
                     "manufacture_year='%s', last_download_time='%s' "
                     "WHERE gateway_id='%s' AND meter_mac_address='%s';",
                     gateway_id, target_mac_address,
                     std::string((char *)name_plate.data.at(NP_DEVICE_ID)[0].getOctetString().data(), name_plate.data.at(NP_DEVICE_ID)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_MANUFACTURE_NAME)[0].getOctetString().data(), name_plate.data.at(NP_MANUFACTURE_NAME)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_METER_FIRMWARE)[0].getOctetString().data(), name_plate.data.at(NP_METER_FIRMWARE)[0].getOctetString().size()).c_str(),
                     static_cast<int>(name_plate.data.at(NP_METER_PHASE)[0].getAsFloat(1.0)) / 6,
                     std::string((char *)name_plate.data.at(NP_CATEGORY)[0].getOctetString().data(), name_plate.data.at(NP_CATEGORY)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_CURRENT_RATING)[0].getOctetString().data(), name_plate.data.at(NP_CURRENT_RATING)[0].getOctetString().size()).c_str(),
                     name_plate.data.at(NP_MANUFACTURE_YEAR)[0].to_string().c_str(),
                     time_str.c_str(),
                     gateway_id, target_mac_address);

            if (execute_query(query_buffer) == FAILURE)
            {
                this->print_and_log("❌ Failed to UPDATE meter_details\n");
                return FAILURE;
            }
        }
        else
        {
            // INSERT new meter_details
            this->print_and_log("➕ Inserting new meter_details for MAC: %s\n", target_mac_address);
            snprintf(query_buffer, sizeof(query_buffer),
                     "INSERT INTO meter_details "
                     "(gateway_id, meter_mac_address, meter_address, device_id, "
                     "meter_manufacture_name, meter_firmware_version, meter_phase, "
                     "category, current_rating, manufacture_year, last_download_time) "
                     "VALUES ('%s','%s','%s','%s','%s','%s','%d','%s','%s','%s','%s')",
                     gateway_id, target_mac_address,
                     std::string((char *)name_plate.data.at(NP_METER_SERIALNUMBER)[0].getOctetString().data(), name_plate.data.at(NP_METER_SERIALNUMBER)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_DEVICE_ID)[0].getOctetString().data(), name_plate.data.at(NP_DEVICE_ID)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_MANUFACTURE_NAME)[0].getOctetString().data(), name_plate.data.at(NP_MANUFACTURE_NAME)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_METER_FIRMWARE)[0].getOctetString().data(), name_plate.data.at(NP_METER_FIRMWARE)[0].getOctetString().size()).c_str(),
                     static_cast<int>(name_plate.data.at(NP_METER_PHASE)[0].getAsFloat(1.0)) / 6,
                     std::string((char *)name_plate.data.at(NP_CATEGORY)[0].getOctetString().data(), name_plate.data.at(NP_CATEGORY)[0].getOctetString().size()).c_str(),
                     std::string((char *)name_plate.data.at(NP_CURRENT_RATING)[0].getOctetString().data(), name_plate.data.at(NP_CURRENT_RATING)[0].getOctetString().size()).c_str(),
                     name_plate.data.at(NP_MANUFACTURE_YEAR)[0].to_string().c_str(),
                     time_str.c_str());

            if (execute_query(query_buffer) == FAILURE)
            {
                this->print_and_log("❌ Failed to INSERT meter_details\n");
                return FAILURE;
            }
        }
    }
    else
    {
        this->print_and_log("❌ No result from meter_details query\n");
        return FAILURE;
    }

    this->print_and_log("✅ Name plate data saved successfully for %s\n", target_mac_address);
    return SUCCESS;
}

int MySqlDatabase::check_for_scalar_profile(char *gateway_id)
{
    this->print_and_log("%s Start\n", __FUNCTION__);

    char query_buffer[512];
    MYSQL_RES *result = nullptr;
    MYSQL_RES *result2 = nullptr;
    int return_value = 0; // 1=missing profile queued

    //  Query 1: Get ALL meter details for this GATEWAY
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT meter_manufacture_name, meter_mac_address, meter_phase, meter_firmware_version "
             "FROM meter_details "
             "WHERE gateway_id = '%s';",
             gateway_id);

    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Query 1 failed\n");
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);
    if (!result)
    {
        this->print_and_log("mysql_store_result failed for Query 1\n");
        return FAILURE;
    }

    //  Process each meter
    MYSQL_ROW row;
    MYSQL_RES *local_result = nullptr;

    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        if (!row[0] || !row[1] || !row[2] || !row[3])
        {
            continue; // Skip incomplete records
        }
        char local_query_buffer[512];
        MYSQL_ROW local_row;
        uint8_t meter_manufacture_name_md[65] = {0};
        uint8_t meter_mac_address[17] = {0};
        uint8_t meter_fw_version[65] = {0};
        uint8_t meter_phase = atoi(row[2]);

        memcpy(meter_manufacture_name_md, row[0], strnlen(row[0], 64));
        meter_manufacture_name_md[64] = '\0';

        memcpy(meter_fw_version, row[3], strnlen(row[3], 64));
        meter_fw_version[64] = '\0';

        memcpy(meter_mac_address, row[1], strnlen(row[1], 16));
        meter_mac_address[16] = '\0';

        this->print_and_log("Processing: %s | %s | Phase=%d\n",
                            meter_manufacture_name_md, meter_mac_address, meter_phase);

        //  Check queue (avoid duplicates)
        bool already_queued = false;

        bool meter_connected = false;

        //  Check source_route_network for disconnection status
        snprintf(local_query_buffer, sizeof(local_query_buffer),
                 "select disconnected_from_gateway from source_route_network where target_mac_address = '%s' and gateway_id = '%s';",
                 meter_mac_address, gateway_id);

        if (this->execute_query(local_query_buffer) == FAILURE)
        {
            this->print_and_log("Fallback query failed\n");
            return FAILURE;
        }
        local_result = mysql_store_result(this->mysql);
        if (!local_result)
        {
            this->print_and_log("mysql_store_result failed for Query 1\n");
            return FAILURE;
        }
        if (mysql_num_rows(local_result) == 0)
        {
            mysql_free_result(local_result);
            local_result = nullptr;
            snprintf(local_query_buffer, sizeof(local_query_buffer),
                     "select status from gateway_status_info where gateway_id = '%s';",
                     gateway_id);

            if (this->execute_query(local_query_buffer) == FAILURE)
            {
                this->print_and_log("Fallback query failed\n");
                continue; // Skip disconnected meters
            }
            local_result = mysql_store_result(this->mysql);
            if ((local_row = mysql_fetch_row(local_result)))
            {
                meter_connected = true;
                if (local_row[0] && atoi(local_row[0]) == 0)
                {
                    this->print_and_log("Gateway %s is disconnected - SKIP\n", gateway_id);
                    mysql_free_result(local_result);
                    continue; // Skip disconnected meters
                }
                mysql_free_result(local_result);
            }
        }

        if (!meter_connected)
        {
            if ((local_row = mysql_fetch_row(local_result)))
            {
                if (local_row[0] && atoi(local_row[0]) == 1)
                {
                    this->print_and_log("Meter %s is disconnected from gateway %s - SKIP\n", meter_mac_address, gateway_id);
                    mysql_free_result(local_result);
                    continue; // Skip disconnected meters
                }
                mysql_free_result(local_result);
            }
        }

        std::queue<meter_details> temp_queue = MySqlDatabase::meterDetailsQueue;
        while (!temp_queue.empty())
        {
            meter_details &q = temp_queue.front();
            if (strncmp((char *)q.meter_manufacture_name.data(), (char *)meter_manufacture_name_md, 64) == 0 &&
                strncmp((char *)q.meter_fw_version.data(), (char *)meter_fw_version, 64) == 0)
            {
                already_queued = true;
                break;
            }
            temp_queue.pop();
        }

        if (already_queued)
        {
            this->print_and_log("Manufacturer '%s' already queued - SKIP\n", meter_manufacture_name_md);
            continue;
        }

        //  Query 2: Check scalar attributes
        snprintf(query_buffer, sizeof(query_buffer),
                 "SELECT attribute_id FROM meter_supported_attributes "
                 "WHERE manufacturer_name='%s' AND firmware_version='%s'",
                 meter_manufacture_name_md, meter_fw_version);

        if (this->execute_query(query_buffer) == FAILURE)
        {
            this->print_and_log("Attribute query failed\n");
            continue;
        }

        result2 = mysql_store_result(this->mysql);
        if (!result2) continue;

        // Parse attributes
        bool has_ip = false, has_bh = false, has_dlp = false, has_blp = false;
        MYSQL_ROW row2;
        while ((row2 = mysql_fetch_row(result2)) != nullptr)
        {
            if (!row2[0]) continue;

            unsigned int attr_id = 0;
            sscanf(row2[0], "%x", &attr_id);

            switch (attr_id)
            {
                case attribute_id_ip:
                    has_ip = true;
                    break;
                case attribute_id_bh:
                    has_bh = true;
                    break;
                case attribute_id_dlp:
                    has_dlp = true;
                    break;
                case attribute_id_blp:
                    has_blp = true;
                    break;
            }
        }

        mysql_free_result(result2); // ✅ Cleanup

        //  Queue missing profiles
        if (!has_ip)
        {
            this->enqueue_info(meter_manufacture_name_md, 1, meter_mac_address, meter_phase, gateway_id, meter_fw_version);
            return_value = 1;
        }
        if (!has_bh)
        {
            this->enqueue_info(meter_manufacture_name_md, 2, meter_mac_address, meter_phase, gateway_id, meter_fw_version);
            return_value = 1;
        }
        if (!has_dlp)
        {
            this->enqueue_info(meter_manufacture_name_md, 3, meter_mac_address, meter_phase, gateway_id, meter_fw_version);
            return_value = 1;
        }
        if (!has_blp)
        {
            this->enqueue_info(meter_manufacture_name_md, 4, meter_mac_address, meter_phase, gateway_id, meter_fw_version);
            return_value = 1;
        }
    }

    //  FINAL CLEANUP
    if (result) mysql_free_result(result);

    this->print_and_log("%s End → return %d\n", __FUNCTION__, return_value);
    return return_value;
}

bool MySqlDatabase::enqueue_info(uint8_t *meter_manufacture_name, uint16_t attribute_id, uint8_t *meter_mac_address, uint8_t meter_phase, char *gateway_id, uint8_t *meter_fw_version)
{
    this->print_and_log("%s: MAC=%.*s attr=%04X\n", __FUNCTION__, 16, meter_mac_address, attribute_id);

    // ✅ ENFORCE queue limit
    if (this->meterDetailsQueue.size() >= 50)
    {
        this->print_and_log("Queue FULL (50/50) → drop oldest\n");
        this->meterDetailsQueue.pop();
    }

    std::vector<uint8_t> dest_address; // Extracted path
    uint8_t cached_hop_count = 0;
    bool use_cache = false;
    uint8_t path_record[65] = {0};

    // ✅ STEP 1: FAST QUEUE SCAN (microseconds)
    std::queue<meter_details> temp_queue = this->meterDetailsQueue;
    while (!temp_queue.empty())
    {
        const meter_details &entry = temp_queue.front();

        // Compare MAC (first 16 bytes)
        if (memcmp(entry.meter_mac_address.data(), meter_mac_address, 16) == 0)
        {
            // Copy cached path + hops
            dest_address.assign(entry.path_record.begin(), entry.path_record.end());
            cached_hop_count = entry.hop_count;
            use_cache = true;
            break;
        }
        temp_queue.pop();
    }

    if (!use_cache)
    {
        bool path_found = false;

        std::queue<silenceND> temp_queue2 = MySqlDatabase::pathSrcRouteQueue; // Copy queue again (original unchanged)

        while (!temp_queue2.empty())
        {

            silenceND &router_mac_add = temp_queue2.front(); // Get front queue element (meter record)

            if (memcmp((uint8_t *)router_mac_add.meter_mac_address.data(), meter_mac_address, 16) == 0)
            {
                memcpy(path_record, router_mac_add.path_record.data(), router_mac_add.path_record.size());
                path_record[router_mac_add.path_record.size()] = '\0';
                cached_hop_count = router_mac_add.hop_count;
                path_found = true;
                break;
            }
            temp_queue2.pop(); // Remove processed queue element
        }

        if (!path_found)
        {
            // ✅ Gateway fallback
            if (memcmp(meter_mac_address, gateway_id, 16) == 0)
            {
                this->print_and_log("🔄 Gateway fallback path\n");
                dest_address.clear();
                size_t copy_start = 8;
                while (copy_start + 8 <= 16)
                {
                    for (size_t i = 0; i < 8; i++)
                    {
                        dest_address.push_back(gateway_id[copy_start + i]);
                    }
                    copy_start += 16;
                }
                dest_address.push_back(0);
                cached_hop_count = 0;
            }
            else
            {
                this->print_and_log("❌ No path for MAC=%.*s\n", 16, meter_mac_address);
                return false;
            }
        }
        else
        {
            this->meterDetailsQueue.emplace(meter_manufacture_name, 64, // Manufacturer (64B)
                                            meter_fw_version, 64,
                                            meter_mac_address, 16,                        // Meter MAC (16B)
                                            path_record, 64,                              // Extracted path (64B)
                                            attribute_id, cached_hop_count, meter_phase); // Profile ID, hops, phase
            return true;
        }
    }
    this->meterDetailsQueue.emplace(meter_manufacture_name, 64, // Manufacturer (64B)
                                    meter_fw_version, 64,
                                    meter_mac_address, 16,                        // Meter MAC (16B)
                                    dest_address.data(), 64,                      // Extracted path (64B)
                                    attribute_id, cached_hop_count, meter_phase); // Profile ID, hops, phase
    return true;
}

int MySqlDatabase::fetch_path_record_from_src_route_network_db(char *gateway_id)
{
    this->print_and_log("%s Start\n", __FUNCTION__);

    MYSQL_ROW row;
    MYSQL_RES *result;
    char query_buffer[256] = {0};

    // Query source_route_network
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT path, hop_count, target_mac_address FROM source_route_network "
             "WHERE gateway_id='%s' AND disconnected_from_gateway='0'",
             gateway_id);

    if (this->execute_query(query_buffer) != SUCCESS)
    {
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);
    if (!result)
    {
        return FAILURE;
    }

    if (mysql_num_rows(result) > 0)
    {
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            if (!row[0] || !row[1] || !row[2]) continue; // Fixed: row[2]

            // ✅ DYNAMIC: Copy full path to vector (no fixed 64B!)
            std::vector<uint8_t> raw_path;
            unsigned long path_len = strlen(row[0]); // Actual MySQL string length

            if (path_len > 0)
            {
                raw_path.assign((uint8_t *)row[0], (uint8_t *)row[0] + path_len);
            }

            uint8_t hop_count = static_cast<uint8_t>(atoi(row[1]));
            uint8_t meter_mac[17] = {0};
            memcpy(meter_mac, (uint8_t *)row[2], 16);
            meter_mac[16] = '\0';

            this->print_and_log("Raw path len=%lu hops=%d MAC=%s\n",
                                path_len, hop_count, meter_mac);

            // ✅ DYNAMIC PATH EXTRACTION
            std::vector<uint8_t> dest_address;
            size_t total_length = hop_count * 16;
            size_t copy_start = 8;

            while (copy_start + 8 <= total_length && copy_start < raw_path.size())
            {
                for (size_t i = 0; i < 8; i++)
                {
                    if (copy_start + i < raw_path.size())
                    {
                        dest_address.push_back(raw_path[copy_start + i]);
                    }
                }
                copy_start += 16;
            }
            dest_address.push_back(0); // Null terminate

            // ✅ ENFORCE 64B MAX
            if (dest_address.size() > 65)
            {
                dest_address.resize(65); // Truncate + null
                dest_address[64] = 0;
            }

            // Queue size check
            if (this->pathSrcRouteQueue.size() >= 50)
            {
                this->print_and_log("Queue full → drop oldest\n");
                this->pathSrcRouteQueue.pop();
            }

            // ✅ QUEUE with ACTUAL sizes (DEEP COPY)
            this->pathSrcRouteQueue.emplace(
                meter_mac, 16,       // MAC + len
                dest_address.data(), // Dynamic path
                (hop_count * 8),     // path_len (excl null)
                hop_count);

            this->print_and_log("✅ Queued: MAC=%s path=%s hops=%d\n",
                                meter_mac, dest_address.data(), hop_count);

            get_alternate_source_route_network_from_db(meter_mac, gateway_id);
        }
    }

    mysql_free_result(result);
    return SUCCESS;
}

int MySqlDatabase::check_for_unsilenced_nodes(char *gateway_id)
{
    char query_buffer[256];      // Query 1: unsilenced FUOTA nodes
    MYSQL_RES *result = nullptr; // Result set 1: unsilenced nodes
    MYSQL_ROW row;               // Unsilenced node row

    // Query 1: Get ALL unsilenced nodes eligible for FUOTA
    snprintf(query_buffer, sizeof(query_buffer), "select meter_mac_address, Fuota_status from unsilenced_nodes_for_fuota where gateway_id = '%s';", gateway_id);
    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Query executed Failed.\n");
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);

    if (result != nullptr)
    {
        uint8_t target_mac_address[17] = {0}; // Target meter MAC
        uint8_t fuota_status;                 // FUOTA status flag
        // uint8_t hop_count;                     // hop_count

        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            if (!(row[0]) || !(row[1])) // Skip incomplete rows
            {
                continue;
            }

            fuota_status = static_cast<uint8_t>(atoi(row[1])); // Parse FUOTA status (col 2)
            if (fuota_status)
            {
                memcpy(target_mac_address, (uint8_t *)row[0], 16); // Copy target MAC (col 1)
                target_mac_address[16] = '\0';

                this->print_and_log("target_mac_address: (%s)\n", target_mac_address);

                std::queue<silenceND> temp_queue = MySqlDatabase::pathSrcRouteQueue; // Copy queue again (original unchanged)

                while (!temp_queue.empty())
                {
                    silenceND &router_mac_add = temp_queue.front(); // Get front queue element (meter record)

                    if (memcmp((uint8_t *)router_mac_add.meter_mac_address.data(), target_mac_address, 16) == 0)
                    {
                        auto qsize = this->silencedNDQueue.size(); // Check unsilence queue

                        if (qsize >= 50) // Queue full → drop oldest
                        {
                            this->print_and_log("HES data queue full, dropping oldest packet\n");
                            this->silencedNDQueue.pop();
                        }

                        // Queue for 9B unsilence command: path_data, path_length, hop_count
                        this->silencedNDQueue.emplace(target_mac_address, 16, router_mac_add.path_record.data(), router_mac_add.hop_count * 8, router_mac_add.hop_count);

                        this->print_and_log("Queued destination_address %.*s, hop count: %d for (%.*s)\n",
                                            static_cast<int>(router_mac_add.path_record.size()), (char *)router_mac_add.path_record.data(),
                                            router_mac_add.hop_count,
                                            16, (char *)target_mac_address);
                        break; // Process first valid path only
                    }
                    temp_queue.pop(); // Remove processed meter from temporary queue
                }
            }
        }
        mysql_free_result(result); // Cleanup unsilenced nodes results
    }
    return SUCCESS; // Scan completed
}

int MySqlDatabase::delete_node_from_db(uint8_t *meter_mac_address, char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buffer[512];
    snprintf(query_buffer, sizeof(query_buffer), "DELETE FROM unsilenced_nodes_for_fuota WHERE meter_mac_address = '%s' and gateway_id = '%s';", meter_mac_address, gateway_id);

    return this->execute_query(query_buffer);
}

int MySqlDatabase::delete_dlms_fuota_table(uint8_t *meter_mac_address, char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buffer[512];
    snprintf(query_buffer, sizeof(query_buffer), "DELETE FROM dlms_fuota_upload WHERE meter_mac_address = '%s' and gateway_id = '%s';", meter_mac_address, gateway_id);

    return this->execute_query(query_buffer);
}

int MySqlDatabase::update_into_gateway_status_info(const uint8_t *str, bool gateway_status, uint8_t val1, uint8_t val2, uint8_t val3)
{

    // Variables for parsing DCU identification from input string
    uint8_t gateway_type = 0; // DCU type (1 for PGWID:3C)

    uint8_t gateway_id[17] = {0}; // DCU ID buffer (16 bytes + null terminator)

    char query_buffer[1024] = {0}; // SQL query construction buffer

    std::string time_str;

    memcpy(gateway_id, str + 6, 16);

    gateway_id[16] = '\0';

    if (gateway_status == Status::DISCONNECTED)
    {
        //  Get current timestamp from client
        Client::client_get_time(time_str, 2);

        // update dcu_status_info in DB
        sprintf(query_buffer, "UPDATE gateway_status_info SET last_disconnected_time = '%s' , status = %d WHERE gateway_id = '%s';", time_str.c_str(), 0, gateway_id); // Update: timestamp + active status

        // Execute UPDATE query
        if (this->execute_query(query_buffer) == FAILURE)
        {
            this->print_and_log("Failed to execute INSERT query.\n"); // Note: Log says INSERT but it's UPDATE
            return FAILURE;
        }
        memset(query_buffer, 0, sizeof(query_buffer));

        snprintf(query_buffer, sizeof(query_buffer),
                 "INSERT INTO gateway_connection_log (gateway_id, gateway_type,status_description,time_stamp,gsm_signal_strength,gsm_modem_type,gateway_last_conn_state)"
                 "values ('%s','%d','%d', '%s','%d','%d','%d')",
                 gateway_id, gateway_type, 0, time_str.c_str(), val1, val2, val3);
        // execute
        if (this->execute_query(query_buffer) == FAILURE)
        {
            this->print_and_log("Failed to execute INSERT query.\n");
            return FAILURE;
        }
        return SUCCESS;
    }
    else if (gateway_status == Status::CONNECTED)
    {
        // Parse DCU type from input string prefix
        if (memcmp(str, "PGWID:3C", 8) == 0)
        {
            gateway_type = 1; // Set PGW type flag
        }

        // MySQL result structures for SELECT query
        MYSQL_RES *result = nullptr;
        MYSQL_ROW row;

        // Query 1: Fetch ALL DCU records to check existence
        snprintf(query_buffer, sizeof(query_buffer), "select gateway_id, gateway_type from gateway_status_info;");
        if (this->execute_query(query_buffer) == FAILURE)
        {
            this->print_and_log("Query executed Failed.\n");
            return FAILURE; // Exit on SELECT failure
        }

        // Store SELECT results for row iteration
        result = mysql_store_result(this->mysql);

        // Process each existing DCU record from database
        if (result != nullptr)
        {
            uint8_t gateway_id_from_db[17] = {0}; // DB DCU ID buffer
            uint8_t gateway_type_from_db = 0;     // DB DCU type

            while ((row = mysql_fetch_row(result)) != nullptr)
            {
                // Skip rows with NULL values
                if (!(row[0]) || !(row[1]))
                {
                    continue;
                }

                // Copy DCU ID from database row (column 0)
                memcpy(&gateway_id_from_db[0], (uint8_t *)row[0], 16);
                gateway_id_from_db[16] = '\0'; // Null-terminate for string comparison

                // Parse DCU type from database row (column 1)
                gateway_type_from_db = static_cast<uint8_t>(atoi(row[1]));

                // Match found: DCU ID + type identical
                if ((memcmp(gateway_id_from_db, gateway_id, 16) == 0) && (gateway_type_from_db == gateway_type))
                {
                    // Get current timestamp from client
                    Client::client_get_time(time_str, 2);

                    // Construct UPDATE query for matched DCU record
                    sprintf(query_buffer,
                            "UPDATE gateway_status_info SET last_connected_time = '%s' , status = %d WHERE gateway_id = '%s';",
                            time_str.c_str(), 1, gateway_id_from_db); // Update: timestamp + active status

                    // Execute UPDATE query
                    if (this->execute_query(query_buffer) == FAILURE)
                    {
                        this->print_and_log("Failed to execute INSERT query.\n"); // Note: Log says INSERT but it's UPDATE
                        return FAILURE;
                    }
                    return SUCCESS; // Success: DCU status updated
                }
            }
            mysql_free_result(result); // Cleanup: Free result memory
            return FAILURE;            // No matching DCU found in loop
        }
    }

    // No results from SELECT query
    this->print_and_log("NO gateway_id found\n");
    return FAILURE; // DCU record doesn't exist
}

int MySqlDatabase::insert_into_gateway_status_info(const uint8_t *str)
{
    this->print_and_log("insert_into_gateway_status_info\n");

    uint8_t gateway_type = 0;      // GATEWAY type (1=PGW, 0=other)
    uint8_t gateway_id[17] = {0};  // GATEWAY ID buffer (16 bytes + null)
    char query_buffer[1024] = {0}; // SQL INSERT query buffer
    std::string time_str;          // Current timestamp buffer

    if (memcmp(str, "PGWID:3C", 8) == 0) // Check PGW type prefix
    {
        gateway_type = 1; // Set PGW type flag
    }
    memcpy(gateway_id, str + 6, 16); // Extract GATEWAY ID (offset 6, 16 bytes)
    gateway_id[16] = '\0';           // Null terminate GATEWAY ID

    Client::client_get_time(time_str, 2); // Get current timestamp "2025-12-06 11:33:45"
    snprintf(query_buffer, sizeof(query_buffer),
             "INSERT INTO gateway_status_info (gateway_id, gateway_type,status,last_connected_time)"
             "values ('%s','%d','%d', '%s')",
             gateway_id, gateway_type, 1, time_str.c_str()); // INSERT: ID, type, status=1, timestamp

    // execute
    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Failed to execute INSERT query.\n");
        return FAILURE;
    }

    return SUCCESS; // New GATEWAY record successfully inserted
}

bool MySqlDatabase::insert_into_gateway_connection_log(const uint8_t *str, int val1, int val2, int val3)
{
    this->print_and_log("insert_into_gateway_status_info\n"); // Note: Log message mismatch (should be connection_log)

    uint8_t gateway_type = 0;      // GATEWAY type (1=PGW)
    uint8_t gateway_id[17] = {0};  // GATEWAY ID buffer
    char query_buffer[1024] = {0}; // SQL INSERT query buffer
    std::string time_str;

    if (memcmp(str, "PGWID:3C", 8) == 0) // Parse PGW type prefix
    {
        gateway_type = 1;
    }
    memcpy(gateway_id, str + 6, 16); // Extract GATEWAY ID from offset 6
    gateway_id[16] = '\0';

    Client::client_get_time(time_str, 2); // Get current timestamp
    snprintf(query_buffer, sizeof(query_buffer),
             "INSERT INTO gateway_connection_log (gateway_id, gateway_type,time_stamp,status_description,gsm_signal_strength,gsm_modem_type,gateway_last_conn_state)"
             "values ('%s','%d', '%s','%d','%d','%d','%d')",
             gateway_id, gateway_type, time_str.c_str(), 1, val1, val2, val3); // Log: ID,type,time,status,signal,modem,state

    // execute
    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Failed to execute INSERT query.\n");
        return FAILURE;
    }
    return SUCCESS; // Connection event logged successfully
}

int MySqlDatabase::check_for_fuota_resume(char *gateway_id)
{
    if (!gateway_id) // Validate GATEWAY ID parameter
    {
        this->print_and_log("%s: null gateway_id\n", __FUNCTION__);
        return FAILURE;
    }
    this->print_and_log("%s Start\n", __FUNCTION__);

    char query_buffer[1024] = {0}; // FUOTA query buffer
    MYSQL_RES *result = nullptr;   // FUOTA results
    MYSQL_ROW row;                 // Result row iterator

    sprintf(query_buffer, "select request_id, gateway_id, meter_mac_address, filepath_for_targetnode, file_name, schedule_time from dlms_fuota_upload where gateway_id = '%s';", gateway_id);

    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Query execution Failed.\n");
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);

    if (result != nullptr)
    {
        uint8_t target_mac_address[17] = {0}; // Target meter MAC
        uint8_t request_id = 0;               // FUOTA request ID
        uint8_t gateway_id[17] = {0};         // Gateway ID buffer
        uint8_t file_path[100] = {0};         // File path buffer
        uint8_t file_name[65] = {0};          // File name buffer
        char schedule_time_str[65] = {0};     // Scheduled time "2025-12-06 11:00:00"
        std::string time_str;                 // Current time buffer

        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            if (!(row[0]) || !(row[1]) || !(row[2]) || !(row[3]) || !(row[4]) || !(row[5])) // Skip incomplete rows
            {
                continue;
            }

            request_id = static_cast<uint8_t>(atoi(row[0])); // Parse request ID (col 0)

            memcpy(&gateway_id[0], (uint8_t *)row[1], sizeof(gateway_id)); // Copy gateway ID (col 1)
            gateway_id[16] = '\0';

            memcpy(&target_mac_address[0], (uint8_t *)row[2], sizeof(target_mac_address)); // Target MAC (col 2)
            target_mac_address[16] = '\0';

            memcpy(&file_path[0], (uint8_t *)row[3], sizeof(file_path)); // File path (col 3)
            file_path[99] = '\0';

            memcpy(&file_name[0], (uint8_t *)row[4], sizeof(file_name)); // File name (col 4)
            file_name[64] = '\0';

            memcpy(&schedule_time_str[0], (uint8_t *)row[5], sizeof(schedule_time_str)); // Schedule time (col 5)
            schedule_time_str[64] = '\0';

            this->print_and_log("request_id: %d, gateway_id: %s, target_mac_address: %s, file_path: %s, file_name: %s, time: %s\n",
                                request_id, gateway_id, target_mac_address, file_path, file_name, schedule_time_str);

            // Get current time                           // Shadow variable (overwrites previous time_str)
            Client::client_get_time(time_str, 2); // current time
            if (compare_schedule_time((const char *)schedule_time_str, time_str) == SUCCESS)
            {
                this->print_and_log("Resume FUOTA\n");
                mysql_free_result(result); // Cleanup before early return
                return SUCCESS;            // FUOTA within 30min window → RESUME
            }
            else
            {
                // this->delete_dlms_fuota_table(target_mac_address,gateway_id);
            }
        }
        mysql_free_result(result);
    }
    else
    {
        this->print_and_log("GATEWAY ID NOT FOUND IN FUOTA REQUEST TABLE\n");
        return FAILURE;
    }
    return FAILURE; // No valid FUOTA jobs or all expired
}

int MySqlDatabase::compare_schedule_time(const char *schedule_time_str, std::string current_time_str)
{
    this->print_and_log("Compare_schedule_time\n");
    struct tm sched_tm{};         // Scheduled time structure
    struct tm curr_tm{};          // Current time structure
    time_t sched_time, curr_time; // Epoch time values

    // Parse schedule time: "2025-12-05 10:15:30"
    if (strptime(schedule_time_str, "%Y-%m-%d %H:%M:%S", &sched_tm) == nullptr ||
        strptime(current_time_str.c_str(), "%Y-%m-%d %H:%M:%S", &curr_tm) == nullptr)
    {
        this->print_and_log("null pointer\n");
        return -1; // Parse error
    }

    sched_tm.tm_isdst = -1; // Ignore daylight savings
    curr_tm.tm_isdst = -1;

    sched_time = mktime(&sched_tm); // Convert scheduled time to epoch
    curr_time = mktime(&curr_tm);   // Convert current time to epoch

    double minutes_diff = difftime(curr_time, sched_time) / 60.0; // Minutes difference

    this->print_and_log("Schedule: %s\n", schedule_time_str);
    this->print_and_log("Current:  %s (%.1f min diff)\n", current_time_str.c_str(), minutes_diff);

    if (minutes_diff <= 30.0) // Within 30 minute resume window
    {
        this->print_and_log("✅ WITHIN 30min - RESUME FUOTA!\n");
        return SUCCESS; // Success - within 30min
    }
    else
    {
        this->print_and_log("⏰ TOO OLD (>30min)\n");
        return FAILURE; // Too old
    }
}

int MySqlDatabase::get_nodes_info_from_src_route_network_db(std::map<std::array<uint8_t, 8>, NodeInfo> &nodes_info, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query[256] = {0};

    snprintf(query, sizeof(query), "SELECT target_mac_address, hop_count, path FROM source_route_network WHERE gateway_id = '%s' AND disconnected_from_gateway = '0';", gateway_id);

    if (this->execute_query(query))
        return FAILURE;

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (!result)
        return FAILURE;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)) != nullptr)
    {
        if (!(row[0]) || !(row[1]) || !(row[2]))
        {
            continue;
        }

        const char *mac_hex = row[0];
        int hop_count = atoi(row[1]);
        const char *path_hex = row[2];

        std::array<uint8_t, 8> mac = PullData::mac_from_hex(mac_hex);

        // Build path
        std::string hex_path(path_hex);
        std::vector<uint8_t> parsed_path;

        if (!PullData::extract_path(hex_path, hop_count, parsed_path))
        {
            this->print_and_log("Invalid path length for MAC %s\n", mac_hex);
            continue;
        }

        // Create or reference existing node
        NodeInfo &info = nodes_info[mac];
        info.node_mac_address = mac;
        // Store into primary_path
        info.primary_path.hop_count = hop_count;
        info.primary_path.path = std::move(parsed_path);

        this->print_and_log("Added node %s with %d hop ", mac_hex, info.primary_path.hop_count);

        this->print_and_log("Path:");
        for (uint8_t b : info.primary_path.path)
            this->print_and_log(" %02X", b);
        this->print_and_log("\n");
    }

    mysql_free_result(result);
    return SUCCESS;
}

int MySqlDatabase::get_alternate_path_for_node(NodeInfo &node, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node.node_mac_address, mac_hex);

    char query[256];
    snprintf(query, sizeof(query), "SELECT hop_count, path FROM alternate_source_route_network WHERE target_mac_address = '%s' and gateway_id = '%s' LIMIT 1;", mac_hex, gateway_id);

    if (this->execute_query(query))
        return FAILURE;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return false;

    MYSQL_ROW row;
    node.alternate_paths.clear(); // clear old list
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        if (!row)
            return false;

        int hop_count = atoi(row[0]);
        const char *hex_path = row[1];

        PathInfo pi;
        pi.hop_count = hop_count;

        if (!PullData::extract_path(hex_path, hop_count, pi.path))
            continue;

        node.alternate_paths.push_back(pi);

        // Print the path
        this->print_and_log("Alternate path %d (hop_count=%d): ", node.alternate_paths.size(), hop_count);
        for (size_t i = 0; i < pi.path.size(); ++i)
        {
            this->print_and_log("%02X", pi.path[i]);
            if ((i + 1) % 4 == 0 && i + 1 < pi.path.size())
                this->print_and_log(" -> ");
        }
        this->print_and_log("\n");
    }

    mysql_free_result(res);

    return true;
}

std::vector<int> MySqlDatabase::get_last_hour_missing_ip_cycles_for_node(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    std::vector<int> missing_cycles;

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    // Compute current cycle and start cycle
    int hour = 0, minute = 0;
    std::time_t t = std::time(nullptr);
    std::tm *lt = std::localtime(&t);
    hour = lt->tm_hour;
    minute = lt->tm_min;

    int current_cycle = (hour * 4) + (minute / 15) + 1;
    int start_cycle = ((current_cycle - 4 + 96) % 96) + 1;

    std::string query = "SELECT nums.cycle_id FROM (SELECT n1.n + n2.n*10 + 1 AS cycle_id "
                        "FROM (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 "
                        "UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 "
                        "UNION ALL SELECT 8 UNION ALL SELECT 9) n1, "
                        "(SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 "
                        "UNION ALL SELECT 4 UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 "
                        "UNION ALL SELECT 8 UNION ALL SELECT 9) n2 "
                        "WHERE n1.n + n2.n*10 < 96) AS nums "
                        "LEFT JOIN dlms_ip_push_data d ON d.cycle_id = nums.cycle_id "
                        "AND d.meter_mac_address='" +
                        std::string(mac_hex) + "' " +
                        "AND d.gateway_id='" +
                        std::string(gateway_id) + "' " +
                        "AND d.updated_time >= NOW() - INTERVAL 1 HOUR "
                        "AND d.updated_time <= NOW() "
                        "WHERE (( " +
                        std::to_string(start_cycle) + " <= " + std::to_string(current_cycle) +
                        " AND nums.cycle_id BETWEEN " + std::to_string(start_cycle) + " AND " + std::to_string(current_cycle) + ") " +
                        "OR (" +
                        std::to_string(start_cycle) + " > " + std::to_string(current_cycle) +
                        " AND (nums.cycle_id >= " + std::to_string(start_cycle) + " OR nums.cycle_id <= " + std::to_string(current_cycle) + "))) " +
                        "AND d.cycle_id IS NULL "
                        "ORDER BY ((nums.cycle_id - " +
                        std::to_string(start_cycle) + " + 96) % 96);";

    if (this->execute_query(const_cast<char *>(query.c_str())))
        return missing_cycles;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return missing_cycles;

    std::vector<int> abs_cycles;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
    {
        if (row[0])
        {
            if (abs_cycles.size() >= 4)
                break;
            int cycle = std::stoi(row[0]);
            abs_cycles.push_back(cycle);
            this->print_and_log("Missing cycle: %d\n", cycle);
        }
    }

    mysql_free_result(res);

    // Convert absolute to relative 0–3
    for (int cid : abs_cycles)
    {
        int relative = (current_cycle - cid + 96) % 96;
        if (relative >= 0 && relative <= 3)
        {
            missing_cycles.push_back(relative);
            this->print_and_log("Converted relative cycle: %d (from %d)\n", relative, cid);
        }
        else
        {
            this->print_and_log("Skipping relative cycle %d (from %d)\n", relative, cid);
        }
    }

    return missing_cycles;
}

bool MySqlDatabase::is_blp_available_last_hour(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query_buffer[1024];
    snprintf(query_buffer, sizeof(query_buffer), "SELECT CASE WHEN COUNT(DISTINCT cycle_id) = 0 THEN 0 ELSE 1 END AS result FROM dlms_block_load_push_profile WHERE meter_mac_address = '%s' AND gateway_id = '%s' AND last_download_time >= DATE_SUB(NOW(), INTERVAL 1 HOUR) AND (cycle_id %% 4) = 0;", mac_hex, gateway_id);

    if (this->execute_query(query_buffer))
        return false;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool available = false;

    if (row && row[0])
        available = (std::stoi(row[0]) == 1);

    mysql_free_result(res);

    this->print_and_log("BLP last-hour available: %s\n", available ? "TRUE" : "FALSE");

    return available;
}

bool MySqlDatabase::is_dlp_available_previous_day(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query_buffer[1024];
    snprintf(query_buffer, sizeof(query_buffer), "SELECT CASE WHEN COUNT(*) = 0 THEN 0 ELSE 1 END AS result FROM dlms_daily_load_push_profile WHERE meter_mac_address = '%s' AND gateway_id = '%s' AND real_time_clock >= DATE_SUB(CURDATE(), INTERVAL 1 DAY) AND real_time_clock < CURDATE() LIMIT 1;", mac_hex, gateway_id);

    if (this->execute_query(query_buffer))
        return false;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool available = false;

    if (row && row[0])
        available = (std::stoi(row[0]) == 1);

    mysql_free_result(res);

    this->print_and_log("DLP previous day available: %s\n", available ? "TRUE" : "FALSE");

    return available;
}

bool MySqlDatabase::is_bhp_available_previous_month(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query[1024];
    snprintf(query, sizeof(query), "SELECT CASE WHEN COUNT(*) = 0 THEN 0 ELSE 1 END AS result FROM dlms_history_data WHERE meter_mac_address = '%s' AND gateway_id = '%s' AND billing_date_import_mode >= DATE_FORMAT(CURRENT_DATE - INTERVAL 1 MONTH, '%%Y-%%m-01') AND billing_date_import_mode < DATE_FORMAT(CURRENT_DATE + INTERVAL 1 MONTH, '%%Y-%%m-01');", mac_hex, gateway_id);

    if (execute_query(query))
        return false;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool available = false;

    if (row && row[0])
        available = (std::stoi(row[0]) == 1);

    mysql_free_result(res);

    this->print_and_log("BHP previous month available: %s\n", available ? "TRUE" : "FALSE");

    return available;
}

bool MySqlDatabase::is_nameplate_available(std::array<uint8_t, 8> node_mac_address)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query_buffer[512];
    snprintf(query_buffer, sizeof(query_buffer), "SELECT CASE WHEN COUNT(*) = 0 THEN 0 ELSE 1 END AS result FROM name_plate_data WHERE meter_mac_address = '%s';", mac_hex);

    if (this->execute_query(query_buffer))
        return false;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool available = false;

    if (row && row[0])
        available = (std::stoi(row[0]) == 1);

    mysql_free_result(res);

    this->print_and_log("Nameplate available: %s\n", available ? "TRUE" : "FALSE");

    return available;
}

bool MySqlDatabase::is_scalar_profile_available(std::array<uint8_t, 8> node_mac_address)
{
    this->print_and_log("%s start\n", __FUNCTION__);
    (void)node_mac_address;
    return true;
}

bool MySqlDatabase::is_node_silenced(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query_buffer[512];
    snprintf(query_buffer, sizeof(query_buffer), "SELECT CASE WHEN COUNT(*) = 0 THEN 0 ELSE 1 END AS result FROM unsilenced_nodes_for_fuota WHERE meter_mac_address = '%s' and gateway_id = '%s';", mac_hex, gateway_id);

    if (this->execute_query(query_buffer))
        return false;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return false;

    MYSQL_ROW row = mysql_fetch_row(res);
    bool silenced = false;

    if (row && row[0])
        silenced = (std::stoi(row[0]) == 1);

    mysql_free_result(res);

    this->print_and_log("Node unsilenced: %s\n", silenced ? "TRUE" : "FALSE");

    return silenced;
}

int MySqlDatabase::is_ifv_available_for_node(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query_buffer[512];
    snprintf(query_buffer, sizeof(query_buffer), "SELECT rf_module_internal_fw_version FROM meter_details WHERE meter_mac_address = '%s' AND gateway_id = '%s';", mac_hex, gateway_id);

    if (this->execute_query(query_buffer))
        return FAILURE;

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return FAILURE;

    MYSQL_ROW row = mysql_fetch_row(res);

    int result = 0;

    if (!row)
    {
        // row not present
        result = 2;
        mysql_free_result(res);
        return result;
    }

    if (row[0] == nullptr)
    {
        result = 0; // row present but NULL
    }
    else
    {
        result = 1; // row present and has value
    }

    mysql_free_result(res);

    this->print_and_log("IFV available: %s\n", result ? "TRUE" : "FALSE");

    return result;
}

int MySqlDatabase::delete_node_from_unsilence_nodes_from_fuota(std::array<uint8_t, 8> node_mac_address, const char *gateway_id)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    char query_buffer[512];
    snprintf(query_buffer, sizeof(query_buffer), "DELETE FROM unsilenced_nodes_for_fuota WHERE meter_mac_address = '%s' and gateway_id = '%s';", mac_hex, gateway_id);

    return this->execute_query(query_buffer);
}

int MySqlDatabase::insert_name_plate_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> *name_plate_data)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};
    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    const DlmsRecordMap &rec = name_plate_data->profile_data;

    auto meter_serial_number = DlmsValueExtractor::get_octet_string(rec, 0x00);
    std::string meter_serial_number_str(meter_serial_number.begin(), meter_serial_number.end());

    auto device_id = DlmsValueExtractor::get_octet_string(rec, 0x01);
    std::string device_id_str(device_id.begin(), device_id.end());

    auto manufacturer_name = DlmsValueExtractor::get_octet_string(rec, 0x02);
    std::string manufacturer_name_str(manufacturer_name.begin(), manufacturer_name.end());

    auto firmware_version = DlmsValueExtractor::get_octet_string(rec, 0x03);
    std::string firmware_version_str(firmware_version.begin(), firmware_version.end());

    uint32_t meter_type = static_cast<uint32_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x04, 0.0));

    auto meter_category = DlmsValueExtractor::get_octet_string(rec, 0x05);
    std::string meter_category_str(meter_category.begin(), meter_category.end());

    auto current_rating = DlmsValueExtractor::get_octet_string(rec, 0x06);
    std::string current_rating_str(current_rating.begin(), current_rating.end());

    uint32_t meter_year_of_manufacture = static_cast<uint32_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x07, 0.0));

    // Rest of the function remains the same...
    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO name_plate_data(meter_serial_number, meter_mac_address, gateway_id, device_id, manufacturer_name, firmware_version_for_meter, meter_type, category, current_rating, meter_manufacture_year, last_download_time, push_alaram) VALUES('%s','%s','%s','%s','%s','%s',%d,'%s','%s',%d,'%s',%d);",
             meter_serial_number_str.c_str(),
             mac_hex,
             gateway_id,
             device_id_str.c_str(),
             manufacturer_name_str.c_str(),
             firmware_version_str.c_str(),
             meter_type,
             meter_category_str.c_str(),
             current_rating_str.c_str(),
             meter_year_of_manufacture,
             this->now().c_str(),
             0);

    if (execute_query(query_buf) == FAILURE)
    {
        return FAILURE;
    }

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (result != nullptr)
    {
        MYSQL_ROW row = mysql_fetch_row(result);
        int row_count = atoi(row[0]);
        mysql_free_result(result);

        memset(query_buf, 0, sizeof(query_buf));

        if (row_count > 0)
        {
            printf("Updating meter_details for MAC: %s\n", mac_hex);
            snprintf(query_buf, sizeof(query_buf), "UPDATE meter_details SET gateway_id='%s', meter_mac_address='%s', device_id='%s', meter_manufacture_name='%s', meter_firmware_version='%s', meter_phase='%d', category='%s', current_rating='%s', manufacture_year='%d', last_download_time='%s' WHERE meter_address ='%s';",
                     gateway_id,
                     mac_hex,
                     device_id_str.c_str(),
                     manufacturer_name_str.c_str(),
                     firmware_version_str.c_str(),
                     meter_type,
                     meter_category_str.c_str(),
                     current_rating_str.c_str(),
                     meter_year_of_manufacture,
                     this->now().c_str(),
                     &mac_hex[8]);
        }
        else
        {
            snprintf(query_buf, sizeof(query_buf), "INSERT INTO meter_details(gateway_id, meter_mac_address, meter_address, device_id, meter_manufacture_name, meter_firmware_version, meter_phase, category, current_rating, manufacture_year, last_download_time) VALUES ('%s','%s','%s','%s','%s','%s','%d','%s','%s','%d','%s')",
                     gateway_id,
                     mac_hex,
                     meter_serial_number_str.c_str(),
                     device_id_str.c_str(),
                     manufacturer_name_str.c_str(),
                     firmware_version_str.c_str(),
                     meter_type,
                     meter_category_str.c_str(),
                     current_rating_str.c_str(),
                     meter_year_of_manufacture,
                     this->now().c_str());
        }
    }

    return SUCCESS;
}

int MySqlDatabase::calculate_cycle_id_for_block_load(void)
{
    std::time_t now = std::time(nullptr);
    std::tm local_time{};

    localtime_r(&now, &local_time); // thread-safe

    int hour = local_time.tm_hour; // 0–23

    return (hour * 4) + 4;
}

int MySqlDatabase::calculate_cycle_id(int subtract_cycles)
{
    // sanitize input (expected 0..3)
    if (subtract_cycles < 0 || subtract_cycles > 3)
        subtract_cycles = 0;

    // subtract_cycles represents how many 15-minute intervals to go back
    const std::time_t seconds_to_subtract = static_cast<std::time_t>(subtract_cycles) * 15 * 60;

    std::time_t now = std::time(nullptr);

    // apply subtraction
    now -= seconds_to_subtract;

    std::tm local_time{};
    localtime_r(&now, &local_time);

    int hour = local_time.tm_hour;  // 0–23
    int minute = local_time.tm_min; // 0–59

    int total_minutes = hour * 60 + minute;

    // Round to the nearest 15-minute cycle
    // Add tolerance = 5 minutes before dividing.
    int cycle = (total_minutes + 5) / 15;

    // convert 0–95 → 1–96
    int cycle_id = cycle + 1;

    if (cycle_id < 1)
        cycle_id = 96;
    else if (cycle_id > 96)
        cycle_id = ((cycle_id - 1) % 96) + 1;

    return cycle_id;
}

std::string MySqlDatabase::format_timestamp(uint32_t hex_timestamp)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    this->print_and_log("Hex timestamp: 0x%08X\n", hex_timestamp);

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
    return result;
}

std::string MySqlDatabase::parse_dlms_date_time(const uint8_t *data, size_t len)
{
    if (!data || len < 7)
    {
        this->print_and_log("DT RAW: <invalid input len=%zu>\n", len);
        return "2000-01-01 00:00";
    }

    uint16_t year = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint8_t month = data[2];
    uint8_t day = data[3];
    uint8_t hour = data[5];
    uint8_t minute = data[6];

    if (year == 0xFFFF || year == 0) year = 2000;
    if (month == 0xFF || month == 0) month = 1;
    if (day == 0xFF || day == 0) day = 1;
    if (hour == 0xFF) hour = 0;
    if (minute == 0xFF) minute = 0;

    char out[32];
    snprintf(out, sizeof(out), "%04u-%02u-%02u %02u:%02u",
             static_cast<unsigned>(year),
             static_cast<unsigned>(month),
             static_cast<unsigned>(day),
             static_cast<unsigned>(hour),
             static_cast<unsigned>(minute));

    return std::string(out);
}

int MySqlDatabase::insert_instantaneous_parameters_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, int cycle, PacketBuffer<DlmsRecordMap> &instantaneous_profile, bool push_status)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};
    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);
    std::string meter_serial_no(&mac_hex[8], 8);

    const DlmsRecordMap &rec = instantaneous_profile.profile_data;

    auto rtc_raw = DlmsValueExtractor::get_numeric_or_default(rec, 0x00);
    rtc_raw -= SECONDS_5H30M;
    std::string meter_rtc_time = format_timestamp(static_cast<uint32_t>(rtc_raw));
    float voltage = DlmsValueExtractor::get_numeric_or_default(rec, IP_VOLTAGE) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_VOLTAGE));
    float phase_current = DlmsValueExtractor::get_numeric_or_default(rec, 0x02) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_PHASE_CURRENT));
    float neutral_current = DlmsValueExtractor::get_numeric_or_default(rec, 0x03) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_NEUTRAL_CURRENT));
    float signed_powerfactor = DlmsValueExtractor::get_numeric_or_default(rec, 0x04) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_SIGNED_POWER_FACTOR));
    float frequency = DlmsValueExtractor::get_numeric_or_default(rec, 0x05) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_FREQUENCY));
    float apparent_power_kva = DlmsValueExtractor::get_numeric_or_default(rec, 0x06) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_APPARENT_POWER));
    float active_power_kw = DlmsValueExtractor::get_numeric_or_default(rec, 0x07) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_ACTIVE_POWER));
    float cum_energy_kwh_import = DlmsValueExtractor::get_numeric_or_default(rec, 0x08) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_IMPORT_KWH));
    float cum_energy_kvah_import = DlmsValueExtractor::get_numeric_or_default(rec, 0x09) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_IMPORT_KVAH));
    float maximum_demand_kw = DlmsValueExtractor::get_numeric_or_default(rec, 0x0A) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_MAXIMUM_DEMAND_KW));

    auto md_kw_octet = DlmsValueExtractor::get_octet_string(rec, 0x0B);
    std::string md_kw_datetime = parse_dlms_date_time(md_kw_octet.data(), md_kw_octet.size());

    float maximum_demand_kva = DlmsValueExtractor::get_numeric_or_default(rec, 0x0C) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_MAXIMUM_DEMAND_KVA));

    auto md_kva_octet = DlmsValueExtractor::get_octet_string(rec, 0x0D);
    std::string md_kva_datetime = parse_dlms_date_time(md_kva_octet.data(), md_kva_octet.size());

    float cum_power_on_duration = DlmsValueExtractor::get_numeric_or_default(rec, 0x0E) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_POWER_ON));
    float cum_tamper_count = DlmsValueExtractor::get_numeric_or_default(rec, 0x0F) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_TAMPER_COUNT));
    float cum_billing_count = DlmsValueExtractor::get_numeric_or_default(rec, 0x10) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_BILLING_COUNT));
    float cum_programming_count = DlmsValueExtractor::get_numeric_or_default(rec, 0x11) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_PROGRAMMING_COUNT));
    float cum_energy_kwh_export = DlmsValueExtractor::get_numeric_or_default(rec, 0x12) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_EXPORT_KWH));
    float cum_energy_kvah_export = DlmsValueExtractor::get_numeric_or_default(rec, 0x13) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_CUMULATIVE_ENERGY_EXPORT_KVAH));
    bool loadlimit_function_sts = DlmsValueExtractor::get_bool(rec, 0x14, false);
    float loadlimit_value_kw = DlmsValueExtractor::get_numeric_or_default(rec, 0x15) * convertScalar(getScalarValue(meter_serial_no, "0100", IP_LOAD_LIMIT_VALUE_KW));

    // Rest of SQL query...
    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO dlms_ip_push_data("
             "meter_serial_number,meter_mac_address,gateway_id,meter_rtc_time,"
             "voltage,phase_current,neutral_current,signed_powerfactor,frequency,"
             "apparent_power_kva,active_power_kw,cum_energy_kwh_import,cum_energy_kvah_import,"
             "maximum_demand_kw,md_kw_datetime,maximum_demand_kva,md_kva_datetime,"
             "cum_power_on_duration,cum_tamper_count,cum_billing_count,cum_programming_count,"
             "cum_energy_kwh_export,cum_energy_kvah_export,loadlimit_function_sts,loadlimit_value_kw,"
             "last_download_time,cycle_id,push_alaram,error_code) "
             "VALUES('%s', '%s', '%s', '%s', %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f,"
             "%.2f, '%s', %.2f, '%s', %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %d, %.2f, '%s',%d, %d, %d);",

             &mac_hex[8], mac_hex, gateway_id, meter_rtc_time.c_str(),
             voltage, phase_current, neutral_current, signed_powerfactor, frequency,
             apparent_power_kva, active_power_kw, cum_energy_kwh_import, cum_energy_kvah_import,
             maximum_demand_kw, md_kw_datetime.c_str(),
             maximum_demand_kva, md_kva_datetime.c_str(),
             cum_power_on_duration, cum_tamper_count, cum_billing_count, cum_programming_count,
             cum_energy_kwh_export, cum_energy_kvah_export,
             loadlimit_function_sts ? 1 : 0, loadlimit_value_kw,
             this->now().c_str(), cycle, push_status ? 1 : 0, push_status ? 1 : 0);

    if (execute_query(query_buf) == SUCCESS)
    {
        // int freq_val = 0;
        int8_t frequency_offset = 0;
        int16_t temperature = 0;
        uint32_t tdc = 0u;

        int8_t rssi = static_cast<int8_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0xF0));        // RSSI
        int8_t noise_floor = static_cast<int8_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0xF2)); // Noise Floor

        // Frequency Offset
        {
            frequency_offset = static_cast<int8_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0xF1));
            // freq_val = (static_cast<int>(frequency_offset) * 25000) / 64;
        }

        // TDC
        {
            uint32_t raw = static_cast<uint32_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0xF3));
            tdc = ((raw & 0x000000FFu) << 24) |
                  ((raw & 0x0000FF00u) << 8) |
                  ((raw & 0x00FF0000u) >> 8) |
                  ((raw & 0xFF000000u) >> 24);
        }

        // Temperature
        {
            uint16_t raw = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0xF4));
            uint16_t swapped = static_cast<uint16_t>((raw >> 8) | (raw << 8));
            temperature = static_cast<int16_t>(swapped);
        }

        memset(query_buf, 0, sizeof(query_buf));

        snprintf(query_buf, sizeof(query_buf), "INSERT INTO rf_param_info(gateway_id,meter_mac_address,meter_sl_no,frequency_offset,frequency_last_download_time,meter_rtc_time,rssi_value,noise_floor,temprature,tdc) VALUES('%s', '%s', '%s', %d, '%s', '%s', %d, %d, %d, %u);",
                 gateway_id,
                 mac_hex,
                 &mac_hex[8],
                 frequency_offset,
                 this->now().c_str(),
                 meter_rtc_time.c_str(),
                 rssi,
                 noise_floor,
                 temperature,
                 tdc);

        return execute_query(query_buf);
    }

    return FAILURE;
}

int MySqlDatabase::insert_daily_load_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &daily_load_profile, bool push_status)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);
    std::string meter_serial_no(&mac_hex[8], 8);

    const DlmsRecordMap &rec = daily_load_profile.profile_data;

    auto rtc_raw = DlmsValueExtractor::get_numeric_or_default(rec, 0x00);
    std::string meter_rtc_time = format_timestamp(static_cast<uint32_t>(rtc_raw));
    float cum_energy_kwh_export = DlmsValueExtractor::get_numeric_or_default(rec, 0x01) * convertScalar(getScalarValue(meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_EXPORT_KWH));
    float cum_energy_kvah_export = DlmsValueExtractor::get_numeric_or_default(rec, 0x02) * convertScalar(getScalarValue(meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_EXPORT_KVAH));
    float cum_energy_kwh_import = DlmsValueExtractor::get_numeric_or_default(rec, 0x03) * convertScalar(getScalarValue(meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_IMPORT_KWH));
    float cum_energy_kvah_import = DlmsValueExtractor::get_numeric_or_default(rec, 0x04) * convertScalar(getScalarValue(meter_serial_no, "0300", DLP_CUMULATIVE_ENERGY_IMPORT_KVAH));

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO dlms_daily_load_push_profile(meter_mac_address,meter_serial_number,gateway_id,real_time_clock, cum_energy_kwh_export,cum_energy_kvah_export,cum_energy_kwh_import,cum_energy_kvah_import,last_download_time,push_alaram,error_code) VALUES('%s','%s','%s','%s',%.3f,%.3f,%.3f,%.3f,'%s',%d,%d);",

             /* meter_mac_address    */ mac_hex,
             /* meter_serial_number  */ &mac_hex[8],
             /* gateway_id               */ gateway_id,
             /* real_time_clock      */ meter_rtc_time.c_str(),

             /* export kWh           */ cum_energy_kwh_export,
             /* export kVAh          */ cum_energy_kvah_export,
             /* import kWh           */ cum_energy_kwh_import,
             /* import kVAh          */ cum_energy_kvah_import,

             /* last_download_time   */ this->now().c_str(),
             /* push_alarm           */ push_status ? 1 : 0,
             /* error_code           */ push_status ? 1 : 0);

    return execute_query(query_buf);
}

int MySqlDatabase::insert_block_load_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, int cycle_id, PacketBufferBlockLoad &block_load_buffer, bool push_status)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);
    std::string meter_serial_no(&mac_hex[8], 8);

    int ret = 0;

    for (const auto &profile : block_load_buffer.profiles_data)
    {
        this->print_and_log("Inserting Block Load Data to db of profile\n");
        char query_buf[4096] = {0};

        auto rtc_raw = DlmsValueExtractor::get_numeric_or_default(profile, 0x00);
        rtc_raw -= SECONDS_5H30M;
        std::string meter_rtc_time = format_timestamp(static_cast<uint32_t>(rtc_raw));

        float avg_voltage = DlmsValueExtractor::get_numeric_or_default(profile, 0x01) * convertScalar(getScalarValue(meter_serial_no, "0400", BLP_AVERAGE_VOLTAGE));
        float imp_kwh = DlmsValueExtractor::get_numeric_or_default(profile, 0x02) * convertScalar(getScalarValue(meter_serial_no, "0400", BLP_BLOCK_ENERGY_IMPORT_KWH));
        float imp_kvah = DlmsValueExtractor::get_numeric_or_default(profile, 0x03) * convertScalar(getScalarValue(meter_serial_no, "0400", BLP_BLOCK_ENERGY_IMPORT_KVAH));
        float exp_kwh = DlmsValueExtractor::get_numeric_or_default(profile, 0x04) * convertScalar(getScalarValue(meter_serial_no, "0400", BLP_BLOCK_ENERGY_EXPORT_KWH));
        float exp_kvah = DlmsValueExtractor::get_numeric_or_default(profile, 0x05) * convertScalar(getScalarValue(meter_serial_no, "0400", BLP_BLOCK_ENERGY_EXPORT_KVAH));
        // float avg_current = static_cast<float>(profile.average_current.data) * convertScalar(getScalarValue(meter_serial_no, "0400", BLP_AVERAGE_CURRENT));

        snprintf(query_buf, sizeof(query_buf),
                 "INSERT INTO dlms_block_load_push_profile(meter_mac_address,meter_serial_number,gateway_id,real_time_clock,average_voltage,block_energy_kwh_import,block_energy_kvah_import,block_energy_kwh_export,block_energy_kvah_export,last_download_time,cycle_id,push_alaram,error_code) VALUES('%s','%s','%s','%s',%.2f,%.3f,%.3f,%.3f,%.3f,'%s',%d,%d,%d);",

                 /* meter_mac_address   */ mac_hex,
                 /* meter_serial_number */ &mac_hex[8],
                 /* gateway_id              */ gateway_id,
                 /* rtc time            */ meter_rtc_time.c_str(),

                 /* avg voltage         */ avg_voltage,
                 /* import kWh          */ imp_kwh,
                 /* import kVAh         */ imp_kvah,
                 /* export kWh          */ exp_kwh,
                 /* export kVAh         */ exp_kvah,

                 /* last_download_time  */ this->now().c_str(),
                 /* cycle_id            */ cycle_id,
                 /* push_alarm          */ push_status ? 1 : 0,
                 /* error_code          */ push_status ? 1 : 0);

        ret = execute_query(query_buf);

        if (ret != 0)
        {
            this->print_and_log("Block load insert failed\n");
            return ret;
        }
    }

    return 0;
}

int MySqlDatabase::insert_billing_history_profile_data(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &billing_history, bool push_status)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);
    std::string meter_serial_no(&mac_hex[8], 8);

    char query_buf[4096] = {0};

    const DlmsRecordMap &rec = billing_history.profile_data;

    auto billing_date = DlmsValueExtractor::get_octet_string(rec, 0x00);
    std::string billing_date_import_mode = parse_dlms_date_time(billing_date.data(), billing_date.size());

    float average_pwr_factor = DlmsValueExtractor::get_numeric_or_default(rec, 0x01) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_AVERAGE_POWER_FACTOR_FOR_BILLING_PERIOD));
    float cumulative_energy_import_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x02) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_IMPORT_KWH));
    float cumulative_energy_TZ1_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x03) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ1_KWH));
    float cumulative_energy_TZ2_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x04) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ2_KWH));
    float cumulative_energy_TZ3_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x05) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ3_KWH));
    float cumulative_energy_TZ4_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x06) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ4_KWH));
    float cumulative_energy_TZ5_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x07) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ5_KWH));
    float cumulative_energy_TZ6_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x08) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ6_KWH));
    float cumulative_energy_TZ7_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x09) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ7_KWH));
    float cumulative_energy_TZ8_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x0A) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ8_KWH));

    float cumulative_energy_import_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x0B) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_IMPORT_KVAH));
    float cumulative_energy_TZ1_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x0C) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ1_KVAH));
    float cumulative_energy_TZ2_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x0D) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ2_KVAH));
    float cumulative_energy_TZ3_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x0E) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ3_KVAH));
    float cumulative_energy_TZ4_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x0F) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ4_KVAH));
    float cumulative_energy_TZ5_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x10) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ5_KVAH));
    float cumulative_energy_TZ6_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x11) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ6_KVAH));
    float cumulative_energy_TZ7_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x12) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ7_KVAH));
    float cumulative_energy_TZ8_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x13) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_TZ8_KVAH));

    float MD_kW = DlmsValueExtractor::get_numeric_or_default(rec, 0x14) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_MD_KW));

    auto md_kw_date_and_time = DlmsValueExtractor::get_octet_string(rec, 0x15);
    std::string MD_kW_date_and_time = parse_dlms_date_time(md_kw_date_and_time.data(), md_kw_date_and_time.size());

    float MD_kVA = DlmsValueExtractor::get_numeric_or_default(rec, 0x16) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_MD_KVA));

    auto md_kva_date_and_time = DlmsValueExtractor::get_octet_string(rec, 0x17);
    std::string MD_kVA_date_and_time = parse_dlms_date_time(md_kva_date_and_time.data(), md_kva_date_and_time.size());

    float billing_power_on_duration = DlmsValueExtractor::get_numeric_or_default(rec, 0x18) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_BILLING_POWER_ON_DURATION));
    float cumulative_energy_export_kWh = DlmsValueExtractor::get_numeric_or_default(rec, 0x19) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_EXPORT_KWH));
    float cumulative_energy_export_kVAh = DlmsValueExtractor::get_numeric_or_default(rec, 0x1A) * convertScalar(getScalarValue(meter_serial_no, "0200", BHP_CUMULATIVE_ENERGY_EXPORT_KVAH));

    snprintf(query_buf, sizeof(query_buf),
             "INSERT INTO dlms_history_data(meter_mac_address,meter_serial_number,gateway_id,billing_date_import_mode,average_pwr_factor_for_billing_period,cum_energy_kwh_import,cum_energy_kwh_tz1,cum_energy_kwh_tz2,cum_energy_kwh_tz3,cum_energy_kwh_tz4,cum_energy_kwh_tz5,cum_energy_kwh_tz6,cum_energy_kwh_tz7,cum_energy_kwh_tz8,cum_energy_kvah_import,cum_energy_kvah_tz1,cum_energy_kvah_tz2,cum_energy_kvah_tz3,cum_energy_kvah_tz4,cum_energy_kvah_tz5,cum_energy_kvah_tz6,cum_energy_kvah_tz7,cum_energy_kvah_tz8,max_demand_kw,max_demand_kva,md_kw_datetime,md_kva_datetime,cum_active_energy_kwh_export,cum_apparent_energy_kvah_export,billing_power_on_duration,last_download_time,cycle_id,push_alaram,error_code) VALUES('%s','%s','%s','%s',%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,'%s','%s',%.2f,%.2f,%.2f,'%s',%d, %d, %d)",

             /* Identification */
             mac_hex,
             &mac_hex[8],
             gateway_id,
             billing_date_import_mode.c_str(),

             /* kWh import */
             average_pwr_factor,
             cumulative_energy_import_kWh,
             cumulative_energy_TZ1_kWh,
             cumulative_energy_TZ2_kWh,
             cumulative_energy_TZ3_kWh,
             cumulative_energy_TZ4_kWh,
             cumulative_energy_TZ5_kWh,
             cumulative_energy_TZ6_kWh,
             cumulative_energy_TZ7_kWh,
             cumulative_energy_TZ8_kWh,

             /* kVAh import */
             cumulative_energy_import_kVAh,
             cumulative_energy_TZ1_kVAh,
             cumulative_energy_TZ2_kVAh,
             cumulative_energy_TZ3_kVAh,
             cumulative_energy_TZ4_kVAh,
             cumulative_energy_TZ5_kVAh,
             cumulative_energy_TZ6_kVAh,
             cumulative_energy_TZ7_kVAh,
             cumulative_energy_TZ8_kVAh,

             /* MD */
             MD_kW,
             MD_kVA,
             MD_kW_date_and_time.c_str(),
             MD_kVA_date_and_time.c_str(),

             /* Export + misc */
             cumulative_energy_export_kWh,
             cumulative_energy_export_kVAh,
             billing_power_on_duration,
             this->now().c_str(),
             0,
             push_status ? 1 : 0,
             push_status ? 1 : 0);

    return execute_query(query_buf);
}

std::string MySqlDatabase::get_event_code_string_from_event_code(uint16_t event_code)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[512] = {0};

    snprintf(query_buf, sizeof(query_buf), "SELECT event_string FROM dlms_event_codes WHERE event_code = %u", event_code);

    if (this->execute_query(query_buf))
        return "";

    MYSQL_RES *res = mysql_store_result(this->mysql);
    if (!res)
        return "";

    std::string event_string;

    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0])
    {
        event_string = row[0];
    }

    mysql_free_result(res);
    return event_string;
}

int MySqlDatabase::insert_update_hes_nms_sync_time(const char *gateway_mac, int status)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buffer[4096] = {0};

    snprintf(query_buffer, sizeof(query_buffer), "select gateway_aquired_by_nms FROM hes_nms_sync_time where gateway_id='%s' ", gateway_mac);

    if (this->execute_query(query_buffer))
    {
        return FAILURE;
    }

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (!result)
    {
        this->print_and_log("MySQL store result failed\n");
        return FAILURE;
    }

    int num_row = mysql_num_rows(result);

    if (num_row == 1)
    {
        MYSQL_ROW row = mysql_fetch_row(result);

        if (status == 1)
        {
            this->print_and_log("HES trying to acquire the network\n");
        }
        else
        {
            this->print_and_log("HES releasing the network\n");
        }

        this->print_and_log("NMS status: %d\n", atoi(row[0]));

        if ((status != 0) && (atoi(row[0]) == 1))
        {
            this->print_and_log("NMS acquired this gateway\n");
            mysql_free_result(result);
            return FAILURE;
        }

        snprintf(query_buffer, sizeof(query_buffer), "update hes_nms_sync_time set hes_last_rf_network_aquired_time='%s', gateway_aquired_by_hes='%d' where gateway_id='%s' ", this->now().c_str(), status, gateway_mac);
    }
    else
    {
        snprintf(query_buffer, sizeof(query_buffer), "insert into hes_nms_sync_time(gateway_id, gateway_aquired_by_hes, hes_last_rf_network_aquired_time) values('%s','%d','%s')", gateway_mac, status, this->now().c_str());
    }

    mysql_free_result(result);
    return this->execute_query(query_buffer);
}

#if PUSH_EVENT_DETAILED_PARSING

int MySqlDatabase::insert_power_on_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<EventPowerFailureRestoration> &power_on_event)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    uint32_t rtc = from_big_endian<uint32_t>(power_on_event.profile_data.real_time_clock.data.data);
    std::string real_time_clock = format_timestamp(rtc);

    uint8_t len = 0;
    uint8_t *src = nullptr;
    len = power_on_event.profile_data.meter_sl_number.data.length;
    src = power_on_event.profile_data.meter_sl_number.data.data;
    std::string meter_sl_number_str(reinterpret_cast<const char *>(src), len);

    len = power_on_event.profile_data.device_id.data.length;
    src = power_on_event.profile_data.device_id.data.data;
    std::string device_id_str(reinterpret_cast<const char *>(src), len);

    uint16_t voltage_event = power_on_event.profile_data.voltage.data;
    std::string voltage_event_string = this->get_event_code_string_from_event_code(voltage_event);

    uint16_t current_event = power_on_event.profile_data.current.data;
    std::string current_event_string = this->get_event_code_string_from_event_code(current_event);

    uint16_t power_event = power_on_event.profile_data.power.data;
    std::string power_event_string = this->get_event_code_string_from_event_code(power_event);

    uint16_t transaction_event = power_on_event.profile_data.transaction.data;
    std::string transaction_event_string = this->get_event_code_string_from_event_code(transaction_event);

    uint16_t other_event = power_on_event.profile_data.other_event.data;
    std::string other_event_string = this->get_event_code_string_from_event_code(other_event);

    uint16_t non_roll_over_event = power_on_event.profile_data.non_roll_over.data;
    std::string non_roll_over_event_string = this->get_event_code_string_from_event_code(non_roll_over_event);

    uint16_t control_event = power_on_event.profile_data.control.data;
    std::string control_event_string = this->get_event_code_string_from_event_code(control_event);

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO push_on_off_events (gateway_id, Push_type, Event_code, Device_id, RF_mac_address, RTC, Uploaded_time) VALUES ('%s', %u, %u, '%s', '%s', '%s', '%s')", gateway_id, 1, power_event, device_id_str.c_str(), mac_hex, real_time_clock.c_str(), this->now().c_str());

    if (execute_query(query_buf) != 0)
    {
        this->print_and_log("push_on_off_events insert failed\n");
        return FAILURE;
    }

    memset(query_buf, 0, sizeof(query_buf));

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO Push_latest_event_alarm (gateway_id, Device_id, RF_mac_address, Voltage_event_code, Voltage_event_string, Current_event_code, Current_event_string, Power_event_code, Power_event_string, Transaction_event_code, Transaction_event_string, Other_event_code, Other_event_string, Non_roll_over_event_code, Non_roll_over_event_string, Control_event_code, Control_event_string, RTC, Uploaded_time) VALUES ('%s','%s','%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s','%s','%s')", gateway_id, device_id_str.c_str(), mac_hex, voltage_event, voltage_event_string.c_str(), current_event, current_event_string.c_str(), power_event, power_event_string.c_str(), transaction_event, transaction_event_string.c_str(), other_event, other_event_string.c_str(), non_roll_over_event, non_roll_over_event_string.c_str(), control_event, control_event_string.c_str(), real_time_clock.c_str(), this->now().c_str());

    return execute_query(query_buf);
}

int MySqlDatabase::insert_power_off_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<EventPowerFailureOccurance> &power_off_event)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    uint8_t len = 0;
    uint8_t *src = nullptr;
    len = power_off_event.profile_data.meter_sl_number.data.length;
    src = power_off_event.profile_data.meter_sl_number.data.data;
    std::string meter_sl_number_str(reinterpret_cast<const char *>(src), len);

    len = power_off_event.profile_data.device_id.data.length;
    src = power_off_event.profile_data.device_id.data.data;
    std::string device_id_str(reinterpret_cast<const char *>(src), len);

    uint16_t power_event = power_off_event.profile_data.power.data;
    // std::string power_event_string = this->get_event_code_string_from_event_code(power_event);

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO push_on_off_events (gateway_id, Push_type, Event_code, Device_id, RF_mac_address, RTC, Uploaded_time) VALUES ('%s', %u, %u, '%s', '%s', '%s', '%s')", gateway_id, 1, power_event, device_id_str.c_str(), mac_hex, "-", this->now().c_str());

#if 0
    
    if (execute_query(query_buf) != 0)
    {
        this->print_and_log("push_on_off_events insert failed\n");
        return FAILURE;
    }

    memset(query_buf, 0, sizeof(query_buf));

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO Push_latest_event_alarm (gateway_id, Device_id, RF_mac_address, Voltage_event_code, Voltage_event_string, Current_event_code, Current_event_string, Power_event_code, Power_event_string, Transaction_event_code, Transaction_event_string, Other_event_code, Other_event_string, Non_roll_over_event_code, Non_roll_over_event_string, Control_event_code, Control_event_string, RTC, Uploaded_time) VALUES ('%s','%s','%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s','%s','%s')", gateway_id, device_id_str.c_str(), mac_hex, voltage_event, voltage_event_string.c_str(), current_event, current_event_string.c_str(), power_event, power_event_string.c_str(), transaction_event, transaction_event_string.c_str(), other_event, other_event_string.c_str(), non_roll_over_event, non_roll_over_event_string.c_str(), control_event, control_event_string.c_str(), real_time_clock.c_str(), this->now().c_str());

#endif

    return execute_query(query_buf);
}

#else // PUSH_EVENT_DETAILED_PARSING

int MySqlDatabase::insert_power_on_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &power_on_event)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    const DlmsRecordMap &rec = power_on_event.profile_data;

    auto rtc_raw = DlmsValueExtractor::get_numeric_or_default(rec, 0x00);
    rtc_raw -= SECONDS_5H30M;
    std::string real_time_clock = format_timestamp(static_cast<uint32_t>(rtc_raw));

    auto meter_sl_number = DlmsValueExtractor::get_octet_string(rec, 0x01);
    std::string meter_sl_number_str(meter_sl_number.begin(), meter_sl_number.end());

    auto device_id = DlmsValueExtractor::get_octet_string(rec, 0x02);
    std::string device_id_str(device_id.begin(), device_id.end());

    // auto event_status_word = DlmsValueExtractor::get_octet_string(rec, 0x03);
    // std::string event_status_word_str(event_status_word.begin(), event_status_word.end());

    uint16_t voltage_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x04));
    std::string voltage_event_string = this->get_event_code_string_from_event_code(voltage_event);

    uint16_t current_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x05));
    std::string current_event_string = this->get_event_code_string_from_event_code(current_event);

    uint16_t power_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x06));
    std::string power_event_string = this->get_event_code_string_from_event_code(power_event);

    uint16_t transaction_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x07));
    std::string transaction_event_string = this->get_event_code_string_from_event_code(transaction_event);

    uint16_t other_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x08));
    std::string other_event_string = this->get_event_code_string_from_event_code(other_event);

    uint16_t non_roll_over_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x09));
    std::string non_roll_over_event_string = this->get_event_code_string_from_event_code(non_roll_over_event);

    uint16_t control_event = static_cast<uint16_t>(DlmsValueExtractor::get_numeric_or_default(rec, 0x0A));
    std::string control_event_string = this->get_event_code_string_from_event_code(control_event);

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO push_on_off_events (gateway_id, Push_type, Event_code, Device_id, RF_mac_address, RTC, Uploaded_time) VALUES ('%s', %u, %u, '%s', '%s', '%s', '%s')", gateway_id, 1, power_event, device_id_str.c_str(), mac_hex, real_time_clock.c_str(), this->now().c_str());

    if (execute_query(query_buf) != 0)
    {
        this->print_and_log("push_on_off_events insert failed\n");
        return FAILURE;
    }

    memset(query_buf, 0, sizeof(query_buf));

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO Push_latest_event_alarm (gateway_id, Device_id, RF_mac_address, Voltage_event_code, Voltage_event_string, Current_event_code, Current_event_string, Power_event_code, Power_event_string, Transaction_event_code, Transaction_event_string, Other_event_code, Other_event_string, Non_roll_over_event_code, Non_roll_over_event_string, Control_event_code, Control_event_string, RTC, Uploaded_time) VALUES ('%s','%s','%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s','%s','%s')", gateway_id, device_id_str.c_str(), mac_hex, voltage_event, voltage_event_string.c_str(), current_event, current_event_string.c_str(), power_event, power_event_string.c_str(), transaction_event, transaction_event_string.c_str(), other_event, other_event_string.c_str(), non_roll_over_event, non_roll_over_event_string.c_str(), control_event, control_event_string.c_str(), real_time_clock.c_str(), this->now().c_str());

    return execute_query(query_buf);
}

int MySqlDatabase::insert_power_off_event_to_db(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, PacketBuffer<DlmsRecordMap> &power_off_event)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    const DlmsRecordMap &rec = power_off_event.profile_data;

    auto meter_sl_number = DlmsValueExtractor::get_octet_string(rec, 0x00);
    std::string meter_sl_number_str(meter_sl_number.begin(), meter_sl_number.end());

    auto device_id = DlmsValueExtractor::get_octet_string(rec, 0x01);
    std::string device_id_str(device_id.begin(), device_id.end());

    uint16_t power_event = DlmsValueExtractor::get_numeric_or_default(rec, 0x02);

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO push_on_off_events (gateway_id, Push_type, Event_code, Device_id, RF_mac_address, RTC, Uploaded_time) VALUES ('%s', %u, %u, '%s', '%s', '%s', '%s');", gateway_id, 1, power_event, device_id_str.c_str(), mac_hex, "-", this->now().c_str());

#if 0
    
    if (execute_query(query_buf) != 0)
    {
        this->print_and_log("push_on_off_events insert failed\n");
        return FAILURE;
    }

    memset(query_buf, 0, sizeof(query_buf));

    snprintf(query_buf, sizeof(query_buf), "INSERT INTO Push_latest_event_alarm (gateway_id, Device_id, RF_mac_address, Voltage_event_code, Voltage_event_string, Current_event_code, Current_event_string, Power_event_code, Power_event_string, Transaction_event_code, Transaction_event_string, Other_event_code, Other_event_string, Non_roll_over_event_code, Non_roll_over_event_string, Control_event_code, Control_event_string, RTC, Uploaded_time) VALUES ('%s','%s','%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s',%u,'%s','%s','%s')", gateway_id, device_id_str.c_str(), mac_hex, voltage_event, voltage_event_string.c_str(), current_event, current_event_string.c_str(), power_event, power_event_string.c_str(), transaction_event, transaction_event_string.c_str(), other_event, other_event_string.c_str(), non_roll_over_event, non_roll_over_event_string.c_str(), control_event, control_event_string.c_str(), real_time_clock.c_str(), this->now().c_str());

#endif

    return execute_query(query_buf);
}

#endif // PUSH_EVENT_DETAILED_PARSING

int MySqlDatabase::update_internal_firmware_version_in_meter_details(std::array<uint8_t, 8> node_mac_address, const char *gateway_id, uint8_t *internal_firmware_version)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buf[4096] = {0};

    char mac_hex[17] = {0};
    PullData::mac_to_hex(node_mac_address, mac_hex);

    snprintf(query_buf, sizeof(query_buf), "UPDATE meter_details SET rf_module_internal_fw_version = '%s' WHERE meter_mac_address = '%s' AND gateway_id = '%s';", internal_firmware_version, mac_hex, gateway_id);

    return execute_query(query_buf);
}

bool MySqlDatabase::update_dlms_mqtt_info(char *gateway_id, uint8_t status)
{

    char query_buffer[1024] = {0}; // SQL query construction buffer

    std::string time_str;

    Client::client_get_time(time_str, 2);

    if (status == 0)
    {

        // update dcu_status_info in DB
        sprintf(query_buffer, "UPDATE dlms_mqtt_info SET last_disconnected_time = '%s' , mqtt_sub_status = %d WHERE gateway_id = '%s';", time_str.c_str(), 0, gateway_id); // Update: timestamp + active status

        // Execute UPDATE query
        if (this->execute_query(query_buffer) == FAILURE)
        {
            this->print_and_log("❌ Failed to execute Update query.\n"); // Note: Log says INSERT but it's UPDATE
            return FAILURE;
        }
        return SUCCESS;
    }
    else if (status == 1)
    {

        MYSQL_RES *result = nullptr;

        // Query 1: Check if THIS gateway exists (efficient!)
        snprintf(query_buffer, sizeof(query_buffer), "SELECT 1 FROM dlms_mqtt_info WHERE gateway_id = '%s' LIMIT 1;", gateway_id);

        if (this->execute_query(query_buffer) == FAILURE)
        {
            this->print_and_log("❌ Gateway check query failed\n");
            return FAILURE;
        }

        result = mysql_store_result(this->mysql);

        if (result == nullptr)
        {
            mysql_free_result(result);
            return false;
        }

        bool gateway_exists = (mysql_num_rows(result) > 0);

        memset(query_buffer, 0, sizeof(query_buffer));

        if (gateway_exists)
        {

            // Construct UPDATE query for matched DCU record
            sprintf(query_buffer, "UPDATE dlms_mqtt_info SET last_connected_time = '%s' , mqtt_sub_status = '%d' WHERE gateway_id = '%s';", time_str.c_str(), 1, gateway_id); // Update: timestamp + active status
                                                                                                                                                                              // Execute UPDATE query
            if (this->execute_query(query_buffer) == FAILURE)
            {
                this->print_and_log("❌ Failed to execute INSERT query.\n"); // Note: Log says INSERT but it's UPDATE
                return FAILURE;
            }
        }
        else
        {
            // INSERT new gateway record
            sprintf(query_buffer, "INSERT INTO dlms_mqtt_info (last_connected_time, mqtt_sub_status,gateway_id) values('%s','%d','%s');", time_str.c_str(), 1, gateway_id); // Update: timestamp + active status
            // Execute UPDATE query
            if (this->execute_query(query_buffer) == FAILURE)
            {
                this->print_and_log("❌ Failed to execute INSERT query.\n"); // Note: Log says INSERT but it's UPDATE
                return FAILURE;
            }
        }
        mysql_free_result(result);
        return SUCCESS; // Success: DCU status updated
    }

    this->print_and_log("NO gateway_id found\n");
    return FAILURE;
}

void MySqlDatabase::load_scaler_details_from_db(const std::string &meter_serial_no, char *gateway_id, Client *Client)
{
    std::string meter_manufacture_name;
    std::string FW_version;
    std::string attribute_id;
    //  Check if meter serial exists
    auto meter_it = Client->meter_info_map.find(meter_serial_no);
    if (meter_it != Client->meter_info_map.end())
    {
        return; // Meter serial found, no need to load
    }

    char query_buffer[512];

    // ✅ STEP 1: Fetch manufacturer name for this meter serial number
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT meter_manufacture_name,meter_firmware_version "
             "FROM meter_details "
             "WHERE gateway_id='%s' AND meter_address='%s';",
             gateway_id, meter_serial_no.c_str());

    int qres = this->execute_query(query_buffer);
    if (qres == FAILURE)
        return;

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (!result)
        return;

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row)
    {
        mysql_free_result(result);
        this->print_and_log("[DB] No manufacturer found for meter serial: %s\n", meter_serial_no.c_str());
        return;
    }

    meter_manufacture_name = std::string(row[0] ? row[0] : "");
    FW_version = std::string(row[1] ? row[1] : "");

    mysql_free_result(result);

    this->print_and_log("[DB] Found manufacturer '%s' for meter serial: %s\n",
                        meter_manufacture_name.c_str(), meter_serial_no.c_str());

    // ✅ STEP 3: Fetch ALL scalar values for this manufacturer/profile
    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT meter_supported_index, scalar_value,attribute_id "
             "FROM meter_supported_attributes "
             "WHERE manufacturer_name='%s' "
             "ORDER BY meter_supported_index;",
             meter_manufacture_name.c_str());

    int qres2 = this->execute_query(query_buffer);
    if (qres2 == FAILURE)
        return;

    MYSQL_RES *scalar_result = mysql_store_result(this->mysql);
    if (!scalar_result)
        return;

    // ✅ STEP 4: Parse results → store in manufacture_details
    MYSQL_ROW scalar_row;
    while ((scalar_row = mysql_fetch_row(scalar_result)) != NULL)
    {
        uint8_t index_byte = static_cast<uint8_t>(atoi(scalar_row[0])); // meter_supported_index
        int32_t scalar_val = static_cast<int32_t>(atoi(scalar_row[1])); // scalar_value
        attribute_id = std::string(scalar_row[2] ? scalar_row[2] : ""); // attribute_id

        // Store in map: {"0100", index_byte} → scalar_val
        Client->manufacture_details[{attribute_id.c_str(), index_byte}] = scalar_val;
    }
    mysql_free_result(scalar_result);

    // ✅ STEP 5: Call addManufacturer with loaded data
    if (!Client->manufacture_details.empty())
    {
        Client->addManufacturer(meter_manufacture_name, Client->manufacture_details);
    }
    else
    {
        this->print_and_log("[DB] No scalar data found for %s\n",
                            meter_manufacture_name.c_str());
    }
    get_meter_details_from_db(meter_manufacture_name, FW_version, gateway_id);
}

int MySqlDatabase::check_for_scaler_before_insert(char *meter_manufacture_name, char *meter_fw_version, int profile_name)
{
    this->print_and_log("%s start\n", __FUNCTION__);

    char query_buffer[512] = {0};

    MYSQL_RES *result = nullptr;

    char attribute_id_hex[5] = {0}; // "0100\0"

    snprintf(attribute_id_hex, sizeof(attribute_id_hex), "%02X00", profile_name);

    snprintf(query_buffer, sizeof(query_buffer),
             "SELECT COUNT(*) FROM meter_supported_attributes WHERE manufacturer_name='%s' and firmware_version='%s' and attribute_id = %s;",
             meter_manufacture_name, meter_fw_version, attribute_id_hex);

    if (this->execute_query(query_buffer) == FAILURE)
    {
        this->print_and_log("Query executed Failed.\n");
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);
    if (result && mysql_num_rows(result) > 0)
    {
        MYSQL_ROW row = mysql_fetch_row(result);
        int row_count = atoi(row[0]);
        mysql_free_result(result);

        if (row_count > 0)
        {
            this->print_and_log("✅ [%s] [%s] [%s] Scaler already exists in DB\n", meter_manufacture_name, meter_fw_version, attribute_id_hex);
            return SUCCESS;
        }
        else
        {
            return FAILURE;
        }
    }
    return FAILURE;
}

float MySqlDatabase::convertScalar(int32_t raw)
{
    return std::pow(10.0f, raw); // -1 → 0.1, -2 → 0.01, 0-> 1.0 ,1 → 10, 2 → 100, 3 → 1000, etc.
}

int32_t MySqlDatabase::getScalarValue(const std::string &meter_serial_no, const std::string &attribute_id, uint8_t index)
{
    auto meter_it = this->meter_info_map.find(meter_serial_no);
    if (meter_it != this->meter_info_map.end())
    {
        // this->print_and_log("[STAGE1]\n");
        MeterInfo info = meter_it->second;
        std::string manufacturer = info.manufacturer;
        // this->print_and_log("[MANUFACTURER] = %s\n", manufacturer.c_str());
        auto manufacturer_it = this->manufacturer_scalar_map.find(manufacturer);
        if (manufacturer_it != this->manufacturer_scalar_map.end())
        {
            // this->print_and_log("[STAGE2]\n");
            auto attr_it = manufacturer_it->second.scalar_values.find({attribute_id, index});
            if (attr_it != manufacturer_it->second.scalar_values.end())
            {
                // this->print_and_log("[STAGE3]\n");
                return attr_it->second;
            }
        }
    }

    this->print_and_log("[N0 value]\n");
    return 0; // Default value
}

/*.................................Added BY LHK on 20112025......
 *........FUOTA Dependent Functions..............................*/

// Get the alternate path at timeout try then back to primary path

int MySqlDatabase::get_alternate_source_route_network_from_db(char *query_buffer, unsigned char *dcuid, unsigned char *target_mac, Fuota *pan_list_ptr)
{
    if (!dcuid || !target_mac || !pan_list_ptr)
        return -1;

    this->print_and_log("Func: get_alternate_sourceroutenetwork_from_db\n");

    // convert binary target_mac -> ASCII hex for DB compare
    std::string target_hex = Utility::bin_to_hex_upper(target_mac, 8);

    // construct query: order by hop_count ascending, limit 2
    snprintf(query_buffer, 1024, "SELECT target_mac_address, hop_count, path FROM alternate_source_route_network WHERE gateway_id = '%s' ORDER BY hop_count ASC LIMIT 2;", dcuid);

    this->print_and_log("Execute Query:%s\n ", query_buffer);

    if (this->connect_to_mysql() == false)
        return -1;

    int rc = mysql_query(this->mysql, query_buffer);

    if (rc != 0)
    {
        std::cout << "Error Msg:" << mysql_error(this->mysql) << std::endl;
        return -1;
    }
    else
    {
        std::cout << "SQL Query Return Value = " << rc << std::endl;
    }

    MYSQL_RES *result = mysql_store_result(this->mysql);
    std::cout << "result = " << result << std::endl;

    int num_row = mysql_num_rows(result);
    cout << "Rows fetched: " << num_row << endl;

    // reset linked list
    pan_list_ptr->alternatepaths.reset();

    if (num_row > 0)
    {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result)) != nullptr)
        {
            if (!row[0] || !row[1] || !row[2])
            {
                cout << "Skipping row with null fields" << endl;
                continue;
            }

            // row[0] is ASCII hex target_mac (should be 16 chars)
            // row[1] is hop_count as string
            // row[2] is path ascii hex (length should be hop_count * 16 chars)

            int hop_count = atoi(row[1]);
            if (hop_count <= 0 || hop_count > 128) // sanity bound
            {
                cout << "Invalid hop_count: " << hop_count << endl;
                continue;
            }

            size_t expected_hex_len = (size_t)hop_count * 16; // 16 hex chars per hop (8 bytes)
            size_t actual_hex_len = strlen(row[2]);
            if (actual_hex_len < expected_hex_len)
            {
                cout << "Invalid path length: actual " << actual_hex_len << " expected " << expected_hex_len << endl;
                continue;
            }

            uint8_t node_mac_address[8] = {0};
            uint8_t path_data[512] = {0}; // enough for many hops (512 bytes => 64 hops)
            // convert target_mac ascii hex => 8 bytes
            Utility::aschexstr2hex(node_mac_address, row[0], 8);

            // convert path ascii hex => hop_count * 8 bytes
            size_t bytes_required = (size_t)hop_count * 8;
            Utility::aschexstr2hex(path_data, row[2], bytes_required);

            // insert into pan_list
            pan_list_ptr->insert_alternate_path(node_mac_address, path_data, (unsigned char)hop_count);
        }
    }

    if (num_row > 0)
    {
        pan_list_ptr->print_alternate_paths();
    }
    else
    {
        mysql_free_result(result);
        return FAILURE;
    }
    mysql_free_result(result);
    return e_success_0;
}

/*
 * To get the rf meter previous firmware version
 */
int MySqlDatabase::get_rf_internal_firmware_version_from_meter_details(char *query_buffer, unsigned char *dcuid, unsigned char *meter_mac_addreess, char *fw_version_out)
{
    this->print_and_log("get_rf_internal_firmwareversion_from_meter_details() invoked\n");

    if (!dcuid || !fw_version_out || !query_buffer || !meter_mac_addreess)
    {
        std::cout << "Invalid arguments passed!" << std::endl;
        return -1;
    }
    char standard_buf[17] = {0};
    // Clear output buffer safely (assuming caller allocates enough space)
    fw_version_out[0] = '\0';
    printf("----------------mac address---------\n");
    for (int i = 0; i < 8; i++)
    {
        printf("%02X ", meter_mac_addreess[i]);
    }
    cout << endl;

    memset(standard_buf, 0, sizeof(standard_buf));
    memcpy(standard_buf, "3CC1F601", strlen("3CC1F601"));

    // Append remaining bytes as hex
    sprintf(standard_buf + 8, "%02X%02X%02X%02X", meter_mac_addreess[4], meter_mac_addreess[5], meter_mac_addreess[6], meter_mac_addreess[7]);

    // Build query safely
    sprintf(query_buffer, "SELECT DISTINCT rf_module_internal_fw_version FROM meter_details  WHERE meter_mac_address='%s' AND gateway_id='%s';", standard_buf, dcuid);

    this->print_and_log("Query: %s\n", query_buffer);
    std::cout << query_buffer << std::endl;

    // Validate MySQL connection
    if (!this->mysql || mysql_ping(this->mysql) != 0)
    {
        std::cout << "MySQL connection lost. Reconnecting..." << std::endl;
        if (!this->connect_to_mysql())
        {
            std::cout << "Failed to reconnect to MySQL." << std::endl;
            return -1;
        }
    }

    // Execute the query
    if (mysql_query(this->mysql, query_buffer) != 0)
    {
        std::cout << "MySQL Query Error: " << mysql_error(this->mysql) << std::endl;
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (!result)
    {
        std::cout << "Error storing result: " << mysql_error(this->mysql) << std::endl;
        return -1;
    }

    if (mysql_num_rows(result) > 0)
    {
        MYSQL_ROW row = mysql_fetch_row(result);
        if (row && row[0])
        {
            strncpy(fw_version_out, row[0], 15);
            fw_version_out[15] = '\0';
            std::cout << "RF Internal Firmware Version: " << fw_version_out << std::endl;
        }
    }

    mysql_free_result(result);
    return SUCCESS;
}

/*
 * Get the serialnumber and panid from the gatewayid
 */
int MySqlDatabase::get_serial_number_panid_for_gateway(char *query_buffer, unsigned char *gateway_id, unsigned char *meter_mac_address, char *panid, char *serial_number)
{
    this->print_and_log("get_serialnumber/panid_forgateway invoked\n");

    if (!gateway_id || !panid || !serial_number || !query_buffer)
    {
        std::cout << "Invalid arguments!" << std::endl;
        return -1;
    }

    // Clear outputs
    panid[0] = '\0';
    serial_number[0] = '\0';

    // SQL: Only pan_id + meter_mac from table
    sprintf(query_buffer, "SELECT pan_id, meter_mac_address FROM gateway_network_details WHERE gateway_id='%s' AND meter_mac_address='%s';", gateway_id, gateway_id);

    std::cout << query_buffer << std::endl;

    // Check DB connection
    if (!this->mysql || mysql_ping(this->mysql) != 0)
    {
        if (!this->connect_to_mysql())
        {
            std::cout << "MySQL reconnect failed\n";
            return -1;
        }
    }

    if (mysql_query(this->mysql, query_buffer) != 0)
    {
        std::cout << "MySQL Query Error: " << mysql_error(this->mysql) << std::endl;
        return -1;
    }

    MYSQL_RES *result = mysql_store_result(this->mysql);
    if (!result)
    {
        std::cout << "Error storing result: " << mysql_error(this->mysql) << std::endl;
        return -1;
    }

    if (mysql_num_rows(result) > 0)
    {
        MYSQL_ROW row = mysql_fetch_row(result);

        // row[0] → pan_id
        if (row[0])
        {
            strncpy(panid, row[0], 8);
            panid[8] = '\0';
            std::cout << "panid: " << panid << std::endl;
        }

        // serial_number = last 8 characters of meter_mac_address
        if (meter_mac_address)
        {
            int len = strlen((char *)meter_mac_address);
            if (len >= 8)
            {
                strncpy(serial_number, (char *)meter_mac_address + (len - 8), 8);
                serial_number[8] = '\0';
            }
        }

        std::cout << "serial_number: " << serial_number << std::endl;
    }

    mysql_free_result(result);
    return SUCCESS;
}

int MySqlDatabase::Arrange_route_path(unsigned char *source, unsigned char *dest, short int hop_count)
{
    this->print_and_log("Arrange_route_path function invoed\n");
    size_t dest_len = (hop_count > 0) ? (size_t)hop_count * 4u : 0u;
    if (dest_len)
        memset(dest, 0, dest_len);
    unsigned char dest_data[8];
    char source_data[10];
    short int track_index = 8, track_index1 = 0;
    for (short int i = 0; i < hop_count; i++)
    {
        memset(&dest_data[0], 0, sizeof(dest_data));
        memset(&source_data[0], 0, sizeof(source_data));

        memcpy(&source_data[0], &source[track_index], 8);

        Utility::aschexstr2hex(dest_data, source_data, 4);
        memcpy(&dest[track_index1], dest_data, 4);
        track_index1 += 4;
        track_index += 16;
    }

    printf("4 byte Route is % d\n", track_index1);
    for (short int i = 0; i < track_index1; i++)
    {
        printf("%02X ", dest[i]);
    }
    printf("\n");
    printf("***************\n");

    return 0;
}

int MySqlDatabase::check_gateway_nodelist_empty_or_not(unsigned char *dcuid)
{
    this->print_and_log("Function Invoked: checkgateway_nodelist_empty_or_not\n");

    MYSQL_RES *result = nullptr;
    MYSQL_ROW row;
    char query_buffer[512];

    sprintf(query_buffer, "SELECT COUNT(*) FROM source_route_network WHERE gateway_id = '%s' AND disconnected_from_gateway = 0", dcuid);

    this->print_and_log("Executing Query : %s\n", query_buffer);

    if (!this->connect_to_mysql())
        return -1;

    if (mysql_query(this->mysql, query_buffer) != 0)
    {
        this->print_and_log("MySQL Query Failed: %s\n", mysql_error(this->mysql));
        return -1;
    }

    result = mysql_store_result(this->mysql);
    if (!result)
    {
        this->print_and_log("mysql_store_result returned NULL\n");
        return -1;
    }

    row = mysql_fetch_row(result);
    if (!row || !row[0])
    {
        mysql_free_result(result);
        return -1;
    }

    int count = atoi(row[0]);
    mysql_free_result(result);

    this->print_and_log("Gateway Under Nodes count = %d\n", count);

    /*
     * Requirement:
     * rows present  -> return 0
     * no rows       -> return 1
     */
    if (count > 0)
        return 0; // nodes present
    else
        return 1; // node list empty
}

/*
 * To get the Routers list under gateway meter
 */
int MySqlDatabase::get_the_gateway_undernodes_details_for_fuota(char *query_buffer, unsigned char *dcuid, Fuota *pan_list_ptr)
{

    this->print_and_log("Function invoked: get_the_gatewayundernodes_details_for_fuota\n");

    MYSQL_RES *result = nullptr;
    MYSQL_ROW row;
    int rc = 0;

    sprintf(query_buffer, "SELECT target_mac_address, path, hop_count FROM source_route_network WHERE gateway_id ='%s' and disconnected_from_gateway = 0", dcuid);

    this->print_and_log("Executing Query : %s\n", query_buffer);

    if (!this->connect_to_mysql())
        return -1;

    rc = mysql_query(this->mysql, query_buffer);
    if (rc != 0)
    {
        this->print_and_log("MySQL Query Failed: %s\n", mysql_error(this->mysql));
        return -1;
    }
    else
    {

        this->print_and_log("SQL Query Return Value = %d\n", rc);
    }

    result = mysql_store_result(this->mysql);
    if (!result)
    {
        this->print_and_log("mysql_store_result returned NULL\n");
        return FAILURE;
    }

    int no_row = mysql_num_rows(result);

    this->print_and_log("Meters found in sourceroute_network:%d\n ", no_row); // 26122025

    if (!pan_list_ptr->gate_node)
    {
        pan_list_ptr->gate_node = new gateway_details();
    }

    pan_list_ptr->gate_node->meter_list = new vector<struct meter_vital_info *>();

    if (!pan_list_ptr->gate_node->meter_list)
    {
        this->print_and_log("Memory allocation failed for meter list\n");
        return -1;
    }

    if (no_row > 0)
    {
        pan_list_ptr->gate_node->total_meter_under_gatway = no_row;
    }
    else
    {
        std::cout << "No meters found under gateway, Then it's Gateway only Fuota upload state!" << std::endl;
        pan_list_ptr->gate_node->total_meter_under_gatway = 0;
        mysql_free_result(result);
        return e_success_0;
    }
    unsigned char dest_data[10] = {0};
    char source_data[20] = {0};
    unsigned char route_data[100] = {0};

    while ((row = mysql_fetch_row(result)) != NULL)
    {
        if (!row[0] || !row[1] || !row[2])
            continue;

        struct meter_vital_info *node_info = (struct meter_vital_info *)new meter_vital_info();
        if (!node_info)
        {
            this->print_and_log("Failed to allocate memory for node_info\n");
            continue;
        }

        memset(node_info, 0, sizeof(struct meter_vital_info));
        memcpy(source_data, row[0], 16);                   // MAC as ASCII
        memcpy(node_info->meter_serial_number, row[0], 8); // Store first 8 bytes as serial

        Utility::aschexstr2hex(dest_data, source_data, 8); // Convert ASCII to hex
        memcpy(node_info->meter_mac_address, dest_data, 8);

        node_info->hop_count = atoi(row[2]);

        if (node_info->hop_count > 0)
        {
            memcpy(node_info->route_path, row[1], node_info->hop_count * 16);

            this->Arrange_route_path(node_info->route_path, route_data, node_info->hop_count);

            memset(node_info->route_path, 0, sizeof(node_info->route_path));
            memcpy(node_info->route_path, route_data, node_info->hop_count * 4);
        }

        pan_list_ptr->gate_node->meter_list->push_back(node_info);
    }

    mysql_free_result(result);

    // Print result
    this->print_and_log("------- Gateway Node Route Info -------\n");

    for (auto &meter : *(pan_list_ptr->gate_node->meter_list))
    {
        printf("Target MAC: ");
        for (int i = 0; i < 8; i++)
            printf("%02X ", meter->meter_mac_address[i]);
        printf("\nHop Count: %d\n", meter->hop_count);
        printf("Route Path: ");
        for (int i = 0; i < meter->hop_count * 4; i++)
            printf("%02X ", meter->route_path[i]);
        printf("\n");
    }
    this->print_and_log("------- Gateway Node Route Info List End-------\n");
    return e_success_0;
}

/*
 * During fuota state silenced meters count,serial number ,hop and status of node(i.e. it's silence successfuly or not)
 */
int MySqlDatabase::insert_update_fuota_silenced_meter_details_in_db(char *query_buffer, unsigned char *meter_mac, unsigned char *dcuid, int status, int hopCount)
{
    if (!this->connect_to_mysql())
        return -1;
    std::string mac_str = Utility::mac_to_string_nocolon(meter_mac);
    // UPSERT query
    sprintf(query_buffer, "INSERT INTO silenced_nodes_for_fuota (gateway_id, meter_mac_address, hop_count, Fuota_status)VALUES('%s', '%s', % d, % d) ON DUPLICATE KEY UPDATE hop_count = VALUES(hop_count), Fuota_status = VALUES(Fuota_status),last_update_time = NOW();", dcuid, mac_str.c_str(), hopCount, status);
    this->print_and_log("Executing Query: %s", query_buffer);

    if (mysql_query(this->mysql, query_buffer) != 0)
        return -2;

    // Update total silenced count Fuota_status = 1
    sprintf(query_buffer, "UPDATE silenced_nodes_for_fuota AS t JOIN (SELECT gateway_id, COUNT(*) AS cnt FROM silenced_nodes_for_fuota  WHERE Fuota_status = 1 GROUP BY gateway_id) AS x ON t.gateway_id = x.gateway_id  SET t.silenced_nodes_count = x.cnt;");
    this->print_and_log("Updating total silenced nodes count with query: %s\n", query_buffer);
    if (mysql_query(this->mysql, query_buffer) != 0)
        return -3;

    return 0; // success
}

/*
 * Update the unsilenced meters serial number , hopcount , list of nodes count and status of nodes(i.e. it's unsilence successfuly or not )
 */

int MySqlDatabase::insert_update_fuota_Unsilenced_meter_details_in_db(char *query_buffer, unsigned char *meter_mac, unsigned char *dcuid, int status, int hopCount)
{
    if (!this->connect_to_mysql())
        return -1;
    if (hopCount != 0)
    {
        std::string mac_str = Utility::mac_to_string_nocolon(meter_mac);
        printf("DB->Leaf mac is: %s\n", mac_str.c_str());
        // UPSERT query
        sprintf(query_buffer, "INSERT INTO unsilenced_nodes_for_fuota (gateway_id, meter_mac_address, hop_count, Fuota_status)VALUES('%s', '%s', % d, % d) ON DUPLICATE KEY UPDATE hop_count = VALUES(hop_count), Fuota_status = VALUES(Fuota_status),last_update_time = NOW();", dcuid, mac_str.c_str(), hopCount, status);
    }
    else
    {
        // UPSERT query for gateway or self node
        sprintf(query_buffer, "INSERT INTO unsilenced_nodes_for_fuota (gateway_id, meter_mac_address, hop_count, Fuota_status)VALUES('%s', '%s', % d, % d) ON DUPLICATE KEY UPDATE hop_count = VALUES(hop_count), Fuota_status = VALUES(Fuota_status),last_update_time = NOW();", dcuid, dcuid, hopCount, status);
    }

    this->print_and_log("Executing Query: %s\n", query_buffer);

    if (mysql_query(this->mysql, query_buffer) != 0)
        return -2;

    // Update total Unsilenced count Fuota_status = 0
    sprintf(query_buffer, "UPDATE unsilenced_nodes_for_fuota AS t JOIN (SELECT gateway_id, COUNT(*) AS cnt FROM unsilenced_nodes_for_fuota  WHERE Fuota_status = 0 GROUP BY gateway_id) AS x ON t.gateway_id = x.gateway_id  SET t.unsilenced_nodes_count = x.cnt;");
    this->print_and_log("Updating total Unsilenced nodes count with query: %s", query_buffer);
    if (mysql_query(this->mysql, query_buffer) != 0)
        return -3;

    return 0; // success
}

/*
 * during Fuota Update the  progressing statuses
 */
int MySqlDatabase::update_ondemand_RF_Fuota_upload_status(char *query_buffer, unsigned int req_id, int status, int error_code)
{
    this->print_and_log("update_ondemand_fuotaupload_status Functionis invoked!\n");
    std::string server_time;
    int ret = 0;

    Client::client_get_time(server_time, 2);

    sprintf(&query_buffer[0], "update dlms_fuota_upload set upload_status = %d,error_code = %d,uploaded_time = '%s' where request_id = %d", status, error_code, server_time, req_id);

    this->print_and_log("QUERY RFFUOTA STS:%s\n", (unsigned char *)query_buffer);

    ret = this->execute_query(query_buffer);
    this->print_and_log("ret update_ondemand fuotaupload_status = %d\n", ret);
    return ret;
}

/*
 * Once fuota done/Failure reset the fuota sync status flag for further requests to be allowed
 */
int MySqlDatabase::update_fuota_upload_sync_status(char *query_buffer, unsigned char *gateway_id)
{
    this->print_and_log("update_fuota_uploadsync_status Function is invoked!\n");

    std::string server_time;
    Client::client_get_time(server_time, 2); // Expected format: YYYY-MM-DD HH:MM:SS

    int rc = 0;
    char gateway_id_str[17] = {0}; // 16 chars + null terminator
    memcpy(gateway_id_str, gateway_id, 16);
    gateway_id_str[16] = '\0'; // Safe null-termination

    // 17102025 - Replaced Variable 'fuota_status' by '0'
    sprintf(query_buffer, "UPDATE odm_fuota_status_flag SET fuota_status_flag = 0, last_update_time = '%s' WHERE gateway_id = '%s'", server_time.c_str(), gateway_id_str);

    this->print_and_log("FUOTA_SQL Query: ", query_buffer);

    // Open MySQL connection

    if (!this->connect_to_mysql())
    {
        this->print_and_log("FUOTA_MySQL connection failed\n");
        return -1;
    }

    // Execute the query
    rc = mysql_query(this->mysql, query_buffer);

    if (rc != 0)
    {
        const char *err = mysql_error(this->mysql);

        this->print_and_log("MySQL Error:%d\n ", err);
        return -1;
    }
    else
    {
        // cout << "FUOTA_SQLQuery ReturnValue = " << rc << endl;
    }

    // Check if any rows were actually affected
    my_ulonglong affected_rows = mysql_affected_rows(this->mysql);
    if (affected_rows == 0)
    {
        this->print_and_log("Warning: Query executed but no rows were affected\n");
    }
    else
    {
        this->print_and_log("Rows affected: %d\n", (int)affected_rows);
    }

    this->print_and_log("Executed Query: %s\n", (unsigned char *)query_buffer);
    return SUCCESS;
}

/*
 * Fetch the last pending request and continue the fuota process
 */
int MySqlDatabase::fetch_pending_fuota_targetnode_path(char *query_buffer, const unsigned char *gateway_id, unsigned char *target_mac, std::string &file_path, int &request_id, std::string &file_name)
{
    this->print_and_log("Function: %s\n", __FUNCTION__);

    MYSQL_RES *result = nullptr;
    MYSQL_ROW row = nullptr;

    sprintf(query_buffer,
            "SELECT request_id, gateway_id, meter_sno, filepath_for_targetnode, filename "
            "FROM dlms_fuota_upload "
            "WHERE gateway_id = '%s' "
            "AND request_id NOT IN (14, 15) "
            "AND uploaded_time >= DATE_SUB(NOW(), INTERVAL 30 MINUTE) "
            "ORDER BY request_id ASC LIMIT 1;",
            gateway_id);

    if (!this->connect_to_mysql())
        return FAILURE;

    if (mysql_query(this->mysql, query_buffer) != 0)
    {
        this->print_and_log("MySQL Query Failed: %s\n", mysql_error(this->mysql));
        return FAILURE;
    }

    result = mysql_store_result(this->mysql);
    if (!result)
    {
        this->print_and_log("mysql_store_result NULL\n");
        return FAILURE;
    }

    if (mysql_num_rows(result) == 0)
    {
        mysql_free_result(result);
        return FAILURE;
    }

    row = mysql_fetch_row(result);
    if (!row || !row[0] || !row[2] || !row[3] || !row[4])
    {
        mysql_free_result(result);
        return FAILURE;
    }

    // ----------------------------
    // Extract values
    // ----------------------------
    request_id = atoi(row[0]);

    strcpy((char *)target_mac, row[2]); // meter_sno (MAC as string)

    file_path = row[3]; // std::string
    file_name = row[4]; // std::string

    this->print_and_log("Pending FUOTA: ReqID=%d MAC=%s PATH=%s FILE=%s\n", request_id, target_mac, file_path.c_str(), file_name.c_str());

    mysql_free_result(result);
    return SUCCESS;
}