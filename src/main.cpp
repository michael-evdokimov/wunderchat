#include <unistd.h>
#include <cstring>
#include <chrono>
#include <unordered_map>
#include <arpa/inet.h>
#include <sys/epoll.h>


struct Client
{
    int clientfd = 0;
    int epollfd = 0;
    std::chrono::high_resolution_clock::time_point last = {};

    Client() = default;
    Client(const Client&) = delete;
    Client(Client&& o)
    {
        std::swap(clientfd, o.clientfd);
        std::swap(epollfd, o.epollfd);
        std::swap(last, o.last);
    }

    ~Client()
    {
        if (epollfd)
            epoll_ctl(epollfd, EPOLL_CTL_DEL, clientfd, nullptr);
        if (clientfd)
            close(clientfd);
    }

    int write(const char* msg, ssize_t len)
    {
        ssize_t r = ::write(clientfd, msg, len);
        if (r < 0)
            return printf("Could not write: %s\n", strerror(errno)), EINVAL;
        if (r != len)
            return printf("Could not write\n"), EINVAL;
        return 0;
    }

    template<size_t N>
    int write(const char (&msg)[N])
    {
        return write(msg, N - 1);
    }
};

struct Server
{
    int serverfd = 0;
    int epollfd = 0;
    std::unordered_map<int, Client> clients;

    ~Server()
    {
        clients.clear();
        if (epollfd)
            close(epollfd);
        if (serverfd)
            close(serverfd);
    }

    int open(const char* hostname, int port)
    {
        serverfd = socket(AF_INET, SOCK_STREAM, 0);
        if (!serverfd)
            return printf("Could not create socket: %s\n", strerror(errno)),
                                                                         EINVAL;
        int opt = 1;
        if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
            return printf("Could not set option reuse: %s\n", strerror(errno)),
                                                                         EINVAL;
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, hostname, &addr.sin_addr.s_addr) != 1)
            return printf("Could not bind addr <%s>: %s\n",
                                             hostname, strerror(errno)), EINVAL;
        if (bind(serverfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
            return printf("Could not bind: %s\n", strerror(errno)), EINVAL;

        if (listen(serverfd, 1))
            return printf("Could not listen: %s\n", strerror(errno)), EINVAL;

        epollfd = epoll_create(1);
        if (!epollfd)
            return printf("Could not create epoll: %s\n", strerror(errno)),
                                                                         EINVAL;
        epoll_event event = {};
        event.events = EPOLLIN;
        event.data.fd = serverfd;
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, serverfd, &event))
            return printf("Could not add to epoll: %s\n", strerror(errno)),
                                                                         EINVAL;
        return 0;
    }

    int work()
    {
        epoll_event event = {};
        int r = epoll_wait(epollfd, &event, 1, 10);
        if (r < 0)
            return printf("Could not epoll_wait: %s\n", strerror(errno)), EINVAL;
        if (r == 0)
            return 0;

        if (event.data.fd == serverfd)
            return accept();

        auto it = clients.find(event.data.fd);
        if (it == clients.end())
            return printf("Could not find client\n"), EFAULT;

        if (process(it->second) == drop)
            clients.erase(it);

        return 0;
    }

private:

    int accept()
    {
        Client client;
        client.clientfd = ::accept(serverfd, nullptr, nullptr);
        if (!client.clientfd)
            return printf("Could not accept new socket: %s\n", strerror(errno)),
                                                                         EINVAL;
        epoll_event event = {};
        event.events = EPOLLIN;
        event.data.fd = client.clientfd;
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, client.clientfd, &event))
            return printf("Could not add to epoll: %s\n", strerror(errno)),
                                                                         EINVAL;
        client.epollfd = epollfd;

        clients.emplace(client.clientfd, std::move(client));
        return 0;
    }

    enum action {drop, keep};

    action process(Client& client)
    {
        char buf[181];
        ssize_t len = read(client.clientfd, buf, sizeof(buf));
        if (len < 0)
            return printf("Could not read: %s\n", strerror(errno)), drop;
        if (len == 0)
            return drop;
        if (len == sizeof(buf))
            return printf("message overflow, dropped\n"),
                   client.write("message overflow\n"),
                   drop;
        if (!valid(buf, len))
            return client.write("forbidden symbols\n"), keep;

        std::chrono::high_resolution_clock::time_point now =
                std::chrono::high_resolution_clock::now();
        if (now - client.last < std::chrono::seconds(1))
            return client.write("flood is detected\n"), keep;
        client.last = now;

        for (auto i = clients.begin(); i != clients.end(); ) {
            if (&i->second != &client) {
                if (i->second.write(buf, len)) {
                    i = clients.erase(i);
                    printf("dropped\n");
                    continue;
                }
            }
            ++i;
        }

        return keep;
    }

    bool valid(const char* s, size_t len) const
    {
        for (size_t i = 0; i != len; ++i)
            if (0 <= s[i] && s[i] < 0x20 && s[i] != '\n' && s[i] != '\r')
                return false;
        return true;
    }
};

int main()
{
    Server server;
    if (int r = server.open("0.0.0.0", 8080))
        return printf("Could not open server: %s\n", strerror(r)), EINVAL;

    while (true)
        if (int r = server.work())
            return printf("Could not work: %s\n", strerror(r)), EINVAL;

    return 0;
}
