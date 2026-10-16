// Result.hpp
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace r4d
{

    enum class ErrorCode
    {
        Unknown,
        InvalidArgument,
        NotFound,
        IoFailure,
        UnsupportedFeature,
        DeviceCreationFailed,
        ApiCallFailed,
        ShaderLoadFailed,
    };

    constexpr std::string_view ToString(ErrorCode code)
    {
        switch (code)
        {
            case ErrorCode::Unknown: return "Unknown";
            case ErrorCode::InvalidArgument: return "InvalidArgument";
            case ErrorCode::NotFound: return "NotFound";
            case ErrorCode::IoFailure: return "IoFailure";
            case ErrorCode::UnsupportedFeature: return "UnsupportedFeature";
            case ErrorCode::DeviceCreationFailed: return "DeviceCreationFailed";
            case ErrorCode::ApiCallFailed: return "ApiCallFailed";
            case ErrorCode::ShaderLoadFailed: return "ShaderLoadFailed";
        }
        return "Unknown";
    }

    struct Error
    {
        ErrorCode code = ErrorCode::Unknown;
        std::string message;
        long hresult = 0;

        std::string ToString() const;
    };

    class [[nodiscard]] Status
    {
    public:
        Status() = default;
        Status(Error error) : m_error{std::move(error)} {}

        static Status Ok() { return Status{}; }

        bool ok() const { return !m_error.has_value(); }
        explicit operator bool() const { return ok(); }

        const Error& error() const
        {
            assert(!ok());
            return *m_error;
        }

    private:
        std::optional<Error> m_error;
    };

    template<typename T>
    class [[nodiscard]] Result
    {
    public:
        Result(T value) : m_storage{std::move(value)} {}
        Result(Error error) : m_storage{std::move(error)} {}

        bool ok() const { return std::holds_alternative<T>(m_storage); }
        explicit operator bool() const { return ok(); }

        T& value()
        {
            assert(ok());
            return std::get<T>(m_storage);
        }
        const T& value() const
        {
            assert(ok());
            return std::get<T>(m_storage);
        }

        const Error& error() const
        {
            assert(!ok());
            return std::get<Error>(m_storage);
        }

        Status status() const { return ok() ? Status::Ok() : Status{error()}; }

    private:
        std::variant<T, Error> m_storage;
    };

    inline Error makeError(ErrorCode code, std::string message, long hresult = 0)
    {
        return Error{code, std::move(message), hresult};
    }

} // namespace r4d

#define R4D_TRY(expr)                                                                    \
    do                                                                                   \
    {                                                                                    \
        auto _r4d_status = (expr);                                                       \
        if (!_r4d_status)                                                                \
            return _r4d_status.error();                                                  \
    } while (false)
