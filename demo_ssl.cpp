#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <iostream>
#include <mutex>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace json = boost::json;
using tcp = net::ip::tcp;

// Используем ssl stream поверх tcp::socket
using ssl_stream = ssl::stream<tcp::socket>;
using ws_ptr = std::shared_ptr<websocket::stream<ssl_stream>>;

struct Client {
    std::string name;
    std::string room;
    ws_ptr socket;
};

std::mutex global_mutex;
std::unordered_map<std::string, std::shared_ptr<Client>> clients;
std::unordered_map<std::string, std::unordered_set<std::string>> rooms;

void send_message(ws_ptr ws, const std::string& msg) {
    ws->async_write(net::buffer(msg), [msg](beast::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Ошибка отправки сообщения: " << ec.message() << "\n";
        }
    });
}

void broadcast_to_room(const std::string& room, const std::string& from, const std::string& msg) {
    if (!rooms.count(room)) return;

    for (const auto& name : rooms[room]) {
        if (name != from && clients.count(name)) {
            send_message(clients[name]->socket, msg);
        }
    }
}

class session : public std::enable_shared_from_this<session> {
public:
    explicit session(tcp::socket&& socket, ssl::context& ctx)
        : ws_(std::make_shared<websocket::stream<ssl_stream>>(ssl_stream(std::move(socket), ctx))) {}

    void start() {
        auto self = shared_from_this();
        // TLS handshake сначала
        ws_->next_layer().async_handshake(ssl::stream_base::server,
            [self](beast::error_code ec) {
                if (ec) {
                    std::cerr << "Ошибка SSL handshake: " << ec.message() << "\n";
                    return;
                }
                // После успешного TLS handshake — принимаем websocket
                self->ws_->async_accept([self](beast::error_code ec) {
                    if (ec) {
                        std::cerr << "Ошибка accept: " << ec.message() << "\n";
                        return;
                    }
                    self->do_read();
                });
            });
    }

private:
    ws_ptr ws_;
    beast::flat_buffer buffer_;
    std::string client_name;
    std::string current_room;

    void do_read() {
        ws_->async_read(buffer_, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) {
                self->handle_disconnect();
                return;
            }

            std::string msg = beast::buffers_to_string(self->buffer_.data());
            self->buffer_.consume(self->buffer_.size());

            self->handle_message(msg);
            self->do_read();
        });
    }

    void handle_message(const std::string& msg) {
        try {
            auto val = json::parse(msg).as_object();
            std::string type = json::value_to<std::string>(val.at("type"));

            if (type == "register") {
                client_name = json::value_to<std::string>(val.at("name"));
                auto client = std::make_shared<Client>(Client{client_name, "", ws_});
                {
                    std::lock_guard<std::mutex> lock(global_mutex);
                    clients[client_name] = client;
                }
                std::cout << "Клиент зарегистрирован: " << client_name << "\n";

            } else if (type == "join_room") {
                std::string room = json::value_to<std::string>(val.at("room"));
                {
                    std::lock_guard<std::mutex> lock(global_mutex);
                    if (clients.count(client_name)) {
                        if (!clients[client_name]->room.empty()) {
                            rooms[clients[client_name]->room].erase(client_name);
                        }

                        clients[client_name]->room = room;
                        rooms[room].insert(client_name);
                        current_room = room;

                        std::cout << client_name << " присоединился к комнате " << room << "\n";
                    }
                }

            } else if (type == "offer" || type == "answer" || type == "ice-candidate" || type == "ready") {
                std::lock_guard<std::mutex> lock(global_mutex);
                if (clients.count(client_name)) {
                    const std::string& room = clients[client_name]->room;
                    if (!room.empty()) {
                        val["from"] = client_name;
                        std::string updated_msg = json::serialize(val);
                        broadcast_to_room(room, client_name, updated_msg);
                    }
                }

            } else {
                std::cerr << "Неизвестный тип сообщения: " << type << "\n";
            }

        } catch (const std::exception& e) {
            std::cerr << "Ошибка обработки JSON: " << e.what() << "\n";
        }
    }

    void handle_disconnect() {
        std::lock_guard<std::mutex> lock(global_mutex);
        if (!client_name.empty()) {
            if (clients.count(client_name)) {
                std::string room = clients[client_name]->room;
                if (!room.empty()) {
                    rooms[room].erase(client_name);
                    std::cout << client_name << " покинул комнату " << room << "\n";
                }
                clients.erase(client_name);
                std::cout << "Клиент отключён: " << client_name << "\n";
            }
        }
    }
};

class server {
public:
    server(net::io_context& ioc, ssl::context& ctx, tcp::endpoint endpoint)
        : acceptor_(ioc, endpoint), ctx_(ctx) {
        do_accept();
    }

private:
    tcp::acceptor acceptor_;
    ssl::context& ctx_;

    void do_accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<session>(std::move(socket), ctx_)->start();
            } else {
                std::cerr << "Ошибка accept: " << ec.message() << "\n";
            }
            do_accept();
        });
    }
};

int main() {
    try {
        net::io_context ioc;

        // SSL context и загрузка сертификатов
        ssl::context ctx(ssl::context::tlsv12_server);
        ctx.use_certificate_file("myCA/certificates/server/server.cert.pem", ssl::context::pem);
        ctx.use_private_key_file("myCA/certificates/server/server.key.pem", ssl::context::pem);

        auto local_ip = net::ip::make_address("172.28.210.12");
        tcp::endpoint endpoint(local_ip, 2222);

        server srv(ioc, ctx, endpoint);
        std::cout << "Signaling-сервер с TLS запущен на " << local_ip.to_string() << ":2222\n";

        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Ошибка запуска сервера: " << e.what() << "\n";
    }
}
