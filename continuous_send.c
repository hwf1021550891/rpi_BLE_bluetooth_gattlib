#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <glib.h>
#include <gattlib.h>
#include <stdint.h>
#include <time.h>



// 配置参数
static struct {
    const char* adapter_name;
    const char* mac_address;
    uuid_t char_uuid;
    int send_count;         // 发送次数
    int interval_ms;        // 发送间隔(毫秒)
    uint8_t* data_buffer;   // 发送数据缓冲区
    size_t data_len;        // 数据长度
} m_config;

// 连接状态控制
static struct {
    pthread_cond_t cond;
    pthread_mutex_t lock;
    gattlib_connection_t* connection;
    bool is_connected;
    bool is_finished;
    int success_count;
    int fail_count;
} m_state;

// 全局变量：标记是否发现目标设备
static bool device_found = false;

// 连接回调函数
static void on_connect(gattlib_adapter_t* adapter, const char* dst, 
                      gattlib_connection_t* connection, int error, void* user_data) {
    if (error != 0) {
        fprintf(stderr, "连接失败: %d\n", error);
        pthread_mutex_lock(&m_state.lock);
        m_state.is_connected = false;
        pthread_cond_signal(&m_state.cond);
        pthread_mutex_unlock(&m_state.lock);
        return;
    }

    printf("成功连接到设备: %s\n", dst);
    pthread_mutex_lock(&m_state.lock);
    m_state.connection = connection;
    m_state.is_connected = true;
    pthread_cond_signal(&m_state.cond);
    pthread_mutex_unlock(&m_state.lock);

}
// 生成72字节字符数据（示例：hello开头，后续填充x）
static void generate_72bytes_char_data(uint8_t* buffer, size_t len) {
    if (len != 72) {
        fprintf(stderr, "数据长度必须为72字节\n");
        exit(1);
    }
    // 基础字符内容：hello + 67个x（总72字节）
    const char* prefix = "hello";
    size_t prefix_len = strlen(prefix);
    memcpy(buffer, prefix, prefix_len);
    // 剩余部分填充'x'
    memset(buffer + prefix_len, 'x', len - prefix_len);
    // 确保字符串终止（可选，仅用于打印）
    buffer[len - 2] = '\r';
    buffer[len - 1] = '\n';
    buffer[len] = '\0';
}
// 连续发送函数
static void send_continuous_data() {
    pthread_mutex_lock(&m_state.lock);
    // 等待连接完成
    while (!m_state.is_connected) {
        pthread_cond_wait(&m_state.cond, &m_state.lock);
    }
    gattlib_connection_t* connection = m_state.connection;
    pthread_mutex_unlock(&m_state.lock);

    // 循环发送数据
    for (int i = 0; i < m_config.send_count; i++) {
        printf("发送第 %d/%d 个包 (72字节字符数据: %s)... ", 
               i + 1, m_config.send_count, (char*)m_config.data_buffer);

        // 发送72字节数据（GattLib自动处理长写/分片）
        int ret = gattlib_write_char_by_uuid(
            connection,
            &m_config.char_uuid,
            m_config.data_buffer,
            m_config.data_len
        );

        if (ret == 0) {
            printf("成功\n");
            m_state.success_count++;
        } else {
            printf("失败 (错误码: %d)\n", ret);
            m_state.fail_count++;
        }

        // 最后一次发送不等待
        if (i != m_config.send_count - 1) {
            usleep(m_config.interval_ms * 1000);
        }
    }

    // 标记完成
    pthread_mutex_lock(&m_state.lock);
    m_state.is_finished = true;
    pthread_cond_signal(&m_state.cond);
    pthread_mutex_unlock(&m_state.lock);
}
// 扫描回调函数：发现目标设备后停止扫描并连接
static void ble_discovered_device(gattlib_adapter_t* adapter, const char* addr, const char* name, void* user_data) {
    if (strcasecmp(addr, m_config.mac_address) == 0) {
        printf("发现目标设备: %s\n", addr);
        device_found = true;
        gattlib_adapter_scan_disable(adapter); // 停止扫描

        // 发起连接
        printf("正在连接设备 %s...\n", m_config.mac_address);
        int ret = gattlib_connect(adapter, m_config.mac_address, 
                            GATTLIB_CONNECTION_OPTIONS_NONE, on_connect, NULL);
        if (ret != 0) {
            fprintf(stderr, "连接请求失败: %d\n", ret);
            gattlib_adapter_close(adapter);
            return;
        }
    }
}


// 主任务函数
static void* ble_task(void* arg) {
    gattlib_adapter_t* adapter;
    int ret;

    // 打开适配器
    ret = gattlib_adapter_open(m_config.adapter_name, &adapter);
    if (ret != 0) {
        fprintf(stderr, "无法打开蓝牙适配器\n");
        return NULL;
    }

    // 开始扫描设备（超时30秒）
    printf("正在扫描设备 %s...\n", m_config.mac_address);
    ret = gattlib_adapter_scan_enable(adapter, ble_discovered_device, 30, NULL);
    if (ret != 0) {
        fprintf(stderr, "扫描失败: %d\n", ret);
        gattlib_adapter_close(adapter);
        return NULL;
    }


    // 等待扫描发现设备
    int scan_wait = 0;
    while (!device_found && scan_wait < 30) { // 最多等30秒
        g_usleep(1000000); // 每秒检查一次
        scan_wait++;
    }
    if (!device_found) {
        fprintf(stderr, "30秒内未发现目标设备\n");
        gattlib_adapter_close(adapter);
        return NULL;
    }
    // 等待连接完成并执行发送任务
    send_continuous_data();

    // 等待发送完成后断开连接
    pthread_mutex_lock(&m_state.lock);
    while (!m_state.is_finished) {
        pthread_cond_wait(&m_state.cond, &m_state.lock);
    }
    pthread_mutex_unlock(&m_state.lock);

    // 断开连接
    gattlib_disconnect(m_state.connection, true);
    gattlib_adapter_close(adapter);
    return NULL;
}

// 帮助信息
static void usage(const char* program) {
    printf("用法: %s <设备MAC> <特征UUID> <发送次数> <间隔毫秒>\n", program);
    printf("示例: %s 70:19:88:3D:30:68 0000ffe1-0000-1000-8000-00805f9b34fb 10 100\n", program);
    printf("说明: 自动发送72字节字符数据（hello+67个x）\n");
}


int main(int argc, char* argv[]) {
    // 检查参数
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    // 初始化状态变量
    pthread_mutex_init(&m_state.lock, NULL);
    pthread_cond_init(&m_state.cond, NULL);
    m_state.is_connected = false;
    m_state.is_finished = false;
    m_state.success_count = 0;
    m_state.fail_count = 0;
    device_found = false;

    // 解析参数
    m_config.mac_address = argv[1];
    m_config.adapter_name = NULL;  // 使用默认适配器(hci0)
    m_config.send_count = atoi(argv[3]);
    m_config.interval_ms = atoi(argv[4]);
    m_config.data_len = 72; // 固定72字节
    // 解析UUID
    if (gattlib_string_to_uuid(argv[2], strlen(argv[2]) + 1, &m_config.char_uuid) != 0) {
        fprintf(stderr, "无效的UUID格式\n");
        return 1;
    }

    // 检查参数有效性
    if (m_config.send_count <= 0 || m_config.interval_ms < 0 || m_config.data_len == 0) {
        fprintf(stderr, "无效的参数值\n");
        return 1;
    }
    // 分配72字节缓冲区并生成字符数据
    m_config.data_buffer = malloc(m_config.data_len + 1); // +1用于字符串终止符
    if (!m_config.data_buffer) {
        fprintf(stderr, "内存分配失败\n");
        return 1;
    }
    generate_72bytes_char_data(m_config.data_buffer, m_config.data_len);

    // 启动主循环
    printf("开始连续发送测试: 共%d个包, 间隔%dms, 72字节字符数据: %s\n",
           m_config.send_count, m_config.interval_ms, (char*)m_config.data_buffer);

    int ret = gattlib_mainloop(ble_task, NULL);

    // 输出统计结果
    printf("\n发送完成 - 成功: %d, 失败: %d, 总发送: %d\n",
           m_state.success_count, m_state.fail_count, m_config.send_count);

    // 清理资源
    free(m_config.data_buffer);
    pthread_mutex_destroy(&m_state.lock);
    pthread_cond_destroy(&m_state.cond);

    return ret;
}