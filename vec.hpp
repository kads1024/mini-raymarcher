#pragma once
#include <concepts>
#include <ostream>
#include <cmath>
#include <cassert>
#include <cstddef>

template<typename T>
concept Arithmetic = std::integral<T> || std::floating_point<T>;

template<Arithmetic T, size_t N>
struct vec_storage { T data[N]; };

template <Arithmetic T>
struct vec_storage<T, 2>
{
    union {
        struct {
            T x, y;
        };
        T data[2];
    };

    constexpr vec_storage() noexcept : x(T()), y(T()) {}
};

template <Arithmetic T>
struct vec_storage<T, 3>
{
    union {
        struct {
            T x, y, z;
        };
        T data[3];
    };

    constexpr vec_storage() noexcept : x(T()), y(T()), z(T()) {}
};

template <Arithmetic T>
struct vec_storage<T, 4>
{
    union {
        struct {
            T x, y, z, w;
        };
        T data[4];
    };

    constexpr vec_storage() noexcept : x(T()), y(T()), z(T()), w(T()) {}
};

// vec
template<Arithmetic T, size_t N>
struct vec : vec_storage<T, N>
{
	using vec_storage<T, N>::data;
	
    static constexpr size_t size = N;

    constexpr vec() noexcept = default;
    
    template<typename... Args>
    requires(sizeof...(Args) == N)
    constexpr explicit vec(Args... args) noexcept 
    {
        T tmp[] = { static_cast<T>(args)... };
        for(size_t i = 0; i < N; i++)
        {
            data[i] = tmp[i];
        }
    }

    template<Arithmetic U> 
    constexpr explicit vec(const vec<U, N>& other) noexcept 
    {
        for(size_t i = 0; i < N; i++)
        {
            data[i] = static_cast<T>(other[i]);
        }
    }

    [[nodiscard]]
    constexpr T& operator[](const size_t index) noexcept
    {
        assert(index < N);
        return data[index];
    }

    [[nodiscard]]
    constexpr const T& operator[](const size_t index) const noexcept
    {
        assert(index < N);
        return data[index];
    }

    [[nodiscard]]
    constexpr T* begin() noexcept
    {
        return data;
    }

    [[nodiscard]]
    constexpr const T* begin() const noexcept
    {
        return data;
    }

    [[nodiscard]]
    constexpr T* end() noexcept
    {
        return data + N;
    }

    [[nodiscard]]
    constexpr const T* end() const noexcept
    {
        return data + N;
    }

    [[nodiscard]]
    constexpr bool operator==(const vec& other) const noexcept = default;
};

// operation
template<Arithmetic T, size_t N>
[[nodiscard]]
constexpr vec<T, N> operator+(vec<T, N> lhs, const vec<T, N>& rhs) noexcept
{
    for(size_t i = 0; i < N; i++)
    {
        lhs[i] += rhs[i];
    }
    return lhs;
}

template<Arithmetic T, size_t N>
[[nodiscard]]
constexpr vec<T, N> operator-(vec<T, N> lhs, const vec<T, N>& rhs) noexcept
{
    for(size_t i = 0; i < N; i++)
    {
        lhs[i] -= rhs[i];
    }
    return lhs;   
}

template<Arithmetic T, size_t N>
[[nodiscard]]
constexpr vec<T, N> operator*(vec<T, N> lhs, T rhs) noexcept
{   
    for(size_t i = 0; i < N; i++)
    {
        lhs[i] *= rhs;
    }
    return lhs;
}

template<Arithmetic T, size_t N>
[[nodiscard]]
constexpr vec<T, N> operator*(T lhs, vec<T, N> rhs) noexcept
{
    return rhs * lhs;
}

template<Arithmetic T, size_t N>
[[nodiscard]]
constexpr vec<T, N> operator-(vec<T, N> v) noexcept
{
    for(size_t i = 0; i < N; i++)
    {
        v[i] *= -1;
    }

    return v;
}

// math
template<Arithmetic T, size_t N>
[[nodiscard]]
constexpr T dot(const vec<T, N>& lhs, const vec<T, N>& rhs) noexcept
{
    T result{};
    for(size_t i = 0; i < N; i++)
    {
        result += lhs[i] * rhs[i];
    }
    return result;
}

template<Arithmetic T>
[[nodiscard]]
constexpr vec<T, 3> cross(const vec<T, 3>& lhs, const vec<T, 3>& rhs) noexcept
{
    return vec<T, 3>
    {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x
    };
}

template<std::floating_point T, size_t N>
[[nodiscard]]
constexpr vec<T, N> normalized(const vec<T, N>& v) noexcept
{
    vec<T, N> unitV{};
    for(size_t i = 0; i < N; i++)
    {
        unitV[i] = v[i] / length(v);
    }
    return unitV;
}

template<std::floating_point T, size_t N>
[[nodiscard]]
T length(const vec<T, N>& v) noexcept
{
    return std::sqrt(dot(v, v));
}

// stream
template<Arithmetic T, size_t N>
std::ostream& operator<<(std::ostream& os, const vec<T, N>& v)
{
    for(size_t i = 0; i < N; i++)
    {
        os << v[i] << " ";
    }
    return os;
}

// typedefs
using Vec2f = vec<float, 2>;
using Vec3f = vec<float, 3>;
using Vec4f = vec<float, 4>;

using Vec2i = vec<int, 2>;
using Vec3i = vec<int, 3>;
using Vec4i = vec<int, 4>;