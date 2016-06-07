#include <functional>
#include <iostream>
#include <queue>
#include <vector>

#include <cstring>

#include <signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>


using namespace std;

vector<unsigned char> add_header(vector<unsigned char> data) {
    if(data.size() > 1000) {
        throw std::runtime_error("Data too long...");
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

class Client {
protected:
    int efd = -1;
    int cfd = STDIN_FILENO;
    int max_events = 4;
    bool working = true;
    bool hasinput = true;
    struct epoll_event *events;

    // Writing
    queue<vector<unsigned char>> send_buffer;
    int write_offset = 0;

    // Reading
    std::vector<unsigned char> read_buffer;
    int current_message_length = -2; // <0 means we read buffer length

    int fd = -1;
public:
    Client(std::string host, std::string service) {
        efd = ::epoll_create(0xCAFE);
        events = new struct epoll_event[max_events];

        setup_network(host, service);

        add(fd, EPOLLIN | EPOLLRDHUP);
        add(cfd, EPOLLIN | EPOLLRDHUP);
    }

    ~Client() {
        if(::close(fd) < 0)
            throw runtime_error("Closing network failed.");
        if(::close(efd) < 0)
            throw runtime_error("Closing poll failed.");
    }

    void add(int desc, uint32_t events) {
        struct epoll_event e;
        e.data.fd = desc;
        e.events = events;

        if(epoll_ctl(efd, EPOLL_CTL_ADD, desc, &e) < 0) {
            throw std::runtime_error("Epoll add failed");
        }
    }

    void modify(int desc, uint32_t events) {
        struct epoll_event e;
        e.data.fd = desc;
        e.events = events;

        if(epoll_ctl(efd, EPOLL_CTL_MOD, desc, &e) < 0) {
            throw std::runtime_error("Epoll modify failed");
        }
    }

    void send() {
        if(send_buffer.empty()) {
            return;
        }
        auto &msg = send_buffer.front();
        ssize_t written = write(fd, msg.data() + write_offset, msg.size() - write_offset);
        if(written < 0) {
            throw std::runtime_error("Sending failed");
        }

        write_offset += written;
        if(write_offset == (int)msg.size()) {
            write_offset = 0;
            send_buffer.pop();
            if(send_buffer.empty()) {
                modify(fd, EPOLLIN | EPOLLRDHUP);
            }
        }
    }

    void work() {
        working = true;
        while(working) {
            int got_events = epoll_wait(efd, events, max_events, 100);

            for(int i = 0; i < got_events; i++) {
                if(events[i].data.fd == cfd) {
                    if(events[i].events & EPOLLIN) {
                        // READ LINE AND PUT TO send_buffer
                        char *buff;
                        size_t linen = 0;
                        ssize_t len = getline(&buff, &linen, stdin);
                        if(len == -1) {
                            if(feof(stdin)) {
                                hasinput = false;
                                return;
                            } else {
                                throw runtime_error("Error within stdin!");
                            }
                        }
                        while(len > 0 and (buff[len-1] == '\n' or buff[len-1] == '\r')) {
                            len--;
                        }
                        if(len > 1000) len = 1000;  // Too long message - cut
                        vector<unsigned char> message;
                        message.reserve(len + 3);
                        copy(buff, buff + len, back_inserter(message));
                        ::free(buff);
                        message = add_header(move(message));
                        send_buffer.push(message);
                        modify(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
                    }
                    if(events[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                        hasinput = false;
                    }
                } else if(events[i].data.fd == fd) {
                    if(events[i].events & EPOLLIN) {
                        // READ MESSAGE FROM NETWORK
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
                                    throw runtime_error("Too long message received.");
                                }
                                if(current_message_length == 0) {
                                    cout << endl;
                                }
                            }
                        } else {
                            // Read body
                            unsigned char buff[current_message_length];
                            int red = read(fd, buff, current_message_length);
                            if(red < 0) {
                                throw runtime_error("Reading body failed");
                            }
                            // Chenges to:
                            /*
                             * Czy wiadomości zakończone znakiem \0 lub \n są uważane za błędne?
                             * Odp. W definicji protokołu nie ma ograniczenia na to jakie znaki mogą pojawiać się
                             * wewnątrz wiadomości, więc wiadomość taka jest uznawana za poprawną.
                             */
                            /*for(int i = 0; i < red; i++) {
                                if(buff[i] == 0 || buff[i] == '\n') {
                                    throw runtime_error("Got invalid message.");
                                }
                            }*/
                            copy(buff, buff+red, back_inserter(read_buffer));
                            current_message_length -= red;
                            if(current_message_length <= 0) {
                                cout << string(read_buffer.begin(), read_buffer.end()) << endl;
                                read_buffer.clear();
                            }

                        }
                    }
                    if(events[i].events & EPOLLOUT) {
                        // WRITE MESSAGE TO NETWORK
                        send();
                    }
                    if(events[i].events & EPOLLRDHUP) {
                        throw runtime_error("Server shouldn't close the connection.");
                    }
                }
            }
            if(!hasinput && send_buffer.empty()) {
                working = false;
            }
        }
    }

    void stop() {
        working = false;
    }

private:
    void setup_network(std::string host, std::string service) {
        struct addrinfo hints, *result;
        std::memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = 0;
        hints.ai_protocol = 0;

        if(::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0) {
            throw std::runtime_error("GetAddrInfo failed.");
        }

        fd = -1;
        for(auto i = result; i != nullptr; i = i->ai_next) {
            fd = ::socket(i->ai_family, i->ai_socktype, i->ai_protocol);
            if(fd == -1)
                continue;

            if(::connect(fd, i->ai_addr, i->ai_addrlen) != -1)
                break;

            ::close(fd);
            fd = -1;
        }
        ::freeaddrinfo(result);

        if(fd == -1) {
            throw std::runtime_error("Host unreachable.");
        }
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
    string host = "localhost";
    string port = "20160";
    if(argc < 2 || argc > 3) {
        cerr << argv[0] << " host [port]" << endl;
        return 1;
    }

    host = argv[1];
    if(argc > 2) {
        port = argv[2];
    }
    try {
        Client client(host, port);
        client.work();
    } catch(runtime_error &e) {
        cerr << "Exception message: " << e.what() << endl;
        if(errno > 0) {
            cerr << "Error number (errno): " << errno << " [" << strerror(errno) << "]" << endl;
        }
        return 100;
    }
    return 0;
}
