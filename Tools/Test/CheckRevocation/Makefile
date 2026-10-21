# Makefile
#    CheckRevocation —— 签名吊销记录与引导状态区单调纪元的宿主侧检查（C++20）

# 中间对象目录
OBJ_DIR  ?= Obj
# 可执行文件输出目录
BIN_DIR  ?= ../../Bin
# 本工具的源目录
SRC_DIR  := Src
# 共用实现：吊销记录与引导状态记录
COMMON_DIR := ../../../Packages/Common
# 目标侧密码学：清单验签与记录验签用的是将要交付的那份实现
BALEEN_COMMON_DIR := ../../../Packages/Baleen/Common
# 目标侧清单验证与信任锚查找
CORE_DIR := ../../../Packages/Baleen/Core
# 宿主侧共享源码与头文件目录
HOST_SRC := ../../Common/Src
HOST_INC := ../../Common/Include

# C++ 编译器
CXX      ?= g++
# 预处理选项：目标侧头文件（含构建期生成的内嵌最低安全版本）与宿主侧共享代码
CPPFLAGS ?= -I$(CORE_DIR)/Include -I$(CORE_DIR)/Out -I$(COMMON_DIR)/Include -I$(BALEEN_COMMON_DIR)/Include -I$(HOST_INC)
# 编译选项：常见警告全开并视为错误
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
# 链接选项：链接器警告视为错误
LDFLAGS  ?= -Wl,--fatal-warnings

# 本工具的可执行文件
BIN  := $(BIN_DIR)/CheckRevocation
# 目标文件：main 是用例与夹具；Revocation/BootState/KernelImage 取自 Packages/Common，
# ManifestVerify/TrustAnchor 取自 BaleenCore（检查的就是将要交付的那些实现）；
# 目标侧与宿主侧的密码学各有同名实现，按对象名区分
OBJS := $(OBJ_DIR)/main.o \
        $(OBJ_DIR)/Revocation.target.o $(OBJ_DIR)/BootState.target.o $(OBJ_DIR)/KernelImage.target.o \
        $(OBJ_DIR)/LoadKernel.target.o $(OBJ_DIR)/BootMemory.target.o \
        $(OBJ_DIR)/ManifestVerify.target.o $(OBJ_DIR)/TrustAnchor.target.o \
        $(OBJ_DIR)/Error.target.o $(OBJ_DIR)/Path.target.o $(OBJ_DIR)/FileSystem.target.o \
        $(OBJ_DIR)/Sha256.target.o $(OBJ_DIR)/Ed25519.target.o $(OBJ_DIR)/Sha512.target.o \
        $(OBJ_DIR)/Sha256.host.o $(OBJ_DIR)/Ed25519.host.o $(OBJ_DIR)/Sha512.host.o $(OBJ_DIR)/HostIo.o

vpath %.cpp $(SRC_DIR) $(HOST_SRC)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

# 共用实现与目标侧源码：显式规则，避免与宿主侧同名文件撞名
$(OBJ_DIR)/Revocation.target.o: $(COMMON_DIR)/$(SRC_DIR)/Revocation.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/BootState.target.o: $(COMMON_DIR)/$(SRC_DIR)/BootState.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/KernelImage.target.o: $(COMMON_DIR)/$(SRC_DIR)/KernelImage.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/LoadKernel.target.o: $(CORE_DIR)/$(SRC_DIR)/LoadKernel.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/BootMemory.target.o: $(BALEEN_COMMON_DIR)/$(SRC_DIR)/BootMemory.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Error.target.o: $(COMMON_DIR)/$(SRC_DIR)/Storage/Error.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Path.target.o: $(COMMON_DIR)/$(SRC_DIR)/Storage/Path.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/FileSystem.target.o: $(COMMON_DIR)/$(SRC_DIR)/Storage/FileSystem.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/ManifestVerify.target.o: $(CORE_DIR)/$(SRC_DIR)/ManifestVerify.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 目标侧信任锚查找：与本工具自带的锚表一起验按 KeyId 取公钥
$(OBJ_DIR)/TrustAnchor.target.o: $(CORE_DIR)/$(SRC_DIR)/TrustAnchor.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 密码学：目标侧供清单与记录验签，宿主侧供夹具构造与签名
$(OBJ_DIR)/Sha256.target.o: $(BALEEN_COMMON_DIR)/$(SRC_DIR)/Sha256.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Ed25519.target.o: $(BALEEN_COMMON_DIR)/$(SRC_DIR)/Ed25519.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Sha512.target.o: $(BALEEN_COMMON_DIR)/$(SRC_DIR)/Sha512.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Sha256.host.o: $(HOST_SRC)/Sha256.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Ed25519.host.o: $(HOST_SRC)/Ed25519.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJ_DIR)/Sha512.host.o: $(HOST_SRC)/Sha512.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

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
	@echo "CheckRevocation（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(OBJS:.o=.d)
