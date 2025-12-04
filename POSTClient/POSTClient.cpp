#include <iostream>
#include <string>
#include <fstream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <limits>

#pragma comment(lib, "Ws2_32.lib")

const std::string kLocalServerIP = "127.0.0.1";
const int kLocalServerPort = 8080;

int main() {
    while (true) {
        std::cout << "Enter the path of the file to upload (or empty/quit to exit): ";
        std::string userChosenPath;
        std::getline(std::cin, userChosenPath);

        if (userChosenPath.empty() || userChosenPath == "quit") {
            std::cout << "Exiting...\n";
            break;
        }

        std::ifstream uploadFileStream(userChosenPath, std::ios::binary | std::ios::ate);
        if (!uploadFileStream.is_open()) {
            std::cerr << "Failed to open file." << std::endl;
            continue;
        }

        std::streamsize fileTotalBytes = uploadFileStream.tellg();
        uploadFileStream.seekg(0, std::ios::beg);

        std::cout << "[DEBUG] File size to upload: " << fileTotalBytes << " bytes." << std::endl;

        std::string extractedName = userChosenPath;
        {
            size_t pos = extractedName.find_last_of("\\/");
            if (pos != std::string::npos) extractedName = extractedName.substr(pos + 1);
        }

        WSADATA socketInitInfo;
        if (WSAStartup(MAKEWORD(2, 2), &socketInitInfo) != 0) {
            std::cerr << "WSAStartup failed." << std::endl;
            uploadFileStream.close();
            continue;
        }

        SOCKET outboundSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (outboundSocket == INVALID_SOCKET) {
            std::cerr << "Failed to create socket." << std::endl;
            WSACleanup();
            uploadFileStream.close();
            continue;
        }

        sockaddr_in targetServerInfo;
        std::memset(&targetServerInfo, 0, sizeof(targetServerInfo));
        targetServerInfo.sin_family = AF_INET;
        targetServerInfo.sin_port = htons(kLocalServerPort);
        inet_pton(AF_INET, kLocalServerIP.c_str(), &targetServerInfo.sin_addr);

        if (connect(outboundSocket, (sockaddr*)&targetServerInfo, sizeof(targetServerInfo)) == SOCKET_ERROR) {
            std::cerr << "Connect failed." << std::endl;
            closesocket(outboundSocket);
            WSACleanup();
            uploadFileStream.close();
            continue;
        }

        std::string assembledHeader =
            "POST /upload HTTP/1.0\r\n"
            "Host: " + kLocalServerIP + "\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " + std::to_string(fileTotalBytes) + "\r\n"
            "X-Filename: " + extractedName + "\r\n"
            "\r\n";

        int headerTransmitStatus = send(outboundSocket, assembledHeader.c_str(), (int)assembledHeader.size(), 0);
        if (headerTransmitStatus == SOCKET_ERROR) {
            std::cerr << "Failed to send POST header. Error: " << WSAGetLastError() << std::endl;
            closesocket(outboundSocket);
            WSACleanup();
            uploadFileStream.close();
            continue;
        }

        const int kDataChunkSize = 8192;
        char binaryChunkBuffer[kDataChunkSize];
        std::streamsize cumulativeBytesTransferred = 0;
        bool chunkFailure = false;

        while (true) {
            uploadFileStream.read(binaryChunkBuffer, kDataChunkSize);
            std::streamsize bytesReadThisRound = uploadFileStream.gcount();
            if (bytesReadThisRound <= 0) break;

            char* ptrCursor = binaryChunkBuffer;
            std::streamsize bytesLeft = bytesReadThisRound;

            while (bytesLeft > 0) {
                int sentSegment = send(outboundSocket, ptrCursor, (int)bytesLeft, 0);
                if (sentSegment == SOCKET_ERROR) {
                    std::cerr << "\nFailed to send file chunk. Error: " << WSAGetLastError() << std::endl;
                    chunkFailure = true;
                    break;
                }
                ptrCursor += sentSegment;
                bytesLeft -= sentSegment;
                cumulativeBytesTransferred += sentSegment;

                double progress = (double)cumulativeBytesTransferred * 100.0 / (double)fileTotalBytes;
                int totalBarUnits = 50;
                int filledUnits = (int)(progress / 100.0 * totalBarUnits);

                std::cout << "\rUploading [";
                for (int i = 0; i < totalBarUnits; ++i) {
                    if (i < filledUnits) std::cout << "=";
                    else if (i == filledUnits) std::cout << ">";
                    else std::cout << " ";
                }
                std::cout << "] " << (int)progress << "% (" << cumulativeBytesTransferred << "/" << fileTotalBytes << " bytes)" << std::flush;
            }

            if (chunkFailure) break;
        }

        std::cout << std::endl;
        uploadFileStream.close();

        if (chunkFailure) {
            closesocket(outboundSocket);
            WSACleanup();
            continue;
        }

        std::cout << "[DEBUG] File upload finished, total sent: " << cumulativeBytesTransferred << " bytes." << std::endl;

        char inboundBuffer[4096];
        int serverReplySize = recv(outboundSocket, inboundBuffer, sizeof(inboundBuffer) - 1, 0);
        if (serverReplySize <= 0) {
            std::cerr << "No response received or error. WSAGetLastError: " << WSAGetLastError() << std::endl;
            closesocket(outboundSocket);
            WSACleanup();
            continue;
        }

        inboundBuffer[serverReplySize] = '\0';
        std::string rawFullReply(inboundBuffer);

        size_t headerBoundary = rawFullReply.find("\r\n\r\n");
        if (headerBoundary == std::string::npos) {
            std::cerr << "No end of header found." << std::endl;
            closesocket(outboundSocket);
            WSACleanup();
            continue;
        }

        int parsedContentLength = 0;
        {
            size_t clMarker = rawFullReply.find("Content-Length:");
            if (clMarker != std::string::npos) {
                size_t valueStart = clMarker + 15;
                while (valueStart < rawFullReply.size() && (rawFullReply[valueStart] == ' ' || rawFullReply[valueStart] == '\t')) valueStart++;
                size_t valueEnd = rawFullReply.find("\r\n", valueStart);
                if (valueEnd != std::string::npos) {
                    std::string lengthToken = rawFullReply.substr(valueStart, valueEnd - valueStart);
                    try { parsedContentLength = std::stoi(lengthToken); }
                    catch (...) { std::cerr << "Failed to parse Content-Length: " << lengthToken << std::endl; }
                }
            }
        }

        int bodyBytesAlready = (int)rawFullReply.size() - (int)(headerBoundary + 4);
        while (bodyBytesAlready < parsedContentLength) {
            int remainingReq = parsedContentLength - bodyBytesAlready;
            int allowedRead = (remainingReq < (int)sizeof(inboundBuffer) - 1) ? remainingReq : (int)sizeof(inboundBuffer) - 1;
            int actualRead = recv(outboundSocket, inboundBuffer, allowedRead, 0);
            if (actualRead <= 0) {
                std::cerr << "Connection lost or error during body receive. WSAGetLastError: " << WSAGetLastError() << std::endl;
                break;
            }
            inboundBuffer[actualRead] = '\0';
            rawFullReply.append(inboundBuffer, actualRead);
            bodyBytesAlready += actualRead;
        }

        closesocket(outboundSocket);
        WSACleanup();

        std::cout << "Response: " << rawFullReply << std::endl;

        size_t urlLabelPos = rawFullReply.find("\"url\":\"");
        if (urlLabelPos == std::string::npos) {
            std::cerr << "No URL found in response." << std::endl;
            continue;
        }

        urlLabelPos += 7;
        size_t urlEndPos = rawFullReply.find("\"", urlLabelPos);
        if (urlEndPos == std::string::npos) {
            std::cerr << "Invalid URL in response." << std::endl;
            continue;
        }

        std::string extractedURL = rawFullReply.substr(urlLabelPos, urlEndPos - urlLabelPos);
        std::cout << "Opening browser at URL: " << extractedURL << std::endl;

        ShellExecuteA(nullptr, "open", extractedURL.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    return 0;
}