#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <glib.h>
#include <gattlib.h>
#include <stdint.h>
#include <time.h>

// 协议常量定义
#define CMD_A0 0xA0
#define CMD_A1 0xA1
#define CMD_A2 0xA2
#define CMD_A3 0xA3
#define NOTIFY_UUID "0xffe4"
#define MAX_RETRIES 3           // 最大重发次数
#define FEEDBACK_TIMEOUT_MS 1000 // 反馈超时时间(毫秒)

// 配置参数
static struct {
    const char* adapter_name;
    const char* mac_address;
    uuid_t char_uuid;           // 发送特征UUID
    uuid_t notify_uuid;         // 通知特征UUID
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
    bool last_send_success;     // 上一次发送结果
    uint8_t* last_packet;       // 上一次发送的数据包
    size_t last_packet_len;     // 上一次发送的数据包长度
    bool waiting_feedback;      // 是否等待反馈
} m_state;

// 全局变量：标记是否发现目标设备
static bool device_found = false;
// 校验和计算函数
static uint16_t calculate_checksum(const uint8_t* data, size_t len) {
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return sum; // 取低16位作为校验和
}

// 数据包打印函数
static void print_packet(const uint8_t* data, size_t len) {
    printf("Packet (len: %zu): ", len);
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n          ");
    }
    printf("\n");
}


// 构建A0包 (调速)
// 返回值：构建的数据包长度，0表示失败
static size_t build_a0_packet(uint8_t gear, uint8_t* buffer, size_t buffer_len) {
    const size_t required_len = 1 + 1 + 2; // cmd(1) + gear(1) + checksum(2)
    if (buffer_len < required_len) return 0;

    buffer[0] = CMD_A0;
    buffer[1] = gear;
    
    // 计算校验和
    uint16_t checksum = calculate_checksum(buffer, 2);
    buffer[2] = (checksum >> 8) & 0xFF;  // 高8位
    buffer[3] = checksum & 0xFF;         // 低8位

    return required_len;
}

// 构建A1包 (基础信息)
static size_t build_a1_packet(uint8_t play_mode, uint8_t total_lists, 
                             uint8_t current_list, uint8_t effect_count, 
                             uint8_t current_effect, uint8_t* buffer, size_t buffer_len) {
    const size_t required_len = 1 + 5 + 2; // cmd(1) + 5个u8 + checksum(2)
    if (buffer_len < required_len) return 0;

    buffer[0] = CMD_A1;
    buffer[1] = play_mode;
    buffer[2] = total_lists;
    buffer[3] = current_list;
    buffer[4] = effect_count;
    buffer[5] = current_effect;
    
    // 计算校验和
    uint16_t checksum = calculate_checksum(buffer, 6);
    buffer[6] = (checksum >> 8) & 0xFF;
    buffer[7] = checksum & 0xFF;

    return required_len;
}

// 构建A2包 (名称基本)
static size_t build_a2_packet(uint16_t total_bytes, uint8_t total_packets, 
                             uint8_t total_chars, const uint8_t* type_list,
                             uint8_t* buffer, size_t buffer_len) {
    const size_t required_len = 1 + 2 + 1 + 1 + 16 + 2; // 各字段总和
    if (buffer_len < required_len || !type_list) return 0;

    buffer[0] = CMD_A2;
    buffer[1] = (total_bytes >> 8) & 0xFF;  // 总字节数高8位
    buffer[2] = total_bytes & 0xFF;         // 总字节数低8位
    buffer[3] = total_packets;
    buffer[4] = total_chars;
    memcpy(buffer + 5, type_list, 16);      // 类型列表
    
    // 计算校验和
    uint16_t checksum = calculate_checksum(buffer, 4 + 16);
    buffer[21] = (checksum >> 8) & 0xFF;    // 5+16=21
    buffer[22] = checksum & 0xFF;

    return required_len;
}

// 构建A3包 (字节数据)
static size_t build_a3_packet(uint8_t current_packet, uint8_t current_len, 
                             const uint8_t* data, uint8_t* buffer, size_t buffer_len) {
    const size_t required_len = 1 + 1 + 1 + 64 + 2; // 各字段总和
    if (buffer_len < required_len || !data || current_len > 64) return 0;

    buffer[0] = CMD_A3;
    buffer[1] = current_packet;
    buffer[2] = current_len;
    memcpy(buffer + 3, data, 64);  // 数据段(固定64字节)
    
    // 计算校验和
    uint16_t checksum = calculate_checksum(data, 64);
    buffer[67] = (checksum >> 8) & 0xFF;    // 3+64=67
    buffer[68] = checksum & 0xFF;

    return required_len;
}

// 通知回调函数 (接收设备反馈)
static void notification_callback(const uuid_t* uuid, const uint8_t* data, 
                                 size_t data_length, void* user_data) {
    char uuid_str[37];
    char temp[37];
    gattlib_uuid_to_string(uuid, uuid_str, sizeof(uuid_str));
    
    printf("收到通知: UUID=%s, 长度=%zu\n", uuid_str, data_length);
    
    // 打印完整数据用于调试
    if (data_length > 0) {
        printf("数据内容: ");
        for (size_t i = 0; i < data_length; i++) {
            printf("%02X ", data[i]);
        }
        printf("\n");
    }
    printf("uuid_str: %s strlen : %d \n", uuid_str, (int)strlen(uuid_str));
    for (size_t i = 0; i < strlen(uuid_str); i++) {
        printf("char %zu : %c (0x%02X)\n", i, uuid_str[i], (uint8_t)uuid_str[i]);
    }
    memcpy(temp, NOTIFY_UUID, sizeof(NOTIFY_UUID));
    printf("NOTIFY_UUID: %s strlen : %d \n", NOTIFY_UUID, (int)strlen(NOTIFY_UUID));
    printf("temp: %s strlen : %d \n", temp, (int)strlen(temp));
    for (size_t i = 0; i < strlen(temp); i++) {
        printf("char %zu : %c (0x%02X)\n", i, temp[i], (uint8_t)temp[i]);
    }
    if (strcmp(uuid_str, NOTIFY_UUID) == 0) {
        pthread_mutex_lock(&m_state.lock);
        if (data_length >= 1) {
            if (data[0] == 0x01) {
                printf("收到成功反馈 (0x01)\n");
                m_state.last_send_success = true;
            } else if (data[0] == 0x00) {
                printf("收到失败反馈 (0x00)\n");
                m_state.last_send_success = false;
            } else {
                printf("收到未知反馈: 0x%02X\n", data[0]);
                // 从gattool日志看，设备可能发送的是0x01
                // 但您的协议可能定义不同
                m_state.last_send_success = (data[0] == 0x01);
            }
        } else {
            printf("收到无效反馈数据（空）\n");
            m_state.last_send_success = false;
        }
        pthread_cond_signal(&m_state.cond);
        pthread_mutex_unlock(&m_state.lock);
    } else {
        printf("收到非目标UUID的通知, uuid_str : %s\n", uuid_str);
    }
}

// 带重发机制的数据包发送函数
static bool send_packet_with_retry(gattlib_connection_t* connection, 
                                  const uint8_t* data, size_t len) {
    int retries = 0;
    while (retries < MAX_RETRIES) {
        printf("发送数据包 (第%d次尝试):\n", retries + 1);
        print_packet(data, len);
        // 调试：检查连接状态
        if (connection == NULL) {
            printf("错误：连接为空\n");
            return false;
        }

        // 发送数据
        int ret = gattlib_write_char_by_uuid(connection, &m_config.char_uuid, data, len);
        if (ret != 0) {
            printf("发送失败 (错误码: %d)\n", ret);
            
            // 尝试使用句柄发送（从gattool日志看句柄是0x0023）
            printf("尝试使用句柄发送...\n");
            ret = gattlib_write_char_by_handle(connection, 0x0023, data, len);
            if (ret != 0) {
                printf("句柄发送也失败 (错误码: %d)\n", ret);
                retries++;
                continue;
            } else {
                printf("使用句柄发送成功\n");
            }
        } else {
            printf("使用UUID发送成功\n");
        }
        // 等待反馈
        pthread_mutex_lock(&m_state.lock);
        m_state.waiting_feedback = true;
        m_state.last_send_success = false;
        
        // 缓存当前包用于重发
        free(m_state.last_packet);
        m_state.last_packet = malloc(len);
        if (m_state.last_packet) {
            memcpy(m_state.last_packet, data, len);
            m_state.last_packet_len = len;
        }

        // 设置超时
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += FEEDBACK_TIMEOUT_MS / 1000;
        ts.tv_nsec += (FEEDBACK_TIMEOUT_MS % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }

        // 等待反馈
        int cond_ret = pthread_cond_timedwait(&m_state.cond, &m_state.lock, &ts);
        m_state.waiting_feedback = false;
        bool success = m_state.last_send_success;
        pthread_mutex_unlock(&m_state.lock);

        if (cond_ret == ETIMEDOUT) {
            printf("等待反馈超时\n");
            retries++;
        } else if (success) {
            printf("数据包发送成功\n");
            m_state.success_count++;
            return true;
        } else {
            printf("数据包发送失败，准备重发\n");
            retries++;
        }
    }

    printf("达到最大重发次数，发送失败\n");
    m_state.fail_count++;
    return false;
}

// 发送A2+A3组合数据包
static bool send_a2_a3_combination(gattlib_connection_t* connection, 
                                  uint16_t total_bytes, uint8_t total_packets,
                                  uint8_t total_chars, const uint8_t* type_list,
                                  const uint8_t* data, size_t data_len) {
    // 构建并发送A2包
    uint8_t a2_buffer[32]; // 足够容纳A2包
    size_t a2_len = build_a2_packet(total_bytes, total_packets, 
                                   total_chars, type_list, a2_buffer, sizeof(a2_buffer));
    if (a2_len == 0) {
        fprintf(stderr, "构建A2包失败\n");
        return false;
    }

    if (!send_packet_with_retry(connection, a2_buffer, a2_len)) {
        fprintf(stderr, "A2包发送失败\n");
        return false;
    }

    // 发送A3包序列
    for (uint8_t i = 0; i < total_packets; i++) {
        uint8_t a3_buffer[70]; // 足够容纳A3包
        size_t current_len = (i == total_packets - 1) ? 
                            (data_len % 64 ? data_len % 64 : 64) : 64;
        
        // 构建A3包
        size_t a3_len = build_a3_packet(i + 1, current_len, 
                                       data + i * 64, a3_buffer, sizeof(a3_buffer));
        if (a3_len == 0) {
            fprintf(stderr, "构建A3包 %d 失败\n", i + 1);
            return false;
        }

        // 发送A3包
        if (!send_packet_with_retry(connection, a3_buffer, a3_len)) {
            fprintf(stderr, "A3包 %d 发送失败\n", i + 1);
            return false;
        }
    }

    return true;
}

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
    // 步骤1：注册通知回调（关键修正）
    int ret = gattlib_register_notification(connection, notification_callback, NULL);
    if (ret != 0) {
        fprintf(stderr, "注册通知回调失败: %d\n", ret);
    } else {
        printf("已注册通知回调\n");
    }

    // 步骤2：启动通知监听（仅传2个参数，修正编译错误）
    ret = gattlib_notification_start(connection, &m_config.notify_uuid);
    if (ret != 0) {
        fprintf(stderr, "启动通知监听失败: %d\n", ret);
        return;
    } else {
        printf("已启动通知监听，等待设备反馈...\n");
        // 关键：等待一小段时间确保通知完全启用
        usleep(500000);  // 等待500ms，让设备准备就绪
    }
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
    while (!m_state.is_connected) {
        pthread_cond_wait(&m_state.cond, &m_state.lock);
    }
    gattlib_connection_t* connection = m_state.connection;
    pthread_mutex_unlock(&m_state.lock);

    // 示例1: 发送A0包 (调速，档位3)
    printf("\n===== 发送A0包 =====");
    uint8_t a0_buffer[4];
    size_t a0_len = build_a0_packet(3, a0_buffer, sizeof(a0_buffer));
    if (a0_len > 0) {
        send_packet_with_retry(connection, a0_buffer, a0_len);
    }

    // 示例2: 发送A1包 (基础信息)
    printf("\n===== 发送A1包 =====");
    uint8_t a1_buffer[8];
    size_t a1_len = build_a1_packet(
        0x01,   // 播放模式
        5,      // 总列表数
        2,      // 当前列表
        10,     // 效果数目
        3,      // 当前效果序号
        a1_buffer, sizeof(a1_buffer)
    );
    if (a1_len > 0) {
        send_packet_with_retry(connection, a1_buffer, a1_len);
    }

    // 示例3: 发送A2+A3组合包
    printf("\n===== 发送A2+A3组合包 =====");
    uint8_t type_list[16] = {0x01, 0x01, 0x01, 0x01}; // 示例类型列表
   // uint8_t big_data[180]; // 示例大数据 (3个A3包：2*64 + 52 = 180)

     uint8_t big_data[] = {
        0x4 , 0x4 , 0xc4, 0xfc, 0x14, 0x2f, 0xa4, 0xa4,
        0xa4, 0xa4, 0x2f, 0x24, 0xe4, 0x24, 0x24, 0x0 ,
        0x2 , 0x1 , 0x0 , 0xff, 0x0 , 0x0 , 0x1f, 0x8 ,
        0x8 , 0x1f, 0x40, 0x80, 0x7f, 0x0 , 0x0 , 0x0 , // 1
        0x10, 0x10, 0xfe, 0x10, 0x10, 0xfc, 0x44, 0x54,
        0x55, 0xfe, 0x54, 0x54, 0xf4, 0x44, 0x44, 0x0 ,
        0x10, 0x10, 0xf , 0x48, 0x28, 0x1f, 0x0 , 0x7d,
        0x25, 0x27, 0x25, 0x25, 0x7d, 0x0 , 0x0 , 0x0 , //  2
        0x0 , 0x0 , 0x0 , 0x0 , 0x0 , 0xff, 0x11, 0x11,
        0x11, 0x11, 0x11, 0xff, 0x0 , 0x0 , 0x0 , 0x0 ,
        0x0 , 0x40, 0x20, 0x10, 0xc , 0x3 , 0x1 , 0x1 ,
        0x1 , 0x21, 0x41, 0x3f, 0x0 , 0x0 , 0x0 , 0x0 ,  // 3
        0x0 , 0x40, 0x20, 0xf0, 0x28, 0x27, 0x24, 0xe4,
        0x24, 0x34, 0x2c, 0xe4, 0x0 , 0x0 , 0x0 , 0x0 ,
        0x0 , 0x0 , 0x0 , 0x3f, 0x42, 0x42, 0x42, 0x43,
        0x42, 0x42, 0x42, 0x43, 0x40, 0x78, 0x0 , 0x0 ,  // 4
        0x0 , 0x30, 0x8 , 0x88, 0x88, 0x48, 0x30, 0x0 ,
        0x0 , 0x18, 0x20, 0x20, 0x20, 0x11, 0xe , 0x0 ,  // 5
        0x0 , 0x0 , 0x0 , 0x80, 0x80, 0x88, 0xf8, 0x0 ,
        0x0 , 0xe , 0x11, 0x20, 0x20, 0x10, 0x3f, 0x20, // 6
        0x80, 0x80, 0x80, 0x0 , 0x80, 0x80, 0x80, 0x0 ,
        0x20, 0x20, 0x3f, 0x21, 0x20, 0x0 , 0x1 , 0x0 , // 7
        0x80, 0x80, 0x80, 0x0 , 0x80, 0x80, 0x80, 0x0 ,
        0x20, 0x20, 0x3f, 0x21, 0x20, 0x0 , 0x1 , 0x0  // 8
    };
    send_a2_a3_combination(
        connection,
        sizeof(big_data),                // 总字节数
        3,                  // 总包数
        8,                 // 总字符数
        type_list,          // 类型列表
        big_data,           // 数据内容
        sizeof(big_data)   // 数据长度
    );

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
    printf("用法: %s <设备MAC> <发送特征UUID> <通知特征UUID>\n", program);
    printf("示例: %s 70:19:88:3D:30:68 0000ffe1-0000-1000-8000-00805f9b34fb 0000ffe4-0000-1000-8000-00805f9b34fb\n", program);
    printf("说明: 发送四种协议数据包并处理反馈\n");
}
int main(int argc, char* argv[]) {
    // 检查参数
    if (argc != 4) {
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
    m_state.last_send_success = false;
    m_state.last_packet = NULL;
    m_state.last_packet_len = 0;
    m_state.waiting_feedback = false;
    device_found = false;

    // 解析参数
    m_config.mac_address = argv[1];
    m_config.adapter_name = NULL;  // 使用默认适配器(hci0)
    
    // 解析发送特征UUID
    if (gattlib_string_to_uuid(argv[2], strlen(argv[2]) + 1, &m_config.char_uuid) != 0) {
        fprintf(stderr, "无效的发送特征UUID格式\n");
        return 1;
    }

    // 解析通知特征UUID
    if (gattlib_string_to_uuid(argv[3], strlen(argv[3]) + 1, &m_config.notify_uuid) != 0) {
        fprintf(stderr, "无效的通知特征UUID格式\n");
        return 1;
    }

    // 启动主循环
    printf("开始BLE协议数据发送测试...\n");
    int ret = gattlib_mainloop(ble_task, NULL);

    // 输出统计结果
    printf("\n发送完成 - 成功: %d, 失败: %d, 总尝试: %d\n",
           m_state.success_count, m_state.fail_count, 
           m_state.success_count + m_state.fail_count);

    // 清理资源
    free(m_state.last_packet);
    pthread_mutex_destroy(&m_state.lock);
    pthread_cond_destroy(&m_state.cond);

    return ret;
}