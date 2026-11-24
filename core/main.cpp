// Native Windows camera capture and minimal localhost WebSocket emitter.
// OpenCV is used for frame acquisition and laser detection.
// WebSocket is intentionally implemented with WinSock to avoid a second runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <conio.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
std::mutex socketMutex;
SOCKET clientSocket = INVALID_SOCKET;
std::atomic<bool> running{true};
HWND previewWindow = nullptr;

LRESULT CALLBACK previewWindowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        running = false;
        previewWindow = nullptr;
        return 0;
    }
    return DefWindowProcA(window, message, wordParameter, longParameter);
}

HWND createPreviewWindow() {
    const char* className = "LaserCoreNativePreview";
    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = previewWindowProcedure;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    HWND window = CreateWindowExA(0, className,
        "Laser Core - Camera / Detection mask (Q to exit)", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1150, 550,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window) {
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
    }
    return window;
}

void drawPreview(const cv::Mat& cameraFrame, const cv::Mat& detectionMask,
                 bool foundLaser, int detectionX, int detectionY) {
    if (!previewWindow || cameraFrame.empty() || detectionMask.empty()) return;
    cv::Mat cameraDisplay;
    cv::Mat maskDisplay;
    cameraFrame.copyTo(cameraDisplay);
    cv::cvtColor(detectionMask, maskDisplay, cv::COLOR_GRAY2BGR);
    if (foundLaser) {
        const cv::Point center(detectionX, detectionY);
        cv::circle(cameraDisplay, center, 13, cv::Scalar(0, 255, 0), 2);
        cv::circle(maskDisplay, center, 13, cv::Scalar(0, 255, 0), 2);
    }
    cv::Mat cameraPixels;
    cv::Mat maskPixels;
    cv::cvtColor(cameraDisplay, cameraPixels, cv::COLOR_BGR2BGRA);
    cv::cvtColor(maskDisplay, maskPixels, cv::COLOR_BGR2BGRA);

    RECT clientRect = {};
    GetClientRect(previewWindow, &clientRect);
    const int totalWidth = clientRect.right - clientRect.left;
    const int totalHeight = clientRect.bottom - clientRect.top;
    if (totalWidth <= 40 || totalHeight <= 40) return;
    const int panelWidth = totalWidth / 2;
    const int panelHeight = totalHeight - 32;
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = cameraPixels.cols;
    bitmapInfo.bmiHeader.biHeight = -cameraPixels.rows; // OpenCV top-down order
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    HDC deviceContext = GetDC(previewWindow);
    if (!deviceContext) return;
    SetStretchBltMode(deviceContext, HALFTONE);
    StretchDIBits(deviceContext, 0, 30, panelWidth, panelHeight,
        0, 0, cameraPixels.cols, cameraPixels.rows,
        cameraPixels.data, &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
    StretchDIBits(deviceContext, panelWidth, 30, totalWidth - panelWidth, panelHeight,
        0, 0, maskPixels.cols, maskPixels.rows,
        maskPixels.data, &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
    SetBkMode(deviceContext, OPAQUE);
    SetBkColor(deviceContext, RGB(20, 30, 45));
    SetTextColor(deviceContext, RGB(240, 250, 255));
    const char* cameraLabel = "CAMERA (green ring = accepted onset)";
    TextOutA(deviceContext, 10, 8, cameraLabel, static_cast<int>(std::strlen(cameraLabel)));
    const char* maskLabel = "MASK (white = brightness rise)";
    TextOutA(deviceContext, panelWidth + 10, 8, maskLabel, static_cast<int>(std::strlen(maskLabel)));
    ReleaseDC(previewWindow, deviceContext);
}


bool sendAll(SOCKET socket, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const int written = send(socket, data.data() + offset,
                                 static_cast<int>(data.size() - offset), 0);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string websocketAcceptKey(const std::string& key) {
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[20] = {};
    DWORD digestLength = sizeof(digest);
    std::string result;
    if (CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) &&
            CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()),
                          static_cast<DWORD>(input.size()), 0) &&
            CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLength, 0)) {
            DWORD encodedSize = 0;
            const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
            if (CryptBinaryToStringA(digest, digestLength, flags, nullptr, &encodedSize)) {
                std::vector<char> buffer(encodedSize);
                if (CryptBinaryToStringA(digest, digestLength, flags, buffer.data(), &encodedSize))
                    result.assign(buffer.data());
            }
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return result;
}

void closeCurrentClient() {
    std::lock_guard<std::mutex> lock(socketMutex);
    if (clientSocket != INVALID_SOCKET) {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
}

void websocketServer() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return;

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(8765);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR) {
        std::cerr << "Cannot listen on 127.0.0.1:8765\n";
        closesocket(listener);
        return;
    }
    std::cout << "WebSocket: ws://127.0.0.1:8765\n";

    while (running) {
        SOCKET incoming = accept(listener, nullptr, nullptr);
        if (incoming == INVALID_SOCKET) break;

        DWORD timeoutMilliseconds = 2500;
        setsockopt(incoming, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMilliseconds),
                   sizeof(timeoutMilliseconds));

        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const int bytesReceived = recv(incoming, buffer, sizeof(buffer), 0);
            if (bytesReceived <= 0) break;
            request.append(buffer, bytesReceived);
        }
        const std::string header = "Sec-WebSocket-Key:";
        const auto headerPosition = request.find(header);
        if (request.find("GET ") != 0 || headerPosition == std::string::npos) {
            closesocket(incoming);
            continue;
        }
        const auto keyStart = request.find_first_not_of(" \t", headerPosition + header.size());
        const auto keyEnd = request.find("\r\n", keyStart);
        if (keyStart == std::string::npos || keyEnd == std::string::npos) {
            closesocket(incoming);
            continue;
        }
        const std::string acceptKey = websocketAcceptKey(request.substr(keyStart, keyEnd - keyStart));
        if (acceptKey.empty()) {
            closesocket(incoming);
            continue;
        }
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        if (!sendAll(incoming, response)) {
            closesocket(incoming);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
            clientSocket = incoming;
        }
        std::cout << "Game connected\n";
    }
    closesocket(listener);
}

void sendShot(double normalizedX, double normalizedY, long long timestampMilliseconds) {
    std::ostringstream message;
    message.precision(6);
    message << "{\"type\":\"shot\",\"x\":" << normalizedX
            << ",\"y\":" << normalizedY
            << ",\"timestamp\":" << timestampMilliseconds << "}";
    const std::string payload = message.str();

    // The small shot messages fit the basic (unfragmented) WebSocket text frame.
    if (payload.size() >= 126) return;
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(payload.size()));
    frame += payload;

    std::lock_guard<std::mutex> lock(socketMutex);
    if (clientSocket != INVALID_SOCKET && !sendAll(clientSocket, frame)) {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
}

int argumentValue(int argc, char** argv, int index, int defaultValue) {
    if (argc <= index) return defaultValue;
    try { return std::stoi(argv[index]); }
    catch (...) { return defaultValue; }
}
}

int main(int argc, char** argv) {
    // laser-core.exe [camera-index] [capture-width] [capture-height]
    //                [requested-fps] [brightness] [red-difference]
    const int cameraIndex = argumentValue(argc, argv, 1, 0);
    const int captureWidth = argumentValue(argc, argv, 2, 640);
    const int captureHeight = argumentValue(argc, argv, 3, 480);
    const int requestedFps = argumentValue(argc, argv, 4, 30);
    const int minimumBrightness = argumentValue(argc, argv, 5, 190);
    const int minimumRedDifference = argumentValue(argc, argv, 6, 45);
    const int minimumSpotPixels = std::max(1, argumentValue(argc, argv, 7, 4));

    WSADATA winsockData = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
        std::cerr << "Winsock initialization failed\n";
        return 1;
    }

    cv::VideoCapture camera(cameraIndex, cv::CAP_DSHOW);
    if (!camera.isOpened()) {
        std::cerr << "Cannot open USB camera " << cameraIndex << "\n";
        WSACleanup();
        return 1;
    }
    camera.set(cv::CAP_PROP_FRAME_WIDTH, captureWidth);
    camera.set(cv::CAP_PROP_FRAME_HEIGHT, captureHeight);
    camera.set(cv::CAP_PROP_FPS, requestedFps);
    camera.set(cv::CAP_PROP_BUFFERSIZE, 1);
    std::cout << "Camera actual settings: "
              << camera.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
              << camera.get(cv::CAP_PROP_FRAME_HEIGHT) << " at "
              << camera.get(cv::CAP_PROP_FPS) << " fps (driver-reported)\n";
    std::cout << "Detector: minimum red=" << minimumBrightness
              << ", red-difference=" << minimumRedDifference << "\n";

    previewWindow = createPreviewWindow();
    if (!previewWindow) std::cerr << "Camera preview unavailable; detection will continue.\n";

    std::thread serverThread(websocketServer);
    serverThread.detach(); // Process lifetime; accept loop ends when the process exits.

    cv::Mat frame, red, green, blue, otherMaximum, redExcess;
    cv::Mat brightMask, redMask, colorMask, previousRed, redRise, riseMask, onsetMask;
    cv::Mat labels, stats, centroids;
    long long captureCount = 0;
    long long totalFrames = 0;
    long long shotCount = 0;
    auto statisticsStart = std::chrono::steady_clock::now();
    auto lastShotTime = statisticsStart - std::chrono::seconds(1);
    constexpr int redRiseThreshold = 28;

    constexpr int maximumSpotPixels = 160;
    constexpr int minimumIntervalMilliseconds = 220;

    std::cout << "Onset detector: red rise >= " << redRiseThreshold
              << " in ONE captured frame; 220 ms debounce.\n";
    std::cout << "Minimum spot area: " << minimumSpotPixels << " pixels. Camera preview shows raw frame and detection mask.\n";
    std::cout << "Keep camera fixed. Press Q in this console or close the preview window.\n";
    while (running) {
        const auto readStart = std::chrono::steady_clock::now();
        if (!camera.read(frame) || frame.empty()) {
            std::cerr << "Camera read failed\n";
            break;
        }
        const auto capturedAt = std::chrono::steady_clock::now();
        const double readMilliseconds =
            std::chrono::duration<double, std::milli>(capturedAt - readStart).count();
        captureCount++;
        totalFrames++;

        cv::extractChannel(frame, blue, 0);
        cv::extractChannel(frame, green, 1);
        cv::extractChannel(frame, red, 2);
        cv::max(blue, green, otherMaximum);
        cv::subtract(red, otherMaximum, redExcess);
        cv::threshold(red, brightMask, minimumBrightness - 1, 255, cv::THRESH_BINARY);
        cv::threshold(redExcess, redMask, minimumRedDifference - 1, 255, cv::THRESH_BINARY);
        cv::bitwise_and(brightMask, redMask, colorMask);

        bool detectedOnset = false;
        int risingPixelCount = 0;
        int bestArea = 0;
        int strongestRise = 0;
        int detectedX = -1;
        int detectedY = -1;
        if (!previousRed.empty()) {
            // Difference from the preceding CAPTURED frame. A fixed red object
            // cannot remain a candidate merely because of a frozen background.
            cv::subtract(red, previousRed, redRise);
            cv::threshold(redRise, riseMask, redRiseThreshold - 1, 255, cv::THRESH_BINARY);
            cv::bitwise_and(colorMask, riseMask, onsetMask);
            risingPixelCount = cv::countNonZero(onsetMask);

            // Reject global exposure changes as they usually brighten hundreds
            // or thousands of pixels simultaneously.
            const int maximumGlobalChangedPixels = std::max(120, frame.rows * frame.cols / 200);
            if (risingPixelCount > 0 && risingPixelCount <= maximumGlobalChangedPixels) {
                const int regions = cv::connectedComponentsWithStats(
                    onsetMask, labels, stats, centroids, 8);
                int bestRegion = -1;
                for (int region = 1; region < regions; region++) {
                    const int area = stats.at<int>(region, cv::CC_STAT_AREA);
                    if (area >= minimumSpotPixels && area <= maximumSpotPixels && area > bestArea) {
                        bestRegion = region;
                        bestArea = area;
                    }
                }
                if (bestRegion >= 0) {
                    detectedOnset = true;
                    const auto millisecondsSinceShot =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            capturedAt - lastShotTime).count();
                    const int regionX = static_cast<int>(centroids.at<double>(bestRegion, 0));
                    const int regionY = static_cast<int>(centroids.at<double>(bestRegion, 1));
                    strongestRise = static_cast<int>(redRise.at<unsigned char>(regionY, regionX));
                    detectedX = regionX;
                    detectedY = regionY;
                    if (millisecondsSinceShot >= minimumIntervalMilliseconds) {
                        const double pointX =
                            (centroids.at<double>(bestRegion, 0) + 0.5) / frame.cols;
                        const double pointY =
                            (centroids.at<double>(bestRegion, 1) + 0.5) / frame.rows;
                        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            capturedAt.time_since_epoch()).count();
                        sendShot(pointX, pointY, timestamp);
                        shotCount++;
                        lastShotTime = capturedAt;
                        std::cout << "Shot " << shotCount
                                  << " frame=" << totalFrames
                                  << " at (" << pointX << ", " << pointY << ")"
                                  << " area=" << bestArea
                                  << " rise=" << strongestRise
                                  << " read=" << readMilliseconds << "ms\n";
                    }
                }
            }
        }
        // Show the raw camera frame and EXACT onset mask used by the detector.
        // Throttle preview to avoid wasting time on rendering every frame.
        if ((totalFrames % 3) == 0 && !onsetMask.empty()) {
            drawPreview(frame, onsetMask, detectedOnset, detectedX, detectedY);
        }
        MSG windowMessage = {};
        while (PeekMessageA(&windowMessage, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&windowMessage);
            DispatchMessageA(&windowMessage);
        }
        if (!running) break;

        // ALWAYS update the reference frame, including during an extended
        // illumination. Thus holding a laser steady will not fire repeatedly.
        red.copyTo(previousRed);

        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - statisticsStart).count();
        if (elapsed >= 3.0) {
            std::cout << "Captured FPS: " << (captureCount / elapsed)
                      << " | onset: " << (detectedOnset ? "YES" : "no")
                      << " | rising pixels: " << risingPixelCount
                      << " | read: " << readMilliseconds << "ms"
                      << " | shots: " << shotCount << "\n";
            captureCount = 0;
            statisticsStart = std::chrono::steady_clock::now();
        }

        if (_kbhit()) {
            const int pressedKey = _getch();
            if (pressedKey == 'q' || pressedKey == 'Q') break;
        }
    }
    running = false;
    if (previewWindow) DestroyWindow(previewWindow);
    closeCurrentClient();
    camera.release();
    WSACleanup();
    return 0;
}
