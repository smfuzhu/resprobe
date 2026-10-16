# ResProbe - Windows 显示器分辨率变化探测与远程桌面诊断工具
# 构建: mingw32-make   (或直接运行 编译.bat)

CXX      ?= g++
RC       ?= windres
TARGET   := resprobe.exe
SRCDIR   := src
RESDIR   := res
BUILDDIR := build

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
            -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00

# 注意: 不指定 -mwindows，保持控制台子系统（g++ 默认）。
# .cmd 脚本依赖「stdout 可被重定向捕获」+「退出码即契约」；
# 改成 GUI 子系统必须同时保证这两点不退化，详见 DEVELOPMENT.md 第 3 节。
LDFLAGS  := -static-libgcc -static-libstdc++

LIBS     := -luser32 -lgdi32 -lsetupapi -lwtsapi32 -lpowrprof \
            -ladvapi32 -lshell32 -lole32 -loleaut32

SRCS     := $(wildcard $(SRCDIR)/*.cpp)
OBJS     := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS     := $(OBJS:.o=.d)

# 资源对象: 图标 + manifest(requireAdministrator/DPI) + 版本信息
RCOBJ    := $(BUILDDIR)/res.o
RC_SRCS  := $(RESDIR)/resprobe.rc $(RESDIR)/resprobe.manifest $(RESDIR)/resprobe.ico

.PHONY: all clean run icon test

all: $(TARGET)

$(BUILDDIR):
	-mkdir $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# -I $(RESDIR) 让 .rc 里的裸文件名 (resprobe.ico / resprobe.manifest) 能被找到
#
# 先跑清单校验：损坏的 manifest 不会让构建失败，而是让进程**启动时**报
# "side by side configuration is incorrect" —— 排查成本极高，所以在构建期拦掉。
$(RCOBJ): $(RC_SRCS) | $(BUILDDIR)
	powershell -NoProfile -ExecutionPolicy Bypass -File $(RESDIR)/check-manifest.ps1
	$(RC) -I $(RESDIR) $(RESDIR)/resprobe.rc -o $@

$(TARGET): $(OBJS) $(RCOBJ)
	$(CXX) $(OBJS) $(RCOBJ) -o $@ $(LDFLAGS) $(LIBS)
	@echo.
	@echo Build OK: $(TARGET)

# 重新生成多尺寸图标（仅改动图案时才需要）
icon:
	powershell -NoProfile -ExecutionPolicy Bypass -File $(RESDIR)/make-icon.ps1

run: $(TARGET)
	$(TARGET) snapshot

# ---- 单元测试 ---------------------------------------------------------------
# 只链接"除 main.o 外"的全部目标文件，复用同一批编译产物。
# 刻意**不**链接资源对象(RCOBJ)：RCOBJ 里的 manifest 是 requireAdministrator，
# 带它进来会让测试程序也需要提权才能运行。
TESTBIN  := resprobe-tests.exe
TEST_OBJS := $(filter-out $(BUILDDIR)/main.o,$(OBJS))

$(BUILDDIR)/test_main.o: tests/test_main.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TESTBIN): $(TEST_OBJS) $(BUILDDIR)/test_main.o
	$(CXX) $(TEST_OBJS) $(BUILDDIR)/test_main.o -o $@ $(LDFLAGS) $(LIBS)
	@echo.
	@echo Build OK (tests): $(TESTBIN)

test: $(TESTBIN)
	$(TESTBIN)

clean:
	-del /q $(BUILDDIR)\*.o $(BUILDDIR)\*.d $(TARGET) $(TESTBIN) 2>nul
	-rmdir /q $(BUILDDIR) 2>nul

-include $(DEPS)
