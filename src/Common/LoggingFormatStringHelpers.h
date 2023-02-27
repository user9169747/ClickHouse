#pragma once
#include <base/defines.h>
#include <fmt/format.h>

struct PreformattedMessage;
consteval void formatStringCheckArgsNumImpl(std::string_view str, size_t nargs);
template <typename T> constexpr std::string_view tryGetStaticFormatString(T && x);

/// Extract format string from a string literal and constructs consteval fmt::format_string
template <typename... Args>
struct FormatStringHelperImpl
{
    std::string_view message_format_string;
    fmt::format_string<Args...> fmt_str;
    template<typename T>
    consteval FormatStringHelperImpl(T && str) : message_format_string(tryGetStaticFormatString(str)), fmt_str(std::forward<T>(str))
    {
        formatStringCheckArgsNumImpl(message_format_string, sizeof...(Args));
    }
    template<typename T>
    FormatStringHelperImpl(fmt::basic_runtime<T> && str) : message_format_string(), fmt_str(std::forward<fmt::basic_runtime<T>>(str)) {}

    PreformattedMessage format(Args && ...args) const;
};

template <typename... Args>
using FormatStringHelper = FormatStringHelperImpl<std::type_identity_t<Args>...>;

/// Saves a format string for already formatted message
struct PreformattedMessage
{
    std::string text;
    std::string_view format_string;

    template <typename... Args>
    static PreformattedMessage create(FormatStringHelper<Args...> fmt, Args &&... args);

    operator const std::string & () const { return text; }
    operator std::string () && { return std::move(text); }
    operator fmt::format_string<> () const { UNREACHABLE(); }
};

template <typename... Args>
PreformattedMessage FormatStringHelperImpl<Args...>::format(Args && ...args) const
{
    return PreformattedMessage{fmt::format(fmt_str, std::forward<Args>(args)...), message_format_string};
}

template <typename... Args>
PreformattedMessage PreformattedMessage::create(FormatStringHelper<Args...> fmt, Args && ...args)
{
    return fmt.format(std::forward<Args>(args)...);
}

template<typename T> struct is_fmt_runtime : std::false_type {};
template<typename T> struct is_fmt_runtime<fmt::basic_runtime<T>> : std::true_type {};

template <typename T> constexpr std::string_view tryGetStaticFormatString(T && x)
{
    /// Format string for an exception or log message must be a string literal (compile-time constant).
    /// Failure of this assertion may indicate one of the following issues:
    ///  - A message was already formatted into std::string before passing to Exception(...) or LOG_XXXXX(...).
    ///    Please use variadic constructor of Exception.
    ///    Consider using PreformattedMessage or LogToStr if you want to avoid double formatting and/or copy-paste.
    ///  - A string literal was converted to std::string (or const char *).
    ///  - Use Exception::createRuntime or fmt::runtime if there's no format string
    ///    and a message is generated in runtime by a third-party library
    ///    or deserialized from somewhere.
    static_assert(!std::is_same_v<std::string, std::decay_t<T>>);

    if constexpr (is_fmt_runtime<std::decay_t<T>>::value)
    {
        /// It definitely was fmt::runtime(something).
        /// We are not sure about a lifetime of the string, so return empty view.
        /// Also it can be arbitrary string, not a formatting pattern.
        /// So returning empty pattern will not pollute the set of patterns.
        return std::string_view();
    }
    else
    {
        if constexpr (std::is_same_v<PreformattedMessage, std::decay_t<T>>)
        {
            return x.format_string;
        }
        else
        {
            /// Most likely it was a string literal.
            /// Unfortunately, there's no good way to check if something is a string literal.
            /// But fmtlib requires a format string to be compile-time constant unless fmt::runtime is used.
            static_assert(std::is_nothrow_convertible<T, const char * const>::value);
            static_assert(!std::is_pointer<T>::value);
            return std::string_view(x);
        }
    }
}

template <typename... Ts> constexpr size_t numArgs(Ts &&...) { return sizeof...(Ts); }
template <typename T, typename... Ts> constexpr auto firstArg(T && x, Ts &&...) { return std::forward<T>(x); }
/// For implicit conversion of fmt::basic_runtime<> to char* for std::string ctor
template <typename T, typename... Ts> constexpr auto firstArg(fmt::basic_runtime<T> && data, Ts &&...) { return data.str.data(); }

consteval ssize_t formatStringCountArgsNum(const char * const str, size_t len)
{
    /// It does not count named args, but we don't use them
    size_t cnt = 0;
    size_t i = 0;
    while (i + 1 < len)
    {
        if (str[i] == '{' && str[i + 1] == '}')
        {
            i += 2;
            cnt += 1;
        }
        else if (str[i] == '{')
        {
            /// Ignore checks for complex formatting like "{:.3f}"
            return -1;
        }
        else
        {
            i += 1;
        }
    }
    return cnt;
}

[[noreturn]] void functionThatFailsCompilationOfConstevalFunctions(const char * error);

/// fmt::format checks that there are enough arguments, but ignores extra arguments (e.g. fmt::format("{}", 1, 2) compiles)
/// This function will fail to compile if the number of "{}" substitutions does not exactly match
consteval void formatStringCheckArgsNumImpl(std::string_view str, size_t nargs)
{
    if (str.empty())
        return;
    ssize_t cnt = formatStringCountArgsNum(str.data(), str.size());
    if (0 <= cnt && cnt != nargs)
        functionThatFailsCompilationOfConstevalFunctions("unexpected number of arguments in a format string");
}

template <typename... Args>
struct CheckArgsNumHelperImpl
{
    template<typename T>
    consteval CheckArgsNumHelperImpl(T && str)
    {
        formatStringCheckArgsNumImpl(tryGetStaticFormatString(str), sizeof...(Args));
    }

    /// No checks for fmt::runtime and PreformattedMessage
    template<typename T> CheckArgsNumHelperImpl(fmt::basic_runtime<T> &&) {}
    template<> CheckArgsNumHelperImpl(PreformattedMessage &) {}
    template<> CheckArgsNumHelperImpl(const PreformattedMessage &) {}
    template<> CheckArgsNumHelperImpl(PreformattedMessage &&) {}

};

template <typename... Args> using CheckArgsNumHelper = CheckArgsNumHelperImpl<std::type_identity_t<Args>...>;
template <typename... Args> void formatStringCheckArgsNum(CheckArgsNumHelper<Args...>, Args &&...) {}
