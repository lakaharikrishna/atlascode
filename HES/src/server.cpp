#include "../inc/server.h"

#include "../inc/client.h"

std::mutex Server::clients_mutex;
std::map<std::array<char, 16>, Client *> Server::g_clients;

Server::Server()
{
    std::cout << "Server constructor called" << std::endl;

    mosquitto_lib_init();
    mysql_library_init(0, nullptr, nullptr);

    try
    {
        this->server_port = Utility::readConfig<int>("HES.port");
    }
    catch (const std::exception &e)
    {
        std::cout << "Error: " << e.what() << std::endl;
    }

    this->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (this->server_socket == -1)
    {
        std::cout << "Socket creation error" << std::endl;
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(this->server_port);

    int yes = 1;
    if (setsockopt(this->server_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
    {
        std::cout << "setsockopt failed" << std::endl;
        close(this->server_socket);
        exit(EXIT_FAILURE);
    }

    if (bind(this->server_socket, (sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        std::cout << "Failed to bind" << std::endl;
        close(this->server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(this->server_socket, 256) < 0)
    {
        std::cout << "Listen failed" << std::endl;
        close(this->server_socket);
        exit(EXIT_FAILURE);
    }

    std::cout << "Server is listening on port: " << this->server_port << std::endl;
}

Server::~Server()
{
    std::cout << "Server destructor called" << std::endl;

    mosquitto_lib_cleanup();
    mysql_library_end();
}

bool Server::configure_socket_options(int socket_fd)
{
    int flag = 1;
    int keepcnt = 10;
    int keepidle = 10;
    int keepintvl = 5;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0)
    {
        std::cout << "Failed to set keepalive" << std::endl;
        return false;
    }

    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
    {
        std::cout << "Failed to set nodelay" << std::endl;
        return false;
    }

    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_QUICKACK, &flag, sizeof(flag)) < 0)
    {
        std::cout << "Failed to set quickack" << std::endl;
        return false;
    }

    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) < 0)
    {
        std::cout << "Failed to set keepcount" << std::endl;
        return false;
    }

    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) < 0)
    {
        std::cout << "Failed to set keepidle" << std::endl;
        return false;
    }

    if (setsockopt(socket_fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) < 0)
    {
        std::cout << "Failed to set keepintvl" << std::endl;
        return false;
    }

    return true;
}

void Server::accept_and_dispatch(void)
{
    std::cout << __FUNCTION__ << " start" << std::endl;

    socklen_t client_size = sizeof(sockaddr_in);

    while (true)
    {
        auto client = std::make_unique<Client>();

        if (!client)
        {
            std::cout << "Memory allocation failed" << std::endl;
            exit(EXIT_FAILURE);
        }

        int mqtt_socket = eventfd(0, EFD_NONBLOCK);
        client->set_mqtt_socket(mqtt_socket);

        std::cout << "Waiting for client connection..." << std::endl;
        int client_sock_fd = accept(this->server_socket, (sockaddr *)&client_addr, &client_size);

        if (client_sock_fd < 0)
        {
            std::cout << "Accept failed" << std::endl;
            continue;
        }

        std::cout << "Client connected" << std::endl;
        std::cout << "Client fd after accept: " << client_sock_fd << std::endl;

        client->set_client_socket(client_sock_fd);

        if (!configure_socket_options(client_sock_fd))
        {
            close(client_sock_fd);
            continue;
        }

        std::thread(handle_client, std::move(client)).detach();
    }
}

void Server::handle_client(std::unique_ptr<Client> client)
{
    client->pfd[0].fd = client->get_client_socket();
    client->pfd[0].events = POLLIN;

    client->pfd[1].fd = client->get_mqtt_socket();
    client->pfd[1].events = POLLIN;

    client->set_recv_timeout_for_client(12);

    while (client->gatewayStatus == Status::CONNECTED)
    {
        int pollret = 0;

        if (client->duplicate_gateway == true)
        {
            client->print_and_log("Duplicate gateway ID socket: %s\n", client->gateway_id);
            client->gatewayStatus = Status::DISCONNECTED;
            continue;
        }

        if (client->polltimeout <= 0)
        {
            client->set_poll_timeout(15);
        }

        pollret = poll(client->pfd, 2, client->polltimeout);

        if (pollret == 0)
        {
            client->poll_timeout_handler();
        }
        else if (pollret > 0)
        {
            if (client->pfd[0].revents & POLLIN)
            {
                client->receive_data_and_validate_response();
            }
            else if (client->pfd[1].revents & POLLIN)
            {
                client->process_ondemand_request();
            }
        }
        else
        {
            client->print_and_log("poll error: %d gateway_id: %s\n", errno, client->gateway_id);
            client->gatewayStatus = Status::DISCONNECTED;
            continue;
        }
    }
    client->print_and_log("Update gateway_status_info\n");
    client->update_into_gateway_status_info((const uint8_t *)client->pgwid, Status::DISCONNECTED, client->val1, client->val2, client->val3);
    client->print_and_log("[❌GW_DISCONNECTED][%s]\n", client->gateway_id);
    client->update_dlms_mqtt_info(client->gateway_id, 0);
    while (!client->ODM.empty())
    {
        // Get a const reference to the first command's byte vector in the ODM queue
        const std::vector<uint8_t> &cmd_bytes = client->ODM.front();
        // Convert the vector of bytes into a std::string for easier processing and parsing
        std::string cmd_str(cmd_bytes.begin(), cmd_bytes.end());

        // Split the converted command string by ':' delimiter into individual parts/fields
        std::vector<std::string> parts = client->split(cmd_str, ':');
        // Initialize request_id to invalid/default value
        int request_id = -1;

        // If the parts vector is non-empty and first part is not empty, convert first part to integer request ID
        if (parts.size() > 0 && !parts[0].empty())
            request_id = std::stoi(parts[0]);

        client->Update_dlms_on_demand_request_status(request_id, GW_DISCONNECTED, 0);

        // Remove the processed command from the front of the ODM queue
        client->ODM.pop();
    }
    client->unregister_client(client->gateway_id, client.get()); // Delete gateway info from Server::g_clients
}
