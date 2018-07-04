#include "Worker.h"

#include <iostream>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <netdb.h>
#include <cstring>
#include <protocol/Parser.h>
#include <afina/execute/Command.h>
#include <map>
#include "Conn.h"

#include "Utils.h"
#define MAXEVENTS 100

namespace Afina {
namespace Network {
namespace NonBlocking {

// See Worker.h
Worker::Worker(std::shared_ptr<Afina::Storage> ps, std::shared_ptr<std::atomic<bool>> running_): pStorage(ps) {
        running = std::move(running_);
}

// See Worker.h
Worker::~Worker() {
}
void *Worker::OnRunProxy(void *p) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    auto data = reinterpret_cast<std::pair<Worker,int>*>(p);
    try {
        data->first.OnRun(&data->second);
    } catch (std::runtime_error &ex) {
        std::cerr << "Server worker fails: " << ex.what() << std::endl;
    }
}

// See Worker.h
void Worker::Start(int server_socket) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    socket = server_socket;
    auto data = new std::pair<Worker, int>(*this, socket);
    if (pthread_create(&thread, NULL, OnRunProxy, data) < 0) {
        throw std::runtime_error("Could not create server thread");
    }
}

// See Worker.h
void Worker::Stop() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running->store(false);
}

// See Worker.h
void Worker::Join() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    pthread_join(thread, nullptr);
}

// See Worker.h
void* Worker::OnRun(void *args) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    struct epoll_event epoll_cfg;
    struct epoll_event events[MAXEVENTS];
    int res;
    int events_count;
    int infd;
    std::map<int, Conn> fd_conns;

    int efd = epoll_create(100500);
    if (efd == -1) {
        throw std::runtime_error("epoll_create failed");
    }

    socket = *reinterpret_cast<int*>(args);
    epoll_cfg.data.fd = socket;
    epoll_cfg.events = EPOLLEXCLUSIVE | EPOLLIN | EPOLLHUP | EPOLLERR;
    res = epoll_ctl(efd, EPOLL_CTL_ADD, epoll_cfg.data.fd, &epoll_cfg);
    if (res == -1) {
        throw std::runtime_error("epoll ctl");
    }

    while(running->load()) {
        events_count = epoll_wait(efd, events, MAXEVENTS, -1);
        if (events_count == -1) {
            if(errno != EINTR) {
                throw std::runtime_error("epoll wait failed");
            }
        } else {
            for (int i = 0; i < events_count; i++) {
                if (
                    (events[i].events & EPOLLERR)
                    || (events[i].events & EPOLLHUP)
                    || (!(events[i].events & EPOLLIN) && !(events[i].events & EPOLLOUT))
                ) {
                    // an error or smth strange
                    std::cout << "User on socket " << events[i].data.fd << " disconnected\n";
                    fd_conns.erase(events[i].data.fd);
                    close(events[i].data.fd);

                } else if (socket == events[i].data.fd) {
                    auto processing = true;
                    while (processing) {
                        try {
                            infd = GetNewConn(efd, socket);
                        }
                        catch(std::runtime_error &err) {
                            std::cout << "Error: " << err.what() << std::endl;
                        }

                        if (infd < 0) { // All new connections acquired
                            processing = false;
                        } else {
                            fd_conns[infd] = Conn(pStorage, infd);
                        }
                    }
                } else { // processing data got
                    try {
                        fd_conns[events[i].data.fd].processor();
                    }
                    catch (std::runtime_error &err) {
                        std::cout << err.what() << "processing fd " << events[i].data.fd << '\n';
                    }
                }
            }
        }
    }
    // Server was stopped

    for (auto &conn : fd_conns) {
        std::cout << "Closing conn on fd " << conn.first << std::endl;
        conn.second.cState = Conn::State::kStopping;
        conn.second.processor();
        fd_conns.erase(conn.first);
    }
}

int Worker::GetNewConn(int efd, int socket) {
    struct sockaddr in_addr;
    socklen_t in_len;
    int infd;
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

    in_len = sizeof in_addr;
    infd = accept(socket, &in_addr, &in_len);
    if (infd == -1) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            /* We have processed all incoming
               connections. */
            return -1;
        } else {
            throw std::runtime_error("Accept");
        }
    }

    int s = getnameinfo(
        &in_addr,
        in_len,
        hbuf,
        sizeof hbuf,
        sbuf,
        sizeof sbuf,
        NI_NUMERICHOST | NI_NUMERICSERV
    );

    make_socket_non_blocking(infd);
    if (s == -1) {
        abort();
    }
    struct epoll_event epoll_cfg;
    epoll_cfg.data.fd = infd;
    epoll_cfg.events = EPOLLERR | EPOLLHUP | EPOLLIN | EPOLLOUT;
    s = epoll_ctl(efd, EPOLL_CTL_ADD, infd, &epoll_cfg);
    if (s == -1) {
        throw std::runtime_error("epoll_ctl error");
    }

    return infd;
}

} // namespace NonBlocking
} // namespace Network
} // namespace Afina
