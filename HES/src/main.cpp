#include "../inc/main.h"

#include "../inc/server.h"

void CloseAllFds() {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "opendir failed: %s\n", strerror(errno));
        len = write(STDOUT_FILENO, buf, len);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        int fd = atoi(entry->d_name);

        if (fd > 2 && fd != dirfd(dir)) {
            char msg[128];
            int len = snprintf(msg, sizeof(msg), "Closing fd: %d\n", fd);
            len = write(STDOUT_FILENO, msg, len);
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
    }
    closedir(dir);
}

void signalHandler(int sig) {
    char buff[512];
    int len = snprintf(buff, sizeof(buff), "Caught signal: %s (%d)\n", strsignal(sig), sig);
    len = write(STDOUT_FILENO, buff, len);

    if (sig == SIGSEGV || sig == SIGABRT) {
        void* buffer[BUFF_SIZE];
        int nptrs = backtrace(buffer, BUFF_SIZE);
        backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO);
    }

    exit(EXIT_FAILURE);
}

void CloseAllFdsAtExit() {
    CloseAllFds();
    char msg[] = "All fds closed in atexit handler\n";
    [[maybe_unused]] int len = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

int main() {
    std::cout << "HES Service" << std::endl;

    struct sigaction sa{};

    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);

    std::unique_ptr<Server> server = std::make_unique<Server>();

    if (atexit(CloseAllFdsAtExit) != 0) {
        std::cout << "Failed to register atexit handler" << std::endl;
    }

    server->accept_and_dispatch();

    return 0;
}
