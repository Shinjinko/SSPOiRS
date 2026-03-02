#define NOMINMAX // 1. Исправляет ошибку с std::min и синтаксисом ::
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
#include <ctime>   // 2. Добавлено для time() и ctime()

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
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

const int PORT = 12345;
const int UDP_PORT = 12346;
const int BUFFER_SIZE = 4096;
const int UDP_PAYLOAD_SIZE = 1460;
const int WINDOW_SIZE = 16;
const int TIMEOUT_MS = 100;

#pragma pack(push, 1)
struct UdpPacket {
    uint32_t seq;
    uint32_t type;
    uint32_t dataSize;
    char data[UDP_PAYLOAD_SIZE];
};
#pragma pack(pop)

struct TransferState {
    std::string filename;
    std::string direction;
    uint64_t bytesTransferred = 0; // 3. Исправлена ошибка инициализации (type.6)
};

std::map<std::string, TransferState> g_transferStates;
std::mutex g_stateMutex;

struct Command {
    std::string keyword;
    std::vector<std::string> args;
};

void printError(const std::string& message) {
#ifdef _WIN32
    std::cerr << message << " with error code: " << WSAGetLastError() << std::endl;
#else
    perror(message.c_str());
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

void setSocketTimeout(socket_t s, int ms) {
#ifdef _WIN32
    DWORD timeout = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

void sendUdpAck(socket_t s, uint32_t seq, const sockaddr_in& addr) {
    UdpPacket ack;
    ack.seq = seq; ack.type = 1; ack.dataSize = 0;
    sendto(s, (const char*)&ack, sizeof(uint32_t) * 3, 0, (const sockaddr*)&addr, sizeof(addr));
}

void udpSendFile(socket_t s, const std::string& path, const sockaddr_in& destAddr) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { std::cerr << "File not found." << std::endl; return; }

    UdpPacket start;
    start.type = 3;
    std::string name = getBasename(path);
    // (std::min) в скобках защищает от макроса min
    memcpy(start.data, name.c_str(), (std::min)((size_t)UDP_PAYLOAD_SIZE, name.length()));
    start.dataSize = (uint32_t)name.length();
    sendto(s, (const char*)&start, sizeof(uint32_t) * 3 + start.dataSize, 0, (const sockaddr*)&destAddr, sizeof(destAddr));

    uint32_t nextSeq = 0, base = 0;
    std::vector<UdpPacket> window(WINDOW_SIZE);
    auto startTime = std::chrono::high_resolution_clock::now();
    uint64_t totalSent = 0;

    while (!file.eof() || base < nextSeq) {
        while (nextSeq < base + WINDOW_SIZE && !file.eof()) {
            UdpPacket& pkt = window[nextSeq % WINDOW_SIZE];
            pkt.seq = nextSeq; pkt.type = 0;
            file.read(pkt.data, UDP_PAYLOAD_SIZE);
            pkt.dataSize = (uint32_t)file.gcount();
            if (pkt.dataSize > 0) {
                sendto(s, (const char*)&pkt, sizeof(uint32_t) * 3 + pkt.dataSize, 0, (const sockaddr*)&destAddr, sizeof(destAddr));
                nextSeq++;
            }
        }
        UdpPacket ack; sockaddr_in from = { 0 }; socklen_t fromLen = sizeof(from);
        if (recvfrom(s, (char*)&ack, sizeof(ack), 0, (sockaddr*)&from, &fromLen) > 0) {
            if (ack.type == 1 && ack.seq >= base) {
                totalSent += (ack.seq - base + 1) * UDP_PAYLOAD_SIZE;
                base = ack.seq + 1;
            }
        }
        else {
            for (uint32_t i = base; i < nextSeq; ++i) {
                UdpPacket& p = window[i % WINDOW_SIZE];
                sendto(s, (const char*)&p, sizeof(uint32_t) * 3 + p.dataSize, 0, (const sockaddr*)&destAddr, sizeof(destAddr));
            }
        }
    }
    UdpPacket fin; fin.type = 2;
    sendto(s, (const char*)&fin, sizeof(uint32_t) * 3, 0, (const sockaddr*)&destAddr, sizeof(destAddr));
    auto duration = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - startTime).count();
    std::cout << "UDP Transfer finished. Bitrate: " << calculateBitrate(totalSent, duration) << " Mbps" << std::endl;
}

void udpReceiveFile(socket_t s) {
    sockaddr_in clientAddr = { 0 }; socklen_t addrLen = sizeof(clientAddr);
    UdpPacket pkt;
    std::string filename = "received_udp_file.dat";
    uint32_t expectedSeq = 0;
    std::ofstream outFile;

    while (true) {
        int bytes = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);
        if (bytes <= 0) continue;
        if (pkt.type == 3) {
            filename = "udp_" + std::string(pkt.data, pkt.dataSize);
            outFile.open(filename, std::ios::binary);
            expectedSeq = 0;
            std::cout << "Receiving UDP file: " << filename << std::endl;
            sendUdpAck(s, 0, clientAddr);
        }
        else if (pkt.type == 0 && outFile.is_open()) {
            if (pkt.seq == expectedSeq) {
                outFile.write(pkt.data, pkt.dataSize);
                sendUdpAck(s, expectedSeq++, clientAddr);
            }
            else sendUdpAck(s, expectedSeq - 1, clientAddr);
        }
        else if (pkt.type == 2) {
            if (outFile.is_open()) outFile.close();
            std::cout << "UDP file " << filename << " saved." << std::endl;
            break;
        }
    }
}

void handleTcpUpload(socket_t sock, const Command& cmd, const std::string& clientIp) {
    if (cmd.args.size() < 2) return;
    std::string filename = cmd.args[0];
    uint64_t fileSize = std::stoull(cmd.args[1]);
    uint64_t offset = 0;

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_transferStates.count(clientIp) && g_transferStates[clientIp].filename == filename)
            offset = g_transferStates[clientIp].bytesTransferred;
    }

    std::string res = "RESUME " + std::to_string(offset) + "\n";
    send(sock, res.c_str(), (int)res.length(), 0);

    std::ofstream outFile(filename, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
    char buffer[BUFFER_SIZE];
    uint64_t received = offset;
    auto start = std::chrono::high_resolution_clock::now();

    while (received < fileSize) {
        int r = recv(sock, buffer, BUFFER_SIZE, 0);
        if (r <= 0) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_transferStates[clientIp] = { filename, "UPLOAD", received };
            return;
        }
        outFile.write(buffer, r);
        received += r;
    }
    double d = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
    std::cout << "TCP Upload complete. Bitrate: " << calculateBitrate(fileSize - offset, d) << " Mbps" << std::endl;
}

void handleTcpConnection(socket_t clientSocket, std::string clientIp) {
    char buffer[BUFFER_SIZE];
    while (true) {
        int r = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (r <= 0) break;
        buffer[r] = '\0';
        Command cmd = parseCommand(buffer);

        if (cmd.keyword == "ECHO") {
            std::string msg = (cmd.args.empty() ? "" : cmd.args[0]) + "\n";
            send(clientSocket, msg.c_str(), (int)msg.length(), 0);
        }
        else if (cmd.keyword == "TIME") {
            time_t now = time(0);
            std::string t = std::ctime(&now);
            send(clientSocket, t.c_str(), (int)t.length(), 0);
        }
        else if (cmd.keyword == "UPLOAD") {
            handleTcpUpload(clientSocket, cmd, clientIp);
        }
        else if (cmd.keyword == "EXIT" || cmd.keyword == "CLOSE") {
            break;
        }
    }
    closesocket(clientSocket);
}

void runServer() {
    // 4. Лямбда-функция вынесена для стабильности в некоторых версиях VS
    std::thread udpSrv([]() {
        socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(UDP_PORT); a.sin_addr.s_addr = INADDR_ANY;
        bind(s, (struct sockaddr*)&a, sizeof(a));
        while (true) udpReceiveFile(s);
        });
    udpSrv.detach();

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr*)&a, sizeof(a));
    listen(s, SOMAXCONN);
    std::cout << "Server running. TCP:" << PORT << " UDP:" << UDP_PORT << std::endl;

    while (true) {
        sockaddr_in ca = { 0 }; socklen_t cl = sizeof(ca);
        socket_t cs = accept(s, (struct sockaddr*)&ca, &cl);
        char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ca.sin_addr, ip, INET_ADDRSTRLEN);
        std::thread(handleTcpConnection, cs, std::string(ip)).detach();
    }
}

void runClient(const char* ip) {
    socket_t ts = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa = { 0 }; sa.sin_family = AF_INET; sa.sin_port = htons(PORT); inet_pton(AF_INET, ip, &sa.sin_addr);
    if (connect(ts, (struct sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) return;

    std::cout << "Commands: ECHO <msg>, TIME, UPLOAD <path>, UDP_UPLOAD <path>, EXIT" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        Command cmd = parseCommand(line);
        if (cmd.keyword == "UDP_UPLOAD" && !cmd.args.empty()) {
            socket_t us = socket(AF_INET, SOCK_DGRAM, 0);
            setSocketTimeout(us, TIMEOUT_MS);
            sockaddr_in da = { 0 }; da.sin_family = AF_INET; da.sin_port = htons(UDP_PORT); inet_pton(AF_INET, ip, &da.sin_addr);
            udpSendFile(us, cmd.args[0], da);
            closesocket(us);
        }
        else if (cmd.keyword == "UPLOAD" && !cmd.args.empty()) {
            std::ifstream f(cmd.args[0], std::ios::binary | std::ios::ate);
            if (!f) {
                std::cerr << "Local file not found: " << cmd.args[0] << std::endl;
                continue;
            }
            uint64_t sz = f.tellg(); f.seekg(0);
            std::string h = "UPLOAD " + getBasename(cmd.args[0]) + " " + std::to_string(sz) + "\n";
            send(ts, h.c_str(), (int)h.length(), 0);
            char rb[100]; int r = recv(ts, rb, 99, 0);
            if (r > 0) {
                rb[r] = '\0';
                Command rc = parseCommand(rb);
                uint64_t off = (rc.keyword == "RESUME") ? std::stoull(rc.args[0]) : 0;
                f.seekg(off);
                char b[BUFFER_SIZE];
                while (f.read(b, BUFFER_SIZE) || f.gcount() > 0) send(ts, b, (int)f.gcount(), 0);
                std::cout << "TCP Upload finished." << std::endl;
            }
        }
        else {
            send(ts, (line + "\n").c_str(), (int)line.length() + 1, 0);
            if (cmd.keyword == "EXIT") break;
            char b[BUFFER_SIZE]; int r = recv(ts, b, BUFFER_SIZE - 1, 0);
            if (r > 0) { b[r] = '\0'; std::cout << "Server: " << b; }
        }
    }
    closesocket(ts);
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsa;
    // 5. Исправлена проверка возвращаемого значения WSAStartup
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    if (argc < 2) return 1;
    if (std::string(argv[1]) == "server") runServer();
    else if (argc > 2) runClient(argv[2]);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}