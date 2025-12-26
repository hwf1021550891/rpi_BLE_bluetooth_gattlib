#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include "gattlib.h"

// 发送/接收UUID
#define TX_UUID "0000ffe1-0000-1000-8000-00805f9b34fb"
#define RX_UUID "0000ffe4-0000-1000-8000-00805f9b34fb"
#define SCAN_TIMEOUT 10 // 扫描超时时间（秒）

// 全局控制变量
static volatile int g_terminate = 0;          // 全局退出标志
static volatile int g_scan_found_device = 0;  // 扫描到目标设备标志
static char g_target_mac[20] = {0};          // 目标设备MAC地址
static gattlib_connection_t* g_conn = NULL;  // 蓝牙连接句柄

// 同步变量（扫描/连接）
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;


/**
 * 异步连接回调函数（匹配gatt_connect_cb_t原型）
 * @param adapter    本地蓝牙适配器
 * @param dst        远程设备MAC地址
 * @param connection 建立的连接句柄
 * @param error      连接错误码（0=成功）
 * @param user_data  用户自定义数据
 */
static void connect_callback(gattlib_adapter_t* adapter, const char *dst, 
                             gattlib_connection_t* connection, int error, void* user_data) {
    pthread_mutex_lock(&g_mutex);
    if (error != 0) {
        fprintf(stderr, "❌ 异步连接失败（设备: %s），错误码: %d\n", dst, error);
        g_conn = NULL;
    } else {
        printf("✅ 成功连接到设备：%s\n", dst);
        g_conn = connection;
    }
    // 唤醒主线程
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}
/**
 * 扫描设备回调函数：匹配目标MAC地址
 * @param addr  扫描到的设备MAC地址
 * @param name  扫描到的设备名称（可能为NULL）
 * @param user_data 用户数据（未使用）
 */

static void scan_callback(gattlib_adapter_t* adapter, const char* addr, const char* name, void* user_data) {
    if (strcasecmp(addr, "70:19:88:3D:30:68") == 0) {
        printf("发现目标设备: %s\n", addr);
        gattlib_adapter_scan_disable(adapter); // 停止扫描

        // 发起连接
        printf("正在连接设备 %s...\n", "70:19:88:3D:30:68");
        int ret = gattlib_connect(adapter, "70:19:88:3D:30:68", 
                            GATTLIB_CONNECTION_OPTIONS_NONE, connect_callback, NULL);
        if (ret != 0) {
            fprintf(stderr, "连接请求失败: %d\n", ret);
            gattlib_adapter_close(adapter);
            return;
        }
    }
}
// 通知回调：接收设备响应数据
static void notification_handler(const uuid_t* uuid, const uint8_t* data, size_t data_len, void* user_data) {
    char uuid_str[MAX_LEN_UUID_STR + 1];
    gattlib_uuid_to_string(uuid, uuid_str, sizeof(uuid_str));

    printf("\n===== 接收到设备响应 =====\n");
    printf("UUID: %s\n", uuid_str);
    printf("十六进制数据: ");
    for (size_t i = 0; i < data_len; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n字符串数据: %.*s\n", (int)data_len, data);
    printf("==========================\n");
}

// 信号处理：Ctrl+C优雅退出
static void int_handler(int sig) {
    printf("\n⚠️  接收到退出信号，开始清理资源...\n");
    g_terminate = 1;

    // 断开蓝牙连接
    if (g_conn != NULL) {
        gattlib_disconnect(g_conn, false);
        g_conn = NULL;
    }

    // 唤醒等待的条件变量
    pthread_mutex_lock(&g_mutex);
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
}

// 核心逻辑：连接成功后初始化监听+发送数据
static int init_ble_comm(gattlib_connection_t* conn) {
    uuid_t tx_uuid, rx_uuid;
    gattlib_characteristic_t* chars = NULL;
    int char_count = 0;
    uint16_t tx_handle = 0, rx_handle = 0;
    int ret = 0;
    const char* test_data = "Hello, Bluetooth!";

    // 1. 转换UUID（字符串 → uuid_t）
    ret = gattlib_string_to_uuid(TX_UUID, strlen(TX_UUID) + 1, &tx_uuid);
    if (ret != 0) {
        fprintf(stderr, "❌ 转换发送UUID（%s）失败\n", TX_UUID);
        goto err;
    }
    ret = gattlib_string_to_uuid(RX_UUID, strlen(RX_UUID) + 1, &rx_uuid);
    if (ret != 0) {
        fprintf(stderr, "❌ 转换接收UUID（%s）失败\n", RX_UUID);
        goto err;
    }

    // 2. 发现设备所有GATT特征
    ret = gattlib_discover_char(conn, &chars, &char_count);
    if (ret != 0 || char_count == 0) {
        fprintf(stderr, "❌ 发现设备特征失败，错误码: %d\n", ret);
        goto err;
    }
    printf("🔍 发现 %d 个GATT特征\n", char_count);

    // 3. 查找TX/RX特征对应的句柄
    for (int i = 0; i < char_count; i++) {
        if (gattlib_uuid_cmp(&chars[i].uuid, &tx_uuid) == 0) {
            tx_handle = chars[i].value_handle;
            printf("✅ 找到TX特征（%s），句柄: 0x%04x\n", TX_UUID, tx_handle);
        } else if (gattlib_uuid_cmp(&chars[i].uuid, &rx_uuid) == 0) {
            rx_handle = chars[i].value_handle;
            printf("✅ 找到RX特征（%s），句柄: 0x%04x\n", RX_UUID, rx_handle);
        }
    }

    // 校验TX/RX句柄是否找到
    if (tx_handle == 0) {
        fprintf(stderr, "❌ 未找到TX特征（UUID: %s）\n", TX_UUID);
        ret = -1; goto err;
    }
    if (rx_handle == 0) {
        fprintf(stderr, "❌ 未找到RX特征（UUID: %s）\n", RX_UUID);
        ret = -1; goto err;
    }

    // 4. 先注册RX通知监听（核心：必须先监听，再发送）
    ret = gattlib_register_notification(conn, notification_handler, NULL);
    if (ret != 0) {
        fprintf(stderr, "❌ 注册RX通知回调失败，错误码: %d\n", ret);
        goto err;
    }
    ret = gattlib_notification_start(conn, &rx_uuid);
    if (ret != 0) {
        fprintf(stderr, "❌ 启动RX特征通知监听失败，错误码: %d\n", ret);
        goto err;
    }
    printf("📡 RX特征（%s）通知监听已启动\n", RX_UUID);

    // 5. 发送测试数据（先监听，后发送）
    printf("\n📤 准备发送测试数据：\n");
    printf("   字符串内容：%s\n", test_data);
    printf("   十六进制内容：");
    for (size_t i = 0; i < strlen(test_data); i++) {
        printf("%02x ", (uint8_t)test_data[i]);
    }
    printf("\n");

    // 无响应写（适配蓝牙透传设备常用场景）
    ret = gattlib_write_without_response_char_by_handle(
        conn, tx_handle, 
        (const uint8_t*)test_data, 
        strlen(test_data)
    );
    if (ret != 0) {
        fprintf(stderr, "❌ 发送数据失败，错误码: %d\n", ret);
        goto err;
    }
    printf("✅ 测试数据发送成功！\n");

err:
    // 释放特征内存
    if (chars != NULL) {
        free(chars);
    }
    return ret;
}

int main(int argc, char* argv[]) {
    gattlib_adapter_t* adapter = NULL;
    int ret = 0;
    unsigned long conn_options = GATTLIB_CONNECTION_OPTIONS_LEGACY_DEFAULT;

    // 1. 参数校验
    if (argc != 2) {
        fprintf(stderr, "🚫 用法错误！正确用法：\n");
        fprintf(stderr, "   %s <蓝牙设备MAC地址>\n", argv[0]);
        fprintf(stderr, "   示例：%s AA:BB:CC:DD:EE:FF\n", argv[0]);
        return 1;
    }

    // 2. 初始化目标MAC地址
    strncpy(g_target_mac, argv[1], sizeof(g_target_mac)-1);
    printf("🎯 目标设备MAC：%s\n", g_target_mac);

    // 3. 初始化信号处理（Ctrl+C退出）
    signal(SIGINT, int_handler);

    // 4. 初始化同步变量
    pthread_mutex_init(&g_mutex, NULL);
    pthread_cond_init(&g_cond, NULL);


    // 5. 打开本地蓝牙适配器
    const char* adapter_name = "hci0"; // 明确指定适配器
    ret = gattlib_adapter_open(adapter_name, &adapter);
    if (ret != 0) {
        fprintf(stderr, "❌ 打开蓝牙适配器（%s）失败，错误码: %d\n", adapter_name, ret);
        goto cleanup_sync;
    }
    printf("✅ 本地蓝牙适配器（%s）打开成功\n", adapter_name);

    // 6. 启动蓝牙设备扫描（核心新增逻辑）
    printf("\n📡 开始扫描蓝牙设备（超时%ds）...\n", SCAN_TIMEOUT);
    ret = gattlib_adapter_scan_enable(
        adapter,                // 适配器句柄
        scan_callback,          // 扫描回调
        SCAN_TIMEOUT,           // 扫描超时（秒）
        NULL                    // 用户数据
    );
    if (ret != 0) {
        fprintf(stderr, "❌ 启动蓝牙扫描失败，错误码: %d\n", ret);
        goto cleanup_adapter;
    }

    // 7. 等待扫描结果（找到目标/超时/退出）
    pthread_mutex_lock(&g_mutex);
    while (!g_scan_found_device && !g_terminate) {
        // 等待条件变量（超时时间=扫描超时+1秒）
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += SCAN_TIMEOUT + 1;
        int cond_ret = pthread_cond_timedwait(&g_cond, &g_mutex, &ts);
        
        if (cond_ret == ETIMEDOUT) {
            fprintf(stderr, "⏰ 扫描超时（%ds），未找到目标设备：%s\n", SCAN_TIMEOUT, g_target_mac);
            ret = -1;
            pthread_mutex_unlock(&g_mutex);
        }
    }
    pthread_mutex_unlock(&g_mutex);

    // 8. 停止扫描（找到目标/退出）

    // 10. 等待连接回调完成
    pthread_mutex_lock(&g_mutex);
    while (g_conn == NULL && !g_terminate) {
        pthread_cond_wait(&g_cond, &g_mutex);
    }
    pthread_mutex_unlock(&g_mutex);

    // 11. 连接成功则初始化通信
    if (g_conn != NULL && !g_terminate) {
        ret = init_ble_comm(g_conn);
        if (ret == 0) {
            // 持续等待设备响应（直到Ctrl+C退出）
            printf("\n⏳ 等待设备响应（按 Ctrl+C 退出）...\n");
            while (!g_terminate) {
                sleep(1);
            }
        }
    }

// 资源清理流程
cleanup_adapter:
    // 关闭蓝牙适配器
    if (adapter != NULL) {
        gattlib_adapter_close(adapter);
    }
cleanup_sync:
    // 销毁同步变量
    pthread_mutex_destroy(&g_mutex);
    pthread_cond_destroy(&g_cond);
    // 断开最终的连接
    if (g_conn != NULL) {
        gattlib_disconnect(g_conn, false);
    }

    printf("\n👋 程序正常退出\n");
    return ret;
}