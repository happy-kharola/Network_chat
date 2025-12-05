#pragma once
// filetransfer.hpp
// simple file sending and receiving functions for chat program

#include <string>
#include <WinSock2.h>

bool send_frame(SOCKET s, const std::string& data);    // send data safely
bool recv_frame(SOCKET s, std::string& out);           // receive data safely

bool send_file(SOCKET s, const std::string& filename); // send file
bool recv_file(SOCKET s, const std::string& sender, const std::string& header); // receive file
