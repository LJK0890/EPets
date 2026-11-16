/* ============================================================
 * EPets · 活动模组
 *   附近对战（2.4G 扫描 + 猜拳三局两胜）
 *   桥屋打工（工作事件 + 宠物居家事件）
 *   注册 KIND_MINIGAME（两个通用活动，pet_type = NULL）+ 桥屋 / 祭坛菜单（KIND_MENU）
 *
 * 编译: cl /LD /utf-8 /I. Activity.c /Fe:Activity.dll
 * ============================================================ */

#define _CRT_SECURE_NO_WARNINGS 1

#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>

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

static void pause_key(void)
{
    g_api->ui_print("\nZ:继续\n");
    g_api->key();
}

/* 反复进入一个子菜单，直到玩家选“返回” */
static int run_sub(Pet *pet, const char *name)
{
    int r;
    while ((r = g_api->menu_run(pet, name)) == 1) { }
    return (r < 0) ? -1 : 1;
}

/* ============ 小工具 ============ */

static uint8_t add_stat(uint8_t v, int d)
{
    int r = (int)v + d;
    if (r < MIN_STAT) return (uint8_t)MIN_STAT;
    if (r > MAX_STAT) return (uint8_t)MAX_STAT;
    return (uint8_t)r;
}

/* ============ 附近对战 ============ */

typedef struct {
    const char *name;
    Id type_id;
    uint8_t level;
    Gender_t gender;
} Foe_t;

/* 模拟扫描：随机 2~4 只附近的宠物 */
static void scan_foes(Foe_t *foe, int *n)
{
    uint64_t types = g_api->count(KIND_PET_TYPE);
    int i;

    *n = (int)g_api->rnd(2, 4);
    for (i = 0; i < *n; i++)
    {
        Gender_t g = (Gender_t)g_api->rnd(G_MALE, G_FEMALE);
        uint64_t nc = g_api->name_count(g);

        foe[i].name = nc ? g_api->name_get(g, g_api->rnd64(0, nc - 1)) : "???";
        foe[i].type_id = types ? (Id)g_api->rnd64(1, types) : ID_NONE;
        foe[i].level = (uint8_t)g_api->rnd(1, 15);
        foe[i].gender = g;
    }
}

/* 猜拳三局两胜；返回1=赢 */
static int rps(Pet *pet)
{
    static const char *HAND[] = { "石头", "剪刀", "布" };
    int win = 0, lose = 0, round;

    for (round = 1; round <= 3; round++)
    {
        int me, foe;

        if (win >= 2 || lose >= 2) break;

        g_api->ui_title("猜拳对战");
        g_api->ui_print("\n  第 %d 回合  (你 %d胜 %d负)\n\n", round, win, lose);
        g_api->ui_print("  选择你的出招：\n\n  ↑:石头 ↓:剪刀 Z:布\n");

        me = g_api->key();
        if (me < 1 || me > 3) me = 1;
        foe = (int)g_api->rnd(1, 3);

        g_api->ui_title("对战结果");
        g_api->ui_print("\n  你出: %s\n  对方: %s\n\n", HAND[me - 1], HAND[foe - 1]);

        if (me == foe)
            g_api->ui_print("  平局!\n");
        else if ((me == 1 && foe == 2) || (me == 2 && foe == 3) || (me == 3 && foe == 1))
        {
            g_api->ui_print("  你赢了!\n");
            win++;
        }
        else
        {
            g_api->ui_print("  你输了!\n");
            lose++;
        }

        g_api->ui_print("\nZ:继续\n");
        g_api->key();
    }

    g_api->ui_title("对战结束");
    if (win >= 2)
    {
        uint32_t coin = g_api->rnd(50, 150);
        g_api->ui_print("\n  恭喜！你赢了！\n  +%u鲁尼\n", coin);
        pet->coins += coin;
        return 1;
    }

    g_api->ui_print("\n  惜败...%d胜%d负\n", win, lose);
    pet->happy = add_stat(pet->happy, 10);
    return 0;
}

static int game_battle(Pet *pet)
{
    Foe_t foe[4];
    int n = 0, sel = 0, i;

    g_api->ui_title("2.4G 搜索中");
    g_api->ui_print("\n  正在扫描附近的宠物...\n");
    g_api->ui_print("  (后期接入 2.4G 通信)\n\n");
    g_api->delay(1500);

    scan_foes(foe, &n);
    if (n <= 0) return 0;

    while (1)
    {
        int k;

        g_api->ui_title("附近的宠物");
        g_api->ui_print("\n");
        for (i = 0; i < n; i++)
        {
            const PetType *t = PET_GET(g_api, foe[i].type_id);
            char line[96];
            _snprintf(line, sizeof(line), "%u. %s Lv.%u  (%s)",
                      (unsigned)(i + 1), foe[i].name,
                      (unsigned)foe[i].level,
                      t ? t->name : "未知");
            line[sizeof(line) - 1] = '\0';
            g_api->ui_option(line, i == sel);
        }
        g_api->ui_print("\n  ↑↓:选择  Z:对战  X:返回\n");

        k = g_api->key();
        if (k == 1) sel = (sel - 1 + n) % n;
        else if (k == 2) sel = (sel + 1) % n;
        else if (k == 4) return 0;
        else if (k == 3) break;
    }

    {
        const PetType *t = PET_GET(g_api, foe[sel].type_id);
        g_api->ui_title("对战开始");
        g_api->ui_print("\n  %s 挑战 %s 的 %s Lv.%u！\n",
                        pet->name ? pet->name : "它", foe[sel].name,
                        t ? t->name : "宠物", (unsigned)foe[sel].level);
        g_api->delay(1200);
    }

    {
        int won = rps(pet);

        /* 连胜/连败统计（成就用） */
        if (won)
        {
            pet->total_win++;
            pet->win_streak++;
            pet->lose_streak = 0;
            g_api->exp_add(pet, 25);
        }
        else
        {
            pet->lose_streak++;
            pet->win_streak = 0;
        }
        return won;
    }
}

/* ============ 桥屋打工 ============ */

static const char *WORK_PAGE[5] = {
    "  一位顾客对你大发脾气,\n  说找零给错了...",
    "  店长让你整理货架,\n  但标签有些模糊...",
    "  收银时发现少了50鲁尼,\n  不确定是找错了还是...",
    "  你发现有人偷偷\n  把零食塞进口袋...",
    "  一位游客向你问路,\n  你其实也不太确定..."
};

static const StoryOpt_t WORK_OPTS[5][3] = {
    { {"耐心解释", 1}, {"据理力争", 2}, {"道歉了事", 3} },
    { {"仔细核对", 1}, {"凭感觉放", 2}, {"问店长",   3} },
    { {"报告店长", 1}, {"自己垫付", 2}, {"假装没发现", 3} },
    { {"当面制止", 1}, {"悄悄报警", 2}, {"不理会",   3} },
    { {"详细指路", 1}, {"说不知道", 2}, {"查手机",   3} }
};

/* 每种事件的最佳/最差选项（照原版 best/worst 表） */
static const int WORK_BEST[5]  = { 1, 1, 1, 2, 3 };
static const int WORK_WORST[5] = { 2, 2, 3, 3, 2 };

static int game_work(Pet *pet)
{
    int income = 100;

    g_api->ui_title("便利店打工");
    g_api->ui_print("\n  今天去便利店打工...\n\n");
    g_api->delay(1200);

    /* 60% 触发工作事件 */
    if (g_api->rnd(0, 99) < 60)
    {
        int evt = (int)g_api->rnd(0, 4);
        const char *pages[1];
        int choice;

        pages[0] = WORK_PAGE[evt];
        choice = g_api->dialog("工作事件", pages, 1, WORK_OPTS[evt], 3);

        g_api->ui_title("打工结果");
        if (choice == WORK_BEST[evt])
        {
            income += 50;
            g_api->ui_print("\n  处理得很好！\n  店长夸奖了你 +50鲁尼\n");
        }
        else if (choice == WORK_WORST[evt])
        {
            income -= 30;
            g_api->ui_print("\n  处理不太妥当...\n  被扣了30鲁尼\n");
        }
        else
        {
            g_api->ui_print("\n  中规中矩的处理\n  没有额外奖惩\n");
        }
        g_api->delay(1500);
    }

    /* 领工资 */
    if (income < 0) income = 0;
    pet->coins += (uint32_t)income;

    g_api->ui_title("打工结束");
    g_api->ui_print("\n  今天收入：%d鲁尼\n  当前鲁尼：%u\n\n", income, pet->coins);

    /* 主人外出期间宠物在家可能出事：年龄越小概率越高 */
    {
        int chance = (pet->age < 10) ? 80 : ((pet->age < 15) ? 60 : 40);

        if ((int)g_api->rnd(0, 99) < chance)
        {
            int e = (int)g_api->rnd(0, 7);

            g_api->ui_print("  外出期间发生了事情...\n\n");
            g_api->delay(1000);
            g_api->ui_title("宠物消息");

            switch (e)
            {
            case 0: /* 走丢 */
                g_api->ui_print("\n  %s 趁你不在偷偷溜出去了！\n  好在邻居帮忙找回来了...\n",
                                pet->name ? pet->name : "它");
                pet->happy = add_stat(pet->happy, -30);
                pet->health = add_stat(pet->health, -20);
                g_api->ui_print("  心情-30 健康-20\n");
                break;
            case 1: /* 受伤 */
                g_api->ui_print("\n  %s 在家里摔了一跤！\n", pet->name ? pet->name : "它");
                pet->health = add_stat(pet->health, -30);
                g_api->ui_print("  健康-30\n");
                break;
            case 2: /* 误食 */
                g_api->ui_print("\n  %s 偷吃了不该吃的东西！\n", pet->name ? pet->name : "它");
                pet->hunger = add_stat(pet->hunger, -20);
                pet->health = add_stat(pet->health, -20);
                g_api->ui_print("  饱食-20 健康-20\n");
                break;
            case 3: /* 捣乱 */
                g_api->ui_print("\n  %s 把家里弄得一团糟！\n", pet->name ? pet->name : "它");
                pet->clean = add_stat(pet->clean, -30);
                g_api->ui_print("  清洁-30\n");
                break;
            case 4: /* 自己玩耍 */
                g_api->ui_print("\n  %s 自己玩得很开心！\n", pet->name ? pet->name : "它");
                pet->happy = add_stat(pet->happy, 20);
                g_api->ui_print("  心情+20\n");
                break;
            case 5: /* 睡觉 */
                g_api->ui_print("\n  %s 乖乖睡了一觉~\n", pet->name ? pet->name : "它");
                pet->energy = add_stat(pet->energy, 30);
                g_api->ui_print("  活力+30\n");
                break;
            case 6: /* 找到食物 */
                g_api->ui_print("\n  %s 在家里找到了零食！\n", pet->name ? pet->name : "它");
                pet->hunger = add_stat(pet->hunger, 20);
                g_api->ui_print("  饱食+20\n");
                break;
            default: /* 意外之财 */
            {
                uint32_t found = g_api->rnd(10, 30);
                g_api->ui_print("\n  %s 在沙发缝里找到了钱！\n", pet->name ? pet->name : "它");
                pet->coins += found;
                g_api->ui_print("  +%u鲁尼\n", found);
                break;
            }
            }

            g_api->ui_print("\nZ:继续\n");
            g_api->key();
        }
        else
        {
            g_api->ui_print("  %s 在家很乖~\n", pet->name ? pet->name : "它");
            g_api->ui_print("\nZ:继续\n");
            g_api->key();
        }
    }

    return 1;
}

/* ============ 桥屋菜单 ============ */

static int menu_train(Pet *pet)
{
    if (pet->coins < 50) { g_api->ui_warn("  需要 50 鲁尼"); pause_key(); return 1; }
    pet->coins -= 50;
    pet->personality[1] = add_stat(pet->personality[1], 3);
    pet->energy = add_stat(pet->energy, -10);
    g_api->ui_ok("训练完，聪明+3");
    g_api->exp_add(pet, 10);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

static int menu_spa(Pet *pet)
{
    if (pet->coins < 80) { g_api->ui_warn("  需要 80 鲁尼"); pause_key(); return 1; }
    pet->coins -= 80;
    pet->clean = add_stat(pet->clean, 30);
    pet->happy = add_stat(pet->happy, 10);
    (void)g_api->story_play(pet, "spa", "小春");
    g_api->ui_ok("美容完成，清洁+30");
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

static int menu_board(Pet *pet)
{
    if (pet->coins < 100) { g_api->ui_warn("  需要 100 鲁尼"); pause_key(); return 1; }
    pet->coins -= 100;
    pet->happy = add_stat(pet->happy, 20);
    pet->hunger = add_stat(pet->hunger, 20);
    g_api->ui_ok("寄养了一天，状态回满一些");
    g_api->save(pet);
    pause_key();
    return 1;
}

static int menu_battle(Pet *pet)
{
    Id game = g_api->find(KIND_MINIGAME, "附近对战");

    if (game == ID_NONE)
    {
        g_api->ui_warn("  找不到「附近对战」小游戏（Activity 模组没有注册成功？）");
        pause_key();
        return 1;
    }
    (void)g_api->story_play(pet, "battle", "小春");
    g_api->minigame_run(pet, game);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

static int menu_center(Pet *pet)
{
    (void)g_api->story_play(pet, "center", NULL);
    return run_sub(pet, "桥屋");
}

/* ============ 祭坛：改名 / 特质定制 ============ */

/* 祭坛价格（名字与特质分开定价） */
#define TREE_NAME_RANDOM_COST 500u
#define TREE_NAME_PICK_COST  1000u
#define TREE_TRAIT_RANDOM     500u
#define TREE_TRAIT_PICK      1000u
#define TREE_TRAIT_DELETE    2000u

/* 改名：名字重设（随机 500 / 选取 1000） */
static int menu_rename(Pet *pet)
{
    static const char *const PAGES[] = {
        "  $C$N祭坛$n ：想给它换个名字？$P",
        "  $C$N祭坛$n ：名字会跟它一辈子，想好了再改。$P"
    };
    static const StoryOpt_t OPTS[] = {
        {"随机名字（500 鲁尼）", 1},
        {"选取名字（1000 鲁尼）", 2},
        {"返回", 0}
    };
    const char *name;
    uint32_t cost;
    int choice;

    choice = g_api->dialog("祭坛·改名", PAGES, 2, OPTS, 3);
    if (choice == 1)
    {
        if (pet->coins < TREE_NAME_RANDOM_COST)
        {
            g_api->ui_warn("  鲁尼不够：随机名字需要 %u 鲁尼", TREE_NAME_RANDOM_COST);
            pause_key();
            return 1;
        }
        name = g_api->rand_name(pet->gender, pet->name); /* 尽量不和现在的名字重复 */
        if (!name)
        {
            g_api->ui_warn("  名字库是空的，改不了名");
            pause_key();
            return 1;
        }
        cost = TREE_NAME_RANDOM_COST;
    }
    else if (choice == 2)
    {
        if (pet->coins < TREE_NAME_PICK_COST)
        {
            g_api->ui_warn("  鲁尼不够：选取名字需要 %u 鲁尼", TREE_NAME_PICK_COST);
            pause_key();
            return 1;
        }
        name = g_api->name_pick(pet->gender, "选择名字");
        if (!name) return 1; /* 取消不算钱 */
        cost = TREE_NAME_PICK_COST;
    }
    else return 1;

    pet->coins -= cost;
    if (cost > pet->big_spend) pet->big_spend = cost;
    pet->name = name;
    g_api->ui_ok("  它现在叫「%s」，剩余 %u 鲁尼", pet->name, pet->coins);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

/* 特质定制：只在已有的 2 个免费特质后面追加（随机 500 / 选取 1000），删除 2000 */
static int menu_traits(Pet *pet)
{
    (void)g_api->trait_shop(pet, "祭坛·特质定制", 1,
                            TREE_TRAIT_RANDOM, TREE_TRAIT_PICK, TREE_TRAIT_DELETE);
    g_api->ach_poll(pet);
    g_api->save(pet);
    return 1;
}

/* ============ 注册 ============ */

static const Minigame_t GAMES[] = {
    {"附近对战", "2.4G 扫描附近的宠物，猜拳三局两胜", 0, game_battle},
    {"桥屋打工", "接差事赚鲁尼，期间可能出状况",       0, game_work}
};

static const Menu_t MENUS[] = {
    {"桥屋",     "桥屋",     0,      70, MENU_FLAG_NONE, menu_center},
    {"训练",     "训练",     "桥屋", 10, MENU_FLAG_NONE, menu_train},
    {"美容",     "美容",     "桥屋", 20, MENU_FLAG_NONE, menu_spa},
    {"寄养",     "寄养",     "桥屋", 30, MENU_FLAG_NONE, menu_board},
    {"附近对战", "附近对战", "桥屋", 40, MENU_FLAG_NONE, menu_battle},
    {"祭坛",     "祭坛",     "桥屋", 50, MENU_FLAG_NONE, 0},
    {"改名",     "改名",     "祭坛", 10, MENU_FLAG_NONE, menu_rename},
    {"特质定制", "特质定制", "祭坛", 20, MENU_FLAG_NONE, menu_traits}
};

static const char *const REQUIRES[] = {
    "Core:宠物类型/暗影猫",
    "Story:剧情/首次洗浴",
    "Story:剧情/首次比试"
};

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Activities", "1.2.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
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
    if (MOD_ADD(api, KIND_MINIGAME, GAMES) == ID_NONE)
        return fail("KIND_MINIGAME 注册失败：内容表已满，或 Minigame_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_MENU, MENUS) == ID_NONE)
        return fail("KIND_MENU 注册失败：内容表已满，或 Menu_t 尺寸与加载器不一致");

    /* 自检：名字可查回 */
    if (g_api->find(KIND_MINIGAME, "附近对战") == ID_NONE)
        return fail("自检失败：注册后按名字查不到小游戏「附近对战」");
    if (g_api->find(KIND_MINIGAME, "桥屋打工") == ID_NONE)
        return fail("自检失败：注册后按名字查不到小游戏「桥屋打工」");
    if (g_api->find(KIND_MENU, "桥屋") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「桥屋」");
    if (g_api->find(KIND_MENU, "祭坛") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「祭坛」");
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
