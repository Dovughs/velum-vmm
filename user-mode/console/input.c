#include "app.h"
#include "telemetry_symbols.h"
#include <ctype.h>
#include <errno.h>
#include <wctype.h>

// Each field has its own namespace; equal numeric bits do not imply equal semantics.
typedef struct INPUT_SYMBOLS {
    const HV_SYMBOL *Table[4];
    size_t Count[4];
    unsigned Used;
    int Enum;
} INPUT_SYMBOLS;

#define ADD(Name) do { \
    s.Table[s.Used] = HvSymbols##Name; \
    s.Count[s.Used++] = ARRAYSIZE(HvSymbols##Name); \
} while (0)

static INPUT_SYMBOLS FieldSymbols(ULONG target, ULONG field)
{
    INPUT_SYMBOLS s = {0};
    if (!HvTargetSupported(target)) { return s; }

    if (target == HV_TARGET_COPY_VIRTUAL_MEMORY && field == 5) { ADD(ProcessorMode); s.Enum = 1; }
    else if (target == HV_TARGET_DUPLICATE_OBJECT && field == 0) {
        ADD(ProcessAccess); ADD(ThreadAccess); ADD(FileAccess); ADD(CommonAccess);
    }
    else if (target == HV_TARGET_DUPLICATE_OBJECT && field == 4) { ADD(Object); }
    else if (target == HV_TARGET_DUPLICATE_OBJECT && field == 5) { ADD(DuplicateOptions); }
    else if (target == HV_TARGET_DUPLICATE_OBJECT && field >= 8 && field <= 10) { ADD(Boolean); s.Enum = 1; }
    else if (target == HV_TARGET_DUPLICATE_OBJECT && field == 11) { ADD(DuplicateReason); s.Enum = 1; }
    else if ((target == 13 || target == 17) && field == 0) { ADD(PoolFlags); }
    else if ((target == 14 || target == 16 || target == 18) && field == 0) { ADD(PoolType); s.Enum = 1; }
    else if (target == 16 && field == 3) { ADD(PoolPriority); s.Enum = 1; }
    else if (field == 7 && (target == 0 || target == 2 || target == 3 || target == 6 || target == 8)) {
        ADD(Object);
    } else if (field == 0 && (target == 0 || target == 2 || target == 6 || target == 8)) {
        if (target == 6) { ADD(SectionAccess); }
        else if (target == 8) { ADD(ProcessAccess); }
        else { ADD(FileAccess); }
        ADD(CommonAccess);
    } else if ((target == 0 || target == 2) && field == 1) { ADD(Share); }
    else if ((target == 0 || target == 2) && field == 3) { ADD(Options); }
    else if (target == 0 && field == 2) { ADD(CreateDisposition); s.Enum = 1; }
    else if (target == 0 && field == 4) { ADD(Attributes); }
    else if ((target == 1 && field == 5) || (target == 6 && field == 1) ||
        (target == 7 && field == 9) || (target == 10 && field == 3) || (target == 22 && field == 2)) { ADD(Protection); }
    else if (target == 1 && field == 4) { ADD(Allocation); }
    else if (target == 7 && field == 8) { ADD(MapAllocation); }
    else if (target == 6 && field == 2) { ADD(Section); }
    else if (target == 7 && field == 7) { ADD(Inherit); s.Enum = 1; }
    else if ((target == 5 || target == 12) && field == 3) { ADD(Boolean); s.Enum = 1; }
    else if (target == 4 && field == 0) { ADD(Information); s.Enum = 1; }
    else if (target == 4 && field == 2) {
        // One numeric condition cannot also constrain the information class.
        ADD(Disposition); ADD(Rename); ADD(Link); ADD(Boolean);
    }
    return s;
}
#undef ADD

void AppListFieldSymbols(ULONG Target, ULONG Field)
{
    INPUT_SYMBOLS symbols = FieldSymbols(Target, Field);
    unsigned table;
    size_t i;

    for (table = 0; table < symbols.Used; ++table) {
        for (i = 0; i < symbols.Count[table]; ++i) {
            printf("  %s = 0x%llX\n", symbols.Table[table][i].Name, symbols.Table[table][i].Value);
        }
    }
    if (!symbols.Used) { puts("  This field accepts numeric values only."); }
    if (Target == 4 && Field == 2) {
        puts("  This is a numeric flags/replace filter; the name does not constrain information_class.");
    }
}

int AppParseFieldValue(ULONG Target, ULONG Field, const char *Text, ULONG64 *Value)
{
    INPUT_SYMBOLS symbols = FieldSymbols(Target, Field);
    ULONG64 result = 0;
    unsigned tokens = 0, family = 0;
    const char *cursor = Text;
    if (!HvTargetSupported(Target) || Field >= 12) { return 0; }

    // Publish only after parsing the complete expression.
    for (;;) {
        char token[128], *end;
        size_t length = 0, i;
        unsigned table, found = 0;
        ULONG64 number = 0;

        while (isspace((unsigned char)*cursor)) { ++cursor; }
        while (*cursor && *cursor != '|') {
            if (length + 1 >= sizeof(token)) { return 0; }
            token[length++] = *cursor++;
        }
        while (length && isspace((unsigned char)token[length - 1])) { --length; }
        token[length] = 0;
        if (!length || (symbols.Enum && tokens)) { return 0; }

        if (isdigit((unsigned char)token[0])) {
            errno = 0;
            number = strtoull(token, &end,
                token[0] == '0' && (token[1] == 'x' || token[1] == 'X') ? 16 : 10);
            if (errno == ERANGE || *end) { return 0; }
            found = 1;
        } else {
            if (_stricmp(token, "PAGE_NO_ACCESS") == 0) { strcpy_s(token, sizeof(token), "PAGE_NOACCESS"); }
            for (table = 0; table < symbols.Used && !found; ++table) {
                for (i = 0; i < symbols.Count[table]; ++i) {
                    if (_stricmp(token, symbols.Table[table][i].Name) == 0) {
                        number = symbols.Table[table][i].Value;
                        found = 1;
                        if (Target == 4 && Field == 2) {
                            if (family && family != table + 1) { return 0; }
                            family = table + 1;
                        }
                        break;
                    }
                }
            }
        }
        if (!found) { return 0; }
        result |= number;
        ++tokens;
        if (!*cursor) { break; }
        ++cursor;
    }
    *Value = result;
    return 1;
}

int AppCleanPath(WCHAR *Path)
{
    size_t start = 0, end = wcslen(Path), i;
    WCHAR close = 0;

    // Only trim outside the path; spaces inside quotes belong to the filename.
    while (start < end && iswspace(Path[start])) { ++start; }
    while (end > start && iswspace(Path[end - 1])) { --end; }
    if (start == end) { return 0; }

    if (Path[start] == L'"') { close = L'"'; }
    else if (Path[start] == 0x201C) { close = 0x201D; }
    if (close) {
        if (end - start < 2 || Path[end - 1] != close) { return 0; }
        ++start;
        --end;
    }
    if (start == end) { return 0; }
    for (i = start; i < end; ++i) {
        if (Path[i] == L'"' || Path[i] == 0x201C || Path[i] == 0x201D || Path[i] < 32) { return 0; }
    }

    memmove(Path, Path + start, (end - start) * sizeof(*Path));
    Path[end - start] = 0;
    return 1;
}

