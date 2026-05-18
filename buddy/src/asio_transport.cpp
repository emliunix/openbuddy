#include "asio_transport.h"
#include <nlohmann/json.hpp>
#include <SDL.h>
#include "log.h"

// Custom SDL event type for transport messages.
// Registered at runtime to avoid collision with SDL user events.
static Uint32 CUSTOM_EVENT_TRANSPORT_MSG = 0;

static Uint32 get_transport_event_type() {
    if (CUSTOM_EVENT_TRANSPORT_MSG == 0) {
        CUSTOM_EVENT_TRANSPORT_MSG = SDL_RegisterEvents(1);
    }
    return CUSTOM_EVENT_TRANSPORT_MSG;
}

// ------------------------------------------------------------------
// Construction / destruction (RAII)
// ------------------------------------------------------------------

AsioTransport::AsioTransport(
    std::function<void(DaemonMsg)> on_msg,
    int listen_port
)
    : on_msg_(std::move(on_msg))
    , listen_port_(listen_port)
    , acceptor_(io_)
{
}

AsioTransport::~AsioTransport() {
    if (connected_.exchange(false)) {
        emit_disconnect("shutdown");
    }
    std::error_code ec;
    if (socket_) {
        socket_->close(ec);
    }
    if (acceptor_.is_open()) {
        acceptor_.close(ec);
    }
    io_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

// ------------------------------------------------------------------
// ITransport
// ------------------------------------------------------------------

void AsioTransport::start() {
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), listen_port_);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();

    do_accept();

    io_thread_ = std::thread([this] {
        io_.run();
    });

    LOG_INFO("[transport] listening on port %d", acceptor_.local_endpoint().port());
}

void AsioTransport::send(const BuddyMsg& msg) {
    if (!connected_ || !socket_) return;
    serialize_and_send(msg);
}

bool AsioTransport::is_connected() const {
    return connected_.load();
}

// ------------------------------------------------------------------
// Internal
// ------------------------------------------------------------------

void AsioTransport::do_accept() {
    acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (ec) {
            LOG_ERROR("[transport] accept error: %s", ec.message().c_str());
            return;
        }

        if (connected_.load()) {
            // Already connected — reject new socket; existing connection unaffected
            std::error_code close_ec;
            socket.close(close_ec);
            LOG_WARN("[transport] second connection rejected; already connected");
            do_accept();
            return;
        }

        connected_.store(true);
        socket_.emplace(std::move(socket));
        read_buffer_.clear();
        LOG_INFO("[transport] client connected");

        do_read();
    });
}

void AsioTransport::do_read() {
    if (!socket_ || !connected_) return;

    auto buf = asio::dynamic_buffer(read_buffer_);
    asio::async_read_until(*socket_, buf, '\n',
        [this](std::error_code ec, std::size_t bytes) {
            if (ec) {
                LOG_ERROR("[transport] read error: %s", ec.message().c_str());
                if (connected_.exchange(false)) {
                    emit_disconnect(ec.message());
                }
                socket_.reset();
                do_accept();  // accept next connection
                return;
            }

            // Extract line (without \n)
            auto pos = read_buffer_.find('\n');
            if (pos != std::string::npos) {
                std::string_view line(read_buffer_.data(), pos);
                on_line(line);
                read_buffer_.erase(0, pos + 1);
            }

            do_read();
        });
}

void AsioTransport::on_line(std::string_view line) {
    LOG_DEBUG("[transport] recv: %.*s", (int)line.size(), line.data());
    try {
        auto j = nlohmann::json::parse(line);
        DaemonMsg msg = parse_daemon_msg(j);
        if (on_msg_) on_msg_(std::move(msg));
    } catch (const std::exception& e) {
        LOG_ERROR("[transport] parse error: %s", e.what());
    }
}

void AsioTransport::emit_disconnect(std::string reason) {
    if (on_msg_) {
        on_msg_(TransportDisconnect{std::move(reason)});
    }
}

void AsioTransport::serialize_and_send(const BuddyMsg& msg) {
    try {
        nlohmann::json j = msg;
        auto line = std::make_shared<std::string>(j.dump() + "\n");
        LOG_DEBUG("[transport] send: %s", line->c_str());
        asio::async_write(*socket_, asio::buffer(*line),
            [line](std::error_code, std::size_t) {});
    } catch (const std::exception& e) {
        LOG_ERROR("[transport] serialize error: %s", e.what());
    }
}
