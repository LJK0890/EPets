/* ============================================================
 * EPets · Core 模组
 *   基础数据：20 个特质 / 19 个物品 / 8 个宠物类型 / 3 组名字库
 *
 * 编译: cl /LD /utf-8 /I. Core.c /Fe:Core.dll
 *
 * 这个文件就是“数据层”：名字、价格、概率是数据，效果是回调。
 * 引擎不认识任何具体内容，只会：
 *   注册时拿走描述符 → 分配 Id → 之后按 Id 查表调回调。
 * 名字等字符串都是字面量，链接进 Core.dll 的只读段，结构体里只放指针。
 * ============================================================ */

#include "Defineds.h"
#include "Loader.h"

#include <stdlib.h> /* rand */
#include <stdarg.h>
#include <stdio.h>

/* 加载器接口，回调里需要查表时用它（本模组目前只做自检） */
static const LoaderApi *g_api = 0;

/* mod_init 失败时留下可读原因，加载器通过 mod_error() 取走 */
static char g_err[192];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

/* ============ 数值工具 ============ */

static uint8_t stat_of(int v)
{
    if (v < MIN_STAT) return (uint8_t)MIN_STAT;
    if (v > MAX_STAT) return (uint8_t)MAX_STAT;
    return (uint8_t)v;
}

/* 同时位移 4 项属性：饱食 / 心情 / 清洁 / 健康 */
static int shift4(Pet *pet, int hunger, int happy, int clean, int health)
{
    pet->hunger = stat_of((int)pet->hunger + hunger);
    pet->happy  = stat_of((int)pet->happy  + happy);
    pet->clean  = stat_of((int)pet->clean  + clean);
    pet->health = stat_of((int)pet->health + health);
    return 1;
}

/* ============ 特质 ============ */

/*
 * 特质触发时机由引擎决定（原设计是每日结算时对每只宠物逐个调用）。
 * 效果就是原来 Trait.mod[4] 那四个修正值，只是从数据表搬进了回调。
 */
#define TRAIT_FN(fn, h, m, c, hp) \
    static int fn(Pet *pet) { return shift4(pet, h, m, c, hp); }

TRAIT_FN(trait_lively,      0,  10,  0,  0) /* 活泼 */
TRAIT_FN(trait_gentle,      0,   8,  0,  0) /* 温顺 */
TRAIT_FN(trait_smart,       0,   0,  0,  5) /* 聪明 */
TRAIT_FN(trait_cheerful,    0,   5,  0,  0) /* 乐观 */
TRAIT_FN(trait_clean,       0,   5,  5,  0) /* 爱干净 */
TRAIT_FN(trait_greedy,      5,  10,  0,  0) /* 贪吃 */
TRAIT_FN(trait_loyal,       0,   0,  0,  3) /* 忠诚 */
TRAIT_FN(trait_curious,     0,   5,  0,  0) /* 好奇 */
TRAIT_FN(trait_brave,       0,   0,  0,  5) /* 勇敢 */
TRAIT_FN(trait_hardworking, 0,   0,  0,  0) /* 勤劳（原表就是全 0，留待重调） */
TRAIT_FN(trait_shy,         0,  -8,  0,  0) /* 害羞 */
TRAIT_FN(trait_picky,       0, -10,  0,  0) /* 挑食 */
TRAIT_FN(trait_sloppy,      0,  -5, -5,  0) /* 邋遢 */
TRAIT_FN(trait_lazy,        0,  -5,  0,  0) /* 懒惰 */
TRAIT_FN(trait_grumpy,      0,  -5,  0,  0) /* 暴躁 */
TRAIT_FN(trait_weak,        0,   0,  0, -5) /* 体弱 */
TRAIT_FN(trait_sleepy,      3,  -5,  0,  0) /* 嗜睡 */
TRAIT_FN(trait_timid,       0,  -8,  0,  0) /* 胆小 */
TRAIT_FN(trait_tsun,        0, -10,  0,  0) /* 傲娇 */
TRAIT_FN(trait_aloof,       0,  -8,  0,  0) /* 孤僻 */

/* 注册顺序即本模组的 Id 顺序（1~20） */
static const Trait_t CORE_TRAITS[] = {
    {trait_lively,      T_POS, "活泼",     "懒惰"},
    {trait_gentle,      T_POS, "温顺",     "暴躁"},
    {trait_smart,       T_POS, "聪明",     "害羞"},
    {trait_cheerful,    T_POS, "乐观",     "孤僻"},
    {trait_clean,       T_POS, "爱干净",   "邋遢"},
    {trait_greedy,      T_POS, "贪吃",     0},
    {trait_loyal,       T_POS, "忠诚",     "傲娇"},
    {trait_curious,     T_POS, "好奇",     "挑食"},
    {trait_brave,       T_POS, "勇敢",     "胆小"},
    {trait_hardworking, T_POS, "勤劳",     0},
    {trait_shy,         T_NEG, "害羞"},
    {trait_picky,       T_NEG, "挑食"},
    {trait_sloppy,      T_NEG, "邋遢"},
    {trait_lazy,        T_NEG, "懒惰"},
    {trait_grumpy,      T_NEG, "暴躁"},
    {trait_weak,        T_NEG, "体弱"},
    {trait_sleepy,      T_NEG, "嗜睡"},
    {trait_timid,       T_NEG, "胆小"},
    {trait_tsun,        T_NEG, "傲娇"},
    {trait_aloof,       T_NEG, "孤僻"}
};

/* ============ 物品 ============ */

/*
 * 物品效果同样只是属性位移；是否消耗、哪些能买，
 * 由引擎按 Item.type（类别）决定，物品自己不关心。
 */
#define ITEM_FN(fn, h, m, c, hp) \
    static int fn(Pet *pet) { return shift4(pet, h, m, c, hp); }

/* 食物 */
ITEM_FN(item_food,     5,  -3,   0,  10) /* 粗粮 */
ITEM_FN(item_delic,   15,  -5,   5,  30) /* 鲜食 */
ITEM_FN(item_snack,   20,  -2,  -5,  15) /* 点心 */
/* 玩具 */
ITEM_FN(item_disc,    25,  -8,   0,  50) /* 飞盘 */
ITEM_FN(item_crayon,  30,  -5,   0,  80) /* 彩笔 */
ITEM_FN(item_ball,    20,  -5,   0,  35) /* 弹球 */
/* 装饰 */
ITEM_FN(item_bow,     10,   0,   0,  40) /* 花结 */
ITEM_FN(item_crown,   20,   0,   0, 100) /* 冠羽 */
/* 药品：伤药顺带解除疾病 */
static int item_medicine(Pet *pet)
{
    pet->sick = D_NONE;
    return shift4(pet, 0, 0, 30, 60);
}
ITEM_FN(item_vitamin,  5,   0,  15,  35) /* 补剂 */
ITEM_FN(item_energy,  10,   0,   5,  25) /* 机能饮 */
/* 清洁：清洁值原来是代码里写死的 +50 / +60，现在并进数据 */
ITEM_FN(item_towel,    0,  30,  50,  25) /* 浴布 */
ITEM_FN(item_brush,    5,  40,  60,  45) /* 澡刷 */
/* 旅行用品 */
ITEM_FN(item_bento,   10,   0,   0,  40) /* 干粮 */
ITEM_FN(item_umbrella, 5,  10,   0,  30) /* 遮风伞 */
ITEM_FN(item_camera,  15,   0,   0,  60) /* 相匣 */
/* 纪念品：旅游随机获得，不可购买 */
ITEM_FN(item_stone,    5,   0,   0,   0) /* 小石 */
ITEM_FN(item_photo,   10,   0,   0,   0) /* 旧照片 */
ITEM_FN(item_jewel,   15,   0,   0,   0) /* 琉璃饰品 */

/* 注册顺序即本模组的 Id 顺序（1~19） */
static const Item CORE_ITEMS[] = {
    {item_food,          I_FOOD,        50, "粗粮"},
    {item_delic,         I_FOOD,        80, "鲜食"},
    {item_snack,         I_FOOD,        30, "点心"},
    {item_disc,          I_TOY,         60, "飞盘"},
    {item_crayon,        I_TOY,         90, "彩笔"},
    {item_ball,          I_TOY,         40, "弹球"},
    {item_bow,           I_DECORATION, 100, "花结"},
    {item_crown,         I_DECORATION, 500, "冠羽"},
    {item_medicine,      I_MEDICINE,   200, "伤药"},
    {item_vitamin,       I_MEDICINE,   100, "补剂"},
    {item_energy,        I_MEDICINE,    80, "机能饮"},
    {item_towel,         I_CLEANING,    50, "浴布"},
    {item_brush,         I_CLEANING,    80, "澡刷"},
    {item_bento,         I_TRAVEL,      70, "干粮"},
    {item_umbrella,      I_TRAVEL,      60, "遮风伞"},
    {item_camera,        I_TRAVEL,     120, "相匣"},
    {item_stone,         I_DECORATION,   0, "小石"},
    {item_photo,         I_DECORATION,   0, "旧照片"},
    {item_jewel,         I_DECORATION,   0, "琉璃饰品"}
};

/* ============ 被动技能 ============ */

/*
 * 概率由引擎按 Passive_t.chance 掷骰后决定要不要调，
 * 这里只写“触发之后发生什么”；返回值见 Defineds.h 的 PASSIVE_*。
 */
static int passive_shadow(Pet *pet)  /* 暗影潜行：心情 +10 */
{
    pet->happy = stat_of((int)pet->happy + 10);
    return PASSIVE_OK;
}

static int passive_ice(Pet *pet)     /* 寒冰护体：健康 +10 */
{
    pet->health = stat_of((int)pet->health + 10);
    return PASSIVE_OK;
}

static int passive_thunder(Pet *pet) /* 雷电冲击：心情 +10 */
{
    pet->happy = stat_of((int)pet->happy + 10);
    return PASSIVE_OK;
}

static int passive_treasure(Pet *pet) /* 寻宝专家：鲁尼 +30~80 */
{
    pet->coins += (uint32_t)(30 + rand() % 51);
    return PASSIVE_OK;
}

static int passive_fire(Pet *pet)    /* 烈焰灼烧：心情 +10 */
{
    pet->happy = stat_of((int)pet->happy + 10);
    return PASSIVE_OK;
}

static int passive_water(Pet *pet)   /* 治愈之泉：健康 +20 */
{
    pet->health = stat_of((int)pet->health + 20);
    return PASSIVE_OK;
}

static int passive_wind(Pet *pet)    /* 疾风闪避：心情 +10，并抵消负面效果 */
{
    pet->happy = stat_of((int)pet->happy + 10);
    return PASSIVE_GUARD;
}

static int passive_earth(Pet *pet)   /* 岩石护盾：只抵消/减弱负面效果 */
{
    (void)pet;
    return PASSIVE_GUARD;
}

/* 注册顺序即本模组的 Id 顺序（1~8） */
static const PetType CORE_PETS[] = {
    {{passive_shadow,   30, "暗影潜行"}, "暗影猫", "如影随形的神秘生物"},
    {{passive_ice,      25, "寒冰护体"}, "冰霜兔", "浑身覆盖冰晶的小生灵"},
    {{passive_thunder,  20, "雷电冲击"}, "雷龙",   "蕴含雷电之力的小家伙"},
    {{passive_treasure, 50, "寻宝专家"}, "宝藏鼠", "天生嗅觉敏锐的财富猎手"},
    {{passive_fire,     25, "烈焰灼烧"}, "烈焰鸡", "热情似火的小火焰"},
    {{passive_water,    20, "治愈之泉"}, "水灵鲸", "来自深海的温柔生物"},
    {{passive_wind,     35, "疾风闪避"}, "疾风狐", "速度如风的灵巧小狐"},
    {{passive_earth,    30, "岩石护盾"}, "岩甲熊", "身披岩石铠甲的可靠伙伴"}
};

/* ============ 名字库 ============ */

static const char *const NAMES_MALE[] = {
    "洛基", "维拉托", "阿瑞斯", "雷恩", "凯萨", "亚克", "诺瓦", "赛德里斯"
};

static const char *const NAMES_FEMALE[] = {
    "洛茜", "希洛", "有希", "莉娜", "艾拉", "露娜", "克克莉娅", "伊莎"
};

static const char *const NAMES_NEUTER[] = {
    "程夏", "晓", "澪", "空", "凛", "云", "翼", "夜"
};

/* ============ 模组入口 ============ */

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Core", "2.1.0", MOD_API_VERSION, 0, 0 };
    return &info;
}

MOD_EXPORT int mod_init(const LoaderApi *api)
{
    Id probe;

    if (!api) return fail("mod_init 收到空接口表");
    if (api->version != LOADER_API_VERSION)
        return fail("接口版本不符：模组按 v%u 编译，加载器是 v%u", (unsigned)MOD_API_VERSION, (unsigned)api->version);
    if (api->size < (uint32_t)sizeof(LoaderApi))
        return fail("LoaderApi 大小不足（%u < %u）：Defineds.h/Loader.h 与加载器不一致",
                    (unsigned)api->size, (unsigned)sizeof(LoaderApi));

    g_api = api;

    if (MOD_ADD(api, KIND_TRAIT, CORE_TRAITS) == ID_NONE)
        return fail("KIND_TRAIT 注册失败：内容表已满，或 Trait_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_ITEM, CORE_ITEMS) == ID_NONE)
        return fail("KIND_ITEM 注册失败：内容表已满，或 Item 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_PET_TYPE, CORE_PETS) == ID_NONE)
        return fail("KIND_PET_TYPE 注册失败：内容表已满，或 PetType 尺寸与加载器不一致");

    if (api->add_names(G_MALE, NAMES_MALE, (uint32_t)(sizeof(NAMES_MALE) / sizeof(NAMES_MALE[0]))) == 0)
        return fail("男性名字库为空或超出上限");
    if (api->add_names(G_FEMALE, NAMES_FEMALE, (uint32_t)(sizeof(NAMES_FEMALE) / sizeof(NAMES_FEMALE[0]))) == 0)
        return fail("女性名字库为空或超出上限");
    if (api->add_names(G_NEUTER, NAMES_NEUTER, (uint32_t)(sizeof(NAMES_NEUTER) / sizeof(NAMES_NEUTER[0]))) == 0)
        return fail("中性名字库为空或超出上限");

    /* 自检：注册后按名字能查回 Id、按 Id 能取回描述符 */
    probe = g_api->find(KIND_TRAIT, "活泼");
    if (probe == ID_NONE || !TRAIT_GET(g_api, probe)) return fail("自检失败：特质「活泼」查不回来");
    probe = g_api->find(KIND_ITEM, "粗粮");
    if (probe == ID_NONE || !ITEM_GET(g_api, probe)) return fail("自检失败：物品「粗粮」查不回来");
    probe = g_api->find(KIND_PET_TYPE, "暗影猫");
    if (probe == ID_NONE || !PET_GET(g_api, probe)) return fail("自检失败：宠物类型「暗影猫」查不回来");

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
