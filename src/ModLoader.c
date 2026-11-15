/* ============================================================
 * EPets · ModLoader
 *   加载框架：扫描 DLL → 收集内容 → 提供运行期服务
 *
 * 编译: cl /LD /utf-8 /IMODS ModLoader.c /Fe:ModLoader.dll
 * 主程序: #include "ModLoader.h" 并链接 ModLoader.lib
 * ============================================================ */

#define MODLOADER_BUILD 1
#define _CRT_SECURE_NO_WARNINGS 1

#include "ModLoader.h"

#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_PATHLEN 512

/*
 * 所有不确定长度的表都动态扩容：初始长度只是首次申请量，
 * 之后按 2 倍增长，计数类型统一 uint64_t（上限由内存/地址空间决定）。
 */
static int vec_grow(void **ptr, uint64_t *cap, uint64_t need, size_t elem)
{
    uint64_t ncap;
    void *np;

    if (need <= *cap) return 1;
    if (elem == 0 || need > (uint64_t)(SIZE_MAX / elem)) return 0;

    ncap = *cap ? *cap : 16;
    while (ncap < need)
    {
        if (ncap > UINT64_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    if (ncap > (uint64_t)(SIZE_MAX / elem)) ncap = need;

    np = realloc(*ptr, (size_t)ncap * elem);
    if (!np) return 0;
    *ptr = np;
    *cap = ncap;
    return 1;
}

/* ============ 内容表（每种内容一个动态数组） ============ */

static const void **g_list[KIND_COUNT];
static const char **g_owner[KIND_COUNT]; /* 每条内容的注册者（依赖内容检查用） */
static uint64_t g_n[KIND_COUNT];
static uint64_t g_cap[KIND_COUNT];

/* 下标 0 保留给 ID_NONE，所以容量按 cap+1 个元素申请 */
static int content_reserve(ContentKind_t kind, uint64_t need)
{
    uint64_t cap;
    const void **nl;
    const char **no;

    if (kind >= KIND_COUNT) return 0;
    if (need <= g_cap[kind]) return 1;

    cap = g_cap[kind] ? g_cap[kind] : 16;
    while (cap < need)
    {
        if (cap > UINT64_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    if (cap >= (uint64_t)(SIZE_MAX / sizeof(void *))) return 0;

    nl = (const void **)realloc(g_list[kind], (size_t)(cap + 1) * sizeof(*nl));
    if (!nl) return 0;
    g_list[kind] = nl;

    no = (const char **)realloc(g_owner[kind], (size_t)(cap + 1) * sizeof(*no));
    if (!no) return 0;
    g_owner[kind] = no;

    g_cap[kind] = cap;
    return 1;
}

/* ============ 名字表（每种性别一个动态数组） ============ */

static const char **g_names[3];
static uint64_t g_names_n[3];
static uint64_t g_names_cap[3];

/* ============ 模组表 ============ */

typedef struct {
    HMODULE handle;
    const char *name;
    const char *version;
} ModEntry;

static ModEntry *g_mods;
static uint64_t g_mod_n;
static uint64_t g_mod_cap;

/* ============ 模组诊断 (依赖检查 / 加载失败 / 内容重名) ============ */

static ModError_t *g_errors;
static uint64_t g_error_n;
static uint64_t g_error_cap;
static const char *g_current_mod; /* 正在 mod_init 的模组名（内容重名定位用） */

static char *err_strdup(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void err_add(int warning, const char *mod, const char *file, const char *reason, const char *detail)
{
    ModError_t *e;

    if (!vec_grow((void **)&g_errors, &g_error_cap, g_error_n + 1, sizeof(*g_errors))) return;

    e = &g_errors[g_error_n++];
    e->mod = err_strdup(mod ? mod : "?");
    e->file = err_strdup(file ? file : "");
    e->reason = err_strdup(reason ? reason : "未知错误");
    e->detail = err_strdup(detail ? detail : "");
    e->warning = warning ? 1 : 0;
}

static uint64_t err_mark(void) { return g_error_n; }

static void err_rollback(uint64_t mark)
{
    uint64_t i;
    for (i = mark; i < g_error_n; i++)
    {
        free((void *)g_errors[i].mod);
        free((void *)g_errors[i].file);
        free((void *)g_errors[i].reason);
        free((void *)g_errors[i].detail);
    }
    g_error_n = mark;
}

/* 种类的中文名（报错用） */
static const char *kind_label(ContentKind_t kind)
{
    switch (kind)
    {
    case KIND_TRAIT:       return "特质";
    case KIND_ITEM:        return "物品";
    case KIND_PET_TYPE:    return "宠物类型";
    case KIND_ACHIEVEMENT: return "成就";
    case KIND_STORY:       return "剧情";
    case KIND_EVENT:       return "随机事件";
    case KIND_MINIGAME:    return "小游戏";
    case KIND_TERM:        return "术语";
    case KIND_NPC:         return "NPC";
    case KIND_MENU:        return "菜单";
    case KIND_CONFIG:      return "配置";
    default:               return "内容";
    }
}

/* ============ 运行状态 ============ */

static LoaderApi g_api;
static Pet *g_pet_ctx;
static int g_headless;

/* ============ 种类工具 ============ */

static uint32_t kind_size(ContentKind_t kind)
{
    switch (kind)
    {
    case KIND_TRAIT:       return (uint32_t)sizeof(Trait_t);
    case KIND_ITEM:        return (uint32_t)sizeof(Item);
    case KIND_PET_TYPE:    return (uint32_t)sizeof(PetType);
    case KIND_ACHIEVEMENT: return (uint32_t)sizeof(Achievement_t);
    case KIND_STORY:       return (uint32_t)sizeof(Story_t);
    case KIND_EVENT:       return (uint32_t)sizeof(Event_t);
    case KIND_MINIGAME:    return (uint32_t)sizeof(Minigame_t);
    case KIND_TERM:        return (uint32_t)sizeof(TermDef_t);
    case KIND_NPC:         return (uint32_t)sizeof(Npc_t);
    case KIND_MENU:        return (uint32_t)sizeof(Menu_t);
    case KIND_CONFIG:      return (uint32_t)sizeof(Config_t);
    default:               return 0;
    }
}

static const char *kind_name(ContentKind_t kind, const void *p)
{
    if (!p) return NULL;
    switch (kind)
    {
    case KIND_TRAIT:       return ((const Trait_t *)p)->name;
    case KIND_ITEM:        return ((const Item *)p)->name;
    case KIND_PET_TYPE:    return ((const PetType *)p)->name;
    case KIND_ACHIEVEMENT: return ((const Achievement_t *)p)->name;
    case KIND_STORY:       return ((const Story_t *)p)->name;
    case KIND_EVENT:       return ((const Event_t *)p)->name;
    case KIND_MINIGAME:    return ((const Minigame_t *)p)->name;
    case KIND_TERM:        return ((const TermDef_t *)p)->name;
    case KIND_NPC:         return ((const Npc_t *)p)->name;
    case KIND_MENU:        return ((const Menu_t *)p)->name;
    case KIND_CONFIG:      return ((const Config_t *)p)->name;
    default:               return NULL;
    }
}

/* ============ 内容注册 / 查询 ============ */

static void cfg_register(const Config_t *list, uint64_t count);
static void cfg_load(void);
static void cfg_save(void);

static const void *reg_get(ContentKind_t kind, Id id)
{
    if (kind >= KIND_COUNT || id < 1 || id > g_n[kind]) return NULL;
    return g_list[kind][id];
}

static uint64_t reg_count(ContentKind_t kind)
{
    return (kind >= KIND_COUNT) ? 0 : g_n[kind];
}

static Id reg_find(ContentKind_t kind, const char *name)
{
    uint64_t i;
    if (kind >= KIND_COUNT || !name) return ID_NONE;
    for (i = 1; i <= g_n[kind]; i++)
    {
        const char *n = kind_name(kind, g_list[kind][i]);
        if (n && strcmp(n, name) == 0) return (Id)i;
    }
    return ID_NONE;
}

/* 按 名字 + 注册者 查找：依赖里写 “模组:种类/名字” 时用 */
static Id reg_find_owned(ContentKind_t kind, const char *name, const char *owner)
{
    uint64_t i;
    if (kind >= KIND_COUNT || !name || !owner) return ID_NONE;
    for (i = 1; i <= g_n[kind]; i++)
    {
        const char *n = kind_name(kind, g_list[kind][i]);
        const char *o = g_owner[kind][i];
        if (n && o && strcmp(n, name) == 0 && _stricmp(o, owner) == 0) return (Id)i;
    }
    return ID_NONE;
}

static const char *reg_owner(ContentKind_t kind, Id id)
{
    if (kind >= KIND_COUNT || id < 1 || id > g_n[kind]) return NULL;
    return g_owner[kind][id];
}

static Id reg_add(ContentKind_t kind, const void *list, uint64_t count, uint32_t elem_size)
{
    uint64_t i;
    Id base;

    if (kind >= KIND_COUNT || !list || count == 0) return ID_NONE;
    if (elem_size != kind_size(kind)) return ID_NONE; /* ABI 不一致 */
    if (count > UINT64_MAX - g_n[kind]) return ID_NONE;

    if (!content_reserve(kind, g_n[kind] + count)) return ID_NONE;

    /* 同名内容只警告：仍然注册（Id 各自可用），但 find() 只返回先注册的那个 */
    for (i = 0; i < count; i++)
    {
        const char *fresh = kind_name(kind, (const unsigned char *)list + (size_t)i * elem_size);
        uint64_t j;
        if (!fresh) continue;
        for (j = 1; j <= g_n[kind]; j++)
        {
            const char *old = kind_name(kind, g_list[kind][j]);
            if (old && strcmp(old, fresh) == 0)
            {
                char reason[64];
                char detail[192];
                _snprintf(reason, sizeof(reason), "内容重名（%s）", kind_label(kind));
                reason[sizeof(reason) - 1] = '\0';
                _snprintf(detail, sizeof(detail),
                          "名字 \"%s\" 已被先注册的条目占用；本次注册仍会分配 Id，但 find() 只返回先注册的 Id",
                          fresh);
                detail[sizeof(detail) - 1] = '\0';
                err_add(1, g_current_mod ? g_current_mod : "?", NULL, reason, detail);
                break;
            }
        }
    }

    base = (Id)(g_n[kind] + 1);
    for (i = 0; i < count; i++)
    {
        g_list[kind][base + i] = (const unsigned char *)list + (size_t)i * elem_size;
        g_owner[kind][base + i] = g_current_mod ? g_current_mod : "宿主";
    }
    g_n[kind] += count;

    if (kind == KIND_CONFIG)
        cfg_register((const Config_t *)list, count);

    return base;
}

/* ============ 配置存储（动态数组，值按 name 持久化） ============ */

typedef struct {
    char *name;
    int32_t value;
} ConfigValue;

static ConfigValue *g_cfg;
static uint64_t g_cfg_n;
static uint64_t g_cfg_cap;
static char g_cfg_file[MAX_PATHLEN] = "config.dat";

static ConfigValue *cfg_find(const char *name)
{
    uint64_t i;
    if (!name) return NULL;
    for (i = 0; i < g_cfg_n; i++)
        if (strcmp(g_cfg[i].name, name) == 0) return &g_cfg[i];
    return NULL;
}

/* 按描述符把值夹到合法范围；BOOL 只允许 0/1，INT/SLIDER 夹到 min~max */
static int32_t cfg_normalize(const Config_t *c, int32_t v)
{
    if (!c) return v;
    if (c->type == CFG_BOOL) return v ? 1 : 0;
    if (c->max < c->min) return c->def;
    if (v < c->min) v = c->min;
    if (v > c->max) v = c->max;
    return v;
}

/*
 * 注册配置项：已有同名值时保留（并夹到范围），没有就写入默认值。
 * 不主动落盘；用户改值时才写 config.dat。
 */
static void cfg_register(const Config_t *list, uint64_t count)
{
    uint64_t i;

    for (i = 0; i < count; i++)
    {
        const Config_t *c = &list[i];
        ConfigValue *v;

        if (!c->name || !c->name[0]) continue;

        v = cfg_find(c->name);
        if (v)
        {
            v->value = cfg_normalize(c, v->value);
            continue;
        }

        if (!vec_grow((void **)&g_cfg, &g_cfg_cap, g_cfg_n + 1, sizeof(*g_cfg)))
        {
            err_add(1, g_current_mod ? g_current_mod : "?",
                    NULL, "配置注册失败", "配置数组扩容失败（内存不足）");
            continue;
        }

        v = &g_cfg[g_cfg_n];
        v->name = err_strdup(c->name);
        if (!v->name)
        {
            err_add(1, g_current_mod ? g_current_mod : "?",
                    NULL, "配置注册失败", "配置名分配失败（内存不足）");
            continue;
        }
        v->value = cfg_normalize(c, c->def);
        g_cfg_n++;
    }
}

static int32_t cfg_get(const char *name)
{
    ConfigValue *v = cfg_find(name);
    return v ? v->value : 0;
}

static int cfg_set(const char *name, int32_t value)
{
    ConfigValue *v;
    Id id;
    const Config_t *c;

    if (!name || !name[0]) return 0;

    v = cfg_find(name);
    if (!v)
    {
        if (!vec_grow((void **)&g_cfg, &g_cfg_cap, g_cfg_n + 1, sizeof(*g_cfg))) return 0;
        v = &g_cfg[g_cfg_n];
        v->name = err_strdup(name);
        if (!v->name) return 0;
        v->value = value;
        g_cfg_n++;
    }

    id = reg_find(KIND_CONFIG, name);
    c = (id != ID_NONE) ? (const Config_t *)reg_get(KIND_CONFIG, id) : NULL;
    v->value = cfg_normalize(c, value);
    cfg_save();
    return 1;
}

/* 菜单每页项数由滑动条配置决定（1~16） */
static int cfg_menu_page_size(void)
{
    int32_t v = cfg_get("ui.page_size");
    if (v < 1 || v > 16) return 4;
    return (int)v;
}

/* ============ 变长位图 ============ */

/* 保证能容纳 id 对应的位；失败返回 NULL */
static uint64_t *bs_ensure(BitSet_t *set, Id id)
{
    uint64_t need, n;
    uint64_t *p;

    if (id == ID_NONE) return NULL;
    need = id / 64u + 1u;
    if (need > (uint64_t)(SIZE_MAX / sizeof(uint64_t))) return NULL;

    if (set->count < need)
    {
        n = set->count ? set->count : 4u;
        while (n < need)
        {
            if (n > UINT64_MAX / 2) { n = need; break; }
            n *= 2u;
        }
        if (n > (uint64_t)(SIZE_MAX / sizeof(uint64_t))) return NULL;

        p = (uint64_t *)realloc(set->words, (size_t)n * sizeof(uint64_t));
        if (!p) return NULL;
        memset(p + set->count, 0, (size_t)(n - set->count) * sizeof(uint64_t));
        set->words = p;
        set->count = n;
    }
    return set->words;
}

static int bs_test(const BitSet_t *set, Id id)
{
    uint64_t w;
    if (id == ID_NONE || !set->words) return 0;
    w = id / 64u;
    if (w >= set->count) return 0;
    return ((set->words[w] >> (unsigned)(id % 64u)) & 1u) ? 1 : 0;
}

/* 返回1=本次新置位，0=已经置位/失败 */
static int bs_set(BitSet_t *set, Id id)
{
    uint64_t mask;
    if (id == ID_NONE) return 0;
    if (!bs_ensure(set, id)) return 0;
    mask = (uint64_t)1 << (unsigned)(id % 64u);
    if (set->words[id / 64u] & mask) return 0;
    set->words[id / 64u] |= mask;
    return 1;
}

static void bs_free(BitSet_t *set)
{
    free(set->words);
    set->words = NULL;
    set->count = 0;
}
/* ============ 名字库 ============ */

static uint64_t name_add(Gender_t gender, const char *const *list, uint64_t count)
{
    uint64_t i;

    if (!list || gender > G_NEUTER || count == 0) return 0;
    if (count > UINT64_MAX - g_names_n[gender]) return 0;
    if (!vec_grow((void **)&g_names[gender], &g_names_cap[gender],
                  g_names_n[gender] + count, sizeof(*g_names[gender])))
        return 0;

    for (i = 0; i < count; i++)
        g_names[gender][g_names_n[gender]++] = list[i];
    return g_names_n[gender];
}

static const char *name_get(Gender_t gender, uint64_t index)
{
    if (gender > G_NEUTER || index >= g_names_n[gender]) return NULL;
    return g_names[gender][index];
}

static uint64_t name_count(Gender_t gender)
{
    return (gender > G_NEUTER) ? 0 : g_names_n[gender];
}

/* 跨存档成就：与宠物存档分开存放（ach.dat） */
static BitSet_t g_ach_all;
static char g_ach_file[MAX_PATHLEN] = "ach.dat";
static void ach_global_save(void);
static void ach_global_load(void);
static int svc_trait_conflict(Id a, Id b);

/* ============ 界面 ============ */

static int g_has_vt;       /* 控制台支持 ANSI 颜色 */
static WORD g_console_attr; /* 非 VT 控制台的默认属性（高亮后恢复） */

/*
 * 画一行可选项：选中项用“背景色 + 反色”（黑字青底）高亮，不再用 > 箭头。
 * 无头模式/重定向输出时退化成普通文本；非 VT 控制台走 SetConsoleTextAttribute。
 */
static void print_option_line(const char *text, int selected)
{
    if (!text) text = "";

    if (g_headless)
    {
        printf("  %s\n", text);
        fflush(stdout);
        return;
    }

    if (g_has_vt)
    {
        if (selected) printf("\x1B[30;46m");
        printf("  %s  \n", text);
        if (selected) printf("\x1B[0m");
    }
    else
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (selected) SetConsoleTextAttribute(h, (WORD)(BACKGROUND_BLUE | BACKGROUND_GREEN));
        printf("  %s%s\n", text, selected ? "  " : "");
        if (selected) SetConsoleTextAttribute(h, g_console_attr);
    }
    fflush(stdout);
}

/* 给模组自定义列表用的同款高亮行 */
static void ui_option(const char *text, int selected)
{
    print_option_line(text, selected);
}

static void ui_title(const char *title)
{
    if (!g_headless) system("cls");
    printf("\n  ── %s ──\n", title ? title : "");
    fflush(stdout);
}

static void render_text(const char *text); /* 前置声明：普通输出与 dialog 共用同一套文字引擎 */

/*
 * 普通输出也走文字引擎：可以写 §S / §T / §O / §P / §0、{pet}、⌈术语⌋。
 * 没有这些标记的纯文本仍然直接 fputs，输出与以前完全一致（不额外转义、不逐字刷新）。
 */
static void ui_print(const char *fmt, ...)
{
    va_list args;
    char stack[512];
    char *buf = stack;
    int need;

    if (!fmt) return;

    va_start(args, fmt);
    need = _vscprintf(fmt, args);
    va_end(args);
    if (need < 0) return;

    if (need >= (int)sizeof(stack))
    {
        buf = (char *)malloc((size_t)need + 1);
        if (!buf) return;
    }

    va_start(args, fmt);
    vsnprintf(buf, (size_t)need + 1, fmt, args);
    va_end(args);

    if (strstr(buf, "\xC2\xA7") || strstr(buf, "{pet}") || strstr(buf, "\xE2\x8C\x88"))
        render_text(buf);
    else
        fputs(buf, stdout);
    fflush(stdout);

    if (buf != stack) free(buf);
}

/* 带颜色的三条消息；没有 VT 控制台时只输出文字 */
static void ui_ok(const char *fmt, ...)
{
    va_list args;
    if (g_has_vt) printf("\x1B[32m");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_has_vt) printf("\x1B[0m");
    printf("\n");
    fflush(stdout);
}

static void ui_err(const char *fmt, ...)
{
    va_list args;
    if (g_has_vt) printf("\x1B[31m");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_has_vt) printf("\x1B[0m");
    printf("\n");
    fflush(stdout);
}

static void ui_warn(const char *fmt, ...)
{
    va_list args;
    if (g_has_vt) printf("\x1B[33m");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    if (g_has_vt) printf("\x1B[0m");
    printf("\n");
    fflush(stdout);
}

static int ui_key(void); /* 前置声明：ui_menu 用 */

/* ============ 菜单文本（15 字节短串 / uint64 长度 + 动态数组） ============ */

static uint64_t text_bswap64(uint64_t v)
{
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) << 8)  |
           ((v & 0x000000FF00000000ull) >> 8)  |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
}

static void text_free(Text_t *t)
{
    if (!t) return;
    if (t->u.s.tag != 0x80 && t->u.b.data)
    {
        free(t->u.b.data);
    }
    memset(t, 0, sizeof(*t));
}

static const char *text_get(const Text_t *t)
{
    if (!t) return "";
    if (t->u.s.tag == 0x80) return t->u.s.str;
    if (t->u.b.data) return t->u.b.data;
    return "";
}

/* 写入文本；超过短串容量时分配动态数组。调用前 Text_t 必须是零初始化/之前用 text_set 写过。 */
static void text_set(Text_t *t, const char *s)
{
    size_t len;

    if (!t) return;
    text_free(t);
    if (!s) return;

    len = strlen(s);
    if (len < TEXT_SSO) /* 15 字节缓冲要留结尾 NUL */
    {
        t->u.s.tag = 0x80;
        memcpy(t->u.s.str, s, len + 1);
        return;
    }

    if (len > (size_t)TEXT_MAX) len = (size_t)TEXT_MAX;
    t->u.b.size_be = text_bswap64((uint64_t)len);
    t->u.b.data = (char *)malloc(len + 1);
    if (!t->u.b.data)
    {
        memset(t, 0, sizeof(*t));
        return;
    }
    memcpy(t->u.b.data, s, len);
    t->u.b.data[len] = '\0';
}

/* 菜单选项文本格式化；写进 Text_t，长文本自动动态分配 */
static void opt_text(SelectOpt_t *opt, const char *fmt, ...)
{
    va_list ap;
    int need;
    char stack[256];

    if (!opt || !fmt) return;

    va_start(ap, fmt);
    need = _vscprintf(fmt, ap);
    va_end(ap);
    if (need < 0) return;
    if ((uint64_t)need > TEXT_MAX) return;

    if (need < (int)sizeof(stack))
    {
        va_start(ap, fmt);
        vsnprintf(stack, sizeof(stack), fmt, ap);
        va_end(ap);
        text_set(&opt->text, stack);
    }
    else
    {
        char *buf = (char *)malloc((size_t)need + 1);
        if (!buf) return;
        va_start(ap, fmt);
        vsnprintf(buf, (size_t)need + 1, fmt, ap);
        va_end(ap);
        text_set(&opt->text, buf);
        free(buf);
    }
}

/* 菜单选项文本由 ui_menu 消费后释放 */
static int menu_texts_free(SelectOpt_t *opts, uint64_t count, int ret)
{
    uint64_t i;
    if (opts)
        for (i = 0; i < count; i++) text_free(&opts[i].text);
    return ret;
}

/* 选择菜单（↑↓选择，Z/R确认，X/L返回）；返回选中 value，X 返回 -1 */
static int ui_menu(const char *header, SelectOpt_t *opts, uint64_t count)
{
    uint64_t sel = 0;

    if (!opts || count == 0) return -1;
    if (g_headless) return menu_texts_free(opts, count, opts[0].value);

    while (1)
    {
        uint64_t i, start, end, pages, page;
        int per;

        per = cfg_menu_page_size();
        if (per < 1) per = 4;

        page = sel / (uint64_t)per;
        start = page * (uint64_t)per;
        end = (start + (uint64_t)per > count) ? count : start + (uint64_t)per;
        pages = (count + (uint64_t)per - 1) / (uint64_t)per;

        ui_title(header);
        printf("\n");
        for (i = start; i < end; i++)
        {
            const char *label = text_get(&opts[i].text);
            size_t need = strlen(label) + 32;
            char *line = (char *)malloc(need);
            if (!line) continue;
            _snprintf(line, need, "%llu. %s", (unsigned long long)(i + 1), label);
            line[need - 1] = '\0';
            print_option_line(line, i == sel);
            free(line);
        }
        if (pages > 1) printf("\n  %llu/%llu 页", (unsigned long long)(page + 1), (unsigned long long)pages);
        printf("\n  ↑↓:选择");
        if (pages > 1) printf("   ←→:翻页");
        printf("   Z:确认   X:返回\n");
        fflush(stdout);

        {
            int k = ui_key();
            if (k == KEY_UP) sel = (sel + count - 1) % count;
            else if (k == KEY_DOWN) sel = (sel + 1) % count;
            else if (k == KEY_LEFT)
            {
                if (page > 0) sel = (sel >= (uint64_t)per) ? sel - (uint64_t)per : 0;
            }
            else if (k == KEY_RIGHT)
            {
                if (page + 1 < pages)
                {
                    uint64_t target = sel + (uint64_t)per;
                    if (target < sel || target >= count) target = count - 1;
                    sel = target;
                }
            }
            else if (k == KEY_OK) return menu_texts_free(opts, count, opts[sel].value);
            else if (k == KEY_CANCEL) return menu_texts_free(opts, count, -1);
        }
    }
}
/* 把一次按键解码成 KEY_*：c2<0 表示普通单键；返回 0 = 未识别 */
int ml_decode_key(int c1, int c2)
{
    if (c2 >= 0) /* 方向键扩展码 */
    {
        if (c2 == 72) return KEY_UP;
        if (c2 == 80) return KEY_DOWN;
        if (c2 == 75) return KEY_LEFT;   /* ← */
        if (c2 == 77) return KEY_RIGHT;  /* → */
        return 0;
    }
    if (c1 == 'z' || c1 == 'Z' || c1 == '\r') return KEY_OK;
    if (c1 == 'x' || c1 == 'X' || c1 == 27) return KEY_CANCEL;
    if (c1 == 'c' || c1 == 'C') return KEY_TERM;
    if (c1 >= '1' && c1 <= '4') return c1 - '0'; /* 兼容旧按键 */
    if (c1 == 'a' || c1 == 'A') return KEY_UP;
    if (c1 == 'b' || c1 == 'B') return KEY_DOWN;
    return 0;
}

/* 读一个有效按键：↑ 上 / ↓ 下 / Z 确认 / X 返回 / C 查术语 */
static int ui_key(void)
{
    int c;

    if (g_headless) return KEY_OK; /* 自动确认 */

    while (1)
    {
        c = _getch();
        if (c < 0) return KEY_OK; /* 没有控制台（管道/CI）时自动确认 */
        if (c == 0 || c == 0xE0)  /* 方向键前缀 */
        {
            int k = ml_decode_key(c, _getch());
            if (k) return k;
            continue;
        }
        {
            int k = ml_decode_key(c, -1);
            if (k) return k;
        }
    }
}
static void ui_delay(uint32_t ms)
{
    if (!g_headless) Sleep(ms);
}

/* ============ 统一文字引擎 ============ */

/*
 * 控制码（UTF-8 的 § 是 C2 A7，后接一个 ASCII 字符）：
 *   §S 流式输出      §T 术语标识（绿）   §O 讲述人标识（黄）
 *   §0 清除所有格式  §P 宠物名（蓝）     §C 清屏（该页不能用 ← 回来）
 *   §F 翻页（该页可用 ← 回来）
 * 旧语法 {pet} 仍保留（蓝色），⌈术语⌋ 仍保留（绿色）。
 */

#define TEXT_STREAM_MS 30

/* ---- 当前页面的术语（动态数组） ---- */

static char **g_page_terms;
static size_t g_page_term_n;
static size_t g_page_term_cap;

static void terms_clear(void)
{
    size_t i;
    for (i = 0; i < g_page_term_n; i++) free(g_page_terms[i]);
    g_page_term_n = 0;
}

static void terms_free(void)
{
    terms_clear();
    free(g_page_terms);
    g_page_terms = NULL;
    g_page_term_cap = 0;
}

static int term_add(const char *s, size_t n)
{
    char *p;
    if (!s || n == 0 || n >= (size_t)SIZE_MAX) return 0;

    if (g_page_term_n == g_page_term_cap)
    {
        size_t cap = g_page_term_cap ? g_page_term_cap * 2 : 8;
        char **q;
        if (cap <= g_page_term_n) cap = g_page_term_n + 1;
        if (cap > (size_t)(SIZE_MAX / sizeof(*g_page_terms))) return 0;
        q = (char **)realloc(g_page_terms, cap * sizeof(*g_page_terms));
        if (!q) return 0;
        g_page_terms = q;
        g_page_term_cap = cap;
    }

    p = (char *)malloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n);
    p[n] = '\0';
    g_page_terms[g_page_term_n++] = p;
    return 1;
}

static int strbuf_add(char **buf, size_t *cap, size_t *len, const char *s, size_t n)
{
    if (n == 0) return 1;
    if (*len + n + 1 > *cap)
    {
        size_t ncap = *cap ? *cap : 64;
        char *p;
        while (ncap < *len + n + 1)
        {
            if (ncap > (size_t)(SIZE_MAX / 2)) { ncap = *len + n + 1; break; }
            ncap *= 2;
        }
        p = (char *)realloc(*buf, ncap);
        if (!p) return 0;
        *buf = p;
        *cap = ncap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

/* 取一段可见文字（去掉控制码，{pet}/§P 换成人名），作为术语名 */
static void term_add_visible(const char *s, const char *e)
{
    char *buf = NULL;
    size_t cap = 0, len = 0;
    const char *p = s;

    while (p < e)
    {
        int code = 0;
        size_t l;

        if (p + 3 <= e && p[0] == '\xC2' && p[1] == '\xA7') code = (unsigned char)p[2];
        if (code)
        {
            if (code == 'P' && g_pet_ctx && g_pet_ctx->name)
                (void)strbuf_add(&buf, &cap, &len, g_pet_ctx->name, strlen(g_pet_ctx->name));
            p += 3;
            continue;
        }
        if (p + 5 <= e && strncmp(p, "{pet}", 5) == 0)
        {
            const char *name = (g_pet_ctx && g_pet_ctx->name) ? g_pet_ctx->name : "它";
            (void)strbuf_add(&buf, &cap, &len, name, strlen(name));
            p += 5;
            continue;
        }
        if (p + 3 <= e && (unsigned char)p[0] == 0xE2 &&
            (unsigned char)p[1] == 0x8C &&
            ((unsigned char)p[2] == 0x88 || (unsigned char)p[2] == 0x8B))
        {
            p += 3; /* 旧术语括号本身不进入术语名 */
            continue;
        }
        l = 1;
        if (((unsigned char)p[0] & 0xE0) == 0xC0) l = 2;
        else if (((unsigned char)p[0] & 0xF0) == 0xE0) l = 3;
        else if (((unsigned char)p[0] & 0xF8) == 0xF0) l = 4;
        if (p + l > e) l = 1;
        (void)strbuf_add(&buf, &cap, &len, p, l);
        p += l;
    }

    if (len > 0) term_add(buf, len);
    free(buf);
}

/* 扫出当前页的术语：旧 ⌈术语⌋ + §T ... §0 */
static void collect_terms(const char *text)
{
    const char *p = text;

    terms_clear();
    while (p && *p)
    {
        if (p[0] == '\xE2' && p[1] == '\x8C' && p[2] == '\x88') /* ⌈ */
        {
            const char *close = strstr(p + 3, "\xE2\x8C\x8B");
            if (!close) { p += 3; continue; }
            term_add(p + 3, (size_t)(close - (p + 3)));
            p = close + 3;
            continue;
        }
        if (p[0] == '\xC2' && p[1] == '\xA7' && p[2] == 'T')
        {
            const char *q = p + 3;
            while (*q)
            {
                if (q[0] == '\xC2' && q[1] == '\xA7' && q[2] == '0') break;
                if (q[0] == '\xC2' && q[1] == '\xA7' && q[2]) { q += 3; continue; }
                q++;
            }
            term_add_visible(p + 3, q);
            p = q;
            continue;
        }
        p++;
    }
}

static void show_terms(void)
{
    size_t i;
    ui_title("术语");
    for (i = 0; i < g_page_term_n; i++)
    {
        Id id = reg_find(KIND_TERM, g_page_terms[i]);
        const TermDef_t *t = (const TermDef_t *)reg_get(KIND_TERM, id);
        ui_print("\n  【%s】\n", g_page_terms[i]);
        if (t && t->desc) ui_print("%s\n", t->desc);
        else ui_print("  (模组没有提供解释)\n");
    }
    ui_print("\n  X:返回\n");
    ui_key();
}

/* ---- 格式化输出 ---- */

static size_t text_utf8_len(const char *p)
{
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int text_ctrl(const char *p)
{
    if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xA7 && p[2])
        return (unsigned char)p[2];
    return 0;
}

static void text_color(int *cur, int want)
{
    if (!g_has_vt || *cur == want) return;
    if (want == 1) printf("\x1B[32m");
    else if (want == 2) printf("\x1B[33m");
    else if (want == 3) printf("\x1B[34m");
    else printf("\x1B[0m");
    *cur = want;
}

static void text_emit(const char *s, const char *e, int *color, int want, int stream)
{
    while (s < e)
    {
        size_t l = text_utf8_len(s);
        if (s + l > e) l = 1;
        text_color(color, want);
        fwrite(s, 1, l, stdout);
        if (stream)
        {
            fflush(stdout);
            ui_delay(TEXT_STREAM_MS);
        }
        s += l;
    }
}

/* 输出一页：流式/术语/讲述人/宠物名/旧语法 */
static void render_text(const char *text)
{
    const char *p = text;
    int color = 0, stream = 0, term = 0, narrator = 0;

    if (!text) return;
    while (*p)
    {
        int code = text_ctrl(p);
        if (code)
        {
            if (code == 'S') stream = 1;
            else if (code == 'T') { term = 1; narrator = 0; }
            else if (code == 'O') { narrator = 1; term = 0; }
            else if (code == '0') { stream = 0; term = 0; narrator = 0; text_color(&color, 0); }
            else if (code == 'P')
            {
                const char *name = (g_pet_ctx && g_pet_ctx->name) ? g_pet_ctx->name : "它";
                text_emit(name, name + strlen(name), &color, 3, stream);
            }
            p += 3;
            continue;
        }

        if (p[0] == '\xE2' && p[1] == '\x8C' && p[2] == '\x88') /* ⌈ */
        {
            const char *close = strstr(p + 3, "\xE2\x8C\x8B");
            if (!close) { p += 3; continue; }
            text_emit(p + 3, close, &color, 1, stream);
            p = close + 3;
            continue;
        }
        if (p[0] == '\xE2' && p[1] == '\x8C' && p[2] == '\x8B') /* ⌋ */
        {
            p += 3;
            continue;
        }
        if (strncmp(p, "{pet}", 5) == 0)
        {
            const char *name = (g_pet_ctx && g_pet_ctx->name) ? g_pet_ctx->name : "它";
            text_emit(name, name + strlen(name), &color, 3, stream);
            p += 5;
            continue;
        }

        {
            size_t l = text_utf8_len(p);
            int want = term ? 1 : (narrator ? 2 : 0);
            text_emit(p, p + l, &color, want, stream);
            p += l;
        }
    }
    text_color(&color, 0);
    fflush(stdout);
}

/* ---- 页面模型：§C/§F 切页 ---- */

typedef struct {
    char *text;
    int returnable;
    int visited;
    int auto_enter; /* §F/§C 切出来的后续页：进入时不需要按键 */
} TextPage;

typedef struct {
    TextPage *items;
    uint64_t count;
    uint64_t cap;
} TextPages;

static void text_pages_free(TextPages *ps)
{
    uint64_t i;
    for (i = 0; i < ps->count; i++) free(ps->items[i].text);
    free(ps->items);
    ps->items = NULL;
    ps->count = ps->cap = 0;
}

static int text_page_add(TextPages *ps, const char *s, size_t len, int returnable, int auto_enter)
{
    char *p = (char *)malloc(len + 1);
    if (!p) return 0;
    memcpy(p, s, len);
    p[len] = '\0';
    if (!vec_grow((void **)&ps->items, &ps->cap, ps->count + 1, sizeof(*ps->items)))
    {
        free(p);
        return 0;
    }
    ps->items[ps->count].text = p;
    ps->items[ps->count].returnable = returnable;
    ps->items[ps->count].visited = 0;
    ps->items[ps->count].auto_enter = auto_enter;
    ps->count++;
    return 1;
}

/* 把 pages 数组拆成文本页；§C 标当前页不可回退，§F 标可回退 */
static int text_pages_build(const char *const *pages, uint32_t n_pages, TextPages *ps)
{
    uint32_t i;
    memset(ps, 0, sizeof(*ps));

    {
        int pending_auto = 0; /* 上一个数组页以 §C/§F 结尾：下一页直接进入 */

        for (i = 0; i < n_pages; i++)
        {
            const char *page = pages[i];
            const char *start, *p;
            int seg = 0;
            int first_auto = pending_auto;

            pending_auto = 0;
            if (!page) continue;
            start = page;
            p = page;
            while (*p)
            {
                int code = text_ctrl(p);
                if (code == 'C' || code == 'F')
                {
                    int ret = (code == 'F');
                    if (p > start)
                    {
                        if (!text_page_add(ps, start, (size_t)(p - start), ret,
                                           seg > 0 || first_auto)) goto fail;
                        seg++;
                    }
                    else if (ps->count > 0)
                    {
                        ps->items[ps->count - 1].returnable = ret;
                        if (seg == 0) first_auto = 1; /* 页首/空段标记：本页直接进入 */
                    }
                    p += 3;
                    start = p;
                    if (*p == '\0') pending_auto = 1; /* 页尾标记：下一个数组页直接进入 */
                }
                else
                {
                    p += text_utf8_len(p);
                }
            }
            if (p > start)
            {
                if (!text_page_add(ps, start, (size_t)(p - start), 1,
                                   seg > 0 || first_auto)) goto fail;
            }
            else if (ps->count == 0)
            {
                if (!text_page_add(ps, "", 0, 1, first_auto)) goto fail;
            }
        }
    }

    if (ps->count == 0)
    {
        if (!text_page_add(ps, "", 0, 1, 0)) goto fail;
    }
    return 1;

fail:
    text_pages_free(ps);
    return 0;
}

static uint64_t text_pages_prev(const TextPages *ps, uint64_t cur)
{
    uint64_t j = cur;
    while (j > 0)
    {
        j--;
        if (ps->items[j].returnable) return j;
    }
    return UINT64_MAX;
}

static uint64_t text_pages_next(const TextPages *ps, uint64_t cur)
{
    uint64_t j = cur + 1;
    while (j < ps->count)
    {
        if (ps->items[j].returnable || !ps->items[j].visited) return j;
        j++;
    }
    return UINT64_MAX;
}

static int ui_dialog(const char *speaker, const char *const *pages, uint32_t n_pages,
                     const StoryOpt_t *opts, uint32_t n_opts)
{
    if (pages && n_pages > 0)
    {
        TextPages ps;

        if (text_pages_build(pages, n_pages, &ps))
        {
            uint64_t cur = 0;
            int auto_run = 1; /* 初始/§F/§C 自动前进；用户按键后停下 */

            while (1)
            {
                int k;
                uint64_t prev = text_pages_prev(&ps, cur);
                uint64_t next = text_pages_next(&ps, cur);

                ps.items[cur].visited = 1;
                ui_title(speaker ? speaker : "");
                printf("\n");
                render_text(ps.items[cur].text);
                collect_terms(ps.items[cur].text);
                printf("\n\n");

                /* §F/§C 切出来的后续页直接显示，不等待按键 */
                if (auto_run && next != UINT64_MAX && ps.items[next].auto_enter)
                {
                    cur = next;
                    continue;
                }

                if (prev != UINT64_MAX) printf("  ←:上页");
                if (next != UINT64_MAX) printf("  →:下页");
                else if (n_opts > 0) printf("  Z:选项");
                else printf("  Z:继续");
                if (g_page_term_n > 0) printf("  C:术语");
                printf("  X:跳过\n");
                fflush(stdout);

                k = ui_key();
                if (k == KEY_LEFT && prev != UINT64_MAX) { cur = prev; auto_run = 0; continue; }
                if (k == KEY_RIGHT && next != UINT64_MAX) { cur = next; auto_run = 1; continue; }
                if (k == KEY_TERM && g_page_term_n > 0) { show_terms(); continue; }
                if (k == KEY_OK && next != UINT64_MAX) { cur = next; auto_run = 1; continue; }
                if (k == KEY_OK || k == KEY_CANCEL) break;
            }
            text_pages_free(&ps);
        }
    }

    if (!opts || n_opts == 0) return 0;
    if (g_headless) return opts[0].value; /* 测试时自动选第一项 */

    {
        int sel = 0;
        while (1)
        {
            int i;
            ui_title(speaker ? speaker : "选择");
            printf("\n");
            for (i = 0; i < (int)n_opts && i < 4; i++)
                print_option_line(opts[i].text, i == sel);
            printf("\n  ↑↓:选择   Z:确认   X:取消\n");
            fflush(stdout);
            {
                int k = ui_key();
                if (k == KEY_UP) sel = (sel - 1 + (int)n_opts) % (int)n_opts;
                else if (k == KEY_DOWN) sel = (sel + 1) % (int)n_opts;
                else if (k == KEY_OK) return opts[sel].value;
                else if (k == KEY_CANCEL) return 0;
            }
        }
    }
}
/* ============ 菜单调度 ============ */

typedef struct {
    Id id;
    uint32_t order;
} MenuRef;

static int menu_ref_cmp(const void *a, const void *b)
{
    const MenuRef *x = (const MenuRef *)a;
    const MenuRef *y = (const MenuRef *)b;
    if (x->order < y->order) return -1;
    if (x->order > y->order) return 1;
    if (x->id < y->id) return -1;
    if (x->id > y->id) return 1;
    return 0;
}

/* 收集 parent 下的菜单项，按 order 排序（order 相同按 Id） */
static MenuRef *menu_collect(const char *parent, uint64_t *out_n)
{
    uint64_t i, total = reg_count(KIND_MENU), n = 0;
    MenuRef *refs;

    *out_n = 0;
    for (i = 1; i <= total; i++)
    {
        const Menu_t *m = (const Menu_t *)reg_get(KIND_MENU, (Id)i);
        if (!m) continue;
        if (parent)
        {
            if (!m->parent || strcmp(m->parent, parent) != 0) continue;
        }
        else if (m->parent) continue;
        n++;
    }
    if (n == 0) return NULL;

    refs = (MenuRef *)malloc((size_t)n * sizeof(*refs));
    if (!refs) return NULL;

    n = 0;
    for (i = 1; i <= total; i++)
    {
        const Menu_t *m = (const Menu_t *)reg_get(KIND_MENU, (Id)i);
        if (!m) continue;
        if (parent)
        {
            if (!m->parent || strcmp(m->parent, parent) != 0) continue;
        }
        else if (m->parent) continue;
        refs[n].id = (Id)i;
        refs[n].order = m->order;
        n++;
    }

    qsort(refs, (size_t)n, sizeof(*refs), menu_ref_cmp);
    *out_n = n;
    return refs;
}

static int svc_menu_run(Pet *pet, const char *parent)
{
    MenuRef *refs;
    SelectOpt_t *opts;
    uint64_t n = 0, i, back = 0, total;
    int sel;

    refs = menu_collect(parent, &n);
    if (n == 0)
    {
        free(refs);
        ui_print("\n  （这个菜单是空的）\n");
        if (!g_headless) { ui_print("\nZ:继续\n"); ui_key(); }
        return 0; /* 空菜单没有可选项，直接退回上一级 */
    }

    total = n + (parent ? 1u : 0u);
    opts = (SelectOpt_t *)calloc((size_t)total, sizeof(*opts));
    if (!opts)
    {
        free(refs);
        ui_print("\n  （菜单分配失败）\n");
        return 0;
    }

    for (i = 0; i < n; i++)
    {
        const Menu_t *m = (const Menu_t *)reg_get(KIND_MENU, refs[i].id);
        const char *label = (m && m->label) ? m->label : ((m && m->name) ? m->name : "?");
        opt_text(&opts[i], "%s", label);
        opts[i].value = (int)i + 1;
    }
    if (parent)
    {
        opt_text(&opts[n], "返回");
        opts[n].value = 0;
        back = 1;
    }

    sel = ui_menu(parent ? parent : "主菜单", opts, n + back);
    free(opts); /* 文本已被 ui_menu 释放 */
    if (sel <= 0 || (uint64_t)sel > n)
    {
        free(refs);
        return 0; /* 返回 / 取消 */
    }

    {
        Id picked = refs[sel - 1].id;
        const Menu_t *m;
        int r;

        free(refs);
        m = (const Menu_t *)reg_get(KIND_MENU, picked);
        if (!m) return 1;
        if (m->flags & MENU_FLAG_EXIT) return -1;
        if (m->run)
        {
            r = m->run(pet);
            if (r < 0) return -1;
            if (r == 0) return 0;
        }
        else
        {
            /* 纯容器：进入子菜单，直到玩家按返回 */
            do { r = svc_menu_run(pet, m->name); } while (r > 0);
            if (r < 0) return -1;
        }
    }

    /* 无头自检：子菜单执行一项就退回，避免容器循环一直转 */
    if (g_headless && parent) return 0;
    return 1;
}

int modloader_menu_run(Pet *pet, const char *parent)
{
    return svc_menu_run(pet, parent);
}

/* ============ 实时输入 / 时钟 ============ */

static uint32_t g_boot_ms;

/* 单调毫秒时钟；无头模式用虚拟时间快进（每查一次 +16ms），自检不真的等 5 秒 */
static uint32_t svc_now_ms(void)
{
    static uint32_t vt;
    uint32_t now;

    if (g_headless) { vt += 16u; return vt; }

    now = (uint32_t)GetTickCount();
    if (g_boot_ms == 0) g_boot_ms = now ? now : 1u;
    return now - g_boot_ms;
}

/* 非阻塞读键：有键立即返回 KEY_*，无键返回 0 */
static int svc_key_poll(void)
{
    int c;

    if (g_headless) return KEY_OK; /* 自检：模拟一直按确认键 */
    if (!_kbhit()) return 0;

    c = _getch();
    if (c < 0) return 0;
    if (c == 0 || c == 0xE0) return ml_decode_key(c, _getch());
    return ml_decode_key(c, -1);
}

/* 从背包里挑一个指定类别的物品（菜单模组的通用小工具） */
static Id svc_inv_pick(Pet *pet, ItemType_t type, const char *title)
{
    SelectOpt_t *opts;
    Id *ids;
    uint64_t cnt = 0, i;
    int sel;

    if (!pet) return ID_NONE;

    for (i = 0; i < pet->inv_cnt; i++)
    {
        const Item *it;
        if (pet->inv[i].id == ID_NONE || pet->inv[i].count == 0) continue;
        it = (const Item *)reg_get(KIND_ITEM, pet->inv[i].id);
        if (it && it->type == type) cnt++;
    }

    if (cnt == 0)
    {
        ui_print("\n没有可用的物品\n");
        if (!g_headless) { ui_print("\nZ:继续\n"); ui_key(); }
        return ID_NONE;
    }

    opts = (SelectOpt_t *)calloc((size_t)cnt, sizeof(*opts));
    ids = (Id *)malloc((size_t)cnt * sizeof(*ids));
    if (!opts || !ids)
    {
        free(opts);
        free(ids);
        ui_print("\n物品列表分配失败\n");
        return ID_NONE;
    }

    cnt = 0;
    for (i = 0; i < pet->inv_cnt; i++)
    {
        const Item *it;
        if (pet->inv[i].id == ID_NONE || pet->inv[i].count == 0) continue;
        it = (const Item *)reg_get(KIND_ITEM, pet->inv[i].id);
        if (!it || it->type != type) continue;
        opt_text(&opts[cnt], "%s x%llu", it->name, (unsigned long long)pet->inv[i].count);
        opts[cnt].value = (int)cnt;
        ids[cnt] = pet->inv[i].id;
        cnt++;
    }

    sel = ui_menu(title, opts, cnt);
    {
        Id picked = (sel < 0 || (uint64_t)sel >= cnt) ? ID_NONE : ids[sel];
        free(opts); /* 文本已被 ui_menu 释放 */
        free(ids);
        return picked;
    }
}

/* ============ 数值 / 背包 / 被动 ============ */

static uint8_t stat_of(int v)
{
    if (v < MIN_STAT) return (uint8_t)MIN_STAT;
    if (v > MAX_STAT) return (uint8_t)MAX_STAT;
    return (uint8_t)v;
}

static uint32_t svc_rnd(uint32_t lo, uint32_t hi)
{
    if (hi <= lo) return lo;
    return lo + (uint32_t)(rand() % (int)(hi - lo + 1));
}

/* 表长度可能是 uint64，索引随机数需要 64 位版本 */
static uint64_t svc_rnd64(uint64_t lo, uint64_t hi)
{
    uint64_t span, r;

    if (hi <= lo) return lo;
    span = hi - lo + 1;
    if (span == 0) return lo; /* 整个 uint64 区间，交给下面的 r */

    r = ((uint64_t)(rand() & 0x7fff))
      | ((uint64_t)(rand() & 0x7fff) << 15)
      | ((uint64_t)(rand() & 0x7fff) << 30)
      | ((uint64_t)(rand() & 0x7fff) << 45);
    return lo + (r % span);
}

/* 随机名字：最多试 10 次避开 avoid */
static const char *svc_rand_name(Gender_t gender, const char *avoid)
{
    uint64_t n = name_count(gender);
    uint32_t i;
    const char *pick = NULL;

    if (n == 0) return NULL;
    for (i = 0; i < 10; i++)
    {
        const char *cand = name_get(gender, svc_rnd64(0, n - 1));
        if (!cand) continue;
        pick = cand;
        if (!avoid || strcmp(cand, avoid) != 0) return cand;
    }
    return pick;
}

/* 特质数组按需扩容（上限 uint64_t / 内存） */
static int pet_traits_reserve(Pet *pet, uint64_t need)
{
    return vec_grow((void **)&pet->traits, &pet->trait_cap, need, sizeof(*pet->traits));
}

/* 背包数组按需扩容 */
static int pet_inv_reserve(Pet *pet, uint64_t need)
{
    return vec_grow((void **)&pet->inv, &pet->inv_cap, need, sizeof(*pet->inv));
}

/* 摇 1 正 1 负，且两者不互斥 */
static int svc_gen_traits(Pet *pet)
{
    uint64_t n = reg_count(KIND_TRAIT);
    uint32_t i;
    Id pos = ID_NONE, neg = ID_NONE;

    if (!pet || n == 0) return 0;
    if (!pet_traits_reserve(pet, MAX_TRAITS)) return 0;

    for (i = 0; i < 64 && pos == ID_NONE; i++)
    {
        Id id = (Id)svc_rnd64(1, n);
        const Trait_t *t = (const Trait_t *)reg_get(KIND_TRAIT, id);
        if (t && t->type == T_POS) pos = id;
    }
    for (i = 0; i < 64 && neg == ID_NONE; i++)
    {
        Id id = (Id)svc_rnd64(1, n);
        const Trait_t *t = (const Trait_t *)reg_get(KIND_TRAIT, id);
        if (!t || t->type != T_NEG) continue;
        if (pos != ID_NONE && svc_trait_conflict(pos, id)) continue;
        neg = id;
    }

    pet->traits[0] = pos;
    pet->traits[1] = neg;
    pet->trait_cnt = (uint64_t)((pos != ID_NONE ? 1 : 0) + (neg != ID_NONE ? 1 : 0));
    return (int)pet->trait_cnt;
}

/* ============ 宠物定制（重生 / 祭坛共用） ============ */

/* 名字库手选：列出该性别的全部名字；取消返回 NULL */
static const char *svc_name_pick(Gender_t gender, const char *title)
{
    uint64_t n = name_count(gender), i;
    SelectOpt_t *opts;
    const char *out = NULL;
    int sel;

    if (n == 0 || n > (uint64_t)(SIZE_MAX / sizeof(*opts))) return NULL;
    opts = (SelectOpt_t *)calloc((size_t)(n + 1), sizeof(*opts));
    if (!opts) return NULL;

    for (i = 0; i < n; i++)
    {
        const char *nm = name_get(gender, i);
        opt_text(&opts[i], "%s", nm ? nm : "?");
        opts[i].value = (int)i + 1;
    }
    opt_text(&opts[n], "返回");
    opts[n].value = 0;

    sel = ui_menu(title ? title : "选择名字", opts, n + 1);
    if (sel >= 1 && (uint64_t)sel <= n) out = name_get(gender, (uint64_t)sel - 1);
    free(opts); /* 选项文本已被 ui_menu 释放 */
    return out;
}

/* 是否已经拥有该特质 */
static int pet_trait_has(const Pet *pet, Id id)
{
    uint64_t i;

    if (!pet || id == ID_NONE) return 0;
    for (i = 0; i < pet->trait_cnt; i++)
        if (pet->traits[i] == id) return 1;
    return 0;
}

/* 和已有特质是否互斥 */
static int pet_trait_clash(const Pet *pet, Id id)
{
    uint64_t i;

    if (!pet || id == ID_NONE) return 0;
    for (i = 0; i < pet->trait_cnt; i++)
        if (svc_trait_conflict(pet->traits[i], id)) return 1;
    return 0;
}

/* 还能不能加：注册过、有名字（能存档）、没拥有、不与已有互斥 */
static int pet_trait_addable(const Pet *pet, Id id)
{
    const Trait_t *t = (const Trait_t *)reg_get(KIND_TRAIT, id);

    if (!t || !t->name) return 0;
    return !pet_trait_has(pet, id) && !pet_trait_clash(pet, id);
}

/* 追加一个特质（容量按需扩容） */
static int pet_trait_append(Pet *pet, Id id)
{
    if (!pet || id == ID_NONE) return 0;
    if (!pet_traits_reserve(pet, pet->trait_cnt + 1)) return 0;
    pet->traits[pet->trait_cnt++] = id;
    return 1;
}

/* 删除一个特质：后面的元素前移，数组保持紧凑 */
static int pet_trait_drop(Pet *pet, Id id)
{
    uint64_t i, j;

    if (!pet || id == ID_NONE) return 0;
    for (i = 0; i < pet->trait_cnt; i++)
    {
        if (pet->traits[i] != id) continue;
        for (j = i; j + 1 < pet->trait_cnt; j++) pet->traits[j] = pet->traits[j + 1];
        pet->trait_cnt--;
        return 1;
    }
    return 0;
}

/* 随机加一个特质：先数候选，再抽第 k 个，不建临时数组 */
static Id svc_trait_add_random(Pet *pet)
{
    uint64_t total = reg_count(KIND_TRAIT), n = 0, i, k;
    Id pick = ID_NONE;

    if (!pet) return ID_NONE;

    for (i = 1; i <= total; i++)
        if (pet_trait_addable(pet, (Id)i)) n++;
    if (n == 0) return ID_NONE;

    k = svc_rnd64(0, n - 1);
    for (i = 1; i <= total; i++)
    {
        if (!pet_trait_addable(pet, (Id)i)) continue;
        if (k == 0) { pick = (Id)i; break; }
        k--;
    }
    if (pick == ID_NONE || !pet_trait_append(pet, pick)) return ID_NONE;
    return pick;
}

/* 手选一个还能加的特质；取消返回 ID_NONE */
static Id svc_trait_pick(const Pet *pet)
{
    uint64_t total = reg_count(KIND_TRAIT), n = 0, i;
    SelectOpt_t *opts;
    Id *ids;
    Id out = ID_NONE;
    int sel;

    if (total == 0) { ui_warn("  这个版本没有注册任何特质"); return ID_NONE; }
    if (total > (uint64_t)(SIZE_MAX / sizeof(*opts))) return ID_NONE;

    ids = (Id *)calloc((size_t)total, sizeof(*ids));
    opts = (SelectOpt_t *)calloc((size_t)(total + 1), sizeof(*opts));
    if (!ids || !opts)
    {
        free(ids);
        free(opts);
        ui_err("  内存不足，打不开特质列表");
        return ID_NONE;
    }

    for (i = 1; i <= total; i++)
    {
        const Trait_t *t;

        if (!pet_trait_addable(pet, (Id)i)) continue;
        t = (const Trait_t *)reg_get(KIND_TRAIT, (Id)i);
        opt_text(&opts[n], "%s（%s）", t->name,
                 t->type == T_POS ? "正面" : (t->type == T_NEG ? "负面" : "中性"));
        opts[n].value = (int)n + 1;
        ids[n] = (Id)i;
        n++;
    }
    if (n == 0)
    {
        free(ids);
        free(opts);
        ui_warn("  没有可以再加的特质了（都和已有的重复或互斥）");
        return ID_NONE;
    }

    opt_text(&opts[n], "返回");
    opts[n].value = 0;

    sel = ui_menu("选择要添加的特质", opts, n + 1);
    if (sel >= 1 && (uint64_t)sel <= n) out = ids[(uint64_t)sel - 1];
    free(ids);
    free(opts); /* 选项文本已被 ui_menu 释放 */
    return out;
}

/* 手选一个已拥有的特质删除；取消返回 ID_NONE */
static Id svc_trait_pick_drop(const Pet *pet)
{
    SelectOpt_t *opts;
    Id out = ID_NONE;
    uint64_t i;
    int sel;

    if (!pet || pet->trait_cnt == 0)
    {
        ui_warn("  它现在没有任何特质");
        return ID_NONE;
    }
    if (pet->trait_cnt > (uint64_t)(SIZE_MAX / sizeof(*opts))) return ID_NONE;

    opts = (SelectOpt_t *)calloc((size_t)(pet->trait_cnt + 1), sizeof(*opts));
    if (!opts) return ID_NONE;

    for (i = 0; i < pet->trait_cnt; i++)
    {
        const Trait_t *t = (const Trait_t *)reg_get(KIND_TRAIT, pet->traits[i]);
        opt_text(&opts[i], "%s", (t && t->name) ? t->name : "?");
        opts[i].value = (int)i + 1;
    }
    opt_text(&opts[pet->trait_cnt], "返回");
    opts[pet->trait_cnt].value = 0;

    sel = ui_menu("选择要删除的特质", opts, pet->trait_cnt + 1);
    if (sel >= 1 && (uint64_t)sel <= pet->trait_cnt) out = pet->traits[(uint64_t)sel - 1];
    free(opts); /* 选项文本已被 ui_menu 释放 */
    return out;
}

/* 菜单标题里带上当前特质，太长就截断 */
static void trait_head(const Pet *pet, const char *title, char *out, size_t cap)
{
    size_t used;
    uint64_t i;

    if (!out || cap == 0) return;
    _snprintf(out, cap, "%s  鲁尼:%u  特质:", title ? title : "特质", pet ? pet->coins : 0u);
    out[cap - 1] = '\0';
    used = strlen(out);

    if (!pet || pet->trait_cnt == 0)
    {
        if (used + 4 < cap) strcat(out, " 无");
        return;
    }
    for (i = 0; i < pet->trait_cnt; i++)
    {
        const Trait_t *t = (const Trait_t *)reg_get(KIND_TRAIT, pet->traits[i]);
        const char *nm = (t && t->name) ? t->name : "?";
        int w = _snprintf(out + used, cap - used, " %s", nm);

        if (w < 0 || (size_t)w >= cap - used) { out[used] = '\0'; break; }
        used += (size_t)w;
    }
}

/*
 * 特质商店：随机加 / 手选加 /（可选）删除。
 * 价钱由调用方给（内容层定价），这里只负责扣钱、查重、查互斥、维护动态数组。
 */
static int svc_trait_shop(Pet *pet, const char *title, int allow_delete,
                          uint32_t cost_random, uint32_t cost_pick, uint32_t cost_delete)
{
    uint32_t guard;

    if (!pet) return 0;
    if (g_headless) return 1; /* 自检不花钱：免得自动点菜单时反复购买转不出去 */

    /* 200 次只是保险：没有控制台时 _getch 一直返回“确认”，别在这儿转死 */
    for (guard = 0; guard < 200; guard++)
    {
        SelectOpt_t opts[4];
        char head[256];
        int n = 0, sel;

        trait_head(pet, title, head, sizeof(head));
        memset(opts, 0, sizeof(opts));

        opt_text(&opts[n], "随机特质（%u 鲁尼）", cost_random);
        opts[n].value = 1;
        n++;
        opt_text(&opts[n], "选取特质（%u 鲁尼）", cost_pick);
        opts[n].value = 2;
        n++;
        if (allow_delete)
        {
            opt_text(&opts[n], "删除特质（%u 鲁尼）", cost_delete);
            opts[n].value = 3;
            n++;
        }
        opt_text(&opts[n], "完成");
        opts[n].value = 0;
        n++;

        sel = ui_menu(head, opts, (uint64_t)n);
        if (sel <= 0) return 1;

        if (sel == 1)
        {
            const Trait_t *t;
            Id got;

            if (pet->coins < cost_random)
            {
                ui_warn("  鲁尼不够：随机特质需要 %u 鲁尼", cost_random);
                ui_key();
                continue;
            }
            got = svc_trait_add_random(pet);
            if (got == ID_NONE)
            {
                ui_warn("  没有可以再加的特质了");
                ui_key();
                continue;
            }
            pet->coins -= cost_random;
            if (cost_random > pet->big_spend) pet->big_spend = cost_random;
            t = (const Trait_t *)reg_get(KIND_TRAIT, got);
            ui_ok("  获得特质「%s」，剩余 %u 鲁尼", (t && t->name) ? t->name : "?", pet->coins);
            ui_key();
        }
        else if (sel == 2)
        {
            const Trait_t *t;
            Id want = svc_trait_pick(pet);

            if (want == ID_NONE) continue;
            if (pet->coins < cost_pick)
            {
                ui_warn("  鲁尼不够：选取特质需要 %u 鲁尼", cost_pick);
                ui_key();
                continue;
            }
            if (!pet_trait_append(pet, want))
            {
                ui_err("  特质添加失败（内存不足？）");
                ui_key();
                continue;
            }
            pet->coins -= cost_pick;
            if (cost_pick > pet->big_spend) pet->big_spend = cost_pick;
            t = (const Trait_t *)reg_get(KIND_TRAIT, want);
            ui_ok("  获得特质「%s」，剩余 %u 鲁尼", (t && t->name) ? t->name : "?", pet->coins);
            ui_key();
        }
        else
        {
            const Trait_t *t;
            Id gone = svc_trait_pick_drop(pet);

            if (gone == ID_NONE) continue;
            if (pet->coins < cost_delete)
            {
                ui_warn("  鲁尼不够：删除特质需要 %u 鲁尼", cost_delete);
                ui_key();
                continue;
            }
            t = (const Trait_t *)reg_get(KIND_TRAIT, gone);
            if (!pet_trait_drop(pet, gone))
            {
                ui_err("  特质删除失败");
                ui_key();
                continue;
            }
            pet->coins -= cost_delete;
            if (cost_delete > pet->big_spend) pet->big_spend = cost_delete;
            ui_ok("  删掉了特质「%s」，剩余 %u 鲁尼", (t && t->name) ? t->name : "?", pet->coins);
            ui_key();
        }
    }
    return 1;
}

static int svc_inv_add(Pet *pet, Id item, uint64_t count)
{
    uint64_t i;
    if (!pet || item == ID_NONE || count == 0) return 0;

    for (i = 0; i < pet->inv_cnt; i++)
    {
        if (pet->inv[i].id == item)
        {
            if (UINT64_MAX - pet->inv[i].count < count) pet->inv[i].count = UINT64_MAX;
            else pet->inv[i].count += count;
            return 1;
        }
    }

    if (!pet_inv_reserve(pet, pet->inv_cnt + 1)) return 0;
    pet->inv[pet->inv_cnt].id = item;
    pet->inv[pet->inv_cnt].count = count;
    pet->inv_cnt++;
    return 1;
}

static uint64_t svc_inv_count(const Pet *pet, Id item)
{
    uint64_t total = 0, i;
    if (!pet || item == ID_NONE) return 0;
    for (i = 0; i < pet->inv_cnt; i++)
        if (pet->inv[i].id == item)
        {
            if (UINT64_MAX - total < pet->inv[i].count) return UINT64_MAX;
            total += pet->inv[i].count;
        }
    return total;
}

/* 扣物品：数量不够返回 0 */
static int svc_inv_rm(Pet *pet, Id item, uint64_t count)
{
    uint64_t i;
    if (!pet || item == ID_NONE || count == 0) return 0;
    for (i = 0; i < pet->inv_cnt; i++)
    {
        if (pet->inv[i].id != item) continue;
        if (pet->inv[i].count < count) return 0;
        pet->inv[i].count -= count;
        if (pet->inv[i].count == 0)
        {
            pet->inv[i] = pet->inv[pet->inv_cnt - 1];
            pet->inv_cnt--;
        }
        return 1;
    }
    return 0;
}

/* 直接设为指定数量（0 = 清空该物品） */
static int svc_inv_set(Pet *pet, Id item, uint64_t count)
{
    uint64_t i;
    if (!pet || item == ID_NONE) return 0;

    for (i = 0; i < pet->inv_cnt; i++)
    {
        if (pet->inv[i].id != item) continue;
        if (count == 0)
        {
            pet->inv[i] = pet->inv[pet->inv_cnt - 1];
            pet->inv_cnt--;
        }
        else pet->inv[i].count = count;
        return 1;
    }

    if (count == 0) return 1;
    if (!pet_inv_reserve(pet, pet->inv_cnt + 1)) return 0;
    pet->inv[pet->inv_cnt].id = item;
    pet->inv[pet->inv_cnt].count = count;
    pet->inv_cnt++;
    return 1;
}
static int svc_passive_try(Pet *pet)
{
    const PetType *type;
    if (!pet) return PASSIVE_FAIL;
    type = (const PetType *)reg_get(KIND_PET_TYPE, pet->type_id);
    if (!type || !type->passive.on_trigger) return PASSIVE_FAIL;
    if (svc_rnd(0, 99) >= type->passive.chance) return PASSIVE_FAIL;
    return type->passive.on_trigger(pet);
}

static int svc_damage(Pet *pet, uint32_t amount)
{
    if (!pet) return 0;
    if (svc_passive_try(pet) == PASSIVE_GUARD) return 0;
    pet->health = stat_of((int)pet->health - (int)amount);
    return 1;
}

/* ============ 成就 ============ */

static int svc_ach_unlock(Pet *pet, Id ach)
{
    const Achievement_t *a;
    uint32_t i;

    if (!pet || ach < 1) return 0;
    a = (const Achievement_t *)reg_get(KIND_ACHIEVEMENT, ach);
    if (!a) return 0;
    if (!bs_set(&pet->ach, ach)) return 0;
    if (bs_set(&g_ach_all, ach)) ach_global_save(); /* 同步到跨存档成就 */

    for (i = 0; i < a->reward_count; i++)
    {
        Id item = reg_find(KIND_ITEM, a->rewards[i].item);
        if (item != ID_NONE) svc_inv_add(pet, item, a->rewards[i].count);
    }

    g_pet_ctx = pet;
    ui_title("成就解锁！");
    ui_print("\n  ★ %s ★\n  %s\n", a->name, a->desc);
    if (!g_headless) { ui_print("\nZ:继续\n"); ui_key(); }
    return 1;
}

static int svc_ach_unlocked(const Pet *pet, Id ach) { return pet && bs_test(&pet->ach, ach); }
static int svc_ach_claimed(const Pet *pet, Id ach) { return pet && bs_test(&pet->ach_claim, ach); }

/* 标记领奖（奖励在解锁时已发） */
static int svc_ach_claim(Pet *pet, Id ach)
{
    if (!pet || ach < 1) return 0;
    if (!bs_test(&pet->ach, ach)) return 0;
    return bs_set(&pet->ach_claim, ach);
}

/* 把跨存档成就并进本档（新宠物开始时用） */
static void svc_ach_merge_global(Pet *pet)
{
    uint64_t i, n = reg_count(KIND_ACHIEVEMENT);
    if (!pet) return;
    for (i = 1; i <= n; i++)
        if (bs_test(&g_ach_all, (Id)i)) bs_set(&pet->ach, (Id)i);
}

static int svc_story_seen(const Pet *pet, Id story) { return pet && bs_test(&pet->story_flag, story); }
static int svc_story_mark(Pet *pet, Id story) { return pet && bs_set(&pet->story_flag, story); }

/* ============ 经验 ============ */

static void svc_exp_add(Pet *pet, int amount)
{
    if (!pet) return;
    pet->exp += (uint32_t)amount;
    while (pet->exp >= (uint32_t)(100 * pet->level) && pet->level < 99)
    {
        pet->exp -= (uint32_t)(100 * pet->level);
        pet->level++;
        pet->health = stat_of((int)pet->health + 5);
        pet->energy = stat_of((int)pet->energy + 10);

        g_pet_ctx = pet;
        ui_title("升级！");
        ui_print("\n  %s升到了 Lv.%d！\n  健康值 +5\n  活力值 +10\n", pet->name, pet->level);
        if (!g_headless) { ui_print("\nZ:继续\n"); ui_key(); }
    }

    /* 加经验后统一过一遍成就（和原版 ach_check 的位置一致） */
    (void)modloader_ach_poll(pet);
}

/* ============ 时钟 / 规则 ============ */

static uint32_t g_boot_time;

static uint32_t svc_rtc_tick(void)
{
    uint32_t now = (uint32_t)time(NULL);
    if (g_boot_time == 0) g_boot_time = now;
    return now - g_boot_time;
}

/* 游戏内天数：10 秒一天（和原版 rtc_day 一致） */
static uint32_t svc_rtc_day(void)
{
    return svc_rtc_tick() / 10u;
}

/* 夜间：真实时间 20:00~06:00，或 tick 末位为 1（伪装夜间） */
static int svc_rtc_night(void)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);

    if (lt && (lt->tm_hour >= 20 || lt->tm_hour < 6)) return 1;
    return (svc_rtc_tick() & 1u) ? 1 : 0;
}

/* 特质互斥：任一方的 conflict 指向对方名字就算冲突 */
static int svc_trait_conflict(Id a, Id b)
{
    const Trait_t *ta = (const Trait_t *)reg_get(KIND_TRAIT, a);
    const Trait_t *tb = (const Trait_t *)reg_get(KIND_TRAIT, b);

    if (!ta || !tb || a == b) return 0;
    if (ta->conflict && tb->name && strcmp(ta->conflict, tb->name) == 0) return 1;
    if (tb->conflict && ta->name && strcmp(tb->conflict, ta->name) == 0) return 1;
    return 0;
}

/* ============ 生死流程 ============ */

static DeadPet_t g_dead[MAX_DEAD];
static uint32_t g_dead_n;
static char g_dead_file[MAX_PATHLEN] = "dead.dat"; /* 跨启动保存的逝去记录 */

#define DEAD_MAGIC "EPDEAD1" /* 8 字节（含结尾 0） */
#define DEAD_VER   2u

static char *dup_str(const char *s)
{
    size_t n;
    char *p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void dead_free(DeadPet_t *d)
{
    free((void *)d->name);
    free((void *)d->type_name);
    free((void *)d->death_reason);
    d->name = NULL;
    d->type_name = NULL;
    d->death_reason = NULL;
}

/* ---- 序列化缓冲（落盘前整块异或 + 校验字节） ---- */

#define DEAD_XOR 0x5A /* 与原版 Save/Ach 相同的异或密钥 */

typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buf;

static int buf_put(Buf *b, const void *src, size_t n)
{
    if (b->len + n > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        unsigned char *p;
        while (cap < b->len + n) cap *= 2;
        p = (unsigned char *)realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, src, n);
    b->len += n;
    return 1;
}

static int buf_put_str(Buf *b, const char *s)
{
    uint64_t n = s ? (uint64_t)strlen(s) : 0;
    if (!buf_put(b, &n, sizeof(n))) return 0;
    return (n == 0) ? 1 : buf_put(b, s, (size_t)n);
}

typedef struct {
    const unsigned char *p;
    size_t n;
    size_t pos;
} Reader;

static int rd_bytes(Reader *r, void *dst, size_t n)
{
    if (n > r->n - r->pos) return 0;
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
    return 1;
}

/* 读一个字符串（堆分配，调用方负责 free）；失败返回 0 */
static int rd_str(Reader *r, const char **out)
{
    uint64_t n = 0;
    char *p;

    if (!rd_bytes(r, &n, sizeof(n))) return 0;
    if (n > (uint64_t)(r->n - r->pos) || n >= (uint64_t)SIZE_MAX) return 0;
    p = (char *)malloc((size_t)n + 1);
    if (!p) return 0;
    if (n && !rd_bytes(r, p, (size_t)n)) { free(p); return 0; }
    p[n] = '\0';
    *out = p;
    return 1;
}

/* ---- 配置文件（config.dat，整块异或 + 校验字节） ---- */

#define CFG_MAGIC "EPCFG01" /* 8 字节（含结尾 0） */
#define CFG_VER   3u
#define CFG_XOR   0x5A

static void cfg_save(void)
{
    Buf b;
    FILE *f;
    uint64_t i;
    uint32_t ver = CFG_VER;
    unsigned char sum = 0;

    memset(&b, 0, sizeof(b));
    if (!buf_put(&b, CFG_MAGIC, 8) ||
        !buf_put(&b, &ver, sizeof(ver)) ||
        !buf_put(&b, &g_cfg_n, sizeof(g_cfg_n)))
    {
        free(b.data);
        return;
    }

    for (i = 0; i < g_cfg_n; i++)
    {
        int32_t val = g_cfg[i].value;
        if (!buf_put_str(&b, g_cfg[i].name) || !buf_put(&b, &val, sizeof(val)))
        {
            free(b.data);
            return;
        }
    }

    for (i = 0; i < (uint64_t)b.len; i++)
    {
        b.data[i] = (unsigned char)(b.data[i] ^ CFG_XOR);
        sum = (unsigned char)(sum ^ b.data[i]);
    }

    f = fopen(g_cfg_file, "wb");
    if (f)
    {
        fwrite(b.data, 1, b.len, f);
        fwrite(&sum, 1, 1, f);
        fclose(f);
    }
    free(b.data);
}

static void cfg_load(void)
{
    FILE *f;
    long sz;
    unsigned char *raw, sum = 0;
    size_t len, i;
    Reader r;
    char magic[8];
    uint32_t ver = 0;
    uint64_t n = 0, k;

    g_cfg_n = 0;
    f = fopen(g_cfg_file, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz < 17) { fclose(f); return; }
    rewind(f);
    raw = (unsigned char *)malloc((size_t)sz);
    if (!raw) { fclose(f); return; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return; }
    fclose(f);

    len = (size_t)sz - 1;
    for (i = 0; i < len; i++) sum = (unsigned char)(sum ^ raw[i]);
    if (sum != raw[len]) { free(raw); return; }
    for (i = 0; i < len; i++) raw[i] = (unsigned char)(raw[i] ^ CFG_XOR);

    r.p = raw;
    r.n = len;
    r.pos = 0;

    if (!rd_bytes(&r, magic, 8) || memcmp(magic, CFG_MAGIC, 8) != 0 ||
        !rd_bytes(&r, &ver, sizeof(ver)) || ver != CFG_VER ||
        !rd_bytes(&r, &n, sizeof(n)))
    {
        free(raw);
        return;
    }

    for (k = 0; k < n; k++)
    {
        char *name = NULL;
        int32_t val = 0;

        if (!rd_str(&r, (const char **)&name) || !rd_bytes(&r, &val, sizeof(val)))
        {
            free(name);
            while (g_cfg_n > 0) free(g_cfg[--g_cfg_n].name);
            free(raw);
            return;
        }

        if (name[0] && !cfg_find(name) &&
            vec_grow((void **)&g_cfg, &g_cfg_cap, g_cfg_n + 1, sizeof(*g_cfg)))
        {
            ConfigValue *v = &g_cfg[g_cfg_n++];
            v->name = name;
            v->value = val;
        }
        else free(name);
    }
    free(raw);
}

static void dead_save(void)
{
    Buf b;
    FILE *f;
    uint32_t ver = DEAD_VER;
    uint32_t i;
    unsigned char sum = 0;

    memset(&b, 0, sizeof(b));
    if (!buf_put(&b, DEAD_MAGIC, 8) ||
        !buf_put(&b, &ver, sizeof(ver)) ||
        !buf_put(&b, &g_dead_n, sizeof(g_dead_n)))
    {
        free(b.data);
        return;
    }

    for (i = 0; i < g_dead_n; i++)
    {
        const DeadPet_t *d = &g_dead[i];
        uint16_t age = d->age;
        uint8_t level = d->level;
        uint8_t gender = (uint8_t)d->gender;
        uint32_t coins = d->coins;
        int ok = buf_put_str(&b, d->name) &&
                 buf_put_str(&b, d->type_name) &&
                 buf_put_str(&b, d->death_reason) &&
                 buf_put(&b, &age, sizeof(age)) &&
                 buf_put(&b, &level, sizeof(level)) &&
                 buf_put(&b, &gender, sizeof(gender)) &&
                 buf_put(&b, &coins, sizeof(coins));
        if (!ok) { free(b.data); return; }
    }

    /* 整块异或 + 校验字节（校验值 = 异或后所有字节的异或） */
    for (i = 0; i < b.len; i++)
    {
        b.data[i] = (unsigned char)(b.data[i] ^ DEAD_XOR);
        sum = (unsigned char)(sum ^ b.data[i]);
    }

    f = fopen(g_dead_file, "wb");
    if (f)
    {
        fwrite(b.data, 1, b.len, f);
        fwrite(&sum, 1, 1, f);
        fclose(f);
    }
    free(b.data);
}

static void dead_load(void)
{
    FILE *f;
    long sz;
    unsigned char *raw;
    unsigned char sum = 0;
    size_t len, i;
    Reader r;

    f = fopen(g_dead_file, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz < 17) { fclose(f); return; } /* magic+ver+count+checksum 都不够 */
    rewind(f);
    raw = (unsigned char *)malloc((size_t)sz);
    if (!raw) { fclose(f); return; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return; }
    fclose(f);

    len = (size_t)sz - 1;
    for (i = 0; i < len; i++) sum = (unsigned char)(sum ^ raw[i]);
    if (sum != raw[len])
    {
        free(raw); /* 校验失败：当成损坏，不加载 */
        return;
    }

    for (i = 0; i < len; i++) raw[i] = (unsigned char)(raw[i] ^ DEAD_XOR);

    r.p = raw;
    r.n = len;
    r.pos = 0;

    {
        char magic[8];
        uint32_t ver = 0, n = 0, k;

        if (!rd_bytes(&r, magic, 8) || memcmp(magic, DEAD_MAGIC, 8) != 0 ||
            !rd_bytes(&r, &ver, sizeof(ver)) || ver != DEAD_VER ||
            !rd_bytes(&r, &n, sizeof(n)))
        {
            free(raw);
            return;
        }
        if (n > MAX_DEAD) n = MAX_DEAD;

        for (k = 0; k < n; k++)
        {
            DeadPet_t d;
            uint16_t age = 0;
            uint8_t level = 0, gender = 0;
            uint32_t coins = 0;

            memset(&d, 0, sizeof(d));
            if (!rd_str(&r, &d.name) || !rd_str(&r, &d.type_name) || !rd_str(&r, &d.death_reason) ||
                !rd_bytes(&r, &age, sizeof(age)) ||
                !rd_bytes(&r, &level, sizeof(level)) ||
                !rd_bytes(&r, &gender, sizeof(gender)) ||
                !rd_bytes(&r, &coins, sizeof(coins)))
            {
                dead_free(&d);
                break;
            }
            d.age = age;
            d.level = level;
            d.gender = (Gender_t)gender;
            d.coins = coins;
            g_dead[g_dead_n++] = d;
        }
    }

    free(raw);
}
/* 按当前状态推断死因 */
static const char *death_reason_of(const Pet *pet)
{
    if (pet->hunger < 20) return "饿死了";
    if (pet->clean < 20) return "脏死了";
    if (pet->sick != D_NONE) return "病死了";
    if (pet->age > 30) return "寿终正寝";
    return "健康耗尽";
}
/* 记一条逝去记录：字符串一律深拷贝，模组卸载/换版本后记录仍然有效 */
static void dead_record(const Pet *pet)
{
    const PetType *t = (const PetType *)reg_get(KIND_PET_TYPE, pet->type_id);
    DeadPet_t rec;

    memset(&rec, 0, sizeof(rec));
    rec.name = dup_str(pet->name ? pet->name : "?");
    rec.type_name = dup_str(t && t->name ? t->name : "未知");
    rec.death_reason = dup_str(pet->death_reason ? pet->death_reason : "未知原因");
    rec.age = pet->age;
    rec.level = pet->level;
    rec.gender = pet->gender;
    rec.coins = pet->coins;

    if (g_dead_n >= MAX_DEAD)
    {
        dead_free(&g_dead[0]);
        memmove(&g_dead[0], &g_dead[1], sizeof(DeadPet_t) * (MAX_DEAD - 1));
        g_dead_n = MAX_DEAD - 1;
    }
    g_dead[g_dead_n++] = rec;

    dead_save(); /* 每次离世立刻落盘 */
}

int modloader_pet_die(Pet *pet, const char *reason)
{
    if (!pet || pet->dead) return 0;

    pet->health = 0;
    pet->dead = 1;
    pet->sleeping = 0;
    pet->death_reason = reason ? reason : death_reason_of(pet);

    g_pet_ctx = pet;
    /* 死亡状态立即落盘：即使玩家在离世画面直接关掉进程，下次也能读到 dead */
    (void)modloader_save(pet);


    /* 内容钩子：模组可以注册 scene 为 death 的剧情（遗言/悼念） */
    (void)modloader_story_play(pet, "death", NULL);

    ui_title("宠物离世");
    ui_print("\n  %s 因为%s永远离开了我们...\n",
             pet->name ? pet->name : "它",
             pet->death_reason ? pet->death_reason : "未知原因");
    ui_print("\n  它陪伴了你 %u 天\n", (unsigned)pet->age);
    if (!g_headless) { ui_print("Z:返回\n"); ui_key(); }

    dead_record(pet);
    (void)modloader_ach_poll(pet);
    return 1;
}

int modloader_pet_day(Pet *pet)
{
    if (!pet || pet->dead) return 0;
    g_pet_ctx = pet;

    /* 睡觉：只回活力，不衰老 */
    if (pet->sleeping)
    {
        pet->energy = stat_of((int)pet->energy + 15);
        if (pet->energy >= MAX_STAT) pet->sleeping = 0;
        return 0;
    }

    /* 自然衰减 */
    pet->hunger = stat_of((int)pet->hunger - 1);
    pet->happy  = stat_of((int)pet->happy - 1);
    pet->clean  = stat_of((int)pet->clean - 1);
    pet->energy = stat_of((int)pet->energy - 1);

    /* 幼年/少年额外的清洁衰减 */
    if (pet->stage == S_BABY && svc_rnd(0, 99) < 10u) pet->clean = stat_of((int)pet->clean - 5);
    if (pet->stage == S_CHILD && svc_rnd(0, 99) < 5u) pet->clean = stat_of((int)pet->clean - 8);

    /* 属性过低拖健康 */
    if (pet->hunger < 20 || pet->clean < 20 || pet->happy < 20)
        pet->health = stat_of((int)pet->health - 2);

    /* 健康过低会生病 */
    if (!pet->sick && pet->health < 30 && svc_rnd(0, 4) == 0u)
    {
        if (pet->hunger < 20) pet->sick = D_HUNGRY;
        else if (pet->clean < 20) pet->sick = D_DIRTY;
        else pet->sick = D_TIRED;
    }

    /* 活力耗尽自己去睡 */
    if (pet->energy < 10 && svc_rnd(0, 99) < 50u) pet->sleeping = 1;

    /* 成长 */
    pet->age++;
    if (pet->age == 5) pet->stage = S_CHILD;
    if (pet->age == 15) pet->stage = S_ADULT;
    pet->rebellious = (pet->age >= 10 && pet->age <= 18) ? 1 : 0;

    (void)modloader_ach_poll(pet);

    /* 老年寿终：30 天后概率随年龄上升，封顶 50% */
    if (pet->age > 30)
    {
        uint32_t chance = (uint32_t)((int)pet->age - 30) * 2u;
        if (chance > 50u) chance = 50u;
        if (svc_rnd(0, 99) < chance)
        {
            pet->health = 0;
            pet->death_reason = "寿终正寝";
        }
    }

    /* 好结局之后：满级每天 8% 安详离世 */
    if (pet->level >= 100 && svc_rnd(0, 99) < 8u)
    {
        pet->health = 0;
        pet->death_reason = "安详离世";
    }

    if (pet->health == 0) return modloader_pet_die(pet, pet->death_reason);
    return 0;
}

uint32_t modloader_dead_count(void) { return g_dead_n; }

const DeadPet_t *modloader_dead_at(uint32_t index)
{
    return (index < g_dead_n) ? &g_dead[index] : NULL;
}

void modloader_dead_clear(void)
{
    uint32_t i;

    for (i = 0; i < g_dead_n; i++) dead_free(&g_dead[i]);
    g_dead_n = 0;
    dead_save(); /* 清空也要落盘 */
}

void modloader_dead_menu(void)
{
    uint32_t page = 0, per = 4, pages;

    if (g_dead_n == 0)
    {
        ui_title("逝去的宠物");
        ui_print("暂无记录\n");
        if (!g_headless) ui_key();
        return;
    }

    pages = (g_dead_n + per - 1) / per;

    while (1)
    {
        uint32_t i, start, end;

        ui_title("逝去的宠物");
        ui_print("\n  第 %u/%u 页\n\n", (unsigned)(page + 1), (unsigned)pages);

        start = page * per;
        end = (start + per > g_dead_n) ? g_dead_n : start + per;
        for (i = start; i < end; i++)
        {
            const DeadPet_t *d = &g_dead[i];
            ui_print("  %u. %s (%s) - %u天 · %s\n",
                     (unsigned)(i + 1),
                     d->name ? d->name : "?",
                     d->type_name ? d->type_name : "?",
                     (unsigned)d->age,
                     d->death_reason ? d->death_reason : "未知");
        }

        if (g_headless)
        {
            /* 无头模式：把后面几页也打出来 */
            if (end >= g_dead_n) return;
            page++;
            continue;
        }

        ui_print("\n");
        if (page > 0) ui_print("  B:上一页");
        if (end < g_dead_n) ui_print("  C:下一页");
        ui_print("  X:返回\n");

        {
            int k = ui_key();
            if (k == 2 && page > 0) page--;
            else if (k == 3 && end < g_dead_n) page++;
            else if (k == 4) break;
        }
    }
}

void modloader_pet_inherit(Pet *pet)
{
    if (pet) pet->lost_friend = (g_dead_n > 0) ? 1 : 0;
}

/* ============ 存档（Pet 按名字序列化） ============ */

#define PET_MAGIC "EPPET01" /* 8 字节（含结尾 0） */
#define PET_VER   3u
#define SAVE_XOR  0x5A

static char g_save_file[MAX_PATHLEN] = "pet.sav";

/* 加载时分配的字符串：由 modloader_pet_release / 下次加载释放（动态数组） */
static void **g_owned;
static uint64_t g_owned_n;
static uint64_t g_owned_cap;

static int owned_add(void *p)
{
    if (!p) return 1;
    if (!vec_grow((void **)&g_owned, &g_owned_cap, g_owned_n + 1, sizeof(*g_owned))) return 0;
    g_owned[g_owned_n++] = p;
    return 1;
}

static void owned_free_from(uint64_t mark)
{
    uint64_t i;
    for (i = mark; i < g_owned_n; i++) free(g_owned[i]);
    g_owned_n = mark;
}

static void owned_keep_from(uint64_t mark)
{
    uint64_t i;
    for (i = 0; i < mark; i++) free(g_owned[i]);
    for (i = mark; i < g_owned_n; i++) g_owned[i - mark] = g_owned[i];
    g_owned_n -= mark;
}

static int put_u8(Buf *b, uint8_t v) { return buf_put(b, &v, 1); }
static int put_u16(Buf *b, uint16_t v) { return buf_put(b, &v, 2); }
static int put_u32(Buf *b, uint32_t v) { return buf_put(b, &v, 4); }
static int put_u64(Buf *b, uint64_t v) { return buf_put(b, &v, 8); }

static int rd_u8(Reader *r, uint8_t *v) { return rd_bytes(r, v, 1); }
static int rd_u16(Reader *r, uint16_t *v) { return rd_bytes(r, v, 2); }
static int rd_u32(Reader *r, uint32_t *v) { return rd_bytes(r, v, 4); }
static int rd_u64(Reader *r, uint64_t *v) { return rd_bytes(r, v, 8); }

/* Id → 名字写盘；ID_NONE 写空串 */
static int put_id(Buf *b, ContentKind_t kind, Id id)
{
    const void *p = reg_get(kind, id);
    return buf_put_str(b, p ? kind_name(kind, p) : "");
}

/* 读名字 → Id；找不到就是 ID_NONE */
static int rd_id(Reader *r, ContentKind_t kind, Id *out)
{
    const char *name;
    if (!rd_str(r, &name)) return 0;
    *out = (name[0] == '\0') ? ID_NONE : reg_find(kind, name);
    free((void *)name);
    return 1;
}

/* 位图按名字列表存 */
static int put_bitset(Buf *b, ContentKind_t kind, const BitSet_t *set)
{
    uint64_t i, n = reg_count(kind), cnt = 0;

    for (i = 1; i <= n; i++) if (bs_test(set, (Id)i)) cnt++;
    if (!put_u64(b, cnt)) return 0;
    for (i = 1; i <= n; i++)
        if (bs_test(set, (Id)i) && !put_id(b, kind, (Id)i)) return 0;
    return 1;
}

static int rd_bitset(Reader *r, ContentKind_t kind, BitSet_t *set)
{
    uint64_t cnt = 0, k;
    Id id;

    if (!rd_u64(r, &cnt)) return 0;
    for (k = 0; k < cnt; k++)
    {
        if (!rd_id(r, kind, &id)) return 0;
        if (id != ID_NONE) bs_set(set, id);
    }
    return 1;
}

/* 跨存档成就（ach.dat） */
static void ach_global_save(void)
{
    Buf b;
    FILE *f;
    size_t i;
    unsigned char sum = 0;

    memset(&b, 0, sizeof(b));
    if (!buf_put(&b, "EPACH01", 8) || !put_u32(&b, 2u) || !put_bitset(&b, KIND_ACHIEVEMENT, &g_ach_all))
    {
        free(b.data);
        return;
    }
    for (i = 0; i < b.len; i++)
    {
        b.data[i] = (unsigned char)(b.data[i] ^ SAVE_XOR);
        sum = (unsigned char)(sum ^ b.data[i]);
    }
    f = fopen(g_ach_file, "wb");
    if (f)
    {
        fwrite(b.data, 1, b.len, f);
        fwrite(&sum, 1, 1, f);
        fclose(f);
    }
    free(b.data);
}

static void ach_global_load(void)
{
    FILE *f = fopen(g_ach_file, "rb");
    long sz;
    unsigned char *raw, sum = 0;
    size_t len, i;
    Reader r;
    char magic[8];
    uint32_t ver = 0;

    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    sz = ftell(f);
    if (sz < 17) { fclose(f); return; }
    rewind(f);
    raw = (unsigned char *)malloc((size_t)sz);
    if (!raw) { fclose(f); return; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return; }
    fclose(f);

    len = (size_t)sz - 1;
    for (i = 0; i < len; i++) sum = (unsigned char)(sum ^ raw[i]);
    if (sum != raw[len]) { free(raw); return; }
    for (i = 0; i < len; i++) raw[i] = (unsigned char)(raw[i] ^ SAVE_XOR);

    r.p = raw;
    r.n = len;
    r.pos = 0;
    if (rd_bytes(&r, magic, 8) && memcmp(magic, "EPACH01", 8) == 0 &&
        rd_u32(&r, &ver) && ver == 2u)
        (void)rd_bitset(&r, KIND_ACHIEVEMENT, &g_ach_all);
    free(raw);
}

static int save_build(const Pet *pet, Buf *b)
{
    const PetType *t = (const PetType *)reg_get(KIND_PET_TYPE, pet->type_id);
    uint64_t i;

    if (!buf_put(b, PET_MAGIC, 8)) return 0;
    if (!put_u32(b, PET_VER)) return 0;
    if (!buf_put_str(b, t && t->name ? t->name : "")) return 0;
    if (!buf_put_str(b, pet->name ? pet->name : "")) return 0;

    if (!put_u8(b, (uint8_t)pet->gender)) return 0;
    if (!put_u16(b, pet->age)) return 0;
    if (!put_u8(b, (uint8_t)pet->stage)) return 0;
    if (!put_u8(b, pet->hunger)) return 0;
    if (!put_u8(b, pet->clean)) return 0;
    if (!put_u8(b, pet->happy)) return 0;
    if (!put_u8(b, pet->health)) return 0;
    if (!put_u8(b, pet->energy)) return 0;
    for (i = 0; i < 3; i++) if (!put_u8(b, pet->personality[i])) return 0;

    {
        uint64_t cnt = 0;
        for (i = 0; i < pet->trait_cnt; i++)
            if (pet->traits[i] != ID_NONE) cnt++;
        if (!put_u64(b, cnt)) return 0;
        for (i = 0; i < pet->trait_cnt; i++)
            if (pet->traits[i] != ID_NONE && !put_id(b, KIND_TRAIT, pet->traits[i])) return 0;
    }

    if (!put_u32(b, pet->coins)) return 0;

    {
        uint64_t cnt = 0;
        for (i = 0; i < pet->inv_cnt; i++)
            if (pet->inv[i].id != ID_NONE && pet->inv[i].count > 0) cnt++;
        if (!put_u64(b, cnt)) return 0;
        for (i = 0; i < pet->inv_cnt; i++)
        {
            if (pet->inv[i].id == ID_NONE || pet->inv[i].count == 0) continue;
            if (!put_id(b, KIND_ITEM, pet->inv[i].id)) return 0;
            if (!put_u64(b, pet->inv[i].count)) return 0;
        }
    }

    if (!put_u8(b, (uint8_t)pet->sick)) return 0;
    if (!put_u8(b, pet->dead)) return 0;
    if (!put_u8(b, pet->rebellious)) return 0;
    if (!put_u8(b, pet->lost_friend)) return 0;
    if (!put_u8(b, pet->sleeping)) return 0;
    if (!put_u8(b, pet->task_feed)) return 0;
    if (!put_u8(b, pet->task_play)) return 0;
    if (!put_u8(b, pet->task_complete)) return 0;

    if (!put_u32(b, pet->exp)) return 0;
    if (!put_u8(b, pet->level)) return 0;
    if (!put_u16(b, pet->login_days)) return 0;

    if (!put_bitset(b, KIND_ACHIEVEMENT, &pet->ach)) return 0;
    if (!put_bitset(b, KIND_ACHIEVEMENT, &pet->ach_claim)) return 0;
    if (!put_bitset(b, KIND_STORY, &pet->story_flag)) return 0;

    if (!put_u16(b, pet->total_feed)) return 0;
    if (!put_u16(b, pet->total_play)) return 0;
    if (!put_u16(b, pet->total_visit)) return 0;
    if (!put_u16(b, pet->total_win)) return 0;
    if (!put_u16(b, pet->total_gift_ok)) return 0;
    if (!put_u16(b, pet->hungry_streak)) return 0;
    if (!put_u16(b, pet->win_streak)) return 0;
    if (!put_u16(b, pet->lose_streak)) return 0;
    if (!put_u8(b, pet->trait_reroll_cnt)) return 0;
    if (!put_u32(b, pet->big_spend)) return 0;

    if (!buf_put_str(b, pet->death_reason ? pet->death_reason : "")) return 0;
    return 1;
}

static int load_parse(Reader *r, Pet *pet)
{
    char magic[8];
    uint32_t ver = 0;
    uint64_t cnt = 0;
    uint8_t u8 = 0;
    uint64_t i;
    const char *str = NULL;
    Id id;

    if (!rd_bytes(r, magic, 8) || memcmp(magic, PET_MAGIC, 8) != 0) return 0;
    if (!rd_u32(r, &ver) || ver != PET_VER) return 0;

    if (!rd_id(r, KIND_PET_TYPE, &pet->type_id)) return 0;

    if (!rd_str(r, &str)) return 0;
    if (!owned_add((void *)str)) { free((void *)str); return 0; }
    pet->name = str;

    if (!rd_u8(r, &u8)) return 0; pet->gender = (Gender_t)u8;
    if (!rd_u16(r, &pet->age)) return 0;
    if (!rd_u8(r, &u8)) return 0; pet->stage = (Stage_t)u8;
    if (!rd_u8(r, &pet->hunger)) return 0;
    if (!rd_u8(r, &pet->clean)) return 0;
    if (!rd_u8(r, &pet->happy)) return 0;
    if (!rd_u8(r, &pet->health)) return 0;
    if (!rd_u8(r, &pet->energy)) return 0;
    for (i = 0; i < 3; i++) if (!rd_u8(r, &pet->personality[i])) return 0;

    if (!rd_u64(r, &cnt)) return 0;
    if (cnt > 0 && !pet_traits_reserve(pet, cnt)) return 0;
    for (i = 0; i < cnt; i++)
    {
        if (!rd_id(r, KIND_TRAIT, &id)) return 0;
        if (id != ID_NONE) pet->traits[pet->trait_cnt++] = id;
    }

    if (!rd_u32(r, &pet->coins)) return 0;

    if (!rd_u64(r, &cnt)) return 0;
    if (cnt > 0 && !pet_inv_reserve(pet, cnt)) return 0;
    for (i = 0; i < cnt; i++)
    {
        uint64_t amount = 0;
        if (!rd_id(r, KIND_ITEM, &id)) return 0;
        if (!rd_u64(r, &amount)) return 0;
        if (id != ID_NONE && amount > 0)
        {
            pet->inv[pet->inv_cnt].id = id;
            pet->inv[pet->inv_cnt].count = amount;
            pet->inv_cnt++;
        }
    }

    if (!rd_u8(r, &u8)) return 0; pet->sick = (Disease_t)u8;
    if (!rd_u8(r, &pet->dead)) return 0;
    if (!rd_u8(r, &pet->rebellious)) return 0;
    if (!rd_u8(r, &pet->lost_friend)) return 0;
    if (!rd_u8(r, &pet->sleeping)) return 0;
    if (!rd_u8(r, &pet->task_feed)) return 0;
    if (!rd_u8(r, &pet->task_play)) return 0;
    if (!rd_u8(r, &pet->task_complete)) return 0;

    if (!rd_u32(r, &pet->exp)) return 0;
    if (!rd_u8(r, &pet->level)) return 0;
    if (!rd_u16(r, &pet->login_days)) return 0;

    if (!rd_bitset(r, KIND_ACHIEVEMENT, &pet->ach)) return 0;
    if (!rd_bitset(r, KIND_ACHIEVEMENT, &pet->ach_claim)) return 0;
    if (!rd_bitset(r, KIND_STORY, &pet->story_flag)) return 0;

    if (!rd_u16(r, &pet->total_feed)) return 0;
    if (!rd_u16(r, &pet->total_play)) return 0;
    if (!rd_u16(r, &pet->total_visit)) return 0;
    if (!rd_u16(r, &pet->total_win)) return 0;
    if (!rd_u16(r, &pet->total_gift_ok)) return 0;
    if (!rd_u16(r, &pet->hungry_streak)) return 0;
    if (!rd_u16(r, &pet->win_streak)) return 0;
    if (!rd_u16(r, &pet->lose_streak)) return 0;
    if (!rd_u8(r, &pet->trait_reroll_cnt)) return 0;
    if (!rd_u32(r, &pet->big_spend)) return 0;

    if (!rd_str(r, &str)) return 0;
    if (str[0] == '\0') { free((void *)str); pet->death_reason = NULL; }
    else
    {
        if (!owned_add((void *)str)) { free((void *)str); return 0; }
        pet->death_reason = str;
    }
    return 1;
}

int modloader_save(const Pet *pet)
{
    Buf b;
    FILE *f;
    size_t i;
    unsigned char sum = 0;

    if (!pet) return 0;
    memset(&b, 0, sizeof(b));
    if (!save_build(pet, &b)) { free(b.data); return 0; }

    for (i = 0; i < b.len; i++)
    {
        b.data[i] = (unsigned char)(b.data[i] ^ SAVE_XOR);
        sum = (unsigned char)(sum ^ b.data[i]);
    }

    f = fopen(g_save_file, "wb");
    if (!f) { free(b.data); return 0; }
    fwrite(b.data, 1, b.len, f);
    fwrite(&sum, 1, 1, f);
    fclose(f);
    free(b.data);
    return 1;
}

int modloader_load(Pet *pet)
{
    FILE *f;
    long sz;
    unsigned char *raw, sum = 0;
    size_t len, i;
    Reader r;
    Pet tmp;
    uint64_t mark;
    int ok;

    if (!pet) return 0;
    f = fopen(g_save_file, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz < 17) { fclose(f); return 0; }
    rewind(f);
    raw = (unsigned char *)malloc((size_t)sz);
    if (!raw) { fclose(f); return 0; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { free(raw); fclose(f); return 0; }
    fclose(f);

    len = (size_t)sz - 1;
    for (i = 0; i < len; i++) sum = (unsigned char)(sum ^ raw[i]);
    if (sum != raw[len]) { free(raw); return 0; }
    for (i = 0; i < len; i++) raw[i] = (unsigned char)(raw[i] ^ SAVE_XOR);

    memset(&tmp, 0, sizeof(tmp));

    mark = g_owned_n;
    r.p = raw;
    r.n = len;
    r.pos = 0;

    ok = load_parse(&r, &tmp);
    free(raw);

    if (!ok)
    {
        owned_free_from(mark);
        free(tmp.traits);
        free(tmp.inv);
        bs_free(&tmp.ach);
        bs_free(&tmp.ach_claim);
        bs_free(&tmp.story_flag);
        return 0;
    }

    /* 成功：丢掉旧状态与旧字符串，接管新状态（动态数组/位图指针整体转移） */
    free(pet->traits);
    free(pet->inv);
    bs_free(&pet->ach);
    bs_free(&pet->ach_claim);
    bs_free(&pet->story_flag);
    owned_keep_from(mark);
    *pet = tmp;
    return 1;
}

void modloader_set_save_file(const char *path)
{
    if (!path) return;
    strncpy(g_save_file, path, sizeof(g_save_file) - 1);
    g_save_file[sizeof(g_save_file) - 1] = '\0';
}

/* ============ 加载模组 ============ */

/*
 * 两阶段加载：
 *   1) 只 LoadLibrary + 读 mod_info：校验导出/接口版本/依赖声明，不跑 mod_init；
 *   2) 按依赖做拓扑排序，依赖就绪才 mod_init。
 * 这样“A 依赖 B”不再受 DLL 文件名顺序影响，失败也能给出具体原因。
 */

typedef struct {
    char *file;
    char *path;
    char *name;
    HMODULE handle;
    const ModInfo_t *info;
    const char *(*p_error)(void);
    int (*p_init)(const LoaderApi *);
    int state; /* 0=等待依赖 1=已加载 2=失败 */
} ModCand;

static ModCand *g_cand;
static uint64_t g_cand_n;
static uint64_t g_cand_cap;

/* 注册表快照：mod_init 失败时回滚它已经登记的内容与名字 */
typedef struct {
    uint64_t content[KIND_COUNT];
    uint64_t names[3];
    uint64_t cfg_n;
    ConfigValue *cfg; /* g_cfg 前 cfg_n 项的副本（name 指针共享） */
} RegMark;

static void reg_mark(RegMark *m)
{
    uint64_t i;
    for (i = 0; i < KIND_COUNT; i++) m->content[i] = g_n[i];
    for (i = 0; i < 3; i++) m->names[i] = g_names_n[i];
    m->cfg_n = g_cfg_n;
    m->cfg = NULL;
    if (g_cfg_n)
    {
        m->cfg = (ConfigValue *)malloc((size_t)g_cfg_n * sizeof(*m->cfg));
        if (m->cfg) memcpy(m->cfg, g_cfg, (size_t)g_cfg_n * sizeof(*m->cfg));
    }
}

static void reg_mark_free(RegMark *m)
{
    free(m->cfg);
    m->cfg = NULL;
}

static void reg_rollback(RegMark *m)
{
    uint64_t i;

    /* 撤掉本模组新增的配置项（名字是堆内存） */
    for (i = m->cfg_n; i < g_cfg_n; i++) free(g_cfg[i].name);

    for (i = 0; i < KIND_COUNT; i++) g_n[i] = m->content[i];
    for (i = 0; i < 3; i++) g_names_n[i] = m->names[i];

    if (m->cfg) memcpy(g_cfg, m->cfg, (size_t)m->cfg_n * sizeof(*m->cfg));
    g_cfg_n = m->cfg_n;
    reg_mark_free(m);
}

static const char *cand_name(const ModCand *c)
{
    if (c->name) return c->name;
    return c->file ? c->file : "?";
}

static ModCand *cand_by_name(const char *name)
{
    uint64_t i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < g_cand_n; i++)
        if (g_cand[i].name && _stricmp(g_cand[i].name, name) == 0) return &g_cand[i];
    return NULL;
}

/* 把 Windows 错误码转成可读文本（尽量用 UTF-8，失败了就只留错误码） */
static void win_error_text(DWORD code, char *out, size_t cap)
{
    wchar_t wbuf[256];
    DWORD n;
    int len;

    out[0] = '\0';
    n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])), NULL);
    if (!n)
    {
        _snprintf(out, cap, "Windows 错误 %lu", (unsigned long)code);
        out[cap - 1] = '\0';
        return;
    }
    while (n > 0 && (wbuf[n - 1] == L'\r' || wbuf[n - 1] == L'\n' || wbuf[n - 1] == L' ')) n--;
    wbuf[n] = L'\0';

    len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL);
    if (len <= 0)
    {
        _snprintf(out, cap, "Windows 错误 %lu", (unsigned long)code);
        out[cap - 1] = '\0';
    }
}

static void cand_fail(ModCand *c, const char *reason, const char *detail)
{
    err_add(0, cand_name(c), c->file, reason, detail);
    c->state = 2;
    if (c->handle) { FreeLibrary(c->handle); c->handle = NULL; }
}

static void cand_clear_strings(ModCand *c)
{
    free(c->file);
    free(c->path);
    free(c->name);
    c->file = NULL;
    c->path = NULL;
    c->name = NULL;
}

static void cand_drop(ModCand *c)
{
    if (c->handle) { FreeLibrary(c->handle); c->handle = NULL; }
    cand_clear_strings(c);
}

/* 阶段 1：只读 mod_info，做静态校验 */
static void cand_load(const char *dir, const char *file)
{
    ModCand *c;
    DWORD e;
    char detail[320];
    const char *miss[3];
    int miss_n = 0;
    size_t path_len;

    if (!vec_grow((void **)&g_cand, &g_cand_cap, g_cand_n + 1, sizeof(*g_cand))) return;
    c = &g_cand[g_cand_n];
    memset(c, 0, sizeof(*c));

    c->file = err_strdup(file);
    path_len = strlen(dir) + 1 + strlen(file) + 1;
    c->path = (char *)malloc(path_len);
    if (!c->file || !c->path)
    {
        err_add(0, file, file, "内存不足", "无法分配模组文件名/路径");
        cand_drop(c);
        return;
    }
    _snprintf(c->path, path_len, "%s\\%s", dir, file);

    c->handle = LoadLibraryA(c->path);
    if (!c->handle)
    {
        char fixed[320];
        const char *pct;

        e = GetLastError();
        win_error_text(e, detail, sizeof(detail));

        /* FormatMessage 的系统文案里带 %1（文件名占位符），这里补上真实文件名 */
        pct = strstr(detail, "%1");
        if (pct)
        {
            _snprintf(fixed, sizeof(fixed), "%.*s%s%s",
                      (int)(pct - detail), detail, file, pct + 2);
            fixed[sizeof(fixed) - 1] = '\0';
            _snprintf(detail, sizeof(detail), "%s", fixed);
            detail[sizeof(detail) - 1] = '\0';
        }

        err_add(0, file, file, "无法加载 DLL", detail);
        cand_drop(c);
        return;
    }

    {
        const ModInfo_t *(*p_info)(void) = (const ModInfo_t *(*)(void))GetProcAddress(c->handle, "mod_info");
        c->p_init = (int (*)(const LoaderApi *))GetProcAddress(c->handle, "mod_init");
        c->p_error = (const char *(*)(void))GetProcAddress(c->handle, "mod_error");

        if (!p_info) miss[miss_n++] = "mod_info";
        if (!c->p_init) miss[miss_n++] = "mod_init";
        if (!GetProcAddress(c->handle, "mod_exit")) miss[miss_n++] = "mod_exit";

        if (miss_n > 0)
        {
            char missing[96];
            int k;
            missing[0] = '\0';
            for (k = 0; k < miss_n; k++)
            {
                if (k > 0) strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1);
                strncat(missing, miss[k], sizeof(missing) - strlen(missing) - 1);
            }
            _snprintf(detail, sizeof(detail), "缺少导出函数：%s（三个入口见 Loader.h）", missing);
            detail[sizeof(detail) - 1] = '\0';
            err_add(0, file, file, "缺少导出函数", detail);
            cand_drop(c);
            return;
        }

        c->info = p_info();
    }

    if (!c->info)
    {
        err_add(0, file, file, "mod_info 返回空指针", "mod_info() 必须返回指向静态 ModInfo_t 的指针");
        cand_drop(c);
        return;
    }
    if (!c->info->name || !c->info->name[0])
    {
        err_add(0, file, file, "mod_info 没有名字", "ModInfo_t.name 不能为空：依赖检查与错误定位都靠它");
        cand_drop(c);
        return;
    }
    if (c->info->api_version != LOADER_API_VERSION)
    {
        _snprintf(detail, sizeof(detail),
                  "模组要求接口 v%u，当前加载器是 v%u；请用同一份 Loader.h 重新编译该模组",
                  (unsigned)c->info->api_version, (unsigned)LOADER_API_VERSION);
        detail[sizeof(detail) - 1] = '\0';
        err_add(0, c->info->name, file, "接口版本不符", detail);
        cand_drop(c);
        return;
    }
    if (c->info->require_count > 0 && !c->info->requires)
    {
        err_add(0, c->info->name, file, "依赖表损坏", "require_count > 0，但 requires 是空指针");
        cand_drop(c);
        return;
    }
    if (c->info->require_count > 32)
    {
        err_add(0, c->info->name, file, "依赖表过长", "单个模组最多声明 32 个依赖");
        cand_drop(c);
        return;
    }

    c->name = err_strdup(c->info->name);
    if (!c->name)
    {
        err_add(0, file, file, "内存不足", "无法分配模组名");
        cand_drop(c);
        return;
    }
    g_cand_n++;
}

/* 模组名重复：先枚举到的生效，后面的报错并跳过 */
static void cand_check_duplicates(void)
{
    uint64_t i, j;
    for (i = 0; i < g_cand_n; i++)
    {
        if (g_cand[i].state == 2) continue;
        for (j = 0; j < i; j++)
        {
            char detail[256];
            if (g_cand[j].state == 2) continue;
            if (_stricmp(g_cand[i].name, g_cand[j].name) != 0) continue;
            _snprintf(detail, sizeof(detail),
                      "与 %s 的模组重名；先加载的生效，本 DLL 被跳过", g_cand[j].file);
            detail[sizeof(detail) - 1] = '\0';
            cand_fail(&g_cand[i], "模组重名", detail);
            break;
        }
    }
}

/* 对一个依赖已就绪的模组跑 mod_init；失败时回滚内容并记录原因 */
static void cand_init(ModCand *c)
{
    RegMark mark;
    uint64_t errs = err_mark();
    int ok;

    if (!vec_grow((void **)&g_mods, &g_mod_cap, g_mod_n + 1, sizeof(*g_mods)))
    {
        err_add(0, cand_name(c), c->file, "内存不足", "无法扩容模组表");
        c->state = 2;
        FreeLibrary(c->handle);
        c->handle = NULL;
        return;
    }

    g_current_mod = cand_name(c);
    reg_mark(&mark);
    ok = c->p_init(&g_api);
    g_current_mod = NULL;

    if (!ok)
    {
        const char *why = c->p_error ? c->p_error() : NULL;
        char detail[320];

        reg_rollback(&mark);
        err_rollback(errs);

        if (why && why[0]) _snprintf(detail, sizeof(detail), "%s", why);
        else _snprintf(detail, sizeof(detail), "模组没有导出 mod_error()，无法得知具体原因");
        detail[sizeof(detail) - 1] = '\0';

        err_add(0, cand_name(c), c->file, "mod_init 返回 0", detail);
        c->state = 2;
        FreeLibrary(c->handle);
        c->handle = NULL;
        return;
    }

    c->state = 1;
    g_mods[g_mod_n].handle = c->handle;
    g_mods[g_mod_n].name = c->info->name;
    g_mods[g_mod_n].version = c->info->version;
    g_mod_n++;
    reg_mark_free(&mark);
}

/* ============ 依赖描述解析 ============ */

/*
 * requires 支持两种写法：
 *   1) "Core"               只要求模组 Core 加载成功；
 *   2) "Core:物品/粗粮"      还要求 Core 注册了“物品/粗粮”这条内容。
 * 种类可用中文或英文：特质/物品/宠物类型/成就/剧情/事件/小游戏/术语/NPC/菜单/配置。
 */

typedef enum {
    DEP_OK = 0,
    DEP_ERR_FORMAT,
    DEP_ERR_KIND
} DepParseResult;

typedef struct {
    char module[128];
    int has_kind;
    ContentKind_t kind;
    char content[128];
} DepSpec;

static int dep_label_kind(const char *s, size_t n, ContentKind_t *out)
{
    static const struct { const char *name; ContentKind_t kind; } MAP[] = {
        {"特质",       KIND_TRAIT},
        {"trait",      KIND_TRAIT},
        {"物品",       KIND_ITEM},
        {"item",       KIND_ITEM},
        {"宠物类型",   KIND_PET_TYPE},
        {"宠物",       KIND_PET_TYPE},
        {"pet",        KIND_PET_TYPE},
        {"成就",       KIND_ACHIEVEMENT},
        {"achievement", KIND_ACHIEVEMENT},
        {"剧情",       KIND_STORY},
        {"story",      KIND_STORY},
        {"事件",       KIND_EVENT},
        {"随机事件",   KIND_EVENT},
        {"event",      KIND_EVENT},
        {"小游戏",     KIND_MINIGAME},
        {"minigame",   KIND_MINIGAME},
        {"术语",       KIND_TERM},
        {"term",       KIND_TERM},
        {"NPC",        KIND_NPC},
        {"npc",        KIND_NPC},
        {"菜单",       KIND_MENU},
        {"menu",       KIND_MENU},
        {"配置",       KIND_CONFIG},
        {"config",     KIND_CONFIG}
    };
    uint32_t i;

    for (i = 0; i < (uint32_t)(sizeof(MAP) / sizeof(MAP[0])); i++)
    {
        if (strlen(MAP[i].name) == n && _strnicmp(s, MAP[i].name, n) == 0)
        {
            *out = MAP[i].kind;
            return 1;
        }
    }
    return 0;
}

/* 去掉首尾空白后拷进定长缓冲；空串或超长返回 0 */
static int dep_take(char *dst, size_t cap, const char *src, size_t len)
{
    while (len > 0 && (src[0] == ' ' || src[0] == '\t')) { src++; len--; }
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
    if (len == 0 || len >= cap) return 0;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 1;
}

static DepParseResult dep_parse(const char *spec, DepSpec *out, char *err, size_t cap)
{
    const char *colon, *slash, *kind_s;
    size_t len;

    memset(out, 0, sizeof(*out));
    if (!spec || !spec[0])
    {
        _snprintf(err, cap, "依赖名为空");
        return DEP_ERR_FORMAT;
    }

    colon = strchr(spec, ':');
    if (!colon)
    {
        if (!dep_take(out->module, sizeof(out->module), spec, strlen(spec)))
        {
            _snprintf(err, cap, "「%s」的模组名为空或过长", spec);
            return DEP_ERR_FORMAT;
        }
        return DEP_OK;
    }

    if (!dep_take(out->module, sizeof(out->module), spec, (size_t)(colon - spec)))
    {
        _snprintf(err, cap, "「%s」缺少模组名（应写成 模组名:种类/名字，例如 Core:物品/粗粮）", spec);
        return DEP_ERR_FORMAT;
    }

    slash = strchr(colon + 1, '/');
    if (!slash)
    {
        _snprintf(err, cap,
                  "「%s」缺少 /（应写成 模组名:种类/名字，例如 Core:物品/粗粮）", spec);
        return DEP_ERR_FORMAT;
    }

    kind_s = colon + 1;
    len = (size_t)(slash - kind_s);
    {
        char kindbuf[32];
        if (!dep_take(kindbuf, sizeof(kindbuf), kind_s, len))
        {
            _snprintf(err, cap, "「%s」缺少种类（应写成 模组名:种类/名字）", spec);
            return DEP_ERR_FORMAT;
        }
        if (!dep_label_kind(kindbuf, strlen(kindbuf), &out->kind))
        {
            _snprintf(err, cap,
                      "「%s」里的种类「%s」不认识；可用：特质/物品/宠物类型/成就/剧情/事件/小游戏/术语/NPC/菜单/配置",
                      spec, kindbuf);
            return DEP_ERR_KIND;
        }
        out->has_kind = 1;
    }

    if (!dep_take(out->content, sizeof(out->content), slash + 1, strlen(slash + 1)))
    {
        _snprintf(err, cap, "「%s」缺少内容名（应写成 模组名:种类/名字）", spec);
        return DEP_ERR_FORMAT;
    }

    return DEP_OK;
}

/* 阶段 2：依赖就绪 + 依赖内容存在，才初始化（候选先按文件名排序） */
static void cand_resolve(void)
{
    int progress = 1;

    while (progress)
    {
        uint64_t i, r;
        progress = 0;

        for (i = 0; i < g_cand_n; i++)
        {
            ModCand *c = &g_cand[i];
            char reason[160];
            char detail[320];
            int ready = 1;
            int failed = 0;

            if (c->state != 0) continue;
            reason[0] = '\0';
            detail[0] = '\0';

            for (r = 0; r < c->info->require_count && !failed; r++)
            {
                const char *req = c->info->requires[r];
                DepSpec dep;
                char derr[200];
                DepParseResult pr;
                ModCand *d;

                if (!req || !req[0]) continue;

                pr = dep_parse(req, &dep, derr, sizeof(derr));
                if (pr != DEP_OK)
                {
                    _snprintf(reason, sizeof(reason), "%s",
                              (pr == DEP_ERR_KIND) ? "依赖种类未知" : "依赖格式错误");
                    _snprintf(detail, sizeof(detail), "require \"%s\"：%s", req, derr);
                    failed = 1;
                    break;
                }

                d = cand_by_name(dep.module);
                if (!d)
                {
                    _snprintf(reason, sizeof(reason), "缺少依赖模组 %s", dep.module);
                    _snprintf(detail, sizeof(detail),
                              "没有找到名为 \"%s\" 的模组；检查 mods\\ 目录、DLL 文件名，或对方的 mod_info.name",
                              dep.module);
                    failed = 1;
                    break;
                }
                if (d->state == 2)
                {
                    _snprintf(reason, sizeof(reason), "依赖的模组 %s 加载失败", dep.module);
                    _snprintf(detail, sizeof(detail),
                              "依赖的模组 \"%s\" 没有加载成功（见它自己的错误条目）", dep.module);
                    failed = 1;
                    break;
                }
                if (d->state == 0)
                {
                    ready = 0;
                    continue;
                }

                /* 依赖模组已经初始化完，再检查它是否真的注册了指定的内容 */
                if (dep.has_kind)
                {
                    Id cid = reg_find_owned(dep.kind, dep.content, dep.module);
                    if (cid == ID_NONE)
                    {
                        Id any = reg_find(dep.kind, dep.content);
                        if (any == ID_NONE)
                        {
                            _snprintf(reason, sizeof(reason),
                                      "依赖内容缺失（%s %s）", kind_label(dep.kind), dep.content);
                            _snprintf(detail, sizeof(detail),
                                      "模组 %s 已加载，但没有注册%s「%s」；检查内容名是否拼错",
                                      dep.module, kind_label(dep.kind), dep.content);
                        }
                        else
                        {
                            const char *owner = reg_owner(dep.kind, any);
                            _snprintf(reason, sizeof(reason), "依赖内容不属于该模组");
                            _snprintf(detail, sizeof(detail),
                                      "%s「%s」实际由 %s 注册，而依赖要求 %s",
                                      kind_label(dep.kind), dep.content,
                                      owner ? owner : "未知模组", dep.module);
                        }
                        failed = 1;
                        break;
                    }
                }
            }

            reason[sizeof(reason) - 1] = '\0';
            detail[sizeof(detail) - 1] = '\0';

            if (failed)
            {
                cand_fail(c, reason, detail);
                progress = 1;
                continue;
            }
            if (ready)
            {
                cand_init(c);
                progress = 1;
            }
        }
    }

    /* 仍处于等待状态的：依赖成环，永远等不到 */
    {
        uint64_t i;
        size_t need = 1;
        char *names;
        char detail[320];

        for (i = 0; i < g_cand_n; i++)
            if (g_cand[i].state == 0 && g_cand[i].name)
                need += strlen(g_cand[i].name) + 2;
        if (need == 1) return;

        names = (char *)calloc(need, 1);
        if (!names) return;

        for (i = 0; i < g_cand_n; i++)
        {
            if (g_cand[i].state != 0 || !g_cand[i].name) continue;
            if (names[0]) strcat(names, ", ");
            strcat(names, g_cand[i].name);
        }

        _snprintf(detail, sizeof(detail),
                  "依赖成环，无法确定加载顺序：%s（检查各自的 requires 是否互相等待）", names);
        detail[sizeof(detail) - 1] = '\0';
        free(names);

        /* 统一用完整的环成员列表报错，再逐个失败 */
        for (i = 0; i < g_cand_n; i++)
            if (g_cand[i].state == 0) cand_fail(&g_cand[i], "依赖循环", detail);
    }
}

/* ============ 错误报告 ============ */

static void print_error_line(const ModError_t *e)
{
    printf("  [%s] %s", e->warning ? "警告" : "错误",
           (e->mod && e->mod[0]) ? e->mod : "?");
    if (e->file && e->file[0] && (!e->mod || strcmp(e->file, e->mod) != 0))
        printf(" (%s)", e->file);
    printf("：%s\n", (e->reason && e->reason[0]) ? e->reason : "未知");
    if (e->detail && e->detail[0]) printf("        %s\n", e->detail);
}

static void print_load_report(void)
{
    uint64_t i;

    if (g_error_n == 0)
    {
        printf("\n[模组] %llu 个模组已加载，依赖检查通过。\n", (unsigned long long)g_mod_n);
    }
    else
    {
        printf("\n[模组] 成功 %llu 个，发现 %llu 条问题：\n",
               (unsigned long long)g_mod_n, (unsigned long long)g_error_n);
        for (i = 0; i < g_error_n; i++) print_error_line(&g_errors[i]);
    }
    fflush(stdout);
}

uint64_t modloader_error_count(void) { return g_error_n; }

const ModError_t *modloader_error_at(uint64_t index)
{
    return (index < g_error_n) ? &g_errors[index] : NULL;
}

void modloader_print_errors(const char *title)
{
    uint64_t i;

    printf("\n%s：", title ? title : "模组诊断");
    if (g_error_n == 0) { printf("没有错误\n"); fflush(stdout); return; }
    printf("%llu 条\n", (unsigned long long)g_error_n);
    for (i = 0; i < g_error_n; i++) print_error_line(&g_errors[i]);
    fflush(stdout);
}

static int svc_save(Pet *pet) { return modloader_save(pet); }
static int svc_load(Pet *pet) { return modloader_load(pet); }

static int filename_cmp(const void *a, const void *b)
{
    return _stricmp(*(char *const *)a, *(char *const *)b);
}

uint64_t modloader_init(const char *mods_dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE find;
    char *pattern;
    char **files = NULL;
    uint64_t n_files = 0, files_cap = 0;
    uint64_t i;
    size_t pattern_len;

    if (!mods_dir) return UINT64_MAX;

    srand((unsigned int)time(NULL));
    SetConsoleOutputCP(CP_UTF8);
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleMode(hOut, &mode))
        {
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            g_has_vt = 1;
        }
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) g_console_attr = csbi.wAttributes;
    }

    /* 错误表/候选表每次初始化都从干净状态开始 */
    err_rollback(0);
    g_cand_n = 0;

    /* 配置值要在模组注册配置项之前读回（按 name 匹配，与描述符无关） */
    cfg_load();

    /* 接口表：全部指向本文件的实现 */
    g_api.size = (uint32_t)sizeof(LoaderApi);
    g_api.version = LOADER_API_VERSION;
    g_api.add = reg_add;
    g_api.get = reg_get;
    g_api.find = reg_find;
    g_api.count = reg_count;
    g_api.add_names = name_add;
    g_api.name_get = name_get;
    g_api.name_count = name_count;
    g_api.rnd = svc_rnd;
    g_api.rnd64 = svc_rnd64;
    g_api.rand_name = svc_rand_name;
    g_api.gen_traits = svc_gen_traits;
    g_api.inv_add = svc_inv_add;
    g_api.inv_count = svc_inv_count;
    g_api.inv_rm = svc_inv_rm;
    g_api.inv_set = svc_inv_set;
    g_api.passive_try = svc_passive_try;
    g_api.damage = svc_damage;
    g_api.ui_title = ui_title;
    g_api.ui_print = ui_print;
    g_api.ui_ok = ui_ok;
    g_api.ui_err = ui_err;
    g_api.ui_warn = ui_warn;
    g_api.ui_menu = ui_menu;
    g_api.opt_text = opt_text;
    g_api.key = ui_key;
    g_api.delay = ui_delay;
    g_api.dialog = ui_dialog;
    g_api.ach_unlock = svc_ach_unlock;
    g_api.ach_unlocked = svc_ach_unlocked;
    g_api.ach_claimed = svc_ach_claimed;
    g_api.ach_claim = svc_ach_claim;
    g_api.ach_merge_global = svc_ach_merge_global;
    g_api.story_seen = svc_story_seen;
    g_api.story_mark = svc_story_mark;
    g_api.exp_add = svc_exp_add;
    g_api.rtc_day = svc_rtc_day;
    g_api.rtc_night = svc_rtc_night;
    g_api.trait_conflict = svc_trait_conflict;
    g_api.menu_run = svc_menu_run;
    g_api.story_play = modloader_story_play;
    g_api.ach_poll = modloader_ach_poll;
    g_api.minigame_list = modloader_minigame_list;
    g_api.minigame_run = modloader_minigame_run;
    g_api.inv_pick = svc_inv_pick;
    g_api.save = svc_save;
    g_api.load = svc_load;
    g_api.key_poll = svc_key_poll;
    g_api.now_ms = svc_now_ms;
    g_api.is_headless = modloader_is_headless;
    g_api.config_get = cfg_get;
    g_api.config_set = cfg_set;
    g_api.ui_option = ui_option;
    g_api.name_pick = svc_name_pick;
    g_api.trait_shop = svc_trait_shop;

    pattern_len = strlen(mods_dir) + 8;
    pattern = (char *)malloc(pattern_len);
    if (!pattern) return UINT64_MAX;
    _snprintf(pattern, pattern_len, "%s\\*.dll", mods_dir);

    find = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return UINT64_MAX;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_stricmp(fd.cFileName, "ModLoader.dll") == 0) continue;
        if (!vec_grow((void **)&files, &files_cap, n_files + 1, sizeof(*files))) break;
        files[n_files] = err_strdup(fd.cFileName);
        if (files[n_files]) n_files++;
    } while (FindNextFileA(find, &fd));
    FindClose(find);

    /* 按文件名排序：依赖同层时的初始化顺序稳定 */
    qsort(files, (size_t)n_files, sizeof(*files), filename_cmp);

    for (i = 0; i < n_files; i++)
    {
        cand_load(mods_dir, files[i]);
        free(files[i]);
    }
    free(files);

    cand_check_duplicates();
    cand_resolve();

    dead_load();       /* 读回逝去记录 */
    ach_global_load(); /* 读回跨存档成就 */

    print_load_report();
    return g_mod_n;
}

void modloader_pet_release(Pet *pet)
{
    if (!pet) return;
    free(pet->traits);
    free(pet->inv);
    pet->traits = NULL;
    pet->inv = NULL;
    pet->trait_cnt = pet->trait_cap = 0;
    pet->inv_cnt = pet->inv_cap = 0;
    bs_free(&pet->ach);
    bs_free(&pet->ach_claim);
    bs_free(&pet->story_flag);
    owned_free_from(0); /* 释放加载时分配的字符串 */
}

void modloader_shutdown(void)
{
    uint64_t i;
    uint32_t k;

    for (i = 0; i < g_dead_n; i++) dead_free(&g_dead[i]);
    g_dead_n = 0;

    err_rollback(0); /* 错误表里的字符串也是堆内存 */
    terms_free();
    bs_free(&g_ach_all);
    free(g_errors);
    g_errors = NULL;
    g_error_cap = 0;

    for (i = 0; i < g_mod_n; i++)
    {
        void (*p_exit)(void) = (void (*)(void))GetProcAddress(g_mods[i].handle, "mod_exit");
        if (p_exit) p_exit();
        FreeLibrary(g_mods[i].handle);
    }
    free(g_mods);
    g_mods = NULL;
    g_mod_n = g_mod_cap = 0;

    for (i = 0; i < g_cand_n; i++) cand_clear_strings(&g_cand[i]);
    free(g_cand);
    g_cand = NULL;
    g_cand_n = g_cand_cap = 0;

    for (k = 0; k < (uint32_t)KIND_COUNT; k++)
    {
        free((void *)g_list[k]);
        free((void *)g_owner[k]);
        g_list[k] = NULL;
        g_owner[k] = NULL;
        g_n[k] = g_cap[k] = 0;
    }
    for (k = 0; k < 3; k++)
    {
        free((void *)g_names[k]);
        g_names[k] = NULL;
        g_names_n[k] = g_names_cap[k] = 0;
    }
    for (i = 0; i < g_cfg_n; i++) free(g_cfg[i].name);
    free(g_cfg);
    g_cfg = NULL;
    g_cfg_n = g_cfg_cap = 0;

    free(g_owned);
    g_owned = NULL;
    g_owned_n = g_owned_cap = 0;
}

uint64_t modloader_mod_count(void) { return g_mod_n; }

const char *modloader_mod_name(uint64_t index)
{
    return (index < g_mod_n) ? g_mods[index].name : NULL;
}

/* ============ 内容访问 ============ */

Id modloader_find(ContentKind_t kind, const char *name) { return reg_find(kind, name); }
const void *modloader_get(ContentKind_t kind, Id id) { return reg_get(kind, id); }
uint64_t modloader_count(ContentKind_t kind) { return reg_count(kind); }

const char *modloader_name_of(ContentKind_t kind, Id id)
{
    return kind_name(kind, reg_get(kind, id));
}

uint64_t modloader_name_count(Gender_t gender) { return name_count(gender); }
const char *modloader_name_at(Gender_t gender, uint64_t index) { return name_get(gender, index); }

/* ============ 内容驱动 ============ */

int modloader_ach_poll(Pet *pet)
{
    uint64_t i, n;
    int unlocked = 0;

    if (!pet) return 0;
    n = reg_count(KIND_ACHIEVEMENT);

    for (i = 1; i <= n; i++)
    {
        const Achievement_t *a = (const Achievement_t *)reg_get(KIND_ACHIEVEMENT, (Id)i);
        if (!a || !a->on_check) continue;
        if (bs_test(&pet->ach, (Id)i)) continue;
        if (a->on_check(pet)) unlocked += svc_ach_unlock(pet, (Id)i);
    }
    return unlocked;
}

int modloader_story_play(Pet *pet, const char *scene, const char *npc)
{
    uint64_t i, n;

    if (!pet) return 0;
    g_pet_ctx = pet;
    n = reg_count(KIND_STORY);

    for (i = 1; i <= n; i++)
    {
        const Story_t *st = (const Story_t *)reg_get(KIND_STORY, (Id)i);
        uint32_t d;
        int choice = 0;
        const char *speaker = "";

        if (!st) continue;
        if (st->scene && (!scene || strcmp(st->scene, scene) != 0)) continue;
        if (bs_test(&pet->story_flag, (Id)i)) continue;
        if (st->on_check && !st->on_check(pet)) continue;

        uint32_t matched = 0;
        for (d = 0; d < st->dialog_count; d++)
        {
            const Dialog_t *dl = &st->dialogs[d];
            if (dl->npc && (!npc || strcmp(dl->npc, npc) != 0)) continue;
            matched++;
            if (dl->speaker) speaker = dl->speaker;
            ui_dialog(dl->speaker, dl->pages, dl->page_count, NULL, 0);
        }
        /* 所有对白都不匹配当前 NPC（比如当值员工不是小春）：不算播过 */
        if (st->dialog_count > 0 && matched == 0) continue;

        if (st->opts && st->opt_count > 0)
            choice = ui_dialog(speaker, NULL, 0, st->opts, st->opt_count);

        bs_set(&pet->story_flag, (Id)i);
        if (st->on_play) st->on_play(pet, choice);
        return 1;
    }
    return 0;
}

int modloader_event_roll(Pet *pet)
{
    uint64_t i, n, total = 0, acc = 0, pick;
    const Event_t *chosen = NULL;

    if (!pet) return 0;
    g_pet_ctx = pet;
    n = reg_count(KIND_EVENT);

    for (i = 1; i <= n; i++)
    {
        const Event_t *e = (const Event_t *)reg_get(KIND_EVENT, (Id)i);
        if (e) total += e->weight;
    }
    if (total == 0) return 0;

    pick = svc_rnd64(0, total - 1);
    for (i = 1; i <= n; i++)
    {
        const Event_t *e = (const Event_t *)reg_get(KIND_EVENT, (Id)i);
        if (!e || e->weight == 0) continue;
        if (pick < acc + e->weight) { chosen = e; break; }
        acc += e->weight;
    }
    if (!chosen) return 0;

    if (chosen->text && chosen->text[0])
    {
        const char *pages[1];
        pages[0] = chosen->text;
        ui_dialog("随机事件", pages, 1, NULL, 0);
    }
    else
    {
        ui_title("随机事件");
    }
    if (chosen->on_fire) chosen->on_fire(pet);
    if (!g_headless && (!chosen->text || !chosen->text[0]))
    {
        ui_print("\nZ:继续\n");
        ui_key();
    }
    return 1;
}

uint64_t modloader_minigame_list(Id pet_type, Id *out, uint64_t max)
{
    const PetType *type = (const PetType *)reg_get(KIND_PET_TYPE, pet_type);
    const char *type_name = type ? type->name : NULL;
    uint64_t i, n, k = 0;

    n = reg_count(KIND_MINIGAME);
    for (i = 1; i <= n && k < max; i++)
    {
        const Minigame_t *g = (const Minigame_t *)reg_get(KIND_MINIGAME, (Id)i);
        if (!g) continue;
        if (g->pet_type && (!type_name || strcmp(g->pet_type, type_name) != 0)) continue;
        if (out) out[k] = (Id)i;
        k++;
    }
    return k;
}

int modloader_minigame_run(Pet *pet, Id game)
{
    const Minigame_t *g = (const Minigame_t *)reg_get(KIND_MINIGAME, game);
    if (!pet || !g || !g->run) return 0;
    g_pet_ctx = pet;
    return g->run(pet);
}

/* ============ 服务导出 ============ */

void ml_ui_title(const char *title) { ui_title(title); }
void ml_ui_print(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}
int ml_key(void) { return ui_key(); }
void ml_delay(uint32_t ms) { ui_delay(ms); }
int ml_dialog(const char *speaker, const char *const *pages, uint32_t n_pages,
              const StoryOpt_t *opts, uint32_t n_opts)
{
    return ui_dialog(speaker, pages, n_pages, opts, n_opts);
}
uint32_t ml_rnd(uint32_t lo, uint32_t hi) { return svc_rnd(lo, hi); }
int ml_inv_add(Pet *pet, Id item, uint64_t count) { return svc_inv_add(pet, item, count); }
uint64_t ml_inv_count(const Pet *pet, Id item) { return svc_inv_count(pet, item); }
int ml_passive_try(Pet *pet) { return svc_passive_try(pet); }
int ml_damage(Pet *pet, uint32_t amount) { return svc_damage(pet, amount); }
int ml_ach_unlock(Pet *pet, Id ach) { return svc_ach_unlock(pet, ach); }
void ml_exp_add(Pet *pet, int amount) { svc_exp_add(pet, amount); }
const LoaderApi *ml_api(void) { return &g_api; }

const char *modloader_name_pick(Gender_t gender, const char *title)
{
    return svc_name_pick(gender, title);
}

int modloader_trait_shop(Pet *pet, const char *title, int allow_delete,
                         uint32_t cost_random, uint32_t cost_pick, uint32_t cost_delete)
{
    return svc_trait_shop(pet, title, allow_delete, cost_random, cost_pick, cost_delete);
}

int32_t modloader_config_get(const char *name) { return cfg_get(name); }

int modloader_config_set(const char *name, int32_t value) { return cfg_set(name, value); }

void modloader_set_config_file(const char *path)
{
    if (!path) return;
    strncpy(g_cfg_file, path, sizeof(g_cfg_file) - 1);
    g_cfg_file[sizeof(g_cfg_file) - 1] = '\0';
}

void modloader_set_headless(int on) { g_headless = on ? 1 : 0; }

int modloader_is_headless(void) { return g_headless; }

void modloader_set_dead_file(const char *path)
{
    if (!path) return;
    strncpy(g_dead_file, path, sizeof(g_dead_file) - 1);
    g_dead_file[sizeof(g_dead_file) - 1] = '\0';
}
