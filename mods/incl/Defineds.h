#ifndef DEFINEDS_H
#define DEFINEDS_H

#include <stdint.h>

/**
 * @file Defineds.h
 * @brief 模组数据接口（只定义数据结构，不含实现，不注册任何内容）
 *
 * 本文件是“加载器 ↔ 模组”之间的纯数据契约，只回答“数据长什么样”。
 * 所有内容都由模组在运行时注册，可注册的种类共十一种：
 *
 *   特质 Trait_t        物品 Item          宠物类型 PetType
 *   成就 Achievement_t  剧情 Story_t       随机事件 Event_t
 *   小游戏 Minigame_t   菜单 Menu_t        配置 Config_t
 *   NPC  Npc_t          术语 TermDef_t
 *
 *   1. 一切 id 都是运行时注册的句柄（Id），本文件不声明内容枚举、
 *      也不声明数据表；每个模组可以只注册其中任意几种内容。
 *   2. Id 仅本次运行有效，只记录名字用于持久化：存档写名字，读档时
 *      按名字重新查回 Id。
 *   3. 所有字符串都是 const char*，指向模组 DLL 只读段（.rdata）里的
 *      字面量：不拷贝、不分配、不放进结构体内部。
 *   4. 跨内容引用一律用名字（成就奖励写物品名、剧情分支写 NPC 名、
 *      小游戏写宠物类型名），由加载器运行时解析。
 */

/* ==== 前置声明 ==== */

/* 宠物状态（定义见文末；回调只拿指针即可） */
typedef struct Pet Pet;

/* ==== 运行时 ID ==== */

/*
 * 内容句柄：由加载器按注册顺序分配，0 表示无效/空槽，注册从 1 开始。
 * 每种内容各自编号（特质 1..、物品 1..、成就 1..），互不干扰。
 */
typedef uint64_t Id;

#define ID_NONE ((Id)0) /* 无效 / 空槽 */

/* ==== 变长位图 ==== */

/*
 * 第 n 条内容占第 (n/64) 个字的第 (n%64) 位，数量不设上限。
 * 由加载器按需扩容（realloc），模组只通过 LoaderApi 的服务读写，
 * 不要自行分配/释放；整只宠物用完调 modloader_pet_release()。
 */
typedef struct {
    uint64_t *words; /* 位数据，NULL=空 */
    uint64_t  count; /* 已分配的 uint64 个数 */
} BitSet_t;

/* ==== 基础枚举（引擎固定语义，不是注册内容） ==== */

/* 性别：顺序与名字库分组一致 */
typedef enum { G_MALE, G_FEMALE, G_NEUTER } Gender_t;

/* 年龄阶段 */
typedef enum { S_BABY, S_CHILD, S_ADULT } Stage_t;

/* 疾病 */
typedef enum { D_NONE, D_HUNGRY, D_DIRTY, D_TIRED, D_OLD } Disease_t;

/* ==== 常量（状态模型的结构边界） ==== */

#define MIN_STAT     0   /* 属性最小值 */
#define MAX_STAT     255 /* 属性最大值（uint8_t 上限） */

/* 摇特质固定 1 正 1 负；这不是数组容量，特质数组按需动态扩容 */
#define MAX_TRAITS   2

/* 动态数组的初始容量（之后按 2 倍扩容，上限由 uint64_t 决定） */
#define PET_INITIAL_TRAITS 4u
#define PET_INITIAL_ITEMS  8u

/* 菜单文本：短串 15 字节（含结尾 NUL）；长串用动态分配 */
#define TEXT_SSO 15
#define TEXT_MAX 0x7FFFFFFFu /* 长串长度上限（0x0-0x7fffffff） */

/* 每日任务标志（Pet.task_complete 的位） */
#define TASK_BIT_LOGIN 0x01 /* 已登录 */
#define TASK_BIT_FEED  0x02 /* 喂食满3次 */
#define TASK_BIT_PLAY  0x04 /* 玩耍满3次 */

/* ==== 特质 ==== */

/* 特质类型：加载器据此保证摇出 1 正面 + 1 负面 */
typedef enum { T_NULL, T_POS, T_NEG, T_MID } TraitType_t;

/* 特质触发回调函数：返回1=已生效，0=无效果 */
typedef int (*OnTraitUsed)(Pet *pet);

/* 特质（模组只读描述符，效果写在 on_used 里） */
typedef struct {
    OnTraitUsed on_used;
    TraitType_t type;
    const char *name;     /* 只读串，同时是持久化用的键 */
    const char *conflict; /* 互斥特质名（只读串），NULL=无 */
} Trait_t;

/* ==== 物品 ==== */

/* 物品类型（分类，决定使用方式；I_TOY/I_DECORATION 为耐用品） */
typedef enum { I_NULL, I_TOY, I_DECORATION, I_FOOD, I_MEDICINE, I_CLEANING, I_TRAVEL } ItemType_t;

/* 物品使用回调函数：返回1=使用成功，0=失败 */
typedef int (*OnItemUsed)(Pet *pet);

/* 物品（模组只读描述符，效果写在 on_used 里） */
typedef struct {
    OnItemUsed on_used;
    ItemType_t type;
    uint32_t price;
    const char *name; /* 只读串，同时是持久化用的键 */
} Item;

/* ==== 被动技能 ==== */

/* 被动技能回调返回值 */
#define PASSIVE_FAIL  0 /* 未生效 */
#define PASSIVE_OK    1 /* 已生效（纯增益） */
#define PASSIVE_GUARD 2 /* 抵消负面效果（闪避/减伤）：引擎按此减免伤害 */

/* 被动技能回调函数：引擎按 chance 掷骰后调用 */
typedef int (*OnPassiveTrigger)(Pet *pet);

/* 被动技能（效果写在 on_trigger 里，不再有被动类型枚举） */
typedef struct {
    OnPassiveTrigger on_trigger;
    uint8_t chance;   /* 触发概率(%) */
    const char *name; /* 只读串 */
} Passive_t;

/* ==== 宠物类型 ==== */

/* 宠物类型（各模组注册自己的种类） */
typedef struct {
    Passive_t passive;       /* 内嵌被动，没被动就填 on_trigger = NULL */
    const char *name;        /* 只读串，同时是持久化用的键 */
    const char *describtion; /* 只读串 */
} PetType;

/* ==== 背包 ==== */

/* 背包槽位（只存引用，不存物品本体） */
typedef struct {
    Id id;          /* 物品Id，ID_NONE=空槽 */
    uint64_t count; /* 数量 */
} InvSlot_t;

/* ==== 宠物 ==== */

/**
 * 宠物状态模型（运行期状态，只存 Id 与数值）
 *
 * 种类、特质、物品一律只存 Id，要描述符或回调时向加载器查表；
 * 名字/死亡原因是指向只读串的指针，所以本结构体不做整体二进制存档，
 * 存档由持久化层按名字序列化。
 */
struct Pet {
    /* ---- 身份 ---- */
    Id type_id;                  /* 宠物类型Id */
    Gender_t gender;             /* 性别 */
    const char *name;            /* 指向名字库里的只读串 */

    /* ---- 基础属性（MIN_STAT ~ MAX_STAT） ---- */
    uint8_t hunger;              /* 饱食度 */
    uint8_t clean;               /* 清洁度 */
    uint8_t happy;               /* 心情值 */
    uint8_t health;              /* 健康值 */
    uint8_t energy;              /* 活力值 */
    uint16_t age;                /* 年龄(天) */
    Stage_t stage;               /* 阶段 */
    uint8_t personality[3];      /* 性格：魅力, 聪明, 勇敢 */

    /* ---- 特质（动态数组，容量按需扩容） ---- */
    Id *traits;                  /* 特质Id数组，NULL=未分配 */
    uint64_t trait_cnt;          /* 特质数量 */
    uint64_t trait_cap;          /* 已分配容量 */

    /* ---- 资源 / 背包（动态数组，容量按需扩容） ---- */
    uint32_t coins;              /* 鲁尼（货币） */
    InvSlot_t *inv;              /* 背包槽位数组，NULL=未分配 */
    uint64_t inv_cnt;            /* 已用槽位数 */
    uint64_t inv_cap;            /* 已分配容量 */

    /* ---- 状态 ---- */
    Disease_t sick;              /* 疾病 */
    uint8_t dead;                /* 是否死亡 */
    const char *death_reason;    /* 指向只读串 */
    uint8_t rebellious;          /* 叛逆期 */
    uint8_t lost_friend;         /* 曾经失去过伴兽：新宠物继承，旧友的信条件 */
    uint8_t sleeping;            /* 睡眠状态 */

    /* ---- 成长 ---- */
    uint32_t exp;                /* 经验值 */
    uint8_t level;               /* 等级 */
    uint16_t login_days;         /* 登录天数 */
    uint8_t task_feed;           /* 今日喂食次数 */
    uint8_t task_play;           /* 今日玩耍次数 */
    uint8_t task_complete;       /* 任务完成标志位（TASK_BIT_*） */

    /* ---- 成就 / 剧情（变长位图：bit = 注册Id，数量不限） ---- */
    BitSet_t ach;                /* 已完成成就位 */
    BitSet_t ach_claim;          /* 已领取奖励位 */
    BitSet_t story_flag;         /* 剧情标志位 */

    /* ---- 成就统计 ---- */
    uint16_t total_feed;         /* 累计喂食次数 */
    uint16_t total_play;         /* 累计玩耍次数 */
    uint16_t total_visit;        /* 累计拜访次数 */
    uint16_t total_win;          /* 累计对战胜利次数 */
    uint16_t total_gift_ok;      /* 累计送礼被接受次数 */
    uint16_t hungry_streak;      /* 当前饿着喂食连续次数 */
    uint16_t win_streak;         /* 当前对战连胜次数 */
    uint16_t lose_streak;        /* 当前对战连败次数 */
    uint8_t trait_reroll_cnt;    /* 领养时特质重摇次数（隐藏成就用） */
    uint32_t big_spend;          /* 单次消费最大金额（隐藏成就用） */
};

/* ==== 成就 ==== */

/* 成就奖励条目：物品按名字引用，加载器在发奖时解析成 Id */
typedef struct {
    const char *item; /* 物品名（只读串） */
    uint16_t count;   /* 数量 */
} AchReward_t;

/*
 * 成就：达成条件写在 on_check 里，奖励是纯数据。
 * on_check 为 NULL 表示“条件由引擎判定”（如剧情性成就），只按名字强制解锁。
 */
typedef struct {
    const char *name;                /* 只读串，同时是持久化用的键 */
    const char *desc;                /* 只读串 */
    uint8_t hidden;                  /* 1=达成前显示 ??? */
    int (*on_check)(const Pet *pet); /* 返回1=达成 */
    const AchReward_t *rewards;      /* 可空 */
    uint32_t reward_count;
} Achievement_t;

/* ==== 剧情 ==== */

/* 剧情选项（分页播完后出现，value 回传给 on_play） */
typedef struct {
    const char *text; /* 选项文字，只读串 */
    int value;        /* 选项值 */
} StoryOpt_t;

/*
 * 一段对话：pages 里可用 %NAME% 占位当前宠物名、%TYPE% 占位宠物种类，由引擎替换。
 * 每个元素是一段文本，按顺序播放；节奏用格式符控制（$L 逻辑换行、$C 清屏、$P 等 Z、$p 停顿）。
 * 文本里的 \n 只是同一逻辑行里的普通换行，$c 清行时会把整条逻辑行（含这些 \n）一起清掉。
 */
typedef struct {
    const char *npc;          /* NULL=通用；否则只在当前 NPC 是该角色时播放 */
    const char *speaker;      /* 说话人显示名 */
    const char *const *pages; /* 顺序播放的文本段 */
    uint32_t page_count;
} Dialog_t;

/*
 * 剧情节点：引擎在匹配的场景里检查，条件满足且未播放过就播放。
 * 播放完成后引擎把 story_flag 的 (1 << 剧情Id) 置位（持久化按名字）。
 *
 * opts 非空时，所有对话播完后再弹选项，选择结果传给 on_play。
 * 需要循环问答、多步流程的剧情（领养问答），在 on_play 里用加载器的
 * dialog() 服务自己跑流程。
 */
typedef struct {
    const char *name;                /* 剧情名：持久化 + 标志位 */
    const char *scene;               /* 触发场景标签，NULL=任意场景 */
    int (*on_check)(const Pet *pet); /* 返回1=条件满足；NULL=无条件 */
    const Dialog_t *dialogs;         /* 按顺序播放，NPC 不匹配的跳过 */
    uint32_t dialog_count;
    const StoryOpt_t *opts;          /* 可空 */
    uint32_t opt_count;
    int (*on_play)(Pet *pet, int choice); /* 播完后的效果；choice=0 表示无选项 */
} Story_t;

/* ==== 随机事件 ==== */

/* 随机事件：引擎按 weight 加权抽取，命中后显示 text 并调用 on_fire */
typedef struct {
    const char *name;          /* 只读串 */
    const char *text;          /* 默认旁白（只读串，可用 {pet}） */
    uint16_t weight;           /* 相对权重，0=停用 */
    int (*on_fire)(Pet *pet);  /* 效果，可空 */
} Event_t;

/* ==== 互动小游戏 ==== */

/*
 * 小游戏：run 里用加载器的 ui_* / key() / delay() 服务跑完整流程，
 * 直接改 pet 属性作为奖励。返回1=玩成功（用于经验/统计）。
 * pet_type 是绑定关系：NULL=通用玩具游戏，否则只有该种类能玩。
 */
typedef struct {
    const char *name;       /* 只读串 */
    const char *desc;       /* 一句话说明 */
    const char *pet_type;   /* 宠物类型名，NULL=通用 */
    int (*run)(Pet *pet);   /* 完整流程，返回1=成功 */
} Minigame_t;

/* ==== 菜单 ==== */

/*
 * 菜单回调：返回 1=已处理（继续），0=返回上一级，-1=请求退出游戏。
 * 菜单本身也是内容：模组注册 Menu_t，宿主只负责调度，不含具体菜单项。
 */
typedef int (*OnMenuRun)(Pet *pet);

/*
 * 一个菜单项。父子关系用名字（parent）表达：
 *   parent = NULL → 出现在主菜单；
 *   parent = "出门" → 出现在“出门”的子菜单里。
 * run = NULL 表示纯容器：选中后进入它的子菜单。
 */
typedef struct {
    const char *name;   /* 菜单唯一名，也是子菜单引用的键 */
    const char *label;  /* 菜单里显示的文字；NULL 表示用 name */
    const char *parent; /* 父菜单名；NULL = 主菜单 */
    uint32_t order;     /* 排序权重，小的在前；相同则按注册顺序 */
    uint32_t flags;     /* MENU_FLAG_* */
    OnMenuRun run;      /* 执行回调；NULL = 容器 */
} Menu_t;

#define MENU_FLAG_NONE 0u
#define MENU_FLAG_EXIT 1u /* 选中后结束游戏（“退出”） */
#define MENU_FLAG_STAY 2u /* 页面型：回调返回 1 留在本页重画，返回 0 才回上一级 */

/* ==== 配置 ==== */

/* 配置值类型：开关（0/1）、列表选择（min~max）、滑动条（min~max） */
typedef enum { CFG_BOOL = 0, CFG_INT = 1, CFG_SLIDER = 2 } ConfigType_t;

/*
 * 一个可配置项。配置页按注册顺序列出；值按 name 存在 config.dat，
 * 没有存过就用 def（并夹到 min~max）。name 是持久化键，改名字等于丢配置。
 */
typedef struct {
    const char *name;  /* 持久化键，建议 "模块.项"，全局唯一 */
    const char *label; /* 配置页显示名 */
    const char *desc;  /* 说明，可空 */
    ConfigType_t type; /* CFG_BOOL / CFG_INT */
    int32_t def;       /* 默认值 */
    int32_t min;       /* 最小值（BOOL 固定 0/1） */
    int32_t max;       /* 最大值（BOOL 固定 0/1） */
} Config_t;

/* ==== NPC ==== */

/* NPC（剧情与社群用，字符串全部指向只读段） */
typedef struct {
    const char *name;     /* 只读串，同时是剧情分支引用的键 */
    const char *desc;     /* 主控的主观简介 */
    const char *greet;    /* 打招呼 */
    const char *intro;    /* 介绍宠物 */
    const char *farewell; /* 道别 */
} Npc_t;

/* ==== 文本 / 界面数据 ==== */

/* 术语条目（文本里 $K词$k 高亮，对话里按 C 键查解释） */
typedef struct {
    const char *name;
    const char *desc;
} TermDef_t;

/*
 * 文本函数：文本引擎的 $f / $F 用（模组用 LoaderApi.text_funcs 注册，id 从 0 开始）。
 * ret 是引擎给的结果缓冲区，约定：
 *   返回 0   → 结果是数字，写在 *(uint64_t *)ret 里；
 *   返回 >0  → 结果是长度为该返回值的 UTF-8 串，指针写在 *(const char **)ret 里。
 * $f!T 把结果当文本插入；$F!T!L 把结果当逻辑块 L 的项目下标（超出项目数就取模）。
 */
typedef uint64_t (*TextFunc_t)(Pet *pet, void *ret);

/*
 * 菜单文本：16 字节的带标记字符串。
 *   短串：tag = 0x80，后接 15 字节定长串（含 NUL，最多 14 字节可见文本）；
 *   长串：uint64 大端长度（0x0-0x7fffffff，首字节为 0） + char* 动态数组。
 * 由加载器的 opt_text() 写入、ui_menu() 消费并释放。
 */
typedef struct {
    union {
        struct {
            uint8_t tag;         /* 0x80 = 短串 */
            char str[TEXT_SSO];  /* 15 字节定长串（含 NUL） */
        } s;
        struct {
            uint64_t size_be;    /* 长度，大端存放，0x0-0x7fffffff */
            char *data;          /* 动态分配的文本（含 NUL） */
        } b;
    } u;
} Text_t;

/* 菜单选项（文本是运行期拼出来的，由 opt_text 写入） */
typedef struct {
    Text_t text; /* 显示文本：短串内联，长串动态 */
    int value;   /* 选项值 */
} SelectOpt_t;

/* ==== 逝去记录 ==== */

/*
 * 离世宠物的快照：只留展示用字段，不持有位图/背包等运行期资源，
 * 所以可以安全地按值复制、长期保存。
 */
typedef struct {
    const char *name;         /* 名字（只读串） */
    const char *type_name;    /* 种类名（只读串） */
    const char *death_reason; /* 死因（只读串） */
    uint16_t age;             /* 陪伴了几天 */
    uint8_t level;            /* 最终等级 */
    Gender_t gender;          /* 性别 */
    uint32_t coins;           /* 留下的鲁尼 */
} DeadPet_t;

#define MAX_DEAD 5 /* 保留最近 5 条逝去记录 */

/* ==== 联机模拟 ==== */

/* 附近宠物（宠物中心扫描结果，后期替换为2.4G扫描） */
typedef struct {
    const char *name; /* 指向名字库只读串 */
    Id type_id;       /* 宠物类型Id */
    uint8_t level;    /* 等级 */
    Gender_t gender;  /* 性别 */
    uint8_t charm;    /* 魅力值，用于互动判定 */
} NearbyPet_t;

/* ==== 注册契约（仅供参考，代码在加载器与各模组里） ====
 *
 *  模组侧：把描述符数组放在只读段，字符串全是字面量
 *
 *      static const Trait_t MY_TRAITS[] = {
 *          {trait_lively_used, T_POS, "活泼"},
 *          {trait_lazy_used,   T_NEG, "懒惰"},
 *      };
 *      static int mod_init(const LoaderApi *api)  // 只注册自己有的内容
 *      {
 *          api->add(KIND_TRAIT, MY_TRAITS, 2, sizeof(Trait_t));
 *          return 1;
 *      }
 *
 *  加载器侧：注册时按种类各自分配 Id（从1开始），建立 Id → 描述符 的表，
 *  并把“名字 → Id”记进持久化索引；读档时用名字查回 Id。
 *  多个模组可以往同一类内容里注册，加载顺序决定 Id 顺序，同名以先注册为准。
 *
 *  逻辑层：只拿 Id 查表，例如
 *      pet->traits[0] → 查表得到 const Trait_t * → 调 on_used(pet)
 *  需要显示文字时直接用 name 指针，不复制字符串。
 */

#endif
