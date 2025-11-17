#pragma once

#include <switch.h>
#include <string>
#include <vector>
#include <span>

namespace sphaira::ui {

struct ProgressBox {
    ProgressBox() {
        ueventCreate(&m_uevent, true);
    }

    ProgressBox& SetActionName(const std::string& action) {
        m_action = action;
        return *this;
    }

    ProgressBox& SetTitle(const std::string& title) {
        m_title = title;
        return *this;
    }

    ProgressBox& NewTransfer(const std::string& transfer) {
        m_transfer = transfer;
        m_offset = 0;
        m_size = 0;
        return *this;
    }

    ProgressBox& UpdateTransfer(s64 offset, s64 size) {
        m_offset = offset;
        m_size = size;
        return *this;
    }

    ProgressBox& SetImage(int) {
        return *this;
    }

    ProgressBox& SetImageData(std::vector<u8>& data) {
        m_image_data = data;
        return *this;
    }

    ProgressBox& SetImageDataConst(std::span<const u8> data) {
        m_image_data.assign(data.begin(), data.end());
        return *this;
    }

    void RequestExit() {
        m_exit = true;
        ueventSignal(&m_uevent);
    }

    bool ShouldExit() {
        return m_exit;
    }

    Result ShouldExitResult() {
        return 0;
    }

    UEvent* GetCancelEvent() {
        return &m_uevent;
    }

    void Yield() {
        svcSleepThread(10000000);
    }

private:
    UEvent m_uevent{};
    bool m_exit{};
    std::string m_action{};
    std::string m_title{};
    std::string m_transfer{};
    s64 m_size{};
    s64 m_offset{};
    std::vector<u8> m_image_data{};
};

} // namespace sphaira::ui
