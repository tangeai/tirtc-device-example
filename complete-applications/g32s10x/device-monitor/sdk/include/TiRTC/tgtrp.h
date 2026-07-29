#ifndef __TGTRP_API_HEADER_H__
#define __TGTRP_API_HEADER_H__
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define MAX_TGTRP_ADDR_INFO_SIZE 40

typedef void (*tgtrp_log_cb_t)(const char* fmt, va_list args);

/**
 * @brief 监听器句柄，用于服务端监听连接请求
 */
typedef void* tgtrp_listener;

/**
 * @brief 连接句柄，代表一个P2P连接
 */
typedef void* tgtrp_connection;

/**
 * @brief 通道句柄，代表连接中的一个数据传输通道
 */
typedef void* tgtrp_channel;

/**
 * @brief 数据发送向量结构体，用于scatter/gather IO
 */
struct tgtrp_channel_vec
{
    char* buf; ///< 数据缓冲区指针
    int  len;  ///< 数据长度
};

/**
 * @brief 监听器配置选项枚举
 */
enum TGTRP_LISTENER_OPTION
{
    /**
     * @brief 是否当前listener的所有连接都使用同一个线程
     * @details 默认值为0 (每个连接独立线程)。
     * 取值示例:
     * - 0: 每个连接使用独立线程
     * - 1: 所有连接共享同一个线程
     */
    TGTRP_MULTIPLE_CONNECTION_SHARED_THREAD,

    /**
     * @brief 加密级别
     * @details 取值范围0~7。
     * 取值示例:
     * - 0: 最低加密级别/不加密
     * - 7: 最高加密级别 (算法复杂，CPU代价高)
     */
     TGTRP_CONNECTION_ENCRYPTO_LEVEL,

     /**
      * @brief 是否需要开启唤醒服务
      * @details 默认值为0。
      * 取值示例:
      * - 0: 不开启
      * - 1: 开启唤醒服务
      */
      TGTRP_ENABLE_WAKEUP_PROPERTY,

      /**
       * @brief 是否强制使用域名作为中转服务器地址
       * @details 默认为0。用于定向流量卡等无白名单IP无法连接的场景。
       * 取值示例:
       * - 0: 使用IP或域名 (默认)
       * - 1: 强制服务器返回域名
       */
       TGTRP_TURN_SERVER_ADDR_DOMAIN,
};


/**
 * @brief 连接信息结构体
 */
struct tgtrp_connection_info
{
    char link_mode[MAX_TGTRP_ADDR_INFO_SIZE];       ///< 连接模式，例如host-host host-relay relay-relay等(P2P的连接模式)
    char local_candidate[MAX_TGTRP_ADDR_INFO_SIZE]; ///< 本地候选地址. 示例: "192.168.1.101:60380.host.udplocal"
    char remote_candidate[MAX_TGTRP_ADDR_INFO_SIZE];///< 远端候选地址. 示例: "192.168.1.101:60380.host"
    char app_internal_ip[MAX_TGTRP_ADDR_INFO_SIZE]; ///< app端(呼叫端)外网地址. 示例 "113.106.106.98" 用于网络分析
    char device_internal_ip[MAX_TGTRP_ADDR_INFO_SIZE];///< 设备端(被呼叫端)外网地址. 示例"113.106.106.98" 用于网络分析
};

/**
 * @brief 初始化TIRTC库
 * @note 异步调用: 非阻塞
 *
 * @param max_snd_buff_size 最大发送缓冲区大小 (字节)。示例: 1024 * 1024 (1MB)
 */
void tgtrp_init(size_t max_snd_buff_size);

/**
 * @brief 反初始化TIRTC库，释放全局资源
 * @note 异步调用: 否 (非阻塞)
 */
void tgtrp_uninit();

/**
 * @brief 创建一个新的监听器对象
 * @note 异步调用: 非阻塞
 *
 * @param max_conn_num 允许的最大并发连接数。示例: 10
 * @return tgtrp_listener 成功返回监听器句柄，失败返回NULL
 */
tgtrp_listener tgtrp_listener_new(int max_conn_num);

/**
 * @brief 设置监听器的配置选项
 * @note 异步调用: 非阻塞
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param opt 选项枚举值，参见 enum TGTRP_LISTENER_OPTION。示例: TGTRP_CONNECTION_ENCRYPTO_LEVEL
 * @param value 选项对应的值。示例: 1
 */
void tgtrp_listener_set_opt(tgtrp_listener listener, enum TGTRP_LISTENER_OPTION opt, int value);

/**
 * @brief 绑定监听器参数并设置新连接回调
 * @note 异步调用: 非阻塞
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param config_str 配置字符串。示例: "w9bl84KRoLbI1bOi0sD05LDQ4u2FlLi2x9b88YeSrL7A0eI="
 * @param token_str 鉴权Token字符串。示例: "eyJh..."
 * @param device_id 本地设备ID(控制在32字符以内)。示例: "YT4F77V532RR"
 * @param newconn_cb 新连接建立时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程.
 *        - context: 用户传入的上下文
 *        - pconn: 新建立的连接句柄
 * @param context 用户自定义上下文指针，将传递给回调函数。
 */
int tgtrp_listener_bind(tgtrp_listener listener, const char* config_str, const char* token_str, const char* device_id, void (*newconn_cb)(void* context, tgtrp_connection pconn), void* context);

/**
 * @brief 开始监听
 * @note 异步调用: 非阻塞
 *
 * @param listener 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @return int 0 代表成功，-1 代表失败
 */
int tgtrp_listen(tgtrp_listener listener);

/**
 * @brief 关闭监听句柄
 * @details 在异步回调中清除应用层注册的context相关数据
 * @note异步调用: 非阻塞，上层context对象需要等待close_finish_cb回调后才能释放
 *
 * @param obj 监听器句柄。示例: 由 tgtrp_listener_new 返回的指针
 * @param close_finish_cb 关闭完成时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - context: 用户传入的上下文
 * @param context 用户自定义上下文指针，将传递给回调函数。
 */
void tgtrp_close(tgtrp_listener obj, void (*close_finish_cb)(void* context), void* context);


/**
 * @brief 创建一个新的P2P连接对象
 * @note 异步调用: 非阻塞
 *
 * @return tgtrp_connection 成功返回连接句柄，失败返回NULL
 */
tgtrp_connection tgtrp_connection_new();

/**
 * @brief 设置连接超时时间
 * @note 异步调用: 非阻塞
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param timeout_ms 超时时间 (毫秒)。示例: 5000
 */
void tgtrp_connection_set_timeout(tgtrp_connection pconn, unsigned int timeout_ms);

/**
 * @brief 销毁连接对象
 * @note 异步调用: 非阻塞，上层context对象需要等待close_finish_cb回调后才能释放
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param stat_info_cb 统计信息回调通知，关闭期间可能会被回调多次。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pconn: 连接句柄
 *        - stat_context: 统计上下文
 *        - stat_str: 统计信息字符串
 *        - length: 字符串长度
 * @param stat_context 统计信息回调通知的context指针。
 * @param close_finish_cb 关闭成功的回调通知。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - close_context: 关闭上下文
 * @param close_context 关闭成功通知的context指针。
 */
void  tgtrp_connection_destroy(tgtrp_connection pconn,
    void (*stat_info_cb)(tgtrp_connection pconn, void* stat_context, const char* stat_str, int length),
    void* stat_context,
    void (*close_finish_cb)(void* close_context),
    void* close_context);

/**
 * @brief 获取连接信息
 * @note 异步调用: 否 (非阻塞)
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param pinfo 输出参数，用于存储连接信息的结构体指针。示例: 指向 struct tgtrp_connection_info 的指针
 */
void tgtrp_connection_get_info(tgtrp_connection pconn, struct tgtrp_connection_info* pinfo);

/**
 * @brief 注册连接错误回调通知
 * @note  异步调用: 非阻塞
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param error_notify_cb 错误发生时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pconn: 连接句柄
 *        - context: 用户上下文
 *        - ev: 错误码/事件ID
 * @param context 用户自定义上下文指针。
 */
void tgtrp_connection_set_on_error(tgtrp_connection pconn, void (*error_notify_cb)(tgtrp_connection pconn, void* context, int ev), void* context);

/**
 * @brief 注册新Channel创建成功的回调通知
 * @details 通知一个新的tgtrp_channel创建成功，此时可以开始收发数据
 * @note  异步调用: 非阻塞
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param on_newchannel_cb 新Channel就绪时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pconn: 连接句柄
 *        - c: 新创建的Channel句柄
 *        - context: 用户上下文
 * @param context 用户自定义上下文指针。
 */
void tgtrp_connection_set_on_channel(tgtrp_connection pconn, void (*on_newchannel_cb)(tgtrp_connection pconn, tgtrp_channel c, void* context), void* context);

/**
 * @brief 请求创建一个新的Channel
 * @details 创建后不会立即返回channel句柄，需要底层握手成功后，通过 `tgtrp_connection_set_on_channel` 设置的回调通知上层。
 * @note 异步调用: 否 (非阻塞)
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param label_name Channel的标签名称，长度不超过12字节。示例: "data_ch_1"
 */
void tgtrp_channel_new(tgtrp_connection pconn, const char label_name[12]);

/**
 * @brief 呼叫对方建立连接
 * @note 异步调用: 否 (非阻塞)
 *
 * @param pconn 连接句柄 (参数名补充)。示例: 由 tgtrp_connection_new 返回的指针
 * @param config_str 配置字符串。示例: "w9bl84KRoLbI1bOi0sD05LDQ4u2FlLi2x9b88YeSrL7A0eI="
 * @param token_str 鉴权Token字符串。示例: "eyJh..."
 * @param appid 应用ID(16字符以内)。示例: "my_app_001"
 * @param remote_device 远端设备ID。示例: "remote_dev_999"
 * @param link_param 链接参数，控制传输方式。
 *                   目前设备方未使用，全部传0。
 *                   示例: 0
 * @param timeout_ms 呼叫超时时间 (毫秒)。示例: 10000
 */
void tgtrp_connection_call(tgtrp_connection pconn, const char* config_str, const char* token_str, const char* appid, const char* remote_device, int link_param, uint32_t timeout_ms);

/**
 * @brief 获取Channel的标签名称
 * @note  异步调用: 非阻塞
 *
 * @param c Channel句柄。示例: 回调函数中传入的 tgtrp_channel 指针
 * @return const char* 返回Channel的标签字符串
 */
const char* tgtrp_channel_get_label(tgtrp_channel c);

/**
 * @brief 设置Channel的数据接收回调
 * @note  异步调用: 非阻塞
 *
 * @param pc 该channel所属的p2p连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 * @param c Channel句柄。示例: 回调函数中传入的 tgtrp_channel 指针
 * @param ondata 收到数据时的回调函数。此回调函数不能阻塞，否则会卡住P2P线程。
 *        - pc: 连接句柄
 *        - c: Channel句柄
 *        - context: 用户上下文
 *        - buffer: 数据缓冲区指针
 *        - size: 数据长度
 * @param context 用户自定义上下文指针。
 */
void  tgtrp_channel_set_on_data(
    tgtrp_connection pc,
    tgtrp_channel c,
    void (*ondata)(tgtrp_connection pc, tgtrp_channel c, void* context, char* buffer, int size),
    void* context);

/**
 * @brief 通过Channel发送数据 (Scatter/Gather I/O)
 * @note  异步调用: 非阻塞
 *
 * @param pc 连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 * @param c Channel句柄。示例: 回调函数中传入的 tgtrp_channel 指针
 * @param vec 数据向量数组。示例: struct tgtrp_channel_vec my_vecs[2];
 * @param vec_cnt 向量数组的元素个数。示例: 2
 * @return int >0 代表成功(返回需要发送的字节数)，-1 代表失败
 */
int tgtrp_channel_sendv(tgtrp_connection pc, tgtrp_channel c, struct tgtrp_channel_vec vec[], int vec_cnt);

/**
 * @brief 获取当前发送缓冲区的已使用量
 * @note  异步调用: 非阻塞
 *
 * @param pc 连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 * @return size_t 已使用的字节数。示例: 1024，应用层根据返回值判定缓存大小来决定是否要丢帧。
 */
size_t tgtrp_channel_get_used_send_buffer_size(tgtrp_connection pc);

/**
 * @brief 开启连接的调试模式
 * @details 仅呼叫端有效，用于输出调试信息
 * @note  异步调用: 非阻塞
 *
 * @param pc 连接句柄。示例: 由 tgtrp_connection_new 返回的指针
 */
void tgtrp_connection_enable_debug(tgtrp_connection pc);

/**
 * @brief 设置TIRTC库的日志级别
 * @note  异步属性: 同步调用
 *
 * @param log_level 日志级别，取值范围：
 *        - 0: 关闭日志输出
 *        - 1: 最高级别日志（仅输出最重要的错误信息）
 *        - 2~6: 中间级别日志（逐级增加日志详细程度）
 *        - 7: 最低级别日志（输出所有日志，包括调试信息）
 *
 *        说明：级别越高（数字越大），输出的日志越详细
 *        建议：开发调试时使用7，生产环境使用1~3
 */
void tgtrp_set_log_level(int log_level);

/**
 * @brief 设置日志回调函数
 * @note  异步属性: 同步调用
 *
 * @param cb 日志回调函数指针，类型为 tgtrp_log_cb_t
 *           函数签名: void (*)(const char* fmt, va_list args)
 *           可用于将日志重定向到自定义处理函数
 */
void tgtrp_set_log_callback(tgtrp_log_cb_t cb);


/**
 * @brief 创建一个新的P2P连接对象
 * @note 异步调用: 非阻塞
 * @param use_dtls, 如果主叫方为第三方(例如chrome)，则需要传参数为1
 * use_dtls特别说明：为什么会导出这样的设置，原因在于如果对于自研库之间不想走dtls(消耗性能)，则传0，但是如果对方是web端，此时库并不知道对方是web，需要上层传use_dtls为1. 如果业务后续全部走dtls，则可以去掉该参数，强制所有通信都走dtls加密
 * @return tgtrp_connection 成功返回连接句柄，失败返回NULL
 */
tgtrp_connection tgtrp_connection_whip_new(int use_dtls);

/**
 * @brief [WHIP 主叫] 异步生成 Offer SDP
 * @details 内部触发 ICE gather，收集所有候选后将其拼接到 SDP 末尾，
 *          通过 on_sdp_ready 回调返回含所有候选的完整 offer SDP。
 *          回调执行在 WebRTC 内部线程，不得阻塞；sdp 指针在回调返回后失效，需自行拷贝。
 *
 *          调用顺序：
 *            create_offer_sdp(pc, cb, ctx)
 *            → on_sdp_ready 回调: HTTP POST 完整 offer SDP
 *            → set_remote_sdp(answer) → add_candidate×N → start_connect
 *
 * @param pc           连接句柄，由 tgtrp_connection_new 创建
 * @param on_sdp_ready gather 完成后的回调，参数含完整 SDP 字符串和长度
 * @param context      用户自定义上下文指针
 * @return 0 表示成功，-1 表示失败
 */
int tgtrp_connection_whip_create_offer_sdp(
    tgtrp_connection pc,
    void (*on_sdp_ready)(tgtrp_connection pc, void* context, const char* sdp, int len),
    void* context);

/**
 * @brief [WHIP 主叫] 设置远端 Answer SDP
 * @details 将 WHIP 服务端返回的 answer SDP 设置为远端描述，并自动解析其中内嵌的候选。
 *
 * @param pc                连接句柄
 * @param sdp_buffer        远端 answer SDP 字符串
 * @param sdp_buffer_length SDP 字符串长度
 * @return 0 表示成功，-1 表示失败
 */
int tgtrp_connection_whip_set_remote_sdp(tgtrp_connection pc, const char* sdp_buffer, int sdp_buffer_length);

/**
 * @brief [WHIP 被叫] 异步生成 Answer SDP
 * @details 在 set_remote_sdp 之后调用。内部触发 ICE gather，收集所有候选后将其拼接到 SDP
 *          末尾，通过 on_sdp_ready 回调返回含所有候选的完整 answer SDP。
 *          回调执行在 WebRTC 内部线程，不得阻塞；sdp 指针在回调返回后失效，需自行拷贝。
 *
 *          调用顺序：
 *            set_remote_sdp(offer) → create_answer_sdp(pc, cb, ctx)
 *            → on_sdp_ready 回调: 发送 201（含完整 answer SDP）
 *            → add_candidate×N → start_connect
 *
 * @param pc           连接句柄，由 tgtrp_connection_new 创建
 * @param on_sdp_ready gather 完成后的回调，参数含完整 SDP 字符串和长度
 * @param context      用户自定义上下文指针
 * @return 0 表示成功，-1 表示失败
 */
int tgtrp_connection_whip_create_answer_sdp(
    tgtrp_connection pc,
    void (*on_sdp_ready)(tgtrp_connection pc, void* context, const char* sdp, int len),
    void* context);

/**
 * @brief 添加远端 ICE Candidate
 * @details 主叫和被叫均可调用，将对端通过信令发来的 candidate 字符串加入 ICE 候选列表。
 *          可在 start_connect 之前或之后调用。
 *
 * @param pc                连接句柄
 * @param sdp_buffer        candidate SDP 字符串（"candidate:xxx ..."）
 * @param sdp_buffer_length 字符串长度
 * @return 0 或正数表示成功，-1 表示失败
 */
int tgtrp_connection_whip_add_candidate(tgtrp_connection pc, const char* sdp_buffer, int sdp_buffer_length);

/**
 * @brief 开始 ICE 连接
 * @details 在 on_sdp_ready 回调之后、完成 SDP 交换后调用。
 *          ICE gather 已在 create_offer/answer_sdp 内部触发，此函数只启动连接协商。
 *
 * @param pc  连接句柄
 */
void tgtrp_connection_whip_start_connect(tgtrp_connection pc);

/**
 * @brief [WHIP] 设置 candidate IP 覆盖地址
 * @details 设置后，gather 阶段生成的所有本地 candidate 的 IP 地址均替换为指定值。
 *          适用于设备位于 NAT 后、需对外暴露固定公网 IP 的场景。
 *          传入 NULL 或空字符串则清除覆盖，恢复使用实际 IP。
 *          须在 create_offer_sdp / create_answer_sdp 之前调用。
 *
 * @param pc  连接句柄
 * @param ip  要覆盖的 IP 字符串（如 "203.0.113.1"），或 NULL/空串取消覆盖
 */
void tgtrp_connection_whip_set_candidate_ip(tgtrp_connection pc, const char* ip);


/*
if define SMALL_MEM_NO_DTLS:device_property |= 0x1;  device cpu low performance, no dtls.
if define SMALL_MEMORY_WITHOUT_SCTP:device_property |= 0x2;  only support kcp
if define  TGRTC_SECURITY_ENHANCEMENT device_property |= 0x04; force use dtls encrypt.
*/
uint8_t tgtrp_get_library_property();

#endif

/***************************示例程序


// Test configuration
//#define TEST_CONFIG "w9bl84KRoLbI1bOi0sD05LDV5e2FlaGpwdjj7YCWrLXA0eLz"
#define TEST_CONFIG "w9bl84KRoLbI1bOi0sD05LDQ4u2FlLi2x9b88YeSrL7A0eI="
#define TEST_DEVICE "test_device_001"
#define TEST_APPID "test_app_001"
#define TEST_REMOTE_DEVICE "remote_device_001"
#define TEST_CHANNEL_LABEL "test_channel"
#define TEST_MAX_CONNECTIONS 5
#define TEST_TIMEOUT_MS 10000

// Test data structure
typedef struct test_context {
    tgtrp_listener obj;
    tgtrp_connection conn;
    tgtrp_channel channel;
    int test_completed;
    char test_name[64];
} test_context_t;


#define MAX_CHANNEL_NUM 10
typedef struct _my_context
{
    tgtrp_connection conn;
    tgtrp_channel chs[MAX_CHANNEL_NUM];
    int channel_num;
    int is_error;
    int is_app;
    SA_HTHREAD th;
}my_context;


void stat_info_cb(tgtrp_connection conn, void* stat_context, const char* stat_str, int length)
{
    printf("%s\n", stat_str);
}


void close_finish_cb(void* close_context)
{
    Mx_free(close_context);
}

// 业务方定义的发送数据到连接的线程.
void conn_thread(void* arg)
{
    my_context* ctx = (my_context*)arg;
    struct tgtrp_channel_vec vec[1];
    char buffer[128];
    memset(buffer, '0', sizeof(buffer));
    vec[0].buf = buffer;
    struct tgtrp_connection_info info;
    vec[0].len = sizeof(buffer);
    while (1)
    {
        SA_Sleep(400);
        if (ctx->is_error)
        {
            tgtrp_connection_destroy(ctx->conn, stat_info_cb, ctx, close_finish_cb, ctx);
            break;
        }
        tgtrp_connection_get_info(ctx->conn, &info);
        printf("type %s----------------------%s %s\n", info.link_mode, info.local_candidate, info.remote_candidate);
        if (ctx->channel_num > 0)
        {
           tgtrp_channel_sendv(ctx -> conn, ctx->chs[0], vec, 1);
        }
    }


}

void ondata(tgtrp_connection pc, tgtrp_channel c, void* context, char* buffer, int size)
{
    my_context* ctx = (my_context*)context;
    assert(ctx->conn == pc);
    printf("ondata size=%d\n", size);
}

void on_newchannel_cb111(tgtrp_connection conn, tgtrp_channel c, void* context)
{
    my_context* ctx = (my_context*)context;
    ctx->chs[ctx->channel_num++] = c;
    assert(ctx->conn == conn);
    tgtrp_channel_set_on_data(ctx->conn, c, ondata, ctx);
}
void error_notiry_cb(tgtrp_connection conn, void* context, int ev)
{
    my_context* ctx = (my_context*)context;
    assert(conn == ctx->conn);
    APP_LOG_NOTICE("error_notiry_cb conn=%p ev=%d", conn, ev);
    ctx->is_error = 1;
}
void new_conn_cb(void* context, tgtrp_connection conn)
{
    my_context* ctx = Mx_calloc(1, sizeof(my_context));
    ctx->conn = conn;
    tgtrp_connection_set_on_channel(conn, on_newchannel_cb111, ctx);
    tgtrp_connection_set_on_error(conn, error_notiry_cb, ctx);
    SA_ThreadCreate(ctx->th, conn_thread, ctx, "xxxx"); //业务创建一个线程处理该连接.
}

//模拟device设备端
void simulate_device()
{
    tgtrp_init(512 * 1024);

    tgtrp_listener l = tgtrp_listener_new(5);

    tgtrp_listener_set_opt(l, TGTRP_MULTIPLE_CONNECTION_SHARED_THREAD, 1);
    tgtrp_listener_set_opt(l, TGTRP_CONNECTION_ENCRYPTO_LEVEL, 2);
    tgtrp_listener_bind(l, TEST_CONFIG,"ajc.c.....", "server", new_conn_cb, NULL);
    tgtrp_listen(l);
}

/// 模拟APP呼叫端
void simulate_app()
{
    tgtrp_init(512 * 1024);
    my_context* ctx = Mx_calloc(1, sizeof(my_context));
    ctx->conn =  tgtrp_connection_new();

    tgtrp_connection_set_on_channel(ctx->conn, on_newchannel_cb111, ctx);
    tgtrp_connection_set_on_error(ctx->conn, error_notiry_cb, ctx);
    SA_ThreadCreate(ctx->th, conn_thread, ctx, "xxxx");

    tgtrp_channel_new(ctx->conn, "1");
    tgtrp_channel_new(ctx->conn, "2");
    tgtrp_channel_new(ctx->conn, "3");

    tgtrp_connection_call(ctx->conn, TEST_CONFIG, "TOKEN", "client", "server", 0, 10000);
}

int main(int argc, char* argv[]) {

    if (argc > 1)
        simulate_app();
    else simulate_device();

    while (1)
        SA_Sleep(100);

}

**********************************/