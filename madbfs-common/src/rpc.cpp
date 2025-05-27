#include "madbfs-common/rpc.hpp"

#include <cstring>

namespace
{
    using madbfs::Opt;
    using madbfs::Span;
    using madbfs::u8;

    template <std::integral T>
    constexpr T swap_endian(const T& value) noexcept
    {
        if constexpr (std::endian::native == std::endian::big) {
            return value;
        } else {
            constexpr auto size  = sizeof(T);
            auto           array = std::bit_cast<std::array<std::byte, size>>(value);
            for (auto i = 0u; i < size / 2; ++i) {
                std::swap(array[i], array[size - i - 1]);
            }
            return std::bit_cast<T>(array);
        }
    }

    Opt<Span<const u8>> read_incr(Span<const u8> buffer, std::size_t& index, std::size_t size)
    {
        if (index + size > buffer.size()) {
            return std::nullopt;
        }

        auto span  = buffer.subspan(index, size);
        index     += size;

        return span;
    }
}

namespace madbfs::rpc
{
    enum class Type : u8
    {
        Bool = 0,
        I32,
        I64,
        U64,
        Bytes,
    };

    struct Request
    {
        Procedure procedure;
        Span<u8>  parameter;
    };

    struct Response
    {
        Procedure procedure;
        u8        status;    // posix error codes
        Span<u8>  result;
    };

    class PayloadBuilder
    {
    public:
        PayloadBuilder(Vec<u8>& buffer)
            : m_buffer{ buffer }
        {
        }

        PayloadBuilder& add_bool(bool value);
        PayloadBuilder& add_i32(i32 value);
        PayloadBuilder& add_i64(i64 value);
        PayloadBuilder& add_bytes(Span<const u8> bytes);

        Span<u8> build() &&;

    private:
        Vec<u8> m_buffer;
        usize   m_index;
    };

    class PayloadParser
    {
    public:
        PayloadParser(const Span<const u8> buffer)
            : m_buffer{ buffer }
        {
        }

        Opt<Type> peek() const;

        Opt<bool>           read_bool();
        Opt<i32>            read_i32();
        Opt<i64>            read_i64();
        Opt<Span<const u8>> read_bytes();

    private:
        Opt<Type> next_is(Type type);

        Span<const u8> m_buffer;
        usize          m_index;
    };

    PayloadBuilder& PayloadBuilder::add_bool(bool value)
    {
        m_buffer.push_back(static_cast<u8>(Type::Bool));
        m_buffer.push_back(std::bit_cast<u8>(value));
        return *this;
    }

    PayloadBuilder& PayloadBuilder::add_i32(i32 value)
    {
        auto net_value = swap_endian(value);
        auto bytes     = std::bit_cast<std::array<u8, sizeof(i32)>>(net_value);
        m_buffer.push_back(static_cast<u8>(Type::I32));
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
        return *this;
    }

    PayloadBuilder& PayloadBuilder::add_i64(i64 value)
    {
        auto net_value = swap_endian(value);
        auto bytes     = std::bit_cast<std::array<u8, sizeof(i64)>>(net_value);
        m_buffer.push_back(static_cast<u8>(Type::I64));
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
        return *this;
    }

    PayloadBuilder& PayloadBuilder::add_bytes(Span<const u8> bytes)
    {
        auto net_size   = swap_endian(bytes.size());
        auto bytes_size = std::bit_cast<std::array<u8, sizeof(std::size_t)>>(net_size);
        m_buffer.push_back(static_cast<u8>(Type::Bytes));
        m_buffer.insert(m_buffer.end(), bytes_size.begin(), bytes_size.end());
        return *this;
    }
}

namespace madbfs::rpc
{
    Opt<Type> PayloadParser::peek() const
    {
        if (m_index < m_buffer.size()) {
            auto type = static_cast<Type>(m_buffer[m_index]);
            switch (type) {
            case Type::Bool:
            case Type::I32:
            case Type::I64:
            case Type::Bytes: return type;
            };
        }

        return std::nullopt;
    }

    Opt<Type> PayloadParser::next_is(Type type)
    {
        if (m_index < m_buffer.size() and type == static_cast<Type>(m_buffer[m_index++])) {
            return type;
        }
        return std::nullopt;
    }

    Opt<bool> PayloadParser::read_bool()
    {
        return next_is(Type::Bool)
            .and_then([&](auto) { return read_incr(m_buffer, m_index, 1); })
            .transform([](Span<const u8> bytes) { return bytes[0] == 1; });
    }

    Opt<i32> PayloadParser::read_i32()
    {
        return next_is(Type::I32).and_then([&](auto) { return read_incr(m_buffer, m_index, sizeof(i32)); }
        ).transform([](Span<const u8> bytes) {
            i32 value;
            std::memcpy(&value, bytes.data(), bytes.size());
            return swap_endian(value);
        });
    }

    Opt<i64> PayloadParser::read_i64()
    {
        return next_is(Type::I64).and_then([&](auto) { return read_incr(m_buffer, m_index, sizeof(i64)); }
        ).transform([](Span<const u8> bytes) {
            i64 value;
            std::memcpy(&value, bytes.data(), bytes.size());
            return swap_endian(value);
        });
    }

    Opt<Span<const u8>> PayloadParser::read_bytes()
    {
        return next_is(Type::Bytes)
            .and_then([&](auto) { return read_incr(m_buffer, m_index, sizeof(std::size_t)); })
            .and_then([&](Span<const u8> bytes_size) {
                std::size_t size;
                std::memcpy(&size, bytes_size.data(), bytes_size.size());
                return read_incr(m_buffer, m_index, size);
            });
    }
}
