#include "length_framed_unbuffered_connection.hpp"

#include <array>

#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>

#include "byte_order.hpp"
#include "check.hpp"
#include "debug_log.hpp"

namespace riak {

namespace ph = std::placeholders;
namespace asio = boost::asio;
namespace ip = boost::asio::ip;

struct length_framed_unbuffered_connection::active_request_state {
  request request;
  std::string response;
  uint32_t request_length = 0;
  uint32_t response_length = 0;
  std::atomic<bool> done{false};
};

length_framed_unbuffered_connection::length_framed_unbuffered_connection(
    asio::io_service& io_service,
    const std::vector<ip::tcp::endpoint>& endpoints)
    : strand_{io_service},
      socket_{io_service},
      deadline_timer_{io_service},
      endpoints_{endpoints},
      blocker_{*this} {}

length_framed_unbuffered_connection::~length_framed_unbuffered_connection() {
  shutdown();
  blocker_.destroy();
}

void length_framed_unbuffered_connection::send_and_consume_request(
    request& new_request) {
  // If there's another active request, kill the process.
  RIAKPP_CHECK(!has_active_request_.exchange(true))
      << "Unbuffered connection called again before request completion.";

  auto state = std::make_shared<active_request_state>();
  state->request = std::move(new_request);
  current_request_state_ = state;

  strand_.post(std::bind(&self_type::start_request, blocker_.new_ptr(), state));
}


void length_framed_unbuffered_connection::shutdown() {
  if (auto state = current_request_state_.lock()) {
    auto blocking_this = blocker_.new_ptr();
    strand_.post([blocking_this, state] {
      state->done = true;
      blocking_this->deadline_timer_.cancel();
      if (blocking_this->socket_.is_open()) blocking_this->socket_.close();
    });
  }
}

void length_framed_unbuffered_connection::start_request(
    shared_request_state state) {
  // Setup deadline timer if needed.
  auto deadline_ms = state->request.deadline_ms;
  if (deadline_ms >= 0) {
    deadline_timer_.expires_from_now(
        boost::posix_time::milliseconds(deadline_ms));

    auto blocking_this = blocker_.new_ptr();
    deadline_timer_.async_wait(strand_.wrap(
        [blocking_this, state](asio_error error) {
          if (state->done || error) return;
          blocking_this->report_std_error(
              std::move(state), std::make_error_code(std::errc::timed_out));
        }));
  }

  // Send the request, but first connect asynchronously if disconnected.
  if (!socket_.is_open()) {
    connect(std::move(state));
  } else {
    write_request(std::move(state));
  }
}

void length_framed_unbuffered_connection::connect(shared_request_state state,
                                                  size_t endpoint_index) {
  RIAKPP_CHECK_LT(endpoint_index, endpoints_.size());

  auto blocking_this = blocker_.new_ptr();
  auto callback = [blocking_this, state, endpoint_index](asio_error error) {
    if (state->done) return;
    if (error) {
      if (endpoint_index == blocking_this->endpoints_.size() - 1) {
        blocking_this->report(std::move(state), error);
      } else {
        blocking_this->connect(std::move(state), endpoint_index + 1);
      }
    } else {
      blocking_this->write_request(std::move(state));
    }
  };

  if (socket_.is_open()) socket_.close();
  socket_.async_connect(endpoints_[endpoint_index],
                        strand_.wrap(std::move(callback)));
}

void length_framed_unbuffered_connection::write_request(
    shared_request_state state) {
  if (state->done) return;

  auto& content = state->request.message;
  auto& length = state->request_length;
  length = byte_order::host_to_network_long(content.size());

  std::array<asio::const_buffer, 2> buffers = {{
      asio::buffer(reinterpret_cast<const uint8_t*>(&length), sizeof(length)),
      asio::buffer(content.data(), content.size())}};

  asio::async_write(
      socket_, std::move(buffers),
      strand_.wrap(std::bind(&self_type::wait_for_length, blocker_.new_ptr(),
                             state, ph::_1, ph::_2)));
}

void length_framed_unbuffered_connection::wait_for_length(
    shared_request_state state, asio_error error, size_t /*bytes*/) {
  if (state->done) return;
  if (error) {
    report(state, error);
    return;
  }

  // Clean up memory used to store the request.
  std::string().swap(state->request.message);

  auto& length = state->response_length;
  auto handler =
      std::bind(&self_type::wait_for_content, blocker_.new_ptr(),
                std::move(state), ph::_1, ph::_2);
  asio::async_read(
      socket_,
      asio::buffer(reinterpret_cast<uint8_t*>(&length), sizeof(length)),
      strand_.wrap(std::move(handler)));

}

void length_framed_unbuffered_connection::wait_for_content(
    shared_request_state state, asio_error error, size_t /*bytes*/) {
  if (state->done) return;
  if (error) {
    report(state, error);
    return;
  }

  auto& length = state->response_length;
  auto& content = state->response;
  length = byte_order::host_to_network_long(length);
  content.resize(length);

  auto handler = std::bind(&self_type::report,
                           blocker_.new_ptr(), state, ph::_1);
  asio::async_read(socket_, asio::buffer(&content[0], length),
                   strand_.wrap(std::move(handler)));
}

void length_framed_unbuffered_connection::report(shared_request_state state,
                                                 asio_error error) {
  // Convert boost::system_error to std::system_error.
  report_std_error(std::move(state),
                   std::make_error_code(static_cast<std::errc>(error.value())));
}

void length_framed_unbuffered_connection::report_std_error(
    shared_request_state state, std::error_code error) {
  if (state->done) return;
  RIAKPP_CHECK(current_request_state_.lock() == state);

  if (error && socket_.is_open()) socket_.close();
  deadline_timer_.cancel();

  state->done.store(true);
  has_active_request_.store(false);

  if (state->request.on_response) {
    auto handler = std::bind(state->request.on_response,
                             std::move(state->response), error);
    state.reset();
    handler();
  }
}

}  // namespace riak
