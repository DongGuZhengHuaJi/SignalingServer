#include "user_presence.h"
#include "connection.h"
#include "signaling_server.h" 
#include "redis_mgr.h"
#include <iostream>

UserPresence::UserPresence(std::shared_ptr<SignalingServer> server)
    : _server(server), _heartbeat_timer(server->get_io_context()), _hover_timer(server->get_io_context()), _status(UserStatus::ONLINE) {}

void UserPresence::init_connection(std::shared_ptr<Connection> connection) {
    _connection = connection;
    if (connection) {
        connection->bind_user(shared_from_this());
        // 从connection的websocket获取executor来初始化heartbeat_timer
        // 这需要在connection中添加一个获取executor的方法，暂时使用io_context
        
        // 设置消息回调
        connection->set_message_callback([self = shared_from_this()](const std::string& msg) {
            try {
                auto json_msg = nlohmann::json::parse(msg);
                self->handle_message(json_msg);
            } catch (const std::exception& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
        });
    }
}

const std::string& UserPresence::get_user_id() const {
    return _user_id;
}

void UserPresence::set_user_id(const std::string& user_id) {
    _user_id = user_id;
}

const std::string& UserPresence::get_self_name() const {
    return _self_name.empty() ? _user_id : _self_name;
}

void UserPresence::set_self_name(const std::string& self_name) {
    _self_name = self_name;
}

void UserPresence::set_current_room(const std::string& room_id) {
    _current_room = room_id;
}

std::string UserPresence::get_current_room() const {
    return _current_room;
}

void UserPresence::send_json(const nlohmann::json& msg) {
    auto conn = _connection.lock();
    if (conn) {
        conn->send_json(msg);
    }
}

std::shared_ptr<Connection> UserPresence::get_connection() const {
    return _connection.lock();
}

void UserPresence::set_connection(std::shared_ptr<Connection> connection) {
    _connection = connection;
}

void UserPresence::handle_message(const nlohmann::json& data) {
    if (!_server) {
        std::cerr << "Error: SignalingServer is not set" << std::endl;
        return;
    }

    std::string type = data.value("type", "unknown");

    // 心跳包单独处理：允许无 access_token，且支持双向 ping/pong。
    if (type == "ping") {
        reset_heartbeat();
        send_json({{"type", "pong"}});
        return;
    }
    if (type == "pong") {
        reset_heartbeat();
        return;
    }

    std::string token = data.value("access_token", data.value("token", ""));
    
    if (token.empty()) {
        std::cerr << "Missing token in message" << std::endl;
        send_json({{"type", "error"}, {"message", "Missing token"}});
        cleanup();
        auto conn = _connection.lock();
        if (conn) {
            conn->close_socket();
        }
        return;
    }
    
    RedisManager& redis = RedisManager::getInstance();
    std::string token_id;
    bool access_hit = redis.get("access:" + token, token_id) && !token_id.empty();

    if(!access_hit) {
        std::cerr << "Invalid or expired token: " << token << std::endl;
        send_json({{"type", "error"}, {"message", "Invalid or expired token"}});
        cleanup();
        auto conn = _connection.lock();
        if (conn) {
            conn->close_socket();
        }
        return;
    }
    
    if(!_user_id.empty() && _user_id != token_id) {
        std::cerr << "Token does not match user_id: " << token_id << " vs " << _user_id << std::endl;
        send_json({{"type", "error"}, {"message", "Token does not match user_id"}});
        cleanup();
        auto conn = _connection.lock();
        if (conn) {
            conn->close_socket();
        }
        return;
    }
    
    if(type != "register" && type != "reconnect" && _user_id.empty()) {
        std::cerr << "Unauthenticated message type before register: " << type << std::endl;
        send_json({{"type", "error"}, {"message", "Must register before sending business messages"}});
        cleanup();
        auto conn = _connection.lock();
        if (conn) {
            conn->close_socket();
        }
        return;
    }

    if (type == "register") {
        if(token_id == data.value("session_id", "")) {
            _user_id = token_id;
            const std::string self_name = data.value("self_name", _user_id);
            _self_name = self_name.empty() ? _user_id : self_name;
            _server->register_user(_user_id, _self_name, shared_from_this());
            std::cout << "User Registered: " << _user_id << std::endl;   

            start_heartbeat();
        }
        else{
            std::cerr << "Token does not match session_id: " << token_id << " vs " << data.value("session_id", "") << std::endl;
            send_json({{"type", "error"}, {"message", "Token does not match session_id"}});
            cleanup();
            auto conn = _connection.lock();
            if (conn) {
                conn->close_socket();
            }
            return;
        }
    } else if (type == "reconnect"){
        std::string reconnect_token = data.value("reconnect_token", "");
        if (reconnect_token.empty()) {
            std::cerr << "Missing reconnect_token for reconnect message" << std::endl;
            send_json({{"type", "error"}, {"message", "Missing reconnect_token"}});
            cleanup();
            auto conn = this->get_connection();
            if (conn) {                
                conn->close_socket();
            }            
            return;
        }
        _server->reconnect_user(reconnect_token, shared_from_this(), this->get_connection());
        std::cout << "User " << _user_id << " is reconnecting with token " << reconnect_token << std::endl;
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
        std::string meeting_type = data.value("meeting_type", "quick");
        _server->create_room(room, shared_from_this(), std::chrono::system_clock::now(), meeting_type);
    }
    else if (type == "media_state") {
        std::string room = data.value("room", "");
        _server->broadcast(room, data, shared_from_this());
    }
    else if (type == "chat") {
        std::string room = data.value("room", "");
        _server->broadcast(room, data, shared_from_this());
    }
    else{
        std::cerr << "Unknown message type: " << type << std::endl;
        std::cerr << "Full message: " << data.dump() << std::endl;
    }
}

void UserPresence::cleanup() {
    if (!_user_id.empty()) {
        _server->remove_user(_user_id);
    }
    if(!_current_room.empty()) {
        _server->leave_room(_current_room, shared_from_this(), false);
    }
    std::cout << "User Offline: " << _user_id << std::endl;
}

void UserPresence::start_heartbeat() {
    _last_heartbeat = std::chrono::system_clock::now();
    schedule_heartbeat();
}

void UserPresence::schedule_heartbeat() {
    auto conn = _connection.lock();
    if (!conn) return;
    _heartbeat_timer.expires_after(std::chrono::seconds(30));
    _heartbeat_timer.async_wait(net::bind_executor(conn->get_executor(),[self = shared_from_this()](const boost::system::error_code& ec) {
        if (ec) {
            return;
        }
        if(std::chrono::system_clock::now() - self->_last_heartbeat > std::chrono::seconds(40)) {
            std::cerr << "Heartbeat timeout for user " << self->_user_id << std::endl;
            
            // 修复：停止心跳，手动触发掉线保护，而不是直接 cleanup()
            self->stop_heartbeat();
            auto conn = self->_connection.lock();
            if (conn) {
                // 主动关闭 socket，这可能也会触发 on_read 报错，
                // 但我们在 on_connection_lost 里有防重入机制，所以安全。
                self->on_connection_lost(conn); 
                conn->close_socket();
            } else {
                // 如果连 connection 都没了，那才是彻底凉了
                self->cleanup();
            }
            return;
        }

        self->send_json({{"type", "ping"}});
        self->schedule_heartbeat();
    }));
}

void UserPresence::reset_heartbeat() {
    _last_heartbeat = std::chrono::system_clock::now();
}

void UserPresence::stop_heartbeat() {
    boost::system::error_code ec;
    _heartbeat_timer.cancel(ec);
    if (ec) {
        std::cerr << "Error stopping heartbeat timer: " << ec.message() << std::endl;
    }
}

void UserPresence::on_connection_lost(std::shared_ptr<Connection> conn) {
    if (conn != _connection.lock()) {
        std::cerr << "Connection lost callback for non-current connection" << std::endl;
        return;
    }

    _connection.reset();
    _status = UserStatus::RECONNECTING;
    std::cout << "Connection lost for user " << _user_id << ", waiting for reconnection..." << std::endl;

    _hover_timer.expires_after(std::chrono::seconds(60));
    _hover_timer.async_wait([self = weak_from_this()](beast::error_code ec) {
        if (ec) return;  // 被取消（重连成功）
        if (auto s = self.lock()) {
            if (!s->_current_room.empty()) {
                s->_server->broadcast(s->_current_room, {{"type", "user_connection_lost"}, {"from", s->_user_id}, {"from_name", s->_self_name}});
            }
            s->on_hover_timeout();
        }
    });


}

void UserPresence::on_hover_timeout() {
    if (_status == UserStatus::RECONNECTING) {
        std::cerr << "Hover timeout, marking user " << _user_id << " as offline" << std::endl;
        _status = UserStatus::OFFLINE;
        // 连接丢失后30秒内未重连成功，执行离线清理,包括从房间移除用户、通知其他用户等
        cleanup();
    }
}

void  UserPresence::on_connection_recovered() {
    _hover_timer.cancel();
    _status = UserStatus::ONLINE;
    std::cout << "Connection recovered for user " << _user_id << std::endl;
    // stop_heartbeat();
    start_heartbeat();

    // 重连成功以后恢复用户状态，通知房间内其他用户，其他用户客户端收到消息向该用户重新发送offer等必要信息
    if (!_current_room.empty()) {
        _server->broadcast(_current_room, {{"type", "user_reconnected"}, {"from", _user_id}, {"from_name", _self_name}}, shared_from_this());
    }
}