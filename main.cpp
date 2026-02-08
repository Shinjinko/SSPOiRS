#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>

// Заголовки для сетевого программирования
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
    typedef int socket_t;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

// --- Глобальные константы и утилиты ---
const int PORT = 12345;
const int BUFFER_SIZE = 4096;

void printError(const std::string& message) {
#ifdef _WIN32
    std::cerr << message << " с кодом ошибки: " << WSAGetLastError() << std::endl;
#else
    perror(message.c_str());
#endif
}

void setKeepAlive(socket_t sock) {
    int optval = 1;
#ifdef _WIN32
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&optval), sizeof(optval)) == SOCKET_ERROR) {
        printError("setsockopt(SO_KEEPALIVE) не удался");
    }
#else
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0) {
        printError("setsockopt(SO_KEEPALIVE) не удался");
    }
#endif
}

// --- Логика Сервера ---
void handleClient(socket_t clientSocket) {
    char buffer[BUFFER_SIZE];
    int bytesReceived;

    std::cout << "Клиент подключен. Ожидание команд..." << std::endl;

    while ((bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0)) > 0) {
        std::string command(buffer, bytesReceived);
        command.erase(std::remove(command.begin(), command.end(), '\n'), command.end());
        command.erase(std::remove(command.begin(), command.end(), '\r'), command.end());

        std::istringstream iss(command);
        std::string cmd;
        iss >> cmd;

        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        if (cmd == "ECHO") {
            std::string echoMsg = command.substr(command.find(" ") + 1) + "\n";
            send(clientSocket, echoMsg.c_str(), echoMsg.length(), 0);
        } else if (cmd == "TIME") {
            auto now = std::chrono::system_clock::now();
            std::time_t server_time = std::chrono::system_clock::to_time_t(now);
            std::string timeStr = std::ctime(&server_time);
            send(clientSocket, timeStr.c_str(), timeStr.length(), 0);
        } else if (cmd == "CLOSE" || cmd == "EXIT" || cmd == "QUIT") {
            std::cout << "Клиент запросил закрытие соединения." << std::endl;
            break;
        } else if (cmd == "UPLOAD") {
            std::string filename;
            uint64_t file_size;
            iss >> filename >> file_size;

            std::ofstream outFile(filename, std::ios::binary);
            if (!outFile) {
                std::string errMsg = "Ошибка: не удалось создать файл на сервере.\n";
                send(clientSocket, errMsg.c_str(), errMsg.length(), 0);
                continue;
            }

            std::string readyMsg = "READY\n";
            send(clientSocket, readyMsg.c_str(), readyMsg.length(), 0);

            uint64_t totalBytesReceived = 0;
            auto startTime = std::chrono::high_resolution_clock::now();

            while (totalBytesReceived < file_size) {
                bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE, 0);
                if (bytesReceived <= 0) break;
                outFile.write(buffer, bytesReceived);
                totalBytesReceived += bytesReceived;
            }
            outFile.close();

            auto endTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> duration = endTime - startTime;
            double bitrate = (totalBytesReceived * 8) / (duration.count() * 1000000); // Мбит/с

            std::ostringstream response;
            response << "Файл " << filename << " успешно загружен. "
                     << "Размер: " << totalBytesReceived << " байт. "
                     << "Скорость: " << std::fixed << bitrate << " Мбит/с\n";
            send(clientSocket, response.str().c_str(), response.str().length(), 0);

        } else if (cmd == "DOWNLOAD") {
            std::string filename;
            uint64_t offset = 0;
            iss >> filename >> offset;

            std::ifstream inFile(filename, std::ios::binary | std::ios::ate);
            if (!inFile) {
                std::string errMsg = "Ошибка: Файл не найден на сервере.\n";
                send(clientSocket, errMsg.c_str(), errMsg.length(), 0);
                continue;
            }

            uint64_t file_size = inFile.tellg();
            uint64_t remaining_size = file_size - offset;
            inFile.seekg(offset, std::ios::beg);

            std::ostringstream header;
            header << "FILE " << filename << " " << remaining_size << "\n";
            send(clientSocket, header.str().c_str(), header.str().length(), 0);

            char clientResponse[10];
            recv(clientSocket, clientResponse, sizeof(clientResponse), 0);
            if (std::string(clientResponse).find("READY") == std::string::npos) {
                continue;
            }

            uint64_t totalBytesSent = 0;
            auto startTime = std::chrono::high_resolution_clock::now();

            while (totalBytesSent < remaining_size && inFile.good()) {
                inFile.read(buffer, BUFFER_SIZE);
                std::streamsize bytesRead = inFile.gcount();
                if (bytesRead > 0) {
                    int bytesSent = send(clientSocket, buffer, bytesRead, 0);
                    if (bytesSent == SOCKET_ERROR) {
                        break;
                    }
                    totalBytesSent += bytesSent;
                }
            }
            inFile.close();

            auto endTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> duration = endTime - startTime;
            double bitrate = (totalBytesSent * 8) / (duration.count() * 1000000); // Мбит/с

            std::cout << "Передача файла " << filename << " завершена. "
                      << "Отправлено: " << totalBytesSent << " байт. "
                      << "Скорость: " << std::fixed << bitrate << " Мбит/с" << std::endl;

        } else {
            std::string unknownCmd = "Неизвестная команда\n";
            send(clientSocket, unknownCmd.c_str(), unknownCmd.length(), 0);
        }
    }

    if (bytesReceived == 0) {
        std::cout << "Клиент отключился." << std::endl;
    } else if (bytesReceived == SOCKET_ERROR) {
        printError("recv не удался");
    }

    closesocket(clientSocket);
}

void runServer() {
    socket_t serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == INVALID_SOCKET) {
        printError("Создание сокета не удалось");
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(PORT);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printError("Bind не удался");
        closesocket(serverSocket);
        return;
    }

    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        printError("Listen не удался");
        closesocket(serverSocket);
        return;
    }

    std::cout << "Сервер запущен на порту " << PORT << ". Ожидание подключений..." << std::endl;

    while (true) {
        sockaddr_in clientAddr;
        socklen_t clientAddrSize = sizeof(clientAddr);
        socket_t clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            printError("Accept не удался");
            continue;
        }

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
        std::cout << "Принято соединение от " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

        setKeepAlive(clientSocket);
        handleClient(clientSocket);
    }

    closesocket(serverSocket);
}

// --- Логика Клиента ---
void runClient(const char* serverIp) {
    socket_t clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket == INVALID_SOCKET) {
        printError("Создание сокета не удалось");
        return;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    inet_pton(AF_INET, serverIp, &serverAddr.sin_addr);

    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printError("Подключение не удалось");
        closesocket(clientSocket);
        return;
    }

    std::cout << "Успешно подключено к серверу " << serverIp << ". Введите команду:" << std::endl;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // Добавляем \n для совместимости с серверным recv
        std::string command_to_send = line + "\n";

        // Отдельно обрабатываем UPLOAD
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        if (cmd == "UPLOAD") {
            std::string filename;
            iss >> filename;
            std::ifstream inFile(filename, std::ios::binary | std::ios::ate);
            if (!inFile) {
                std::cerr << "Ошибка: не удалось открыть локальный файл " << filename << std::endl;
                continue;
            }
            uint64_t file_size = inFile.tellg();
            inFile.seekg(0, std::ios::beg);

            std::ostringstream header;
            header << "UPLOAD " << filename << " " << file_size << "\n";
            send(clientSocket, header.str().c_str(), header.str().length(), 0);

            char serverResponse[BUFFER_SIZE];
            int bytesReceived = recv(clientSocket, serverResponse, BUFFER_SIZE, 0);
            if (bytesReceived > 0 && std::string(serverResponse, bytesReceived).find("READY") != std::string::npos) {
                std::cout << "Сервер готов к приему. Начинаю загрузку..." << std::endl;
                char buffer[BUFFER_SIZE];
                while(inFile.good()) {
                    inFile.read(buffer, BUFFER_SIZE);
                    send(clientSocket, buffer, inFile.gcount(), 0);
                }
                std::cout << "Загрузка завершена." << std::endl;
            } else {
                std::cerr << "Сервер не готов к приему файла." << std::endl;
            }
            inFile.close();

        } else {
            // Отправляем все остальные команды как есть
            send(clientSocket, command_to_send.c_str(), command_to_send.length(), 0);
        }

        // Получаем ответ от сервера
        char buffer[BUFFER_SIZE] = {0};
        int bytesReceived = recv(clientSocket, buffer, BUFFER_SIZE - 1, 0);
        if (bytesReceived > 0) {
            std::cout << "Ответ сервера: " << std::string(buffer, bytesReceived);
        } else {
            std::cout << "Соединение с сервером потеряно." << std::endl;
            break;
        }

        if (cmd == "CLOSE" || cmd == "EXIT" || cmd == "QUIT") {
            break;
        }
    }

    closesocket(clientSocket);
}


// --- Главная функция-переключатель ---
void printUsage(const char* progName) {
    std::cerr << "Использование: " << std::endl;
    std::cerr << "  " << progName << " server" << std::endl;
    std::cerr << "  " << progName << " client <ip_адрес_сервера>" << std::endl;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printError("WSAStartup не удался");
        return 1;
    }
#endif

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];
    if (mode == "server") {
        runServer();
    } else if (mode == "client") {
        if (argc < 3) {
            std::cerr << "Ошибка: не указан IP-адрес сервера." << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        runClient(argv[2]);
    } else {
        std::cerr << "Ошибка: неизвестный режим '" << mode << "'" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}