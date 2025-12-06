#include "communication.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <chrono>
#include <thread>
#include<iomanip>

using namespace std;

#define CHUNK_SIZE 20480

// simple helper to send a message with its length first
bool send_frame(SOCKET s, const string& msg) {
    int len = msg.size();
    send(s, (char*)&len, sizeof(len), 0);    // send the length first
    send(s, msg.c_str(), len, 0);            // send the actual message
    return true;
}

// simple helper to receive a message (read length, then data)
bool recv_frame(SOCKET s, string& out) {
    int len = 0;
    int r = recv(s, (char*)&len, sizeof(len), 0);
    if (r <= 0) return false;

    out.resize(len);
    recv(s, &out[0], len, 0);  // read the actual message
    return true;
}

// send a file in chunks
bool send_file(SOCKET s, const string& filename) {
    ifstream file(filename, ios::binary);
    if (!file.is_open()) {
        cout << "Can't open file: " << filename << endl;
        return false;
    }

    file.seekg(0, ios::end);
    int size = file.tellg();
    file.seekg(0, ios::beg);

    string header = filename + "|" + to_string(size);
    send_frame(s, "#sendfile " + header);

    

    char buffer[CHUNK_SIZE];
    int sent = 0;

// for progress bar it also delay a lil to make progress visible

    while (!file.eof()) {
        file.read(buffer, CHUNK_SIZE);
        int count = file.gcount();
        send_frame(s, string(buffer, count));
        sent += count;

        // percentage
        int percent = (100 * sent) / size;

        // progress bar width
        int barWidth = 40;
        int filled = (barWidth * percent) / 100;

        // build bar string
        string bar = "[" 
            + string(filled, '#') 
            + string(barWidth - filled, '-') 
            + "]";


        // convert sizes to MB
        double sentMB = sent / (1024.0 * 1024.0);
        double sizeMB = size / (1024.0 * 1024.0);

        cout << "\rUploading " << filename << "\n"
            << bar << " " << percent << "% "
            << "(" << fixed << setprecision(2) << sentMB << " MB / "
            << sizeMB << " MB)" << flush;

        // slow down animation (20ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // move cursor up 1 line so bar overwrites properly
        cout << "\033[F";
    }
    cout << endl << "\n\n Upload complete!" << endl<<endl;;



    
    return true;
}

// receive a file
bool recv_file(SOCKET s, const string& sender, const string& header) {
    size_t pos = header.find('|');
    if (pos == string::npos) return false;

    string name = header.substr(0, pos);

  
    size_t slash = name.find_last_of("/\\");
    if (slash != string::npos) name = name.substr(slash + 1);

    int size = stoi(header.substr(pos + 1));

    


   ofstream out("receivedfiles/received_" + name, ios::binary);

    if (!out.is_open()) return false;

    int got = 0;
    string chunk;

 //progress bar to show progress, slowed down to make progress visible

    while (got < size) {
        if (!recv_frame(s, chunk)) break;

        out.write(chunk.c_str(), chunk.size());
        got += chunk.size();

        // percentage
        int percent = (100 * got) / size;

        // progress bar width
        int barWidth = 40;
        int filled = (barWidth * percent) / 100;

        string bar = "[" 
            + string(filled, '#') 
            + string(barWidth - filled, '-') 
            + "]";


        double gotMB = got / (1024.0 * 1024.0);
        double sizeMB = size / (1024.0 * 1024.0);

        cout << "\rReceiving " << name << " from " << sender << "\n"
            << bar << " " << percent << "% "
            << "(" << fixed << setprecision(2) << gotMB << " MB / "
            << sizeMB << " MB)" << flush;

        // slow animation (20ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // move cursor up 1 line
        cout << "\033[F";
    }

    cout << "\nDownload complete!\n";
    cout << "Saved at : receivedfiles/received_" << name << "\n\n";
    cout << "--------------------------------------------------\n";

    return true;
}
