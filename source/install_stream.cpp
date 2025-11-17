#include "install_stream.hpp"
#include "log.hpp"
#include "defines.hpp"
#include <cstring>

namespace sphaira::mtp {

InstallStream::InstallStream(const fs::FsPath& path) {
    m_path = path;
    m_active = true;
    m_buffer.reserve(1024ULL * 1024ULL * 32ULL);  // 预留 32MB

    mutexInit(&m_mutex);
    condvarInit(&m_can_read);
    condvarInit(&m_can_write);

    log_write("[InstallStream] Created for: %s\n", path.s);
}

InstallStream::~InstallStream() {
    log_write("[InstallStream] Destroyed: %s\n", m_path.s);
}

Result InstallStream::ReadChunk(void* buf, s64 size, u64* bytes_read) {
    log_write("[InstallStream::ReadChunk] Request size=%lld\n", (long long)size);

    u32 wait_count = 0;
    constexpr u32 MAX_WAIT_COUNT = 30;  // 最多等待 30 秒

    while (true) {
        {
            SCOPED_MUTEX(&m_mutex);

            // 如果缓冲区为空且流还活跃，等待数据（带超时）
            if (m_active && m_buffer.empty()) {
                log_write("[InstallStream::ReadChunk] Buffer empty, waiting...\n");
                
                // 等待最多 1 秒
                const u64 timeout = armNsToTicks(1e9);  // 1 秒
                Result wait_rc = condvarWaitTimeout(&m_can_read, &m_mutex, timeout);
                
                if (R_FAILED(wait_rc)) {
                    // 超时
                    wait_count++;
                    if (wait_count >= MAX_WAIT_COUNT) {
                        // 超过 30 秒没有数据，认为传输已被取消
                        log_write("[InstallStream::ReadChunk] Timeout after %u seconds, aborting\n", MAX_WAIT_COUNT);
                        m_active = false;  // 标记为非活跃
                        *bytes_read = 0;
                        R_THROW(0xBB10);  // 自定义错误码：传输超时
                    }
                    continue;  // 继续等待
                }
                
                // 成功收到信号，重置计数
                wait_count = 0;
            }

            // 如果流已关闭且缓冲区为空，返回 EOF
            if (!m_active && m_buffer.empty()) {
                *bytes_read = 0;
                log_write("[InstallStream::ReadChunk] EOF reached\n");
                R_SUCCEED();
            }

            // 从缓冲区读取数据
            if (!m_buffer.empty()) {
                const s64 read_size = std::min<s64>(size, m_buffer.size());
                std::memcpy(buf, m_buffer.data(), read_size);
                m_buffer.erase(m_buffer.begin(), m_buffer.begin() + read_size);
                *bytes_read = read_size;

                log_write("[InstallStream::ReadChunk] Read %lld bytes, buffer left=%zu\n",
                    (long long)read_size, m_buffer.size());

                // 通知写端可以继续写
                condvarWakeOne(&m_can_write);
                R_SUCCEED();
            }
        }

        // 防止忙等
        svcSleepThread(1e6);  // 1ms
    }
}

bool InstallStream::Push(const void* buf, s64 size) {
    log_write("[InstallStream::Push] size=%lld\n", (long long)size);

    while (true) {
        {
            SCOPED_MUTEX(&m_mutex);

            // 先检查流是否还活跃
            if (!m_active) {
                // Stream 已关闭（安装完成），但仍返回 true 让 MTP 认为传输成功
                // 这样 Windows 才会继续传输下一个文件
                log_write("[InstallStream::Push] Stream not active, discarding data\n");
                return true;  // 返回 true，不中断 MTP 传输
            }

            // 如果缓冲区满，等待空间（带超时检查）
            if (m_buffer.size() >= MAX_BUFFER_SIZE) {
                // log_write("[InstallStream::Push] Buffer full, waiting...\n");  // 减少日志噪音
                
                // 等待最多 1 秒
                const u64 timeout = armNsToTicks(1e9);  // 1 秒
                Result wait_rc = condvarWaitTimeout(&m_can_write, &m_mutex, timeout);
                
                // 超时或等待失败后再次检查 active 状态
                if (!m_active) {
                    log_write("[InstallStream::Push] Stream became inactive during wait\n");
                    return true;  // 返回 true，让 Windows 继续下一个文件
                }
                
                // 如果等待超时，继续循环（可能安装线程还在工作）
                if (R_FAILED(wait_rc)) {
                    // log_write("[InstallStream::Push] Wait timeout, retrying...\n");  // 减少日志噪音
                    continue;
                }
            }

            // 再次确认流还活跃
            if (!m_active) {
                log_write("[InstallStream::Push] Stream not active after wait\n");
                return true;  // 返回 true，让 Windows 继续下一个文件
            }

            // 写入缓冲区
            const auto offset = m_buffer.size();
            m_buffer.resize(offset + size);
            std::memcpy(m_buffer.data() + offset, buf, size);

            log_write("[InstallStream::Push] Pushed %lld bytes, buffer=%zu\n",
                (long long)size, m_buffer.size());

            // 通知读端有数据了
            condvarWakeOne(&m_can_read);
            return true;
        }
    }
}

void InstallStream::Disable() {
    log_write("[InstallStream::Disable] Disabling stream: %s\n", m_path.s);

    SCOPED_MUTEX(&m_mutex);
    m_active = false;

    // 唤醒所有等待的线程
    condvarWakeOne(&m_can_read);
    condvarWakeOne(&m_can_write);
}

} // namespace sphaira::mtp
