#pragma once
// communication.hpp
// Shared declarations for chat + file transfer functions

#include <string>
#include <mutex>
#include <WinSock2.h>

// Guards all console output (cout) in whichever program includes this header,
// so file-transfer progress printing never interleaves with chat printing.
extern std::mutex g_cout_mutex;

bool send_frame(SOCKET s, const std::string& data);    // send data safely
bool recv_frame(SOCKET s, std::string& out);           // receive data safely

bool send_file(SOCKET s, const std::string& filename); // send file
bool recv_file(SOCKET s, const std::string& sender, const std::string& header); // receive file