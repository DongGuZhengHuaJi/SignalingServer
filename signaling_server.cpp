#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <future>
#include "thread_pool.hpp"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

// 1. 前向声明两个类
class Session;
class SignalingServer;

// 2. 定义 SignalingServer 类
class SignalingServer : public std::enable_shared_from_this<SignalingServer> {
public:
    SignalingServer(net::io_context& ioc, short port)
        : _ioc(ioc), _acceptor(ioc, tcp::endpoint(tcp::v4(), port)) {}

    void start() { start_accept(); }
    void stop() { _ioc.stop(); }
    void start_accept();

    void register_user(const std::string& id, std::shared_ptr<Session> session);
    void remove_user(const std::string& id);
    void deliver(const std::string& target_id, const nlohmann::json& msg);

    net::io_context& get_io_context() { return _ioc; }

private:
    net::io_context& _ioc;
    tcp::acceptor _acceptor;
    std::mutex _map_mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> _sessions;
};

// 3. 定义 Session 类
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<SignalingServer> server)
        : _ws(std::move(socket)), _server(server) {}

    void start() {
        _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        _ws.async_accept(beast::bind_front_handler(&Session::on_accept, shared_from_this()));
    }

    void send_json(const nlohmann::json& msg) {
        auto msg_str = std::make_shared<std::string>(msg.dump());
        net::post(_ws.get_executor(), [self = shared_from_this(), msg_str]() {
            self->_ws.async_write(net::buffer(*msg_str), 
                [self, msg_str](beast::error_code ec, std::size_t) {
                    if (ec) std::cerr << "Write error: " << ec.message() << std::endl;
                });
        });
    }

private:
    void on_accept(beast::error_code ec) {
        if (ec) return;
        do_read();
    }

    void do_read() {
        _ws.async_read(_buffer, beast::bind_front_handler(&Session::on_read, shared_from_this()));
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void handle_read(const nlohmann::json& data);
    void cleanup();

    websocket::stream<beast::tcp_stream> _ws;
    beast::flat_buffer _buffer;
    std::shared_ptr<SignalingServer> _server;
    std::string _session_id;
};

// --- 4. 定义成员函数实现 ---

void SignalingServer::start_accept() {
    _acceptor.async_accept(net::make_strand(_ioc), 
        [this, self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(std::move(socket), self);
                session->start();
            }
            start_accept();
        });
}

void SignalingServer::register_user(const std::string& id, std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lock(_map_mutex);
    _sessions[id] = session;
}

void SignalingServer::remove_user(const std::string& id) {
    std::lock_guard<std::mutex> lock(_map_mutex);
    _sessions.erase(id);
}

void SignalingServer::deliver(const std::string& target_id, const nlohmann::json& msg) {
    std::lock_guard<std::mutex> lock(_map_mutex);
    if (_sessions.contains(target_id)) {
        _sessions[target_id]->send_json(msg); // 此时编译器已经知道 send_json 是什么了
    }
}

void Session::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) { cleanup(); return; }
    if (ec) { cleanup(); return; }

    try {
        std::string msg = beast::buffers_to_string(_buffer.data());
        handle_read(nlohmann::json::parse(msg));
    } catch (...) {}

    _buffer.consume(bytes_transferred);
    do_read();
}

void Session::handle_read(const nlohmann::json& data) {
    std::string type = data.value("type", "unknown");
    if (type == "register") {
        _session_id = data.value("session_id", "");
        if (!_session_id.empty()) {
            _server->register_user(_session_id, shared_from_this());
            std::cout << "User Registered: " << _session_id << std::endl;
        }
    } else if (type == "offer" || type == "answer" || type == "candidate") {
        std::string to = data.value("to", "");
        _server->deliver(to, data);
    }
}

void Session::cleanup() {
    if (!_session_id.empty()) {
        _server->remove_user(_session_id);
        std::cout << "User Offline: " << _session_id << std::endl;
    }
}



int main() {
    try {
        net::io_context ioc;
        auto server = std::make_shared<SignalingServer>(ioc, 8080);
        server->start();

        std::promise<void> exit_signal;
        std::future<void> exit_future = exit_signal.get_future();

        // 信号处理
        net::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](auto, int) { 
            ioc.stop(); 
            exit_signal.set_value();
        });

        // 线程池运行
        auto& tp = ThreadPool::getInstance();
        for (int i = 0; i < std::thread::hardware_concurrency(); ++i) {
            tp.commit([&ioc] { ioc.run(); });
        }

        std::cout << "WebSocket Signaling Server running on port 8080..." << std::endl;
        std::cout << std::thread::hardware_concurrency() << " threads in the pool." << std::endl;
        std::cout << "Press Ctrl+C to stop the server." << std::endl;

        exit_future.wait(); // 等待退出信号

        std::cout << "Shutting down server..." << std::endl;
    } catch (boost::system::system_error& e) {
        std::cerr << "Boost System Error: " << e.what() << std::endl
                    << "Error Code: " << e.code() << std::endl;
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }
    return 0;
}