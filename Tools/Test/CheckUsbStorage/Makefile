# Makefile
#    CheckUsbStorage —— UsbStorage 模块（BOT/SCSI/BlockDeviceOps/Dev.Model v2 登记）的宿主侧检查

OBJ_DIR  ?= Obj
BIN_DIR  ?= ../../Bin
SRC_DIR  := Src
KMOD_DIR := ../../../Packages/KernelModules
USB_DIR  := $(KMOD_DIR)/UsbStorage
COMMON_DIR   := ../../../Packages/Common
CONTRACT_DIR := $(COMMON_DIR)/Out/Contract

CXX      ?= g++
CPPFLAGS ?= -I$(USB_DIR)/Include -I$(KMOD_DIR)/Common -I$(CONTRACT_DIR) -I$(COMMON_DIR)/Include
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror
LDFLAGS  ?= -Wl,--fatal-warnings

BIN  := $(BIN_DIR)/CheckUsbStorage
OBJS := $(OBJ_DIR)/main.o $(OBJ_DIR)/Module.o

vpath %.cpp $(SRC_DIR) $(USB_DIR)/$(SRC_DIR)

.DEFAULT_GOAL := all
.PHONY: all clean help

all: $(BIN)
$(BIN): $(OBJS) | $(BIN_DIR)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)
$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c -o $@ $<
$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@
clean:
	rm -rf $(OBJ_DIR)
	rm -f $(BIN)
help:
	@echo "CheckUsbStorage（宿主工具）：make [all|clean]"
-include $(OBJS:.o=.d)
