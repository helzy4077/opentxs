// Copyright (c) 2018 The Open-Transactions developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "stdafx.hpp"

#include "Socket.hpp"

#include "opentxs/core/Log.hpp"
#include "opentxs/network/zeromq/Context.hpp"
#include "opentxs/network/zeromq/Frame.hpp"
#include "opentxs/network/zeromq/FrameIterator.hpp"
#include "opentxs/network/zeromq/Message.hpp"

#include <zmq.h>

#include <random>

#define INPROC_PREFIX "inproc://opentxs/"

#define OT_METHOD "opentxs::network::zeromq::implementation::Socket::"

namespace opentxs::network::zeromq::implementation
{
const std::map<SocketType, int> Socket::types_{
    {SocketType::Request, ZMQ_REQ},
    {SocketType::Reply, ZMQ_REP},
    {SocketType::Publish, ZMQ_PUB},
    {SocketType::Subscribe, ZMQ_SUB},
    {SocketType::Pull, ZMQ_PULL},
    {SocketType::Push, ZMQ_PUSH},
    {SocketType::Pair, ZMQ_PAIR},
    {SocketType::Dealer, ZMQ_DEALER},
    {SocketType::Router, ZMQ_ROUTER},
};

Socket::Socket(
    const zeromq::Context& context,
    const SocketType type,
    const Socket::Direction direction)
    : context_(context)
    , direction_(direction)
    , socket_(zmq_socket(context, types_.at(type)))
    , linger_(0)
    , send_timeout_(-1)
    , receive_timeout_(-1)
    , endpoints_()
    , running_(Flag::Factory(true))
    , type_(type)
{
    OT_ASSERT(nullptr != socket_);
}

Socket::operator void*() const { return socket_; }

bool Socket::apply_timeouts(const Lock& lock) const
{
    OT_ASSERT(nullptr != socket_)
    OT_ASSERT(verify_lock(lock))

    auto set = zmq_setsockopt(socket_, ZMQ_LINGER, &linger_, sizeof(linger_));

    if (0 != set) {
        otErr << OT_METHOD << __FUNCTION__ << ": Failed to set ZMQ_LINGER"
              << std::endl;

        return false;
    }

    set = zmq_setsockopt(
        socket_, ZMQ_SNDTIMEO, &send_timeout_, sizeof(send_timeout_));

    if (0 != set) {
        otErr << OT_METHOD << __FUNCTION__ << ": Failed to set ZMQ_SNDTIMEO"
              << std::endl;

        return false;
    }

    set = zmq_setsockopt(
        socket_, ZMQ_RCVTIMEO, &receive_timeout_, sizeof(receive_timeout_));

    if (0 != set) {
        otErr << OT_METHOD << __FUNCTION__ << ": Failed to set ZMQ_RCVTIMEO"
              << std::endl;

        return false;
    }

    return true;
}

bool Socket::bind(const Lock& lock, const std::string& endpoint) const
{
    if (false == running_.get()) { return false; }

    apply_timeouts(lock);

    const auto output = (0 == zmq_bind(socket_, endpoint.c_str()));

    if (output) { endpoints_.emplace_back(endpoint); }

    return output;
}

bool Socket::connect(const Lock& lock, const std::string& endpoint) const
{
    if (false == running_.get()) { return false; }

    apply_timeouts(lock);

    const auto output = (0 == zmq_connect(socket_, endpoint.c_str()));

    if (output) { endpoints_.emplace_back(endpoint); }

    return output;
}

bool Socket::Close() const
{
    OT_ASSERT(nullptr != socket_);

    Lock lock(lock_);

    return (0 == zmq_close(socket_));
}

bool Socket::receive_message(
    const Lock& lock,
    void* socket,
    zeromq::Message& message)
{
    bool receiving{true};

    while (receiving) {
        auto& frame = message.AddFrame();
        const bool received = (-1 != zmq_msg_recv(frame, socket, ZMQ_DONTWAIT));

        if (false == received) {
            auto zerr = zmq_errno();
            if (EAGAIN == zerr) {
                otErr << OT_METHOD << __FUNCTION__ << ": No messages."
                      << std::endl;
            } else {
                otErr << OT_METHOD << __FUNCTION__
                      << ": Receive error: " << zmq_strerror(zerr) << std::endl;
            }

            return false;
        }

        int option{0};
        std::size_t optionBytes{sizeof(option)};

        const bool haveOption =
            (-1 != zmq_getsockopt(socket, ZMQ_RCVMORE, &option, &optionBytes));

        if (false == haveOption) {
            otErr << OT_METHOD << __FUNCTION__
                  << ": Failed to check socket options error:\n"
                  << zmq_strerror(zmq_errno()) << std::endl;

            return false;
        }

        OT_ASSERT(optionBytes == sizeof(option))

        if (1 != option) { receiving = false; }
    }

    return true;
}

bool Socket::send_message(const Lock& lock, void* socket, Message& message)
{
    bool sent{true};
    const auto parts = message.size();
    std::size_t counter{0};

    for (auto& frame : message) {
        int flags{0};

        if (++counter < parts) { flags = ZMQ_SNDMORE; }

        sent |= (-1 != zmq_msg_send(frame, socket, flags));
    }

    if (false == sent) {
        otErr << OT_METHOD << __FUNCTION__ << ": Send error:\n"
              << zmq_strerror(zmq_errno()) << std::endl;
    }

    return sent;
}

bool Socket::send_message(const Lock& lock, Message& message) const
{
    return send_message(lock, socket_, message);
}

std::string Socket::random_inproc_endpoint()
{
    std::random_device seed;
    std::mt19937 generator(seed());
    std::uniform_int_distribution<std::uint64_t> rand;

    return std::string(INPROC_PREFIX) + std::to_string(rand(generator)) +
           std::to_string(rand(generator));
}

bool Socket::receive_message(const Lock& lock, Message& message) const
{
    return receive_message(lock, socket_, message);
}

bool Socket::set_socks_proxy(const std::string& proxy) const
{
    OT_ASSERT(nullptr != socket_);

    Lock lock(lock_);
    const auto set =
        zmq_setsockopt(socket_, ZMQ_SOCKS_PROXY, proxy.data(), proxy.size());

    return (0 == set);
}

bool Socket::SetTimeouts(
    const std::chrono::milliseconds& linger,
    const std::chrono::milliseconds& send,
    const std::chrono::milliseconds& receive) const
{
    Lock lock(lock_);
    linger_ = linger.count();
    send_timeout_ = send.count();
    receive_timeout_ = receive.count();

    return apply_timeouts(lock);
}

void Socket::shutdown()
{
    Lock lock(lock_);
    running_->Off();

    for (const auto& endpoint : endpoints_) {
        if (Socket::Direction::Connect == direction_) {
            zmq_disconnect(socket_, endpoint.c_str());
        } else {
            zmq_unbind(socket_, endpoint.c_str());
        }
    }

    endpoints_.clear();
}

bool Socket::start_client(const Lock& lock, const std::string& endpoint) const
{
    OT_ASSERT(nullptr != socket_);

    if (false == connect(lock, endpoint)) {
        otErr << OT_METHOD << __FUNCTION__ << ": Failed to connect to "
              << endpoint << std::endl;

        return false;
    }

    return true;
}

SocketType Socket::Type() const { return type_; }

Socket::~Socket()
{
    Lock lock(lock_);

    if (nullptr != socket_) { zmq_close(socket_); }
}
}  // namespace opentxs::network::zeromq::implementation
