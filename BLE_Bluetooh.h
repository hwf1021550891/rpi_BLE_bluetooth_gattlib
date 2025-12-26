#ifndef BLE_BLUETOOTH_H

#define BLE_BLUETOOTH_H


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


// A0命令：调速
typedef struct {
    uint8_t cmd;            // 命令字 (0xA0)
    uint8_t gear;           // 档位 (0x1C 等)
    uint16_t checksum;       // 校验和
} __attribute__((packed)) ble_cmd_a0_t;

// A1命令：基础信息
typedef struct {
    uint8_t cmd;            // 命令字 (0xA1)
    uint8_t play_mode;      // 播放模式
    uint8_t total_lists;    // 总列表数
    uint8_t current_list;   // 当前列表
    uint8_t effect_count;   // 效果数目
    uint8_t current_effect; // 当前列表效果序号
    uint16_t checksum;       // 校验和
} __attribute__((packed)) ble_cmd_a1_t;

// A2命令：包头
typedef struct {
    uint8_t cmd;            // 命令字 (0xA2)
    uint16_t total_bytes;   // 总字节数 (小端序)
    uint8_t total_packets;  // 总包数
    uint8_t char_len;      // 总字符数
    uint8_t type_list[16];
    uint16_t checksum;       // 校验和
} __attribute__((packed)) ble_cmd_a2_t;

// A3命令：数据包 (可变长度)
typedef struct {
    uint8_t cmd;            // 命令字 (0xA3)
    uint8_t packet_num;     // 当前包序号 (从1开始)
    uint8_t data_len;        // 保留字节
      // 本包数据校验
    uint8_t data[64];         // 可变长度数据
    uint16_t data_checksum;
} __attribute__((packed)) ble_cmd_a3_t;


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


// 数据包打印函数
void print_packet(const uint8_t* data, size_t len) ;

// 构建A0包 (调速)
// 返回值：构建的数据包长度，0表示失败
size_t build_a0_packet(ble_cmd_a0_t* a0_data, uint8_t* buffer, size_t buffer_len) ;
// 构建A1包 (基础信息)
size_t build_a1_packet(ble_cmd_a1_t* a1_data, uint8_t* buffer, size_t buffer_len) ;
// 构建A2包 (名称基本)

size_t build_a2_packet(ble_cmd_a2_t *a2_data, uint8_t* buffer, size_t buffer_len) ;
// 构建A3包 (字节数据)
size_t build_a3_packet(ble_cmd_a3_t* a3_data, uint8_t* buffer, size_t buffer_len) ;
// 带重发机制的数据包发送函数
bool send_packet_with_retry(gattlib_connection_t* connection, 
                                  const uint8_t* data, size_t len);

// 发送A2+A3组合数据包
bool send_a2_a3_combination(gattlib_connection_t* connection, 
                                  ble_cmd_a2_t *a2_data, ble_cmd_a3_t* a3_data,
                                  const uint8_t* data, size_t data_len) ;



// 主任务函数
static void* ble_task(void* arg) ;

// 帮助信息
static void usage(const char* program);

#endif  /* BLE_BLUETOOTH_H */