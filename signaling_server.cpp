#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <set>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>
#include <future>
#include "thread_pool.hpp"
#include "redis_mgr.h"

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

namespace {
constexpr const char* kReservationEventChannel = "meeting:reservation_events";
constexpr const char* kRoomClosedEventChannel = "meeting:room_closed_events";
}


// 1. 前向声明两个类
class Session;
class SignalingServer;

struct Room{
    std::string room_id;
    std::string meeting_type; // quick | reserved
    std::set<std::string> participants; // 存储用户ID
    std::set<std::shared_ptr<Session>> current_sessions; // 存储用户的Session对象
    std::string host_id; // 房主ID
    std::chrono::system_clock::time_point start_time; // 预约时间
    std::chrono::system_clock::time_point empty_time; // 变空的时间点
    Room(std::string room_id_, std::chrono::system_clock::time_point start_time_ = std::chrono::system_clock::now())
        : room_id(std::move(room_id_)), meeting_type("quick"), start_time(start_time_) {}
};

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

    void create_room(const std::string& room_id, std::shared_ptr<Session> session,
                     std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now());
    void create_reserved_room(const std::string& room_id, const std::string& host_id,
                              std::chrono::system_clock::time_point start_time);
    void join_room(const std::string& room_id, std::shared_ptr<Session> session);
    void leave_room(const std::string& room_id, std::shared_ptr<Session> session, bool end_meeting = false);

    void remove_room(const std::string& room_id, const std::string& reason = "empty_timeout");
    void remove_empty_rooms();


    net::io_context& get_io_context();

private:
    net::io_context& _ioc;
    tcp::acceptor _acceptor;
    std::recursive_mutex _map_mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> _sessions;
    std::unordered_map<std::string, Room> _rooms;
    std::unordered_map<std::string, std::chrono::system_clock::time_point> _empty_rooms;
    std::unique_ptr<net::steady_timer> _empty_room_timer;
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
    : _ioc(ioc), _acceptor(ioc, tcp::endpoint(tcp::v4(), port)), _empty_room_timer(std::make_unique<net::steady_timer>(_ioc)) {}

void SignalingServer::start() {
    start_accept();

    // 启动定时器定期清理空房间
    remove_empty_rooms();
}

void SignalingServer::stop() {
    if (_empty_room_timer) {
        boost::system::error_code ec;
        _empty_room_timer->cancel(ec);
    }
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
    auto it = _sessions.find(target_id);
    if (it != _sessions.end()) {
        it->second->send_json(msg);
    }
}

void SignalingServer::broadcast(const std::string& room_id, const nlohmann::json& msg, std::shared_ptr<Session> exclude_session) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto room_it = _rooms.find(room_id);
    if (room_it != _rooms.end()) {
        for (auto& session : room_it->second.current_sessions) {
            if (session != exclude_session) {
                session->send_json(msg);
            }
        }
    }
}

void SignalingServer::create_room(const std::string& room_id, std::shared_ptr<Session> session,
                                  std::chrono::system_clock::time_point start_time) {
    if(room_id.empty()) {
        session->send_json({{"type", "error"}, {"message", "Room ID cannot be empty"}});
        std::cerr << "Failed to create room: Room ID cannot be empty" << std::endl;
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    if(_rooms.find(room_id) == _rooms.end()) {
        Room room(room_id, start_time);
        room.room_id = room_id;
        room.meeting_type = "quick";
        room.host_id = session->get_session_id();
        room.current_sessions.insert(session);
        if (!room.host_id.empty()) {
            room.participants.insert(room.host_id);
        }
        _rooms.emplace(room_id, std::move(room));
        session->set_current_room(room_id);
        std::cout << "Room Created: " << room_id << std::endl;
        session->send_json({{"type", "create_room_success"}, {"room_id", room_id}});
    }
    else{
        session->send_json({{"type", "error"}, {"message", "Room already exists"}});
        std::cerr << "Room already exists: " << room_id << std::endl;
    }
}

void SignalingServer::create_reserved_room(const std::string& room_id, const std::string& host_id,
                                           std::chrono::system_clock::time_point start_time) {
    if (room_id.empty()) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto room_it = _rooms.find(room_id);
    if (room_it == _rooms.end()) {
        Room room(room_id, start_time);
        room.meeting_type = "reserved";
        room.host_id = host_id;
        if (!host_id.empty()) {
            room.participants.insert(host_id);
        }
        _rooms.emplace(room_id, std::move(room));
        std::cout << "Reserved room prepared: " << room_id << std::endl;
        return;
    }

    room_it->second.start_time = start_time;
    room_it->second.meeting_type = "reserved";
    if (!host_id.empty()) {
        room_it->second.host_id = host_id;
        room_it->second.participants.insert(host_id);
    }
}

void SignalingServer::join_room(const std::string& room_id, std::shared_ptr<Session> session) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto room_it = _rooms.find(room_id);
    if(room_it != _rooms.end() && room_it->second.current_sessions.find(session) == room_it->second.current_sessions.end()) {
        if(room_it->second.start_time>std::chrono::system_clock::now()) {
            session->send_json({{"type", "error"}, {"message", "Cannot join room before start time"}});
            std::cerr << "User " << session->get_session_id() << " cannot join room " << room_id << " before start time" << std::endl;
            return;
        }
        room_it->second.current_sessions.insert(session);
        if (!session->get_session_id().empty()) {
            room_it->second.participants.insert(session->get_session_id());
        }
        session->set_current_room(room_id);
        if(_empty_rooms.find(room_id) != _empty_rooms.end()) {
            room_it->second.empty_time = std::chrono::system_clock::time_point(); // 重置变空时间
            _empty_rooms.erase(room_id);
        }
        broadcast(room_id, {{"type", "user_joined"}, {"session_id", session->get_session_id()}}, session);
        session->send_json({{"type", "join_room_success"}, {"room_id", room_id}});
        std::cout << "User " << session->get_session_id() << " joined room " << room_id << std::endl;
    }
    else if(room_it == _rooms.end()) {
        session->send_json({{"type", "error"}, {"message", "Room does not exist"}});
        std::cout << "User " << session->get_session_id() << " failed to join non-existent room " << room_id << std::endl;
    }
    else if(room_it->second.current_sessions.find(session) != room_it->second.current_sessions.end()) {
        session->send_json({{"type", "error"}, {"message", "Already in the room"}});
        std::cout << "User " << session->get_session_id() << " is already in room " << room_id << std::endl;
    }
    else{
        session->send_json({{"type", "error"}, {"message", "Unknown error joining room"}});
        std::cerr << "Unknown error: User " << session->get_session_id() << " failed to join room " << room_id << std::endl;
    }
}

void SignalingServer::leave_room(const std::string& room_id, std::shared_ptr<Session> session, bool end_meeting) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto room_it = _rooms.find(room_id);
    if (room_it != _rooms.end()) {
        const std::string leaving_id = session->get_session_id();
        const bool host_left = !leaving_id.empty() && leaving_id == room_it->second.host_id;

        room_it->second.current_sessions.erase(session);

        if (host_left && end_meeting) {
            remove_room(room_id, "host_end");
            return;
        }

        //todo: 可以考虑如果房主离开但会议未结束，自动转移房主身份给其他参与者
        // if (host_left && !room_it->second.current_sessions.empty()) {
        //     std::string new_host;
        //     for (const auto& sess : room_it->second.current_sessions) {
        //         const std::string candidate = sess->get_session_id();
        //         if (!candidate.empty()) {
        //             new_host = candidate;
        //             break;
        //         }
        //     }
        //     room_it->second.host_id = new_host;
        // }

        if (!room_it->second.current_sessions.empty()) {
            broadcast(room_id, {{"type", "user_left"}, {"session_id", leaving_id}}, session);
        }
        else{
            room_it->second.empty_time = std::chrono::system_clock::now();
            _empty_rooms[room_id] = room_it->second.empty_time;
            std::cout << "Room " << room_id << " marked as empty." << std::endl;
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
        const bool end_meeting = data.value("end_meeting", false);
        _server->leave_room(room, shared_from_this(), end_meeting);
    }
    else if (type == "create") {
        std::string room = data.value("room", "");
        _server->create_room(room, shared_from_this(), std::chrono::system_clock::now());
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
        _server->leave_room(_current_room, shared_from_this(), false);
    }
    std::cout << "User Offline: " << _session_id << std::endl;
}

void start_reservation_subscription(const std::shared_ptr<SignalingServer>& server) {
    std::thread([server]() {
        try {
            auto subscriber = RedisManager::getInstance().getClient().subscriber();

            subscriber.on_message([server](std::string channel, std::string msg) {
                if (channel != kReservationEventChannel) {
                    return;
                }

                // 信令服务端接收到预约事件后，解析消息并将通知发送给相关用户
                try {
                    const auto payload = nlohmann::json::parse(msg);
                    const std::string user_id = payload.value("user_id", "");
                    if (user_id.empty()) {
                        return;
                    }

                    // nlohmann::json push_msg = {
                    //     {"type", "reservation_notice"},
                    //     {"event", payload}
                    // };
                    // 给预约者返回预约通知
                    // server->deliver(user_id, push_msg);

                    const std::string room_id = payload.value("room", "");
                    const long long reserve_epoch = payload.value("time", 0LL);
                    std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now();
                    if (reserve_epoch > 0) {
                        start_time = std::chrono::system_clock::from_time_t(static_cast<std::time_t>(reserve_epoch));
                    }
                    server->create_reserved_room(room_id, user_id, start_time);
                } catch (const std::exception& e) {
                    std::cerr << "Reservation event parse error: " << e.what() << std::endl;
                }
            });

            subscriber.subscribe(kReservationEventChannel);
            while (true) {
                subscriber.consume();
            }
        } catch (const std::exception& e) {
            std::cerr << "Reservation subscription stopped: " << e.what() << std::endl;
        }
    }).detach();
}

void SignalingServer::remove_room(const std::string& room_id, const std::string& reason) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto room_it = _rooms.find(room_id);
    if (room_it == _rooms.end()) {
        return;
    }

    Room& room = room_it->second;

    for(auto session_id: room.participants) {
        if(_sessions.find(session_id) != _sessions.end()) {
            _sessions[session_id]->set_current_room("");
            _sessions[session_id]->send_json({
                {"type", "room_closed"},
                {"room_id", room.room_id},
                {"reason", reason},
                {"meeting_type", room.meeting_type}
            });
        }
    }
    _rooms.erase(room.room_id);
    if(_empty_rooms.find(room.room_id) != _empty_rooms.end()) {
        _empty_rooms.erase(room.room_id);
    }

    RedisManager& redis_mgr = RedisManager::getInstance();
    nlohmann::json close_event = {
        {"type", "room_closed"},
        {"room_id", room.room_id},
        {"reason", reason},
        {"meeting_type", room.meeting_type}
    };
    const long long subscriber_count = redis_mgr.getClient().publish(kRoomClosedEventChannel, close_event.dump());
    if (subscriber_count <= 0) {
        std::cerr << "No subscriber for room closed event of room " << room.room_id << std::endl;
    }
    
    std::cout << "Room removed: " << room.room_id << std::endl;
}

void SignalingServer::remove_empty_rooms() {
    _empty_room_timer->expires_after(std::chrono::minutes(5));
    _empty_room_timer->async_wait([server = shared_from_this()](const boost::system::error_code& ec) {
        if (ec) {
            std::cerr << "Timer error: " << ec.message() << std::endl;
            return;
        }

        std::vector<std::string> expired_room_ids;
        {
            std::lock_guard<std::recursive_mutex> lock(server->_map_mutex);
            const auto now = std::chrono::system_clock::now();
            for (const auto& [room_id, empty_time] : server->_empty_rooms) {
                if (now - empty_time > std::chrono::minutes(15)) {
                    expired_room_ids.push_back(room_id);
                }
            }
        }

        for (const auto& room_id : expired_room_ids) {
            server->remove_room(room_id, "empty_timeout");
            std::cout << "Deleted empty room: " << room_id << std::endl;
        }

        server->remove_empty_rooms(); // 继续设置下一次检查
    });
}


// --- 5. 主函数 ---
int main() {
    try {
        net::io_context ioc;
        auto server = std::make_shared<SignalingServer>(ioc, 8080);
        server->start();
        start_reservation_subscription(server);

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