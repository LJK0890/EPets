#ifndef LOADER_H
#define LOADER_H

#include <stdint.h>
#include "Defineds.h"

/**
 * @file Loader.h
 * @brief 加载器提供的接口（只有声明，实现由 ModLoader.dll 完成）
 *
 * 模组不链接加载器：加载器把 LoaderApi 传进模组的 mod_init，
 * 模组只通过这张函数表与外界打交道。
 *
 * 多模组规则：
 *   - 每种内容各自一个命名空间，Id 从 1 开始按“加载顺序 + 注册顺序”递增；
 *   - 一个模组可以只注册任意几种内容（例如只加 3 个成就），其余留空；
 *   - 不同模组可以往同一类内容里继续加，互不覆盖；
 *   - 名字全局查找，同名以先注册的为准（加载器会记录冲突）。
 *
 * 字符串一律由模组提供并放在只读段，加载器只保存指针，不复制。
 */

#define LOADER_API_VERSION 11u /* 接口版本，不一致则拒绝加载模组 */

/* ==== 内容种类（引擎固定，不是内容 Id） ==== */

typedef enum {
    KIND_TRAIT = 0,    /* Trait_t       特质 */
    KIND_ITEM,         /* Item          物品 */
    KIND_PET_TYPE,     /* PetType       宠物类型 */
    KIND_ACHIEVEMENT,  /* Achievement_t 成就 */
    KIND_STORY,        /* Story_t       剧情 */
    KIND_EVENT,        /* Event_t       随机事件 */
    KIND_MINIGAME,     /* Minigame_t    互动小游戏 */
    KIND_TERM,         /* TermDef_t     术语 */
    KIND_NPC,          /* Npc_t         NPC */
    KIND_MENU,         /* Menu_t        菜单（界面也由模组提供） */
    KIND_CONFIG,       /* Config_t      可持久化的配置项 */
    KIND_COUNT
} ContentKind_t;

/* ==== 按键编码 ==== */
/* 上下方向键选择，Z 确认，X 返回/取消；C 在对话里查术语（$K 标出的关键词） */
#define KEY_UP     1
#define KEY_DOWN   2
#define KEY_OK     3
#define KEY_CANCEL 4
#define KEY_TERM   5
#define KEY_LEFT   6  /* ←：菜单/对话上页；滑动条减小 */
#define KEY_RIGHT  7  /* →：菜单/对话下页；滑动条增大 */

/* ==== 加载器接口表 ==== */

typedef struct LoaderApi {
    uint32_t size;    /* sizeof(LoaderApi)，用于 ABI 兼容检查 */
    uint32_t version; /* LOADER_API_VERSION */

    /* ---- 内容注册：返回这批内容的第一个 Id，失败返回 ID_NONE ----
     * elem_size 必须等于加载器理解的描述符大小，用于发现 ABI 不一致。 */
    Id (*add)(ContentKind_t kind, const void *list, uint64_t count, uint32_t elem_size);
    const void *(*get)(ContentKind_t kind, Id id);    /* 无效 Id 返回 NULL */
    Id (*find)(ContentKind_t kind, const char *name); /* 按名字查 Id（持久化/热重载） */
    uint64_t (*count)(ContentKind_t kind);            /* 该类内容的数量（遍历用） */

    /* ---- 名字库 ---- */
    uint64_t (*add_names)(Gender_t gender, const char *const *list, uint64_t count);
    const char *(*name_get)(Gender_t gender, uint64_t index); /* 越界返回 NULL */
    uint64_t (*name_count)(Gender_t gender);

    /* ---- 运行期服务（数值 / 背包 / 被动） ---- */
    uint32_t (*rnd)(uint32_t lo, uint32_t hi);         /* 闭区间随机数（32 位） */
    uint64_t (*rnd64)(uint64_t lo, uint64_t hi);       /* 闭区间随机数（64 位，用于动态表下标） */
    const char *(*rand_name)(Gender_t gender, const char *avoid); /* 随机名字，avoid 可空 */
    int (*gen_traits)(Pet *pet);                       /* 摇 1 正 1 负且不冲突的特质，返回数量 */
    int (*inv_add)(Pet *pet, Id item, uint64_t count); /* 加物品，返回1=成功 */
    uint64_t (*inv_count)(const Pet *pet, Id item);    /* 背包里该物品数量 */
    int (*inv_rm)(Pet *pet, Id item, uint64_t count);  /* 扣物品，数量不够返回0 */
    int (*inv_set)(Pet *pet, Id item, uint64_t count); /* 直接设为指定数量（0=清空） */
    int (*passive_try)(Pet *pet);                      /* 掷 chance 并跑被动，返回 PASSIVE_* */
    int (*damage)(Pet *pet, uint32_t amount);          /* 扣血：返回1=造成伤害, 0=被闪避/减伤 */

    /* ---- 运行期服务（界面 / 输入） ---- */
    void (*ui_title)(const char *title);  /* 清屏并画标题 */
    void (*ui_print)(const char *fmt, ...); /* 普通输出（printf 语义） */
    void (*ui_ok)(const char *fmt, ...);   /* 绿色成功消息 */
    void (*ui_err)(const char *fmt, ...);  /* 红色错误消息 */
    void (*ui_warn)(const char *fmt, ...); /* 黄色警告消息 */
    /* 选择菜单：↑↓ 选择，Z 确认，X 返回；返回选中 value，X 返回 -1。
     * 按 header 记住上次停留的项：下次进入同一标题的列表时从原位开始。 */
    int (*ui_menu)(const char *header, SelectOpt_t *opts, uint64_t count);
    /* 写入菜单选项文本：<=14 字节内联，更长自动动态分配；ui_menu 消费后释放 */
    void (*opt_text)(SelectOpt_t *opt, const char *fmt, ...);
    int (*key)(void);           /* 阻塞读键：返回 KEY_*（↑↓ Z X C ← →） */
    void (*delay)(uint32_t ms); /* 停顿 */
    /* 对话 + 可选选项；返回选项 value，无选项/取消返回 0。
     * pages 按顺序播放（没有翻页）：节奏靠文本里的格式符控制，
     *   $L 逻辑换行   $C 清屏   $c 清当前逻辑行   $P 等 Z   $p!T 等 T 毫秒
     * 一页播完若文本自己没等过键，引擎会补一次 Z 等待。 */
    int (*dialog)(const char *speaker, const char *const *pages, uint32_t n_pages,
                  const StoryOpt_t *opts, uint32_t n_opts);

    /* ---- 运行期服务（成长 / 时间 / 规则） ---- */
    int (*ach_unlock)(Pet *pet, Id ach);          /* 解锁成就并发放奖励，返回1=新解锁 */
    int (*ach_unlocked)(const Pet *pet, Id ach);  /* 成就是否已解锁 */
    int (*ach_claimed)(const Pet *pet, Id ach);   /* 成就奖励是否已领 */
    int (*ach_claim)(Pet *pet, Id ach);           /* 标记成就奖励已领，返回1=新标记 */
    void (*ach_merge_global)(Pet *pet);           /* 把跨存档成就并入本档 */
    int (*story_seen)(const Pet *pet, Id story);  /* 剧情是否播过 */
    int (*story_mark)(Pet *pet, Id story);        /* 手动置位剧情标志，返回1=新置位 */
    void (*exp_add)(Pet *pet, int amount);        /* 加经验并处理升级 */
    uint32_t (*rtc_day)(void);                    /* 游戏内天数（10 秒一天） */
    int (*rtc_night)(void);                       /* 是否夜间 */
    int (*trait_conflict)(Id a, Id b);            /* 两个特质是否互斥 */

    /* ---- 菜单（界面注册）：宿主只调 menu_run，不含具体菜单项 ---- */
    /* 渲染 parent 的子菜单并执行选中项；parent=NULL 表示主菜单。
     * 返回 1=已处理（继续），0=返回上一级，-1=请求退出游戏。 */
    int (*menu_run)(Pet *pet, const char *parent);

    /* ---- 内容驱动（模组做菜单时也需要） ---- */
    int (*story_play)(Pet *pet, const char *scene, const char *npc); /* 播匹配场景的第一段剧情 */
    int (*ach_poll)(Pet *pet);                                      /* 轮询成就，返回新解锁数 */
    uint64_t (*minigame_list)(Id pet_type, Id *out, uint64_t max);  /* 列出可玩游戏 */
    int (*minigame_run)(Pet *pet, Id game);                         /* 跑一个小游戏 */

    /* ---- 背包辅助 ---- */
    /* 让玩家从背包里挑一个指定类别的物品；返回物品 Id，取消/没有返回 ID_NONE */
    Id (*inv_pick)(Pet *pet, ItemType_t type, const char *title);

    /* ---- 存档 ---- */
    int (*save)(Pet *pet); /* 1=成功 */
    int (*load)(Pet *pet); /* 1=成功（失败不动 pet） */

    /* ---- 实时输入 / 时钟（限时小游戏用） ---- */
    int (*key_poll)(void);     /* 非阻塞读键：有键返回 KEY_*，无键返回 0 */
    uint32_t (*now_ms)(void);  /* 单调毫秒时钟（首次调用为起点） */

    /* ---- 运行模式 ---- */
    int (*is_headless)(void);  /* 1=无头自检模式（--demo/--auto） */

    /* ---- 配置（按 name 读写，config.dat 持久化） ---- */
    int32_t (*config_get)(const char *name);              /* 未注册/未存过返回 0 */
    int (*config_set)(const char *name, int32_t value);   /* 夹到描述符范围并落盘，1=成功 */

    /* ---- 自定义列表行（菜单选中高亮：背景色+反色，不再用箭头） ---- */
    void (*ui_option)(const char *text, int selected);

    /* ---- 宠物定制（重生 / 祭坛共用） ---- */
    /* 从名字库里手选一个名字；返回只读串，取消返回 NULL */
    const char *(*name_pick)(Gender_t gender, const char *title);
    /* 特质商店：随机加一个（cost_random）/ 手选加一个（cost_pick）/
     * 删除一个（cost_delete，仅 allow_delete 时出现）。
     * 价钱从 pet->coins 扣；没得加 / 钱不够只提示，不扣钱。返回 1=已处理。 */
    int (*trait_shop)(Pet *pet, const char *title, int allow_delete,
                      uint32_t cost_random, uint32_t cost_pick, uint32_t cost_delete);

    /* ---- 文本函数（文本引擎的 $f / $F 调用） ---- */
    /* 注册一组文本函数：id 从 0 开始按注册顺序编号，可以多次调用追加。
     * 每个函数是 TextFunc_t（见 Defineds.h）：
     *   返回 0  → 结果数字写在 *(uint64_t *)ret；
     *   返回 >0 → 结果是长度为该返回值的 UTF-8 串，指针写在 *(const char **)ret。
     * 返回注册后的函数总数；失败返回 UINT64_MAX。 */
    uint64_t (*text_funcs)(const TextFunc_t *list, uint64_t count);
} LoaderApi;

/* ==== 模组侧便捷宏 ==== */

/* 注册整个数组：count / elem_size 自动算 */
#define MOD_ADD(api, kind, list) \
    (api)->add((kind), (list), (uint64_t)(sizeof(list) / sizeof((list)[0])), (uint32_t)sizeof((list)[0]))

/* 按 Id 取回描述符（类型化，避免到处写 cast） */
#define TRAIT_GET(api, id)  ((const Trait_t *)(api)->get(KIND_TRAIT, (id)))
#define ITEM_GET(api, id)   ((const Item *)(api)->get(KIND_ITEM, (id)))
#define PET_GET(api, id)    ((const PetType *)(api)->get(KIND_PET_TYPE, (id)))
#define ACH_GET(api, id)    ((const Achievement_t *)(api)->get(KIND_ACHIEVEMENT, (id)))
#define STORY_GET(api, id)  ((const Story_t *)(api)->get(KIND_STORY, (id)))
#define EVENT_GET(api, id)  ((const Event_t *)(api)->get(KIND_EVENT, (id)))
#define GAME_GET(api, id)   ((const Minigame_t *)(api)->get(KIND_MINIGAME, (id)))
#define TERM_GET(api, id)   ((const TermDef_t *)(api)->get(KIND_TERM, (id)))
#define NPC_GET(api, id)    ((const Npc_t *)(api)->get(KIND_NPC, (id)))
#define MENU_GET(api, id)   ((const Menu_t *)(api)->get(KIND_MENU, (id)))

/* ==== 模组导出约定 ==== */

#define MOD_API_VERSION 11u /* 模组要求的最低接口版本 */

/* 模组信息：加载器先读它，再决定要不要调用 mod_init */
typedef struct {
    const char *name;             /* 模组名（依赖检查用的键），只读串 */
    const char *version;          /* 版本串，只读串 */
    uint32_t api_version;         /* 模组要求的接口版本 */
    const char *const *requires;  /* 依赖：模组名 或 模组名:种类/名字，可 NULL */
    uint64_t require_count;       /* requires 的元素个数 */
} ModInfo_t;

/* 声明依赖：MOD_REQUIRES(REQ) 自动算个数，直接放进 ModInfo_t 初值 */
#define MOD_REQUIRES(list) (list), (uint64_t)(sizeof(list) / sizeof((list)[0]))

#if defined(_WIN32)
#  define MOD_EXPORT __declspec(dllexport)
#else
#  define MOD_EXPORT
#endif

/* 每个模组必须导出这三个入口 */
MOD_EXPORT const ModInfo_t *mod_info(void);
MOD_EXPORT int mod_init(const LoaderApi *api); /* 返回1=成功，0=失败放弃加载 */
MOD_EXPORT void mod_exit(void);

/* 可选入口：mod_init 返回 0 后，加载器调用它取详细失败原因（返回只读串）。
 * 建议每个模组都实现，这样启动报错里能看到“哪一步失败了、为什么”。 */
MOD_EXPORT const char *mod_error(void);

#endif
