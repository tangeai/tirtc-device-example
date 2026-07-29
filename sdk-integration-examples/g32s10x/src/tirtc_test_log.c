#include <stdio.h>

#include "TiRTC/tiRTC.h"
#include "tirtc_link_platform.h"
#include "tirtc_test_log.h"

static const char *test_error_meaning(int error)
{
    switch (error) {
    case 0:
        return "成功";
    case TIRTC_E_NOT_INITIALIZED:
        return "TiRTC还没有初始化";
    case TIRTC_E_INVALID_HANDLE:
        return "连接句柄已经失效";
    case TIRTC_E_INVALID_PARAMETER:
        return "传入参数不合法";
    case TIRTC_E_INVALID_LICENSE:
        return "设备License无效";
    case TIRTC_E_TIMEOUTED:
        return "操作等待超时";
    case TIRTC_E_BUSY:
        return "发送缓冲区已满或网络正忙";
    case TIRTC_E_CONN_TIMEOUTCLOSE:
        return "连接因心跳超时而关闭";
    case TIRTC_E_CONN_REMOTECLOSE:
        return "网页端主动关闭了连接";
    case TIRTC_E_CONN_OTHER_ERROR:
        return "连接发生其他错误";
    case TIRTC_E_LACK_OF_RESOURCE:
        return "内存、任务或其他系统资源不足";
    case TIRTC_E_CACHE_EXPIRED:
        return "连接Token缓存已过期";
    case TIRTC_E_SERVER_ERROR:
        return "TiRTC服务器处理失败";
    case TIRTC_E_INTERNAL_ERROR:
        return "TiRTC SDK内部错误";
    case TIRTC_E_NO_SECRET_KEY:
        return "没有设置设备Secret Key";
    case TIRTC_E_UNEXPECTED_RESPONSE:
        return "服务器返回的数据格式不符合预期";
    case TIRTC_LINK_E_WIFI_NOT_CONFIGURED:
        return "没有配置WiFi名称";
    case TIRTC_LINK_E_WIFI_DISCONNECTED:
        return "WiFi连接已断开";
    case TIRTC_LINK_E_NTP_NETWORK:
        return "校时时网络还没有准备好";
    case TIRTC_LINK_E_NTP_DNS:
        return "NTP服务器域名解析失败";
    case TIRTC_LINK_E_NTP_SOCKET:
        return "无法创建NTP网络连接";
    case TIRTC_LINK_E_NTP_SOCKET_OPTION:
        return "无法设置NTP接收超时";
    case TIRTC_LINK_E_NTP_SEND:
        return "NTP请求发送失败";
    case TIRTC_LINK_E_NTP_RECEIVE:
        return "没有收到完整的NTP响应";
    case TIRTC_LINK_E_NTP_RESPONSE:
        return "NTP服务器响应无效";
    case TIRTC_LINK_E_NTP_TIMESTAMP:
        return "NTP时间戳无效或过旧";
    case TIRTC_LINK_E_CLIENT_ID:
        return "没有读取到WiFi网卡MAC地址";
    case TIRTC_LINK_E_IDENTITY_MISSING:
        return "设备ID或Secret Key没有配置";
    case 40003:
        return "云端无法解析设备启动参数";
    case 40305:
        return "云端设备标识映射冲突";
    default:
        if (error >= 300 && error <= 599) {
            return "云端返回HTTP错误状态";
        }
        if (error > 599) {
            return "云端返回业务错误码";
        }
        return TiRtcGetErrorStr(error);
    }
}

static const char *test_error_action(int error)
{
    switch (error) {
    case TIRTC_E_NOT_INITIALIZED:
    case TIRTC_E_INTERNAL_ERROR:
        return "重新启动设备；仍失败时保留完整启动日志";
    case TIRTC_E_INVALID_HANDLE:
    case TIRTC_E_CONN_REMOTECLOSE:
        return "等待网页重新连接";
    case TIRTC_E_INVALID_PARAMETER:
        return "检查stream、帧格式、长度和配置参数";
    case TIRTC_E_INVALID_LICENSE:
    case TIRTC_E_NO_SECRET_KEY:
    case TIRTC_LINK_E_IDENTITY_MISSING:
        return "检查config/local_config.h中的设备凭据";
    case TIRTC_E_TIMEOUTED:
    case TIRTC_E_CONN_TIMEOUTCLOSE:
    case TIRTC_E_CONN_OTHER_ERROR:
    case TIRTC_LINK_E_WIFI_DISCONNECTED:
        return "检查WiFi信号和外网，随后重试";
    case TIRTC_E_BUSY:
        return "降低码率或等待发送缓冲区排空";
    case TIRTC_E_LACK_OF_RESOURCE:
        return "检查剩余内存、任务栈和连接是否正确释放";
    case TIRTC_E_CACHE_EXPIRED:
        return "让网页重新获取Token后再次连接";
    case TIRTC_E_SERVER_ERROR:
        return "稍后重试并检查TiRTC服务状态";
    case TIRTC_E_UNEXPECTED_RESPONSE:
    case 40003:
        return "检查服务地址、设备凭据和服务端接口版本";
    case 40305:
        return "在平台清理旧的client_id映射后重新启动";
    case TIRTC_LINK_E_WIFI_NOT_CONFIGURED:
        return "填写config/local_config.h中的WiFi名称和密码";
    case TIRTC_LINK_E_NTP_NETWORK:
    case TIRTC_LINK_E_NTP_DNS:
    case TIRTC_LINK_E_NTP_SOCKET:
    case TIRTC_LINK_E_NTP_SOCKET_OPTION:
    case TIRTC_LINK_E_NTP_SEND:
    case TIRTC_LINK_E_NTP_RECEIVE:
    case TIRTC_LINK_E_NTP_RESPONSE:
    case TIRTC_LINK_E_NTP_TIMESTAMP:
        return "检查DNS和外网；设备会按配置周期自动重试";
    case TIRTC_LINK_E_CLIENT_ID:
        return "确认ATBM WiFi网卡已初始化并有有效MAC地址";
    default:
        if (error >= 300 && error <= 599) {
            return "按HTTP状态检查请求、鉴权或服务器";
        }
        if (error > 599) {
            return "对照平台错误码文档并保留前后日志";
        }
        return "保留此错误前后的串口日志继续定位";
    }
}

void tirtc_test_log_failure_detail(const char *test, int error,
                                   const char *meaning,
                                   const char *action)
{
    printf("[TEST][FAIL] %s | code=%d | 含义=%s | 建议=%s\n",
           test, error, meaning, action);
}

void tirtc_test_log_failure(const char *test, int error)
{
    tirtc_test_log_failure_detail(test, error, test_error_meaning(error),
                                  test_error_action(error));
}

const char *tirtc_test_state_description(tirtc_link_state_t state)
{
    switch (state) {
    case TIRTC_LINK_STATE_IDLE:
        return "初始化";
    case TIRTC_LINK_STATE_WAIT_WIFI_CONFIG:
        return "等待WiFi配置";
    case TIRTC_LINK_STATE_WAIT_NETWORK:
        return "等待WiFi联网";
    case TIRTC_LINK_STATE_SYNC_TIME:
        return "正在校时";
    case TIRTC_LINK_STATE_WAIT_TIRTC_CONFIG:
        return "等待设备凭据";
    case TIRTC_LINK_STATE_STARTING:
        return "正在启动TiRTC";
    case TIRTC_LINK_STATE_LISTENING:
        return "在线等待网页连接";
    case TIRTC_LINK_STATE_CONNECTING:
        return "正在连接对端";
    case TIRTC_LINK_STATE_CONNECTED:
        return "对端已连接";
    case TIRTC_LINK_STATE_ERROR:
        return "发生错误";
    default:
        return "未知状态";
    }
}
