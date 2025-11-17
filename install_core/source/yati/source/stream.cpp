#include "yati/source/stream.hpp"
#include "defines.hpp"
#include "log.hpp"

namespace sphaira::yati::source {

Result Stream::Read(void* _buf, s64 off, s64 size, u64* bytes_read_out) {
    // streams don't allow for random access (seeking backwards).
    R_UNLESS(off >= m_offset, Result_StreamBadSeek);

    auto buf = static_cast<u8*>(_buf);
    *bytes_read_out = 0;

    // check if we already have some data in the buffer.
    while (size) {
        // while it is invalid to seek backwards, it is valid to seek forwards.
        // this can be done to skip padding, skip undeeded files etc.
        // to handle this, simply read the data into a buffer and discard it.
        if (off > m_offset) {
            const auto skip_size = off - m_offset;
            
            // 使用分块跳过，避免分配巨大内存（XCI secure 分区可能在几 GB 偏移处）
            constexpr s64 SKIP_CHUNK_SIZE = 1024 * 1024 * 8;  // 8MB 每次
            std::vector<u8> temp_buf(std::min<s64>(skip_size, SKIP_CHUNK_SIZE));
            
            s64 remaining = skip_size;
            while (remaining > 0) {
                const auto chunk = std::min<s64>(remaining, SKIP_CHUNK_SIZE);
                u64 bytes_read;
                R_TRY(ReadChunk(temp_buf.data(), chunk, &bytes_read));
                m_offset += bytes_read;
                remaining -= bytes_read;
                
                if (bytes_read < (u64)chunk) {
                    // 数据不足，可能是流结束
                    break;
                }
            }
        } else {
            u64 bytes_read;
            R_TRY(ReadChunk(buf, size, &bytes_read));

            *bytes_read_out += bytes_read;
            buf += bytes_read;
            off += bytes_read;
            m_offset += bytes_read;
            size -= bytes_read;
        }
    }

    R_SUCCEED();
}

} // namespace sphaira::yati::source
