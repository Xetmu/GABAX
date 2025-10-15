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

using ws_stream = beast::websocket::stream<beast::ssl_stream<tcp::socket>>;
using ws_ptr = std::shared_ptr<ws_stream>;

struct Client {
    std::string name;
    std::string room;
    ws_ptr ws;
    
	
    //std::deque<std::string> write_queue;
    //bool writing = false;
	/*
    net::strand<net::io_context::executor_type> strand;


    Client(const std::string& n, const std::string& r, ws_ptr ws)
	: name(n), room(r), ws(std::move(ws)), strand(ws->get_executor().context().get_executor())
	 {}*/
	 
	 net::strand<net::any_io_executor> strand;

Client(const std::string& n, const std::string& r, ws_ptr ws)
    : name(n), room(r), ws(std::move(ws)), strand(this->ws->get_executor())
{}


	
};

std::mutex global_mutex;
std::mutex client_mutex;
std::unordered_map<std::string, std::shared_ptr<Client>> clients;
std::unordered_map<std::string, std::unordered_set<std::string>> rooms;


/*
void send_message(std::shared_ptr<Client> client, const std::string& msg) {
    client->write_queue.push_back(msg);

    if (!client->writing) {
        client->writing = true;
        do_write(client);
    }
}

void do_write(std::shared_ptr<Client> client) {
    if (client->write_queue.empty()) {
        client->writing = false;
        return;
    }

    const std::string& msg = client->write_queue.front();

    client->socket->async_write(net::buffer(msg), [client](beast::error_code ec, std::size_t) {
        if (ec) {
            // Обработка ошибки
            client->writing = false;
            return;
        }

        client->write_queue.pop_front();
        do_write(client); // рекурсивно отправляем следующее сообщение
    });
}
*/

/*
void send_message(ws_ptr ws, const std::string& msg) {
    ws->async_write(net::buffer(msg), [msg](beast::error_code ec, std::size_t) {
        if (ec) {
            std::cerr << "Ошибка отправки сообщения: " << ec.message() << "\n";
        }
    });
}


// Отправляем сообщение всем клиентам в комнате, кроме отправителя
void broadcast_to_room(const std::string& room, const std::string& from, const std::string& msg) {
    if (!rooms.count(room)) return;

    for (const auto& name : rooms[room]) {
        if (name != from && clients.count(name)) {
            send_message(clients[name]->socket, msg);
        }
    }
}
*/


void send_message(std::shared_ptr<Client> client, const std::string& msg) {
    // Запускаем через strand, чтобы serialize async_write вызовы
    boost::asio::post(client->strand, [client, msg]() {
        client->ws->async_write(net::buffer(msg), 
            [msg](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "Ошибка отправки сообщения: " << ec.message() << "\n";
                }
            });
    });
}



// Отправляем сообщение всем клиентам в комнате, кроме отправителя
void broadcast_to_room(const std::string& room, const std::string& from, const std::string& msg) {
    if (!rooms.count(room)) return;

    for (const auto& name : rooms[room]) {
        if (name != from && clients.count(name)) {
            send_message(clients[name], msg);
        }
    }
}

class session : public std::enable_shared_from_this<session> {
public:
	session(tcp::socket&& socket, ssl::context& ctx)
    : ws_(std::make_shared<ws_stream>(std::move(socket), ctx)) {}

		
	ws_ptr get_ws() { return wsp_; }

    void start() {
        auto self = shared_from_this();
        ws_->next_layer().async_handshake(
            ssl::stream_base::server,
            [self](beast::error_code ec) {
                if (ec) {
                    std::cerr << "SSL handshake error: " << ec.message() << "\n";
                    return;
                }
                self->do_ws_accept();
            }
        );
    }

private:
    ws_ptr ws_;
	ws_ptr wsp_;
    beast::flat_buffer buffer_;
    std::string client_name;
    std::string current_room;

    void do_ws_accept() {
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
        ws_->async_read(buffer_, [self](beast::error_code ec, std::size_t) {
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

            } /*else if (type == "offer" || type == "answer" || type == "ice-candidate" || type == "ready") {
                std::lock_guard<std::mutex> lock(global_mutex);
                if (clients.count(client_name)) {
                    const std::string& room = clients[client_name]->room;
                    if (!room.empty()) {
                        val["from"] = client_name;
                        std::string updated_msg = json::serialize(val);
                        broadcast_to_room(room, client_name, updated_msg);
                    }
                }*/
                else if (type == "offer" || type == "answer" || type == "ice-candidate") {
    		std::lock_guard<std::mutex> lock(global_mutex);
    		if (clients.count(client_name)) {
        	    std::string to = json::value_to<std::string>(val.at("to"));
       		    val["from"] = client_name;
                    std::string updated_msg = json::serialize(val);

        	if (clients.count(to)) {
            		send_message(clients[to], updated_msg);
       		 } else {
            		std::cerr << "Клиент " << to << " не найден для передачи " << type << "\n";
        	}
    }
}
else if (type == "ready") {
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
        : acceptor_(ioc), ctx_(ctx)
    {
        beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "open error: " << ec.message() << "\n";
            return;
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "set_option error: " << ec.message() << "\n";
            return;
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            std::cerr << "bind error: " << ec.message() << "\n";
            return;
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "listen error: " << ec.message() << "\n";
            return;
        }

        do_accept();
    }
    private: tcp::acceptor acceptor_; ssl::context& ctx_;

void do_accept() {
        acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<session>(std::move(socket), ctx_)->start();
            } else {
                std::cerr << "Ошибка accept: " << ec.message() << "\n";
            }
            do_accept();  // Продолжить принимать соединения
        });
    }
};
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

        // Использовать локальный IP, на который могут подключиться клиенты
        auto local_ip = net::ip::make_address("62.109.0.102");  // Замените при необходимости
        tcp::endpoint endpoint(local_ip, 2222);

        server srv(ioc, ctx, endpoint);
        std::cout << "Signaling-сервер запущен на " << local_ip.to_string() << ":2222\n";

        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Ошибка запуска сервера: " << e.what() << "\n";
    }
}