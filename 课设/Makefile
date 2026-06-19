# Makefile — 操作系统模拟器构建系统

CC       = gcc
CFLAGS   = -Wall -Wextra -std=c99 -g -O2
LDFLAGS  = -lpthread

# 目录
SRCDIR   = src
INCDIR   = include
BUILDDIR = build
DATADIR  = data
TARGET   = $(BUILDDIR)/os_sim

# 源文件 (后续逐步添加)
SRCS     = $(SRCDIR)/main.c \
           $(SRCDIR)/user.c \
           $(SRCDIR)/process.c \
           $(SRCDIR)/scheduler.c \
           $(SRCDIR)/memory.c \
           $(SRCDIR)/persistence.c \
           $(SRCDIR)/concurrency.c \
           $(SRCDIR)/visualization.c

# 目标文件
OBJS     = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

# 头文件依赖
DEPS     = $(INCDIR)/os_types.h

.PHONY: all clean run init

all: init $(TARGET)

# 确保 build/ 和 data/ 目录存在
init:
	@mkdir -p $(BUILDDIR) $(DATADIR)

# 链接
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "[OK] 编译完成: $(TARGET)"

# 编译规则: 只编译已存在的 .c 文件
$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(DEPS)
	@if [ -f "$<" ]; then \
		$(CC) $(CFLAGS) -I. -c $< -o $@; \
		echo "  CC  $<"; \
	fi

# 运行
run: all
	./$(TARGET)

# 清理
clean:
	rm -rf $(BUILDDIR)
	@echo "[OK] 清理完成"

# 完全清理 (含持久化数据)
distclean: clean
	rm -f $(DATADIR)/os_state.bin $(DATADIR)/os_state.lock $(DATADIR)/users.db
	@echo "[OK] 完全清理完成"
