#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <set>
#include <mutex>
#include <unordered_map>
#include <random>
#include <sstream>
#include <nlohmann/json.hpp>
#include <future>
#include "thread_pool.hpp"
#include "redis_mgr.h"
#include "room_mgr.h"
#include "connection.h"
#include "user_presence.h"
#include "signaling_server.h"

namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;

namespace {
constexpr const char* kReservationEventChannel = "meeting:reservation_events";
constexpr const char* kRoomClosedEventChannel = "meeting:room_closed_events";

std::string generate_reconnect_token() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<unsigned long long> dis;

    std::ostringstream oss;
    oss << std::hex;
    oss << dis(gen) << dis(gen);
    return oss.str();
}
}
// --- 成员函数实现 ---


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
                auto conn = std::make_shared<Connection>(std::move(socket));
                auto user = std::make_shared<UserPresence>(self);
                user->init_connection(conn);
                conn->start();
            }
            start_accept();
        });
}

void SignalingServer::register_user(const std::string& id, const std::string& self_name, std::shared_ptr<UserPresence> user) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);

    RedisManager& redis = RedisManager::getInstance();
    auto old_token_it = _user_reconnect_tokens.find(id);
    if (old_token_it != _user_reconnect_tokens.end()) {
        redis.del("reconnect:" + old_token_it->second);
    }

    const std::string reconnect_token = generate_reconnect_token();
    if (!redis.set("reconnect:" + reconnect_token, id)) {
        user->send_json({{"type", "error"}, {"message", "Failed to create reconnect token"}});
        return;
    }

    _user_reconnect_tokens[id] = reconnect_token;
    _sessions[id] = user;
    user->set_self_name(self_name.empty() ? id : self_name);
    user->send_json({
        {"type", "register_success"},
        {"session_id", id},
        {"self_name", user->get_self_name()},
        {"reconnect_token", reconnect_token}
    });
}

void SignalingServer::remove_user(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    // 保留 reconnect_token：允许 session 被清理后仍可使用 reconnect 恢复。
    // token 会在下次 register_user 时被旋转并覆盖。
    _sessions.erase(id);
}

void SignalingServer::deliver(const std::string& target_id, const nlohmann::json& msg) {
    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto it = _sessions.find(target_id);
    if (it != _sessions.end()) {
        it->second->send_json(msg);
    }
}

void SignalingServer::broadcast(const std::string& room_id, const nlohmann::json& msg, std::shared_ptr<UserPresence> exclude_user) {
    const std::vector<std::string> session_ids = RoomManager::getInstance().getSessionIds(room_id);
    const std::string exclude_user_id = exclude_user ? exclude_user->get_user_id() : "";

    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    for (const auto& user_id : session_ids) {
        if (!exclude_user_id.empty() && user_id == exclude_user_id) {
            continue;
        }
        auto it = _sessions.find(user_id);
        if (it != _sessions.end()) {
            it->second->send_json(msg);
        }
    }
}



void SignalingServer::create_room(const std::string& room_id, std::shared_ptr<UserPresence> user,
                                  std::chrono::system_clock::time_point start_time,
                                  const std::string& meeting_type) {
    if(room_id.empty()) {
        user->send_json({{"type", "error"}, {"message", "Room ID cannot be empty"}});
        std::cerr << "Failed to create room: Room ID cannot be empty" << std::endl;
        return;
    }

    std::string normalized_type = meeting_type;
    if (normalized_type != "quick" &&
        normalized_type != "reserved" &&
        normalized_type != "screen_share") {
        normalized_type = "quick";
    }

    if (RoomManager::getInstance().createRoom(room_id, user->get_user_id(), start_time, normalized_type)) {
        user->set_current_room(room_id);
        std::cout << "Room Created: " << room_id << std::endl;
        user->send_json({
            {"type", "create_room_success"},
            {"room_id", room_id},
            {"is_host", true},
            {"host_id", user->get_user_id()},
            {"meeting_type", normalized_type}
        });
    } else {
        user->send_json({{"type", "error"}, {"message", "Room already exists"}});
        std::cerr << "Room already exists: " << room_id << std::endl;
    }
}

void SignalingServer::create_reserved_room(const std::string& room_id, const std::string& host_id,
                                           std::chrono::system_clock::time_point start_time) {
    if (room_id.empty()) {
        return;
    }

    const bool existed = RoomManager::getInstance().getRoomCopy(room_id).has_value();
    RoomManager::getInstance().createOrUpdateReservedRoom(room_id, host_id, start_time);
    if (!existed) {
        std::cout << "Reserved room prepared: " << room_id << std::endl;
    }
}

void SignalingServer::join_room(const std::string& room_id, std::shared_ptr<UserPresence> user) {
    const JoinRoomResult join_result = RoomManager::getInstance().addSession(room_id, user->get_user_id());
    if (join_result == JoinRoomResult::kOk) {
        auto room_opt = RoomManager::getInstance().getRoomCopy(room_id);
        if (!room_opt.has_value()) {
            user->send_json({{"type", "error"}, {"message", "Room state changed, retry join"}});
            return;
        }

        const Room room = room_opt.value();
        user->set_current_room(room_id);
        const bool joined_is_host = (!user->get_user_id().empty() && user->get_user_id() == room.host_id);
        broadcast(room_id, {
            {"type", "user_joined"},
            {"session_id", user->get_user_id()},
            {"from", user->get_user_id()},
            {"from_name", user->get_self_name()}
        }, user);
        user->send_json({
            {"type", "join_room_success"},
            {"room_id", room_id},
            {"is_host", joined_is_host},
            {"host_id", room.host_id},
            {"meeting_type", room.meeting_type}
        });
        std::cout << "User " << user->get_user_id() << " joined room " << room_id << std::endl;
    } else if (join_result == JoinRoomResult::kRoomNotFound) {
        user->send_json({{"type", "error"}, {"message", "Room does not exist"}});
        std::cout << "User " << user->get_user_id() << " failed to join non-existent room " << room_id << std::endl;
    } else if (join_result == JoinRoomResult::kBeforeStartTime) {
        user->send_json({{"type", "error"}, {"message", "Cannot join room before start time"}});
        std::cerr << "User " << user->get_user_id() << " cannot join room " << room_id << " before start time" << std::endl;
    } else if (join_result == JoinRoomResult::kAlreadyInRoom) {
        user->send_json({{"type", "error"}, {"message", "Already in the room"}});
        std::cout << "User " << user->get_user_id() << " is already in room " << room_id << std::endl;
    } else {struct Room {
    std::string room_id;
    std::string meeting_type; // quick | reserved | screen_share
    std::set<std::string> participants; // 存储用户ID
    std::set<std::shared_ptr<UserPresence>> current_sessions; // 存储用户的UserPresence对象
    std::string host_id; // 房主ID
    std::chrono::system_clock::time_point start_time; // 预约时间
    std::chrono::system_clock::time_point empty_time; // 变空的时间点
    Room(std::string room_id_, std::chrono::system_clock::time_point start_time_ = std::chrono::system_clock::now())
        : room_id(std::move(room_id_)), meeting_type("quick"), start_time(start_time_) {}
};
        user->send_json({{"type", "error"}, {"message", "Unknown error joining room"}});
        std::cerr << "Unknown error: User " << user->get_user_id() << " failed to join room " << room_id << std::endl;
    }
}

void SignalingServer::leave_room(const std::string& room_id, std::shared_ptr<UserPresence> user, bool end_meeting) {
    auto room_opt = RoomManager::getInstance().getRoomCopy(room_id);
    if (!room_opt.has_value()) {
        return;
    }

    const std::string leaving_id = user->get_user_id();
    const bool host_left = !leaving_id.empty() && leaving_id == room_opt->host_id;

    bool became_empty = false;
    if (!RoomManager::getInstance().removeSession(room_id, leaving_id, &became_empty)) {
        return;
    }

    user->set_current_room("");

    if (host_left && end_meeting) {
        remove_room(room_id, "host_end");
        return;
    }

    if (!became_empty) {
        broadcast(room_id, {{"type", "user_left"}, {"session_id", leaving_id}}, user);
    } else {
        std::cout << "Room " << room_id << " marked as empty." << std::endl;
    }
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
    Room room_snapshot("", std::chrono::system_clock::now());
    if (!RoomManager::getInstance().removeRoom(room_id, &room_snapshot)) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    for (const auto& session_id : room_snapshot.participants) {
        auto it = _sessions.find(session_id);
        if (it != _sessions.end()) {
            it->second->set_current_room("");
            it->second->send_json({
                {"type", "room_closed"},
                {"room_id", room_snapshot.room_id},
                {"reason", reason},
                {"meeting_type", room_snapshot.meeting_type}
            });
        }
    }

    RedisManager& redis_mgr = RedisManager::getInstance();
    nlohmann::json close_event = {
        {"type", "room_closed"},
        {"room_id", room_snapshot.room_id},
        {"reason", reason},
        {"meeting_type", room_snapshot.meeting_type}
    };
    const long long subscriber_count = redis_mgr.getClient().publish(kRoomClosedEventChannel, close_event.dump());
    if (subscriber_count <= 0) {
        std::cerr << "No subscriber for room closed event of room " << room_snapshot.room_id << std::endl;
    }
    
    std::cout << "Room removed: " << room_snapshot.room_id << std::endl;
}

void SignalingServer::remove_empty_rooms() {
    _empty_room_timer->expires_after(std::chrono::minutes(5));
    _empty_room_timer->async_wait([server = shared_from_this()](const boost::system::error_code& ec) {
        if (ec) {
            std::cerr << "Timer error: " << ec.message() << std::endl;
            return;
        }

        const std::vector<std::string> expired_room_ids = RoomManager::getInstance().listExpiredEmptyRooms(std::chrono::minutes(15));

        for (const auto& room_id : expired_room_ids) {
            server->remove_room(room_id, "empty_timeout");
            std::cout << "Deleted empty room: " << room_id << std::endl;
        }

        server->remove_empty_rooms(); // 继续设置下一次检查
    });
}

void SignalingServer::reconnect_user(const std::string& reconnect_token, 
                                    std::shared_ptr<UserPresence> temp_user, 
                                    std::shared_ptr<Connection> new_conn) {
    if (!new_conn) {
        std::cerr << "Reconnect failed: new_conn is null" << std::endl;
        return;
    }

    // 1. 从 Redis 验证 Token 并获取真正的 UserID
    RedisManager& redis = RedisManager::getInstance();
    std::string user_id;
    bool hit = redis.get("reconnect:" + reconnect_token, user_id) && !user_id.empty();

    if (!hit) {
        std::cerr << "Reconnect rejected: Invalid or expired token: " << reconnect_token << std::endl;
        new_conn->send_json({{"type", "error"}, {"message", "Invalid or expired reconnect token"}});
        // 这里直接关闭新 Socket
        new_conn->close_socket();
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(_map_mutex);
    auto it = _sessions.find(user_id);

    if (it != _sessions.end()) {
        // --- 情况 A: 内存中存在该用户的旧 Session 对象 ---
        auto old_user = it->second;

        std::cout << "Reconnecting: Recovering existing session for user: " << user_id << std::endl;

        // 1. 【关键】解除临时对象对新连接的持有关系
        // temp_user 是处理当前消息的临时对象，函数结束后它可能析构。
        // 我们必须先断开它对 new_conn 的引用，否则它析构时会触发 close_socket() 导致新连接也被关掉。
        if (temp_user) {
            temp_user->set_connection(nullptr);
        }

        // 2. 停掉旧对象上的心跳和连接
        // 如果旧连接还在（僵尸连接），强制关闭它
        if (auto old_conn = old_user->get_connection()) {
            std::cout << "Closing zombie connection for user: " << user_id << std::endl;
            old_conn->close_socket(); 
        }
        old_user->stop_heartbeat();

        // 3. 将新连接注入到旧的 UserPresence 对象中
        // 这会自动调用 connection->bind_user(old_user) 并重设消息回调
        old_user->init_connection(new_conn);

        // 4. 恢复旧用户的状态 (取消掉线计时器, 设为 ONLINE, 启动心跳)
        old_user->on_connection_recovered();

        // 5. 再次把 token 存入映射（防止 token 旋转逻辑导致的失效）
        _user_reconnect_tokens[user_id] = reconnect_token;

        // 6. 发送成功回执给客户端
        old_user->send_json({
            {"type", "reconnect_success"},
            {"session_id", user_id},
            {"self_name", old_user->get_self_name()},
            {"current_room", old_user->get_current_room()},
            {"reconnect_token", reconnect_token}
        });

    } else {
        // --- 情况 B: 内存中找不到旧 Session (可能服务器重启了，但 Redis 里的 Token 还在) ---
        std::cout << "Reconnecting: Session not in memory, promoting temp_user for: " << user_id << std::endl;

        if (!temp_user) {
            new_conn->close_socket();
            return;
        }

        // 1. 将当前的临时对象正式转正
        temp_user->set_user_id(user_id);
        temp_user->set_self_name(user_id); // 默认先设为 ID
        
        _sessions[user_id] = temp_user;
        _user_reconnect_tokens[user_id] = reconnect_token;

        // 2. 初始化状态
        temp_user->on_connection_recovered();

        // 3. 发送成功回执
        temp_user->send_json({
            {"type", "reconnect_success"},
            {"session_id", user_id},
            {"self_name", temp_user->get_self_name()},
            {"current_room", ""}, // 内存丢了，房间状态肯定也丢了，让客户端重新 join
            {"reconnect_token", reconnect_token}
        });
    }
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