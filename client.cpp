// client.cpp
// simple chat client for the server (supports sending files)
#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <thread>
#include <atomic>
#include <string>
#include "communication.hpp"

#pragma comment(lib, "ws2_32.lib")
using namespace std;

#define PORT 12345
#define SERVER_IP "10.62.45.117"

atomic<bool> exit_flag(false);

bool Initialize() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2,2), &data) == 0;
}

void recv_thread(SOCKET s, string name) {
    string sender, msg;
    while (!exit_flag) {
        if (!recv_frame(s, sender)) break;
        if (!recv_frame(s, msg)) break;
        if (msg.rfind("#sendfile ", 0) == 0) {
            recv_file(s, sender, msg.substr(10));
            continue;  
        }

        else if (sender == "SERVER") cout << "\n## SERVER ## " << msg << endl<<endl;
        else if (sender == name) cout << "\nYOU: " << msg << endl;
        else cout <<"[ "<<sender<<" ]" << ": " << msg<<endl<<endl;

        cout << " " << flush;
    }
    exit_flag = true;
}

void send_thread(SOCKET s) {
    string line;
    while (!exit_flag) {
        cout << " " << flush;
        getline(cin, line);
        cout << "\x1b[A" << "\x1b[2K";
        cout<<" [ YOU ]: "<<line<<endl<<endl;

        if (line.rfind("#sendfile ", 0) == 0) {
            string filename = line.substr(10);
            send_file(s, filename);
            continue;
        }

        send_frame(s, line);
        if (line == "#exit") {
            exit_flag = true;
            break;
        }
    }
}

int main() {
    if (!Initialize()) {
        cout << "Winsock init failed.\n";
        return 1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        cout << "Socket creation failed!\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cout << "Can't connect to server!\n";
        closesocket(s);
        WSACleanup();
        return 1;
    }

    cout << "\n===============================================" << endl;
    cout << "         Welcome to the Chat Room!" << endl;
    cout << "===============================================" << endl;
    cout << "Type messages normally to chat.\n";
    cout << "Use '#sendfile <filename>' to send files.\n";
    cout << "Type '#exit' to leave.\n" << endl;

    cout << "Enter your name: ";
    string name;
    getline(cin, name);
    if (name.empty()) name = "Anonymous";

    send_frame(s, name);

    thread t_recv(recv_thread, s, name);
    thread t_send(send_thread, s);

    t_send.join();
    closesocket(s);
    exit_flag = true;
    if (t_recv.joinable()) t_recv.join();

    WSACleanup();
    cout << "\nDisconnected. Goodbye!\n";
    return 0;
}
