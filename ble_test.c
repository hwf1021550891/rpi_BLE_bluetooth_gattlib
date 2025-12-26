#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>  // 新增：用于MAC地址大小写不敏感比较
#include "gattlib.h"
#include <errno.h>
// 设备MAC和UUID（替换为实际值）
#define DEVICE_MAC "70:19:88:3D:30:97"
#define WRITE_UUID "0000ffe1-0000-1000-8000-00805f9b34fb"
#define NOTIFY_UUID "0000ffec-0000-1000-8000-00805f9b34fb"

// 全局变量
static gattlib_connection_t* g_connection = NULL;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_connected = 0;

// 通知回调函数
static void notification_handler(const uuid_t* uuid, const uint8_t* data, size_t data_length, void* user_data) {
    printf("Received data: ");
    for (size_t i = 0; i < data_length; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n");
}

// 连接回调函数
static void on_connect(gattlib_adapter_t* adapter, const char* dst, gattlib_connection_t* connection, int error, void* user_data) {
    if (error != GATTLIB_SUCCESS) {
        fprintf(stderr, "连接失败: %d\n", error);
        pthread_mutex_lock(&g_mutex);
        g_connected = -1;
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    printf("成功连接到设备 %s\n", dst);
    g_connection = connection;

    // 注册通知
    int ret = gattlib_register_notification(connection, notification_handler, NULL);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "注册通知失败\n");
        goto exit;
    }

    // 启动通知监听
    uuid_t notify_uuid;
    ret = gattlib_string_to_uuid(NOTIFY_UUID, strlen(NOTIFY_UUID) + 1, &notify_uuid);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "通知UUID无效\n");
        goto exit;
    }
    ret = gattlib_notification_start(connection, &notify_uuid);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "启动通知失败\n");
        goto exit;
    }

    // 发送测试数据
    uuid_t write_uuid;
    ret = gattlib_string_to_uuid(WRITE_UUID, strlen(WRITE_UUID) + 1, &write_uuid);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "写入UUID无效\n");
        goto exit;
    }
    uint8_t send_data[] = {0x12, 0x34, 0x56};
    ret = gattlib_write_char_by_uuid(connection, &write_uuid, send_data, sizeof(send_data));
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "发送数据失败\n");
        goto exit;
    }
    printf("发送数据: 0x12 0x34 0x56\n");

    pthread_mutex_lock(&g_mutex);
    g_connected = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    return;

exit:
    gattlib_disconnect(connection, false);
    pthread_mutex_lock(&g_mutex);
    g_connected = -1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}

// 新增：MAC地址大小写不敏感比较（解决MAC地址大小写导致的匹配失败）
static int mac_compare(const char* mac1, const char* mac2) {
    while (*mac1 && *mac2) {
        if (*mac1 == ':') {
            mac1++;
            mac2++;
            continue;
        }
        if (toupper(*mac1) != toupper(*mac2)) {
            return 1;
        }
        mac1++;
        mac2++;
    }
    return (*mac1 == *mac2) ? 0 : 1;
}

// 扫描回调函数（修改：接收所有广告类型，不局限于扫描响应）
static void on_discover(gattlib_adapter_t* adapter, const char* addr, const char* name, void* user_data) {
    // 打印所有发现的设备（用于调试）
    printf("发现设备: %s, 名称: %s\n", addr, name ? name : "未知");

    // 使用大小写不敏感的MAC比较
    if (mac_compare(addr, DEVICE_MAC) != 0) {
        return;
    }

    printf("找到目标设备: %s\n", addr);
    gattlib_adapter_scan_disable(adapter);

    // 发起连接
    int ret = gattlib_connect(adapter, addr, GATTLIB_CONNECTION_OPTIONS_NONE, on_connect, NULL);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "发起连接失败: %d\n", ret);
        pthread_mutex_lock(&g_mutex);
        g_connected = -1;
        pthread_cond_signal(&g_cond);
        pthread_mutex_unlock(&g_mutex);
    }
}

int main() {
    gattlib_adapter_t* adapter;
    int ret;

    pthread_mutex_init(&g_mutex, NULL);

    // 打开蓝牙适配器
    ret = gattlib_adapter_open(NULL, &adapter);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "打开适配器失败\n");
        return 1;
    }

    // 延长扫描超时到30秒（增加发现概率）
    printf("扫描目标设备 %s (30秒)...\n", DEVICE_MAC);
    ret = gattlib_adapter_scan_enable(adapter, on_discover, 30, NULL);
    if (ret != GATTLIB_SUCCESS) {
        fprintf(stderr, "启动扫描失败\n");
        gattlib_adapter_close(adapter);
        return 1;
    }

    // 等待连接结果（超时30秒）
    pthread_mutex_lock(&g_mutex);
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 30;
    int cond_ret = pthread_cond_timedwait(&g_cond, &g_mutex, &timeout);
    if (cond_ret == ETIMEDOUT) {
        fprintf(stderr, "操作超时\n");
        g_connected = -1;
    }
    pthread_mutex_unlock(&g_mutex);

    // 清理资源
    if (g_connection != NULL) {
        gattlib_disconnect(g_connection, false);
    }
    gattlib_adapter_close(adapter);
    pthread_mutex_destroy(&g_mutex);
    pthread_cond_destroy(&g_cond);

    return (g_connected == 1) ? 0 : 1;
}