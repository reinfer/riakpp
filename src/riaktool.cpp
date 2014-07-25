#include "connection_pool.hpp"
#include "debug_log.hpp"
#include "thread_pool.hpp"

#include <boost/asio/io_service.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>

#include <atomic>
#include <iostream>
#include <mutex>

namespace {
void wait_on_signal() {
  boost::asio::io_service service;
  boost::asio::signal_set signals(service, SIGINT, SIGTERM);
  signals.async_wait([&](...) { DLOG << "Signal caught."; service.stop(); });
  service.run();
}

template<class T, class U>
inline const T max(T a, U b) { return a < b ? b : a; }

template <class T>
double seconds_since(T& t) {
  using namespace std::chrono;
  auto now = high_resolution_clock::now();
  auto secs = duration_cast<milliseconds>(now - t).count() / 1000.0;
  t = now;
  return secs;
}

}  // namespace

int main(int argc, char *argv[]) {
  using namespace riak;
  using namespace std::chrono;
  namespace po = boost::program_options;

  // Parse arguments
  std::string hostname;
  int64_t deadline_ms;
  uint16_t port, num_threads, num_sockets;
  size_t highwatermark;
  uint32_t nmsgs;
  po::options_description description{
    "Sends a lot of get_object requests to a Riak node using a connection pool."
  };
  description.add_options()
      ("help,h", "prints this help message")
      ("hostname,n",
       po::value<std::string>(&hostname)->default_value("localhost"),
       "hostname of Riak node")
      ("port,p",
       po::value<uint16_t>(&port)->default_value(10017),
       "port to connect on Riak node")
      ("num-threads,t",
       po::value<uint16_t>(&num_threads)->default_value(2),
       "number of I/O threads")
      ("num-sockets,s",
       po::value<uint16_t>(&num_sockets)->default_value(256),
       "number of sockets in pool")
      ("highwatermark,k",
       po::value<size_t>(&highwatermark)->default_value(65536),
       "max buffered requests")
      ("nmsgs,m",
       po::value<uint32_t>(&nmsgs)->default_value(1000),
       "number of messages to send to the node")
      ("deadline,d",
       po::value<int64_t>(&deadline_ms)->default_value(5000),
       "Milliseconds before timing out a request. Negative for no deadline.");

  po::variables_map variables;
  try {
    po::store(po::parse_command_line(argc, argv, description), variables);
    po::notify(variables);
  } catch (const std::exception& e) {
    DLOG << e.what();
    return -1;
  }
  if (variables.count("help")) {
    std::cerr << description << std::endl;
    return -1;
  }

  // Simple connection_pool usage:
  //   riak::connection_pool conn(hostname, port, num_threads, num_sockets,
  //                              highwatermark);
  //   conn.send(string_message1, deadline_ms, handler);
  //   conn.send(string_message2, deadline_ms, handler);
  //   etc.
  //
  // What follows is a mess because this is throwaway code.

  std::mutex mutex;
  uint32_t num_sent{0}, num_failed{0};
  auto last_clock = high_resolution_clock::now();
  boost::asio::io_service service;
  riak::thread_pool threads{num_threads, &service};

  std::string message{"\x09\x0A\01\x62\x12\x01\x6B", 7};
  DLOG << "Creating connection pool...";

  std::unique_ptr<riak::connection_pool> conn{new riak::connection_pool{
      hostname, port, service, num_sockets, highwatermark}};

  DLOG << "Buffering messages... Don't Ctrl-C until done.";
  auto log_every = max(1u, nmsgs / 20u);
  for (size_t i = 0 ; i < nmsgs ; ++ i) {
    conn->send(message, deadline_ms,
               [&, i](std::string response, std::error_code error) {
      std::lock_guard<std::mutex> lock{mutex};
      ++num_sent;
      if (error) {
        ++num_failed;
        DLOG << "Failed: " << error.message() << " [message " << i << "].";
      } else if (response.empty() || response[0] != 10) {
        DLOG << "Bad reply from Riak: " << response.size() << " / "
             << static_cast<int>(response[0]);
      } else if (num_sent == 1) {
        double secs = seconds_since(last_clock);
        DLOG << error.message() << " [first message " << secs << " secs].";
      } else if (num_sent % log_every == 0 || num_sent == nmsgs) {
        auto msgs_per_sec = log_every / seconds_since(last_clock);
        DLOG << error.message() << " [sent " << num_sent << " at "
             << msgs_per_sec << " messages/sec]";
      }

      if (num_sent == nmsgs)
        service.post([&] {
          DLOG << "All messages sent.";
          conn.reset();
          service.stop();
        });
    });

    if (i % (log_every * 4) == 0) DLOG << "Buffered " << i + 1 << " messages.";
  }
  DLOG << "Buffered all the messages.";

  wait_on_signal();
  conn.reset();
  service.stop();
  DLOG << "Destroying connection pool and cancelling any remaining requests...";
  conn.reset();
  DLOG << "Done. " << (num_sent - num_failed) << " out of " << num_sent
       << " messages successful.";

  return 0;
}
