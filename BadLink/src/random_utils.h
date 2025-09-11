#ifndef BADLINK_SRC_RANDOM_UTILS_H_
#define BADLINK_SRC_RANDOM_UTILS_H_

#include <random>

namespace BadLink {

    class RandomUtils {
    public:
     
        [[nodiscard]] static float GetPercentage() {
            thread_local std::mt19937 rng{ std::random_device{}() };
            thread_local std::uniform_real_distribution<float> dist{ 0.0f, 100.0f };
            return dist(rng);
        }

        [[nodiscard]] static std::mt19937& GetGenerator() {
            thread_local std::mt19937 rng{ std::random_device{}() };
            return rng;
        }
    };

}
#endif  // BADLINK_SRC_RANDOM_UTILS_H_