#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <queue>
#include <vector>

#include <signal.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <functional>
#include <string.h>

using namespace std;

using Message = std::vector<unsigned char>;
using MessageReference = std::shared_ptr<Message>;


vector<unsigned char> add_header(vector<unsigned char> data) {
    if(data.size() > 1000) {
        throw std::runtime_error("Data to long...");
    }

    vector<unsigned char> result;
    result.reserve(data.size() + 2);

    unsigned char len[2];
    (*(uint16_t *) len) = htons(data.size());
    result.push_back(len[0]);
    result.push_back(len[1]);

    std::copy(data.begin(), data.end(), back_inserter(result));

    return result;
}


class Descriptor {
protected:
    int fd;
public:
    Descriptor() : fd(-1) {
    }
    Descriptor(int fd) : fd(fd) {
    }

    Descriptor(const Descriptor &) = delete;
    Descriptor(Descriptor &&orig) {
        this->fd = orig.fd;
        orig.fd = -1;
    }

    Descriptor & operator= (const Descriptor &orig) = delete;
    Descriptor & operator= (Descriptor &&orig) {
        this->fd = orig.fd;
        orig.fd = -1;
        return *this;
    }

    virtual ~Descriptor() {
        this->close();
    }

    void set_nonblocking() {
        if(fd == -1) {
            throw std::runtime_error("Invalid descriptor.");
        }
        int flags = ::fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    virtual void on_notify(uint32_t event) {
        throw std::runtime_error("Not implemented but allows compiling.");
    }

    int get_internal() const { return fd; }

    operator bool() {
        return fd >= 0;
    }

    void close() {
        if(fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

class Connection : public Descriptor {
protected:
    // Writing
    std::queue<MessageReference> to_send;
    Message current_to_send;
    int write_offset = -1;

    // Reading
    std::vector<unsigned char> read_buffer;
    int current_message_length = -2; // <0 means we read buffer length
public:
    std::function<void()> on_connection_closed = [](){};
    std::function<void(uint32_t)> subscribe_events = [](uint32_t) {};
    std::function<void(MessageReference)> on_message_received = [](MessageReference m){};

    Connection(int fd) : Descriptor(fd) {
    }

    virtual void on_notify(uint32_t event) {
        if(event & (EPOLLRDHUP | EPOLLHUP)) {
            if(*this) {
                on_connection_closed();
                this->close();
            }
            return;
        }
        if(event & EPOLLIN) {
            if(current_message_length == 0) current_message_length = -2;
            else if(current_message_length < 0) {
                // Read header
                unsigned char buff[2];
                int red = read(fd, buff, -current_message_length);
                if(red < 0) {
                    throw runtime_error("Reading failed");
                }
                copy(buff, buff + red, back_inserter(read_buffer));
                current_message_length += red;
                if(current_message_length >= 0) {
                    char buff[2];
                    buff[0] = read_buffer[0];
                    buff[1] = read_buffer[1];
                    int msglen = ntohs(*(uint16_t*)buff);
                    read_buffer.clear();
                    current_message_length = msglen;
                    if(current_message_length > 1000) {
                        if(*this) {
                            on_connection_closed();
                            this->close();
                        }
                        return;
                    }
                    if(current_message_length == 0) {
                        MessageReference m = shared_ptr<Message>(new vector<unsigned char>());
                        read_buffer.clear();
                        on_message_received(m);
                    }
                }
            } else {
                // Read body
                unsigned char buff[current_message_length];
                int red = read(fd, buff, current_message_length);
                if(red < 0) {
                    throw runtime_error("Reading body failed");
                }
                for(int i = 0; i < red; i++) {
                    if(buff[i] == 0 || buff[i] == '\n') {
                        if(*this) {
                            on_connection_closed();
                            this->close();
                        }
                    }
                }
                copy(buff, buff+red, back_inserter(read_buffer));
                current_message_length -= red;
                if(current_message_length <= 0) {
                    MessageReference m = shared_ptr<Message>(new vector<unsigned char>(move(read_buffer)));
                    read_buffer.clear();
                    on_message_received(m);
                }
            }
        }
        if(event & EPOLLOUT) {
            if(write_offset == -1 and !to_send.empty()) {
                auto msg = *to_send.front();
                current_to_send = add_header(msg);
                write_offset = 0;
                to_send.pop();
            }

            if(write_offset >= 0) {
                // Continue writing
                int written = write(fd, current_to_send.data() + write_offset, current_to_send.size() - write_offset);
                if(written < 0)
                    throw runtime_error("Failed writing");
                write_offset += written;
                if(write_offset >= (int)current_to_send.size()) {
                    write_offset = -1;
                }
            }

            if(write_offset == -1 and to_send.empty()) {
                subscribe_events(EPOLLIN | EPOLLRDHUP);
            }
        }
    }

    void send(MessageReference m) {
        if(to_send.empty()) {
            subscribe_events(EPOLLIN | EPOLLOUT | EPOLLRDHUP);
        }
        to_send.push(m);
    }
};

class Listener : public Descriptor {
    struct sockaddr_in addr;
    int max_pending = 16;
public:
    std::function<void(Connection)> on_new_connection = [](Connection c){};
public:
    Listener(int port) : Descriptor(::socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        if(fd < 0) {
            throw std::runtime_error("Creating listener failed.");
        }
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if(::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            throw std::runtime_error("Binding listener failed.");
        }

        if(::listen(fd, max_pending) < 0) {
            throw std::runtime_error("Listening failed.");
        }
        set_nonblocking();
    }

    virtual void on_notify(uint32_t event) {
        if(event & EPOLLIN) {
            sockaddr_in client_addr;
            socklen_t ca_len = sizeof(client_addr);

            int cl = accept(fd, (struct sockaddr *) &client_addr, &ca_len);
            if (cl < 0) {
                throw std::runtime_error("Accepting client failed.");
            }
            on_new_connection(Connection(cl));
        }
    }
};

class Poller : public Descriptor {
    std::map<int, Descriptor*> descriptors;
    int max_events = 16;
    struct epoll_event *events;

public:
    Poller() : Descriptor(epoll_create(0xCAFE)) {
        if(fd < 0) {
            throw std::runtime_error("EPoll creating failed.");
        }

        events = new struct epoll_event[max_events];
    }

    ~Poller() {
        delete[] events;
    }

    void add(Descriptor &descriptor, unsigned int events) {
        epoll_event e;
        e.data.fd = descriptor.get_internal();
        e.events = events;

        if(epoll_ctl(fd, EPOLL_CTL_ADD, descriptor.get_internal(), &e) < 0) {
            throw std::runtime_error("Adding failed.");
        }
        descriptors[descriptor.get_internal()] = &descriptor;
    }

    void remove(Descriptor &descriptor) {
        if(!descriptor) return;
        auto descriptor_iterator = descriptors.find(descriptor.get_internal());

        if(descriptor_iterator == descriptors.end()) {
            throw std::runtime_error("Removing non-existing descriptor.");
        }

        if(epoll_ctl(fd, EPOLL_CTL_DEL, descriptor.get_internal(), nullptr) < 0) {
            throw std::runtime_error("Removing from epoll failed.");
        }
        descriptors.erase(descriptor_iterator);
    }

    void modify(Descriptor &descriptor, unsigned int events) {
        auto descriptor_iterator = descriptors.find(descriptor.get_internal());

        if(descriptor_iterator == descriptors.end()) {
            throw std::runtime_error("Modifying non-existing descriptor.");
        }


        epoll_event e;
        e.data.fd = descriptor.get_internal();
        e.events = events;

        if(epoll_ctl(fd, EPOLL_CTL_MOD, descriptor.get_internal(), &e) < 0) {
            throw std::runtime_error("EPoll modification failed.");
        }
    }

    bool wait(int timeout) {
        int got_events = epoll_wait(fd, events, max_events, timeout);

        for(int i = 0; i < got_events; i++) {
            int cfd = events[i].data.fd;
            auto descriptor_iterator = descriptors.find(cfd);
            if(descriptor_iterator != descriptors.end()) {
                descriptor_iterator->second->on_notify(events[i].events);
            }
        }
        return true;
    }
};

class Server {
    Poller poller;
    Listener listener;
    std::list<Connection> connections;
    bool running = true;

public:
    Server(int port = 20160) : poller(), listener(port) {
        listener.on_new_connection = [this](Connection c) {
            connections.push_back(std::move(c));
            auto iterator = connections.end();
            iterator--;
            Connection &connection = *iterator;
            connection.on_connection_closed = [&connection, this, iterator]() {
                poller.remove(connection);
                connections.erase(iterator);
            };
            connection.on_message_received = [&connection, this] (MessageReference m) {
                for(auto &c : connections) {
                    if(&c == &connection) continue;
                    c.send(m);
                }

            };
            connection.subscribe_events = [&connection, this](uint32_t events) {
                poller.modify(connection, events);
            };
            poller.add(connection, EPOLLIN | EPOLLRDHUP);
        };
        poller.add(listener, EPOLLIN);
    }
    ~Server() {
        poller.remove(listener);
    }

    void run() {
        running = true;
        while(running) {
            poller.wait(1000);
        }
    }

    void stop() {
        running = false;
    }
};

std::function<void()> keyboard_interrupt = [](){};

void signal_handler(int signum) {
    switch(signum) {
        case SIGINT:
            keyboard_interrupt();
            break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    int port = 20160;
    if(argc < 1 || argc > 2) {
        cerr << argv[0] << " [port]" << endl;
        return 1;
    }

    if(argc > 1) {
        port = atoi(argv[1]);
    }

    try {
        Server server(port);
        server.run();
    } catch(runtime_error &e) {
        cerr << "Exception message: " << e.what() << endl;
        if(errno > 0) {
            cerr << "Error number (errno): " << errno << " [" << strerror(errno) << "]" << endl;
        }
        return 100;
    }
    return 0;
}