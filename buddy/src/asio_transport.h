#pragma once
#include "transport.h"
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <thread>
#include <memory>
#include <string>

// Asio-based TCP transport.
// Runs an internal io_context on a background thread.
// Messages are delivered to the main thread via SDL_UserEvent (thread-safe).
//
// RAII: destructor cleanly shuts down socket and io_context, emitting
// TransportDisconnect if previously connected.
//
// NDJSON framing: one JSON object per line (\\n delimited).

class AsioTransport : public ITransport {
public:
    // on_msg: callback invoked for each received DaemonMsg.
    // The callback MUST be thread-safe (typically pushes SDL_UserEvent).
    explicit AsioTransport(
        std::function<void(DaemonMsg)> on_msg,
        int listen_port = 0
    );

    ~AsioTransport() override;

    // Delete copy/move — owns threads and socket state
    AsioTransport(const AsioTransport&) = delete;
    AsioTransport& operator=(const AsioTransport&) = delete;
    AsioTransport(AsioTransport&&) = delete;
    AsioTransport& operator=(AsioTransport&&) = delete;

    // ITransport
    void start() override;
    void send(const BuddyMsg& msg) override;
    bool is_connected() const override;

private:
    std::function<void(DaemonMsg)> on_msg_;
    int listen_port_;

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::optional<asio::ip::tcp::socket> socket_;
    std::atomic<bool> connected_{false};
    std::thread io_thread_;

    std::string read_buffer_;

    void do_accept();
    void do_read();
    void on_line(std::string_view line);
    void emit_disconnect(std::string reason);
    void serialize_and_send(const BuddyMsg& msg);
};
