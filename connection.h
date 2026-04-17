#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core/error.hpp>
#include <cstddef>
#include <memory>
#include <queue>
#include <functional>
#include <nlohmann/json.hpp>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

class UserPresence; // 前向声明 UserPresence 类

// Connection类处理WebSocket连接、读写操作
class Connection: public std::enable_shared_from_this<Connection> {
public:
    Connection(boost::asio::ip::tcp::socket&& socket);

    void start();
    
    // 读操作
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    
    // 写操作
    void queue_write(std::string msg);
    void do_write();
    
    // 发送JSON消息
    void send_json(const nlohmann::json& msg);
    
    // 关闭连接
    void close_socket(websocket::close_code code = websocket::close_code::policy_error);
    
    // 设置消息处理回调
    using MessageCallback = std::function<void(const std::string&)>;
    void set_message_callback(MessageCallback cb);
    
    // 绑定用户对象
    void bind_user(std::shared_ptr<UserPresence> user);
    std::shared_ptr<UserPresence> get_bind_user() const;

    

private:
    void on_accept(beast::error_code ec);

    websocket::stream<beast::tcp_stream> _ws;
    beast::flat_buffer _buffer;
    std::queue<std::string> _write_queue;
    std::weak_ptr<UserPresence> _bind_user; // 绑定的用户对象，避免循环引用
    MessageCallback _message_callback;
};