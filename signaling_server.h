#ifndef SIGNALING_SERVER_H
#define SIGNALING_SERVER_H

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast.hpp>
#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;

class UserPresence; // 前向声明
class Connection; // 前向声明


class SignalingServer : public std::enable_shared_from_this<SignalingServer> {
public:
    SignalingServer(net::io_context& ioc, short port);

    void start();
    void stop();
    void start_accept();

    void register_user(const std::string& id, const std::string& self_name, std::shared_ptr<UserPresence> user);
    void remove_user(const std::string& id);
    void deliver(const std::string& target_id, const nlohmann::json& msg);
    void broadcast(const std::string& room_id, const nlohmann::json& msg, std::shared_ptr<UserPresence> exclude_user = nullptr);

    void create_room(const std::string& room_id, std::shared_ptr<UserPresence> user,
                     std::chrono::system_clock::time_point start_time = std::chrono::system_clock::now(),
                     const std::string& meeting_type = "quick");
    void create_reserved_room(const std::string& room_id, const std::string& host_id,
                              std::chrono::system_clock::time_point start_time);
    void join_room(const std::string& room_id, std::shared_ptr<UserPresence> user);
    void leave_room(const std::string& room_id, std::shared_ptr<UserPresence> user, bool end_meeting = false);

    void remove_room(const std::string& room_id, const std::string& reason = "empty_timeout");
    void remove_empty_rooms();

    net::io_context& get_io_context();

    void reconnect_user(const std::string& reconnect_token, std::shared_ptr<UserPresence> user, std::shared_ptr<Connection> new_conn);

private:
    net::io_context& _ioc;
    tcp::acceptor _acceptor;
    std::recursive_mutex _map_mutex;
    std::unordered_map<std::string, std::shared_ptr<UserPresence>> _sessions;
    std::unordered_map<std::string, std::string> _user_reconnect_tokens;
    std::unique_ptr<net::steady_timer> _empty_room_timer;

    friend class UserPresence;
};

#endif // SIGNALING_SERVER_H
