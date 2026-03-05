#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>

bool keep_running = true;

void log(const std::string &message)
{
    std::cout << message << std::endl;
}

void log_socket_error(int fd) {
    std::ostringstream ss;
    int error = 0;
    socklen_t errlen = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0)
        ss << "Error occured on a socket: " << error << ".";
    else
        ss << "Error occured on a socket, but unable to get it: " << strerror(errno);
    log(ss.str());
}

struct SyncWait {
public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return ready; });
        ready = false;
    }

    void resume() {
        std::lock_guard<std::mutex> lock(mutex);
        ready = true;
        cv.notify_all();
    }

private:
    bool ready { false };
    std::mutex mutex;
    std::condition_variable cv;
};

struct SyncIO {
public:
    std::coroutine_handle<void> get_coro() {
        std::unique_lock<std::mutex> lock(mutex);
        return std::exchange(coro, nullptr);
    }

    void put_coro(std::coroutine_handle<void> handle) {
        std::unique_lock<std::mutex> lock(mutex);
        coro = handle;
    }

private:
    std::mutex mutex;
    std::coroutine_handle<void> coro;
};

class ThreadPool {
public:
    ThreadPool() {
        for (int i = 0; i < 8; i++)
            pool.push_back(std::thread([this] { run_thread(); }));
    }

    ~ThreadPool() {
        // Let it go!
        for (int i = 0; i < pool.size(); i++)
            put_item(nullptr);
        for (std::thread &thread : pool)
            thread.join();
    }

    std::coroutine_handle<void> get_item() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return !queue.empty(); });
        std::coroutine_handle<void> item = queue.front();
        queue.pop();
        return item;
    }

    void put_item(std::coroutine_handle<void> item) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(item);
        cv.notify_one();
    }

    void run_thread() {
        while (true) {
            std::coroutine_handle<void> item = get_item();
            if (item == nullptr)
                break;
            item();
        }
    }

private:
    std::vector<std::thread> pool;
    std::queue<std::coroutine_handle<void>> queue;
    std::mutex mutex;
    std::condition_variable cv;
};

struct WorkerSync {
    WorkerSync() : read(std::make_shared<SyncIO>()), write(std::make_shared<SyncIO>()) {}
    std::shared_ptr<SyncIO> read, write;
    std::atomic<bool> error { false };
};

void exit_with_error(const std::string &errorMessage)
{
    log("ERROR: " + errorMessage);
    keep_running = false;
    exit(1);
}

class EpollTracker {
public:
    EpollTracker(std::shared_ptr<ThreadPool> pool) : thread_pool(pool) {
        epoll_fd = epoll_create1(0);
        if (epoll_fd < 0)
            exit_with_error("Unable to create epoll instance");

        epoll_thread = std::thread([this] { run_epoll(); });
    }

    ~EpollTracker() {
        epoll_thread.join();
        close(epoll_fd);
    }

    // Make socket non-blocking and add it to the epoll interest list
    std::shared_ptr<WorkerSync> register_socket(int fd) {
        std::shared_ptr<WorkerSync> ws = std::make_shared<WorkerSync>();
        {
            std::unique_lock<std::mutex> lock(epoll_mutex);
            auto [it, ok] = registrar.try_emplace(fd, std::move(ws));
            ws = it->second;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            exit_with_error("Unable to get fd's flags");

        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
            exit_with_error("Unable to set O_NONBLOCK on the fd.");

        struct epoll_event accept_event;
        accept_event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        accept_event.data.fd = fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &accept_event) < 0)
            exit_with_error("Unable to add new socket to the epoll interest list");

        return ws;
    }

    // Remove socket from the epoll interest list and close it
    void unregister_socket(int fd) {
        {
            std::unique_lock<std::mutex> lock(epoll_mutex);
            registrar.erase(fd);
        }

        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, (struct epoll_event *)nullptr) < 0)
            exit_with_error("Unable to deregister fd from the epoll interest list");

        close(fd);
    }

private:
    void run_epoll() {
        struct epoll_event events[1024];
        while (keep_running) {
            int fd_count = epoll_wait(epoll_fd, events, sizeof(events) / sizeof(epoll_event), 10);
            if (fd_count < 0) {
                log("Waititing for epoll_events failed");
                keep_running = false;
                break;
            }

            for (int i = 0; i < fd_count && keep_running; i++) {
                epoll_event &event = events[i];
                int ev = event.events;
                int fd = event.data.fd;
                std::unique_lock<std::mutex> lock(epoll_mutex);
                auto it = registrar.find(fd);
                if (it == registrar.end())
                    continue;
                std::shared_ptr<WorkerSync> &ws = it->second;
                if (ev & EPOLLERR) {
                    // Don't wait anymore, and stop running
                    ws->error = true;
                    release(ws);
                    log_socket_error(fd);
                    continue;
                }
                if (ev & EPOLLIN)
                    resume(ws->read);
                if (ev & EPOLLOUT)
                    resume(ws->write);
            }
        }

        // Let it go!
        std::unique_lock<std::mutex> lock(epoll_mutex);
        for (auto &p : registrar)
            release(p.second);
    }

    void release(std::shared_ptr<WorkerSync> &ws) {
        resume(ws->read);
        resume(ws->write);
    }

    void resume(std::shared_ptr<SyncIO> &sw) {
        std::coroutine_handle<void> coro = sw->get_coro();
        if (coro != nullptr)
            thread_pool->put_item(coro);
    }

    int epoll_fd;
    std::mutex epoll_mutex;
    std::unordered_map<int, std::shared_ptr<WorkerSync>> registrar;
    std::thread epoll_thread;
    std::shared_ptr<ThreadPool> thread_pool;
};

class TrackerSync {
public:
    TrackerSync(std::shared_ptr<EpollTracker> et, int fd) : tracker(et), socket_fd(fd) {
        ws = tracker->register_socket(socket_fd);
    }

    ~TrackerSync() {
        tracker->unregister_socket(socket_fd);
    }

    std::shared_ptr<WorkerSync> get() {
        return ws;
    }

private:
    std::shared_ptr<WorkerSync> ws;
    std::shared_ptr<EpollTracker> tracker;
    int socket_fd;
};

struct VoidTask {
    struct promise_type {
        VoidTask get_return_object() {
            return VoidTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        void unhandled_exception() noexcept {}
        void return_void() noexcept {}
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct FinalAwaitable {
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> h) noexcept { h.destroy(); }
            void await_resume() const noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }
    };
    std::coroutine_handle<promise_type> handle;
};

class WaitIO {
public:
    WaitIO(std::shared_ptr<SyncIO> sio) : sync_io(sio) {}

    constexpr bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) {
        sync_io->put_coro(handle);
    }

    constexpr void await_resume() const noexcept {}

private:
    std::shared_ptr<SyncIO> sync_io;
};

VoidTask run_worker(std::shared_ptr<TrackerSync> ts, int fd) {
    std::shared_ptr<WorkerSync> socket_ws = ts->get();

    //log("------ Accepted new connection ------");

    // Read request
    const int BUFFER_SIZE = 30720;
    char buffer[BUFFER_SIZE] = {0};
    const char *ptr = buffer;
    int bytes_left = BUFFER_SIZE;
    while (bytes_left > 0 && keep_running) {
        int bytes_received = read(fd, (void *)ptr, bytes_left);
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await WaitIO(socket_ws->read);
                if (!keep_running)
                    co_return;
                if (!socket_ws->error)
                    continue;
            }

            log("Failed to read bytes from client socket connection");
            co_return;
        }

        ptr += bytes_received;
        bytes_left -= bytes_received;
        break;
    }

    //log("------ Received request from client ------");

    std::string htmlFile = "<!DOCTYPE html><html lang=\"en\"><body><h1> HOME </h1><p> Hello from your Server :) </p></body></html>\n";
    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\nConnection: close\nContent-Type: text/html\nContent-Length: " << htmlFile.size() << "\n\n"
       << htmlFile;
    std::string server_message = ss.str();

    // Send response
    ptr = server_message.c_str();
    bytes_left = server_message.size();
    while (bytes_left > 0 && keep_running) {
        long bytes_sent = write(fd, ptr, bytes_left);
        if (bytes_sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await WaitIO(socket_ws->write);
                if (!keep_running)
                    co_return;
                if (!socket_ws->error)
                    continue;
            }

            log("Error sending response to client");
            co_return;
        }

        ptr += bytes_sent;
        bytes_left -= bytes_sent;
    }

    //log("------ Response sent to client ------");
    co_return;
}

VoidTask run_server(std::shared_ptr<EpollTracker> tracker, std::shared_ptr<ThreadPool> thread_pool, std::shared_ptr<SyncWait> quit_sync) {
    // Start server
    int in_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (in_socket < 0)
        exit_with_error("Cannot create socket");

    int opt = 1;
    if (setsockopt(in_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        exit_with_error("Unable to set EADDRINUSE on a socket");

    struct sockaddr_in sock_addr;
    unsigned int sock_addr_len = sizeof(sock_addr);
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(8080);
    sock_addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    if (bind(in_socket, (sockaddr *)&sock_addr, sock_addr_len) < 0)
        exit_with_error("Cannot connect socket to address");

    // Start listen
    if (listen(in_socket, 1024) < 0)
        exit_with_error("Socket listen failed");

    TrackerSync accept_ts(tracker, in_socket);
    std::shared_ptr<WorkerSync> accept_ws = accept_ts.get();

    std::ostringstream ss;
    ss << "*** Listening on ADDRESS: " << inet_ntoa(sock_addr.sin_addr) << " PORT: " << ntohs(sock_addr.sin_port) << " ***";
    log(ss.str());

    while (keep_running) {
        int new_socket = accept(in_socket, (sockaddr *)&sock_addr, &sock_addr_len);
        if (new_socket < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await WaitIO(accept_ws->read);
                if (!keep_running)
                    break;
                if (!accept_ws->error)
                    continue;
            }

            ss = std::ostringstream();
            ss << "Server failed to accept incoming connection from ADDRESS: " << inet_ntoa(sock_addr.sin_addr) << "; PORT: " << ntohs(sock_addr.sin_port);
            log(ss.str());
        }
        else
            thread_pool->put_item(run_worker(std::make_shared<TrackerSync>(tracker, new_socket), new_socket).handle);
    }

    quit_sync->resume();
    co_return;
}

int main() {
    if (std::signal(SIGINT, [](int) -> void { keep_running = false; }) == SIG_ERR)
        exit_with_error("Unable to register signal handler");

    std::shared_ptr<ThreadPool> thread_pool = std::make_shared<ThreadPool>();
    std::shared_ptr<EpollTracker> tracker = std::make_shared<EpollTracker>(thread_pool);
    std::shared_ptr<SyncWait> quit_sync = std::make_shared<SyncWait>();

    thread_pool->put_item(run_server(tracker, thread_pool, quit_sync).handle);

    quit_sync->wait();

    return 0;
}

