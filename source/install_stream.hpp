#pragma once

#include "yati/source/stream.hpp"
#include "fs.hpp"
#include <vector>
#include <atomic>
#include <switch.h>

namespace sphaira::mtp {

// InstallStream: 流式数据源，用于 MTP Install
// 继承 yati::source::Stream，实现 ReadChunk 接口
// MTP WriteFile 通过 Push() 推送数据，yati 通过 ReadChunk() 读取数据
class InstallStream final : public sphaira::yati::source::Stream {
public:
    InstallStream(const fs::FsPath& path);
    ~InstallStream();

    // yati::source::Stream 接口：读取一块数据（不带 offset）
    Result ReadChunk(void* buf, s64 size, u64* bytes_read) override;

    // MTP WriteFile 调用：推送数据到缓冲区
    bool Push(const void* buf, s64 size);

    // MTP CloseFile 调用：标记数据流结束
    void Disable();

    // 获取文件路径
    auto& GetPath() const { return m_path; }

    // 公开 mutex 和 active 标志（供外部等待/检查）
    Mutex m_mutex{};
    std::atomic_bool m_active{true};

private:
    fs::FsPath m_path{};
    std::vector<u8> m_buffer{};
    CondVar m_can_read{};   // 通知 yati: 缓冲区有数据可读
    CondVar m_can_write{};  // 通知 MTP: 缓冲区有空间可写

    static constexpr u64 MAX_BUFFER_SIZE = 1024ULL * 1024ULL * 8ULL;  // 8MB
};

} // namespace sphaira::mtp
