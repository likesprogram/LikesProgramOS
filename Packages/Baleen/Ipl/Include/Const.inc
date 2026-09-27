; 与 BootInfo.hpp、Superblock.hpp 保持数值一致

MAGIC            EQU 0x20020115  ; Superblock / Boot::Info 魔数
VERSION          EQU 3           ; Superblock 布局版本（3 = 追加升级区字段，见 ADR-034 的 S4）

LOAD_TOP         EQU 0x7E00      ; BootStub 装入点，紧邻 IPL 之上
LOAD_CEIL        EQU 0x10000     ; BootStub 镜像（含 BSS）上限；stub/Bios.asm 实模式 thunk 以 CS=0 取址
BIOS_LOAD        EQU 0x7C00      ; BIOS 把 IPL 放到的物理地址；IPL 留在此处
STACK_TOP        EQU 0x7C00      ; 16 位阶段栈顶（BootStub 之下向下长，避开其镜像）
LOADER_PM_STACK  EQU 0x21000     ; BootStub/BootCore 32 位栈顶，栈区 [0x20000,0x21000)
BOUNCE_PHYS      EQU 0x0800      ; BootStub 的 INT 13h 弹跳缓冲（须在低 64KiB）

SB_MAGIC         EQU 0           ; Superblock.magic 偏移
SB_VERSION       EQU 4           ; Superblock.version 偏移
SB_MEDIA         EQU 8           ; Superblock.media 偏移
SB_LOADER_LBA    EQU 12          ; Superblock.loaderLba 偏移
SB_LOADER_SECTS  EQU 16          ; Superblock.loaderSectors 偏移
SB_SECTOR_SIZE   EQU 44          ; Superblock.sectorSize 偏移
SB_SYSTEM_FS     EQU 176         ; Superblock.systemFs 偏移（v2 追加字段）

SUPER_FS_UNKNOWN EQU 0           ; Superblock.systemFs：按注册表顺序探测
SUPER_FS_EXFAT   EQU 1           ; Superblock.systemFs：ExFAT 系统卷
SUPER_FS_EXT4    EQU 2           ; Superblock.systemFs：Ext4 系统卷

MEDIA_HDD        EQU 2           ; Boot::Media::Hdd（U 盘 USB-HDD 同此）
MEDIA_USB        EQU 3           ; Boot::Media::Usb（本 IPL 不用）
MEDIA_CDROM      EQU 4           ; Boot::Media::Cdrom
