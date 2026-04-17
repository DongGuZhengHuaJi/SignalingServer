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
    if (ec == websocket::error::closed) { 
        auto user = _bind_user.lock();
        if (user) {
            // user->cleanup();
            user->stop_heartbeat();
            this->close_socket();
            user->on_connection_lost(shared_from_this());

        }
        return; 
    }
    if (ec) { 
        std::cerr << "WebSocket read error: " << ec.message() << std::endl;
        auto user = _bind_user.lock();
        if (user) {
            // user->cleanup();
            user->stop_heartbeat();
            this->close_socket();
            user->on_connection_lost(shared_from_this());
        }
        return; 
    }

    try {
        std::string msg = beast::buffers_to_string(_buffer.data());
        if (_message_callback) {
            _message_callback(msg);
        }
    } catch (std::exception& e) {
        std::cerr << "Message processing error: " << e.what() << std::endl;
    }

    _buffer.consume(bytes_transferred);
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
            if (ec) {
                std::cerr << "WebSocket write error: " << ec.message() << std::endl;
                auto user = self->_bind_user.lock();
                if (user) {
                    // user->cleanup();
                    user->stop_heartbeat();
                    self->close_socket();
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
    beast::error_code close_ec;
    _ws.close(code, close_ec);
    if (close_ec && close_ec != websocket::error::closed) {
        std::cerr << "WebSocket close error: " << close_ec.message() << std::endl;
    }
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
