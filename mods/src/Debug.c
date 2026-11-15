/* ============================================================
 * EPets · 控制台模组
 *   开发用「控制台」：命令式修改器，取代原来的调试选项菜单
 *
 *   命令：help / pet / bag / coins / exp / full / stat / heal /
 *         item / itemid / randitem / trait / list / find /
 *         config / story / ach / age / level / sleep / rebel /
 *         save / clear / quit
 *
 *   只注册 KIND_MENU；Core 提供物品/特质名字。
 *   编译: cl /LD /utf-8 /W4 /I..\mods\incl Debug.c /Fe:Debug.dll
 * ============================================================ */

#include "Defineds.h"
#include "Loader.h"

#include <conio.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const LoaderApi *g_api;
static char g_err[192];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

/* ============ 基础工具 ============ */

static char *next_token(char **pp)
{
    char *p = *pp;
    char *start;

    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *pp = p; return NULL; }

    start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = '\0';
    *pp = p;
    return start;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !s[0]) return 0;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || !end || *end != '\0' || end == s) return 0;
    *out = (uint64_t)v;
    return 1;
}

static int parse_i64(const char *s, int64_t *out)
{
    char *end = NULL;
    long long v;

    if (!s || !s[0]) return 0;
    errno = 0;
    v = strtoll(s, &end, 0);
    if (errno != 0 || !end || *end != '\0' || end == s) return 0;
    *out = (int64_t)v;
    return 1;
}

static uint8_t clamp_u8(uint64_t v)
{
    return (v > MAX_STAT) ? (uint8_t)MAX_STAT : (uint8_t)v;
}

/* ============ 内容名字 ============ */

typedef struct {
    const char *name;
    ContentKind_t kind;
} KindName;

static const KindName KINDS[] = {
    {"trait", KIND_TRAIT}, {"traits", KIND_TRAIT}, {"特质", KIND_TRAIT},
    {"item", KIND_ITEM}, {"items", KIND_ITEM}, {"物品", KIND_ITEM},
    {"type", KIND_PET_TYPE}, {"types", KIND_PET_TYPE}, {"pet", KIND_PET_TYPE}, {"宠物", KIND_PET_TYPE},
    {"ach", KIND_ACHIEVEMENT}, {"achievements", KIND_ACHIEVEMENT}, {"成就", KIND_ACHIEVEMENT},
    {"story", KIND_STORY}, {"stories", KIND_STORY}, {"剧情", KIND_STORY},
    {"event", KIND_EVENT}, {"events", KIND_EVENT}, {"事件", KIND_EVENT},
    {"game", KIND_MINIGAME}, {"games", KIND_MINIGAME}, {"minigame", KIND_MINIGAME}, {"小游戏", KIND_MINIGAME},
    {"term", KIND_TERM}, {"terms", KIND_TERM}, {"术语", KIND_TERM},
    {"npc", KIND_NPC}, {"npcs", KIND_NPC},
    {"menu", KIND_MENU}, {"menus", KIND_MENU}, {"菜单", KIND_MENU},
    {"config", KIND_CONFIG}, {"configs", KIND_CONFIG}, {"配置", KIND_CONFIG}
};

static const char *kind_label(ContentKind_t kind)
{
    switch (kind)
    {
    case KIND_TRAIT: return "特质";
    case KIND_ITEM: return "物品";
    case KIND_PET_TYPE: return "宠物类型";
    case KIND_ACHIEVEMENT: return "成就";
    case KIND_STORY: return "剧情";
    case KIND_EVENT: return "事件";
    case KIND_MINIGAME: return "小游戏";
    case KIND_TERM: return "术语";
    case KIND_NPC: return "NPC";
    case KIND_MENU: return "菜单";
    case KIND_CONFIG: return "配置";
    default: return "内容";
    }
}

static ContentKind_t kind_from(const char *s)
{
    uint64_t i;
    if (!s || !s[0]) return KIND_COUNT;
    for (i = 0; i < (uint64_t)(sizeof(KINDS) / sizeof(KINDS[0])); i++)
        if (_stricmp(KINDS[i].name, s) == 0) return KINDS[i].kind;
    return KIND_COUNT;
}

static const char *content_name(ContentKind_t kind, const void *p)
{
    if (!p) return "?";
    switch (kind)
    {
    case KIND_TRAIT: return ((const Trait_t *)p)->name;
    case KIND_ITEM: return ((const Item *)p)->name;
    case KIND_PET_TYPE: return ((const PetType *)p)->name;
    case KIND_ACHIEVEMENT: return ((const Achievement_t *)p)->name;
    case KIND_STORY: return ((const Story_t *)p)->name;
    case KIND_EVENT: return ((const Event_t *)p)->name;
    case KIND_MINIGAME: return ((const Minigame_t *)p)->name;
    case KIND_TERM: return ((const TermDef_t *)p)->name;
    case KIND_NPC: return ((const Npc_t *)p)->name;
    case KIND_MENU: return ((const Menu_t *)p)->name;
    case KIND_CONFIG: return ((const Config_t *)p)->name;
    default: return "?";
    }
}

/* ============ 宠物输出 ============ */

static const char *gender_label(Gender_t g)
{
    return (g == G_MALE) ? "男" : ((g == G_FEMALE) ? "女" : "中性");
}

static const char *stage_label(Stage_t s)
{
    return (s == S_BABY) ? "婴儿" : ((s == S_CHILD) ? "幼年" : "成年");
}

static void print_pet(const Pet *pet)
{
    const PetType *t = (const PetType *)g_api->get(KIND_PET_TYPE, pet->type_id);
    uint64_t i;

    g_api->ui_print("\n名字: %s\n", pet->name ? pet->name : "?");
    g_api->ui_print("类型: %s  性别: %s  年龄: %u 天  阶段: %s\n",
                    t && t->name ? t->name : "?", gender_label(pet->gender),
                    (unsigned)pet->age, stage_label(pet->stage));
    g_api->ui_print("属性: 饱食%u 清洁%u 心情%u 健康%u 活力%u\n",
                    pet->hunger, pet->clean, pet->happy, pet->health, pet->energy);
    g_api->ui_print("资源: 鲁尼%u 经验%u 等级%u 登录%u 天\n",
                    pet->coins, pet->exp, (unsigned)pet->level, (unsigned)pet->login_days);
    g_api->ui_print("状态: 疾病%u 睡眠%d 叛逆%d 死亡%d\n",
                    (unsigned)pet->sick, pet->sleeping ? 1 : 0,
                    pet->rebellious ? 1 : 0, pet->dead ? 1 : 0);

    g_api->ui_print("特质(%llu):", (unsigned long long)pet->trait_cnt);
    for (i = 0; i < pet->trait_cnt; i++)
    {
        const Trait_t *tr = (const Trait_t *)g_api->get(KIND_TRAIT, pet->traits[i]);
        if (tr && tr->name) g_api->ui_print(" %s", tr->name);
    }
    g_api->ui_print("\n背包: %llu 格\n", (unsigned long long)pet->inv_cnt);
}

static void print_bag(const Pet *pet)
{
    uint64_t i;

    g_api->ui_print("\n背包（%llu 格）:\n", (unsigned long long)pet->inv_cnt);
    if (pet->inv_cnt == 0)
    {
        g_api->ui_print("  （空）\n");
        return;
    }
    for (i = 0; i < pet->inv_cnt; i++)
    {
        const Item *it = (const Item *)g_api->get(KIND_ITEM, pet->inv[i].id);
        g_api->ui_print("  %-16s x%llu\n",
                        it && it->name ? it->name : "?",
                        (unsigned long long)pet->inv[i].count);
    }
}

static void list_kind(ContentKind_t kind)
{
    uint64_t n = g_api->count(kind), i;

    g_api->ui_print("\n%s（%llu）:\n", kind_label(kind), (unsigned long long)n);
    for (i = 1; i <= n; i++)
    {
        const void *p = g_api->get(kind, (Id)i);
        if (!p) continue;
        if (kind == KIND_CONFIG)
        {
            const Config_t *c = (const Config_t *)p;
            g_api->ui_print("  %-4llu %-28s = %d\n", (unsigned long long)i,
                            c->name ? c->name : "?", (int)g_api->config_get(c->name));
        }
        else
        {
            g_api->ui_print("  %-4llu %s\n", (unsigned long long)i, content_name(kind, p));
        }
    }
}

/* ============ 控制台输入 ============ */

static char *console_read_line(void)
{
    size_t cap = 128, len = 0;
    char *buf = (char *)malloc(cap);
    int c;

    if (!buf) return NULL;
    g_api->ui_print("\n控制台> ");
    fflush(stdout);

    for (;;)
    {
        c = _getch();
        if (c < 0) { free(buf); return NULL; }
        if (c == 3 || c == 26) { g_api->ui_print("\n"); free(buf); return NULL; }
        if (c == 27) { g_api->ui_print("\n"); free(buf); return NULL; }
        if (c == '\r' || c == '\n') { g_api->ui_print("\n"); break; }
        if (c == 8 || c == 127)
        {
            if (len > 0)
            {
                len--;
                g_api->ui_print("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (c == 0 || c == 0xE0)
        {
            (void)_getch(); /* 忽略方向键/功能键 */
            continue;
        }
        if (c < 32) continue;

        if (len + 2 > cap)
        {
            size_t ncap = cap * 2;
            char *p = (char *)realloc(buf, ncap);
            if (!p) { free(buf); return NULL; }
            buf = p;
            cap = ncap;
        }
        buf[len++] = (char)c;
        g_api->ui_print("%c", c);
        fflush(stdout);
    }

    buf[len] = '\0';
    return buf;
}

/* ============ 命令实现 ============ */

static void print_help(void)
{
    g_api->ui_print(
        "\n命令:\n"
        "  help / ?                显示本帮助\n"
        "  pet                     宠物摘要\n"
        "  bag                     背包列表\n"
        "  coins [n]               加鲁尼（默认 1000）\n"
        "  exp [n]                 加经验（默认 500）\n"
        "  full                    五维全满（255）\n"
        "  stat [名字] [值]        查看/设置属性，名字：hunger clean happy health energy\n"
        "  heal                    清除疾病\n"
        "  age [n]                 查看/设置年龄\n"
        "  level [n]               查看/设置等级\n"
        "  sleep                   切换睡眠\n"
        "  rebel                   切换叛逆\n"
        "  item <名字|Id> [数量]   添加物品（名字可用 list items 查）\n"
        "  itemid <Id> [数量]      按 Id 添加物品\n"
        "  randitem [数量]         随机有价物品（默认 5）\n"
        "  trait <名字|Id>         添加特质（不检查互斥）\n"
        "  list [种类]             列出内容，默认 items\n"
        "  find <种类> <名字>      按名字查 Id\n"
        "  config <名字> [值]      读取/设置配置\n"
        "  story <场景>            播放剧情（mall/hospital/...）\n"
        "  ach                     轮询成就\n"
        "  save                    立即存档\n"
        "  clear                   清屏\n"
        "  quit / exit / q         退出控制台\n"
        "\n种类: traits items types ach story events games terms npcs menus configs\n");
}

static Id find_item_arg(const char *arg)
{
    Id id;
    uint64_t v;

    if (!arg || !arg[0]) return ID_NONE;
    id = g_api->find(KIND_ITEM, arg);
    if (id != ID_NONE) return id;

    if (parse_u64(arg, &v) && v >= 1 && v <= g_api->count(KIND_ITEM))
        return (Id)v;
    return ID_NONE;
}

static int add_trait(Pet *pet, Id id)
{
    uint64_t cap;

    if (pet->trait_cnt == pet->trait_cap)
    {
        cap = pet->trait_cap ? pet->trait_cap * 2 : 4;
        if (cap <= pet->trait_cnt) cap = pet->trait_cnt + 1;
        if (cap > (uint64_t)(SIZE_MAX / sizeof(Id))) return 0;
        {
            Id *p = (Id *)realloc(pet->traits, (size_t)cap * sizeof(Id));
            if (!p) return 0;
            pet->traits = p;
            pet->trait_cap = cap;
        }
    }
    pet->traits[pet->trait_cnt++] = id;
    return 1;
}

static void auto_save(Pet *pet)
{
    (void)g_api->save(pet);
}

/* 返回 0 = 退出控制台，1 = 继续 */
static int console_exec(Pet *pet, const char *line)
{
    size_t n = strlen(line);
    char *work = (char *)malloc(n + 1);
    char *p, *cmd, *a1, *a2;
    int result = 1;

    if (!work) return 1;
    memcpy(work, line, n + 1);
    p = work;
    cmd = next_token(&p);
    a1 = next_token(&p);
    a2 = next_token(&p);

    if (!cmd)
    {
        free(work);
        return 1;
    }

    if (_stricmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0)
    {
        print_help();
    }
    else if (_stricmp(cmd, "quit") == 0 || _stricmp(cmd, "exit") == 0 ||
             _stricmp(cmd, "q") == 0 || _stricmp(cmd, "x") == 0)
    {
        g_api->ui_print("\n离开控制台\n");
        result = 0;
    }
    else if (_stricmp(cmd, "pet") == 0)
    {
        print_pet(pet);
    }
    else if (_stricmp(cmd, "bag") == 0)
    {
        print_bag(pet);
    }
    else if (_stricmp(cmd, "coins") == 0 || _stricmp(cmd, "money") == 0)
    {
        uint64_t amount = 1000;
        if (a1 && !parse_u64(a1, &amount))
        {
            g_api->ui_err("  数量无效：%s", a1);
        }
        else
        {
            uint64_t room = (uint64_t)UINT32_MAX - pet->coins;
            if (amount > room) amount = room;
            pet->coins += (uint32_t)amount;
            g_api->ui_ok("  鲁尼 +%llu，现在 %u", (unsigned long long)amount, pet->coins);
            auto_save(pet);
        }
    }
    else if (_stricmp(cmd, "exp") == 0)
    {
        uint64_t amount = 500;
        if (a1 && !parse_u64(a1, &amount))
        {
            g_api->ui_err("  数量无效：%s", a1);
        }
        else
        {
            if (amount > (uint64_t)INT_MAX) amount = (uint64_t)INT_MAX;
            g_api->exp_add(pet, (int)amount);
            g_api->ui_ok("  经验 +%llu，当前 Lv.%u",
                         (unsigned long long)amount, (unsigned)pet->level);
            auto_save(pet);
        }
    }
    else if (_stricmp(cmd, "full") == 0)
    {
        pet->hunger = MAX_STAT;
        pet->clean = MAX_STAT;
        pet->happy = MAX_STAT;
        pet->health = MAX_STAT;
        pet->energy = MAX_STAT;
        g_api->ui_ok("  五维已全满（%u）", (unsigned)MAX_STAT);
        auto_save(pet);
    }
    else if (_stricmp(cmd, "stat") == 0)
    {
        static const struct { const char *name; size_t off; } STATS[] = {
            {"hunger", offsetof(Pet, hunger)}, {"饱食", offsetof(Pet, hunger)},
            {"clean", offsetof(Pet, clean)}, {"清洁", offsetof(Pet, clean)},
            {"happy", offsetof(Pet, happy)}, {"心情", offsetof(Pet, happy)},
            {"health", offsetof(Pet, health)}, {"健康", offsetof(Pet, health)},
            {"energy", offsetof(Pet, energy)}, {"活力", offsetof(Pet, energy)}
        };
        uint64_t i, found = UINT64_MAX;

        if (!a1)
        {
            g_api->ui_print("  hunger=%u clean=%u happy=%u health=%u energy=%u\n",
                            pet->hunger, pet->clean, pet->happy, pet->health, pet->energy);
        }
        else
        {
            for (i = 0; i < (uint64_t)(sizeof(STATS) / sizeof(STATS[0])); i++)
                if (_stricmp(STATS[i].name, a1) == 0) { found = i; break; }
            if (found == UINT64_MAX)
            {
                g_api->ui_err("  未知属性：%s（hunger/clean/happy/health/energy）", a1);
            }
            else
            {
                uint8_t *field = (uint8_t *)((char *)pet + STATS[found].off);
                if (!a2)
                {
                    g_api->ui_print("  %s = %u\n", a1, *field);
                }
                else
                {
                    uint64_t v;
                    if (!parse_u64(a2, &v))
                    {
                        g_api->ui_err("  数值无效：%s", a2);
                    }
                    else
                    {
                        *field = clamp_u8(v);
                        g_api->ui_ok("  %s = %u", a1, *field);
                        auto_save(pet);
                    }
                }
            }
        }
    }
    else if (_stricmp(cmd, "heal") == 0 || _stricmp(cmd, "cure") == 0)
    {
        pet->sick = D_NONE;
        g_api->ui_ok("  疾病已清除");
        auto_save(pet);
    }
    else if (_stricmp(cmd, "age") == 0)
    {
        if (!a1)
        {
            g_api->ui_print("  age = %u\n", (unsigned)pet->age);
        }
        else
        {
            uint64_t v;
            if (!parse_u64(a1, &v)) g_api->ui_err("  数值无效：%s", a1);
            else
            {
                pet->age = (v > UINT16_MAX) ? UINT16_MAX : (uint16_t)v;
                g_api->ui_ok("  age = %u", (unsigned)pet->age);
                auto_save(pet);
            }
        }
    }
    else if (_stricmp(cmd, "level") == 0)
    {
        if (!a1)
        {
            g_api->ui_print("  level = %u\n", (unsigned)pet->level);
        }
        else
        {
            uint64_t v;
            if (!parse_u64(a1, &v)) g_api->ui_err("  数值无效：%s", a1);
            else
            {
                pet->level = (v > UINT8_MAX) ? UINT8_MAX : (uint8_t)v;
                g_api->ui_ok("  level = %u", (unsigned)pet->level);
                auto_save(pet);
            }
        }
    }
    else if (_stricmp(cmd, "sleep") == 0)
    {
        pet->sleeping = pet->sleeping ? 0 : 1;
        g_api->ui_ok("  睡眠 = %s", pet->sleeping ? "开" : "关");
        auto_save(pet);
    }
    else if (_stricmp(cmd, "rebel") == 0)
    {
        pet->rebellious = pet->rebellious ? 0 : 1;
        g_api->ui_ok("  叛逆 = %s", pet->rebellious ? "开" : "关");
        auto_save(pet);
    }
    else if (_stricmp(cmd, "item") == 0 || _stricmp(cmd, "itemid") == 0)
    {
        Id id = ID_NONE;
        uint64_t amount = 1;

        if (!a1)
        {
            g_api->ui_err("  用法：item <名字|Id> [数量]");
        }
        else if (_stricmp(cmd, "itemid") == 0)
        {
            uint64_t v;
            if (parse_u64(a1, &v) && v >= 1 && v <= g_api->count(KIND_ITEM)) id = (Id)v;
        }
        else
        {
            id = find_item_arg(a1);
        }

        if (id == ID_NONE)
        {
            g_api->ui_err("  找不到物品：%s（list items 查看 Id/名字）", a1 ? a1 : "");
        }
        else if (a2 && !parse_u64(a2, &amount))
        {
            g_api->ui_err("  数量无效：%s", a2);
        }
        else if (amount == 0)
        {
            g_api->ui_err("  数量必须大于 0");
        }
        else
        {
            const Item *it = (const Item *)g_api->get(KIND_ITEM, id);
            if (!g_api->inv_add(pet, id, amount))
                g_api->ui_err("  添加失败（内存不足）");
            else
            {
                g_api->ui_ok("  添加 %s x%llu",
                             it && it->name ? it->name : "?",
                             (unsigned long long)amount);
                auto_save(pet);
            }
        }
    }
    else if (_stricmp(cmd, "randitem") == 0)
    {
        uint64_t amount = 5, count = g_api->count(KIND_ITEM), i;
        Id id = ID_NONE;

        if (a1 && !parse_u64(a1, &amount))
        {
            g_api->ui_err("  数量无效：%s", a1);
        }
        else if (amount == 0)
        {
            g_api->ui_err("  数量必须大于 0");
        }
        else
        {
            for (i = 0; i < 32 && count > 0; i++)
            {
                const Item *it;
                Id pick = (Id)g_api->rnd64(1, count);
                it = (const Item *)g_api->get(KIND_ITEM, pick);
                if (it && it->price > 0) { id = pick; break; }
            }
            if (id == ID_NONE)
            {
                g_api->ui_err("  没有找到有价物品");
            }
            else
            {
                const Item *it = (const Item *)g_api->get(KIND_ITEM, id);
                if (!g_api->inv_add(pet, id, amount))
                    g_api->ui_err("  添加失败（内存不足）");
                else
                {
                    g_api->ui_ok("  添加 %s x%llu",
                                 it && it->name ? it->name : "?",
                                 (unsigned long long)amount);
                    auto_save(pet);
                }
            }
        }
    }
    else if (_stricmp(cmd, "trait") == 0)
    {
        Id id = ID_NONE;
        uint64_t v;

        if (!a1)
        {
            g_api->ui_err("  用法：trait <名字|Id>");
        }
        else
        {
            id = g_api->find(KIND_TRAIT, a1);
            if (id == ID_NONE && parse_u64(a1, &v) && v >= 1 && v <= g_api->count(KIND_TRAIT))
                id = (Id)v;
        }

        if (id == ID_NONE)
        {
            g_api->ui_err("  找不到特质：%s（list traits 查看）", a1 ? a1 : "");
        }
        else
        {
            const Trait_t *tr = (const Trait_t *)g_api->get(KIND_TRAIT, id);
            if (!add_trait(pet, id))
                g_api->ui_err("  添加失败（内存不足）");
            else
            {
                g_api->ui_ok("  添加特质 %s", tr && tr->name ? tr->name : "?");
                auto_save(pet);
            }
        }
    }
    else if (_stricmp(cmd, "list") == 0 || _stricmp(cmd, "ls") == 0)
    {
        ContentKind_t kind = a1 ? kind_from(a1) : KIND_ITEM;
        if (kind == KIND_COUNT) g_api->ui_err("  未知种类：%s", a1);
        else list_kind(kind);
    }
    else if (_stricmp(cmd, "find") == 0)
    {
        ContentKind_t kind = kind_from(a1);
        Id id;

        if (kind == KIND_COUNT)
        {
            g_api->ui_err("  用法：find <种类> <名字>");
        }
        else if (!a2)
        {
            g_api->ui_err("  用法：find <种类> <名字>");
        }
        else
        {
            id = g_api->find(kind, a2);
            if (id == ID_NONE) g_api->ui_err("  没有找到：%s", a2);
            else g_api->ui_ok("  %s %s = Id %llu", kind_label(kind), a2, (unsigned long long)id);
        }
    }
    else if (_stricmp(cmd, "config") == 0)
    {
        if (!a1)
        {
            g_api->ui_print("  ui.page_size = %d\n", (int)g_api->config_get("ui.page_size"));
            g_api->ui_err("  用法：config <名字> [值]");
        }
        else if (g_api->find(KIND_CONFIG, a1) == ID_NONE)
        {
            g_api->ui_err("  没有注册的配置：%s", a1);
        }
        else if (!a2)
        {
            g_api->ui_print("  %s = %d\n", a1, (int)g_api->config_get(a1));
        }
        else
        {
            int64_t v;
            if (!parse_i64(a2, &v)) g_api->ui_err("  数值无效：%s", a2);
            else
            {
                if (v > INT32_MAX) v = INT32_MAX;
                if (v < INT32_MIN) v = INT32_MIN;
                if (!g_api->config_set(a1, (int32_t)v))
                    g_api->ui_err("  设置失败");
                else
                    g_api->ui_ok("  %s = %d", a1, (int)g_api->config_get(a1));
            }
        }
    }
    else if (_stricmp(cmd, "story") == 0)
    {
        if (!a1) g_api->ui_err("  用法：story <场景>");
        else
        {
            int r = g_api->story_play(pet, a1, NULL);
            if (r) { g_api->ui_ok("  剧情已播放"); auto_save(pet); }
            else g_api->ui_warn("  当前没有可播放的剧情");
        }
    }
    else if (_stricmp(cmd, "ach") == 0)
    {
        int new_ach = g_api->ach_poll(pet);
        g_api->ui_ok("  新解锁成就 %d 个", new_ach);
        if (new_ach > 0) auto_save(pet);
    }
    else if (_stricmp(cmd, "save") == 0)
    {
        if (g_api->save(pet)) g_api->ui_ok("  已存档");
        else g_api->ui_err("  存档失败");
    }
    else if (_stricmp(cmd, "clear") == 0 || _stricmp(cmd, "cls") == 0)
    {
        g_api->ui_title("控制台");
        g_api->ui_print("\n  输入 help 查看命令，quit 退出。\n");
    }
    else
    {
        g_api->ui_err("  未知命令：%s（help 查看命令）", cmd);
    }

    free(work);
    return result;
}

/* ============ 菜单入口 ============ */

static int menu_console(Pet *pet)
{
    if (!pet) return 0;

    if (g_api->is_headless())
    {
        static const char *const SCRIPT[] = {
            "help",
            "pet",
            "coins 1000",
            "exp 500",
            "full",
            "heal",
            "item 粗粮 5",
            "randitem 2",
            "list items",
            "config ui.page_size",
            "ach",
            "save",
            "quit"
        };
        uint64_t i;

        for (i = 0; i < (uint64_t)(sizeof(SCRIPT) / sizeof(SCRIPT[0])); i++)
        {
            g_api->ui_print("\n控制台> %s\n", SCRIPT[i]);
            if (!console_exec(pet, SCRIPT[i])) break;
        }
        return 1;
    }

    g_api->ui_title("控制台");
    g_api->ui_print("\n  输入 help 查看命令，quit 退出。\n");

    for (;;)
    {
        char *line = console_read_line();
        if (!line)
        {
            g_api->ui_print("\n离开控制台\n");
            break;
        }
        if (!console_exec(pet, line))
        {
            free(line);
            break;
        }
        free(line);
    }
    return 1;
}

static const Menu_t MENUS[] = {
    {"控制台", "控制台", 0, 150, MENU_FLAG_NONE, menu_console}
};

static const char *const REQUIRES[] = { "Core:物品/粗粮" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Debug", "2.0.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
    return &info;
}

MOD_EXPORT int mod_init(const LoaderApi *api)
{
    if (!api) return fail("mod_init 收到空接口表");
    if (api->version != LOADER_API_VERSION)
        return fail("接口版本不符：模组按 v%u 编译，加载器是 v%u", (unsigned)MOD_API_VERSION, (unsigned)api->version);
    if (api->size < (uint32_t)sizeof(LoaderApi))
        return fail("LoaderApi 大小不足（%u < %u）：Defineds.h/Loader.h 与加载器不一致",
                    (unsigned)api->size, (unsigned)sizeof(LoaderApi));

    g_api = api;

    if (MOD_ADD(api, KIND_MENU, MENUS) == ID_NONE)
        return fail("KIND_MENU 注册失败：内容表已满，或 Menu_t 尺寸与加载器不一致");
    return 1;
}

MOD_EXPORT const char *mod_error(void)
{
    return g_err[0] ? g_err : "mod_init 失败（未提供原因）";
}

MOD_EXPORT void mod_exit(void)
{
    g_api = 0;
}
