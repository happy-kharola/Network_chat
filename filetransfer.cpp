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

// For confirming the packet is sent properly.
// uses "total_sent" and keeps calling send until "total_sent" equals length.
// false: if sent ruturn -1 or 0
// true: data is sent
// pre_conditions to work properly: length > 0, data not too big

bool send_all(SOCKET s, const char* data, int length){
    int total_sent = 0;

    while( total_sent < length ){
        int sent = send(s, data + total_sent, length - total_sent, 0);

        if( sent == SOCKET_ERROR){
            // something went wrong - network error
            cerr << "Send failed: "<< WSAGetLastError() << endl;
            return false;
        }

        if( sent == 0){
            // conection closed
            cerr << "Connection closed during send" << endl;
            return false;
        }
    
        total_sent += sent;
    }

    return true;
    
}


bool recv_all(SOCKET s, char* buffer, int length){
    int total_recv = 0;

    while( total_recv < length){
        int received = recv(s, buffer + total_recv, length - total_recv, 0);

        if( received == SOCKET_ERROR){
            //Network error
            cerr << "Recv failed: " << WSAGetLastError <<endl;
            return false;
        }

        if( received == 0){
            // connection closed gracefully by other side
            return false;
        }

        total_recv += received;
    }

    return true; // All bytes received successfully
}


bool send_frame(SOCKET s, const string& msg){
    int len = msg.size();

    // Validate size 
    if( len < 0 || len > 100 * 1024 * 1024){ // 100MB max
        cerr << "Message too large or invalid: " << len << endl;
        return false;
    }

    // Send length 
    if(!send_all(s, (char*)&len, sizeof(len))){
        return false; // send all already printed error
    }
    
    // Send mesg
    if( len > 0){
            if(!send_all(s, msg.c_str(), len)){
            return false;
    }
    }

    return true;
}



bool recv_frame(SOCKET s, string& out){
    int len = 0;
    
    // Receive the length
    if(!recv_all(s, (char*)&len, sizeof(len))){
        return false;    //connection lost or error
    }

    // Validate the length
    if(len < 0 || len > 100 * 1024 * 1024){ //100MB max
        cerr << "Invalid length received: "<< len << endl;
        return false;    
    }

    // Handle empty messages
    if( len == 0){
        out.clear();
        return true;
    }
    
    // Receive the actual data
    out.resize(len);
    if(!recv_all(s, &out[0], len)){
        return false;  // connection lost during data transfer
    }

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
