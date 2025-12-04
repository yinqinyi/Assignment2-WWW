#include <iostream>
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <limits>
#include <windows.h> // For ShellExecute

#pragma comment(lib, "Ws2_32.lib")

const int SERVER_PORT = 8080;
const std::string LOOPBACK_ADDR = "127.0.0.1";

int main() {
    while (true) {
        std::cout << "Please enter server URL (e.g., 127.0.0.1 or another URL, press Enter or type 'quit' to exit): ";
        std::string userInput;
        std::getline(std::cin, userInput);

        if (userInput.empty() || userInput == "quit") {
            std::cout << "Program ended." << std::endl;
            break;
        }

        while (!userInput.empty() && (userInput.front() == ' ' || userInput.front() == '\t')) userInput.erase(userInput.begin());
        while (!userInput.empty() && (userInput.back() == ' ' || userInput.back() == '\t')) userInput.pop_back();

        if (userInput.empty() || userInput == LOOPBACK_ADDR) {
            userInput = LOOPBACK_ADDR + "/main/index.html";
        }

        bool localRequest = false;
        if (userInput.find(LOOPBACK_ADDR) == 0) {
            localRequest = true;
        }

        if (localRequest) {
            std::string httpPath = userInput.substr(LOOPBACK_ADDR.size());
            if (httpPath.empty()) {
                httpPath = "/index.html";
            }
            else if (httpPath.front() != '/') {
                httpPath = "/" + httpPath;
            }

            WSADATA wsaEnv;
            if (WSAStartup(MAKEWORD(2, 2), &wsaEnv) != 0) {
                std::cerr << "WSAStartup failed." << std::endl;
                continue;
            }

            SOCKET tcpClient = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (tcpClient == INVALID_SOCKET) {
                std::cerr << "Failed to create socket." << std::endl;
                WSACleanup();
                continue;
            }

            sockaddr_in svrInfo;
            memset(&svrInfo, 0, sizeof(svrInfo));
            svrInfo.sin_family = AF_INET;
            svrInfo.sin_port = htons(SERVER_PORT);
            inet_pton(AF_INET, LOOPBACK_ADDR.c_str(), &svrInfo.sin_addr);

            if (connect(tcpClient, (sockaddr*)&svrInfo, sizeof(svrInfo)) == SOCKET_ERROR) {
                std::cerr << "Connect failed." << std::endl;
                closesocket(tcpClient);
                WSACleanup();
                continue;
            }

            std::string httpRequest = "GET " + httpPath + " HTTP/1.0\r\nHost: " + LOOPBACK_ADDR + "\r\n\r\n";
            send(tcpClient, httpRequest.c_str(), (int)httpRequest.size(), 0);

            char recvBuffer[4096];
            int recvSize = recv(tcpClient, recvBuffer, sizeof(recvBuffer) - 1, 0);
            std::string fullResponse;
            while (recvSize > 0) {
                recvBuffer[recvSize] = '\0';
                fullResponse.append(recvBuffer, recvSize);
                recvSize = recv(tcpClient, recvBuffer, sizeof(recvBuffer) - 1, 0);
            }

            closesocket(tcpClient);
            WSACleanup();

            if (fullResponse.empty()) {
                std::cerr << "No response received." << std::endl;
            }
            else {
                std::cout << "Response received:\n" << fullResponse << std::endl;

                std::cout << "Open in browser? (Y/N): ";
                char openChoice;
                std::cin >> openChoice;
                std::cin.ignore(1024, '\n');
                if (openChoice == 'Y' || openChoice == 'y') {
                    std::string openURL = "http://" + LOOPBACK_ADDR + ":" + std::to_string(SERVER_PORT) + httpPath;
                    ShellExecuteA(nullptr, "open", openURL.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                else {
                    std::cout << "Will not open in browser." << std::endl;
                }
            }

        }
        else {
            std::string externalURL = userInput;
            if (externalURL.find("http://") != 0 && externalURL.find("https://") != 0) {
                externalURL = "http://" + externalURL;
            }
            ShellExecuteA(nullptr, "open", externalURL.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    system("pause");
    return 0;
}