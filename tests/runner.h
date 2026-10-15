// SPDX-License-Identifier: Apache-2.0
// The test runner: RGUI_TEST("module.name") { ... } registers a test; CHECK, CHECK_EQ and
// CHECK_NEAR report a failure as "FAIL <test> <file>:<line>: <expr>" and let the test go on.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace rgui::test {

using TestFunction = void (*)();

struct Registrar {
    Registrar(const char* name, TestFunction function);
};

// Marks the running test failed and prints the failure.
void Fail(const char* file, int line, const std::string& expression);

// A value for a failure message: numbers, strings and booleans as they are, anything else "?".
template <typename T>
std::string Describe(const T& value) {
    using U = std::decay_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_same_v<U, char>) {
        return std::string("'") + value + "'";
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_integral_v<U>) {
        return std::to_string(static_cast<unsigned long long>(value));
    } else if constexpr (std::is_enum_v<U>) {
        return std::to_string(static_cast<long long>(value));
    } else if constexpr (std::is_floating_point_v<U>) {
        return std::to_string(static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<const U&, std::string_view>) {
        return "\"" + std::string(std::string_view(value)) + "\"";
    } else {
        return "?";
    }
}

// The directory of the source tree and of the build (CMake's binary directory).
const char* SourceDir();
const char* BuildDir();

} // namespace rgui::test

#define RGUI_TEST_CAT2(a, b) a##b
#define RGUI_TEST_CAT(a, b) RGUI_TEST_CAT2(a, b)
#define RGUI_TEST(name) \
    static void RGUI_TEST_CAT(rgui_test_, __LINE__)(); \
    static const ::rgui::test::Registrar RGUI_TEST_CAT(rgui_test_registrar_, __LINE__)( \
        name, &RGUI_TEST_CAT(rgui_test_, __LINE__)); \
    static void RGUI_TEST_CAT(rgui_test_, __LINE__)()

#define CHECK(cond) \
    do { \
        if (!(cond)) ::rgui::test::Fail(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        const auto& rgui_check_a_ = (a); \
        const auto& rgui_check_b_ = (b); \
        if (!(rgui_check_a_ == rgui_check_b_)) { \
            ::rgui::test::Fail(__FILE__, __LINE__, std::string(#a " == " #b " (") + \
                ::rgui::test::Describe(rgui_check_a_) + " vs " + \
                ::rgui::test::Describe(rgui_check_b_) + ")"); \
        } \
    } while (0)

#define CHECK_NEAR(a, b, eps) \
    do { \
        const double rgui_check_a_ = static_cast<double>(a); \
        const double rgui_check_b_ = static_cast<double>(b); \
        const double rgui_check_d_ = rgui_check_a_ > rgui_check_b_ ? rgui_check_a_ - rgui_check_b_ \
                                                                    : rgui_check_b_ - rgui_check_a_; \
        if (!(rgui_check_d_ <= static_cast<double>(eps))) { \
            ::rgui::test::Fail(__FILE__, __LINE__, std::string(#a " ~= " #b " (") + \
                ::rgui::test::Describe(rgui_check_a_) + " vs " + \
                ::rgui::test::Describe(rgui_check_b_) + ", eps " #eps ")"); \
        } \
    } while (0)
