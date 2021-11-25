#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <moodycamel/concurrentqueue.h>
#define FMT_HEADER_ONLY
#include <sstream>

#include <fmt/args.h>
#include <fmt/format.h>
#include <fmt/os.h>
#include <msgpack.hpp>
#include <msgpack/fbuffer.hpp>

enum class arg_type
{
  type_size_t
};
MSGPACK_ADD_ENUM(arg_type);

class binary_log
{
  std::FILE* m_index_file;
  std::FILE* m_log_file;

  struct MyTraits : public moodycamel::ConcurrentQueueDefaultTraits
  {
    static const size_t BLOCK_SIZE = 256;  // Use bigger blocks
  };

  std::thread m_formatter_thread;
  moodycamel::ConcurrentQueue<std::function<void()>, MyTraits>
      m_formatter_queue;
  std::atomic_size_t m_enqueued_for_formatting {0};
  std::mutex m_formatter_mutex;
  std::condition_variable m_formatter_data_ready;

  std::atomic_size_t m_running {true};

  // Format string table
  std::unordered_map<std::string_view, std::size_t> m_format_string_table;
  std::atomic_size_t m_format_string_index {0};

  void formatter_thread_function()
  {
    std::function<void()> format_function;
    while (m_running || m_enqueued_for_formatting > 0) {
      // // Wait for the `enqueued` signal
      // {
      //   std::unique_lock<std::mutex> lock {m_formatter_mutex};
      //   m_formatter_data_ready.wait(
      //       lock,
      //       [this] { return m_enqueued_for_formatting > 0 || !m_running; });
      // }

      if (m_formatter_queue.try_dequeue(format_function)) {
        format_function();
        m_enqueued_for_formatting--;
      }
    }
  }

  void formatter_thread_function_bulk()
  {
    constexpr std::size_t bulk_dequeue_size = 32;
    std::function<void()> format_functions[bulk_dequeue_size];
    while (m_running || m_enqueued_for_formatting > 0) {
      // Wait for the `enqueued` signal
      {
        std::unique_lock<std::mutex> lock {m_formatter_mutex};
        m_formatter_data_ready.wait(lock,
                                    [this] {
                                      return m_enqueued_for_formatting
                                          >= bulk_dequeue_size
                                          || !m_running;
                                    });
      }

      if (m_formatter_queue.try_dequeue_bulk(format_functions,
                                             bulk_dequeue_size)) {
        for (std::size_t i = 0; i < bulk_dequeue_size; i++) {
          if (format_functions[i]) {
            format_functions[i]();
            m_enqueued_for_formatting--;
          }
        }
      }
    }
  }

  // TODO(pranav): Add overloads of this function for all supported fmt arg
  // types
  constexpr static inline arg_type get_arg_type(std::size_t)
  {
    return arg_type::type_size_t;
  }

  template<typename T>
  constexpr static inline void pack_arg(msgpack::fbuffer& os, T&& arg)
  {
    msgpack::pack(os, get_arg_type(arg));
    msgpack::pack(os, arg);
  }

  template<class T, class... Ts>
  constexpr static inline void pack_args(msgpack::fbuffer& os,
                                         T const& first,
                                         Ts const&... rest)
  {
    pack_arg(os, first);

    if constexpr (sizeof...(rest) > 0) {
      pack_args(os, rest...);
    }
  }

public:
  binary_log(std::string_view path)
  {
    // Create the log file
    // All the log contents go here
    m_log_file = fopen(path.data(), "wb");
    if (m_log_file == nullptr) {
      throw std::invalid_argument("fopen failed");
    }

    // Create the index file
    // All the format_strings go here
    // into a table
    // FORMAT_STRING_1 -> 1
    // FORMAT_STRING_2 -> 2
    // ...
    // FORMAT_STRING_N -> N
    std::string index_file_path = std::string {path} + ".index";
    m_index_file = fopen(index_file_path.c_str(), "wb");
    if (m_index_file == nullptr) {
      throw std::invalid_argument("fopen failed");
    }

    m_formatter_thread =
        std::thread {&binary_log::formatter_thread_function_bulk, this};
  }

  ~binary_log()
  {
    m_running = false;
    m_formatter_data_ready.notify_all();
    m_formatter_thread.join();
  }

  enum class level
  {
    debug,
    info,
    warn,
    error,
    fatal
  };

  template<typename... Args>
  void log(const level& level,
           const std::string_view& format_string,
           Args&&... args)
  {
    // Schedule write to log file
    m_formatter_queue.enqueue(
        [this, level, format_string, args...]()
        {
          // Update format string table if necessary
          if (m_format_string_table.find(format_string)
              == m_format_string_table.end()) {
            m_format_string_table[format_string] = m_format_string_index++;

            msgpack::fbuffer os(m_index_file);
            msgpack::pack(os, format_string.size());
            msgpack::pack(os, format_string);
          }

          // Serialize log message
          msgpack::fbuffer os(m_log_file);
          msgpack::pack(os, static_cast<uint8_t>(level));
          msgpack::pack(os, m_format_string_table[format_string]);
          msgpack::pack(os, sizeof...(args));
          pack_args(os, args...);
        });

    m_enqueued_for_formatting += 1;
    m_formatter_data_ready.notify_one();
  }
};