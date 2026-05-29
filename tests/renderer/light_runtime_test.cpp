#include "renderer/light_runtime.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearly_equal(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

}  // namespace

int main() {
    using namespace renderer;

    {
        const auto atten = build_oed_light_attenuation(10.0f);
        expect(nearly_equal(atten[0], 1.0f),
               "OED light attenuation should keep the constant term at 1");
        expect(nearly_equal(atten[1], 0.0f),
               "OED light attenuation should keep the linear term at 0");
        expect(nearly_equal(atten[2], 15.0f / (12.5f * 12.5f)),
               "OED light attenuation should use the recovered 15/r^2 coefficient");
        expect(nearly_equal(atten[3], 1.0f),
               "OED light attenuation should preserve the trailing 1 term");
    }

    return 0;
}
