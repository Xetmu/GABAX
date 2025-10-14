#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <string>
#include <thread>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace json = boost::json;
using tcp = net::ip::tcp;

std::mutex global_mutex;
using ws_stream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
using ws_ptr = std::shared_ptr<ws_stream>;

struct Client {
    std::string name;
    std::string room;
    ws_ptr socket;
};

std::unordered_map<std::string, std::shared_ptr<Client>> clients;
std::unordered_map<std::string, std::unordered_set<std::string>> rooms;

void send_message(ws_ptr ws, const std::string& msg) {
    ws->async_write(net::buffer(msg), [msg](beast::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "РћС€РёР±РєР° РѕС‚РїСЂР°РІРєРё: " << ec.message() << "\n";
        }
    });
}

void broadcast_to_room(const std::string& room, const std::string& from, const std::string& msg) {
    std::lock_guard<std::mutex> lock(global_mutex);
    if (!rooms.count(room)) return;
    for (const auto& name : rooms[room]) {
        if (name != from && clients.count(name)) {
            send_message(clients[name]->socket, msg);
        }
    }
}

class session : public std::enable_shared_from_this<session> {
public:
    session(tcp::socket&& socket, ssl::context& ctx)
        : ws_(std::make_shared<ws_stream>(std::move(socket), ctx)) {}

    void run() {
        auto self = shared_from_this();
        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
        ws_->next_layer().async_handshake(ssl::stream_base::server,
            [self](beast::error_code ec) {
                if (ec) {
                    std::cerr << "SSL Handshake Error: " << ec.message() << "\n";
                    return;
                }
                self->do_accept();
            });
    }

private:
    ws_ptr ws_;
    beast::flat_buffer buffer_;
    std::string client_name;

    void do_accept() {
        auto self = shared_from_this();
        ws_->async_accept([self](beast::error_code ec) {
            if (ec) {
                std::cerr << "WebSocket accept error: " << ec.message() << "\n";
                return;
            }
            self->do_read();
        });
    }

    void do_read() {
        auto self = shared_from_this();
        ws_->async_read(buffer_, [self](beast::error_code ec, std::size_t bytes_transferred) {
            if (ec) {
                self->on_disconnect();
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
                {
                    std::lock_guard<std::mutex> lock(global_mutex);
                    clients[client_name] = std::make_shared<Client>(Client{client_name, "", ws_});
                }
                std::cout << "РљР»РёРµРЅС‚ Р·Р°СЂРµРіРёСЃС‚СЂРёСЂРѕРІР°РЅ: " << client_name << "\n";
            } else if (type == "join_room") {
                std::string room = json::value_to<std::string>(val.at("room"));
                {
                    std::lock_guard<std::mutex> lock(global_mutex);
                    if (clients.count(client_name)) {
                        auto& client = clients[client_name];
                        if (!client->room.empty()) {
                            rooms[client->room].erase(client_name);
                        }
                        client->room = room;
                        rooms[room].insert(client_name);
                    }
                }
                std::cout << client_name << " РїСЂРёСЃРѕРµРґРёРЅРёР»СЃСЏ Рє " << room << "\n";
            } else if (type == "offer" || type == "answer" || type == "ice-candidate" || type == "ready") {
                std::lock_guard<std::mutex> lock(global_mutex);
                if (clients.count(client_name)) {
                    auto& client = clients[client_name];
                    std::string room = client->room;
                    val["from"] = client_name;
                    std::string serialized = json::serialize(val);
                    broadcast_to_room(room, client_name, serialized);
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "РћС€РёР±РєР° СЂР°Р·Р±РѕСЂР° JSON: " << e.what() << "\n";
        }
    }

    void on_disconnect() {
        std::lock_guard<std::mutex> lock(global_mutex);
        if (!client_name.empty() && clients.count(client_name)) {
            std::string room = clients[client_name]->room;
            if (!room.empty()) {
                rooms[room].erase(client_name);
            }
            clients.erase(client_name);
            std::cout << "РљР»РёРµРЅС‚ РѕС‚РєР»СЋС‡С‘РЅ: " << client_name << "\n";
        }
    }
};

class listener : public std::enable_shared_from_this<listener> {
public:
    listener(net::io_context& ioc, ssl::context& ctx, tcp::endpoint endpoint)
        : acceptor_(ioc), ctx_(ctx) {
        beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) std::cerr << "open error: " << ec.message() << "\n";

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) std::cerr << "set_option error: " << ec.message() << "\n";

        acceptor_.bind(endpoint, ec);
        if (ec) std::cerr << "bind error: " << ec.message() << "\n";

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) std::cerr << "listen error: " << ec.message() << "\n";
    }

    void start() {
        do_accept();
    }

private:
    tcp::acceptor acceptor_;
    ssl::context& ctx_;

    void do_accept() {
        acceptor_.async_accept([self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<session>(std::move(socket), self->ctx_)->run();
            } else {
                std::cerr << "Accept error: " << ec.message() << "\n";
            }
            self->do_accept();
        });
    }
};
/*
int main() {
    try {
        net::io_context ioc;

        ssl::context ctx(ssl::context::tlsv12_server);

	 ctx.set_options(
            ssl::context::default_workarounds
            | ssl::context::no_sslv2
            | ssl::context::no_sslv3
            | ssl::context::no_tlsv1
            | ssl::context::no_tlsv1_1
            // TLS 1.2 Рё РІС‹С€Рµ СЂР°Р·СЂРµС€РµРЅС‹
            );

        ctx.use_certificate_file("/etc/letsencrypt/live/gregere68.fvds.ru/fullchain.pem", ssl::context::pem);
        ctx.use_private_key_file("/etc/letsencrypt/live/gregere68.fvds.ru/privkey.pem", ssl::context::pem);

	auto endpoint = tcp::endpoint(net::ip::make_address("62.109.0.102"), 2222);
        //auto endpoint = tcp::endpoint(net::ip::make_address("gregere68.fvds.ru"), 2222);
	auto server = std::make_shared<listener>(ioc, ctx, endpoint);
        server->start();

        std::cout << "вљЎ TLS WebSocket СЃРµСЂРІРµСЂ Р·Р°РїСѓС‰РµРЅ РЅР° РїРѕСЂС‚Сѓ 2222\n";

        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "РћС€РёР±РєР°: " << e.what() << "\n";
    }
}
*/


int main() {
    try {
        net::io_context ioc;

        ssl::context ctx(ssl::context::tlsv12_server);
        ctx.use_certificate_file("/etc/letsencrypt/live/gregere68.fvds.ru/fullchain.pem", ssl::context::pem);
        ctx.use_private_key_file("/etc/letsencrypt/live/gregere68.fvds.ru/privkey.pem", ssl::context::pem);

        ctx.set_options(
            ssl::context::default_workarounds
            | ssl::context::no_sslv2
            | ssl::context::no_sslv3
            | ssl::context::no_tlsv1
            | ssl::context::no_tlsv1_1
        );

        auto endpoint = tcp::endpoint(net::ip::make_address("62.109.0.102"), 2222);
        auto server = std::make_shared<listener>(ioc, ctx, endpoint);
        server->start();

        std::cout << "вљЎ TLS WebSocket СЃРµСЂРІРµСЂ Р·Р°РїСѓС‰РµРЅ РЅР° РїРѕСЂС‚Сѓ 2222\n";

        // рџ”Ґ РњРЅРѕРіРѕРїРѕС‚РѕС‡РЅР°СЏ РѕР±СЂР°Р±РѕС‚РєР°: Р·Р°РїСѓСЃРєР°РµРј io_context РІ РЅРµСЃРєРѕР»СЊРєРёС… РїРѕС‚РѕРєР°С…
        const int thread_count = std::thread::hardware_concurrency();
        std::vector<std::thread> threads;
        for (int i = 0; i < thread_count; ++i) {
            threads.emplace_back([&ioc]() {
                ioc.run();
            });
        }

        for (auto& t : threads) {
            t.join();
        }

    } catch (const std::exception& e) {
        std::cerr << "РћС€РёР±РєР°: " << e.what() << "\n";
    }
}
