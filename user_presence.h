#include <memory>
#include <string>
#include <chrono>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

namespace net = boost::asio;

class Connection; // 前向声明 Connection 类
class SignalingServer; // 前向声明 SignalingServer 类


enum class UserStatus {
    ONLINE,
    RECONNECTING,
    OFFLINE
};
// UserPresence类处理用户身份、状态、房间管理、消息处理
class UserPresence : public std::enable_shared_from_this<UserPresence> {
public:
    UserPresence(std::shared_ptr<SignalingServer> server);

    // 初始化连接
    void init_connection(std::shared_ptr<Connection> connection);
    void set_connection(std::shared_ptr<Connection> connection);

    // 用户ID和名称管理
    const std::string& get_user_id() const;
    void set_user_id(const std::string& user_id);

    const std::string& get_self_name() const;
    void set_self_name(const std::string& self_name);

    // 房间管理
    void set_current_room(const std::string& room_id);
    std::string get_current_room() const;

    // 消息处理
    void handle_message(const nlohmann::json& message);
    void send_json(const nlohmann::json& msg);

    // 心跳管理
    void start_heartbeat();
    void reset_heartbeat();
    void stop_heartbeat();

    // 清理资源
    void cleanup();

    // 获取连接对象
    std::shared_ptr<Connection> get_connection() const;

    void on_connection_lost(std::shared_ptr<Connection> conn);
    void on_hover_timeout();
    void on_connection_recovered();

private:
    void schedule_heartbeat();

    std::string _user_id;
    std::string _self_name;
    std::string _current_room;
    std::weak_ptr<Connection> _connection; // 连接对象的弱引用，避免循环引用
    std::shared_ptr<SignalingServer> _server;
    
    net::steady_timer _heartbeat_timer;
    std::chrono::system_clock::time_point _last_heartbeat;

    UserStatus _status = UserStatus::ONLINE;
    net::steady_timer _hover_timer;  // 悬停定时器
    std::string _reconnect_token;    // 重连凭证
};