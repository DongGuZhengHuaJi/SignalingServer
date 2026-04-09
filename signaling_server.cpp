#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <set>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <future>
#include "thread_pool.hpp"
#include "redis_mgr.h"

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
    SignalingServer(net::io_context& ioc, short port);

    void start();
    void stop();
    void start_accept();

    void register_user(const std::string& id, std::shared_ptr<Session> session);
    void remove_user(const std::string& id);
    void deliver(const std::string& target_id, const nlohmann::json& msg);
    void broadcast(const std::string& room_id, const nlohmann::json& msg, std::shared_ptr<Session> exclude_session = nullptr);

    void create_room(const std::string& room_id, std::shared_ptr<Session> session);
    void join_room(const std::string& room_id, std::shared_ptr<Session> session);
    void leave_room(const std::string& room_id, std::shared_ptr<Session> session);

    net::io_context& get_io_context();

private:
    net::io_context& _ioc;
    tcp::acceptor _acceptor;
    std::recursive_mutex _map_mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> _sessions;
    std::unordered_map<std::string, std::set<std::shared_ptr<Session>>> _rooms;
};

// 3. 定义 Session 类
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::shared_ptr<SignalingServer> server);

    void start();

    void send_json(const nlohmann::json& msg);

    std::string get_session_id() const;

    void set_current_room(const std::string& room_id);
    std::string get_current_room() const;

private:
    void on_accept(beast::error_code ec);

    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void handle_read(const nlohmann::json& data);
    
    void queue_write(std::string msg);
    void do_write();
    
    void cleanup();
    void close_socket(websocket::close_code code = websocket::close_code::policy_error);

    websocket::stream<beast::tcp_stream> _ws;
    beast::flat_buffer _buffer;
    std::shared_ptr<SignalingServer> _server;
    std::string _session_id;
    std::string _current_room;
    std::queue<std::string> _write_queue;
};

// --- 4. 定义成员函数实现 ---


// --- SignalingServer 类实现 ---
SignalingServer::SignalingServer(net::io_context& ioc, short port)
    : _ioc(ioc), _acceptor(ioc, tcp::endpoint(tcp::v4(), port)) {}

void SignalingServer::start() {
    start_accept();
}

void SignalingServer::stop() {
    _ioc.stop();
}

net::io_context& SignalingServer::get_io_context() {
    return _ioc;
}

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
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    _sessions[id] = session;
    session->send_json({{"type", "register_success"}, {"session_id", id}});
}

void SignalingServer::remove_user(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    _sessions.erase(id);
}

void SignalingServer::deliver(const std::string& target_id, const nlohmann::json& msg) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    if (_sessions.contains(target_id)) {
        _sessions[target_id]->send_json(msg);
    }
}

void SignalingServer::broadcast(const std::string& room_id, const nlohmann::json& msg, std::shared_ptr<Session> exclude_session) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    if (_rooms.contains(room_id)) {
        for (auto& session : _rooms[room_id]) {
            if (session != exclude_session) {
                session->send_json(msg);
            }
        }
    }
}

void SignalingServer::create_room(const std::string& room_id, std::shared_ptr<Session> session) {
    if(room_id.empty()) {
        session->send_json({{"type", "error"}, {"message", "Room ID cannot be empty"}});
        std::cerr << "Failed to create room: Room ID cannot be empty" << std::endl;
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    if(!_rooms.contains(room_id)) {
        _rooms[room_id].insert(session);
        session->set_current_room(room_id);
        std::cout << "Room Created: " << room_id << std::endl;
        session->send_json({{"type", "create_room_success"}, {"room_id", room_id}});
    }
    else{
        session->send_json({{"type", "error"}, {"message", "Room already exists"}});
        std::cerr << "Room already exists: " << room_id << std::endl;
    }
}

void SignalingServer::join_room(const std::string& room_id, std::shared_ptr<Session> session) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    if(_rooms.contains(room_id) && !_rooms[room_id].contains(session)) {
        _rooms[room_id].insert(session);
        session->set_current_room(room_id);
        broadcast(room_id, {{"type", "user_joined"}, {"session_id", session->get_session_id()}}, session);
        session->send_json({{"type", "join_room_success"}, {"room_id", room_id}});
        std::cout << "User " << session->get_session_id() << " joined room " << room_id << std::endl;
    }
    else if(!_rooms.contains(room_id)) {
        session->send_json({{"type", "error"}, {"message", "Room does not exist"}});
        std::cout << "User " << session->get_session_id() << " failed to join non-existent room " << room_id << std::endl;
    }
    else if(_rooms[room_id].contains(session)) {
        session->send_json({{"type", "error"}, {"message", "Already in the room"}});
        std::cout << "User " << session->get_session_id() << " is already in room " << room_id << std::endl;
    }
    else{
        session->send_json({{"type", "error"}, {"message", "Unknown error joining room"}});
        std::cerr << "Unknown error: User " << session->get_session_id() << " failed to join room " << room_id << std::endl;
    }
}

void SignalingServer::leave_room(const std::string& room_id, std::shared_ptr<Session> session) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    if (_rooms.contains(room_id)) {
        _rooms[room_id].erase(session);
        if (!_rooms[room_id].empty()) {
            broadcast(room_id, {{"type", "leave"}, {"session_id", session->get_session_id()}}, session);
        }
        else{
            _rooms.erase(room_id);
            std::cout << "Room " << room_id << " deleted as it became empty." << std::endl;
        }
    }
}

// --- Session 类实现 ---
Session::Session(tcp::socket socket, std::shared_ptr<SignalingServer> server)
    : _ws(std::move(socket)), _server(server) {}

void Session::start() {
    _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    _ws.async_accept(beast::bind_front_handler(&Session::on_accept, shared_from_this()));
}

void Session::send_json(const nlohmann::json& msg) {
    std::string msg_str = msg.dump();
    net::post(_ws.get_executor(), [self = shared_from_this(), msg_str]() {
        self->queue_write(std::move(msg_str));
    });
}

std::string Session::get_session_id() const {
    return _session_id;
}

void Session::set_current_room(const std::string& room_id) {
    _current_room = room_id;
}

std::string Session::get_current_room() const {
    return _current_room;
}

void Session::on_accept(beast::error_code ec) {
    if (ec) return;
    do_read();
}

void Session::do_read() {
    _ws.async_read(_buffer, beast::bind_front_handler(&Session::on_read, shared_from_this()));
}

void Session::queue_write(std::string msg) {
    bool write_in_progress = !_write_queue.empty();
    _write_queue.push(std::move(msg));
    if (!write_in_progress) {
        do_write();
    }
}

void Session::do_write() {
    if (_write_queue.empty()) return;

    _ws.async_write(net::buffer(_write_queue.front()), 
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                self->cleanup();
                return;
            }
            self->_write_queue.pop();
            if (!self->_write_queue.empty()) {
                self->do_write();
            }
        });
}


void Session::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) { cleanup(); return; }
    if (ec) { cleanup(); return; }

    try {
        std::string msg = beast::buffers_to_string(_buffer.data());
        handle_read(nlohmann::json::parse(msg));
    } catch (std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }

    _buffer.consume(bytes_transferred);
    do_read();
}

void Session::close_socket(websocket::close_code code) {
    beast::error_code close_ec;
    _ws.close(code, close_ec);
    if (close_ec && close_ec != websocket::error::closed) {
        std::cerr << "WebSocket close error: " << close_ec.message() << std::endl;
    }
}

void Session::handle_read(const nlohmann::json& data) {

    // std::cout << "[Debug] Received JSON: " << data.dump() << std::endl;


    std::string type = data.value("type", "unknown");
    std::string token = data.value("access_token", data.value("token", ""));
    if (token.empty()) {
        std::cerr << "Missing token in message" << std::endl;
        send_json({{"type", "error"}, {"message", "Missing token"}});
        cleanup();
        close_socket();
        return;
    }
    
    RedisManager& redis = RedisManager::getInstance();
    std::string token_id;
    bool access_hit = redis.get("access:" + token, token_id) && !token_id.empty();

    if(!access_hit) {
        std::cerr << "Invalid or expired token: " << token << std::endl;
        send_json({{"type", "error"}, {"message", "Invalid or expired token"}});
        cleanup();
        close_socket();
        return;
    }
    if(!_session_id.empty() && _session_id != token_id) {
        std::cerr << "Token does not match session_id: " << token_id << " vs " << _session_id << std::endl;
        send_json({{"type", "error"}, {"message", "Token does not match session_id"}});
        cleanup();
        close_socket();
        return;
    }
    if(type != "register" && _session_id.empty()) {
        std::cerr << "Unauthenticated message type before register: " << type << std::endl;
        send_json({{"type", "error"}, {"message", "Must register before sending business messages"}});
        cleanup();
        close_socket();
        return;
    }

    if (type == "register") {
        if(token_id == data.value("session_id", "")) {
            _session_id = token_id;
            _server->register_user(_session_id, shared_from_this());
            std::cout << "User Registered: " << _session_id << std::endl;   
        }
        else{
            std::cerr << "Token does not match session_id: " << token_id << " vs " << data.value("session_id", "") << std::endl;
            send_json({{"type", "error"}, {"message", "Token does not match session_id"}});
            cleanup();
            close_socket();
            return;
        }
    } else if (type == "offer" || type == "answer" || type == "candidate") {
        std::string to = data.value("to", "");
        _server->deliver(to, data);
    }
    else if (type == "join") {
        std::string room = data.value("room", "");
        _server->join_room(room, shared_from_this());
    }
    else if (type == "leave") {
        std::string room = data.value("room", "");
        _server->leave_room(room, shared_from_this());
    }
    else if (type == "create") {
        std::string room = data.value("room", "");
        _server->create_room(room, shared_from_this());
    }
    else if (type == "media_state") {
        std::string room = data.value("room", "");
        _server->broadcast(room, data, shared_from_this());
    }
    else{
        std::cerr << "Unknown message type: " << type << std::endl;
        std::cerr << "Full message: " << data.dump() << std::endl;
    }
}

void Session::cleanup() {
    if (!_session_id.empty()) {
        _server->remove_user(_session_id);
    }
    if(!_current_room.empty()) {
        _server->leave_room(_current_room, shared_from_this());
    }
    std::cout << "User Offline: " << _session_id << std::endl;
}


// --- 5. 主函数 ---
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