/*
 * 网络电台台单：内置台、频率解析、目录查询 URL、响应解析。
 *
 * 逻辑与 leo-radio 的 main/radio_catalog.cc 一一对应，只是改成 C 并适配本工程
 * 的 cJSON（CONFIG_CJSON）。这里没有任何网络调用，HTTP 在 radio_net.c 里。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "radio.catalog"
#include "lisa_log.h"

#include "cJSON.h"
#include "radio_catalog.h"

/* ==================== 常量 ==================== */

#define RADIO_DIRECTORY_BASE "http://de1.api.radio-browser.info/json/stations/search"
/* 目录里多要几条，因为 parse 会把 HLS / 宕机 / 非 MP3 的条目过滤掉 */
#define RADIO_DIRECTORY_EXTRA 3
#define RADIO_DIRECTORY_LIMIT 12

/* 内置台（与 leo-radio 的 kFallbackStations 逐条一致，顺序也一致）。
 * 这些是全国性直播流，不对应某个本地发射台，所以没有刻度频率。 */
static const radio_station_t s_builtin[] = {
    {.name = "中国之声", .full = "中国之声 · 新闻综合", .desc = "新闻 · MP3",
     .lang = "国语", .url = "http://lhttp.qingting.fm/live/15318317/64k.mp3"},
    {.name = "怀旧音乐", .full = "怀旧音乐 · 经典老歌", .desc = "怀旧 · MP3",
     .lang = "国语", .url = "http://lhttp.qingting.fm/live/4804/64k.mp3"},
    {.name = "清晨音乐", .full = "清晨音乐 · 轻松旋律", .desc = "轻音乐 · MP3",
     .lang = "国语", .url = "http://lhttp.qingting.fm/live/4915/64k.mp3"},
    {.name = "两广之声", .full = "两广之声 · 粤语音乐", .desc = "粤语 · MP3",
     .lang = "粤语", .url = "http://lhttp.qingting.fm/live/20500149/64k.mp3"},
    {.name = "羊城交通", .full = "羊城交通 · 城市资讯", .desc = "交通 · MP3",
     .lang = "粤语", .url = "http://lhttp.qingting.fm/live/1262/64k.mp3"},
    {.name = "第一财经", .full = "第一财经 · 财经资讯", .desc = "财经 · MP3",
     .lang = "国语", .url = "http://lhttp.qingting.fm/live/276/64k.mp3"},
};

/* 城市表（与 leo-radio 的 kCityChoices 一致；query 为 NULL 表示自动定位） */
static const radio_city_t s_cities[] = {
    {NULL, "自动定位"},
    {"北京", "北京"},
    {"上海", "上海"},
    {"广州", "广州"},
    {"深圳", "深圳"},
    {"长沙", "长沙"},
    {"杭州", "杭州"},
    {"成都", "成都"},
};

/* ip-api 返回英文城市名，而 radio-browser 的中文条目按中文城市名更好找，
 * 所以常见城市做一次映射；其余走省/州（region）查询。 */
typedef struct {
    const char *english;
    const char *chinese;
} city_translation_t;

static const city_translation_t s_city_translations[] = {
    {"Beijing", "北京"},        {"Shanghai", "上海"},
    {"Tianjin", "天津"},        {"Chongqing", "重庆"},
    {"Guangzhou", "广州"},      {"Shenzhen", "深圳"},
    {"Dongguan", "东莞"},       {"Foshan", "佛山"},
    {"Zhuhai", "珠海"},         {"Huizhou", "惠州"},
    {"Zhongshan", "中山"},      {"Shantou", "汕头"},
    {"Hangzhou", "杭州"},       {"Ningbo", "宁波"},
    {"Wenzhou", "温州"},        {"Jiaxing", "嘉兴"},
    {"Shaoxing", "绍兴"},       {"Jinhua", "金华"},
    {"Nanjing", "南京"},        {"Suzhou", "苏州"},
    {"Wuxi", "无锡"},           {"Changzhou", "常州"},
    {"Nantong", "南通"},        {"Xuzhou", "徐州"},
    {"Chengdu", "成都"},        {"Mianyang", "绵阳"},
    {"Wuhan", "武汉"},          {"Yichang", "宜昌"},
    {"Changsha", "长沙"},       {"Zhengzhou", "郑州"},
    {"Luoyang", "洛阳"},        {"Jinan", "济南"},
    {"Qingdao", "青岛"},        {"Yantai", "烟台"},
    {"Shenyang", "沈阳"},       {"Dalian", "大连"},
    {"Harbin", "哈尔滨"},       {"Changchun", "长春"},
    {"Shijiazhuang", "石家庄"}, {"Taiyuan", "太原"},
    {"Xi'an", "西安"},          {"Xian", "西安"},
    {"Kunming", "昆明"},        {"Guiyang", "贵阳"},
    {"Nanning", "南宁"},        {"Haikou", "海口"},
    {"Fuzhou", "福州"},         {"Xiamen", "厦门"},
    {"Nanchang", "南昌"},       {"Hefei", "合肥"},
    {"Lanzhou", "兰州"},        {"Urumqi", "乌鲁木齐"},
    {"Hohhot", "呼和浩特"},     {"Yinchuan", "银川"},
    {"Xining", "西宁"},         {"Lhasa", "拉萨"},
};

/* ==================== 小工具 ==================== */

static bool starts_with(const char *text, const char *prefix)
{
    return text != NULL && prefix != NULL && strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool contains_text(const char *text, const char *needle)
{
    return text != NULL && needle != NULL && strstr(text, needle) != NULL;
}

static void copy_text(char *target, size_t capacity, const char *source)
{
    if (target == NULL || capacity == 0) {
        return;
    }
    snprintf(target, capacity, "%s", (source != NULL) ? source : "");
}

/* UTF-8 安全截断：不会把多字节字符切成半截 */
static void copy_text_utf8(char *target, size_t capacity, const char *source)
{
    size_t len;

    if (target == NULL || capacity == 0) {
        return;
    }
    if (source == NULL) {
        target[0] = '\0';
        return;
    }
    len = strlen(source);
    if (len >= capacity) {
        len = capacity - 1;
        while (len > 0 && (((unsigned char)source[len]) & 0xC0) == 0x80) {
            --len;
        }
    }
    memcpy(target, source, len);
    target[len] = '\0';
}

static bool ascii_only(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p >= 0x80) {
            return false;
        }
    }
    return true;
}

static void url_encode(const char *source, char *target, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    if (target == NULL || capacity == 0) {
        return;
    }
    target[0] = '\0';
    for (const unsigned char *p = (const unsigned char *)source; p != NULL && *p != '\0'; ++p) {
        const unsigned char value = *p;
        if (isalnum((int)value) || value == '-' || value == '_' || value == '.' || value == '~') {
            if (out + 1 >= capacity) {
                break;
            }
            target[out++] = (char)value;
        } else {
            if (out + 3 >= capacity) {
                break;
            }
            target[out++] = '%';
            target[out++] = hex[value >> 4];
            target[out++] = hex[value & 0x0F];
        }
    }
    target[out] = '\0';
}

/* ==================== 频率解析 ==================== */

static bool in_band(long decihz)
{
    return decihz >= RADIO_BAND_LOW_DECIHZ && decihz <= RADIO_BAND_HIGH_DECIHZ;
}

static bool is_digit_char(char value)
{
    return value >= '0' && value <= '9';
}

/* 读 "94.7" / "1017" 这类数字，返回 0.1MHz 单位的值；
 * consumed 返回吃掉的字符数，失败时为 0。 */
static long read_decihz(const char *text, size_t *consumed)
{
    size_t whole = 0;
    long value = 0;

    if (consumed != NULL) {
        *consumed = 0;
    }
    while (is_digit_char(text[whole])) {
        ++whole;
    }
    if (whole < 2 || whole > 4) {
        return 0;
    }
    for (size_t i = 0; i < whole; ++i) {
        value = value * 10 + (text[i] - '0');
    }

    if (text[whole] == '.' && is_digit_char(text[whole + 1])) {
        if (whole > 3 || is_digit_char(text[whole + 2])) {
            return 0;
        }
        if (consumed != NULL) {
            *consumed = whole + 2;
        }
        return value * 10 + (text[whole + 1] - '0');
    }

    if (consumed != NULL) {
        *consumed = whole;
    }
    if (whole == 2) {
        return value * 10; /* "94" 视为 94.0 */
    }
    if (in_band(value)) {
        return value; /* "971" → 97.1 */
    }
    if (in_band(value * 10)) {
        return value * 10; /* "101" → 101.0 */
    }
    return value;
}

uint16_t radio_catalog_parse_frequency(const char *name)
{
    if (name == NULL) {
        return 0;
    }

    /* 1) 显式 "FM" 前缀 */
    for (const char *p = name; p[0] != '\0' && p[1] != '\0'; ++p) {
        if ((p[0] != 'F' && p[0] != 'f') || (p[1] != 'M' && p[1] != 'm')) {
            continue;
        }
        const char *digits = p + 2;
        while (*digits == ' ') {
            ++digits;
        }
        size_t consumed = 0;
        const long value = read_decihz(digits, &consumed);
        if (in_band(value)) {
            return (uint16_t)value;
        }
    }

    /* 2) 裸数字，且不能紧贴其他数字或小数点 */
    for (const char *p = name; *p != '\0'; ++p) {
        if (!is_digit_char(*p)) {
            continue;
        }
        const bool starts_run = (p == name) || (!is_digit_char(p[-1]) && p[-1] != '.');
        if (!starts_run) {
            continue;
        }
        size_t consumed = 0;
        const long value = read_decihz(p, &consumed);
        if (consumed != 0 && in_band(value)) {
            return (uint16_t)value;
        }
        while (is_digit_char(p[1])) {
            ++p;
        }
    }
    return 0;
}

void radio_catalog_format_frequency(uint16_t decihz, char *out, size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return;
    }
    if (decihz == 0) {
        out[0] = '\0';
        return;
    }
    snprintf(out, capacity, "%u.%u", (unsigned)(decihz / 10u), (unsigned)(decihz % 10u));
}

/* ==================== 显示名 ==================== */

/* 目录里的台名大多带中文运营商前缀（例如 "某某广播 FM97.5"），
 * 屏上一行放不下；能解析到频率时统一显示成 "<城市> FM97.5"，
 * 否则退化成 "<城市> 电台 <序号>"。纯 ASCII 名字原样保留。 */
static void make_display_name(char *target, size_t capacity, const char *raw_name,
                              const char *city, size_t sequence)
{
    if (ascii_only(raw_name)) {
        copy_text(target, capacity, raw_name);
        return;
    }

    const char *frequency = NULL;
    for (const char *p = raw_name; p[0] != '\0' && p[1] != '\0'; ++p) {
        if ((p[0] == 'F' || p[0] == 'f') && (p[1] == 'M' || p[1] == 'm')) {
            frequency = p;
            break;
        }
    }

    if (frequency != NULL) {
        char suffix[20] = {0};
        size_t length = 0;
        while (frequency[length] != '\0' && length + 1 < sizeof(suffix)) {
            const unsigned char value = (unsigned char)frequency[length];
            if (value >= 0x80 ||
                (!isalnum((int)value) && value != '.' && value != '-' && value != ' ')) {
                break;
            }
            suffix[length] = frequency[length];
            ++length;
        }
        while (length > 0 && suffix[length - 1] == ' ') {
            suffix[--length] = '\0';
        }
        if (length >= 2) {
            snprintf(target, capacity, "%s %s", (city != NULL) ? city : "", suffix);
            return;
        }
    }

    /* 解析不到频率：直接用目录原名（之前退化成“城市 电台 N”，信息量为零） */
    copy_text_utf8(target, capacity, raw_name);
    (void)sequence;
}

/* ==================== 内置台单 ==================== */

size_t radio_catalog_builtin(radio_station_t *out, size_t capacity)
{
    const size_t total = sizeof(s_builtin) / sizeof(s_builtin[0]);
    size_t count = 0;

    if (out == NULL || capacity == 0) {
        return 0;
    }
    for (size_t i = 0; i < total && count < capacity; ++i) {
        out[count++] = s_builtin[i];
    }
    return count;
}

const char *radio_catalog_builtin_source(void)
{
    return "内置";
}

/* ==================== 城市表 ==================== */

size_t radio_catalog_city_count(void)
{
    return sizeof(s_cities) / sizeof(s_cities[0]);
}

const radio_city_t *radio_catalog_city(size_t index)
{
    if (index >= radio_catalog_city_count()) {
        return NULL;
    }
    return &s_cities[index];
}

/* ==================== 目录 URL ==================== */

static const char *country_or_default(const char *country)
{
    return (country != NULL && country[0] != '\0') ? country : "CN";
}

int radio_catalog_build_name_query(const char *country, const char *name,
                                   char *out, size_t capacity)
{
    char encoded_country[16] = {0};
    char encoded[192] = {0};
    int written;

    if (out == NULL || capacity == 0 || name == NULL || name[0] == '\0') {
        return -1;
    }

    url_encode(country_or_default(country), encoded_country, sizeof(encoded_country));
    url_encode(name, encoded, sizeof(encoded));
    written = snprintf(out, capacity,
                       "%s?countrycode=%s&name=%s&codec=MP3&hidebroken=true"
                       "&order=clickcount&reverse=true&limit=%u",
                       RADIO_DIRECTORY_BASE, encoded_country, encoded,
                       (unsigned)RADIO_DIRECTORY_LIMIT);
    return (written > 0 && (size_t)written < capacity) ? 0 : -1;
}

int radio_catalog_build_region_query(const char *country, const char *region,
                                     char *out, size_t capacity)
{
    char encoded_country[16] = {0};
    char encoded[192] = {0};
    int written;

    if (out == NULL || capacity == 0 || region == NULL || region[0] == '\0') {
        return -1;
    }

    url_encode(country_or_default(country), encoded_country, sizeof(encoded_country));
    url_encode(region, encoded, sizeof(encoded));
    written = snprintf(out, capacity,
                       "%s?countrycode=%s&state=%s&codec=MP3&hidebroken=true"
                       "&order=clickcount&reverse=true&limit=%u",
                       RADIO_DIRECTORY_BASE, encoded_country, encoded,
                       (unsigned)(RADIO_DIRECTORY_LIMIT));
    return (written > 0 && (size_t)written < capacity) ? 0 : -1;
}

/* ==================== 台单副标题 ==================== */

/* radio-browser 的 tags 是英文（"news,talk"、"classical"、"jazz"），
 * 能认出来的翻成中文，认不出的就不展示——在 240 宽的屏上混排英文更难看。 */
static const struct {
    const char *en;
    const char *cn;
} s_tag_names[] = {
    {"news", "新闻"},        {"talk", "谈话"},
    {"music", "音乐"},       {"pop", "流行"},
    {"rock", "摇滚"},        {"jazz", "爵士"},
    {"classical", "古典"},   {"dance", "舞曲"},
    {"oldies", "怀旧"},      {"sports", "体育"},
    {"culture", "文化"},     {"traffic", "交通"},
    {"business", "财经"},    {"finance", "财经"},
    {"children", "少儿"},    {"comedy", "相声"},
    {"folk", "民谣"},        {"chinese", "华语"},
    {"mandarin", "国语"},    {"cantonese", "粤语"},
    {"easy listening", "轻音乐"}, {"instrumental", "器乐"},
    {"hits", "金曲"},        {"top 40", "榜单"},
    {"local", "本地"},       {"lifestyle", "生活"},
    {"entertainment", "娱乐"}, {"variety", "综艺"},
    {"education", "教育"},   {"public", "公共"},
    {"country", "乡村"},     {"blues", "蓝调"},
    {"electronic", "电子"},  {"reggae", "雷鬼"},
    {"soundtrack", "影视原声"}, {"religious", "宗教"},
    {"60s", "六十年代"},     {"70s", "七十年代"},
    {"80s", "八十年代"},     {"90s", "九十年代"},
};

static bool text_equal_ci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        const char ca = (char)tolower((int)(unsigned char)*a);
        const char cb = (char)tolower((int)(unsigned char)*b);
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* 返回标签的中文名；不认识的返回 NULL */
static const char *translate_tag(const char *tag, size_t len)
{
    char key[24];

    if (len == 0 || len >= sizeof(key)) {
        return NULL;
    }
    memcpy(key, tag, len);
    key[len] = '\0';

    for (size_t i = 0; i < sizeof(s_tag_names) / sizeof(s_tag_names[0]); ++i) {
        if (text_equal_ci(key, s_tag_names[i].en)) {
            return s_tag_names[i].cn;
        }
    }
    return NULL;
}

/* 用 tags / codec / bitrate / votes 拼副标题："新闻 · 谈话 · MP3 64k"
 * 没有可用标签时用目录的 votes（收听热度）顶上，比写一句固定文案信息量大。 */
static void build_desc(char *out, size_t capacity, const char *tags, const char *codec,
                       int bitrate, int votes)
{
    size_t used = 0;
    uint8_t picked = 0;

    out[0] = '\0';
    if (tags != NULL) {
        const char *p = tags;
        while (*p != '\0' && picked < 2) {
            const char *comma = strchr(p, ',');
            size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
            while (len > 0 && (*p == ' ' || *p == '\t')) {
                ++p;
                --len;
            }
            while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) {
                --len;
            }

            const char *cn = translate_tag(p, len);
            if (cn != NULL) {
                const int written = snprintf(out + used, capacity - used,
                                             (picked == 0) ? "%s" : " · %s", cn);
                if (written > 0 && (size_t)written < capacity - used) {
                    used += (size_t)written;
                    ++picked;
                }
            }
            if (comma == NULL) {
                break;
            }
            p = comma + 1;
        }
    }

    if (picked == 0) {
        if (votes > 0) {
            snprintf(out, capacity, "热度 %d", votes);
        } else {
            snprintf(out, capacity, "%s", "广播电台");
        }
        used = strlen(out);
    }

    /* 编码 + 码率：目录里大多数中文台 bitrate 是 0，那就只写编码 */
    const char *codec_text = (codec != NULL && codec[0] != '\0') ? codec : "MP3";
    if (bitrate > 0 && bitrate < 1000) {
        snprintf(out + used, capacity - used, " · %s %dk", codec_text, bitrate);
    } else {
        snprintf(out + used, capacity - used, " · %s", codec_text);
    }
}

/* 语言：只认常见的，认不出就留空（不展示英文） */
static const char *translate_language(const char *language)
{
    static const struct {
        const char *en;
        const char *cn;
    } table[] = {
        {"chinese", "国语"},  {"mandarin", "国语"},  {"cantonese", "粤语"},
        {"english", "英语"},  {"japanese", "日语"},  {"korean", "韩语"},
        {"french", "法语"},   {"german", "德语"},    {"spanish", "西班牙语"},
        {"russian", "俄语"},  {"italian", "意大利语"}, {"portuguese", "葡萄牙语"},
        {"hindi", "印地语"},  {"arabic", "阿拉伯语"}, {"turkish", "土耳其语"},
        {"dutch", "荷兰语"},  {"thai", "泰语"},      {"vietnamese", "越南语"},
    };
    char key[24];
    size_t len;

    if (language == NULL) {
        return NULL;
    }
    /* 只取第一个语言（目录里常见 "chinese,english"） */
    const char *comma = strchr(language, ',');
    len = (comma != NULL) ? (size_t)(comma - language) : strlen(language);
    while (len > 0 && (*language == ' ' || *language == '\t')) {
        ++language;
        --len;
    }
    while (len > 0 && (language[len - 1] == ' ' || language[len - 1] == '\t')) {
        --len;
    }
    if (len == 0 || len >= sizeof(key)) {
        return NULL;
    }
    memcpy(key, language, len);
    key[len] = '\0';

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (text_equal_ci(key, table[i].en)) {
            return table[i].cn;
        }
    }
    return NULL;
}

/* ==================== 响应解析 ==================== */

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : "";
}

int radio_catalog_parse_location(const cJSON *root, radio_location_t *out)
{
    const char *status;
    const char *country;
    const char *region;
    const char *city;
    int result = -1;

    if (root == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    status = json_string(root, "status");
    country = json_string(root, "countryCode");
    region = json_string(root, "regionName");
    city = json_string(root, "city");

    if (strcmp(status, "success") == 0 && country[0] != '\0' &&
        (city[0] != '\0' || region[0] != '\0')) {
        copy_text(out->country, sizeof(out->country), country);
        copy_text(out->region, sizeof(out->region), region);
        copy_text(out->city, sizeof(out->city), city);
        result = 0;
    }
    return result;
}

/* ip-api 的国家码非 CN 时城市名保持英文；CN 时尽量换成中文，换不到返回 NULL */
const char *radio_catalog_translate_city(const char *country, const char *english_city)
{
    if (country == NULL || english_city == NULL || strcmp(country, "CN") != 0) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_city_translations) / sizeof(s_city_translations[0]); ++i) {
        if (strcmp(s_city_translations[i].english, english_city) == 0) {
            return s_city_translations[i].chinese;
        }
    }
    return NULL;
}

/* 蜻蜓 / 企鹅的 https 流在设备上握手开销大，统一降成 http（与 leo-radio 一致） */
static void normalize_stream_url(const char *source, char *target, size_t capacity)
{
    if (starts_with(source, "https://") &&
        (contains_text(source, ".qtfm.cn/") || contains_text(source, ".qingting.fm/"))) {
        snprintf(target, capacity, "http://%s", source + 8);
    } else {
        copy_text(target, capacity, source);
    }
}

static bool duplicate_url(const radio_station_t *stations, size_t count, const char *url)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(stations[i].url, url) == 0) {
            return true;
        }
    }
    return false;
}

size_t radio_catalog_parse_stations(const cJSON *root,
                                    const radio_location_t *location,
                                    const char *city_display,
                                    radio_station_t *out, size_t capacity)
{
    const cJSON *entry = NULL;
    size_t count = 0;
    const char *city;

    if (root == NULL || !cJSON_IsArray(root) || out == NULL || capacity == 0) {
        return 0;
    }
    if (capacity > RADIO_MAX_STATIONS) {
        capacity = RADIO_MAX_STATIONS;
    }

    city = (city_display != NULL && city_display[0] != '\0') ? city_display : NULL;
    if (city == NULL) {
        city = (location != NULL && location->city[0] != '\0') ? location->city :
               ((location != NULL) ? location->region : "");
    }

    cJSON_ArrayForEach(entry, root) {
        if (count >= capacity || !cJSON_IsObject(entry)) {
            break;
        }

        const char *raw_name = json_string(entry, "name");
        const char *raw_url = json_string(entry, "url_resolved");
        const cJSON *last_ok = cJSON_GetObjectItemCaseSensitive(entry, "lastcheckok");
        const cJSON *hls = cJSON_GetObjectItemCaseSensitive(entry, "hls");

        if (raw_url[0] == '\0') {
            raw_url = json_string(entry, "url");
        }
        if (raw_name[0] == '\0' || raw_url[0] == '\0' || !contains_text(raw_url, ".mp3") ||
            (cJSON_IsNumber(last_ok) && last_ok->valueint != 1) ||
            (cJSON_IsNumber(hls) && hls->valueint != 0)) {
            continue;
        }

        radio_station_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        normalize_stream_url(raw_url, candidate.url, sizeof(candidate.url));
        if ((!starts_with(candidate.url, "http://") && !starts_with(candidate.url, "https://")) ||
            duplicate_url(out, count, candidate.url)) {
            continue;
        }

        candidate.freq_decihz = radio_catalog_parse_frequency(raw_name);
        make_display_name(candidate.name, sizeof(candidate.name), raw_name, city, count);
        copy_text_utf8(candidate.full, sizeof(candidate.full), raw_name);
        /* 副标题：优先用目录里的 tags（翻中文）+ 编码/码率 */
        const cJSON *bitrate_item = cJSON_GetObjectItemCaseSensitive(entry, "bitrate");
        const int bitrate = cJSON_IsNumber(bitrate_item) ? bitrate_item->valueint : 0;
        const cJSON *votes_item = cJSON_GetObjectItemCaseSensitive(entry, "votes");
        const int votes = cJSON_IsNumber(votes_item) ? votes_item->valueint : 0;
        build_desc(candidate.desc, sizeof(candidate.desc), json_string(entry, "tags"),
                   json_string(entry, "codec"), bitrate, votes);
        const char *lang = translate_language(json_string(entry, "language"));
        if (lang != NULL) {
            copy_text(candidate.lang, sizeof(candidate.lang), lang);
        }
        out[count++] = candidate;
    }

    LOGI("parsed %u stations for '%s'", (unsigned)count, city);
    return count;
}
