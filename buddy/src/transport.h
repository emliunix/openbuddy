#pragma once
#include "protocol_types.h"
#include <functional>
#include <memory>

// Behavior contract for a bidirectional message transport.
// Creation, destruction, and connection lifecycle are NOT part of this
// interface — they are implementation concerns.
//
// Typical usage:
//   auto transport = std::make_unique<AsioTransport>(on_msg_callback);
//   transport->start();
//   transport->send(msg);
//   // transport dropped: RAII in concrete class handles cleanup

class ITransport {
public:
    virtual ~ITransport() = default;

    // Begin accepting/connecting. Non-blocking; returns immediately.
    // May spawn internal threads.
    virtual void start() = 0;

    // Send a message. Non-blocking; queues for transmission.
    virtual void send(const BuddyMsg& msg) = 0;

    // Current transport-level connection state.
    virtual bool is_connected() const = 0;
};
