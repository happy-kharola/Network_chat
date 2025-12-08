// server.cpp
// simple multi client chat server (supports file sending)
#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <tchar.h>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include "communication.hpp"

#pragma comment(lib, "ws2_32.lib")
using namespace std;

#define PORT 12345
string last_file_path = "";
string last_file_name = "";
string last_file_sender = "";



struct Client {
    int id;
    string name;
    SOCKET sock;
    thread th;
};

vector<Client> clients;
mutex mtx_cout, mtx_clients;
int next_id = 1;

bool Initialize() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2,2), &data) == 0;
}

// safe console printing
void print_safe(string msg ) {
    lock_guard<mutex> lock(mtx_cout);
    cout << msg<<endl;
    
}

// broadcast msg to all clients except sender
void broadcast(string sender, string msg, SOCKET exclude = INVALID_SOCKET) {
    lock_guard<mutex> lock(mtx_clients);
    for (auto &c : clients) {
        if (c.sock == exclude) continue;
        send_frame(c.sock, sender);
        send_frame(c.sock, msg);
    }
}




// remove client from list
void remove_client(SOCKET s) {
    lock_guard<mutex> lock(mtx_clients);
    auto it = find_if(clients.begin(), clients.end(), [&](Client &c){ return c.sock == s; });
    if (it != clients.end()) {
        closesocket(it->sock);
        if (it->th.joinable()) it->th.detach();
        clients.erase(it);
    }
}

// for #kick
void kick_user(const string& name) {
    lock_guard<mutex> lock(mtx_clients);
    for (auto& c : clients) {
        if (c.name == name) {
            send_frame(c.sock, "SERVER");
            send_frame(c.sock, "You have been kicked by the server.");
            closesocket(c.sock);
            print_safe("Kicked: " + name);
            broadcast("SERVER", name + " was kicked by the server.");
            return;
        }
    }
    print_safe("No client named '" + name + "' found.");
}




// handle one client
void client_handler(SOCKET s, int id) {
    string name;
    if (!recv_frame(s, name)) {
        remove_client(s);
        return;
    }

    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto &c : clients)
            if (c.sock == s) c.name = name;
    }

    print_safe(name + " joined the chat!");
    broadcast("SERVER", name + " joined the chat.");

    while (true) {
        string msg;
        if (!recv_frame(s, msg)) break;

        if (msg == "#exit") {
            broadcast("SERVER", name + " left the chat.");
            print_safe(name + " disconnected.");
            remove_client(s);
            return;
        }

        // check if message is file
        if (msg.rfind("#sendfile ", 0) == 0) {
            string header = msg.substr(10);


            
            print_safe("\n--------------------------------------------------");
            print_safe(" FILE SENT FROM : " + name);
            print_safe(" FILE NAME      : " + header.substr(0, header.find('|')));
            print_safe("--------------------------------------------------");
            print_safe(""); // ONE blank line

            recv_file(s, name, header);
            string clean = header.substr(0, header.find('|'));

          
            size_t slash = clean.find_last_of("/\\");
            if (slash != string::npos) clean = clean.substr(slash + 1);

            last_file_sender = name;
            last_file_name = clean;
            last_file_path = "receivedfiles/received_" + clean;


            broadcast("SERVER", name + " sent a file: " + header.substr(0, header.find('|')));

        
            string dummy;
            recv_frame(s, dummy);
            continue;

        }

        print_safe(name + ": " + msg);
        broadcast(name, msg, s);
    }

    remove_client(s);
}



void forward_last_file() {
    if (last_file_path.empty()) {
        print_safe("No file has been received yet.");
        return;
    }

    print_safe("Forwarding last file: " + last_file_name);
    
    // unamed scope to prevent mtx_clients mutex unlocked after the scope
    // for broadcast() to lock again
    {
        lock_guard<mutex> lock(mtx_clients);

        for (auto& c : clients) {
            if (c.name == last_file_sender) continue;

            // Add this — the client expects 2 frames first!
            send_frame(c.sock, "SERVER");
        
            // Then actually send the file data (adjust send_file to skip sending its own header)
            send_file(c.sock, last_file_path);
        

        }
    }
    
    broadcast("SERVER","Server forwarded the last received file.");
}



int main() {
    if (!Initialize()) {
        cout << "Failed to initialize Winsock!\n";
        return 1;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        cout << "Socket creation failed!\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    InetPton(AF_INET, _T("0.0.0.0"), &addr.sin_addr);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cout << "Bind failed!\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        cout << "Listen failed!\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    print_safe("===============================================");
    print_safe("        Server started on port " + to_string(PORT));
    print_safe("===============================================");
        // show server commands
    print_safe("Server Commands:");
    print_safe("  Type any message to broadcast to all clients.");
    print_safe("  #kick <name>   -> Kick a client");
    print_safe("  #forward       -> Forward last received file to all clients");
    print_safe("--------------------------------------------------");

    // thread to read server input
    thread server_input([](){
        string msg;
        while (true) {
            getline(cin, msg);
            if (msg.empty()) continue;

            // handle kick
            if (msg.rfind("#kick ", 0) == 0) {
                string name = msg.substr(6);
                kick_user(name);
                continue;
            }

            // handle file forward
            if (msg == "#forward") {
                forward_last_file();
                continue;
            }

            // normal server chat
            broadcast("SERVER", msg);
            print_safe("SERVER (you): " + msg);
        }
    });
    server_input.detach();


    while (true) {
        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) continue;

        int id = next_id++;
        {
            lock_guard<mutex> lock(mtx_clients);
            clients.push_back({id, "Anonymous", clientSock, thread()});
            clients.back().th = thread(client_handler, clientSock, id);
            clients.back().th.detach();
        }

        print_safe("Client connected (id = " + to_string(id) + ")");
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}
