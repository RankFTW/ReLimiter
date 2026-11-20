// Direct production policy tests (C++17, src include path), not a copied model.
// Intended target: test_osd_position; CTest name: osd_position.
// Registration is left to the integration owner. These tests do not validate
// ImGui rendering, key capture/polling, INI I/O, or the OLED worker at runtime.
#include "osd_position_policy.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Check(bool condition, const char* expression) {
    if (condition) return;
    std::cerr << "FAILED: " << expression << '\n';
    std::exit(1);
}

#define CHECK(expression) Check((expression), #expression)

void CheckNext(float x, float y, float expected_x, float expected_y) {
    const auto next = osd_position_policy::NextClockwise(x, y);
    if (next.x != expected_x || next.y != expected_y) {
        std::cerr << "NextClockwise(" << x << ", " << y << ") = ("
                  << next.x << ", " << next.y << "), expected ("
                  << expected_x << ", " << expected_y << ")\n";
        std::exit(1);
    }
    CHECK(next.x == 0.0f || next.x == 1.0f);
    CHECK(next.y == 0.0f || next.y == 1.0f);
}

void TestClockwiseCorners() {
    CheckNext(0.0f, 0.0f, 1.0f, 0.0f);
    CheckNext(1.0f, 0.0f, 1.0f, 1.0f);
    CheckNext(1.0f, 1.0f, 0.0f, 1.0f);
    CheckNext(0.0f, 1.0f, 0.0f, 0.0f);
    CheckNext(0.005f, 0.005f, 1.0f, 0.0f);
    CheckNext(0.27f, 0.27f, 1.0f, 0.0f);
    CheckNext(0.73f, 0.27f, 1.0f, 1.0f);
    CheckNext(0.73f, 0.73f, 0.0f, 1.0f);
    CheckNext(0.27f, 0.73f, 0.0f, 0.0f);

    osd_position_policy::Position position{0.0f, 0.0f};
    for (int i = 0; i < 4; ++i)
        position = osd_position_policy::NextClockwise(position.x, position.y);
    CHECK(position.x == 0.0f && position.y == 0.0f);
}

void TestThresholdAndPivot() {
    const float below = std::nextafter(0.5f, 0.0f);
    const float above = std::nextafter(0.5f, 1.0f);
    CHECK(osd_position_policy::Pivot(0.0f) == 0.0f);
    CHECK(osd_position_policy::Pivot(0.005f) == 0.0f);
    CHECK(osd_position_policy::Pivot(below) == 0.0f);
    CHECK(osd_position_policy::Pivot(0.5f) == 0.0f); // Strict > for pivot.
    CHECK(osd_position_policy::Pivot(above) == 1.0f);
    CHECK(osd_position_policy::Pivot(0.73f) == 1.0f);
    CHECK(osd_position_policy::Pivot(1.0f) == 1.0f);

    CheckNext(below, below, 1.0f, 0.0f);
    CheckNext(0.5f, below, 1.0f, 1.0f); // Inclusive >= for classification.
    CheckNext(below, 0.5f, 0.0f, 0.0f);
    CheckNext(0.5f, 0.5f, 0.0f, 1.0f);
    CheckNext(above, above, 0.0f, 1.0f);

    // Both axes use the same policy; exact corners keep the full extent inside.
    const float corners[] = {0.0f, 1.0f};
    for (float corner : corners) {
        const float leading_edge = corner * 1920.0f - osd_position_policy::Pivot(corner) * 320.0f;
        CHECK(leading_edge >= 0.0f);
        CHECK(leading_edge + 320.0f <= 1920.0f);
    }
}

void TestFiniteAndClamp() {
    using osd_position_policy::ClampCoordinate;
    CHECK(ClampCoordinate(-10.0f) == 0.0f);
    CHECK(ClampCoordinate(10.0f) == 1.0f);
    CHECK(ClampCoordinate(0.0f) == 0.0f);
    CHECK(ClampCoordinate(1.0f) == 1.0f);
    CHECK(ClampCoordinate(0.005f) == 0.005f);
    CHECK(ClampCoordinate(0.5f) == 0.5f);
    CHECK(ClampCoordinate(0.73f) == 0.73f);

    const float invalid[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()
    };
    for (float value : invalid) {
        CHECK(ClampCoordinate(value) == 0.005f);
        CHECK(std::isfinite(ClampCoordinate(value)));
        CHECK(osd_position_policy::Pivot(value) == 0.0f);
        CheckNext(value, 0.73f, 0.0f, 0.0f);
        CheckNext(0.73f, value, 1.0f, 1.0f);
        CheckNext(value, value, 1.0f, 0.0f);
    }
    CheckNext(-10.0f, 10.0f, 0.0f, 0.0f);
    CheckNext(10.0f, -10.0f, 1.0f, 1.0f);
}

} // namespace

int main() {
    TestClockwiseCorners();
    TestThresholdAndPivot();
    TestFiniteAndClamp();
    std::cout << "OSD position policy tests passed\n";
    return 0;
}