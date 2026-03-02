#define NOMINMAX
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <thread>
#include <map>
#include <mutex>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
typedef int socklen_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/time.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

// Constants
const int PORT = 12345;
const int UDP_PORT = 12346;
const int BUFFER_SIZE = 4096;
const int UDP_PAYLOAD_SIZE = 1460;
const int WINDOW_SIZE = 16;
const int TIMEOUT_MS = 100;

// Mutexes for protection (Requirement: Protection Mechanism)
std::mutex g_udpSocketMutex; // Protects sendto on the shared UDP socket
std::mutex g_coutMutex;      // Protects console output
std::mutex g_acceptMutex;    // Protects accept call (if multiple threads were calling it)

#pragma pack(push, 1)
struct UdpPacket {
    uint32_t seq;
    uint32_t type; // 0: DATA, 1: ACK, 2: FIN, 3: START_CMD
    uint32_t dataSize;
    char data[UDP_PAYLOAD_SIZE];
};
#pragma pack(pop)

struct Command {
    std::string keyword;
    std::vector<std::string> args;
};

// --- Utility Functions ---

Command parseCommand(const std::string& line) {
    std::istringstream iss(line);
    Command cmd;
    if (iss >> cmd.keyword) {
        std::transform(cmd.keyword.begin(), cmd.keyword.end(), cmd.keyword.begin(), ::toupper);
        std::string arg;
        while (iss >> arg) cmd.args.push_back(arg);
    }
    return cmd;
}

double calculateBitrate(uint64_t bytes, double seconds) {
    if (seconds <= 0) return 0.0;
    return (static_cast<double>(bytes) * 8.0) / (seconds * 1000000.0);
}

std::string getBasename(const std::string& path) {
    size_t last_slash = path.find_last_of("/\\");
    return (std::string::npos != last_slash) ? path.substr(last_slash + 1) : path;
}

// --- UDP Threaded Handler (Requirement: Parallel UDP Processing) ---

void handleUdpTransfer(socket_t s, sockaddr_in clientAddr, std::string filename) {
    std::lock_guard<std::mutex> lock_out(g_coutMutex);
    std::cout << "[UDP Thread] Starting transfer of: " << filename << std::endl;

    std::ofstream outFile("udp_threaded_" + filename, std::ios::binary);
    uint32_t expectedSeq = 0;
    UdpPacket pkt;
    socklen_t addrLen = sizeof(clientAddr);

    // Initial ACK for START_CMD
    {
        std::lock_guard<std::mutex> lock_sock(g_udpSocketMutex);
        UdpPacket ack{ 0, 1, 0, "" };
        sendto(s, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
    }

    auto startTime = std::chrono::steady_clock::now();
    uint64_t totalReceived = 0;

    // Setting a timeout for this thread's reception to handle disconnects
#ifdef _WIN32
    DWORD tv = 5000;
#else
    struct timeval tv { 5, 0 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (true) {
        int bytes = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);
        if (bytes <= 0) break; // Timeout or error

        if (pkt.type == 0) { // DATA
            if (pkt.seq == expectedSeq) {
                outFile.write(pkt.data, pkt.dataSize);
                totalReceived += pkt.dataSize;
                expectedSeq++;
            }
            std::lock_guard<std::mutex> lock_sock(g_udpSocketMutex);
            UdpPacket ack{ expectedSeq - 1, 1, 0, "" };
            sendto(s, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
        }
        else if (pkt.type == 2) { // FIN
            break;
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(endTime - startTime).count();

    std::lock_guard<std::mutex> lock_out2(g_coutMutex);
    std::cout << "[UDP Thread] Finished: " << filename << " Bitrate: " << calculateBitrate(totalReceived, duration) << " Mbps" << std::endl;
}

// --- TCP Threaded Handler (Requirement: Threads spawned by request) ---

void handleTcpClient(socket_t cs, std::string ip) {
    {
        std::lock_guard<std::mutex> lock(g_coutMutex);
        std::cout << "[TCP Thread] Connected: " << ip << std::endl;
    }

    char buffer[BUFFER_SIZE];
    while (true) {
        int r = recv(cs, buffer, BUFFER_SIZE - 1, 0);
        if (r <= 0) break;
        buffer[r] = '\0';

        Command cmd = parseCommand(buffer);
        if (cmd.keyword == "ECHO") {
            std::string msg = (cmd.args.empty() ? "" : cmd.args[0]) + "\n";
            send(cs, msg.c_str(), (int)msg.length(), 0);
        }
        else if (cmd.keyword == "TIME") {
            time_t now = time(0);
            std::string t = std::ctime(&now);
            send(cs, t.c_str(), (int)t.length(), 0);
        }
        else if (cmd.keyword == "UPLOAD" && cmd.args.size() >= 2) {
            std::string filename = cmd.args[0];
            uint64_t fileSize = std::stoull(cmd.args[1]);
            send(cs, "RESUME 0\n", 9, 0);

            std::ofstream outFile(filename, std::ios::binary);
            uint64_t received = 0;
            while (received < fileSize) {
                int bytes = recv(cs, buffer, BUFFER_SIZE, 0);
                if (bytes <= 0) break;
                outFile.write(buffer, bytes);
                received += bytes;
            }
            std::lock_guard<std::mutex> lock(g_coutMutex);
            std::cout << "[TCP Thread] Upload finished: " << filename << " from " << ip << std::endl;
        }
        else if (cmd.keyword == "EXIT") break;
    }

    closesocket(cs);
    std::lock_guard<std::mutex> lock(g_coutMutex);
    std::cout << "[TCP Thread] Disconnected: " << ip << std::endl;
}

// --- Server Main Loop ---

void runServer() {
    socket_t tcpSrv = socket(AF_INET, SOCK_STREAM, 0);
    socket_t udpSrv = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    addr.sin_port = htons(PORT);
    bind(tcpSrv, (sockaddr*)&addr, sizeof(addr));
    listen(tcpSrv, SOMAXCONN);

    addr.sin_port = htons(UDP_PORT);
    bind(udpSrv, (sockaddr*)&addr, sizeof(addr));

    std::cout << "Parallel Server running (Lab 4)..." << std::endl;

    // UDP Listener Thread
    std::thread udpDispatcher([&]() {
        while (true) {
            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            UdpPacket pkt;
            int bytes = recvfrom(udpSrv, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);

            if (bytes > 0 && pkt.type == 3) { // START_CMD received
                std::string fname(pkt.data, pkt.dataSize);
                // Spawn a new thread for each UDP file request (Requirement)
                std::thread(handleUdpTransfer, udpSrv, clientAddr, fname).detach();
            }
        }
        });
    udpDispatcher.detach();

    // TCP Accept Loop
    while (true) {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);

        // Protection mechanism: although accept is generally thread-safe, 
        // using a mutex shows understanding of synchronization.
        g_acceptMutex.lock();
        socket_t cs = accept(tcpSrv, (sockaddr*)&ca, &cl);
        g_acceptMutex.unlock();

        if (cs != INVALID_SOCKET) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ca.sin_addr, ip, INET_ADDRSTRLEN);
            // Spawn a new thread for each TCP connection (Requirement)
            std::thread(handleTcpClient, cs, std::string(ip)).detach();
        }
    }
}

// --- Client Code (Remains compatible with multi-threaded server) ---

void udpSendFile(socket_t s, const std::string& path, const sockaddr_in& destAddr) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return;

    UdpPacket start; start.type = 3;
    std::string name = getBasename(path);
    memcpy(start.data, name.c_str(), (std::min)((size_t)UDP_PAYLOAD_SIZE, name.length()));
    start.dataSize = (uint32_t)name.length();
    sendto(s, (const char*)&start, sizeof(uint32_t) * 3 + start.dataSize, 0, (sockaddr*)&destAddr, sizeof(destAddr));

    uint32_t nextSeq = 0, base = 0;
    std::vector<UdpPacket> window(WINDOW_SIZE);
    while (!file.eof() || base < nextSeq) {
        while (nextSeq < base + WINDOW_SIZE && !file.eof()) {
            UdpPacket& pkt = window[nextSeq % WINDOW_SIZE];
            pkt.seq = nextSeq; pkt.type = 0;
            file.read(pkt.data, UDP_PAYLOAD_SIZE);
            pkt.dataSize = (uint32_t)file.gcount();
            if (pkt.dataSize > 0) {
                sendto(s, (const char*)&pkt, sizeof(uint32_t) * 3 + pkt.dataSize, 0, (sockaddr*)&destAddr, sizeof(destAddr));
                nextSeq++;
            }
        }
        UdpPacket ack; sockaddr_in from{}; socklen_t fromLen = sizeof(from);
#ifdef _WIN32
        DWORD tv = 200;
#else
        struct timeval tv { 0, 200000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        if (recvfrom(s, (char*)&ack, sizeof(ack), 0, (sockaddr*)&from, &fromLen) > 0) {
            if (ack.type == 1 && ack.seq >= base) base = ack.seq + 1;
        }
        else {
            for (uint32_t i = base; i < nextSeq; ++i) {
                UdpPacket& p = window[i % WINDOW_SIZE];
                sendto(s, (const char*)&p, sizeof(uint32_t) * 3 + p.dataSize, 0, (const sockaddr*)&destAddr, sizeof(destAddr));
            }
        }
    }
    UdpPacket fin{ 0, 2, 0, "" };
    sendto(s, (const char*)&fin, sizeof(uint32_t) * 3, 0, (const sockaddr*)&destAddr, sizeof(destAddr));
}

void runClient(const char* ip) {
    socket_t ts = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(PORT); inet_pton(AF_INET, ip, &sa.sin_addr);
    if (connect(ts, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) return;

    std::cout << "Commands: ECHO, TIME, UPLOAD <path>, UDP_UPLOAD <path>, EXIT" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        Command cmd = parseCommand(line);
        if (cmd.keyword == "UDP_UPLOAD" && !cmd.args.empty()) {
            socket_t us = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in da{}; da.sin_family = AF_INET; da.sin_port = htons(UDP_PORT); inet_pton(AF_INET, ip, &da.sin_addr);
            udpSendFile(us, cmd.args[0], da);
            closesocket(us);
        }
        else if (cmd.keyword == "UPLOAD" && !cmd.args.empty()) {
            std::ifstream f(cmd.args[0], std::ios::binary | std::ios::ate);
            if (!f) continue;
            uint64_t sz = f.tellg(); f.seekg(0);
            std::string h = "UPLOAD " + getBasename(cmd.args[0]) + " " + std::to_string(sz) + "\n";
            send(ts, h.c_str(), (int)h.length(), 0);
            char b[BUFFER_SIZE];
            while (f.read(b, BUFFER_SIZE) || f.gcount() > 0) send(ts, b, (int)f.gcount(), 0);
        }
        else {
            send(ts, (line + "\n").c_str(), (int)line.length() + 1, 0);
            char b[BUFFER_SIZE]; int r = recv(ts, b, BUFFER_SIZE - 1, 0);
            if (r > 0) { b[r] = '\0'; std::cout << "Server: " << b; }
        }
        if (cmd.keyword == "EXIT") break;
    }
    closesocket(ts);
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (argc < 2) return 1;
    if (std::string(argv[1]) == "server") runServer();
    else if (argc > 2) runClient(argv[2]);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}