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
#include <fcntl.h>

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
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

// Constants
const int PORT = 12345;
const int UDP_PORT = 12346;
const int BUFFER_SIZE = 4096; // Chunk size to maintain response time
const int UDP_PAYLOAD_SIZE = 1460;
const int WINDOW_SIZE = 16;
const int TIMEOUT_MS = 100;

#pragma pack(push, 1)
struct UdpPacket {
    uint32_t seq;
    uint32_t type; // 0: DATA, 1: ACK, 2: FIN, 3: START_CMD
    uint32_t dataSize;
    char data[UDP_PAYLOAD_SIZE];
};
#pragma pack(pop)

// Structure to hold state of each connected TCP client
struct ClientSession {
    socket_t socket;
    std::string ip;
    std::string cmdBuffer;
    bool isUploading = false;
    std::ofstream outFile;
    uint64_t fileSize = 0;
    uint64_t bytesReceived = 0;
    std::string currentFileName;
    std::chrono::steady_clock::time_point startTime;
};

struct Command {
    std::string keyword;
    std::vector<std::string> args;
};

// --- Utility Functions ---

void setNonBlocking(socket_t s) {
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    fcntl(s, F_SETFL, O_NONBLOCK);
#endif
}

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

// --- Lab 3 Multiplexing Logic ---

void handleTcpCommand(ClientSession& session, const Command& cmd) {
    if (cmd.keyword == "ECHO") {
        std::string msg = (cmd.args.empty() ? "" : cmd.args[0]) + "\n";
        send(session.socket, msg.c_str(), (int)msg.length(), 0);
    }
    else if (cmd.keyword == "TIME") {
        time_t now = time(0);
        std::string t = std::ctime(&now);
        send(session.socket, t.c_str(), (int)t.length(), 0);
    }
    else if (cmd.keyword == "UPLOAD" && cmd.args.size() >= 2) {
        session.currentFileName = cmd.args[0];
        session.fileSize = std::stoull(cmd.args[1]);
        session.isUploading = true;
        session.bytesReceived = 0;
        session.outFile.open(session.currentFileName, std::ios::binary | std::ios::trunc);
        session.startTime = std::chrono::steady_clock::now();

        std::string res = "RESUME 0\n"; // Simplifying for Lab 3 multiplex demo
        send(session.socket, res.c_str(), (int)res.length(), 0);
    }
}

// Sequential processing of a data chunk for a client
void processClientData(ClientSession& session) {
    char buffer[BUFFER_SIZE];
    int r = recv(session.socket, buffer, BUFFER_SIZE, 0);

    if (r <= 0) return; // Error or peer closed handled by main loop

    if (session.isUploading) {
        session.outFile.write(buffer, r);
        session.bytesReceived += r;
        if (session.bytesReceived >= session.fileSize) {
            session.isUploading = false;
            session.outFile.close();
            auto end = std::chrono::steady_clock::now();
            double duration = std::chrono::duration<double>(end - session.startTime).count();
            std::cout << "TCP Finished: " << session.currentFileName << " at "
                << calculateBitrate(session.fileSize, duration) << " Mbps" << std::endl;
        }
    }
    else {
        for (int i = 0; i < r; ++i) {
            if (buffer[i] == '\n') {
                handleTcpCommand(session, parseCommand(session.cmdBuffer));
                session.cmdBuffer.clear();
            }
            else {
                session.cmdBuffer += buffer[i];
            }
        }
    }
}

// Global state for UDP (simplified for multiplexing)
std::ofstream g_udpOutFile;
uint32_t g_udpExpectedSeq = 0;

void processUdpPacket(socket_t s) {
    sockaddr_in clientAddr{}; socklen_t addrLen = sizeof(clientAddr);
    UdpPacket pkt;
    int bytes = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);
    if (bytes <= 0) return;

    if (pkt.type == 3) { // START
        g_udpOutFile.close();
        g_udpOutFile.open("udp_mpx_" + std::string(pkt.data, pkt.dataSize), std::ios::binary);
        g_udpExpectedSeq = 0;
        UdpPacket ack{ 0, 1, 0, "" };
        sendto(s, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
    }
    else if (pkt.type == 0 && g_udpOutFile.is_open()) { // DATA
        if (pkt.seq == g_udpExpectedSeq) {
            g_udpOutFile.write(pkt.data, pkt.dataSize);
            g_udpExpectedSeq++;
        }
        UdpPacket ack{ g_udpExpectedSeq - 1, 1, 0, "" };
        sendto(s, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
    }
    else if (pkt.type == 2) { // FIN
        g_udpOutFile.close();
        std::cout << "UDP Multiplexed transfer finished." << std::endl;
    }
}

void runServer() {
    socket_t tcpSrv = socket(AF_INET, SOCK_STREAM, 0);
    socket_t udpSrv = socket(AF_INET, SOCK_DGRAM, 0);

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;

    addr.sin_port = htons(PORT);
    bind(tcpSrv, (sockaddr*)&addr, sizeof(addr));
    listen(tcpSrv, SOMAXCONN);

    addr.sin_port = htons(UDP_PORT);
    bind(udpSrv, (sockaddr*)&addr, sizeof(addr));

    setNonBlocking(tcpSrv);
    setNonBlocking(udpSrv);

    std::vector<ClientSession> clients;
    std::cout << "Single-threaded Multiplexing Server running..." << std::endl;

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(tcpSrv, &readfds);
        FD_SET(udpSrv, &readfds);

        socket_t max_fd = (tcpSrv > udpSrv) ? tcpSrv : udpSrv;

        for (const auto& client : clients) {
            FD_SET(client.socket, &readfds);
            if (client.socket > max_fd) max_fd = client.socket;
        }

        // Multiplexing call
        if (select((int)max_fd + 1, &readfds, NULL, NULL, NULL) < 0) continue;

        // 1. Check for new TCP connections
        if (FD_ISSET(tcpSrv, &readfds)) {
            sockaddr_in ca{}; socklen_t cl = sizeof(ca);
            socket_t cs = accept(tcpSrv, (sockaddr*)&ca, &cl);
            if (cs != INVALID_SOCKET) {
                setNonBlocking(cs);
                char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ca.sin_addr, ip, INET_ADDRSTRLEN);
                clients.push_back({ cs, std::string(ip) });
                std::cout << "New client: " << ip << std::endl;
            }
        }

        // 2. Check for UDP data
        if (FD_ISSET(udpSrv, &readfds)) {
            processUdpPacket(udpSrv);
        }

        // 3. Check for TCP data from existing clients
        for (auto it = clients.begin(); it != clients.end(); ) {
            if (FD_ISSET(it->socket, &readfds)) {
                int peek = recv(it->socket, NULL, 0, MSG_PEEK); // check if closed
                if (peek == SOCKET_ERROR || (peek == 0)) {
                    std::cout << "Client disconnected: " << it->ip << std::endl;
                    closesocket(it->socket);
                    it = clients.erase(it);
                    continue;
                }
                processClientData(*it);
            }
            ++it;
        }
    }
}

// --- Previous Client Code (Stays mostly the same) ---

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
        struct timeval tv { 0, 100000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        if (recvfrom(s, (char*)&ack, sizeof(ack), 0, (sockaddr*)&from, &fromLen) > 0) {
            if (ack.type == 1 && ack.seq >= base) base = ack.seq + 1;
        }
        else {
            for (uint32_t i = base; i < nextSeq; ++i) {
                UdpPacket& p = window[i % WINDOW_SIZE];
                sendto(s, (const char*)&p, sizeof(uint32_t) * 3 + p.dataSize, 0, (sockaddr*)&destAddr, sizeof(destAddr));
            }
        }
    }
    UdpPacket fin{ 0, 2, 0, "" };
    sendto(s, (const char*)&fin, sizeof(uint32_t) * 3, 0, (sockaddr*)&destAddr, sizeof(destAddr));
    std::cout << "UDP Upload finished." << std::endl;
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
            std::cout << "TCP Upload initiated." << std::endl;
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