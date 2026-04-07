#include "communication.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <chrono>
#include <thread>
#include <iomanip>
#include <filesystem>   // for auto-creating receivedfiles/ folder

using namespace std;
namespace fs = std::filesystem;

#define CHUNK_SIZE 20480


// -----------------------------------------------------------------------
// send_all / recv_all  (low-level reliable byte transfer)
// -----------------------------------------------------------------------

bool send_all(SOCKET s, const char* data, int length) {
    int total_sent = 0;
    while (total_sent < length) {
        int sent = send(s, data + total_sent, length - total_sent, 0);
        if (sent == SOCKET_ERROR) {
            cerr << "Send failed: " << WSAGetLastError() << endl;
            return false;
        }
        if (sent == 0) {
            cerr << "Connection closed during send" << endl;
            return false;
        }
        total_sent += sent;
    }
    return true;
}

bool recv_all(SOCKET s, char* buffer, int length) {
    int total_recv = 0;
    while (total_recv < length) {
        int received = recv(s, buffer + total_recv, length - total_recv, 0);
        if (received == SOCKET_ERROR) {
            cerr << "Recv failed: " << WSAGetLastError() << endl;
            return false;
        }
        if (received == 0) return false;
        total_recv += received;
    }
    return true;
}


// -----------------------------------------------------------------------
// send_frame / recv_frame  (length-prefixed framing)
// FIX: use htonl/ntohl so byte order is consistent across machines
// -----------------------------------------------------------------------

bool send_frame(SOCKET s, const string& msg) {
    // FIX: convert length to network byte order before sending
    uint32_t net_len = htonl((uint32_t)msg.size());

    if (msg.size() > 100 * 1024 * 1024) {   // 100 MB cap
        cerr << "Message too large: " << msg.size() << endl;
        return false;
    }

    if (!send_all(s, (char*)&net_len, sizeof(net_len))) return false;

    if (!msg.empty())
        if (!send_all(s, msg.c_str(), (int)msg.size())) return false;

    return true;
}

bool recv_frame(SOCKET s, string& out) {
    uint32_t net_len = 0;

    if (!recv_all(s, (char*)&net_len, sizeof(net_len))) return false;

    // FIX: convert from network byte order back to host order
    uint32_t len = ntohl(net_len);

    if (len > 100 * 1024 * 1024) {   // 100 MB cap
        cerr << "Invalid length received: " << len << endl;
        return false;
    }

    if (len == 0) { out.clear(); return true; }

    out.resize(len);
    if (!recv_all(s, &out[0], (int)len)) return false;

    return true;
}


// -----------------------------------------------------------------------
// send_file   (chunks + progress bar)
// -----------------------------------------------------------------------

bool send_file(SOCKET s, const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) {
        cout << "Can't open file: " << filename << endl;
        return false;
    }

    file.seekg(0, ios::end);
    long long size = file.tellg();
    file.seekg(0, ios::beg);

    // FIX: only send the bare filename in the header, not the full path
    // so the progress bar and the saved filename are always clean
    string bare = fs::path(filename).filename().string();

    string header = bare + "|" + to_string(size);
    send_frame(s, "#sendfile " + header);

    char buffer[CHUNK_SIZE];
    long long sent = 0;

    while (!file.eof()) {
        file.read(buffer, CHUNK_SIZE);
        int count = (int)file.gcount();
        if (count <= 0) break;
        send_frame(s, string(buffer, count));
        sent += count;

        int percent  = (int)((100LL * sent) / size);
        int barWidth = 40;
        int filled   = (barWidth * percent) / 100;

        string bar = "[" + string(filled, '#') + string(barWidth - filled, '-') + "]";

        double sentMB = sent / (1024.0 * 1024.0);
        double sizeMB = size / (1024.0 * 1024.0);

        cout << "\rUploading " << bare << " "
             << bar << " " << percent << "% "
             << "(" << fixed << setprecision(2) << sentMB << " MB / "
             << sizeMB << " MB)" << flush;

        cout << "\033[F";
    }

    cout << endl << "\n\n Upload complete!" << endl << endl;
    return true;
}


// -----------------------------------------------------------------------
// recv_file   (reassemble chunks + progress bar)
// NEW: auto-creates receivedfiles/ if it doesn't exist
// -----------------------------------------------------------------------

bool recv_file(SOCKET s, const string& sender, const string& header) {
    size_t pos = header.find('|');
    if (pos == string::npos) return false;

    string name = header.substr(0, pos);

    // Strip any path the sender might have included
    size_t slash = name.find_last_of("/\\");
    if (slash != string::npos) name = name.substr(slash + 1);

    long long size = stoll(header.substr(pos + 1));

    // NEW: auto-create the output folder so the app never crashes on a missing directory
    fs::create_directories("receivedfiles");

    ofstream out("receivedfiles/received_" + name, ios::binary);
    if (!out.is_open()) {
        cerr << "Could not create output file." << endl;
        return false;
    }

    long long got = 0;
    string chunk;

    while (got < size) {
        if (!recv_frame(s, chunk)) break;
        out.write(chunk.c_str(), chunk.size());
        got += chunk.size();

        int percent  = (int)((100LL * got) / size);
        int barWidth = 40;
        int filled   = (barWidth * percent) / 100;

        string bar = "[" + string(filled, '#') + string(barWidth - filled, '-') + "]";

        double gotMB  = got  / (1024.0 * 1024.0);
        double sizeMB = size / (1024.0 * 1024.0);

        cout << "\rReceiving " << name << " from " << sender << " "
             << bar << " " << percent << "% "
             << "(" << fixed << setprecision(2) << gotMB << " MB / "
             << sizeMB << " MB)" << flush;

        cout << "\033[F";
    }

    cout << "\nDownload complete!\n";
    cout << "Saved at: receivedfiles/received_" << name << "\n\n";
    cout << "--------------------------------------------------\n";

    return true;
}