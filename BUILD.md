# 编译指南

## 环境准备

### 1. 安装DevkitPro

#### Windows
1. 下载DevkitPro安装器：https://github.com/devkitPro/installer/releases
2. 运行安装器，选择安装Nintendo Switch开发工具
3. 安装完成后会自动配置环境变量

#### Linux
```bash
# Arch Linux
sudo pacman -S devkitpro-pacman
sudo pacman -S switch-dev

# Ubuntu/Debian
wget https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman.amd64.deb
sudo dpkg -i devkitpro-pacman.amd64.deb
sudo dkp-pacman -S switch-dev
```

#### macOS
```bash
# 使用Homebrew
brew install devkitpro/devkitpro/devkitpro-pacman
sudo dkp-pacman -S switch-dev
```

### 2. 安装依赖库

在DevkitPro的MSYS2环境中执行：

```bash
# 更新包管理器
pacman -Syu

# 安装基础库
pacman -S switch-libnx switch-tools

# 安装libhaze（MTP库）
pacman -S switch-libhaze
```

如果libhaze不在官方仓库，手动编译：

```bash
# 克隆libhaze仓库
git clone https://github.com/ITotalJustice/libhaze.git
cd libhaze

# 编译并安装
make
make install
```

### 3. 验证环境

```bash
# 检查DEVKITPRO环境变量
echo $DEVKITPRO

# 应该输出类似：/opt/devkitpro 或 C:/devkitPro

# 检查编译器
aarch64-none-elf-gcc --version
```

## 编译步骤

### 方法1: 使用Make（推荐）

```bash
# 进入项目目录
cd /home/24289/mtp/usbhs

# 清理之前的编译文件
make clean

# 编译项目
make

# 并行编译（更快）
make -j$(nproc)
```

### 方法2: 使用编译脚本

```bash
# 赋予执行权限
chmod +x build.sh

# 运行编译脚本
./build.sh
```

## 编译输出

编译成功后会生成以下文件：

```
mtp.nro       # Nintendo Switch homebrew可执行文件
mtp.nacp      # 应用元数据
mtp.elf       # 调试用ELF文件
build/        # 编译中间文件目录
```

## 常见编译错误

### 错误1: DEVKITPRO未设置

```
错误信息: Please set DEVKITPRO in your environment
```

**解决方案**:
```bash
# Windows (MSYS2)
export DEVKITPRO=/c/devkitPro

# Linux
export DEVKITPRO=/opt/devkitpro

# 永久设置（添加到 ~/.bashrc）
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
source ~/.bashrc
```

### 错误2: haze.h找不到

```
错误信息: fatal error: haze.h: No such file or directory
```

**解决方案**:
```bash
# 安装libhaze
pacman -S switch-libhaze

# 或手动编译安装
git clone https://github.com/ITotalJustice/libhaze.git
cd libhaze
make install
```

### 错误3: 找不到编译器

```
错误信息: aarch64-none-elf-gcc: command not found
```

**解决方案**:
```bash
# 确保安装了switch-dev
pacman -S switch-dev

# 检查PATH环境变量
echo $PATH | grep devkitPro
```

### 错误4: 链接错误

```
错误信息: undefined reference to `haze::Initialize'
```

**解决方案**:
- 检查Makefile中的LIBS是否包含 `-lhaze`
- 确保libhaze已正确安装
- 尝试重新安装libhaze

### 错误5: C++标准版本问题

```
错误信息: error: 'shared_ptr' is not a member of 'std'
```

**解决方案**:
- 确保使用的是libnx最新版本
- 检查CXXFLAGS中是否指定了合适的C++标准

## 编译优化

### 调试版本编译

```bash
# 修改Makefile中的CFLAGS
CFLAGS := -g -Wall -O0  # O0表示不优化，便于调试
```

### 发布版本编译

```bash
# 修改Makefile中的CFLAGS
CFLAGS := -O2 -ffunction-sections  # O2优化级别
```

### 减小文件大小

```bash
# 在LDFLAGS中添加
LDFLAGS += -Wl,--gc-sections

# 编译后使用strip
aarch64-none-elf-strip mtp.elf
```

## 交叉编译注意事项

本项目编译生成的是ARM64架构的可执行文件，只能在Nintendo Switch上运行，不能在PC上直接执行。

### 架构信息
- **目标平台**: Nintendo Switch (ARM Cortex-A57)
- **架构**: ARMv8-A (64-bit)
- **编译器**: aarch64-none-elf-gcc
- **标准库**: libnx (Nintendo Switch homebrew库)

## 自定义编译选项

### 修改应用名称

编辑 `Makefile` 的第40行：
```makefile
TARGET := mtp  # 修改为你想要的名称
```

### 添加额外的库

编辑 `Makefile` 的第62行：
```makefile
LIBS := -lhaze -lnx -l你的库名
```

### 添加图标

1. 准备256x256的JPG图标文件
2. 命名为 `icon.jpg`
3. 放在项目根目录
4. 重新编译

### 修改应用版本

在 `Makefile` 中添加：
```makefile
APP_VERSION := 1.0.0
```

## 清理编译文件

```bash
# 清理所有编译生成的文件
make clean

# 只清理目标文件
rm -rf build/

# 深度清理
make clean
rm -f *.nro *.nacp *.elf *.map
```

## 编译性能优化

### 使用并行编译
```bash
# 使用所有CPU核心
make -j$(nproc)

# 指定核心数（如4核）
make -j4
```

### 使用ccache加速
```bash
# 安装ccache
pacman -S ccache

# 配置环境变量
export CC="ccache aarch64-none-elf-gcc"
export CXX="ccache aarch64-none-elf-g++"
```

## 自动化编译

### 创建别名
在 `~/.bashrc` 中添加：
```bash
alias build-mtp='cd /home/24289/mtp/usbhs && make -j$(nproc)'
```

### 使用watch自动编译
```bash
# 监控文件变化自动编译
while inotifywait -e modify source/*.cpp; do
    make -j$(nproc)
done
```

## 编译后操作

### 安装到SD卡
```bash
# 假设SD卡挂载在 /mnt/sd
mkdir -p /mnt/sd/switch/mtp
cp mtp.nro /mnt/sd/switch/mtp/
sync
```

### 通过网络传输
```bash
# 使用nxlink
nxlink -s mtp.nro
```

## 故障排除

如果遇到编译问题：

1. **检查环境**:
   ```bash
   echo $DEVKITPRO
   which aarch64-none-elf-gcc
   ```

2. **更新工具链**:
   ```bash
   pacman -Syu
   pacman -S switch-dev --overwrite='*'
   ```

3. **完全重新编译**:
   ```bash
   make clean
   rm -rf build/
   make -j$(nproc)
   ```

4. **查看详细输出**:
   ```bash
   make V=1
   ```

## 获取帮助

如果以上方法都无法解决问题：

1. 查看DevkitPro官方文档
2. 访问GBAtemp论坛Switch Homebrew版块
3. 查看GitHub Issues
4. 加入Switch homebrew Discord社区

---

**提示**: 首次编译可能需要较长时间，因为需要编译所有依赖。后续编译会快很多。
