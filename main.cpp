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
#include <mutex>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
typedef int socklen_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

const int PORT = 12345;
const int UDP_PORT = 12346;
const int BUFFER_SIZE = 4096;
const int UDP_PAYLOAD_SIZE = 1460;
const int WINDOW_SIZE = 16;

std::mutex g_coutMutex;

#pragma pack(push, 1)
struct UdpPacket {
    uint32_t seq;
    uint32_t type;         
    uint32_t dataSize;
    char data[UDP_PAYLOAD_SIZE];
};
#pragma pack(pop)

struct Command {
    std::string keyword;
    std::vector<std::string> args;
};

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
    DWORD tv = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

void enableKeepAlive(socket_t s) {
    int optval = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&optval, sizeof(optval));
#ifdef _WIN32
    struct tcp_keepalive alive;
    alive.onoff = 1;
    alive.keepalivetime = 30000;
    alive.keepaliveinterval = 5000;
    DWORD dwBytesRet = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), NULL, 0, &dwBytesRet, NULL, NULL);
#else
    int idle = 30, interval = 5, maxpkt = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(maxpkt));
#endif
}

void handleTcpClient(socket_t cs, std::string ip) {
    {
        std::lock_guard<std::mutex> lock(g_coutMutex);
        std::cout << "[TCP] New connection from: " << ip << std::endl;
    }

    enableKeepAlive(cs);

    std::string cmdBuffer;
    char buffer[BUFFER_SIZE];
    bool connected = true;

    while (connected) {
        int r = recv(cs, buffer, BUFFER_SIZE, 0);
        if (r <= 0) break;   

        cmdBuffer.append(buffer, r);

        while (!cmdBuffer.empty()) {
            size_t pos = cmdBuffer.find('\n');
            if (pos == std::string::npos) break;    

            std::string line = cmdBuffer.substr(0, pos);
            cmdBuffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.empty()) continue;

            Command cmd = parseCommand(line);

            if (cmd.keyword == "ECHO") {
                std::string msg = "ECHO: " + (cmd.args.empty() ? "" : cmd.args[0]) + "\n";
                send(cs, msg.c_str(), (int)msg.length(), 0);
            }
            else if (cmd.keyword == "TIME") {
                time_t now = time(0);
                std::string t = std::ctime(&now);
                send(cs, t.c_str(), (int)t.length(), 0);
            }
            else if (cmd.keyword == "DOWNLOAD" && cmd.args.size() >= 1) {
                std::string filename = getBasename(cmd.args[0]);
                std::ifstream inFile(filename, std::ios::binary | std::ios::ate);

                if (inFile.is_open()) {
                    uint64_t fileSize = inFile.tellg();
                    inFile.seekg(0);

                    std::string res = "READY " + std::to_string(fileSize) + "\n";
                    send(cs, res.c_str(), (int)res.length(), 0);

                    uint64_t sent = 0;
                    char fileBuf[BUFFER_SIZE];
                    while (sent < fileSize) {
                        inFile.read(fileBuf, BUFFER_SIZE);
                        int bytesRead = (int)inFile.gcount();
                        if (bytesRead <= 0) break;
                        send(cs, fileBuf, bytesRead, 0);      
                        sent += bytesRead;
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_coutMutex);
                        std::cout << "[TCP] Sent " << filename << " to " << ip << std::endl;
                    }
                }
                else {
                    std::string res = "FILE_NOT_FOUND\n";
                    send(cs, res.c_str(), (int)res.length(), 0);
                }
            }
            else if (cmd.keyword == "UPLOAD" && cmd.args.size() >= 2) {
                std::string filename = getBasename(cmd.args[0]);
                uint64_t fileSize = std::stoull(cmd.args[1]);
                std::ofstream outFile("tcp_upl_" + filename, std::ios::binary);

                std::string res = "READY\n";
                send(cs, res.c_str(), (int)res.length(), 0);

                uint64_t received = 0;
                auto start = std::chrono::steady_clock::now();

                uint64_t remaining = fileSize - received;
                uint64_t toWrite = (std::min)((uint64_t)cmdBuffer.size(), remaining);
                if (toWrite > 0) {
                    outFile.write(cmdBuffer.data(), toWrite);
                    received += toWrite;
                    cmdBuffer.erase(0, toWrite);
                }

                while (received < fileSize) {
                    int bytes = recv(cs, buffer, BUFFER_SIZE, 0);
                    if (bytes <= 0) { connected = false; break; }

                    remaining = fileSize - received;
                    toWrite = (std::min)((uint64_t)bytes, remaining);
                    outFile.write(buffer, toWrite);
                    received += toWrite;

                    if (bytes > toWrite) {
                        cmdBuffer.append(buffer + toWrite, bytes - toWrite);
                    }
                }
                outFile.close();

                auto end = std::chrono::steady_clock::now();
                double duration = std::chrono::duration<double>(end - start).count();

                std::string comp = "UPLOAD_COMPLETE\n";
                send(cs, comp.c_str(), (int)comp.length(), 0);

                std::lock_guard<std::mutex> lock(g_coutMutex);
                std::cout << "[TCP] Received " << filename << " at " << calculateBitrate(fileSize, duration) << " Mbps\n";
            }
            else if (cmd.keyword == "EXIT" || cmd.keyword == "QUIT") {
                send(cs, "GOODBYE\n", 8, 0);
                connected = false;
                break;
            }
            else {
                std::string msg = "UNKNOWN_COMMAND\n";
                send(cs, msg.c_str(), (int)msg.length(), 0);
            }
        }
    }
    closesocket(cs);
    std::lock_guard<std::mutex> lock(g_coutMutex);
    std::cout << "[TCP] Disconnected: " << ip << std::endl;
}

void handleUdpTransfer(sockaddr_in clientAddr, std::string filename) {
    socket_t threadSock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in localAddr{};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;       
    bind(threadSock, (sockaddr*)&localAddr, sizeof(localAddr));

    setSocketTimeout(threadSock, 5000);       

    std::ofstream outFile("udp_th_" + filename, std::ios::binary);
    uint32_t expectedSeq = 0;
    UdpPacket pkt;
    socklen_t addrLen = sizeof(clientAddr);

    UdpPacket ack{ 0, 1, 0, "" };
    sendto(threadSock, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);

    uint64_t totalReceived = 0;
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        int bytes = recvfrom(threadSock, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);
        if (bytes <= 0) break;    

        if (pkt.type == 0) {  
            if (pkt.seq == expectedSeq) {
                outFile.write(pkt.data, pkt.dataSize);
                totalReceived += pkt.dataSize;
                expectedSeq++;
            }
            ack = { expectedSeq - 1, 1, 0, "" };
            sendto(threadSock, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
        }
        else if (pkt.type == 2) {  
            ack = { 0, 2, 0, "" };
            sendto(threadSock, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
            break;
        }
    }

    outFile.close();
    closesocket(threadSock);

    auto endTime = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(endTime - startTime).count();

    std::lock_guard<std::mutex> lock_out(g_coutMutex);
    std::cout << "[UDP Thread] Finished: " << filename << " at " << calculateBitrate(totalReceived, duration) << " Mbps\n";
}

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

    std::cout << "Multi-threaded Server (Lab 4) running..." << std::endl;

    std::thread udpDispatcher([&]() {
        while (true) {
            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            UdpPacket pkt;
            int bytes = recvfrom(udpSrv, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);

            if (bytes > 0 && pkt.type == 3) {
                std::string fname(pkt.data, pkt.dataSize);
                {
                    std::lock_guard<std::mutex> lock(g_coutMutex);
                    std::cout << "[UDP Dispatcher] Spawning thread for: " << fname << std::endl;
                }
                std::thread(handleUdpTransfer, clientAddr, fname).detach();
            }
        }
        });
    udpDispatcher.detach();

    while (true) {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        socket_t cs = accept(tcpSrv, (sockaddr*)&ca, &cl);   

        if (cs != INVALID_SOCKET) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &ca.sin_addr, ip, INET_ADDRSTRLEN);
            std::thread(handleTcpClient, cs, std::string(ip)).detach();
        }
    }
}

void udpSendFile(socket_t s, const std::string& path, sockaddr_in destAddr) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return;

    UdpPacket start; start.type = 3;
    std::string name = getBasename(path);
    memcpy(start.data, name.c_str(), (std::min)((size_t)UDP_PAYLOAD_SIZE, name.length()));
    start.dataSize = (uint32_t)name.length();

    UdpPacket ack; sockaddr_in from{}; socklen_t fromLen = sizeof(from);
    setSocketTimeout(s, 500);

    bool sessionEstablished = false;
    for (int i = 0; i < 5; ++i) {     
        sendto(s, (const char*)&start, sizeof(uint32_t) * 3 + start.dataSize, 0, (sockaddr*)&destAddr, sizeof(destAddr));
        if (recvfrom(s, (char*)&ack, sizeof(ack), 0, (sockaddr*)&from, &fromLen) > 0) {
            if (ack.type == 1) {   
                destAddr.sin_port = from.sin_port;      
                sessionEstablished = true;
                break;
            }
        }
    }

    if (!sessionEstablished) {
        std::cout << "UDP Server not responding.\n";
        return;
    }

    setSocketTimeout(s, 100);
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

    enableKeepAlive(ts);

    if (connect(ts, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect to server." << std::endl;
        return;
    }

    std::cout << "Commands: ECHO, TIME, UPLOAD <path>, DOWNLOAD <filename>, UDP_UPLOAD <path>, EXIT\n";
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
            if (!f) { std::cout << "File not found!\n"; continue; }
            uint64_t sz = f.tellg(); f.seekg(0);

            std::string h = "UPLOAD " + getBasename(cmd.args[0]) + " " + std::to_string(sz) + "\n";
            send(ts, h.c_str(), (int)h.length(), 0);

            std::string respLine; char c;
            while (recv(ts, &c, 1, 0) > 0 && c != '\n') { if (c != '\r') respLine += c; }
            if (respLine != "READY") { std::cout << "Error: " << respLine << "\n"; continue; }

            char b[BUFFER_SIZE];
            auto start = std::chrono::steady_clock::now();
            while (!f.eof()) {
                f.read(b, BUFFER_SIZE);
                std::streamsize bytes = f.gcount();
                if (bytes > 0) send(ts, b, (int)bytes, 0);
            }

            respLine.clear();
            while (recv(ts, &c, 1, 0) > 0 && c != '\n') { if (c != '\r') respLine += c; }

            auto end = std::chrono::steady_clock::now();
            double duration = std::chrono::duration<double>(end - start).count();
            std::cout << "Server: " << respLine << " at " << calculateBitrate(sz, duration) << " Mbps\n";
        }
        else if (cmd.keyword == "DOWNLOAD" && !cmd.args.empty()) {
            std::string h = "DOWNLOAD " + cmd.args[0] + "\n";
            send(ts, h.c_str(), (int)h.length(), 0);

            std::string respLine; char c;
            while (recv(ts, &c, 1, 0) > 0 && c != '\n') { if (c != '\r') respLine += c; }

            Command resp = parseCommand(respLine);
            if (resp.keyword == "READY" && resp.args.size() >= 1) {
                uint64_t sz = std::stoull(resp.args[0]);
                std::ofstream f("dl_" + getBasename(cmd.args[0]), std::ios::binary);
                uint64_t recvd = 0;
                char b[BUFFER_SIZE];

                auto start = std::chrono::steady_clock::now();
                while (recvd < sz) {
                    int toRead = (std::min)((uint64_t)BUFFER_SIZE, sz - recvd);
                    int r = recv(ts, b, toRead, 0);
                    if (r <= 0) break;
                    f.write(b, r);
                    recvd += r;
                }
                auto end = std::chrono::steady_clock::now();
                double duration = std::chrono::duration<double>(end - start).count();
                std::cout << "Download finished at " << calculateBitrate(sz, duration) << " Mbps\n";
            }
            else {
                std::cout << "Server: " << respLine << "\n";
            }
        }
        else {
            send(ts, (line + "\n").c_str(), (int)line.length() + 1, 0);
            char b[BUFFER_SIZE]; int r = recv(ts, b, BUFFER_SIZE - 1, 0);
            if (r > 0) { b[r] = '\0'; std::cout << "Server: " << b; }
        }
        if (cmd.keyword == "EXIT" || cmd.keyword == "QUIT") break;
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