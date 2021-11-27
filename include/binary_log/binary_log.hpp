#pragma once
#include <array>
#include <iostream>
#include <string_view>

#include <binary_log/fixed_string.hpp>
#include <binary_log/packer.hpp>
#include <binary_log/string_utils.hpp>

namespace binary_log
{
class binary_log
{
  std::FILE* m_index_file;
  std::FILE* m_log_file;
  uint8_t m_format_string_index {0};

  template<typename T>
  constexpr void pack_arg_in_index_file(T&& input)
  {
    // If rvalue, store the value in the index file
    // it does not need to go into every log entry in
    // the log file

    // TODO(pranav): Check if T is const char *
    // Save this to index file as well
    // even if the following checks fail

    if constexpr (std::is_rvalue_reference<T&&>::value) {
      constexpr bool is_rvalue = true;
      fwrite(&is_rvalue, sizeof(bool), 1, m_index_file);
      packer::pack_data(m_index_file, std::forward<T>(input));
    } else if constexpr (std::is_lvalue_reference<T&&>::value) {
      constexpr bool is_rvalue = false;
      fwrite(&is_rvalue, sizeof(bool), 1, m_index_file);
    } else {
      static_assert(!std::is_rvalue_reference<T&&>::value
                        && !std::is_lvalue_reference<T&&>::value,
                    "Unsupported type");
    }
  }

  template<class T, class... Ts>
  constexpr void pack_args_in_index_file(T&& first, Ts&&... rest)
  {
    pack_arg_in_index_file(std::forward<T>(first));

    if constexpr (sizeof...(rest) > 0) {
      pack_args_in_index_file(std::forward<Ts>(rest)...);
    }
  }

  template<typename T>
  constexpr void pack_arg(T&& input)
  {
    // If rvalue, store the value in the index file
    // it does not need to go into every log entry in
    // the log file

    if constexpr (std::is_lvalue_reference<T&&>::value) {
      packer::pack_data(m_log_file, std::forward<T>(input));
    }
  }

  template<class T, class... Ts>
  constexpr void pack_args(T&& first, Ts&&... rest)
  {
    pack_arg(std::forward<T>(first));

    if constexpr (sizeof...(rest) > 0) {
      pack_args(std::forward<Ts>(rest)...);
    }
  }

  template<typename T>
  constexpr void pack_arg_type()
  {
    if constexpr (std::is_same_v<T, char>) {
      packer::write_type<packer::datatype::type_char>(m_index_file);
    } else if constexpr (std::is_same_v<T, uint8_t>) {
      packer::write_type<packer::datatype::type_uint8>(m_index_file);
    } else if constexpr (std::is_same_v<T, uint16_t>) {
      packer::write_type<packer::datatype::type_uint16>(m_index_file);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
      packer::write_type<packer::datatype::type_uint32>(m_index_file);
    } else if constexpr (std::is_same_v<T, uint64_t>) {
      packer::write_type<packer::datatype::type_uint64>(m_index_file);
    } else if constexpr (std::is_same_v<T, int8_t>) {
      packer::write_type<packer::datatype::type_int8>(m_index_file);
    } else if constexpr (std::is_same_v<T, int16_t>) {
      packer::write_type<packer::datatype::type_int16>(m_index_file);
    } else if constexpr (std::is_same_v<T, int32_t>) {
      packer::write_type<packer::datatype::type_int32>(m_index_file);
    } else if constexpr (std::is_same_v<T, int64_t>) {
      packer::write_type<packer::datatype::type_int64>(m_index_file);
    } else if constexpr (std::is_same_v<T, float>) {
      packer::write_type<packer::datatype::type_float>(m_index_file);
    } else if constexpr (std::is_same_v<T, double>) {
      packer::write_type<packer::datatype::type_double>(m_index_file);
    }
  }

  template<class T, class... Ts>
  constexpr void pack_arg_types()
  {
    pack_arg_type<T>();

    if constexpr (sizeof...(Ts) > 0) {
      pack_arg_types<Ts...>();
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
    std::string index_file_path = std::string {path} + ".index";
    m_index_file = fopen(index_file_path.c_str(), "wb");
    if (m_index_file == nullptr) {
      throw std::invalid_argument("fopen failed");
    }
  }

  ~binary_log() noexcept
  {
    fclose(m_log_file);
    fclose(m_index_file);
  }

  template<fixed_string F, class... Args>
  constexpr inline uint8_t log_index(Args&&... args)
  {
    // SPEC:
    // <format-string-length> <format-string>
    // <number-of-arguments> <arg-type-1> <arg-type-2> ... <arg-type-N>
    // <arg-1-is-rvalue> <arg-1-value>? <arg-2-is-rvalue> <arg-2-value>? ...
    //
    // If the arg is an rvalue, it is stored in the index file
    // and the value is not stored in the log file
    constexpr char const* Name = F;
    constexpr uint8_t num_args = sizeof...(Args);

    m_format_string_index++;

    // Write the length of the format string
    constexpr uint8_t format_string_length = string_length(Name);
    fwrite(&format_string_length, 1, 1, m_index_file);

    // Write the format string
    fwrite(F, 1, format_string_length, m_index_file);

    // Write the number of args taken by the format string
    fwrite(&num_args, sizeof(uint8_t), 1, m_index_file);

    // Write the type of each argument
    if constexpr (num_args > 0) {
      pack_arg_types<Args...>();
      pack_args_in_index_file(std::forward<Args>(args)...);
    }

    return m_format_string_index - 1;
  }

  template<fixed_string F, class... Args>
  constexpr inline void log(uint8_t pos, Args&&... args)
  {
    constexpr uint8_t num_args = sizeof...(Args);

    // Write to the main log file
    // SPEC:
    // <format-string-index> <arg1> <arg2> ... <argN>
    // <format-string-index> is the index of the format string in the index file
    // <arg1> <arg2> ... <argN> are the arguments to the format string
    //
    // Each <arg> is a pair: <type, value>

    // Write the format string index
    fwrite(&pos, sizeof(uint8_t), 1, m_log_file);

    // Write the args
    if constexpr (num_args > 0) {
      pack_args(std::forward<Args>(args)...);
    }
  }
};

}  // namespace binary_log

#define BINARY_LOG(logger, format_string, ...) \
  static uint8_t CONCAT(format_string_id_pos, __LINE__) = \
      [&logger]<typename... Args>(Args && ... args) constexpr \
  { \
    return logger.log_index<format_string>(std::forward<Args>(args)...); \
  } \
  (__VA_ARGS__); \
  logger.log<format_string>(CONCAT(format_string_id_pos, __LINE__), \
                            ##__VA_ARGS__);
