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
// recv_thread — handles all incoming messages from server
// FIX: server now excludes sender from broadcast, so we no longer
//      receive our own messages back — double-print is gone
// -----------------------------------------------------------------------

void recv_thread(SOCKET s, const string& name) {
    string sender, msg;
    while (!exit_flag) {
        if (!recv_frame(s, sender)) break;
        if (!recv_frame(s, msg))   break;

        // File incoming
        if (msg.rfind("#sendfile ", 0) == 0) {
            cout << "\n";
            recv_file(s, sender, msg.substr(10));
            cout << " " << flush;
            continue;
        }

        // Server notification
        if (sender == "SERVER") {
            cout << "\n## SERVER ## " << msg << "\n\n";
        } else {
            cout << "\n[ " << sender << " ]: " << msg << "\n\n";
        }

        cout << " " << flush;
    }
    exit_flag = true;
}


// -----------------------------------------------------------------------
// send_thread — handles user input and sends to server
// -----------------------------------------------------------------------

void send_thread(SOCKET s) {
    string line;
    while (!exit_flag) {
        cout << " " << flush;
        getline(cin, line);

        // Clear the typed line from terminal
        cout << "\x1b[A" << "\x1b[2K";

        if (exit_flag) break;
        if (line.empty()) continue;

        // File send
        if (line.rfind("#sendfile ", 0) == 0) {
            string filename = line.substr(10);
            if (!fs::exists(filename)) {
                cout << "## File not found: " << filename << "\n\n";
                continue;
            }
            cout << "[ YOU ]: Sending file: " << fs::path(filename).filename().string() << "\n\n";
            send_file(s, filename);
            continue;
        }

        // Exit
        if (line == "#exit") {
            send_frame(s, "#exit");
            exit_flag = true;
            break;
        }

        // Normal message — print locally then send
        cout << "[ YOU ]: " << line << "\n\n";
        send_frame(s, line);
    }
}


// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main() {
    if (!Initialize()) {
        cout << "Winsock init failed.\n";
        return 1;
    }

    // NEW: auto-create receivedfiles/ folder on client side
    fs::create_directories("receivedfiles");

    cout << "\n===============================================\n";
    cout << "         LAN Chat Client\n";
    cout << "===============================================\n\n";

    // NEW: ask for server IP at runtime instead of hardcoding it
    string server_ip;
    cout << "Enter server IP (ask whoever is running the server): ";
    getline(cin, server_ip);
    if (server_ip.empty()) {
        cout << "No IP entered. Exiting.\n";
        WSACleanup();
        return 1;
    }

    // NEW: optional port override
    int port = DEFAULT_PORT;
    cout << "Enter port (press ENTER for default " << DEFAULT_PORT << "): ";
    string port_input;
    getline(cin, port_input);
    if (!port_input.empty()) {
        try { port = stoi(port_input); }
        catch (...) {
            cout << "Invalid port, using default.\n";
            port = DEFAULT_PORT;
        }
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        cout << "Socket creation failed!\n";
        WSACleanup();
        return 1;
    }

    // NEW: connection timeout — fail in ~5 seconds instead of hanging for 20+
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
        cout << "Could not connect to server!\n";
        cout << "  - Make sure the server is running.\n";
        cout << "  - Double-check the IP address.\n";
        cout << "  - Check that port " << port << " is not blocked by firewall.\n";
        closesocket(s);
        WSACleanup();
        return 1;
    }

    // Remove the receive timeout after connecting — normal chat has no timeout
    DWORD no_timeout = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&no_timeout, sizeof(no_timeout));

    cout << "Connected!\n\n";

    // NEW: username negotiation with duplicate rejection
    string name;
    while (true) {
        cout << "Enter your name: ";
        getline(cin, name);
        if (name.empty()) name = "Anonymous";

        send_frame(s, name);

        // Wait for server response
        string resp_sender, resp_msg;
        if (!recv_frame(s, resp_sender) || !recv_frame(s, resp_msg)) {
            cout << "Lost connection during name setup.\n";
            closesocket(s);
            WSACleanup();
            return 1;
        }

        if (resp_msg == "#nameok") {
            cout << "Name accepted!\n";
            break;
        } else if (resp_msg == "#nametaken") {
            cout << "That name is already taken. Please choose another.\n";
        } else {
            // Unexpected message — just proceed
            break;
        }
    }

    cout << "\n===============================================\n";
    cout << " Welcome, " << name << "!\n";
    cout << "===============================================\n\n";

    cout << "Commands:\n";
    cout << "  <message>                  -> Send a message to everyone\n";
    cout << "  #sendfile <path>           -> Send a file\n";
    cout << "  #list                      -> See who is online\n";
    cout << "  #exit                      -> Leave the chat\n";
    cout << "--------------------------------------------------\n\n";

    thread t_recv(recv_thread, s, name);
    thread t_send(send_thread, s);

    t_send.join();
    closesocket(s);
    exit_flag = true;
    if (t_recv.joinable()) t_recv.join();

    WSACleanup();
    cout << "\nSocket closed. Goodbye!\n";
    return 0;
}