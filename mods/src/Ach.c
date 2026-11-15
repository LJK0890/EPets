/* ============================================================
 * EPets · 成就模组
 *   17 个成就（8 公开 + 9 隐藏）+ 记事册菜单；注册 KIND_ACHIEVEMENT / KIND_MENU
 *
 * 编译: cl /LD /utf-8 /I. Ach.c /Fe:Ach.dll
 *
 * 达成条件是回调（on_check），奖励是数据（物品名 + 数量），
 * 引擎负责：轮询 on_check → 置位 ach 位 → 发奖（按名字查物品 Id）。
 * ============================================================ */

#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const LoaderApi *g_api = 0;
static char g_err[192];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

/* ============ 达成条件 ============ */

static int c_lv5(const Pet *p)      { return p->level >= 5; }
static int c_lv10(const Pet *p)     { return p->level >= 10; }
static int c_rich(const Pet *p)     { return p->coins >= 5000; }
static int c_feed30(const Pet *p)   { return p->total_feed >= 30; }
static int c_play30(const Pet *p)   { return p->total_play >= 30; }
static int c_visit10(const Pet *p)  { return p->total_visit >= 10; }
static int c_win5(const Pet *p)     { return p->total_win >= 5; }

/* 收藏家：背包里石头 / 照片 / 饰品 各至少 1 个（跨模组按名字查物品 Id） */
static int c_collect(const Pet *p)
{
    Id stone = g_api->find(KIND_ITEM, "小石");
    Id photo = g_api->find(KIND_ITEM, "旧照片");
    Id jewel = g_api->find(KIND_ITEM, "琉璃饰品");

    if (stone == ID_NONE || photo == ID_NONE || jewel == ID_NONE) return 0;
    return g_api->inv_count(p, stone) >= 1 &&
           g_api->inv_count(p, photo) >= 1 &&
           g_api->inv_count(p, jewel) >= 1;
}

/* 一见钟情：领养时特质一次没重摇（要有名字，避免刚建号就触发） */
static int c_no_reroll(const Pet *p)
{
    return p->level >= 1 && p->name && p->name[0] != '\0' && p->trait_reroll_cnt == 0;
}

static int c_hungry5(const Pet *p)   { return p->hungry_streak >= 5; }
static int c_lucky3(const Pet *p)    { return p->win_streak >= 3; }
static int c_bigspend(const Pet *p)  { return p->big_spend >= 1000; }
static int c_age30(const Pet *p)     { return p->age >= 30; }
static int c_gift10(const Pet *p)    { return p->total_gift_ok >= 10; }

/* 天生我材：魅力 / 聪明 / 勇敢 任一达到 100 */
static int c_person100(const Pet *p)
{
    return p->personality[0] >= 100 || p->personality[1] >= 100 || p->personality[2] >= 100;
}

static int c_lose3(const Pet *p)     { return p->lose_streak >= 3; }

/* ============ 奖励 ============ */

static const AchReward_t R_LV5[]       = { {"补剂", 2} };
static const AchReward_t R_LV10[]      = { {"机能饮", 2}, {"补剂", 1} };
static const AchReward_t R_RICH[]      = { {"冠羽", 1} };
static const AchReward_t R_FEED30[]    = { {"鲜食", 5} };
static const AchReward_t R_PLAY30[]    = { {"彩笔", 1} };
static const AchReward_t R_VISIT10[]   = { {"澡刷", 3}, {"点心", 3} };
static const AchReward_t R_WIN5[]      = { {"琉璃饰品", 1} };
static const AchReward_t R_COLLECT[]   = { {"相匣", 1}, {"机能饮", 1} };
static const AchReward_t R_NO_REROLL[] = { {"花结", 1}, {"鲜食", 3} };
static const AchReward_t R_HUNGRY5[]   = { {"伤药", 3}, {"补剂", 2} };
static const AchReward_t R_LUCKY3[]    = { {"飞盘", 1}, {"琉璃饰品", 1} };
static const AchReward_t R_BIGSPEND[]  = { {"冠羽", 1}, {"花结", 1} };
static const AchReward_t R_AGE30[]     = { {"琉璃饰品", 2}, {"相匣", 1} };
static const AchReward_t R_GIFT10[]    = { {"旧照片", 3}, {"遮风伞", 1} };
static const AchReward_t R_PERSON100[] = { {"冠羽", 1}, {"琉璃饰品", 1}, {"机能饮", 2} };
static const AchReward_t R_LOSE3[]     = { {"伤药", 2}, {"点心", 5} };

#define N(a) (uint32_t)(sizeof(a) / sizeof((a)[0]))

/* ============ 成就表 ============ */

/* 注册顺序即本模组的 Id 顺序（1~17） */
static const Achievement_t ACHS[] = {
    /* ---- 公开成就 ---- */
    {"萌新上路",   "宠物达到 Lv.5",                  0, c_lv5,       R_LV5,       N(R_LV5)},
    {"小有成就",   "宠物达到 Lv.10",                 0, c_lv10,      R_LV10,      N(R_LV10)},
    {"小富翁",     "持有鲁尼 >= 5000",               0, c_rich,      R_RICH,      N(R_RICH)},
    {"辛勤铲屎官", "累计喂食 30 次",                 0, c_feed30,    R_FEED30,    N(R_FEED30)},
    {"玩伴达人",   "累计玩耍 30 次",                 0, c_play30,    R_PLAY30,    N(R_PLAY30)},
    {"社交达人",   "累计拜访 10 次",                 0, c_visit10,   R_VISIT10,   N(R_VISIT10)},
    {"战斗高手",   "对战累计胜利 5 次",              0, c_win5,      R_WIN5,      N(R_WIN5)},
    {"收藏家",     "背包同时拥有石头、照片、饰品",   0, c_collect,   R_COLLECT,   N(R_COLLECT)},
    /* ---- 隐藏成就 ---- */
    {"一见钟情",   "领养时特质一次没重摇",           1, c_no_reroll, R_NO_REROLL, N(R_NO_REROLL)},
    {"永不言弃",   "连续 5 次在饿着时喂食",          1, c_hungry5,   R_HUNGRY5,   N(R_HUNGRY5)},
    {"欧皇附体",   "对战取得 3 连胜",                1, c_lucky3,    R_LUCKY3,    N(R_LUCKY3)},
    {"土豪朋友",   "单次消费 1000 鲁尼以上",         1, c_bigspend,  R_BIGSPEND,  N(R_BIGSPEND)},
    {"长命百岁",   "宠物存活 30 天以上",             1, c_age30,     R_AGE30,     N(R_AGE30)},
    {"社交霸王花", "送礼累计被接受 10 次",           1, c_gift10,    R_GIFT10,    N(R_GIFT10)},
    {"天生我材",   "魅力/聪明/勇敢其中一项达到100",  1, c_person100, R_PERSON100, N(R_PERSON100)},
    {"非酋转世",   "对战遭遇 3 连败",                1, c_lose3,     R_LOSE3,     N(R_LOSE3)},
    /* 拒绝领养：剧情性成就，条件由引擎判定（on_check = NULL），按名字强制解锁 */
    {"铁石心肠",   "拒绝了A5的领养邀请",             1, 0,           0,           0}
};

/* ============ 记事册菜单 ============ */

static void pause_key(void)
{
    g_api->ui_print("\nZ:继续\n");
    g_api->key();
}

static int menu_ach(Pet *pet)
{
    uint64_t i, n = g_api->count(KIND_ACHIEVEMENT), done = 0, claimable = 0;

    g_api->ui_title("记事册");
    g_api->ui_print("\n");
    for (i = 1; i <= n; i++)
    {
        const Achievement_t *a = (const Achievement_t *)g_api->get(KIND_ACHIEVEMENT, (Id)i);
        int ok, claimed;
        if (!a) continue;
        ok = g_api->ach_unlocked(pet, (Id)i);
        claimed = g_api->ach_claimed(pet, (Id)i);
        if (ok)
        {
            done++;
            if (claimed) g_api->ui_print("  [√] %s（已领）\n", a->name);
            else { g_api->ui_print("  [√] %s（可领）\n", a->name); claimable++; }
        }
        else if (a->hidden) g_api->ui_print("  [ ] ???\n");
        else g_api->ui_print("  [ ] %s - %s\n", a->name, a->desc);
    }
    g_api->ui_print("\n  完成 %llu/%llu，可领 %llu\n",
                    (unsigned long long)done, (unsigned long long)n,
                    (unsigned long long)claimable);

    if (claimable > 0)
    {
        SelectOpt_t acts[2];
        memset(acts, 0, sizeof(acts));
        g_api->opt_text(&acts[0], "全部领取");
        acts[0].value = 1;
        g_api->opt_text(&acts[1], "返回");
        acts[1].value = 0;

        if (g_api->ui_menu("领取", acts, 2) == 1)
        {
            for (i = 1; i <= n; i++)
                if (g_api->ach_unlocked(pet, (Id)i) && !g_api->ach_claimed(pet, (Id)i))
                    g_api->ach_claim(pet, (Id)i);
            g_api->ui_ok("  奖励已确认领取（解锁时已入包）");
            g_api->save(pet);
        }
    }
    pause_key();
    return 1;
}

/* ============ 入口 ============ */

static const Menu_t MENUS[] = {
    {"记事册", "记事册", 0, 90, MENU_FLAG_NONE, menu_ach}
};

static const char *const REQUIRES[] = { "Core:物品/冠羽" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Achievements", "1.1.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
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
    if (MOD_ADD(api, KIND_ACHIEVEMENT, ACHS) == ID_NONE)
        return fail("KIND_ACHIEVEMENT 注册失败：内容表已满，或 Achievement_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_MENU, MENUS) == ID_NONE)
        return fail("KIND_MENU 注册失败：内容表已满，或 Menu_t 尺寸与加载器不一致");

    /* 自检：名字可查回 */
    if (g_api->find(KIND_ACHIEVEMENT, "萌新上路") == ID_NONE)
        return fail("自检失败：注册后按名字查不到成就「萌新上路」");
    if (g_api->find(KIND_MENU, "记事册") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「记事册」");
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
