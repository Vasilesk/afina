#include "ServerImpl.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <pthread.h>
#include <signal.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <afina/Storage.h>

#include <protocol/Parser.h>
#include <afina/execute/Command.h>

namespace Afina {
namespace Network {
namespace Blocking {

void *ServerImpl::RunAcceptorProxy(void *p) {
    ServerImpl *srv = reinterpret_cast<ServerImpl *>(p);
    try {
        srv->RunAcceptor();
    } catch (std::runtime_error &ex) {
        std::cerr << "Server fails: " << ex.what() << std::endl;
    }
    return 0;
}

void *ServerImpl::RunConnectionProxy(void *p) {
    auto server_client_pair = reinterpret_cast<std::pair<ServerImpl*, int>*>(p);
    auto srv = server_client_pair->first;
    auto client_socket = server_client_pair->second;
    try {
        srv->RunConnection(client_socket);
    } catch (std::runtime_error &ex) {
        std::cerr << "Server fails: " << ex.what() << std::endl;
    }
    srv->CloseConnection(client_socket);
    return 0;
}

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps) : Server(ps) {}

// See Server.h
ServerImpl::~ServerImpl() {}

// See Server.h
void ServerImpl::Start(uint32_t port, uint16_t n_workers) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // If a client closes a connection, this will generally produce a SIGPIPE
    // signal that will kill the process. We want to ignore this signal, so send()
    // just returns -1 when this happens.
    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, NULL) != 0) {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    // Setup server parameters BEFORE thread created, that will guarantee
    // variable value visibility
    max_workers = n_workers;
    listen_port = port;

    // The pthread_create function creates a new thread.
    //
    // The first parameter is a pointer to a pthread_t variable, which we can use
    // in the remainder of the program to manage this thread.
    //
    // The second parameter is used to specify the attributes of this new thread
    // (e.g., its stack size). We can leave it NULL here.
    //
    // The third parameter is the function this thread will run. This function *must*
    // have the following prototype:
    //    void *f(void *args);
    //
    // Note how the function expects a single parameter of type void*. We are using it to
    // pass this pointer in order to proxy call to the class member function. The fourth
    // parameter to pthread_create is used to specify this parameter value.
    //
    // The thread we are creating here is the "server thread", which will be
    // responsible for listening on port 23300 for incoming connections. This thread,
    // in turn, will spawn threads to service each incoming connection, allowing
    // multiple clients to connect simultaneously.
    // Note that, in this particular example, creating a "server thread" is redundant,
    // since there will only be one server thread, and the program's main thread (the
    // one running main()) could fulfill this purpose.
    running.store(true);
    if (pthread_create(&accept_thread, NULL, ServerImpl::RunAcceptorProxy, this) < 0) {
        throw std::runtime_error("Could not create server thread");
    }
}

// See Server.h
void ServerImpl::Stop() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running.store(false);

    shutdown(server_socket, SHUT_RDWR);
}

// See Server.h
void ServerImpl::Join() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    std::unique_lock<std::mutex> lock(connections_mutex);
    while (!connections.empty()) {
        connections_cv.wait(lock);
    }
    pthread_join(accept_thread, 0);
}

// See Server.h
void ServerImpl::RunAcceptor() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // For IPv4 we use struct sockaddr_in:
    // struct sockaddr_in {
    //     short int          sin_family;  // Address family, AF_INET
    //     unsigned short int sin_port;    // Port number
    //     struct in_addr     sin_addr;    // Internet address
    //     unsigned char      sin_zero[8]; // Same size as struct sockaddr
    // };
    //
    // Note we need to convert the port to network order

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;          // IPv4
    server_addr.sin_port = htons(listen_port); // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Bind to any address

    // Arguments are:
    // - Family: IPv4
    // - Type: Full-duplex stream (reliable)
    // - Protocol: TCP
    server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == -1) {
        throw std::runtime_error("Failed to open socket");
    }

    // when the server closes the socket,the connection must stay in the TIME_WAIT state to
    // make sure the client received the acknowledgement that the connection has been terminated.
    // During this time, this port is unavailable to other processes, unless we specify this option
    //
    // This option let kernel knows that we are OK that multiple threads/processes are listen on the
    // same port. In a such case kernel will balance input traffic between all listeners (except those who
    // are closed already)
    int opts = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opts, sizeof(opts)) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket setsockopt() failed");
    }

    // Bind the socket to the address. In other words let kernel know data for what address we'd
    // like to see in the socket
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket bind() failed");
    }

    // Start listening. The second parameter is the "backlog", or the maximum number of
    // connections that we'll allow to queue up. Note that listen() doesn't block until
    // incoming connections arrive. It just makesthe OS aware that this process is willing
    // to accept connections on this socket (which is bound to a specific IP and port)
    if (listen(server_socket, 5) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket listen() failed");
    }

    int client_socket;
    struct sockaddr_in client_addr;
    socklen_t sinSize = sizeof(struct sockaddr_in);
    while (running.load()) {
        std::cout << "network debug: waiting for connection..." << std::endl;

        // When an incoming connection arrives, accept it. The call to accept() blocks until
        // the incoming connection arrives
        if ((client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &sinSize)) == -1) {
            close(server_socket);
            throw std::runtime_error("Socket accept() failed");
        }
        connections_mutex.lock();
        auto data = std::make_shared<std::pair<ServerImpl*, int>>(this, client_socket);
        bool success;
        if (connections.size() < max_workers) {
            pthread_t client_thread;
            std::cout << "data get " << data.get() << '\n';
            auto result = pthread_create(&client_thread, NULL, RunConnectionProxy, data.get());
            if (result == 0) {
                pthread_detach(client_thread);
                connections.insert(client_thread);
                success = true;
            } else {
                success = false;
            }
        } else {
            success = false;
        }

        if (!success) {
            close(client_socket);
        }

        connections_mutex.unlock();
        connections_cv.notify_one();
        std::cout << "socket closed" << std::endl;
    }

    // Cleanup on exit...
    close(server_socket);
}

// See Server.h
void ServerImpl::RunConnection(int client_socket) {
    std::cout << "new conn run" << std::endl;
    char input_buf[BUFFER_SIZE];
    std::string exec_in_buf;
    std::string exec_out_buf;
    size_t received_count;
    size_t parsed_count;
    uint32_t data_size;

    Protocol::Parser parser;

    auto processing = true;

    while (running.load() && processing) {
        parser.Reset();
        received_count = recv(client_socket, input_buf, BUFFER_SIZE, 0);
        if (received_count <= 0) {
            processing = false;
        } else if (input_buf[0] == '\r') {
            processing = false;
        } else {
            input_buf[received_count] = '\0';
            std::cout << "buf: " << input_buf << '\n';
            auto parse_success = parser.Parse(input_buf, received_count, parsed_count);
            std::cout << "received count:" << received_count << '\n';
            std::cout << "parsed count:" << parsed_count << '\n';
            if (!parse_success) {
                std::cout << "bad command" << '\n';
                processing = false;
            } else {
                auto cmd = parser.Build(data_size);
                std::cout << "build count:" << data_size << '\n';
                std::cout << "cmd:" << (cmd != nullptr) << '\n';
                if (cmd == nullptr) {
                    std::cout << "bad command was built" << '\n';
                    processing = false;
                } else {
                    std::unique_ptr<char[]> data(new char[data_size]);
                    if(data_size > 0) {
                        recChunks(client_socket, data.get(), data_size + 2);
                        exec_in_buf = data.get();
                    } else {
                        exec_in_buf = "";
                    }
                    cmd->Execute(*pStorage, exec_in_buf, exec_out_buf);
                    std::cout << "out execution: " << exec_out_buf << '\n';

                    exec_out_buf += "\r\n";
                    auto message_length = exec_out_buf.size();
                    auto message_ptr = exec_out_buf.c_str();
                    processing = sendChunks(client_socket, message_ptr, message_length);
                }
            }
        }
    }
}

void ServerImpl::CloseConnection(int client_socket) {
    {
        std::lock_guard<std::mutex> lock(connections_mutex);
        connections.erase(pthread_self());
    }
    close(client_socket);
    connections_cv.notify_one();
}

bool ServerImpl::sendChunks(int client_socket, const char* message_ptr, size_t message_length) {
    auto send_left = message_length;
    size_t send_rc;
    while (send_left > 0)
    {
        send_rc = send(client_socket, message_ptr, send_left, 0);
        if (send_rc != -1) {
            send_left -= send_rc;
            message_ptr += send_rc;
        } else {
            return false;
        }
    }
    return true;
}

bool ServerImpl::recChunks(int client_socket, char* message_ptr, size_t message_length) {
    auto input_buf = message_ptr;
    auto rec_left = message_length;
    size_t recieved;
    while (rec_left > 0)
    {
        recieved = recv(client_socket, input_buf, rec_left, 0);
        if (recieved != -1) {
            rec_left -= recieved;
            input_buf += recieved;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace Blocking
} // namespace Network
} // namespace Afina
