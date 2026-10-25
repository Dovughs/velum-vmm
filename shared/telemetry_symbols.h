// Symbolic input fields; values from Windows SDK/WDK 10.0.26100.0.
// No allocation or OS calls. For deferred telemetry consumers only.
#pragma once
#include <stddef.h>
typedef struct HV_SYMBOL { unsigned long long Value; const char *Name; } HV_SYMBOL;
#define HV_SYMBOL_TEXT_CAPACITY 2048u
static const HV_SYMBOL HvSymbolsProcessorMode[] = {
    {0, "KernelMode"}, {1, "UserMode"}
};
static const HV_SYMBOL HvSymbolsDuplicateOptions[] = {
    {1, "DUPLICATE_CLOSE_SOURCE"}, {2, "DUPLICATE_SAME_ACCESS"}, {4, "DUPLICATE_SAME_ATTRIBUTES"}
};
static const HV_SYMBOL HvSymbolsDuplicateReason[] = {
    {0, "NONE"}, {1, "SAME_ACCESS_RESTRICTION_DENIED"}, {2, "GENERIC_ACCESS_RESTRICTION_DENIED"},
    {3, "ARGUMENT_WRITE_FAILED_DENIED"}, {4, "UNKNOWN_OPTIONS_DENIED"}, {5, "DENY_RULE"}
};
static const HV_SYMBOL HvSymbolsFileAccess[] = {
    { 0x0001ull, "FILE_READ_DATA" },
    { 0x0002ull, "FILE_WRITE_DATA" },
    { 0x0004ull, "FILE_APPEND_DATA" },
    { 0x0008ull, "FILE_READ_EA" },
    { 0x0010ull, "FILE_WRITE_EA" },
    { 0x0020ull, "FILE_EXECUTE" },
    { 0x0040ull, "FILE_DELETE_CHILD" },
    { 0x0080ull, "FILE_READ_ATTRIBUTES" },
    { 0x0100ull, "FILE_WRITE_ATTRIBUTES" },
};
static const HV_SYMBOL HvSymbolsProcessAccess[] = {
    { 0x0001ull, "PROCESS_TERMINATE" },
    { 0x0002ull, "PROCESS_CREATE_THREAD" },
    { 0x0004ull, "PROCESS_SET_SESSIONID" },
    { 0x0008ull, "PROCESS_VM_OPERATION" },
    { 0x0010ull, "PROCESS_VM_READ" },
    { 0x0020ull, "PROCESS_VM_WRITE" },
    { 0x0040ull, "PROCESS_DUP_HANDLE" },
    { 0x0080ull, "PROCESS_CREATE_PROCESS" },
    { 0x0100ull, "PROCESS_SET_QUOTA" },
    { 0x0200ull, "PROCESS_SET_INFORMATION" },
    { 0x0400ull, "PROCESS_QUERY_INFORMATION" },
    { 0x0800ull, "PROCESS_SUSPEND_RESUME" },
    { 0x1000ull, "PROCESS_QUERY_LIMITED_INFORMATION" },
    { 0x2000ull, "PROCESS_SET_LIMITED_INFORMATION" },
};
static const HV_SYMBOL HvSymbolsThreadAccess[] = {
    { 0x0001ull, "THREAD_TERMINATE" },
    { 0x0002ull, "THREAD_SUSPEND_RESUME" },
    { 0x0008ull, "THREAD_GET_CONTEXT" },
    { 0x0010ull, "THREAD_SET_CONTEXT" },
    { 0x0020ull, "THREAD_SET_INFORMATION" },
    { 0x0040ull, "THREAD_QUERY_INFORMATION" },
    { 0x0080ull, "THREAD_SET_THREAD_TOKEN" },
    { 0x0100ull, "THREAD_IMPERSONATE" },
    { 0x0200ull, "THREAD_DIRECT_IMPERSONATION" },
    { 0x0400ull, "THREAD_SET_LIMITED_INFORMATION" },
    { 0x0800ull, "THREAD_QUERY_LIMITED_INFORMATION" },
    { 0x1000ull, "THREAD_RESUME" },
};
static const HV_SYMBOL HvSymbolsSectionAccess[] = {
    { 0x0001ull, "SECTION_QUERY" },
    { 0x0002ull, "SECTION_MAP_WRITE" },
    { 0x0004ull, "SECTION_MAP_READ" },
    { 0x0008ull, "SECTION_MAP_EXECUTE" },
    { 0x0010ull, "SECTION_EXTEND_SIZE" },
    { 0x0020ull, "SECTION_MAP_EXECUTE_EXPLICIT" },
};
static const HV_SYMBOL HvSymbolsCommonAccess[] = {
    { 0x00010000ull, "DELETE" },
    { 0x00020000ull, "READ_CONTROL" },
    { 0x00040000ull, "WRITE_DAC" },
    { 0x00080000ull, "WRITE_OWNER" },
    { 0x00100000ull, "SYNCHRONIZE" },
    { 0x01000000ull, "ACCESS_SYSTEM_SECURITY" },
    { 0x02000000ull, "MAXIMUM_ALLOWED" },
    { 0x10000000ull, "GENERIC_ALL" },
    { 0x20000000ull, "GENERIC_EXECUTE" },
    { 0x40000000ull, "GENERIC_WRITE" },
    { 0x80000000ull, "GENERIC_READ" },
};
static const HV_SYMBOL HvSymbolsShare[] = {
    { 0x00000001ull, "FILE_SHARE_READ" },
    { 0x00000002ull, "FILE_SHARE_WRITE" },
    { 0x00000004ull, "FILE_SHARE_DELETE" },
};
static const HV_SYMBOL HvSymbolsOptions[] = {
    { 0x00000001ull, "FILE_DIRECTORY_FILE" },
    { 0x00000002ull, "FILE_WRITE_THROUGH" },
    { 0x00000004ull, "FILE_SEQUENTIAL_ONLY" },
    { 0x00000008ull, "FILE_NO_INTERMEDIATE_BUFFERING" },
    { 0x00000010ull, "FILE_SYNCHRONOUS_IO_ALERT" },
    { 0x00000020ull, "FILE_SYNCHRONOUS_IO_NONALERT" },
    { 0x00000040ull, "FILE_NON_DIRECTORY_FILE" },
    { 0x00000080ull, "FILE_CREATE_TREE_CONNECTION" },
    { 0x00000100ull, "FILE_COMPLETE_IF_OPLOCKED" },
    { 0x00000200ull, "FILE_NO_EA_KNOWLEDGE" },
    { 0x00000400ull, "FILE_OPEN_REMOTE_INSTANCE" },
    { 0x00000800ull, "FILE_RANDOM_ACCESS" },
    { 0x00001000ull, "FILE_DELETE_ON_CLOSE" },
    { 0x00002000ull, "FILE_OPEN_BY_FILE_ID" },
    { 0x00004000ull, "FILE_OPEN_FOR_BACKUP_INTENT" },
    { 0x00008000ull, "FILE_NO_COMPRESSION" },
    { 0x00010000ull, "FILE_OPEN_REQUIRING_OPLOCK" },
    { 0x00020000ull, "FILE_DISALLOW_EXCLUSIVE" },
    { 0x00040000ull, "FILE_SESSION_AWARE" },
    { 0x00100000ull, "FILE_RESERVE_OPFILTER" },
    { 0x00200000ull, "FILE_OPEN_REPARSE_POINT" },
    { 0x00400000ull, "FILE_OPEN_NO_RECALL" },
    { 0x00800000ull, "FILE_OPEN_FOR_FREE_SPACE_QUERY" },
};
static const HV_SYMBOL HvSymbolsAttributes[] = {
    { 0x00000001ull, "FILE_ATTRIBUTE_READONLY" },
    { 0x00000002ull, "FILE_ATTRIBUTE_HIDDEN" },
    { 0x00000004ull, "FILE_ATTRIBUTE_SYSTEM" },
    { 0x00000010ull, "FILE_ATTRIBUTE_DIRECTORY" },
    { 0x00000020ull, "FILE_ATTRIBUTE_ARCHIVE" },
    { 0x00000040ull, "FILE_ATTRIBUTE_DEVICE" },
    { 0x00000080ull, "FILE_ATTRIBUTE_NORMAL" },
    { 0x00000100ull, "FILE_ATTRIBUTE_TEMPORARY" },
    { 0x00000200ull, "FILE_ATTRIBUTE_SPARSE_FILE" },
    { 0x00000400ull, "FILE_ATTRIBUTE_REPARSE_POINT" },
    { 0x00000800ull, "FILE_ATTRIBUTE_COMPRESSED" },
    { 0x00001000ull, "FILE_ATTRIBUTE_OFFLINE" },
    { 0x00002000ull, "FILE_ATTRIBUTE_NOT_CONTENT_INDEXED" },
    { 0x00004000ull, "FILE_ATTRIBUTE_ENCRYPTED" },
    { 0x00008000ull, "FILE_ATTRIBUTE_INTEGRITY_STREAM" },
    { 0x00010000ull, "FILE_ATTRIBUTE_VIRTUAL" },
    { 0x00020000ull, "FILE_ATTRIBUTE_NO_SCRUB_DATA" },
    { 0x00080000ull, "FILE_ATTRIBUTE_PINNED" },
    { 0x00100000ull, "FILE_ATTRIBUTE_UNPINNED" },
};
static const HV_SYMBOL HvSymbolsObject[] = {
    { 0x00000002ull, "OBJ_INHERIT" },
    { 0x00000010ull, "OBJ_PERMANENT" },
    { 0x00000020ull, "OBJ_EXCLUSIVE" },
    { 0x00000040ull, "OBJ_CASE_INSENSITIVE" },
    { 0x00000080ull, "OBJ_OPENIF" },
    { 0x00000100ull, "OBJ_OPENLINK" },
    { 0x00000200ull, "OBJ_KERNEL_HANDLE" },
    { 0x00000400ull, "OBJ_FORCE_ACCESS_CHECK" },
    { 0x00000800ull, "OBJ_IGNORE_IMPERSONATED_DEVICEMAP" },
    { 0x00001000ull, "OBJ_DONT_REPARSE" },
};
static const HV_SYMBOL HvSymbolsProtection[] = {
    { 0x01ull, "PAGE_NOACCESS" },
    { 0x02ull, "PAGE_READONLY" },
    { 0x04ull, "PAGE_READWRITE" },
    { 0x08ull, "PAGE_WRITECOPY" },
    { 0x10ull, "PAGE_EXECUTE" },
    { 0x20ull, "PAGE_EXECUTE_READ" },
    { 0x40ull, "PAGE_EXECUTE_READWRITE" },
    { 0x80ull, "PAGE_EXECUTE_WRITECOPY" },
    { 0x100ull, "PAGE_GUARD" },
    { 0x200ull, "PAGE_NOCACHE" },
    { 0x400ull, "PAGE_WRITECOMBINE" },
};
static const HV_SYMBOL HvSymbolsAllocation[] = {
    { 0x00001000ull, "MEM_COMMIT" },
    { 0x00002000ull, "MEM_RESERVE" },
    { 0x00080000ull, "MEM_RESET" },
    { 0x00100000ull, "MEM_TOP_DOWN" },
    { 0x00200000ull, "MEM_WRITE_WATCH" },
    { 0x00400000ull, "MEM_PHYSICAL" },
    { 0x01000000ull, "MEM_RESET_UNDO" },
    { 0x20000000ull, "MEM_LARGE_PAGES" },
};
static const HV_SYMBOL HvSymbolsMapAllocation[] = {
    { 0x00002000ull, "MEM_RESERVE" },
    { 0x00004000ull, "MEM_REPLACE_PLACEHOLDER" },
    { 0x00100000ull, "MEM_TOP_DOWN" },
    { 0x00800000ull, "MEM_DIFFERENT_IMAGE_BASE_OK" },
    { 0x20000000ull, "MEM_LARGE_PAGES" },
};
static const HV_SYMBOL HvSymbolsSection[] = {
    { 0x11000000ull, "SEC_IMAGE_NO_EXECUTE" },
    { 0x00020000ull, "SEC_HUGE_PAGES" },
    { 0x00040000ull, "SEC_PARTITION_OWNER_HANDLE" },
    { 0x00080000ull, "SEC_64K_PAGES" },
    { 0x00800000ull, "SEC_FILE" },
    { 0x01000000ull, "SEC_IMAGE" },
    { 0x02000000ull, "SEC_PROTECTED_IMAGE" },
    { 0x04000000ull, "SEC_RESERVE" },
    { 0x08000000ull, "SEC_COMMIT" },
    { 0x10000000ull, "SEC_NOCACHE" },
    { 0x40000000ull, "SEC_WRITECOMBINE" },
    { 0x80000000ull, "SEC_LARGE_PAGES" },
};
static const HV_SYMBOL HvSymbolsDisposition[] = {
    { 0x1ull, "FILE_DISPOSITION_DELETE" },
    { 0x2ull, "FILE_DISPOSITION_POSIX_SEMANTICS" },
    { 0x4ull, "FILE_DISPOSITION_FORCE_IMAGE_SECTION_CHECK" },
    { 0x8ull, "FILE_DISPOSITION_ON_CLOSE" },
    { 0x10ull, "FILE_DISPOSITION_IGNORE_READONLY_ATTRIBUTE" },
};
static const HV_SYMBOL HvSymbolsRename[] = {
    { 0x00000001ull, "FILE_RENAME_REPLACE_IF_EXISTS" },
    { 0x00000002ull, "FILE_RENAME_POSIX_SEMANTICS" },
    { 0x00000004ull, "FILE_RENAME_SUPPRESS_PIN_STATE_INHERITANCE" },
    { 0x00000008ull, "FILE_RENAME_SUPPRESS_STORAGE_RESERVE_INHERITANCE" },
    { 0x00000010ull, "FILE_RENAME_NO_INCREASE_AVAILABLE_SPACE" },
    { 0x00000020ull, "FILE_RENAME_NO_DECREASE_AVAILABLE_SPACE" },
    { 0x00000040ull, "FILE_RENAME_IGNORE_READONLY_ATTRIBUTE" },
    { 0x00000080ull, "FILE_RENAME_FORCE_RESIZE_TARGET_SR" },
    { 0x00000100ull, "FILE_RENAME_FORCE_RESIZE_SOURCE_SR" },
};
static const HV_SYMBOL HvSymbolsLink[] = {
    { 0x00000001ull, "FILE_LINK_REPLACE_IF_EXISTS" },
    { 0x00000002ull, "FILE_LINK_POSIX_SEMANTICS" },
    { 0x00000008ull, "FILE_LINK_SUPPRESS_STORAGE_RESERVE_INHERITANCE" },
    { 0x00000010ull, "FILE_LINK_NO_INCREASE_AVAILABLE_SPACE" },
    { 0x00000020ull, "FILE_LINK_NO_DECREASE_AVAILABLE_SPACE" },
    { 0x00000040ull, "FILE_LINK_IGNORE_READONLY_ATTRIBUTE" },
    { 0x00000080ull, "FILE_LINK_FORCE_RESIZE_TARGET_SR" },
    { 0x00000100ull, "FILE_LINK_FORCE_RESIZE_SOURCE_SR" },
};


static const HV_SYMBOL HvSymbolsPoolFlags[] = {
    { 0x0000000000000001ull, "POOL_FLAG_USE_QUOTA" },
    { 0x0000000000000002ull, "POOL_FLAG_UNINITIALIZED" },
    { 0x0000000000000004ull, "POOL_FLAG_SESSION" },
    { 0x0000000000000008ull, "POOL_FLAG_CACHE_ALIGNED" },
    { 0x0000000000000020ull, "POOL_FLAG_RAISE_ON_FAILURE" },
    { 0x0000000000000040ull, "POOL_FLAG_NON_PAGED" },
    { 0x0000000000000080ull, "POOL_FLAG_NON_PAGED_EXECUTE" },
    { 0x0000000000000100ull, "POOL_FLAG_PAGED" },
    { 0x0000000100000000ull, "POOL_FLAG_SPECIAL_POOL" },
};
static const HV_SYMBOL HvSymbolsPoolType[] = {
    {0, "NonPagedPool"}, {1, "PagedPool"}, {2, "NonPagedPoolMustSucceed"},
    {4, "NonPagedPoolCacheAligned"}, {5, "PagedPoolCacheAligned"},
    {6, "NonPagedPoolCacheAlignedMustS"}, {32, "NonPagedPoolSession"},
    {33, "PagedPoolSession"}, {36, "NonPagedPoolCacheAlignedSession"},
    {37, "PagedPoolCacheAlignedSession"}, {512, "NonPagedPoolNx"},
    {516, "NonPagedPoolNxCacheAligned"}, {544, "NonPagedPoolSessionNx"}
};
static const HV_SYMBOL HvSymbolsPoolPriority[] = {
    {0, "LowPoolPriority"}, {8, "LowPoolPrioritySpecialPoolOverrun"},
    {9, "LowPoolPrioritySpecialPoolUnderrun"}, {16, "NormalPoolPriority"},
    {24, "NormalPoolPrioritySpecialPoolOverrun"}, {25, "NormalPoolPrioritySpecialPoolUnderrun"},
    {32, "HighPoolPriority"}, {40, "HighPoolPrioritySpecialPoolOverrun"},
    {41, "HighPoolPrioritySpecialPoolUnderrun"}
};
static const HV_SYMBOL HvSymbolsCreateDisposition[] = {
    {0, "FILE_SUPERSEDE"}, {1, "FILE_OPEN"}, {2, "FILE_CREATE"},
    {3, "FILE_OPEN_IF"}, {4, "FILE_OVERWRITE"}, {5, "FILE_OVERWRITE_IF"}
};
static const HV_SYMBOL HvSymbolsInformation[] = {
    { 1, "FileDirectoryInformation" },
    { 2, "FileFullDirectoryInformation" },
    { 3, "FileBothDirectoryInformation" },
    { 4, "FileBasicInformation" },
    { 5, "FileStandardInformation" },
    { 6, "FileInternalInformation" },
    { 7, "FileEaInformation" },
    { 8, "FileAccessInformation" },
    { 9, "FileNameInformation" },
    { 10, "FileRenameInformation" },
    { 11, "FileLinkInformation" },
    { 12, "FileNamesInformation" },
    { 13, "FileDispositionInformation" },
    { 14, "FilePositionInformation" },
    { 15, "FileFullEaInformation" },
    { 16, "FileModeInformation" },
    { 17, "FileAlignmentInformation" },
    { 18, "FileAllInformation" },
    { 19, "FileAllocationInformation" },
    { 20, "FileEndOfFileInformation" },
    { 21, "FileAlternateNameInformation" },
    { 22, "FileStreamInformation" },
    { 23, "FilePipeInformation" },
    { 24, "FilePipeLocalInformation" },
    { 25, "FilePipeRemoteInformation" },
    { 26, "FileMailslotQueryInformation" },
    { 27, "FileMailslotSetInformation" },
    { 28, "FileCompressionInformation" },
    { 29, "FileObjectIdInformation" },
    { 30, "FileCompletionInformation" },
    { 31, "FileMoveClusterInformation" },
    { 32, "FileQuotaInformation" },
    { 33, "FileReparsePointInformation" },
    { 34, "FileNetworkOpenInformation" },
    { 35, "FileAttributeTagInformation" },
    { 36, "FileTrackingInformation" },
    { 37, "FileIdBothDirectoryInformation" },
    { 38, "FileIdFullDirectoryInformation" },
    { 39, "FileValidDataLengthInformation" },
    { 40, "FileShortNameInformation" },
    { 41, "FileIoCompletionNotificationInformation" },
    { 42, "FileIoStatusBlockRangeInformation" },
    { 43, "FileIoPriorityHintInformation" },
    { 44, "FileSfioReserveInformation" },
    { 45, "FileSfioVolumeInformation" },
    { 46, "FileHardLinkInformation" },
    { 47, "FileProcessIdsUsingFileInformation" },
    { 48, "FileNormalizedNameInformation" },
    { 49, "FileNetworkPhysicalNameInformation" },
    { 50, "FileIdGlobalTxDirectoryInformation" },
    { 51, "FileIsRemoteDeviceInformation" },
    { 52, "FileUnusedInformation" },
    { 53, "FileNumaNodeInformation" },
    { 54, "FileStandardLinkInformation" },
    { 55, "FileRemoteProtocolInformation" },
    { 56, "FileRenameInformationBypassAccessCheck" },
    { 57, "FileLinkInformationBypassAccessCheck" },
    { 58, "FileVolumeNameInformation" },
    { 59, "FileIdInformation" },
    { 60, "FileIdExtdDirectoryInformation" },
    { 61, "FileReplaceCompletionInformation" },
    { 62, "FileHardLinkFullIdInformation" },
    { 63, "FileIdExtdBothDirectoryInformation" },
    { 64, "FileDispositionInformationEx" },
    { 65, "FileRenameInformationEx" },
    { 66, "FileRenameInformationExBypassAccessCheck" },
    { 67, "FileDesiredStorageClassInformation" },
    { 68, "FileStatInformation" },
    { 69, "FileMemoryPartitionInformation" },
    { 70, "FileStatLxInformation" },
    { 71, "FileCaseSensitiveInformation" },
    { 72, "FileLinkInformationEx" },
    { 73, "FileLinkInformationExBypassAccessCheck" },
    { 74, "FileStorageReserveIdInformation" },
    { 75, "FileCaseSensitiveInformationForceAccessCheck" },
    { 76, "FileKnownFolderInformation" },
    { 77, "FileStatBasicInformation" },
    { 78, "FileId64ExtdDirectoryInformation" },
    { 79, "FileId64ExtdBothDirectoryInformation" },
    { 80, "FileIdAllExtdDirectoryInformation" },
    { 81, "FileIdAllExtdBothDirectoryInformation" },
    { 82, "FileStreamReservationInformation" },
    { 83, "FileMupProviderInfo" },
};
static const HV_SYMBOL HvSymbolsInherit[] = {{1, "ViewShare"}, {2, "ViewUnmap"}};
static const HV_SYMBOL HvSymbolsBoolean[] = {{0, "FALSE"}, {1, "TRUE"}};
// Native thread flags: https://github.com/winsiderss/phnt/blob/master/ntpsapi.h
static inline void HvSymbolAppend(char *Buffer, size_t Capacity, const char *Text)
{
    size_t i = 0;
    if (!Capacity) { return; }
    while (i + 1 < Capacity && Buffer[i]) { ++i; }
    while (i + 1 < Capacity && *Text) { Buffer[i++] = *Text++; }
    Buffer[i] = 0;
}
static inline void HvSymbolHex(char *Buffer, size_t Capacity, unsigned long long Value)
{
    char hex[17];
    unsigned int i, start = 0;
    for (i = 0; i < 16; ++i) {
        hex[i] = "0123456789ABCDEF"[(Value >> ((15 - i) * 4)) & 15];
    }
    hex[16] = 0;
    while (start < 15 && hex[start] == '0') { ++start; }
    HvSymbolAppend(Buffer, Capacity, "0x");
    HvSymbolAppend(Buffer, Capacity, hex + start);
}
static inline void HvSymbolBits(char *Buffer, size_t Capacity,
    const HV_SYMBOL *Table, size_t Count, unsigned long long *Remaining)
{
    size_t i;
    for (i = 0; i < Count; ++i) {
        if (Table[i].Value && (*Remaining & Table[i].Value) == Table[i].Value) {
            if (Buffer[0]) { HvSymbolAppend(Buffer, Capacity, " | "); }
            HvSymbolAppend(Buffer, Capacity, Table[i].Name);
            *Remaining &= ~Table[i].Value;
        }
    }
}
// Handles, pointers and sizes have no symbol table. Only valid selectors select unions.
static inline void HvFormatField(unsigned int Target, unsigned int Field,
    unsigned int FieldMask, const unsigned long long *Fields, char *Buffer, size_t Capacity)
{
    const HV_SYMBOL *table = NULL;
    size_t count = 0, i;
    int enumeration = 0, access = 0;
    unsigned long long value, remaining;
    if (!Capacity) { return; }
    Buffer[0] = 0;
    if (Target >= 26 || Target == 9 || Target == 11 || Field >= 12 || !(FieldMask & (1u << Field))) { return; }
    value = remaining = Fields[Field];
#define HV_USE(Name) do { table = HvSymbols##Name; count = sizeof(HvSymbols##Name) / sizeof(HV_SYMBOL); } while (0)
    // A duplicated handle may represent any object type. Do not label unknown
    // object-specific rights as process rights merely because the values overlap.
    if (Target == 25 && Field == 5) { HV_USE(ProcessorMode); enumeration = 1; }
    else if (Target == 24 && Field == 0) { HV_USE(CommonAccess); }
    else if (Target == 24 && Field == 4) { HV_USE(Object); }
    else if (Target == 24 && Field == 5) { HV_USE(DuplicateOptions); }
    else if (Target == 24 && Field >= 8 && Field <= 10) { HV_USE(Boolean); enumeration = 1; }
    else if (Target == 24 && Field == 11) { HV_USE(DuplicateReason); enumeration = 1; }
    else if ((Target == 13 || Target == 17) && Field == 0) { HV_USE(PoolFlags); }
    else if ((Target == 14 || Target == 16 || Target == 18) && Field == 0) { HV_USE(PoolType); enumeration = 1; }
    else if (Target == 16 && Field == 3) { HV_USE(PoolPriority); enumeration = 1; }
    else if (Field == 7 && (Target == 0 || Target == 2 || Target == 3 || Target == 6 || Target == 8)) {
        HV_USE(Object);
    } else if (Field == 0 && (Target == 0 || Target == 2 || Target == 6 || Target == 8)) {
        access = 1;
        if (Target == 6) { HV_USE(SectionAccess); }
        else if (Target == 8) { HV_USE(ProcessAccess); }
        else { HV_USE(FileAccess); }
    } else if ((Target == 0 || Target == 2) && Field == 1) { HV_USE(Share); }
    else if ((Target == 0 || Target == 2) && Field == 3) { HV_USE(Options); }
    else if (Target == 0 && Field == 2) { HV_USE(CreateDisposition); enumeration = 1; }
    else if (Target == 0 && Field == 4) { HV_USE(Attributes); }
    else if ((Target == 1 && Field == 5) || (Target == 6 && Field == 1) ||
        (Target == 7 && Field == 9) || (Target == 10 && Field == 3) || (Target == 22 && Field == 2)) { HV_USE(Protection); }
    else if (Target == 1 && Field == 4) { HV_USE(Allocation); }
    else if (Target == 7 && Field == 8) { HV_USE(MapAllocation); }
    else if (Target == 6 && Field == 2) { HV_USE(Section); }
    else if (Target == 7 && Field == 7) { HV_USE(Inherit); enumeration = 1; }
    else if ((Target == 5 || Target == 12) && Field == 3) { HV_USE(Boolean); enumeration = 1; }
    else if ((Target == 5 || Target == 12) && Field == 2 &&
        (value == ~0ull - 1 || (Target == 5 && value == ~0ull))) {
        HvSymbolAppend(Buffer, Capacity, value == ~0ull ? "FILE_WRITE_TO_END_OF_FILE" : "FILE_USE_FILE_POINTER_POSITION");
    } else if (Target == 4 && Field == 0) { HV_USE(Information); enumeration = 1; }
    else if (Target == 4 && Field == 2 && (FieldMask & 1)) {
        switch (Fields[0]) {
        case 10: case 11: case 13: case 56: case 57: HV_USE(Boolean); enumeration = 1; break;
        case 64: HV_USE(Disposition); break;
        case 65: case 66: HV_USE(Rename); break;
        case 72: case 73: HV_USE(Link); break;
        default: break;
        }
    }
#undef HV_USE
    if (table) {
        if (enumeration) {
            for (i = 0; i < count; ++i) {
                if (table[i].Value == value) { HvSymbolAppend(Buffer, Capacity, table[i].Name); break; }
            }
            if (!Buffer[0]) { HvSymbolAppend(Buffer, Capacity, "UNKNOWN"); }
        } else {
            HvSymbolBits(Buffer, Capacity, table, count, &remaining);
            if (access) {
                HvSymbolBits(Buffer, Capacity, HvSymbolsCommonAccess,
                    sizeof(HvSymbolsCommonAccess) / sizeof(HV_SYMBOL), &remaining);
            }
            if (remaining) {
                if (Buffer[0]) { HvSymbolAppend(Buffer, Capacity, " | "); }
                HvSymbolAppend(Buffer, Capacity, "UNKNOWN_BITS(");
                HvSymbolHex(Buffer, Capacity, remaining);
                HvSymbolAppend(Buffer, Capacity, ")");
            }
            if (!Buffer[0]) { HvSymbolAppend(Buffer, Capacity, "NONE"); }
        }
    }
    if (Buffer[0]) {
        HvSymbolAppend(Buffer, Capacity, " (");
        HvSymbolHex(Buffer, Capacity, value);
        HvSymbolAppend(Buffer, Capacity, ")");
    } else { HvSymbolHex(Buffer, Capacity, value); }
}

