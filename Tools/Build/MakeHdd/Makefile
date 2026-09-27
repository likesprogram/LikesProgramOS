# MakeHdd —— 宿主侧磁盘镜像组装器（C++20）

OBJ_DIR    ?= Obj
BIN_DIR    ?= ../../Bin
SRC_DIR    := Src
INC_DIR    := Include
COMMON_DIR := ../Common

CXX      ?= g++
CPPFLAGS ?= -I$(INC_DIR) -I$(COMMON_DIR)/Include
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
LDFLAGS  ?=

BIN  := $(BIN_DIR)/MakeHdd
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Image.o $(OBJ_DIR)/Baleen.o $(OBJ_DIR)/HostIo.o $(OBJ_DIR)/Fat.o
DEPS := $(OBJS:.o=.d)

# 镜像封装、引导结构、宿主文件工具与 FAT 校验在 ../Common 共享
vpath %.cpp $(SRC_DIR) $(COMMON_DIR)/$(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all test clean help

all: $(BIN)

$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# 冒烟测试：合成载荷组装磁盘镜像，校验分区表、BootDescriptor、分区内容与可复现性
test: $(BIN)
	MAKEHDD="$(abspath $(BIN))" sh Test/Smoke.sh

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

# Bin 目录与 MakeIso 共用，clean 只删本工具的可执行文件
clean:
	@test -n "$(OBJ_DIR)" || { echo "clean: OBJ_DIR 为空，拒绝执行"; exit 1; }
	rm -rf $(OBJ_DIR)
	@test -n "$(BIN)" || { echo "clean: BIN 为空，拒绝执行"; exit 1; }
	rm -f $(BIN)

help:
	@echo "MakeHdd（宿主工具）"
	@echo "  make [all]   构建 $(BIN)"
	@echo "  make test    组装测试镜像并校验"
	@echo "  make clean   删除 $(OBJ_DIR) 与本工具的可执行文件"

-include $(DEPS)
