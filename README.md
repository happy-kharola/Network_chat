# Network Chat

A multi-client LAN chat application with file transfer, written in C++ for Windows (WinSock2). One server relays messages between any number of clients over TCP.

## Features

- Group chat: every message is broadcast to all other connected users
- Unique usernames (the server rejects duplicates and asks again)
- File transfer with a progress bar (sent in 20 KB chunks)
- Server-side controls: kick users, list users, send or forward files
- One thread per client on the server, so clients never block each other

## Quick start

1. Run `server.exe`. Press Enter to use the default port (`12345`). The server prints its LAN address, e.g. `192.168.1.5:12345`.
2. Run `client.exe` on the same PC or another PC on the same network. Enter the server's IP, the port, and a username.
3. Type a message and press Enter to send it to everyone.

Keep `libgcc_s_seh-1.dll` and `libwinpthread-1.dll` in the same folder as the `.exe` files. If Windows Firewall asks on the first run, allow access on private networks.

## Commands

**Client**

| Command | What it does |
|---|---|
| `<message>` | Send a text message to everyone |
| `#sendfile <path>` | Upload a file to the server |
| `#list` | Show all connected users |
| `#exit` | Disconnect and close |

**Server**

| Command | What it does |
|---|---|
| `<message>` | Broadcast a message as `SERVER` |
| `#list` | Show all connected users |
| `#kick <name>` | Disconnect a user |
| `#sendfile <path>` | Send a file to all clients |
| `#sendfile <name> <path>` | Send a file to one client |
| `#forward` | Re-send the last file received from a client to everyone except the original sender |

When a client uploads a file, the server saves it and tells the other clients. They receive the file itself only when the server runs `#forward`.

## Project structure

| File | Role |
|---|---|
| `server.cpp` | Accepts connections, runs one handler thread per client, broadcasts messages, handles server commands |
| `client.cpp` | Connects to the server, negotiates a username, runs a send thread (user input) and a receive thread (incoming messages and files) |
| `filetransfer.cpp` | Shared networking code: reliable send/receive, message framing, file send/receive with progress bar |
| `communication.hpp` | Declarations for the shared functions (`send_frame`, `recv_frame`, `send_file`, `recv_file`) |

## How it works

- **Transport:** TCP sockets, default port `12345`.
- **Framing:** every message is a *frame*: a 4-byte length (network byte order) followed by that many bytes. This lets the receiver know exactly where each message ends.
- **Text messages** are two frames: the sender's name, then the message. Server notices use the sender name `SERVER`.
- **Login:** the client sends its username, and the server replies `#nameok` or `#nametaken`.
- **File transfer:** a header frame `#sendfile <filename>|<size>` is sent first, then the file as a series of chunk frames. Only the bare filename is sent, never the full path.
- **Received files** are saved to `receivedfiles/received_<filename>`. The folder is created automatically if it does not exist. A file with the same name is overwritten.

## Build

Requires MinGW-w64 `g++` version 11 or newer (check with `g++ --version`).

```
g++ server.cpp filetransfer.cpp -o server.exe -lws2_32
g++ client.cpp filetransfer.cpp -o client.exe -lws2_32
```

Add `-static` to bundle the runtime libraries into the `.exe`.

### Older g++ (before version 11)

Older versions don't use C++17 by default, and this project needs it for `<filesystem>`. Add `-std=c++17`:

```
g++ -std=c++17 server.cpp filetransfer.cpp -o server.exe -lws2_32
g++ -std=c++17 client.cpp filetransfer.cpp -o client.exe -lws2_32
```

## Limitations

- Windows only (uses WinSock2)
- No encryption or authentication: messages and files are sent as plain data, so use it on trusted networks only
- Designed for a local network; connecting over the internet needs port forwarding
