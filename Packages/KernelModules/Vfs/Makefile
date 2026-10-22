# Makefile
#    Vfs 模块包的独立构建入口：配置在 Module.mk、规则在 Common/ModuleRules.mk
#
#    顶层 make -C Packages/KernelModules 用同一套规则构建全部模块包
#    单包构建：make -C Packages/KernelModules/Vfs [all|check|contract-check|package|clean]

include $(CURDIR)/../Common/ModuleRules.mk

# 共用存储纯逻辑：统一路径的语法校验、分量切分与名字比较
# 内核 Vfs 编译 Packages/Common/Src/Storage/Path.cpp 的同一份源码，不复制第二份实现
SHARED_STORAGE_DIR  := $(abspath $(CURDIR)/../../Common/Src/Storage)
SHARED_STORAGE_OBJS := $(OBJ_DIR)/Shared/Path.o
OBJS += $(SHARED_STORAGE_OBJS)

$(OBJ_DIR)/Shared/%.o: $(SHARED_STORAGE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(SHARED_STORAGE_OBJS:.o=.d)

# 链接目标补上共用源码对象：ModuleRules 的 $(ELF) 规则在本文件之前展开 OBJS，
# 这里把追加的对象补成显式前置依赖，make 才会先编它们
$(ELF): $(SHARED_STORAGE_OBJS)
