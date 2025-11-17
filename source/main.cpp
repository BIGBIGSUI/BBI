#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>

#include "haze.h"
#include "fs.hpp"
#include "yati/yati.hpp"
#include "ui/progress_box.hpp"
#include "log.hpp"
#include "install_stream.hpp"
#include <algorithm>

namespace {

Mutex g_mutex;
std::vector<haze::CallbackData> g_callback_data;

// 进度跟踪
struct ProgressTracker {
    s64 last_offset = 0;
    u64 last_time = 0;
    double speed = 0.0;  // bytes per second
    
    void Update(s64 current_offset, s64 total_size) {
        u64 current_time = armGetSystemTick();
        
        if (last_time != 0) {
            u64 time_diff = armTicksToNs(current_time - last_time);
            if (time_diff >= 1000000000ULL) {  // 更新间隔 >= 1 秒
                s64 bytes_diff = current_offset - last_offset;
                speed = (double)bytes_diff / ((double)time_diff / 1e9);
                last_offset = current_offset;
                last_time = current_time;
            }
        } else {
            last_time = current_time;
            last_offset = current_offset;
        }
    }
    
    void Reset() {
        last_offset = 0;
        last_time = 0;
        speed = 0.0;
    }
} g_progress_tracker;

// 全局安装上下文
struct InstallContext {
    Mutex mutex;
    std::unique_ptr<sphaira::mtp::InstallStream> stream;
    Thread install_thread;
    std::atomic<bool> in_progress{false};
    std::atomic<Result> install_result{0};
} g_install_ctx;

void BbiLog(const char* fmt, ...) {
    static bool init = false;
    if (!init) {
        fs::CreateDirectoryRecursively("/config/BBI");
        init = true;
    }

    std::FILE* f = std::fopen("/config/BBI/log.txt", "a");
    if (!f) {
        return;
    }

    std::va_list v;
    va_start(v, fmt);
    std::vfprintf(f, fmt, v);
    va_end(v);

    std::fclose(f);
}

struct FsNative : haze::FileSystemProxyImpl {
    FsNative() = default;
    FsNative(FsFileSystem* fs, bool own) {
        m_fs = *fs;
        m_own = own;
    }

    ~FsNative() {
        fsFsCommit(&m_fs);
        if (m_own) {
            fsFsClose(&m_fs);
        }
    }

    const char* FixPath(const char* path, char* out = nullptr) const {
        static char buf[FS_MAX_PATH];
        const auto len = std::strlen(GetName());

        if (!out) {
            out = buf;
        }

        if (len && !strncasecmp(path + 1, GetName(), len)) {
            std::snprintf(out, sizeof(buf), "/%s", path + 1 + len);
        } else {
            std::strcpy(out, path);
        }

        return out;
    }

    Result GetTotalSpace(const char *path, s64 *out) override {
        return fsFsGetTotalSpace(&m_fs, FixPath(path), out);
    }
    Result GetFreeSpace(const char *path, s64 *out) override {
        return fsFsGetFreeSpace(&m_fs, FixPath(path), out);
    }
    Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) override {
        return fsFsGetEntryType(&m_fs, FixPath(path), out_entry_type);
    }
    Result CreateFile(const char* path, s64 size, u32 option) override {
        return fsFsCreateFile(&m_fs, FixPath(path), size, option);
    }
    Result DeleteFile(const char* path) override {
        return fsFsDeleteFile(&m_fs, FixPath(path));
    }
    Result RenameFile(const char *old_path, const char *new_path) override {
        char temp[FS_MAX_PATH];
        return fsFsRenameFile(&m_fs, FixPath(old_path, temp), FixPath(new_path));
    }
    Result OpenFile(const char *path, u32 mode, FsFile *out_file) override {
        return fsFsOpenFile(&m_fs, FixPath(path), mode, out_file);
    }
    Result GetFileSize(FsFile *file, s64 *out_size) override {
        return fsFileGetSize(file, out_size);
    }
    Result SetFileSize(FsFile *file, s64 size) override {
        return fsFileSetSize(file, size);
    }
    Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) override {
        return fsFileRead(file, off, buf, read_size, option, out_bytes_read);
    }
    Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) override {
        return fsFileWrite(file, off, buf, write_size, option);
    }
    void CloseFile(FsFile *file) override {
        fsFileClose(file);
    }

    Result CreateDirectory(const char* path) override {
        return fsFsCreateDirectory(&m_fs, FixPath(path));
    }
    Result DeleteDirectoryRecursively(const char* path) override {
        return fsFsDeleteDirectoryRecursively(&m_fs, FixPath(path));
    }
    Result RenameDirectory(const char *old_path, const char *new_path) override {
        char temp[FS_MAX_PATH];
        return fsFsRenameDirectory(&m_fs, FixPath(old_path, temp), FixPath(new_path));
    }
    Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) override {
        return fsFsOpenDirectory(&m_fs, FixPath(path), mode, out_dir);
    }
    Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) override {
        return fsDirRead(d, out_total_entries, max_entries, buf);
    }
    Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) override {
        return fsDirGetEntryCount(d, out_count);
    }
    void CloseDirectory(FsDir *d) override {
        fsDirClose(d);
    }

    bool MultiThreadTransfer(s64, bool) override {
        return true;
    }

    FsFileSystem m_fs{};
    bool m_own{true};
};

struct FsSdmc final : FsNative {
    FsSdmc() : FsNative(fsdevGetDeviceFileSystem("sdmc"), false) {}

    const char* GetName() const override {
        return "";
    }
    const char* GetDisplayName() const override {
        return "microSD";
    }
};

struct FsNandImage final : FsNative {
    FsNandImage() {
        fsOpenImageDirectoryFileSystem(&m_fs, FsImageDirectoryId_Nand);
        m_own = true;
    }

    const char* GetName() const override {
        return "image_nand:/";
    }
    const char* GetDisplayName() const override {
        return "Game Install (NAND)";
    }
};

struct FsSdImage final : FsNative {
    FsSdImage() {
        fsOpenImageDirectoryFileSystem(&m_fs, FsImageDirectoryId_Sd);
        m_own = true;
    }

    const char* GetName() const override {
        return "image_sd:/";
    }
    const char* GetDisplayName() const override {
        return "Game Install (SD)";
    }
};

struct FsInstall final : haze::FileSystemProxyImpl {
    FsInstall() {
        mutexInit(&g_install_ctx.mutex);
        log_write("[FsInstall] Initialized\n");
    }

    ~FsInstall() {
        // 确保安装线程退出
        if (g_install_ctx.in_progress) {
            log_write("[FsInstall] Destructor: Disabling stream and waiting for install thread\n");
            // 先 Disable stream，让安装线程停止等待
            if (g_install_ctx.stream) {
                g_install_ctx.stream->Disable();
            }
            // 然后等待线程退出
            WaitForInstallThread();
        }
        log_write("[FsInstall] Destroyed\n");
    }

    const char* GetName() const override {
        return "install";
    }

    const char* GetDisplayName() const override {
        return "Install (NSP, XCI, NSZ, XCZ)";
    }

    // 安装线程入口
    static void InstallThreadFunc(void* arg) {
        auto* stream = static_cast<sphaira::mtp::InstallStream*>(arg);
        log_write("[InstallThread] Started for: %s\n", stream->GetPath().s);

        fs::FsNativeSd fs_sd{};
        if (R_FAILED(fs_sd.GetFsOpenResult())) {
            log_write("[InstallThread] Failed to open FsNativeSd\n");
            g_install_ctx.install_result = fs_sd.GetFsOpenResult();
            // 注意：不在这里设置 in_progress = false
            stream->Disable();  // 通知 MTP 写入线程停止
            return;
        }

        sphaira::ui::ProgressBox pbox;
        sphaira::yati::ConfigOverride override{};

        // 调用 yati::InstallFromSource，使用流式数据源
        Result rc = sphaira::yati::InstallFromSource(&pbox, stream, stream->GetPath(), override);
        g_install_ctx.install_result = rc;
        // 注意：不在这里设置 in_progress = false，让 WaitForInstallThread() 来处理

        if (R_SUCCEEDED(rc)) {
            log_write("[InstallThread] SUCCESS: %s\n", stream->GetPath().s);
            BbiLog("[Install] OK: %s\n", stream->GetPath().s);
        } else {
            log_write("[InstallThread] FAILED (0x%x): %s\n", rc, stream->GetPath().s);
            BbiLog("[Install] FAILED (0x%x): %s\n", rc, stream->GetPath().s);
        }
        
        // 无论成功失败都要 Disable，让 MTP 线程停止传输
        stream->Disable();
    }

    static void WaitForInstallThread() {
        if (g_install_ctx.in_progress) {
            log_write("[FsInstall] Waiting for install thread...\n");
            threadWaitForExit(&g_install_ctx.install_thread);
            threadClose(&g_install_ctx.install_thread);
            g_install_ctx.in_progress = false;  // 确保标志被重置
            log_write("[FsInstall] Install thread exited\n");
        }
    }

    static bool IsSupportedExt(const char* name) {
        const char* ext = std::strrchr(name, '.');
        if (!ext) {
            return false;
        }

        // 支持 NSP/NSZ/XCI/XCZ
        return !strcasecmp(ext, ".nsp") ||
               !strcasecmp(ext, ".nsz") ||
               !strcasecmp(ext, ".xci") ||
               !strcasecmp(ext, ".xcz");
    }

    static const char* GetFileName(const char* s) {
        const char* p = std::strrchr(s, '/');
        return p ? p + 1 : s;
    }

    // 生成唯一文件名（处理重名情况）
    static std::string GenerateUniqueName(const std::vector<FsDirectoryEntry>& entries, const char* base_name) {
        // 检查是否已存在
        auto exists = [&](const char* name) {
            return std::find_if(entries.begin(), entries.end(), [name](auto& e) {
                return !strcasecmp(name, e.name);
            }) != entries.end();
        };

        if (!exists(base_name)) {
            return base_name;
        }

        // 提取文件名和扩展名
        std::string name_str(base_name);
        const char* ext = std::strrchr(base_name, '.');
        std::string base = ext ? name_str.substr(0, ext - base_name) : name_str;
        std::string extension = ext ? ext : "";

        // 尝试添加编号 (1), (2), (3)...
        for (int i = 1; i < 1000; i++) {
            char unique_name[300];
            std::snprintf(unique_name, sizeof(unique_name), "%s(%d)%s", 
                         base.c_str(), i, extension.c_str());
            if (!exists(unique_name)) {
                return unique_name;
            }
        }

        // 极端情况：使用时间戳
        char unique_name[300];
        std::snprintf(unique_name, sizeof(unique_name), "%s_%llu%s", 
                     base.c_str(), (unsigned long long)armGetSystemTick(), extension.c_str());
        return unique_name;
    }

    Result GetTotalSpace(const char *path, s64 *out) override {
        // 返回 SD 卡的总空间
        fs::FsNativeSd fs_sd{};
        if (R_FAILED(fs_sd.GetFsOpenResult())) {
            *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;  // 假定 256GB
            R_SUCCEED();
        }
        return fs_sd.GetTotalSpace("/", out);
    }

    Result GetFreeSpace(const char *path, s64 *out) override {
        // 返回 SD 卡的剩余空间
        fs::FsNativeSd fs_sd{};
        if (R_FAILED(fs_sd.GetFsOpenResult())) {
            *out = 1024ULL * 1024ULL * 1024ULL * 256ULL;  // 假定 256GB
            R_SUCCEED();
        }
        return fs_sd.GetFreeSpace("/", out);
    }

    Result GetEntryType(const char *path, FsDirEntryType *out_entry_type) override {
        if (std::strcmp(path, "/") == 0 || std::strcmp(path, "") == 0) {
            *out_entry_type = FsDirEntryType_Dir;
            R_SUCCEED();
        }

        // 检查是否是虚拟目录（所有非根目录的路径都视为目录）
        // 这样 Windows 可以遍历文件夹结构
        const char* name = GetFileName(path);
        if (!name || name == path) {
            // 没有 '/'，可能是根目录下的项目
        } else if (name > path && *(name - 1) == '/') {
            // 有路径分隔符，先检查是否是目录
            // 所有中间路径都视为目录
            *out_entry_type = FsDirEntryType_Dir;
            R_SUCCEED();
        }

        // 在虚拟文件条目中查找
        SCOPED_MUTEX(&g_install_ctx.mutex);
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [name](auto& e) {
            return !strcasecmp(name, e.name);
        });
        R_UNLESS(it != m_entries.end(), FsError_PathNotFound);

        *out_entry_type = FsDirEntryType_File;
        R_SUCCEED();
    }

    Result CreateFile(const char* path, s64 size, u32 option) override {
        const char* name = GetFileName(path);
        R_UNLESS(name, FsError_PathNotFound);
        R_UNLESS(IsSupportedExt(name), FsError_NotImplemented);

        SCOPED_MUTEX(&g_install_ctx.mutex);

        // 生成唯一文件名（处理同名文件）
        std::string unique_name = GenerateUniqueName(m_entries, name);

        // 创建虚拟条目
        FsDirectoryEntry entry{};
        std::strncpy(entry.name, unique_name.c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.type = FsDirEntryType_File;
        entry.file_size = size;
        m_entries.emplace_back(entry);

        if (unique_name != name) {
            log_write("[FsInstall] CreateFile: %s (renamed from %s)\n", unique_name.c_str(), name);
        } else {
            log_write("[FsInstall] CreateFile: %s\n", name);
        }
        R_SUCCEED();
    }

    Result DeleteFile(const char* path) override {
        R_SUCCEED();
    }

    Result RenameFile(const char *old_path, const char *new_path) override {
        R_THROW(FsError_NotImplemented);
    }

    Result OpenFile(const char *path, u32 mode, FsFile *out_file) override {
        R_UNLESS(mode & FsOpenMode_Write, FsError_NotImplemented);

        const char* name = GetFileName(path);
        R_UNLESS(name, FsError_PathNotFound);
        R_UNLESS(IsSupportedExt(name), FsError_NotImplemented);

        SCOPED_MUTEX(&g_install_ctx.mutex);

        // 找到虚拟条目
        auto it = std::find_if(m_entries.begin(), m_entries.end(), [name](auto& e) {
            return !strcasecmp(name, e.name);
        });
        R_UNLESS(it != m_entries.end(), FsError_PathNotFound);

        const auto object_id = std::distance(m_entries.begin(), it);
        out_file->s.object_id = object_id;
        out_file->s.own_handle = mode;

        log_write("[FsInstall] OpenFile: %s mode=0x%X object_id=%d\n", name, mode, (int)object_id);

        // 如果是写模式，创建 stream + 启动安装线程
        if (mode & FsOpenMode_Write) {
            // 等待之前的安装完成
            if (g_install_ctx.in_progress) {
                log_write("[FsInstall] Waiting for previous install...\n");
                mutexUnlock(&g_install_ctx.mutex);
                WaitForInstallThread();
                mutexLock(&g_install_ctx.mutex);
            }

            // 创建新的 stream
            fs::FsPath install_path = "/install/";
            install_path += name;
            g_install_ctx.stream = std::make_unique<sphaira::mtp::InstallStream>(install_path);
            g_install_ctx.in_progress = true;

            // 启动安装线程（使用与 yati 内部线程相同的参数）
            Result rc = threadCreate(&g_install_ctx.install_thread, InstallThreadFunc,
                                      g_install_ctx.stream.get(), nullptr, 1024*128, 0x3B, -2);
            if (R_FAILED(rc)) {
                log_write("[FsInstall] Failed to create install thread: 0x%x\n", rc);
                g_install_ctx.stream.reset();
                g_install_ctx.in_progress = false;
                return rc;
            }

            rc = threadStart(&g_install_ctx.install_thread);
            if (R_FAILED(rc)) {
                log_write("[FsInstall] Failed to start install thread: 0x%x\n", rc);
                threadClose(&g_install_ctx.install_thread);
                g_install_ctx.stream.reset();
                g_install_ctx.in_progress = false;
                return rc;
            }

            log_write("[FsInstall] Install thread started for: %s\n", name);
        }

        R_SUCCEED();
    }

    Result GetFileSize(FsFile *file, s64 *out_size) override {
        SCOPED_MUTEX(&g_install_ctx.mutex);
        auto& e = m_entries[file->s.object_id];
        *out_size = e.file_size;
        R_SUCCEED();
    }

    Result SetFileSize(FsFile *file, s64 size) override {
        SCOPED_MUTEX(&g_install_ctx.mutex);
        auto& e = m_entries[file->s.object_id];
        e.file_size = size;
        R_SUCCEED();
    }

    Result ReadFile(FsFile *file, s64 off, void *buf, u64 read_size, u32 option, u64 *out_bytes_read) override {
        *out_bytes_read = 0;
        R_THROW(FsError_NotImplemented);
    }

    Result WriteFile(FsFile *file, s64 off, const void *buf, u64 write_size, u32 option) override {
        // 推送数据到 stream
        if (!g_install_ctx.stream) {
            log_write("[FsInstall] WriteFile: no stream\n");
            R_THROW(FsError_PathNotFound);
        }

        if (!g_install_ctx.stream->Push(buf, write_size)) {
            log_write("[FsInstall] WriteFile: Push failed\n");
            R_THROW(FsError_NotImplemented);
        }

        // 更新虚拟文件大小
        {
            SCOPED_MUTEX(&g_install_ctx.mutex);
            auto& e = m_entries[file->s.object_id];
            e.file_size = std::max<s64>(e.file_size, off + write_size);
        }

        R_SUCCEED();
    }

    void CloseFile(FsFile *file) override {
        log_write("[FsInstall] CloseFile object_id=%d\n", (int)file->s.object_id);

        // 如果是写模式，通知 stream 数据结束
        if (file->s.own_handle & FsOpenMode_Write) {
            if (g_install_ctx.stream) {
                s64 final_size = 0;
                {
                    SCOPED_MUTEX(&g_install_ctx.mutex);
                    auto& e = m_entries[file->s.object_id];
                    final_size = e.file_size;
                }

                log_write("[FsInstall] CloseFile: disabling stream, size=%lld\n", (long long)final_size);
                BbiLog("[Install] CloseFile size=%lld bytes\n", (long long)final_size);

                g_install_ctx.stream->Disable();

                // 等待安装线程完成
                WaitForInstallThread();

                // 输出安装结果
                const Result rc = g_install_ctx.install_result;
                if (R_SUCCEEDED(rc)) {
                    std::printf("[Install] SUCCESS\n");
                } else {
                    std::printf("[Install] FAILED (0x%x)\n", rc);
                }

                // 清理
                g_install_ctx.stream.reset();
            }
        }

        std::memset(file, 0, sizeof(*file));
    }

    Result CreateDirectory(const char* path) override {
        // 虚假成功，不实际创建目录
        // 这样 Windows 会继续传输文件夹内的文件
        log_write("[FsInstall] CreateDirectory (ignored): %s\n", path);
        R_SUCCEED();
    }

    Result DeleteDirectoryRecursively(const char* path) override {
        R_THROW(FsError_NotImplemented);
    }

    Result RenameDirectory(const char *old_path, const char *new_path) override {
        R_THROW(FsError_NotImplemented);
    }

    Result OpenDirectory(const char *path, u32 mode, FsDir *out_dir) override {
        std::memset(out_dir, 0, sizeof(*out_dir));
        R_SUCCEED();
    }

    Result ReadDirectory(FsDir *d, s64 *out_total_entries, size_t max_entries, FsDirectoryEntry *buf) override {
        SCOPED_MUTEX(&g_install_ctx.mutex);
        max_entries = std::min<s64>(m_entries.size() - d->s.object_id, max_entries);
        std::memcpy(buf, m_entries.data() + d->s.object_id, max_entries * sizeof(*buf));
        d->s.object_id += max_entries;
        *out_total_entries = max_entries;
        R_SUCCEED();
    }

    Result GetDirectoryEntryCount(FsDir *d, s64 *out_count) override {
        SCOPED_MUTEX(&g_install_ctx.mutex);
        *out_count = m_entries.size();
        R_SUCCEED();
    }

    void CloseDirectory(FsDir *d) override {
        std::memset(d, 0, sizeof(*d));
    }

    bool MultiThreadTransfer(s64 size, bool read) override {
        return false;
    }

private:
    std::vector<FsDirectoryEntry> m_entries;
};

void callbackHandler(const haze::CallbackData* data) {
    mutexLock(&g_mutex);
    g_callback_data.emplace_back(*data);
    mutexUnlock(&g_mutex);
}

void processEvents() {
    std::vector<haze::CallbackData> data;

    mutexLock(&g_mutex);
    std::swap(data, g_callback_data);
    mutexUnlock(&g_mutex);

    for (const auto& e : data) {
        switch (e.type) {
            case haze::CallbackType_OpenSession: std::printf("Opening Session\n"); break;
            case haze::CallbackType_CloseSession: std::printf("Closing Session\n"); break;

            case haze::CallbackType_CreateFile: std::printf("Creating File: %s\n", e.file.filename); break;
            case haze::CallbackType_DeleteFile: std::printf("Deleting File: %s\n", e.file.filename); break;

            case haze::CallbackType_RenameFile: std::printf("Rename File: %s -> %s\n", e.rename.filename, e.rename.newname); break;
            case haze::CallbackType_RenameFolder: std::printf("Rename Folder: %s -> %s\n", e.rename.filename, e.rename.newname); break;

            case haze::CallbackType_CreateFolder: std::printf("Creating Folder: %s\n", e.file.filename); break;
            case haze::CallbackType_DeleteFolder: std::printf("Deleting Folder: %s\n", e.file.filename); break;

            case haze::CallbackType_ReadBegin: std::printf("Reading File Begin: %s \r", e.file.filename); break;
            case haze::CallbackType_ReadProgress: std::printf("Reading File: offset: %lld size: %lld\r", e.progress.offset, e.progress.size); break;
            case haze::CallbackType_ReadEnd: std::printf("Reading File Finished: %s\n", e.file.filename); break;

            case haze::CallbackType_WriteBegin: 
                g_progress_tracker.Reset();
                std::printf("Writing File Begin: %s\n", e.file.filename); 
                break;
                
            case haze::CallbackType_WriteProgress: {
                g_progress_tracker.Update(e.progress.offset, e.progress.size);
                
                // 格式化速度
                const double speed_mb = g_progress_tracker.speed / (1024.0 * 1024.0);
                if (speed_mb >= 0.01) {
                    std::printf("\rTransferring... %.2f MiB/s     ", speed_mb);
                } else {
                    const double speed_kb = g_progress_tracker.speed / 1024.0;
                    std::printf("\rTransferring... %.2f KiB/s     ", speed_kb);
                }
                break;
            }
            
            case haze::CallbackType_WriteEnd: 
                std::printf("\nWriting File Finished: %s\n", e.file.filename); 
                g_progress_tracker.Reset();
                break;
        }
    }

    consoleUpdate(nullptr);
}

} // namespace

int main(int argc, char** argv) {
    fsdevMountSdmc();

    mutexInit(&g_mutex);

    consoleInit(nullptr);

    log_file_init();

    // test yati logging backend
    log_write("[YATI] log_init from usbhs main()\n");

    BbiLog("==== usbhs start ===\n");

    haze::FsEntries fs_entries;
    fs_entries.emplace_back(std::make_shared<FsSdmc>());
    fs_entries.emplace_back(std::make_shared<FsInstall>());

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    bool mtpRunning = false;

    std::printf("BBI A fake DBI made by hahappify\n\n");
    std::printf("Press X to start MTP (SD + game install)\n");
    std::printf("Press B to stop MTP and exit\n");
    std::printf("Press + to exit without MTP\n\n");
    consoleUpdate(nullptr);

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 kDown = padGetButtonsDown(&pad);

        if (kDown & HidNpadButton_Plus) {
            if (mtpRunning) {
                haze::Exit();
                mtpRunning = false;
            }
            break;
        }

        if (kDown & HidNpadButton_X) {
            if (!mtpRunning) {
                const bool ok = haze::Initialize(callbackHandler, 0x2C, 2, fs_entries);
                if (ok) {
                    mtpRunning = true;
                    std::printf("MTP started. Connect to PC.\n");
                } else {
                    std::printf("Failed to start MTP (already running?)\n");
                }
                consoleUpdate(nullptr);
            }
        }

        if (kDown & HidNpadButton_B) {
            if (mtpRunning) {
                haze::Exit();
                mtpRunning = false;
            }
            break;
        }

        if (mtpRunning) {
            processEvents();
        } else {
            consoleUpdate(nullptr);
        }

        svcSleepThread(1e9 / 60);
    }

    if (mtpRunning) {
        haze::Exit();
    }

    consoleExit(nullptr);
    fsdevUnmountAll();
    return 0;
}

extern "C" {

void userAppInit(void) {
    Result rc;
    
    if (R_FAILED(rc = appletLockExit())) {
        diagAbortWithResult(rc);
    }
    
    // 初始化 NCM 服务（yati 安装引擎需要）
    if (R_FAILED(rc = ncmInitialize())) {
        diagAbortWithResult(rc);
    }
}

void userAppExit(void) {
    ncmExit();
    appletUnlockExit();
}

}
