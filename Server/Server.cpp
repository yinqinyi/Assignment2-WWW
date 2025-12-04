#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fstream>
#include <string>
#include <map>
#include <filesystem> // C++17
#include <algorithm>
#include <thread>     // Added: for multithreading
#include <cstring>    // for strlen

#pragma comment(lib, "Ws2_32.lib")

const int SERVER_LISTEN_PORT = 8080;

std::string InferMimeType(const std::string& resourcePath) {
    size_t dotIndex = resourcePath.find_last_of('.');
    if (dotIndex == std::string::npos) return "application/octet-stream";

    std::string extension = resourcePath.substr(dotIndex);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    if (extension == ".html" || extension == ".htm") return "text/html";
    else if (extension == ".css") return "text/css";
    else if (extension == ".js") return "application/javascript";
    else if (extension == ".png") return "image/png";
    else if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    else if (extension == ".gif") return "image/gif";
    else if (extension == ".ico") return "image/x-icon";
    else if (extension == ".svg") return "image/svg+xml";
    else if (extension == ".mp4") return "video/mp4";
    else if (extension == ".webm") return "video/webm";
    return "application/octet-stream";
}

int ParseContentLength(const std::string& httpRequest) {
    int parsedLength = 0;
    size_t foundPos = httpRequest.find("Content-Length:");
    if (foundPos != std::string::npos) {
        size_t numberStart = foundPos + 15;
        while (numberStart < httpRequest.size() && (httpRequest[numberStart] == ' ' || httpRequest[numberStart] == '\t')) {
            numberStart++;
        }
        size_t numberEnd = httpRequest.find("\r\n", numberStart);
        if (numberEnd != std::string::npos) {
            std::string numStr = httpRequest.substr(numberStart, numberEnd - numberStart);
            try {
                parsedLength = std::stoi(numStr);
            }
            catch (...) {
                std::cerr << "[ERROR] Failed to parse Content-Length: " << numStr << std::endl;
            }
        }
    }
    return parsedLength;
}

// Get the value of custom header X-Filename from the request header, used to obtain the file name
std::string ExtractXFilename(const std::string& httpRequest) {
    std::string extractedName;
    size_t pos = httpRequest.find("X-Filename:");
    if (pos != std::string::npos) {
        size_t valStart = pos + strlen("X-Filename:");
        // Skip spaces
        while (valStart < httpRequest.size() && (httpRequest[valStart] == ' ' || httpRequest[valStart] == '\t')) {
            valStart++;
        }
        size_t valEnd = httpRequest.find("\r\n", valStart);
        if (valEnd != std::string::npos) {
            extractedName = httpRequest.substr(valStart, valEnd - valStart);
        }
    }
    return extractedName;
}

void ProcessConnection(SOCKET socketForClient) {
    // Read header (loop until "\r\n\r\n" is encountered)
    std::string accumulatedRequest;
    accumulatedRequest.reserve(2048);
    const int TEMP_RECV_SIZE = 4096;
    char tempRecvBuffer[TEMP_RECV_SIZE];
    bool headerComplete = false;
    int recvResult;

    // Set a reasonable upper limit to prevent malicious clients from sending excessively long headers (e.g., 64KB)
    const size_t MAX_ALLOWED_HEADER = 64 * 1024;

    while (!headerComplete) {
        recvResult = recv(socketForClient, tempRecvBuffer, TEMP_RECV_SIZE, 0);
        if (recvResult <= 0) {
            std::cerr << "[ERROR] recv() failed while reading header. ret=" << recvResult
                << " WSAGetLastError=" << WSAGetLastError() << std::endl;
            closesocket(socketForClient);
            return;
        }

        accumulatedRequest.append(tempRecvBuffer, recvResult);

        size_t headerTerm = accumulatedRequest.find("\r\n\r\n");
        if (headerTerm != std::string::npos) {
            headerComplete = true;
            break;
        }

        if (accumulatedRequest.size() > MAX_ALLOWED_HEADER) {
            std::cerr << "[ERROR] Request header too large." << std::endl;
            closesocket(socketForClient);
            return;
        }
    }

    // Find the header end position
    size_t hdrEndIndex = accumulatedRequest.find("\r\n\r\n");
    if (hdrEndIndex == std::string::npos) {
        std::cerr << "[ERROR] header end not found after loop (unexpected)." << std::endl;
        closesocket(socketForClient);
        return;
    }

    std::cout << "[DEBUG] Received request header:\n" << std::string(accumulatedRequest.c_str(), hdrEndIndex + 4) << std::endl;

    // Determine request method
    bool isMethodGet = (accumulatedRequest.rfind("GET ", 0) == 0);
    bool isMethodPost = (accumulatedRequest.rfind("POST ", 0) == 0);

    if (isMethodGet) {
        size_t qstart = accumulatedRequest.find("GET ") + 4;
        size_t qspace = accumulatedRequest.find(' ', qstart);
        if (qspace == std::string::npos) {
            std::string notFoundResponse = "HTTP/1.0 404 Not Found\r\nContent-Length:0\r\n\r\n";
            send(socketForClient, notFoundResponse.c_str(), (int)notFoundResponse.size(), 0);
            closesocket(socketForClient);
            return;
        }
        std::string reqPath = accumulatedRequest.substr(qstart, qspace - qstart);
        if (reqPath == "/" || reqPath == "/main" || reqPath == "/main/") reqPath = "/main/index.html";
        if (!reqPath.empty() && reqPath.front() == '/') reqPath.erase(0, 1);

        std::ifstream ifsFile(reqPath, std::ios::binary);
        if (!ifsFile.is_open()) {
            std::string notFoundResponse = "HTTP/1.0 404 Not Found\r\nContent-Length:0\r\n\r\n";
            send(socketForClient, notFoundResponse.c_str(), (int)notFoundResponse.size(), 0);
            closesocket(socketForClient);
            return;
        }
        std::string fileContents((std::istreambuf_iterator<char>(ifsFile)), std::istreambuf_iterator<char>());
        ifsFile.close();

        std::string mimeType = InferMimeType(reqPath);
        std::string okHeader =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: " + mimeType + "\r\n"
            "Content-Length: " + std::to_string(fileContents.size()) + "\r\n"
            "\r\n";

        send(socketForClient, okHeader.c_str(), (int)okHeader.size(), 0);
        send(socketForClient, fileContents.c_str(), (int)fileContents.size(), 0);

        closesocket(socketForClient);
        return;
    }
    else if (isMethodPost) {
        // Parse POST line and Content-Length
        size_t pstart = accumulatedRequest.find("POST ") + 5;
        size_t pspace = accumulatedRequest.find(' ', pstart);
        if (pspace == std::string::npos) {
            std::cerr << "[DEBUG] Malformed POST request line." << std::endl;
            std::string notFoundResponse = "HTTP/1.0 404 Not Found\r\nContent-Length:0\r\n\r\n";
            send(socketForClient, notFoundResponse.c_str(), (int)notFoundResponse.size(), 0);
            closesocket(socketForClient);
            return;
        }
        std::string postPath = accumulatedRequest.substr(pstart, pspace - pstart);
        if (postPath != "/upload") {
            std::string notFoundResponse = "HTTP/1.0 404 Not Found\r\nContent-Length:0\r\n\r\n";
            send(socketForClient, notFoundResponse.c_str(), (int)notFoundResponse.size(), 0);
            closesocket(socketForClient);
            return;
        }

        int contentLen = ParseContentLength(accumulatedRequest);
        std::cout << "[DEBUG] POST /upload Content-Length: " << contentLen << std::endl;
        if (contentLen <= 0) {
            std::cerr << "[ERROR] Invalid Content-Length." << std::endl;
            closesocket(socketForClient);
            return;
        }

        // Extract X-Filename
        std::string receivedFilename = ExtractXFilename(accumulatedRequest);
        if (receivedFilename.empty()) receivedFilename = "uploaded_file";

        // Prepare target file (ensure the directory exists)
        std::filesystem::create_directories("uploads");
        std::string diskPath = "uploads/" + receivedFilename;

        // Open the file and write the body part that is already in the header buffer
        std::ofstream ofsOut(diskPath, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!ofsOut.is_open()) {
            std::cerr << "[ERROR] Cannot open " << diskPath << " for writing." << std::endl;
            std::string errResp = "HTTP/1.0 500 Internal Server Error\r\nContent-Length:0\r\n\r\n";
            send(socketForClient, errResp.c_str(), (int)errResp.size(), 0);
            closesocket(socketForClient);
            return;
        }

        // Calculate remaining data after the header (the first recv has already been stored into the header string)
        size_t bodyIndex = hdrEndIndex + 4;
        int bytesWritten = 0;
        if (accumulatedRequest.size() > bodyIndex) {
            size_t initialAvail = accumulatedRequest.size() - bodyIndex;
            size_t toCopy = std::min<size_t>(contentLen, initialAvail);
            ofsOut.write(accumulatedRequest.data() + bodyIndex, (std::streamsize)toCopy);
            bytesWritten += (int)toCopy;
        }

        // Continue receiving the remaining body and write directly to the file until contentLength is reached
        const int BODY_BUF_SIZE = 8192;
        char bodyBuffer[BODY_BUF_SIZE];
        while (bytesWritten < contentLen) {
            int needed = contentLen - bytesWritten;
            int readNow = std::min<int>(needed, BODY_BUF_SIZE);
            int r = recv(socketForClient, bodyBuffer, readNow, 0);
            if (r > 0) {
                ofsOut.write(bodyBuffer, r);
                bytesWritten += r;
            }
            else if (r == 0) {
                // Client closed connection: if everything is written, finish normally; otherwise treat as error
                if (bytesWritten == contentLen) {
                    break;
                }
                else {
                    std::cerr << "[ERROR] Client closed connection early. written=" << bytesWritten
                        << " expected=" << contentLen << std::endl;
                    ofsOut.close();
                    // Optional: delete incomplete file
                    // std::filesystem::remove(diskPath);
                    closesocket(socketForClient);
                    return;
                }
            }
            else { // r < 0
                std::cerr << "[ERROR] recv error while reading body. WSAGetLastError=" << WSAGetLastError() << std::endl;
                ofsOut.close();
                // std::filesystem::remove(diskPath);
                closesocket(socketForClient);
                return;
            }
        }

        ofsOut.flush();
        ofsOut.close();

        std::cout << "[DEBUG] Received and wrote " << bytesWritten << " / " << contentLen
            << " bytes to " << diskPath << std::endl;

        // Send JSON response
        std::string fileUrl = "http://127.0.0.1:8080/" + diskPath;
        std::string jsonResponse = "{\"url\":\"" + fileUrl + "\"}";

        std::string responseHeader =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(jsonResponse.size()) + "\r\n"
            "\r\n";

        send(socketForClient, responseHeader.c_str(), (int)responseHeader.size(), 0);
        send(socketForClient, jsonResponse.c_str(), (int)jsonResponse.size(), 0);

        closesocket(socketForClient);
        std::cout << "[DEBUG] Client connection closed. File saved at " << diskPath << std::endl;
        return;
    }
    else {
        std::string notFoundResponse = "HTTP/1.0 404 Not Found\r\nContent-Length:0\r\n\r\n";
        send(socketForClient, notFoundResponse.c_str(), (int)notFoundResponse.size(), 0);
        closesocket(socketForClient);
        return;
    }
}

int main() {
    std::cout << "[DEBUG] Starting server initialization..." << std::endl;

    WSADATA wsaStartupData;
    int wsaInitCode = WSAStartup(MAKEWORD(2, 2), &wsaStartupData);
    if (wsaInitCode != 0) {
        std::cerr << "[ERROR] WSAStartup failed with error: " << wsaInitCode << std::endl;
        return 1;
    }
    std::cout << "[DEBUG] WSAStartup successful." << std::endl;

    SOCKET listeningSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listeningSocket == INVALID_SOCKET) {
        std::cerr << "[ERROR] Failed to create socket. Error: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }
    std::cout << "[DEBUG] Socket created successfully." << std::endl;

    sockaddr_in bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(SERVER_LISTEN_PORT);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listeningSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Bind failed with error: " << WSAGetLastError() << std::endl;
        closesocket(listeningSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "[DEBUG] Bind successful on port " << SERVER_LISTEN_PORT << "." << std::endl;

    if (listen(listeningSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Listen failed with error: " << WSAGetLastError() << std::endl;
        closesocket(listeningSocket);
        WSACleanup();
        return 1;
    }
    std::cout << "[DEBUG] Server is listening on port " << SERVER_LISTEN_PORT << "..." << std::endl;

    std::filesystem::create_directories("uploads");

    while (true) {
        std::cout << "[DEBUG] Waiting for client connection..." << std::endl;
        SOCKET acceptedClient = accept(listeningSocket, nullptr, nullptr);
        if (acceptedClient == INVALID_SOCKET) {
            std::cerr << "[ERROR] Accept failed with error: " << WSAGetLastError() << std::endl;
            continue;
        }
        std::cout << "[DEBUG] Client connected." << std::endl;

        // Key change: start a thread for each connection to achieve concurrency
        std::thread th(ProcessConnection, acceptedClient);
        th.detach();
    }

    closesocket(listeningSocket);
    WSACleanup();
    return 0;
}
