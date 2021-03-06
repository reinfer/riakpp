RIAKPP
======

## Overview
riakpp is a C++(11) client for the [riak](http://basho.com/riak) distributed data store, built on top of the excellent [Boost.Asio](http://www.boost.org/doc/libs/1_55_0/doc/html/boost_asio.html) for TCP asynchronous I/O. However, it optionally encapsulates all boost classes so don't worry if boost isn't your cup of tea.

## Features
riakpp is still very new and shouldn't be considered production-ready. Here's what's already implemented:
* **PBC protocol.** Support for store, fetch and remove operations.
* **Fully asynchronous, multi-threaded socket pool.** By default a single thread runs handlers for a maximum of 8 simultaneous requests. These values are easily customizable though.
* **Completely cutom sibling resolution.** Iterate over siblings and optionally store resolution back automatically (with multiple attempts)
* **Value-semantics and clear ownership rules.** We deal with lifetime in an asynchronous environment the right way, not by making everything a shared_ptr.

## Minimal Example
```c++
#include <riakpp/client.hpp>

int main() {
  riak::client client{"localhost", 8087};

  client.async_store("example_bucket", "example_key", "hello, world!",
                     [&](std::error_code error) {
                        if (error) std::cerr << error.message() << ".\n";
                        client.stop_managed();  // Unblocks main thread.
                      });
  client.run_managed();  // Block until client.stop_managed().
}
```

## Getting Started
### Dependencies
The library depends on **Boost.Asio**, **Boost.System**, **libprotobuf** and **protoc** (Protocol Buffers library and compiler, respectively). On an Ubuntu/Debian-based system these can be installed using
```
sudo apt-get install libboost-dev libboost-system-dev libprotobuf-dev protobuf-compiler
```

### Building and Installing
The project must first be configured using cmake (specify -DBUILD_EXAMPLES=1 and/or -DBUILD_TESTS=1 if you want them built as well)
```
mkdir build && cd build && cmake ..
```
Then built you can build and install it with the traditional
```
make && sudo make install
```
Builds are tested on Clang 3.5 and on g++-4.8 and occasionally on g++-4.9.

## More examples
### Providing your own asio::io_service

If you don't provide an **io\_service**, the **client** object creates and manages one (as well as a thread pool). Chances are, though, if your application uses Asio elsewhere, you probably want to provide your own **io_service** and do your own thread management. You can do this when you construct a **client**:

```c++
#include <boost/asio/io_service.hpp>
#include <riakpp/client.hpp>

int main() {
  boost::asio::io_service io_service;
  boost::asio::io_service::work work{io_service};
  riak::client client{io_service, "localhost", 8087};

  client.async_store("example_bucket", "example_key", "hello, world!",
                     [&](std::error_code error) {
                        if (error) std::cerr << error.message() << ".\n";
                        io_service.stop();
                      });
  io_service.run();
}
```
No additional threads are created in this method, so you need to have at least a thread running ``io_service.run()`` for anything to happen.

### Synchronous API
First, you probably shouldn't use a synchronous API: not only is it inefficient, but in a multithreaded environment it's a deadlock waiting to happen. There aren't any non-async methods defined, but we provide a mechanism called a **blocking\_group**  which allows you to wrap handlers and then block until they're called. Here's an example for storing, fetching and removing an object: 
```c++
#include <riakpp/blocking_group.hpp>
#include <riakpp/client.hpp>

int main() {
  riak::client client{"localhost", 8087};

  riak::blocking_group blocker;
  std::error_code error;
  client.async_store("example_bucket", "example_key", "hello, world!",
                     blocker.wrap([&](std::error_code store_error) {
                       error = store_error;  // Save to variable outside scope.
                     }));

  // Wait until all the wrapped handlers have been called.
  blocker.wait();
  if (error) { std::cerr << error.message() << std::endl; return 1; }
  blocker.reset();  // Reset the group to allow reuse.

  // Wrapping a handler just to save a variable would be cumbersome, so you
  // use for convenience, you can replace
  //    blocker.wrap([&] (type1 arg1, type2 arg2, ...) {
  //      var1 = arg1;
  //      var2 = arg2;
  //      ...
  //    });
  // with blocker.save(var1, var2, ...). For instance, the fetch handler has
  // signature void(std::error_code, riak::object), hence we can write:
  riak::object fetched{"example_bucket", "example_key"};
  client.async_fetch(fetched, blocker.save(error, fetched));
  blocker.wait();
  if (error) { std::cerr << error.message() << std::endl; return 1; }
  blocker.reset();

  std::cout << "Fetched value '" << fetched.value() << "'." << std::endl;

  // Finally, let's remove the object. Again we can use save() to get the error.
  client.async_remove(fetched, blocker.save(error));
  blocker.wait();
  if (error) { std::cerr << error.message() << std::endl; return 1; }

  // Notice we don't reset blocker again. If a 'pending' blocking_group is
  // destroyed the process is aborted -- think of it as destroying an unjoined
  // thread. A blocking_group is pending when it accepts calls to wrap() and
  // wait(): after construction or after a call to reset() and stops being
  // pending after a call to wait().
  //
  // Redundant calls to wait() are OK and simply don't do anything.
  return 0;
}
```
**Note:** If you are going to use this method you should try not mix it with asynchronous calls, since you may accidentally cause a deadlock by blocking in handlers and occupying all the worker threads, preventing the very callbacks you're waiting on to be called.

### Sibling Resolution
First make sure that the bucket you're using allows siblings (i.e. in the riak config set allow_mult=1). Running any of the examples so far in such bucket would have inadvertently created siblings since we were storing without fetching first. A better version of the first example would then be:
```c++
#include <riakpp/client.hpp>

int main() {
  riak::client client{"localhost", 8087};

  // Nested lambdas to fetch, modify and store the object.
  std::error_code exit_with;
  client.async_fetch(
      "example_bucket", "example_key",
      [&](std::error_code fetch_error, riak::object fetched) {
        if (fetch_error) {
          exit_with = fetch_error;
          client.stop_managed();
          return;
        }
        fetched.value() = "hello, world!";
        client.async_store(fetched,
                           [&](std::error_code store_error) {
                             exit_with = store_error;
                             client.stop_managed();
                           });
      });
  client.run_managed();  // Block until client.managed_stop().

  if (exit_with) {
    std::cerr << "Error: " << exit_with.message() << std::endl;
    return 1;
  }
  return 0;
}
```

This still does not deal with the problem of siblings however. To do this we should first discuss the ``riak::object`` class in a bit more detail.  If you call ``.value()`` on a fetched object with multiple siblings, the process will abort since they must first be resolved.

To check if this is the case you can call the ``.in_conflict()`` method on the object which returns true when there are multiple siblings. To resolve the object, you can iterate through the siblings and resolve the conflict either with new content or with one of the siblings. For instance, to pick the sibling with the longest value:
```c++
size_t max_length = 0;
const riak::object::content* max_length_sibling = nullptr;
for (auto& sibling : conflicted.siblings()) {
  if (sibling.value().length() >= max_length) {
    max_length = sibling.value().length();
    max_length_sibling = &sibling;
  }
}
conflicted.resolve_with(*max_length_sibling);
```

A **client** can automatically check if a fetched object is in conflict and calls a sibling resolution function before calling the fetch handler. You can pass such a function to the client on construction. To add the previous sibling resolution function to the example:

```c++
#include <riakpp/client.hpp>

riak::store_resolved_sibling max_length_resolution(riak::object& conflicted) {
  size_t max_length = 0;
  const riak::object::content* max_length_sibling = nullptr;
  for (auto& sibling : conflicted.siblings()) {
    if (sibling.value().length() >= max_length) {
      max_length = sibling.value().length();
      max_length_sibling = &sibling;
    }
  }
  conflicted.resolve_with(*max_length_sibling);

  // Returning yes means we want riakpp to make a store() call with the resolved
  // object before calling the fetch handler.
  return riak::store_resolved_sibling::yes;
}

int main() {
  riak::client client{"localhost", 8087, &max_length_resolution};
...
```
The rest of the code is unchanged, since the sibling resolution function is called automatically on any fetch.

## Connection Options
There are a number of configurable parameters for the connection that can be chosen on construction. Here's an example that sets everything that can be set:
```c++
riak::client client(
    "example.com", port_number, &sibling_resolution_function,
    riak::connection_options{}
        .num_worker_threads(4)        //   Thread pool size, managed-mode only.
                                      // (default:1)

        .max_connections(128)         //   Socket pool size. (default:8)

        .deadline_ms(500)             //   Request timeout in ms. (default:3000)

        .highwatermark(65536)         //   Request buffer size, will block if more
                                      // requests are added. (default:4096)

        .connection_timeout_ms(1000)  //   Timeout when connecting to a node.
                                      // (default:1500)
);
```
