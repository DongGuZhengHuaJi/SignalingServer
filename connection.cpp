#include "connection.h"
#include "user_presence.h"
#include <iostream>

Connection::Connection(boost::asio::ip::tcp::socket&& socket)
    : _ws(std::move(socket)) {
}

void Connection::start() {
    _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    _ws.async_accept(beast::bind_front_handler(&Connection::on_accept, shared_from_this()));
}

void Connection::on_accept(beast::error_code ec) {
    if (ec) {
        std::cerr << "WebSocket accept error: " << ec.message() << std::endl;
        return;
    }
    do_read();
}

void Connection::do_read() {
    _ws.async_read(_buffer, beast::bind_front_handler(&Connection::on_read, shared_from_this()));
}

void Connection::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    // 1. 核心防御：如果是操作被取消（例如我们主动关闭了 Socket），直接返回
    // 这样可以防止在关闭过程中，残留的 read 回调再次触发 cleanup 逻辑
    if (ec == boost::asio::error::operation_aborted) {
        return;
    }

    // 2. 处理错误（包括对端正常关闭 ec == websocket::error::closed 或其他网络错误）
    if (ec) {
        // 如果是正常关闭，不打印 error 日志，如果是异常错误则打印
        if (ec != websocket::error::closed) {
            std::cerr << "WebSocket read error: " << ec.message() << std::endl;
        }

        auto user = _bind_user.lock();
        if (user) {
            // 停止心跳定时器
            user->stop_heartbeat();
            
            // 安全关闭：这里会进入我们修改过的 close_socket
            // 内部的 std::atomic<bool> _is_closing 会保证只有第一次调用生效
            this->close_socket();

            // 通知用户管理器：连接已丢失，进入 30s 重连等待期（hover timer）
            user->on_connection_lost(shared_from_this());
        }
        return; 
    }

    // 3. 正常处理接收到的消息
    try {
        // 将 buffer 数据转为字符串
        std::string msg = beast::buffers_to_string(_buffer.data());
        
        // 必须在回调执行前消耗掉 buffer，否则下次 read 会重叠
        _buffer.consume(bytes_transferred);

        if (_message_callback) {
            _message_callback(msg);
        }
    } catch (const std::exception& e) {
        std::cerr << "Message processing error: " << e.what() << std::endl;
        // 如果处理单条消息崩溃，通常建议消耗掉该条数据并继续读取
        _buffer.consume(_buffer.size()); 
    }

    // 4. 继续监听下一条消息
    do_read();
}

void Connection::queue_write(std::string msg) {
    bool write_in_progress = !_write_queue.empty();
    _write_queue.push(std::move(msg));
    if (!write_in_progress) {
        do_write();
    }
}

void Connection::do_write() {
    if (_write_queue.empty()) return;

    _ws.async_write(net::buffer(_write_queue.front()), 
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec == boost::asio::error::operation_aborted) return; // 拦截取消操作

            if (ec) {
                std::cerr << "WebSocket write error: " << ec.message() << std::endl;
                auto user = self->_bind_user.lock();
                if (user) {
                    user->stop_heartbeat();
                    self->close_socket(); // 再次利用原子锁保护
                    user->on_connection_lost(self);
                }
                return;
            }
            self->_write_queue.pop();
            if (!self->_write_queue.empty()) {
                self->do_write();
            }
        });
}

void Connection::send_json(const nlohmann::json& msg) {
    std::string msg_str = msg.dump();
    net::post(_ws.get_executor(), [self = shared_from_this(), msg_str]() {
        self->queue_write(std::move(msg_str));
    });
}

void Connection::close_socket(websocket::close_code code) {
    if (_is_closing.exchange(true)) return; 

    beast::error_code ec;
    // 1. 先尝试优雅关闭 WebSocket
    // _ws.close(code, ec);

    // 2. 强制关闭底层 TCP Socket (这是解决重连卡死的关键)
    auto& socket = beast::get_lowest_layer(_ws).socket();
    if (socket.is_open()) {
        ec.clear();
        socket.shutdown(tcp::socket::shutdown_both, ec);

        // shutdown 可能返回错误码，不能忽略（例如 socket 已半关闭或已断开）
        if (ec && ec != boost::asio::error::not_connected) {
            std::cerr << "Socket shutdown error: " << ec.message() << std::endl;
        }

        ec.clear();
        socket.close(ec);

        if (ec) {
            std::cerr << "Socket close error: " << ec.message() << std::endl;
        }
    }
    
    // 3. 释放用户绑定，防止回调再次进入 UserPresence
    _bind_user.reset();
}

void Connection::set_message_callback(MessageCallback cb) {
    _message_callback = cb;
}

void Connection::bind_user(std::shared_ptr<UserPresence> user) {
    _bind_user = user;
}

std::shared_ptr<UserPresence> Connection::get_bind_user() const {
    return _bind_user.lock();
}
