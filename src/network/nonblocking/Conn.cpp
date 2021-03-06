#include <iostream>
#include <sys/socket.h>
#include <cstring>
#include "Conn.h"
#include <unistd.h>


namespace Afina {
namespace Network {
namespace NonBlocking {

Conn::Conn(std::shared_ptr<Afina::Storage> ps, int sock): pStorage(ps), socket(sock) {
    parser = Protocol::Parser();
    is_parsed = false;
    cState = State::kRun;
}

Conn::Conn() {}

Conn::~Conn(){}

void Conn::processor() {
    auto buf_size = 1024;
    char buffer[buf_size];
    std::string out;
    size_t parsed = 0;
    size_t curr_pos = 0;
    ssize_t n_read = 0;
    uint32_t body_size = 0;

    n_read = recv(socket, buffer + curr_pos, buf_size - curr_pos, 0);
    if (n_read == 0) {
        close(socket);
        cState == State::kStopped;
        return;
    }
    if (n_read == -1) {
        if (errno & EAGAIN || errno & EWOULDBLOCK) {
            if (cState == State::kStopping){
                out = "Server is shutting down. Connection is closing\n";
                if (send(socket, out.data(), out.size(), 0) <= 0) {
                    throw std::runtime_error("Socket send() failed\n");
                }
                close(socket);
                return;
            }
            return;
        } else {
            close(socket);
            throw std::runtime_error("User irrespectively disconnected");
        }
    }

    curr_pos += n_read;


    while (parsed < curr_pos) {
        try {
            is_parsed = parser.Parse(buffer, curr_pos, parsed);
        }
        catch (std::runtime_error &err) {
            out = std::string("SERVER_ERROR : ") + err.what() + "\r\n";
            if (send(socket, out.data(), out.size(), 0) <= 0) {
                throw std::runtime_error("Socket send() failed\n");
            }
            return;
        }
        if (is_parsed) {
            size_t body_read = curr_pos - parsed;
            memcpy(buffer, buffer + parsed, body_read);
            memset(buffer + body_read, 0, parsed);
            curr_pos = body_read;

            auto cmd = parser.Build(body_size);

            if (body_size <= curr_pos) {
                char args[body_size + 1];
                memcpy(args, buffer, body_size);
                args[body_size] = '\0';
                if (body_size > 0) {
                    memcpy(buffer, buffer + body_size + 2, curr_pos - body_size - 2);
                    memset(buffer + curr_pos - body_size - 2, 0, body_size);
                    curr_pos -= body_size + 2;
                }
                try {
                    cmd->Execute(*(pStorage.get()), args, out);
                    out += "\r\n";
                } catch (std::runtime_error &err) {
                    out = std::string("SERVER_ERROR : ") + err.what() + "\r\n";
                }
                if (send(socket, out.data(), out.size(), 0) <= 0) {
                    throw std::runtime_error("Socket send() failed\n");
                }
                parser.Reset();
                is_parsed = false;
                std::cout << "still in buf: [" << buffer << "]\n";
                std::cout << "parsed: "<< parsed << "\ncurr_pos: " << curr_pos << std::endl;
                parsed = 0;
            }
        }
    }
}

}
}
}
