#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "thread_pool.hpp"

using namespace boost::asio;


class Session : public std::enable_shared_from_this<Session> {
public:
    Session(ip::tcp::socket socket, std::shared_ptr<SignalingServer> server) : _socket(std::move(socket)), _server(server) {}

    void start() {
        read_header();
    }

private:
    void read_header() {
        auto self(shared_from_this());
        
        boost::asio::async_read(_socket, boost::asio::buffer(&_header_data, sizeof(_header_data)),
            [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    // 大端序转小端序，得到 body 长度
                    uint32_t body_length = ntohl(_header_data);
                    
                    // 安全检查：防止恶意超大包
                    if (body_length > 0 && body_length < 1024 * 1024) {
                        read_body(body_length);
                    }
                } else {
                    std::cerr << "Read header error: " << ec.message() << std::endl;
                }
            });
    }

    void read_body(size_t length) {
        auto self(shared_from_this());
        
        // 确保 body_buffer 有足够的空间来存储接收的数据
        _body_buffer.resize(length);
        
        boost::asio::async_read(_socket, boost::asio::buffer(_body_buffer),
            [this, self](boost::system::error_code ec, std::size_t actual_length) {
                if (!ec) {
                    try {
                        // 解析 JSON
                        auto data = nlohmann::json::parse(_body_buffer.begin(), _body_buffer.end());
                        handle_read(data);
                        
                        // 读完 Body 之后，再开启下一次读 Header
                        read_header(); 
                    } catch (const std::exception& e) {
                        std::cerr << "JSON Parse error: " << e.what() << std::endl;
                    }
                }
            });
    }

    void handle_read(const nlohmann::json& data) {
        std::cout << "收到 JSON 信令: " << data.dump() << std::endl;
        std::string type = data["type"];
        if(type == "register") {
            std::cout << "处理 register 信令" << std::endl;
            _session_id = data["session_id"];
            {
                std::lock_guard<std::mutex> lock(_server->get_map_mutex());
                _server->get_sessions()[_session_id] = shared_from_this();
            }
        } else if (type == "offer") {

            std::cout << "处理 offer 信令" << std::endl;

        } else if (type == "answer") {

            std::cout << "处理 answer 信令" << std::endl;

        } else if (type == "candidate") {

            std::cout << "处理 candidate 信令" << std::endl;

        } else {

            std::cout << "未知信令类型: " << type << std::endl;

        }
    }

    void send(const nlohmann::json& msg) {
        auto self(shared_from_this());
        std::string msg_str = msg.dump();
        uint32_t body_length = htonl(msg_str.size());
        
        // 发送 header
        boost::asio::async_write(_socket, buffer(&body_length, sizeof(body_length)),
            [this, self, msg_str](boost::system::error_code ec, std::size_t /*length*/) {
                if (!ec) {
                    // 发送 body
                    boost::asio::async_write(_socket, boost::asio::buffer(msg_str),
                        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
                            if (ec) {
                                std::cerr << "Write error: " << ec.message() << std::endl;
                            }
                        });
                } else {
                    std::cerr << "Write header error: " << ec.message() << std::endl;
                }
            });
    }

private:
    ip::tcp::socket _socket;
    std::string _session_id; // 用于标识会话的唯一 ID
    uint32_t _header_data;           // 存放 4 字节头
    std::vector<char> _body_buffer;  // 存放变长 Body
    std::shared_ptr<SignalingServer> _server; // 用于访问服务器的会话管理等功能
};

class SignalingServer : public std::enable_shared_from_this<SignalingServer> {
public:
    SignalingServer(const short port)
        : _io_context(), _acceptor(_io_context, ip::tcp::endpoint(ip::tcp::v4(), port)) {
    }

    void start() {
        start_accept();
    }

    void stop() {
        _io_context.stop();
    }

    void start_accept(){
        auto self(shared_from_this());
        _acceptor.async_accept([this, self](boost::system::error_code ec, ip::tcp::socket socket) {
            if (!ec) {
                std::cout << "New client connected: " << socket.remote_endpoint() << std::endl;
                // Handle the new connection (e.g., read/write data)
                auto session = std::make_shared<Session>(std::move(socket), shared_from_this());
                session->start();
            }
            start_accept(); // Accept the next connection
        });
    }

    io_context& get_io_context() {
        return _io_context;
    }

    std::mutex& get_map_mutex() {
        return _map_mutex;
    }

    std::unordered_map<std::string, std::shared_ptr<Session>>& get_sessions() {
        return _sessions;
    }

    bool deliver_message(const std::string& target_id, const nlohmann::json& msg) {
        std::lock_guard<std::mutex> lock(_map_mutex);
        if(_sessions.contains(target_id)){
            auto session = _sessions[target_id];
            // session->send(msg);
            return true;
        }
        return false;
    }

private:
    io_context _io_context;
    ip::tcp::acceptor _acceptor;
    std::mutex _map_mutex;
    std::unordered_map<std::string, std::shared_ptr<Session>> _sessions;
};

int main() {
    try {
        auto server = std::make_shared<SignalingServer>(8080);
        server->start();

        // 1. 创建信号通知
        std::promise<void> exit_signal;
        std::future<void> future_signal = exit_signal.get_future();

        // 2. 创建信号集，监听 Ctrl+C (SIGINT) 和 终止信号 (SIGTERM)
        boost::asio::io_context& ioc = server->get_io_context();
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);

        // 3. 异步等待信号
        signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
            if (!error) {
                std::cout << "\nReceived signal: " << signal_number << ". Shutting down..." << std::endl;
                server->stop(); // 触发停止逻辑
                exit_signal.set_value(); // 通知主线程退出
            }
        });

        // 4. 启动线程池运行 IO 任务
        auto& thread_pool = ThreadPool::getInstance();
        for (int i = 0; i < std::thread::hardware_concurrency(); ++i) {
            thread_pool.commit([server]() {
                server->get_io_context().run(); 
            });
        }
        std::cout << "Signaling server started on port 8080." << std::endl;

        std::cout << std::thread::hardware_concurrency() << " threads running the IO context." << std::endl;

        std::cout << "Server is running. Press Ctrl+C to stop." << std::endl;
        
        future_signal.wait(); // 等待退出信号

    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}
