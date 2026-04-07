// client.cpp
// LAN chat client with file transfer support
#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <thread>
#include <atomic>
#include <string>
#include <filesystem>
#include "communication.hpp"

#pragma comment(lib, "ws2_32.lib")
using namespace std;
namespace fs = std::filesystem;

#define DEFAULT_PORT 12345

atomic<bool> exit_flag(false);

bool Initialize() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}


// -----------------------------------------------------------------------
// recv_thread — handles all incoming frames from the server
// Server excludes the sender from broadcast so own messages never echo back
// -----------------------------------------------------------------------

void recv_thread(SOCKET s, const string& name) {
    string sender, msg;
    while (!exit_flag) {
        if (!recv_frame(s, sender)) break;
        if (!recv_frame(s, msg))   break;

        if (msg.rfind("#sendfile ", 0) == 0) {
            cout << "\n";
            recv_file(s, sender, msg.substr(10));
            cout << " " << flush;
            continue;
        }

        if (sender == "SERVER") {
            cout << "\n[SERVER] " << msg << "\n\n";
        } else {
            cout << "\n[ " << sender << " ]: " << msg << "\n\n";
        }

        cout << " " << flush;
    }
    exit_flag = true;
}


// -----------------------------------------------------------------------
// send_thread — reads user input and sends to the server
// -----------------------------------------------------------------------

void send_thread(SOCKET s) {
    string line;
    while (!exit_flag) {
        cout << " " << flush;
        getline(cin, line);
        cout << "\x1b[A" << "\x1b[2K";

        if (exit_flag) break;
        if (line.empty()) continue;

        if (line.rfind("#sendfile ", 0) == 0) {
            string filename = line.substr(10);
            if (!fs::exists(filename)) {
                cout << "[Error] File not found: " << filename << "\n\n";
                continue;
            }
            cout << "[ YOU ]: Sending file: " << fs::path(filename).filename().string() << "\n\n";
            send_file(s, filename);
            continue;
        }

        if (line == "#exit") {
            send_frame(s, "#exit");
            exit_flag = true;
            break;
        }

        cout << "[ YOU ]: " << line << "\n\n";
        send_frame(s, line);
    }
}


// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main() {
    if (!Initialize()) {
        cout << "Winsock initialization failed.\n";
        return 1;
    }

    fs::create_directories("receivedfiles");

    cout << "\n================================================\n";
    cout << "            LAN Chat Client\n";
    cout << "================================================\n\n";

    cout << "Server IP address: ";
    string server_ip;
    getline(cin, server_ip);
    if (server_ip.empty()) {
        cout << "No address entered. Exiting.\n";
        WSACleanup();
        return 1;
    }

    int port = DEFAULT_PORT;
    cout << "Port (press Enter for default " << DEFAULT_PORT << "): ";
    string port_input;
    getline(cin, port_input);
    if (!port_input.empty()) {
        try { port = stoi(port_input); }
        catch (...) {
            cout << "Invalid input — using default port.\n";
            port = DEFAULT_PORT;
        }
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        cout << "Socket creation failed.\n";
        WSACleanup();
        return 1;
    }

    // 5-second connection timeout — fail fast instead of hanging
    DWORD timeout_ms = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, server_ip.c_str(), &addr.sin_addr) != 1) {
        cout << "Invalid IP address format.\n";
        closesocket(s);
        WSACleanup();
        return 1;
    }

    cout << "\nConnecting to " << server_ip << ":" << port << " ...\n";

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cout << "\nConnection failed.\n";
        cout << "  - Verify the server is running.\n";
        cout << "  - Verify the IP address is correct.\n";
        cout << "  - Ensure both devices are on the same network.\n";
        cout << "  - Check that port " << port << " is not blocked by a firewall.\n";
        closesocket(s);
        WSACleanup();
        return 1;
    }

    // Remove receive timeout after connecting — normal chat has no timeout
    DWORD no_timeout = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&no_timeout, sizeof(no_timeout));

    cout << "Connected.\n\n";

    // Username negotiation — retry if name is already taken
    string name;
    while (true) {
        cout << "Enter username: ";
        getline(cin, name);
        if (name.empty()) name = "Anonymous";

        send_frame(s, name);

        string resp_sender, resp_msg;
        if (!recv_frame(s, resp_sender) || !recv_frame(s, resp_msg)) {
            cout << "Connection lost during setup.\n";
            closesocket(s);
            WSACleanup();
            return 1;
        }

        if (resp_msg == "#nameok") {
            break;
        } else if (resp_msg == "#nametaken") {
            cout << "Username already in use. Please choose another.\n";
        }
    }

    cout << "\n================================================\n";
    cout << "  Logged in as: " << name << "\n";
    cout << "================================================\n\n";

    cout << "Commands\n";
    cout << "  <message>\n";
    cout << "    Send a text message to everyone.\n\n";

    cout << "  #sendfile <path>\n";
    cout << "    Send a file to the server.\n";
    cout << "    e.g.  #sendfile notes.pdf\n";
    cout << "    e.g.  #sendfile C:\\Users\\You\\Desktop\\assignment.zip\n\n";

    cout << "  #list\n";
    cout << "    Show all currently connected users.\n\n";

    cout << "  #exit\n";
    cout << "    Disconnect and close the client.\n\n";

    cout << "  Received files are saved to: receivedfiles\\\n";
    cout << "------------------------------------------------\n\n";

    thread t_recv(recv_thread, s, name);
    thread t_send(send_thread, s);

    t_send.join();
    closesocket(s);
    exit_flag = true;
    if (t_recv.joinable()) t_recv.join();

    WSACleanup();
    cout << "\nDisconnected. Goodbye.\n";
    return 0;
}