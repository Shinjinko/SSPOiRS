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
#include <map>
#include <cstring>
#include <ctime>
#include <fcntl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h> // Для tcp_keepalive в Windows
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
typedef int socklen_t;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // Для TCP_KEEPIDLE в Linux
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
const int BUFFER_SIZE = 4096;
const int UDP_PAYLOAD_SIZE = 1460;
const int WINDOW_SIZE = 16;

#pragma pack(push, 1)
struct UdpPacket {
    uint32_t seq;
    uint32_t type; // 0: DATA, 1: ACK, 2: FIN, 3: START_CMD
    uint32_t dataSize;
    char data[UDP_PAYLOAD_SIZE];
};
#pragma pack(pop)

// Структура для TCP-сессий
struct ClientSession {
    socket_t socket;
    std::string ip;
    std::string cmdBuffer;

    // Состояние UPLOAD (от клиента к серверу)
    bool isUploading = false;
    std::ofstream outFile;
    uint64_t bytesReceived = 0;

    // Состояние DOWNLOAD (от сервера к клиенту)
    bool isDownloading = false;
    std::ifstream inFile;
    uint64_t bytesSent = 0;

    uint64_t fileSize = 0;
    std::string currentFileName;
    std::chrono::steady_clock::time_point startTime;
};

// Структура для независимых UDP-сессий (Решает проблему глобальных переменных)
struct UdpSession {
    std::ofstream outFile;
    uint32_t expectedSeq = 0;
};
// Словарь активных UDP-передач. Ключ - строка "IP:PORT"
std::map<std::string, UdpSession> udpSessions;

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

// Настройка SO_KEEPALIVE для быстрого обнаружения обрывов связи (30 сек)
void enableKeepAlive(socket_t s) {
    int optval = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&optval, sizeof(optval));

#ifdef _WIN32
    struct tcp_keepalive alive;
    alive.onoff = 1;
    alive.keepalivetime = 30000;    // 30 секунд бездействия до первой проверки
    alive.keepaliveinterval = 5000; // 5 секунд между повторными проверками
    DWORD dwBytesRet = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), NULL, 0, &dwBytesRet, NULL, NULL);
#else
    int idle = 30;     // Время простоя
    int interval = 5;  // Интервал
    int maxpkt = 3;    // Кол-во попыток
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &maxpkt, sizeof(maxpkt));
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

std::string getAddrString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

// --- Обработка TCP ---

void handleTcpCommand(ClientSession& session, const Command& cmd) {
    if (cmd.keyword == "ECHO") {
        std::string msg = "ECHO: " + (cmd.args.empty() ? "" : cmd.args[0]) + "\n";
        send(session.socket, msg.c_str(), (int)msg.length(), 0);
    }
    else if (cmd.keyword == "TIME") {
        time_t now = time(0);
        std::string t = std::ctime(&now);
        send(session.socket, t.c_str(), (int)t.length(), 0);
    }
    else if (cmd.keyword == "UPLOAD" && cmd.args.size() >= 2) {
        session.currentFileName = getBasename(cmd.args[0]);
        session.fileSize = std::stoull(cmd.args[1]);
        session.isUploading = true;
        session.bytesReceived = 0;
        session.outFile.open(session.currentFileName, std::ios::binary | std::ios::trunc);
        session.startTime = std::chrono::steady_clock::now();

        std::string res = "READY\n";
        send(session.socket, res.c_str(), (int)res.length(), 0);
    }
    // НОВАЯ ЛОГИКА: Команда на скачивание (DOWNLOAD)
    else if (cmd.keyword == "DOWNLOAD" && cmd.args.size() >= 1) {
        std::string filename = getBasename(cmd.args[0]);
        session.inFile.open(filename, std::ios::binary | std::ios::ate);
        if (session.inFile.is_open()) {
            session.fileSize = session.inFile.tellg(); // Узнаем размер файла
            session.inFile.seekg(0);                   // Возвращаемся в начало
            session.isDownloading = true;
            session.bytesSent = 0;
            session.currentFileName = filename;
            session.startTime = std::chrono::steady_clock::now();

            std::string res = "READY " + std::to_string(session.fileSize) + "\n";
            send(session.socket, res.c_str(), (int)res.length(), 0);
            std::cout << "Started sending " << filename << " to " << session.ip << std::endl;
        }
        else {
            std::string res = "FILE_NOT_FOUND\n";
            send(session.socket, res.c_str(), (int)res.length(), 0);
        }
    }
    else if (cmd.keyword == "EXIT" || cmd.keyword == "QUIT" || cmd.keyword == "CLOSE") {
        std::string msg = "GOODBYE\n";
        send(session.socket, msg.c_str(), (int)msg.length(), 0);
        closesocket(session.socket);
        session.socket = INVALID_SOCKET;
    }
    else {
        std::string msg = "UNKNOWN_COMMAND: " + cmd.keyword + "\n";
        send(session.socket, msg.c_str(), (int)msg.length(), 0);
    }
}

void processClientData(ClientSession& session) {
    char buffer[BUFFER_SIZE];
    int r = recv(session.socket, buffer, BUFFER_SIZE, 0);

    if (r <= 0) {
        std::cout << "Client disconnected or error: " << session.ip << std::endl;
        if (session.isUploading) session.outFile.close();
        if (session.isDownloading) session.inFile.close();
        closesocket(session.socket);
        session.socket = INVALID_SOCKET;
        return;
    }

    if (session.isUploading) {
        session.outFile.write(buffer, r);
        session.bytesReceived += r;
        if (session.bytesReceived >= session.fileSize) {
            session.isUploading = false;
            session.outFile.close();
            auto end = std::chrono::steady_clock::now();
            double duration = std::chrono::duration<double>(end - session.startTime).count();
            std::cout << "TCP Upload Finished: " << session.currentFileName << " at "
                << calculateBitrate(session.fileSize, duration) << " Mbps" << std::endl;

            std::string msg = "UPLOAD_COMPLETE\n";
            send(session.socket, msg.c_str(), (int)msg.length(), 0);
        }
    }
    else {
        session.cmdBuffer.append(buffer, r);
        size_t pos;
        while ((pos = session.cmdBuffer.find('\n')) != std::string::npos) {
            std::string line = session.cmdBuffer.substr(0, pos);
            session.cmdBuffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (!line.empty()) {
                Command cmd = parseCommand(line);
                handleTcpCommand(session, cmd);
            }
        }
    }
}

// НОВАЯ ЛОГИКА: Неблокирующая отправка файла клиенту (DOWNLOAD)
void processClientDownload(ClientSession& session) {
    char buffer[BUFFER_SIZE];
    session.inFile.read(buffer, BUFFER_SIZE);
    int bytesRead = (int)session.inFile.gcount();

    if (bytesRead > 0) {
        int sent = send(session.socket, buffer, bytesRead, 0);
        if (sent > 0) {
            session.bytesSent += sent;
            // Если сокет "задохнулся" и отправил не весь чанк - возвращаем курсор файла
            if (sent < bytesRead) {
                session.inFile.seekg((uint64_t)session.inFile.tellg() - (bytesRead - sent));
            }
        }
        else {
            // EAGAIN/EWOULDBLOCK, откатываем чтение файла на следующую итерацию
            session.inFile.seekg((uint64_t)session.inFile.tellg() - bytesRead);
        }
    }

    if (session.bytesSent >= session.fileSize) {
        session.isDownloading = false;
        session.inFile.close();
        auto end = std::chrono::steady_clock::now();
        double duration = std::chrono::duration<double>(end - session.startTime).count();
        std::cout << "TCP Download Finished for " << session.ip << " at "
            << calculateBitrate(session.fileSize, duration) << " Mbps" << std::endl;
    }
}

// --- Обработка UDP (Словарь сессий) ---

void processUdpPacket(socket_t s) {
    sockaddr_in clientAddr{}; socklen_t addrLen = sizeof(clientAddr);
    UdpPacket pkt;
    int bytes = recvfrom(s, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&clientAddr, &addrLen);
    if (bytes <= 0) return;

    std::string clientKey = getAddrString(clientAddr); // Получаем IP:PORT клиента как строку

    if (pkt.type == 3) { // START
        if (udpSessions.count(clientKey)) {
            udpSessions[clientKey].outFile.close(); // Закрываем старый файл если был сбой
        }
        udpSessions[clientKey].outFile.open("udp_mpx_" + std::string(pkt.data, pkt.dataSize), std::ios::binary);
        udpSessions[clientKey].expectedSeq = 0;

        UdpPacket ack{ 0, 1, 0, "" };
        sendto(s, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
    }
    else if (pkt.type == 0 && udpSessions.count(clientKey) && udpSessions[clientKey].outFile.is_open()) { // DATA
        UdpSession& session = udpSessions[clientKey];
        if (pkt.seq == session.expectedSeq) {
            session.outFile.write(pkt.data, pkt.dataSize);
            session.expectedSeq++;
        }
        UdpPacket ack{ session.expectedSeq - 1, 1, 0, "" };
        sendto(s, (char*)&ack, sizeof(uint32_t) * 3, 0, (sockaddr*)&clientAddr, addrLen);
    }
    else if (pkt.type == 2) { // FIN
        if (udpSessions.count(clientKey)) {
            udpSessions[clientKey].outFile.close();
            udpSessions.erase(clientKey); // Очищаем сессию
            std::cout << "UDP Multiplexed transfer finished for: " << clientKey << std::endl;
        }
    }
}

// --- Главный цикл сервера ---

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
        fd_set readfds, writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds); // Теперь мы слушаем и на чтение, и на запись

        FD_SET(tcpSrv, &readfds);
        FD_SET(udpSrv, &readfds);

        socket_t max_fd = (tcpSrv > udpSrv) ? tcpSrv : udpSrv;

        for (const auto& client : clients) {
            FD_SET(client.socket, &readfds); // Всех слушаем на входящие (команды или UPLOAD)
            if (client.isDownloading) {
                FD_SET(client.socket, &writefds); // На отправку слушаем только если идет DOWNLOAD
            }
            if (client.socket > max_fd) max_fd = client.socket;
        }

        // Обновленный вызов select (отслеживает как readfds, так и writefds)
        if (select((int)max_fd + 1, &readfds, &writefds, NULL, NULL) < 0) continue;

        // 1. Новое TCP подключение
        if (FD_ISSET(tcpSrv, &readfds)) {
            sockaddr_in ca{}; socklen_t cl = sizeof(ca);
            socket_t cs = accept(tcpSrv, (sockaddr*)&ca, &cl);
            if (cs != INVALID_SOCKET) {
                setNonBlocking(cs);
                enableKeepAlive(cs); // Включаем SO_KEEPALIVE для быстрого обнаружения обрывов
                char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &ca.sin_addr, ip, INET_ADDRSTRLEN);
                clients.push_back({ cs, std::string(ip) });
                std::cout << "New client: " << ip << std::endl;
            }
        }

        // 2. Новые UDP данные
        if (FD_ISSET(udpSrv, &readfds)) {
            processUdpPacket(udpSrv);
        }

        // 3. Обработка существующих TCP клиентов (Чтение + Запись)
        auto it = clients.begin();
        while (it != clients.end()) {
            bool socketClosed = false;

            // Если есть данные для чтения (поступила команда или чанк UPLOAD файла)
            if (FD_ISSET(it->socket, &readfds)) {
                processClientData(*it);
                if (it->socket == INVALID_SOCKET) socketClosed = true;
            }

            // Если сокет готов принимать данные и у нас включен режим DOWNLOAD
            if (!socketClosed && it->isDownloading && FD_ISSET(it->socket, &writefds)) {
                processClientDownload(*it);
                if (it->socket == INVALID_SOCKET) socketClosed = true;
            }

            if (socketClosed) {
                it = clients.erase(it);
            }
            else {
                ++it;
            }
        }
    }
}

// --- Клиентская часть ---

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

    // Включаем SO_KEEPALIVE и на клиенте тоже, чтобы он реагировал на падение сервера
    enableKeepAlive(ts);

    if (connect(ts, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        std::cerr << "Failed to connect to server." << std::endl;
        return;
    }

    std::cout << "Commands: ECHO, TIME, UPLOAD <path>, DOWNLOAD <filename>, UDP_UPLOAD <path>, EXIT" << std::endl;
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
            std::cout << "TCP Upload initiated (synchronous on client)." << std::endl;
        }
        else if (cmd.keyword == "DOWNLOAD" && !cmd.args.empty()) {
            std::string h = "DOWNLOAD " + cmd.args[0] + "\n";
            send(ts, h.c_str(), (int)h.length(), 0);

            // Читаем первую строку ответа (до \n)
            std::string respLine;
            char c;
            while (recv(ts, &c, 1, 0) > 0 && c != '\n') {
                if (c != '\r') respLine += c;
            }

            Command resp = parseCommand(respLine);
            if (resp.keyword == "READY" && resp.args.size() >= 1) {
                uint64_t sz = std::stoull(resp.args[0]);
                std::ofstream f("dl_" + getBasename(cmd.args[0]), std::ios::binary);
                uint64_t recvd = 0;
                char b[BUFFER_SIZE];

                std::cout << "Downloading " << sz << " bytes..." << std::endl;
                auto start = std::chrono::steady_clock::now();

                while (recvd < sz) {
                    int toRead = std::min((uint64_t)BUFFER_SIZE, sz - recvd);
                    int r = recv(ts, b, toRead, 0);
                    if (r <= 0) break;
                    f.write(b, r);
                    recvd += r;
                }

                auto end = std::chrono::steady_clock::now();
                double duration = std::chrono::duration<double>(end - start).count();
                std::cout << "TCP Download finished at " << calculateBitrate(sz, duration) << " Mbps" << std::endl;
            }
            else {
                std::cout << "Server: " << respLine << std::endl;
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