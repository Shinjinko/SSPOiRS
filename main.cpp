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

// ===================================================================================
// РАЗДЕЛ 1: КРОССПЛАТФОРМЕННАЯ НАСТРОЙКА И ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
// ===================================================================================

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#else
#include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netdb.h>
    typedef int socket_t;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

const int PORT = 12345;
const int BUFFER_SIZE = 4096;

// Структура для хранения состояния прерванной передачи
struct TransferState {
    std::string filename;
    std::string direction; // "UPLOAD" или "DOWNLOAD"
    uint64_t bytesTransferred;
};

// Глобальная карта для хранения состояний (IP -> State)
std::map<std::string, TransferState> g_transferStates;
// Мьютекс для защиты доступа к глобальной карте состояний
std::mutex g_stateMutex;


// ===================================================================================
// РАЗДЕЛ 2: ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ===================================================================================

struct Command {
    std::string keyword;
    std::vector<std::string> args;
};

void printError(const std::string& message) {
#ifdef _WIN32
    std::cerr << message << " с кодом ошибки: " << WSAGetLastError() << std::endl;
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
        while (iss >> arg) {
            cmd.args.push_back(arg);
        }
    }
    return cmd;
}

double calculateBitrate(uint64_t bytes, double seconds) {
    if (seconds <= 0) return 0.0;
    return (static_cast<double>(bytes) * 8.0) / (seconds * 1000000.0); // Мбит/с
}

std::string getBasename(const std::string& path) {
    size_t last_slash = path.find_last_of("/\\");
    if (std::string::npos != last_slash) {
        return path.substr(last_slash + 1);
    }
    return path;
}

// ===================================================================================
// РАЗДЕЛ 3: ЛОГИКА СЕРВЕРА (ОБРАБОТКА КОМАНД)
// ===================================================================================

void handleUpload(socket_t sock, const Command& cmd, const std::string& clientIp) {
    if (cmd.args.empty()) { send(sock, "Ошибка: имя файла не указано.\n", 33, 0); return; }
    const std::string& filename = cmd.args[0];
    uint64_t fileSize = std::stoull(cmd.args[1]);
    uint64_t offset = 0;

    // Проверяем, есть ли незавершенная загрузка для этого клиента и файла
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto it = g_transferStates.find(clientIp);
        if (it != g_transferStates.end() && it->second.filename == filename && it->second.direction == "UPLOAD") {
            offset = it->second.bytesTransferred;
            std::cout << "[" << clientIp << "] Возобновление загрузки файла " << filename << " с " << offset << " байт." << std::endl;
        }
    }

    std::ostringstream response;
    response << "RESUME " << offset << "\n";
    send(sock, response.str().c_str(), response.str().length(), 0);

    std::ofstream outFile(filename, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
    if (!outFile) { send(sock, "Ошибка: не удалось открыть файл на сервере.\n", 50, 0); return; }

    char buffer[BUFFER_SIZE];
    uint64_t totalBytesReceived = offset;
    auto startTime = std::chrono::high_resolution_clock::now();

    while (totalBytesReceived < fileSize) {
        int bytesReceived = recv(sock, buffer, BUFFER_SIZE, 0);
        if (bytesReceived <= 0) { // Соединение разорвано
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_transferStates[clientIp] = {filename, "UPLOAD", totalBytesReceived};
            std::cout << "[" << clientIp << "] Соединение разорвано. Сохранено состояние UPLOAD для " << filename << " на " << totalBytesReceived << " байт." << std::endl;
            return;
        }
        outFile.write(buffer, bytesReceived);
        totalBytesReceived += bytesReceived;
    }
    outFile.close();

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = endTime - startTime;
    double bitrate = calculateBitrate(fileSize - offset, duration.count());

    std::cout << "[" << clientIp << "] Файл " << filename << " успешно загружен. Скорость: " << std::fixed << bitrate << " Мбит/с" << std::endl;
    send(sock, "Загрузка завершена.\n", 29, 0);

    // Удаляем состояние после успешной загрузки
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_transferStates.erase(clientIp);
    }
}

void handleDownload(socket_t sock, const Command& cmd, const std::string& clientIp) {
    if (cmd.args.empty()) { send(sock, "Ошибка: имя файла не указано.\n", 33, 0); return; }
    const std::string& filename = cmd.args[0];

    std::ifstream inFile(filename, std::ios::binary | std::ios::ate);
    if (!inFile) { send(sock, "Ошибка: файл не найден на сервере.\n", 44, 0); return; }

    uint64_t fileSize = inFile.tellg();
    uint64_t offset = 0;

    // Проверяем на возобновление
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        auto it = g_transferStates.find(clientIp);
        if (it != g_transferStates.end() && it->second.filename == filename && it->second.direction == "DOWNLOAD") {
            offset = it->second.bytesTransferred;
            std::cout << "[" << clientIp << "] Возобновление отдачи файла " << filename << " с " << offset << " байт." << std::endl;
        }
    }

    inFile.seekg(offset, std::ios::beg);

    std::ostringstream header;
    header << "FILE " << filename << " " << fileSize << " " << offset << "\n";
    send(sock, header.str().c_str(), header.str().length(), 0);

    char readyBuffer[10];
    if (recv(sock, readyBuffer, sizeof(readyBuffer), 0) <= 0) { return; }

    char buffer[BUFFER_SIZE];
    uint64_t totalBytesSent = offset;
    auto startTime = std::chrono::high_resolution_clock::now();

    while (totalBytesSent < fileSize && inFile.good()) {
        inFile.read(buffer, BUFFER_SIZE);
        std::streamsize bytesRead = inFile.gcount();
        if (bytesRead > 0) {
            int bytesSent = send(sock, buffer, bytesRead, 0);
            if (bytesSent == SOCKET_ERROR) {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_transferStates[clientIp] = {filename, "DOWNLOAD", totalBytesSent};
                std::cout << "[" << clientIp << "] Соединение разорвано. Сохранено состояние DOWNLOAD для " << filename << " на " << totalBytesSent << " байт." << std::endl;
                return;
            }
            totalBytesSent += bytesSent;
        }
    }
    inFile.close();

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = endTime - startTime;
    double bitrate = calculateBitrate(fileSize - offset, duration.count());

    std::cout << "[" << clientIp << "] Файл " << filename << " успешно отправлен. Скорость: " << std::fixed << bitrate << " Мбит/с" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_transferStates.erase(clientIp);
    }
}


// Главная функция для обработки одного клиента в отдельном потоке
// Главная функция для обработки одного клиента в отдельном потоке
void handleClientConnection(socket_t clientSocket, std::string clientIp) {
    std::cout << "[" << clientIp << "] Клиент подключен. Поток ID: " << std::this_thread::get_id() << std::endl;
    char buffer[BUFFER_SIZE];
    std::string commandLine;

    while (true) {
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);
        if (bytesReceived <= 0) {
            std::cout << "[" << clientIp << "] Клиент отключился." << std::endl;
            break;
        }

        commandLine.append(buffer, bytesReceived);
        size_t pos;
        while ((pos = commandLine.find('\n')) != std::string::npos) {
            std::string singleCmdLine = commandLine.substr(0, pos);
            commandLine.erase(0, pos + 1);
            if (singleCmdLine.empty()) continue;

            Command cmd = parseCommand(singleCmdLine);
            std::cout << "[" << clientIp << "] Получена команда: " << cmd.keyword << std::endl;

            // ================== ИСПРАВЛЕНИЕ ЗДЕСЬ ==================
            if (cmd.keyword == "ECHO") {
                std::string dataToEcho;
                size_t space_pos = singleCmdLine.find(' ');
                if (space_pos != std::string::npos) {
                    dataToEcho = singleCmdLine.substr(space_pos + 1);
                }
                std::string response = dataToEcho + "\n";
                send(clientSocket, response.c_str(), response.length(), 0);
            }
                // ================== КОНЕЦ ИСПРАВЛЕНИЯ ==================
            else if (cmd.keyword == "TIME") {
                auto t = std::time(nullptr);
                std::string timeStr = std::ctime(&t);
                send(clientSocket, timeStr.c_str(), timeStr.length(), 0);
            }
            else if (cmd.keyword == "UPLOAD") {
                handleUpload(clientSocket, cmd, clientIp);
            }
            else if (cmd.keyword == "DOWNLOAD") {
                handleDownload(clientSocket, cmd, clientIp);
            }
            else if (cmd.keyword == "CLOSE" || cmd.keyword == "EXIT") {
                goto end_connection;
            }
            else {
                send(clientSocket, "Неизвестная команда\n", 29, 0);
            }
        }
    }

    end_connection:
    closesocket(clientSocket);
    std::cout << "[" << clientIp << "] Соединение закрыто." << std::endl;
}

void runServer() {
    socket_t serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) { printError("Ошибка создания сокета"); return; }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(serverSocket, SOMAXCONN);
    std::cout << "Сервер запущен на порту " << PORT << ". Ожидание подключений..." << std::endl;

    while (true) {
        sockaddr_in clientAddr{};
        socklen_t clientAddrSize = sizeof(clientAddr);
        socket_t clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);
        if (clientSocket == INVALID_SOCKET) continue;

        char clientIpStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIpStr, INET_ADDRSTRLEN);

        // Создаем новый поток для каждого клиента
        std::thread clientThread(handleClientConnection, clientSocket, std::string(clientIpStr));
        clientThread.detach(); // Отсоединяем поток, чтобы он работал независимо
    }
    closesocket(serverSocket);
}


// ===================================================================================
// РАЗДЕЛ 4: ЛОГИКА КЛИЕНТА (ИСПРАВЛЕНО)
// ===================================================================================

void executeUpload(socket_t sock, const std::string& localPath) {
    // ИЗМЕНЕНИЕ ЗДЕСЬ: Используем только имя файла для отправки серверу
    std::string filename = getBasename(localPath);

    std::ifstream inFile(localPath, std::ios::binary | std::ios::ate);
    if (!inFile) {
        std::cerr << "Ошибка: не удалось открыть локальный файл " << localPath << std::endl;
        return;
    }

    uint64_t fileSize = inFile.tellg();

    std::ostringstream header;
    // ИЗМЕНЕНИЕ ЗДЕСЬ: Отправляем только имя файла
    header << "UPLOAD " << filename << " " << fileSize << "\n";
    send(sock, header.str().c_str(), header.str().length(), 0);

    char buffer[BUFFER_SIZE];
    int bytesReceived = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytesReceived <= 0) {
        std::cerr << "Сервер не ответил." << std::endl;
        return;
    }

    buffer[bytesReceived] = '\0';
    Command resumeCmd = parseCommand(buffer);
    uint64_t offset = 0;
    if (resumeCmd.keyword == "RESUME" && !resumeCmd.args.empty()) {
        offset = std::stoull(resumeCmd.args[0]);
        std::cout << "Сервер разрешил возобновление с " << offset << " байт." << std::endl;
    }

    inFile.seekg(offset, std::ios::beg);

    std::cout << "Начинаю загрузку файла " << filename << "..." << std::endl;
    while (inFile.good()) {
        inFile.read(buffer, BUFFER_SIZE);
        if (send(sock, buffer, inFile.gcount(), 0) == SOCKET_ERROR) {
            printError("Ошибка отправки файла");
            return;
        }
    }
    inFile.close();

    bytesReceived = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if(bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        std::cout << "Ответ сервера: " << buffer;
    }
}

void executeDownload(socket_t sock, const std::string& filename) {
    // ИЗМЕНЕНИЕ ЗДЕСЬ: Убеждаемся, что отправляем чистое имя файла без пути
    std::string clean_filename = getBasename(filename);

    std::string cmd_to_send = "DOWNLOAD " + clean_filename + "\n";
    send(sock, cmd_to_send.c_str(), cmd_to_send.length(), 0);

    char buffer[BUFFER_SIZE];
    int bytesReceived = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytesReceived <= 0) {
        std::cout << "Сервер не ответил или файл не найден." << std::endl;
        if(bytesReceived > 0) buffer[bytesReceived] = '\0'; std::cout << "Ответ: " << buffer << std::endl;
        return;
    }

    buffer[bytesReceived] = '\0';
    Command fileCmd = parseCommand(buffer);

    if (fileCmd.keyword != "FILE") {
        std::cerr << "Ошибка: " << std::string(buffer, bytesReceived);
        return;
    }

    uint64_t fileSize = std::stoull(fileCmd.args[1]);
    uint64_t offset = std::stoull(fileCmd.args[2]);

    std::ofstream outFile(clean_filename, std::ios::binary | (offset > 0 ? std::ios::app : std::ios::trunc));
    if (!outFile) {
        std::cerr << "Ошибка: не удалось создать локальный файл." << std::endl;
        return;
    }

    std::cout << "Начинаю скачивание " << clean_filename << " (размер: " << fileSize << " байт) с " << offset << " байт." << std::endl;
    send(sock, "READY\n", 6, 0);

    uint64_t totalBytesReceived = offset;
    while (totalBytesReceived < fileSize) {
        bytesReceived = recv(sock, buffer, BUFFER_SIZE, 0);
        if (bytesReceived <= 0) {
            std::cout << "Соединение разорвано во время скачивания." << std::endl;
            return;
        }
        outFile.write(buffer, bytesReceived);
        totalBytesReceived += bytesReceived;
    }
    outFile.close();
    // ИЗМЕНЕНИЕ ЗДЕСЬ: Добавлено сообщение об успехе
    std::cout << "Файл " << clean_filename << " успешно скачан." << std::endl;
}

void runClient(const char* serverIp) {
    socket_t clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    inet_pton(AF_INET, serverIp, &serverAddr.sin_addr);

    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printError("Ошибка подключения");
        return;
    }

    std::cout << "Успешно подключено. Введите команду (ECHO, TIME, UPLOAD, DOWNLOAD, CLOSE):" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        Command cmd = parseCommand(line);
        if (cmd.keyword.empty()) continue;

        if (cmd.keyword == "UPLOAD") {
            if(cmd.args.empty()) { std::cerr << "Укажите полный путь к файлу для загрузки." << std::endl; continue; }
            executeUpload(clientSocket, cmd.args[0]);
        }
        else if (cmd.keyword == "DOWNLOAD") {
            if(cmd.args.empty()) { std::cerr << "Укажите имя файла для скачивания." << std::endl; continue; }
            executeDownload(clientSocket, cmd.args[0]);
        }
        else {
            send(clientSocket, (line + "\n").c_str(), line.length() + 1, 0);
            char responseBuffer[BUFFER_SIZE] = {0};
            int bytes = recv(clientSocket, responseBuffer, BUFFER_SIZE - 1, 0);
            if (bytes > 0) {
                std::cout << "Ответ сервера: " << std::string(responseBuffer, bytes);
            }
            else {
                std::cout << "Соединение потеряно." << std::endl;
                break;
            }
        }
        if (cmd.keyword == "CLOSE" || cmd.keyword == "EXIT") break;
    }
    closesocket(clientSocket);
}

// ===================================================================================
// РАЗДЕЛ 5: ГЛАВНАЯ ФУНКЦИЯ
// ===================================================================================

void printUsage(const char* progName) {
    std::cerr << "Использование: " << std::endl;
    std::cerr << "  " << progName << " server" << std::endl;
    std::cerr << "  " << progName << " client <ip_адрес_сервера>" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
#endif

    if (argc < 2) { printUsage(argv[0]); return 1; }

    std::string mode = argv[1];
    if (mode == "server") runServer();
    else if (mode == "client" && argc > 2) runClient(argv[2]);
    else printUsage(argv[0]);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}