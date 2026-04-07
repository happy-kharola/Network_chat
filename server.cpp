// server.cpp
// Multi-client LAN chat server with file transfer support
#include <iostream>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <tchar.h>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include "communication.hpp"

#pragma comment(lib, "ws2_32.lib")
using namespace std;
namespace fs = std::filesystem;

#define DEFAULT_PORT 12345


// -----------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------

int SERVER_PORT = DEFAULT_PORT;

bool Initialize() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}


// -----------------------------------------------------------------------
// LastFile  — thread-safe storage for the most recently received file
// -----------------------------------------------------------------------

struct LastFile {
    mutex mtx;
    string path, name, sender;

    void set(const string& p, const string& n, const string& s) {
        lock_guard<mutex> lock(mtx);
        path = p; name = n; sender = s;
    }

    bool get(string& p, string& n, string& s) {
        lock_guard<mutex> lock(mtx);
        if (path.empty()) return false;
        p = path; n = name; s = sender;
        return true;
    }
} last_file;


// -----------------------------------------------------------------------
// Client list
// -----------------------------------------------------------------------

struct Client {
    int    id;
    string name;
    SOCKET sock;
    thread th;
};

vector<Client> clients;
mutex mtx_cout, mtx_clients;
int next_id = 1;


// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

void print_safe(const string& msg) {
    lock_guard<mutex> lock(mtx_cout);
    cout << msg << endl;
}

// NEW: get local LAN IP to display at startup so classmates know what to type
string get_local_ip() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == SOCKET_ERROR) return "unknown";

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(hostname, nullptr, &hints, &res) != 0) return "unknown";

    string ip = "unknown";
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET_ADDRSTRLEN];
        sockaddr_in* addr = (sockaddr_in*)p->ai_addr;
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
            ip = buf;
            // Prefer a 192.168.x.x or 10.x.x.x address (typical LAN)
            if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0) break;
        }
    }
    freeaddrinfo(res);
    return ip;
}

// broadcast a message to all clients except the excluded socket
void broadcast(const string& sender, const string& msg, SOCKET exclude = INVALID_SOCKET) {
    lock_guard<mutex> lock(mtx_clients);
    for (auto& c : clients) {
        if (c.sock == exclude) continue;
        send_frame(c.sock, sender);
        send_frame(c.sock, msg);
    }
}

// NEW: check if a username is already taken (call with mtx_clients held)
bool name_taken(const string& name) {
    for (auto& c : clients)
        if (c.name == name) return true;
    return false;
}

// remove client from the list and close its socket
void remove_client(SOCKET s) {
    lock_guard<mutex> lock(mtx_clients);
    auto it = find_if(clients.begin(), clients.end(), [&](Client& c) { return c.sock == s; });
    if (it != clients.end()) {
        shutdown(s, SD_SEND);
        closesocket(it->sock);
        if (it->th.joinable()) it->th.detach();
        clients.erase(it);
    }
}

// FIX: kick_user — collect socket first, release lock, then act
// avoids deadlock with remove_client and broadcast both trying to lock mtx_clients
void kick_user(const string& name) {
    SOCKET target = INVALID_SOCKET;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c.name == name) { target = c.sock; break; }
    }

    if (target == INVALID_SOCKET) {
        print_safe("No client named '" + name + "' found.");
        return;
    }

    send_frame(target, "SERVER");
    send_frame(target, "You have been kicked by the server.");

    remove_client(target);
    broadcast("SERVER", name + " was kicked by the server.");
    print_safe("Kicked: " + name);
}


// -----------------------------------------------------------------------
// #list — send current online users to one client
// -----------------------------------------------------------------------

void send_user_list(SOCKET s) {
    string list = "Online users: ";
    {
        lock_guard<mutex> lock(mtx_clients);
        for (size_t i = 0; i < clients.size(); i++) {
            list += clients[i].name;
            if (i + 1 < clients.size()) list += ", ";
        }
    }
    send_frame(s, "SERVER");
    send_frame(s, list);
}


// -----------------------------------------------------------------------
// #forward — resend last received file to all other clients
// FIX: collect sockets BEFORE locking for file send, so we don't hold
//      mtx_clients across a potentially large file transfer
// FIX: correct frame order — client expects sender frame then #sendfile frame
// -----------------------------------------------------------------------

void forward_last_file() {
    string path, name, sender;
    if (!last_file.get(path, name, sender)) {
        print_safe("No file has been received yet.");
        return;
    }

    print_safe("Forwarding last file: " + name);

    // Collect target sockets without holding the lock during the actual send
    vector<SOCKET> targets;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c.name != sender) targets.push_back(c.sock);
    }

    // FIX: send proper framing — sender frame, then #sendfile header, then chunks
    // This matches exactly what recv_thread on the client expects
    for (SOCKET sock : targets) {
        send_frame(sock, "SERVER");                                         // frame 1: sender name
        send_file(sock, path);                                              // frame 2+: #sendfile header + chunks
    }

    broadcast("SERVER", "Server forwarded the last received file: " + name);
}


// -----------------------------------------------------------------------
// client_handler — runs in its own thread per connected client
// FIX: duplicate username rejection with a negotiation loop
// -----------------------------------------------------------------------

void client_handler(SOCKET s, int id) {
    string name;

    // NEW: reject duplicate usernames — loop until client sends a unique name
    while (true) {
        if (!recv_frame(s, name)) { remove_client(s); return; }

        bool taken = false;
        {
            lock_guard<mutex> lock(mtx_clients);
            taken = name_taken(name);
        }

        if (taken) {
            // Tell client to pick another name
            send_frame(s, "SERVER");
            send_frame(s, "#nametaken");
        } else {
            // Confirm the name is accepted
            send_frame(s, "SERVER");
            send_frame(s, "#nameok");
            break;
        }
    }

    // Register the accepted name
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c.sock == s) c.name = name;
    }

    print_safe(name + " joined the chat!");
    broadcast("SERVER", name + " joined the chat.", s);

    while (true) {
        string msg;
        if (!recv_frame(s, msg)) break;

        // --- exit ---
        if (msg == "#exit") {
            broadcast("SERVER", name + " left the chat.");
            print_safe(name + " disconnected.");
            remove_client(s);
            return;
        }

        // --- list ---
        if (msg == "#list") {
            send_user_list(s);
            continue;
        }

        // --- file transfer ---
        if (msg.rfind("#sendfile ", 0) == 0) {
            string header = msg.substr(10);
            string clean  = header.substr(0, header.find('|'));

            // Strip path separators from display name
            size_t slash = clean.find_last_of("/\\");
            if (slash != string::npos) clean = clean.substr(slash + 1);

            print_safe("\n--------------------------------------------------");
            print_safe(" FILE FROM : " + name);
            print_safe(" FILE NAME : " + clean);
            print_safe("--------------------------------------------------");

            recv_file(s, name, header);

            last_file.set("receivedfiles/received_" + clean, clean, name);
            broadcast("SERVER", name + " sent a file: " + clean, s);
            continue;
        }

        // --- normal message ---
        print_safe(name + ": " + msg);
        broadcast(name, msg, s);  // NOTE: excludes sender — fixes double-print on client
    }

    broadcast("SERVER", name + " left the chat.");
    remove_client(s);
}


// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main() {
    if (!Initialize()) {
        cout << "Failed to initialize Winsock!\n";
        return 1;
    }

    // NEW: auto-create receivedfiles/ folder on server side too
    fs::create_directories("receivedfiles");

    // NEW: allow runtime port override before starting
    cout << "===============================================\n";
    cout << "         LAN Chat Server\n";
    cout << "===============================================\n";
    cout << "Default port: " << DEFAULT_PORT << "\n";
    cout << "Press ENTER to use default, or type a port number: ";
    string port_input;
    getline(cin, port_input);
    if (!port_input.empty()) {
        try { SERVER_PORT = stoi(port_input); }
        catch (...) {
            cout << "Invalid port, using default " << DEFAULT_PORT << "\n";
            SERVER_PORT = DEFAULT_PORT;
        }
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        cout << "Socket creation failed!\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    InetPton(AF_INET, _T("0.0.0.0"), &addr.sin_addr);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cout << "Bind failed! Port " << SERVER_PORT << " may already be in use.\n";
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

    // NEW: display local IP so classmates know what to connect to
    string local_ip = get_local_ip();

    cout << "\n===============================================\n";
    cout << "  Server started!\n";
    cout << "  IP   : " << local_ip << "\n";
    cout << "  Port : " << SERVER_PORT << "\n";
    cout << "  Tell classmates to connect to: " << local_ip << ":" << SERVER_PORT << "\n";
    cout << "===============================================\n\n";

    cout << "Server Commands:\n";
    cout << "  <message>          -> Broadcast a message to all clients\n";
    cout << "  #kick <name>       -> Kick a client by name\n";
    cout << "  #forward           -> Forward last received file to all clients\n";
    cout << "  #list              -> Show all connected users\n";
    cout << "--------------------------------------------------\n\n";

    // Thread to read server console input
    thread server_input([]() {
        string msg;
        while (true) {
            getline(cin, msg);
            if (msg.empty()) continue;

            if (msg.rfind("#kick ", 0) == 0) {
                kick_user(msg.substr(6));
                continue;
            }

            if (msg == "#forward") {
                forward_last_file();
                continue;
            }

            if (msg == "#list") {
                // Print to server console
                lock_guard<mutex> lock(mtx_clients);
                cout << "Online (" << clients.size() << "): ";
                for (size_t i = 0; i < clients.size(); i++) {
                    cout << clients[i].name;
                    if (i + 1 < clients.size()) cout << ", ";
                }
                cout << endl;
                continue;
            }

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

        print_safe("Client connected (id=" + to_string(id) + ")");
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}