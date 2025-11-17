#pragma once

#include <switch.h>

namespace sphaira {

class App {
public:
    static App* GetApp() {
        return nullptr;
    }

    static void SetAutoSleepDisabled(bool enable) {
        appletSetAutoSleepDisabled(enable);
    }

    static bool IsFileBaseEmummc() {
        return false;
    }
};

} // namespace sphaira
