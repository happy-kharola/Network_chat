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
#include <memory>
#include <atomic>
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
// LastFile — thread-safe storage for the most recently received file
// -----------------------------------------------------------------------

struct LastFile {
    mutex  mtx;
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
    mutex  send_mtx;   // guards writes to this client's socket
    atomic<bool> ready{false};   // true only once username negotiation succeeds

};

vector<shared_ptr<Client>> clients;
mutex g_cout_mutex;   // the one definition of the shared cout mutex for this program
mutex mtx_clients;
int next_id = 1;


// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

void print_safe(const string& msg) {
    lock_guard<mutex> lock(g_cout_mutex);
    cout << msg << endl;
}

// Resolves and returns the machine's local LAN IP address
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
            if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0) break;
        }
    }
    freeaddrinfo(res);
    return ip;
}

// Broadcast a message to all connected clients, optionally excluding one socket
// Sends one chat frame (sender + msg) to a client, serialized per-socket.
void send_to_client(const shared_ptr<Client>& c, const string& sender, const string& msg) {
    lock_guard<mutex> lock(c->send_mtx);
    send_frame(c->sock, sender);
    send_frame(c->sock, msg);
}

// Sends "sender" plus a full file transfer to a client, serialized per-socket —
// the lock is held for the whole transfer so nothing else can interleave with it.
void send_file_from(const shared_ptr<Client>& c, const string& sender, const string& filepath) {
    lock_guard<mutex> lock(c->send_mtx);
    send_frame(c->sock, sender);
    send_file(c->sock, filepath);
}

// Broadcast a message to all connected clients, optionally excluding one socket
void broadcast(const string& sender, const string& msg, SOCKET exclude = INVALID_SOCKET) {
    vector<shared_ptr<Client>> targets;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c->sock != exclude && c->ready) targets.push_back(c);
    }
    for (auto& c : targets)
        send_to_client(c, sender, msg);
}

// Looks up a client by socket. Caller must not be holding mtx_clients.
shared_ptr<Client> find_client(SOCKET s) {
    lock_guard<mutex> lock(mtx_clients);
    for (auto& c : clients)
        if (c->sock == s) return c;
    return nullptr;
}


// Returns true if the given username is already registered (call without holding mtx_clients)
bool name_taken(const string& name) {
    for (auto& c : clients)
        if (c->name == name) return true;
    return false;
}

// Cleanly removes a client from the list and closes its socket
void remove_client(SOCKET s) {
    shared_ptr<Client> target;
    {
        lock_guard<mutex> lock(mtx_clients);
        auto it = find_if(clients.begin(), clients.end(),
                           [&](shared_ptr<Client>& c) { return c->sock == s; });
        if (it != clients.end()) {
            target = *it;
            clients.erase(it);
        }
    }

    if (target) {
        lock_guard<mutex> lock(target->send_mtx); // wait out any in-flight send before closing
        shutdown(target->sock, SD_SEND);
        closesocket(target->sock);
        if (target->th.joinable()) target->th.detach();
    }
}

// Disconnects a client by name — collects socket first, releases lock before acting
// to avoid deadlock with remove_client and broadcast
void kick_user(const string& name) {
    shared_ptr<Client> target;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c->name == name) { target = c; break; }
    }

    if (!target) {
        print_safe("[Server] No connected user named '" + name + "'.");
        return;
    }

    send_to_client(target, "SERVER", "You have been disconnected by the server.");
    remove_client(target->sock);
    broadcast("SERVER", name + " was disconnected by the server.");
    print_safe("[Server] Kicked: " + name);
}


// -----------------------------------------------------------------------
// #list — respond to a client's #list request with the online user list
// -----------------------------------------------------------------------

void send_user_list(SOCKET s) {
    string list = "Connected users: ";
    {
        lock_guard<mutex> lock(mtx_clients);
        for (size_t i = 0; i < clients.size(); i++) {
            list += clients[i]->name;
            if (i + 1 < clients.size()) list += ", ";
        }
    }
    auto self = find_client(s);
    if (self) send_to_client(self, "SERVER", list);
}


// -----------------------------------------------------------------------
// #forward — re-send the last received file to all clients except original sender
// Sockets are collected before the send to avoid holding mtx_clients
// during a potentially large file transfer
// -----------------------------------------------------------------------

void forward_last_file() {
    string path, name, sender;
    if (!last_file.get(path, name, sender)) {
        print_safe("[Server] No file has been received yet.");
        return;
    }

    print_safe("[Server] Forwarding: " + name + " (originally from " + sender + ")");

    vector<shared_ptr<Client>> targets;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c->name != sender) targets.push_back(c);
    }

    for (auto& c : targets)
        send_file_from(c, "SERVER", path);

    broadcast("SERVER", "Server forwarded file: " + name);
}


// -----------------------------------------------------------------------
// server_send_file_all — send a file from the server to all connected clients
// -----------------------------------------------------------------------

void server_send_file_all(const string& filepath) {
    if (!fs::exists(filepath)) {
        print_safe("[Server] File not found: " + filepath);
        return;
    }

    string bare = fs::path(filepath).filename().string();
    print_safe("[Server] Sending " + bare + " to all connected clients...");

    vector<shared_ptr<Client>> targets;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            targets.push_back(c);
    }

    if (targets.empty()) {
        print_safe("[Server] No clients connected.");
        return;
    }

    for (auto& c : targets)
        send_file_from(c, "SERVER", filepath);

    broadcast("SERVER", "Server distributed file: " + bare);
    print_safe("[Server] File sent to all clients: " + bare);
}


// -----------------------------------------------------------------------
// server_send_file_one — send a file from the server to a single named client
// -----------------------------------------------------------------------

void server_send_file_one(const string& target_name, const string& filepath) {
    if (!fs::exists(filepath)) {
        print_safe("[Server] File not found: " + filepath);
        return;
    }

    shared_ptr<Client> target;
    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c->name == target_name) { target = c; break; }
    }

    if (!target) {
        print_safe("[Server] No connected user named '" + target_name + "'.");
        return;
    }

    string bare = fs::path(filepath).filename().string();
    print_safe("[Server] Sending " + bare + " to " + target_name + "...");

    send_file_from(target, "SERVER", filepath);

    print_safe("[Server] File sent to " + target_name + ": " + bare);
}


// -----------------------------------------------------------------------
// client_handler — per-client thread
// Handles username negotiation, messaging, and file receive
// -----------------------------------------------------------------------

void client_handler(SOCKET s, int id) {
    string name;

    // Username negotiation — reject duplicates until a unique name is provided
    while (true) {
        if (!recv_frame(s, name)) { remove_client(s); return; }

        bool taken = false;
        {
            lock_guard<mutex> lock(mtx_clients);
            taken = name_taken(name);
        }

        if (taken) {
            send_frame(s, "SERVER");
            send_frame(s, "#nametaken");
        } else {
            send_frame(s, "SERVER");
            send_frame(s, "#nameok");
            break;
        }
    }

    {
        lock_guard<mutex> lock(mtx_clients);
        for (auto& c : clients)
            if (c->sock == s) { c->name = name; c->ready = true; }
    }

    print_safe("[+] " + name + " connected  (id=" + to_string(id) + ")");
    broadcast("SERVER", name + " joined the session.", s);

    while (true) {
        string msg;
        if (!recv_frame(s, msg)) break;

        if (msg == "#exit") {
            broadcast("SERVER", name + " left the session.");
            print_safe("[-] " + name + " disconnected.");
            remove_client(s);
            return;
        }

        if (msg == "#list") {
            send_user_list(s);
            continue;
        }

        if (msg.rfind("#sendfile ", 0) == 0) {
            string header = msg.substr(10);
            string clean  = header.substr(0, header.find('|'));
            size_t slash  = clean.find_last_of("/\\");
            if (slash != string::npos) clean = clean.substr(slash + 1);

            print_safe("\n--------------------------------------------------");
            print_safe(" Incoming file from : " + name);
            print_safe(" File               : " + clean);
            print_safe("--------------------------------------------------");

            recv_file(s, name, header);
            last_file.set("receivedfiles/received_" + clean, clean, name);
            broadcast("SERVER", name + " sent a file: " + clean, s);
            continue;
        }

        print_safe(name + ": " + msg);
        broadcast(name, msg, s);
    }

    broadcast("SERVER", name + " left the session.");
    remove_client(s);
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

    cout << "================================================\n";
    cout << "           LAN Chat Server\n";
    cout << "================================================\n";
    cout << "Default port: " << DEFAULT_PORT << "\n";
    cout << "Press Enter to use default, or enter a port number: ";
    string port_input;
    getline(cin, port_input);
    if (!port_input.empty()) {
        try { SERVER_PORT = stoi(port_input); }
        catch (...) {
            cout << "Invalid input — using default port " << DEFAULT_PORT << "\n";
            SERVER_PORT = DEFAULT_PORT;
        }
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == INVALID_SOCKET) {
        cout << "Socket creation failed.\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(SERVER_PORT);
    InetPton(AF_INET, _T("0.0.0.0"), &addr.sin_addr);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        cout << "Bind failed. Port " << SERVER_PORT << " may already be in use.\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        cout << "Listen failed.\n";
        closesocket(listenSock);
        WSACleanup();
        return 1;
    }

    string local_ip = get_local_ip();

    cout << "\n================================================\n";
    cout << "  Status  : Listening\n";
    cout << "  Address : " << local_ip << ":" << SERVER_PORT << "\n";
    cout << "================================================\n\n";

    cout << "Commands\n";
    cout << "  <message>\n";
    cout << "    Broadcast a text message to all connected clients.\n\n";

    cout << "  #kick <name>\n";
    cout << "    Disconnect a user by name.  e.g.  #kick Ali\n\n";

    cout << "  #list\n";
    cout << "    Display all currently connected users.\n\n";

    cout << "  #forward\n";
    cout << "    Re-send the last received file to all clients.\n\n";

    cout << "  #sendfile <path>\n";
    cout << "    Send a file to ALL connected clients.\n";
    cout << "    e.g.  #sendfile notes.pdf\n";
    cout << "    e.g.  #sendfile C:\\Users\\You\\Desktop\\sheet.xlsx\n\n";

    cout << "  #sendfile <name> <path>\n";
    cout << "    Send a file to ONE specific client by name.\n";
    cout << "    e.g.  #sendfile Ali notes.pdf\n";
    cout << "    e.g.  #sendfile Sara C:\\files\\lecture.pdf\n\n";

    cout << "------------------------------------------------\n\n";

    // Server console input thread
    thread server_input([]() {
        string msg;
        while (true) {
            getline(cin, msg);
            if (msg.empty()) continue;

            // #kick <name>
            if (msg.rfind("#kick ", 0) == 0) {
                kick_user(msg.substr(6));
                continue;
            }

            // #forward
            if (msg == "#forward") {
                forward_last_file();
                continue;
            }

            // #list
            if (msg == "#list") {
                lock_guard<mutex> lock(mtx_clients);
                if (clients.empty()) {
                    cout << "[Server] No clients connected.\n";
                } else {
                    cout << "[Server] Connected (" << clients.size() << "): ";
                    for (size_t i = 0; i < clients.size(); i++) {
                        cout << clients[i]->name;
                        if (i + 1 < clients.size()) cout << ", ";
                    }
                    cout << "\n";
                }
                continue;
            }

            // #sendfile <name> <path>  — send to one specific client
            // #sendfile <path>         — send to all clients
            if (msg.rfind("#sendfile ", 0) == 0) {
                string arg = msg.substr(10);   // everything after "#sendfile "

                // Check if first token matches a connected client name
                // by scanning for a space and testing the first word
                SOCKET found_sock = INVALID_SOCKET;
                string found_name;
                string remainder;

                size_t space = arg.find(' ');
                if (space != string::npos) {
                    string first_word = arg.substr(0, space);
                    string rest       = arg.substr(space + 1);
                    {
                        lock_guard<mutex> lock(mtx_clients);
                        for (auto& c : clients) {
                            if (c->name == first_word) {
                                found_sock = c->sock;
                                found_name = c->name;
                                remainder  = rest;
                                break;
                            }
                        }
                    }
                }

                if (found_sock != INVALID_SOCKET) {
                    // First word was a valid client name — send to that one client
                    server_send_file_one(found_name, remainder);
                } else {
                    // No client name match — treat entire arg as filepath, send to all
                    server_send_file_all(arg);
                }
                continue;
            }

            // Normal broadcast message
            broadcast("SERVER", msg);
            print_safe("SERVER: " + msg);
        }
    });
    server_input.detach();

    while (true) {
        SOCKET clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) continue;

        int id = next_id++;
        auto client = make_shared<Client>();
        client->id   = id;
        client->name = "Anonymous";
        client->sock = clientSock;

        {
            lock_guard<mutex> lock(mtx_clients);
            clients.push_back(client);
        }
        client->th = thread(client_handler, clientSock, id);
        client->th.detach();
    }

    closesocket(listenSock);
    WSACleanup();
    return 0;
}