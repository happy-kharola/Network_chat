#include "communication.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <chrono>
#include <thread>
#include <iomanip>
#include <filesystem>

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
// htonl/ntohl ensures byte order is consistent across machines
// -----------------------------------------------------------------------

bool send_frame(SOCKET s, const string& msg) {
    if (msg.size() > 100 * 1024 * 1024) {
        cerr << "Message too large: " << msg.size() << endl;
        return false;
    }

    uint32_t net_len = htonl((uint32_t)msg.size());
    if (!send_all(s, (char*)&net_len, sizeof(net_len))) return false;
    if (!msg.empty())
        if (!send_all(s, msg.c_str(), (int)msg.size())) return false;

    return true;
}

bool recv_frame(SOCKET s, string& out) {
    uint32_t net_len = 0;
    if (!recv_all(s, (char*)&net_len, sizeof(net_len))) return false;

    uint32_t len = ntohl(net_len);
    if (len > 100 * 1024 * 1024) {
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
// Always sends only the bare filename in the header, never the full path
// -----------------------------------------------------------------------

bool send_file(SOCKET s, const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) {
        cout << "Cannot open file: " << filename << endl;
        return false;
    }

    file.seekg(0, ios::end);
    long long size = file.tellg();
    file.seekg(0, ios::beg);

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
        string bar   = "[" + string(filled, '#') + string(barWidth - filled, '-') + "]";

        double sentMB = sent / (1024.0 * 1024.0);
        double sizeMB = size / (1024.0 * 1024.0);

        {
            lock_guard<mutex> lock(g_cout_mutex);
            cout << "\rUploading " << bare << " "
                 << bar << " " << percent << "% "
                 << "(" << fixed << setprecision(2) << sentMB << " MB / "
                 << sizeMB << " MB)" << flush;
            cout << "\033[F";
        }
    }

    {
        lock_guard<mutex> lock(g_cout_mutex);
        cout << endl << "\n\n Transfer complete: " << bare << endl << endl;
    }
    return true;
}

// -----------------------------------------------------------------------
// recv_file   (reassemble chunks + progress bar)
// Auto-creates receivedfiles/ if it does not exist
// -----------------------------------------------------------------------

bool recv_file(SOCKET s, const string& sender, const string& header) {
    size_t pos = header.find('|');
    if (pos == string::npos) return false;

    string name = header.substr(0, pos);
    size_t slash = name.find_last_of("/\\");
    if (slash != string::npos) name = name.substr(slash + 1);

    long long size = stoll(header.substr(pos + 1));

    fs::create_directories("receivedfiles");

    string out_path = "receivedfiles/received_" + name;
    ofstream out(out_path, ios::binary);
    if (!out.is_open()) {
        cerr << "Could not create output file." << endl;
        return false;
    }

    // Apply a receive timeout for just this transfer, so a stalled connection
    // (dropped Wi-Fi, frozen peer, etc.) can't hang this thread forever.
    // We save whatever the socket's previous timeout was and restore it after.
    DWORD prev_timeout = 0;
    int prev_len = sizeof(prev_timeout);
    getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&prev_timeout, &prev_len);

    DWORD transfer_timeout = 15000; // 15s of silence = treat as stalled
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&transfer_timeout, sizeof(transfer_timeout));

    long long got = 0;
    string chunk;
    bool ok = true;

    while (got < size) {
        if (!recv_frame(s, chunk)) { ok = false; break; }
        out.write(chunk.c_str(), chunk.size());
        got += chunk.size();

        int percent  = (int)((100LL * got) / size);
        int barWidth = 40;
        int filled   = (barWidth * percent) / 100;
        string bar   = "[" + string(filled, '#') + string(barWidth - filled, '-') + "]";

        double gotMB  = got  / (1024.0 * 1024.0);
        double sizeMB = size / (1024.0 * 1024.0);

        {
            lock_guard<mutex> lock(g_cout_mutex);
            cout << "\rReceiving " << name << " from " << sender << " "
                 << bar << " " << percent << "% "
                 << "(" << fixed << setprecision(2) << gotMB << " MB / "
                 << sizeMB << " MB)" << flush;
            cout << "\033[F";
        }
    }

    out.close();

    // Restore whatever timeout was set before this transfer started.
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&prev_timeout, sizeof(prev_timeout));

    lock_guard<mutex> lock(g_cout_mutex);
    if (!ok) {
        fs::remove(out_path); // don't leave a corrupt partial file lying around
        cout << "\nTransfer failed or timed out — connection stalled.\n";
        cout << "Partial file discarded.\n\n";
        return false;
    }

    cout << "\nTransfer complete.\n";
    cout << "Saved: " << out_path << "\n\n";
    cout << "--------------------------------------------------\n";

    return true;
}