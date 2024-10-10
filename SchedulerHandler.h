#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using namespace std;

class RTTHandler {
private:
    std::string serverIp;
    int serverPort;
    int timeout;
    int id;
    int recvBufSize;

public:
    RTTHandler(const std::string& serverIp, int serverPort, int timeout, int id) :
        serverIp(serverIp), serverPort(serverPort), timeout(timeout), id(id), recvBufSize(20) {}

    void connect() {
        // Send a "Hi" message to the server to create a separate thread for the client
        int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("Error opening socket");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in servaddr;
        memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(serverPort);
        servaddr.sin_addr.s_addr = inet_addr(serverIp.c_str());

        char hiBuf[] = "Hi";
        char recvBuf[recvBufSize];
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        bool doneHandshake = true;

        cout<<serverIp<<endl;
        cout<<serverPort<<endl;
        while (doneHandshake) {
            sendto(sockfd, hiBuf, sizeof(hiBuf), 0, (const struct sockaddr*)&servaddr, sizeof(servaddr));
            std::cout << id << " Hi Sent" << std::endl;

            ssize_t n = recvfrom(sockfd, recvBuf, sizeof(recvBuf), MSG_WAITALL, (struct sockaddr*)&cliaddr, &len);
            if (n > 0) {
                doneHandshake = false;
                recvBufSize = n;
                std::cout << id << " Received HiAck" << std::endl;
            } else {
                std::cout << id << " Resending Hi" << std::endl;
            }
        }

        // Receiving timestamps for RTT estimation
        while (true) {
            ssize_t n = recvfrom(sockfd, recvBuf, sizeof(recvBuf), MSG_WAITALL, (struct sockaddr*)&cliaddr, &len);
            if (n > 0) {
               long long recvTimestamp = std::stol(std::string(recvBuf, n));
                sendto(sockfd, recvBuf, n, 0, (const struct sockaddr*)&servaddr, sizeof(servaddr));
            }
        }

        close(sockfd);
    }

    void run() {
        this_thread::sleep_for(chrono::milliseconds(1000));
        connect();
    }
};


class ControlChannelHandler {
private:
    std::string serverIp;
    int serverPort;
    int id;
    int socketFd;
    struct sockaddr_in serverAddr;
    std::mutex socketMutex;

public:
    ControlChannelHandler(const std::string& serverIp, int serverPort, int id)
        : serverIp(serverIp), serverPort(serverPort), id(id), socketFd(-1) {
        socketFd = socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd < 0) {
            std::cerr << "Failed to create socket: " << std::strerror(errno) << std::endl;
            throw std::runtime_error("Failed to create socket");
        }
        std::cout << "Socket created successfully with descriptor: " << socketFd << std::endl;

        std::memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        if (inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr) <= 0) {
            std::cerr << "Invalid server IP address: " << serverIp << std::endl;
            throw std::runtime_error("Invalid server IP address");
        }
        std::cout << "Server IP and port set successfully." << std::endl;
    }

    ~ControlChannelHandler() {
        std::lock_guard<std::mutex> lock(socketMutex);
        if (socketFd >= 0) {
            close(socketFd);
            std::cout << "Socket closed successfully." << std::endl;
        }
    }

    void sendStats(const std::string& stats) {
        std::lock_guard<std::mutex> lock(socketMutex);
        std::cout << "Socket file descriptor: " << socketFd << std::endl;
        std::cout << "Sending stats: " << stats << std::endl;
        ssize_t bytesSent = sendto(socketFd, stats.c_str(), stats.size(), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
        if (bytesSent < 0) {
            std::cerr << "Failed to send stats: " << std::strerror(errno) << std::endl;
            throw std::runtime_error("Failed to send stats");
        }
        std::cout << "Stats sent successfully: " << stats << std::endl;
    }
};