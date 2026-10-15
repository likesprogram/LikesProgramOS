# Makefile
#    CheckCrypto —— SHA-512、Ed25519 与信任锚表的宿主侧检查（C++20）：标准向量、互验、轮换窗口与 32 位链接检查

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 目标侧密码学源码目录：本工具直接编译将要交付的这份源码
TARGET_SRC := ../../../Packages/Baleen/Common/Src
# 目标侧头文件目录
TARGET_INC := ../../../Packages/Baleen/Common/Include
# 目标侧信任锚源码与头文件目录：查找实现是交付代码，锚表由本工具的 main.cpp 自带
CORE_SRC := ../../../Packages/Baleen/Core/Src
CORE_INC := ../../../Packages/Baleen/Core/Include
# 宿主侧共享源码与头文件目录
HOST_SRC := ../../Common/Src
HOST_INC := ../../Common/Include

# C++ 编译器
CXX      ?= g++
# 预处理选项：目标侧与宿主侧的实现与接口
CPPFLAGS ?= -I$(TARGET_INC) -I$(HOST_INC)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckCrypto
# 目标文件：main 是本工具，其余是目标侧与宿主侧的两份实现，以及目标侧的信任锚查找
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Sha512.target.o $(OBJ_DIR)/Sha512.host.o $(OBJ_DIR)/Ed25519.target.o $(OBJ_DIR)/Ed25519.host.o $(OBJ_DIR)/TrustAnchor.target.o

vpath %.cpp $(SRC_DIR) $(HOST_SRC)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 目标侧与宿主侧有同名实现文件：显式规则区分对象名，避免 vpath 撞名
$(OBJ_DIR)/Sha512.target.o: $(TARGET_SRC)/Sha512.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Sha512.host.o: $(HOST_SRC)/Sha512.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Ed25519.target.o: $(TARGET_SRC)/Ed25519.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Ed25519.host.o: $(HOST_SRC)/Ed25519.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 目标侧信任锚查找：与本工具自带的锚表一起验按 KeyId 取公钥；
# 构建期生成的 Out/TrustAnchors.cpp 定义同名符号，这里绝不编它，否则撞符号
$(OBJ_DIR)/TrustAnchor.target.o: $(CORE_SRC)/TrustAnchor.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) -I$(CORE_INC) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# Bin 目录与其它工具共用，clean 只删本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

help:
	@echo "CheckCrypto（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
