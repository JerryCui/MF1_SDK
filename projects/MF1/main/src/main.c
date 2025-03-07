#include "face_lib.h"

#include "global_build_info_version.h"

#include "face_cb.h"
#include "camera.h"

#include "lcd.h"
#include "lcd_dis.h"

#include "wdt.h"

#if CONFIG_ENABLE_OUTPUT_JPEG
#include "cQueue.h"
#include "core1.h"
#endif /* CONFIG_ENABLE_OUTPUT_JPEG */

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static uint64_t last_open_relay_time_in_s = 0;
static volatile uint8_t relay_open_flag = 0;
static uint64_t last_pass_time = 0;

static void uart_send(char *buf, size_t len);

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
face_recognition_cfg_t face_recognition_cfg = {
    .check_ir_face = 1,
    .auto_out_fea = 0,

    .night_threshold = 60,

#if CONFIG_ENABLE_FLASH_LED
    .use_flash_led = 1,
#else
    .use_flash_led = 0,
#endif
    .no_face_close_lcd = 0,

    .detect_threshold = 0.0,
    .compare_threshold = 0.0,
};

face_lib_callback_t face_recognition_cb = {
    .proto_send = uart_send,
    .proto_record_face = protocol_record_face,

    .detected_face_cb = detected_face_cb,
    .fake_face_cb = fake_face_cb,
    .pass_face_cb = pass_face_cb,

    .lcd_refresh_cb = lcd_refresh_cb,
    .lcd_convert_cb = lcd_convert_cb,
    .lcd_close_cb = lcd_close_bl_cb,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static void uart_send(char *buf, size_t len)
{
    uart_send_data(PROTOCOL_UART_NUM, buf, len);
}

static void open_relay(void)
{
    uint64_t tim = sysctl_get_time_us();

#if CONFIG_RELAY_NUM >= 1
    gpiohs_set_pin(CONFIG_GPIOHS_NUM_RELAY_1, CONFIG_RELAY_1_OPEN_VOL);
#if CONFIG_RELAY_NUM >= 2
    gpiohs_set_pin(CONFIG_GPIOHS_NUM_RELAY_2, CONFIG_RELAY_2_OPEN_VOL);
#endif /* CONFIG_RELAY_NUM */
#endif /* CONFIG_RELAY_NUM */

    last_open_relay_time_in_s = tim / 1000 / 1000;
    relay_open_flag = 1;
}

static void close_relay(void)
{
#if CONFIG_RELAY_NUM >= 1
    gpiohs_set_pin(CONFIG_GPIOHS_NUM_RELAY_1, 1 - CONFIG_RELAY_1_OPEN_VOL);
#if CONFIG_RELAY_NUM >= 2
    gpiohs_set_pin(CONFIG_GPIOHS_NUM_RELAY_2, 1 - CONFIG_RELAY_2_OPEN_VOL);
#endif /* CONFIG_RELAY_NUM */
#endif /* CONFIG_RELAY_NUM */
}

//FIXME: 这个不能多核公用
int wdt_irq_core0(void *ctx)
{
    static int s_wdt_irq_cnt = 0;
    s_wdt_irq_cnt++;
    if (s_wdt_irq_cnt < 2)
    {
        wdt_clear_interrupt(0);
    }
    else
    {
        while (1)
        {
            sysctl_reset(SYSCTL_RESET_SOC);
        };
    }
    return 0;
}

#define WATCH_DOG_TIMEOUT_MS (8 * 1000)
#define WATCH_DOG_FEED_TIME_MS (1 * 1000)

void watchdog_init(uint8_t id)
{
    wdt_start(id, WATCH_DOG_TIMEOUT_MS, wdt_irq_core0);
}

void watchdog_feed(uint8_t id)
{
    wdt_feed(id);
}

#if CONFIG_FACE_PASS_ONLY_OUT_UID
static void hex2str(uint8_t *inchar, uint16_t len, uint8_t *outtxt)
{
    uint16_t i;
    uint8_t hbit, lbit;

    for (i = 0; i < len; i++)
    {
        hbit = (*(inchar + i) & 0xf0) >> 4;
        lbit = *(inchar + i) & 0x0f;
        if (hbit > 9)
            outtxt[2 * i] = 'A' + hbit - 10;
        else
            outtxt[2 * i] = '0' + hbit;
        if (lbit > 9)
            outtxt[2 * i + 1] = 'A' + lbit - 10;
        else
            outtxt[2 * i + 1] = '0' + lbit;
    }
    outtxt[2 * i] = 0;
    return;
}
#endif /* CONFIG_FACE_PASS_ONLY_OUT_UID */

/********************* Add your callback code here *********************/
void face_pass_callback(face_obj_t *obj, uint32_t total, uint32_t current, uint64_t *time)
{
    face_info_t info;
    uint64_t tim = 0;

    tim = sysctl_get_time_us();

    if (g_board_cfg.brd_soft_cfg.cfg.out_interval_ms != 0)
    {
        if (((tim - last_pass_time) / 1000) < g_board_cfg.brd_soft_cfg.cfg.out_interval_ms)
        {
            printk("last face pass time too short\r\n");
            return;
        }
    }

    last_pass_time = tim;

#if CONFIG_ENABLE_UART_PROTOCOL
    /* output feature */
    if (g_board_cfg.brd_soft_cfg.cfg.auto_out_fea)
    {
#if CONFIG_FACE_PASS_ONLY_OUT_UID
        char str[48];
        int len = sprintf(str, "UNKNOWN PEOPLE\r\n");
        uart_send(str, len);
#else
        protocol_send_face_info(obj,
                                0, NULL, obj->feature,
                                total, current, time);
#endif /* CONFIG_FACE_PASS_ONLY_OUT_UID */

        open_relay(); //open when have face
    }
    else
    {
        if (obj->score > g_board_cfg.brd_soft_cfg.out_threshold)
        {
            open_relay(); //open when score > gate
            if (flash_get_saved_faceinfo(&info, obj->index) == 0)
            {
#if CONFIG_FACE_PASS_ONLY_OUT_UID
                char str[48];
                hex2str(info.uid, UID_LEN, str);
                str[UID_LEN * 2 + 0] = 0xd;
                str[UID_LEN * 2 + 1] = 0xa;
                uart_send(str, UID_LEN * 2 + 2);
#endif /* CONFIG_FACE_PASS_ONLY_OUT_UID */

                if (g_board_cfg.brd_soft_cfg.cfg.out_fea == 2)
                {
#if (CONFIG_FACE_PASS_ONLY_OUT_UID == 0)
                    //output real time feature
                    protocol_send_face_info(obj,
                                            obj->score, info.uid, g_board_cfg.brd_soft_cfg.cfg.out_fea ? obj->feature : NULL,
                                            total, current, time);
#endif /* CONFIG_FACE_PASS_ONLY_OUT_UID */
                }
                else
                {
                    //output stored in flash face feature
                    face_fea_t *face_fea = (face_fea_t *)&(info.info);
#if (CONFIG_FACE_PASS_ONLY_OUT_UID == 0)
                    protocol_send_face_info(obj,
                                            obj->score, info.uid,
                                            g_board_cfg.brd_soft_cfg.cfg.out_fea ? (face_fea->stat == 1) ? face_fea->fea_ir : face_fea->fea_rgb : NULL,
                                            total, current, time);
#endif /* CONFIG_FACE_PASS_ONLY_OUT_UID */
                }
            }
            else
            {
                printk("index error!\r\n");
            }
        }
        else
        {
            printk("face score not pass\r\n");
        }
    }
#else
    open_relay();
#endif /* CONFIG_ENABLE_UART_PROTOCOL */
    if (g_board_cfg.brd_soft_cfg.cfg.auto_out_fea == 0)
    {
        lcd_draw_pass();
    }
    return;
}

#if CONFIG_ENABLE_UART_PROTOCOL
//recv: {"version":1,"type":"test"}
//send: {"version":1,"type":"test","code":0,"msg":"test"}
void test_cmd(cJSON *root)
{
    cJSON *ret = protocol_gen_header("test", 0, "test");
    if (ret)
    {
        protocol_send(ret);
    }
    cJSON_Delete(ret);
    return;
}

//recv: {"version":1,"type":"test2","log_tx":10}
//send: {"1":1,"type":"test2","code":0,"msg":"test2"}
void test2_cmd(cJSON *root)
{
    cJSON *ret = NULL;
    cJSON *tmp = NULL;

    tmp = cJSON_GetObjectItem(root, "log_tx");
    if (tmp == NULL)
    {
        printk("no log_tx recv\r\n");
    }
    else
    {
        printk("log_tx:%d\r\n", tmp->valueint);
    }

    ret = protocol_gen_header("test2", 0, "test2");
    if (ret)
    {
        protocol_send(ret);
    }
    cJSON_Delete(ret);
    return;
}

protocol_custom_cb_t user_custom_cmd[] = {
    {.cmd = "test", .cmd_cb = test_cmd},
    {.cmd = "test2", .cmd_cb = test2_cmd},
};
#endif /* CONFIG_ENABLE_UART_PROTOCOL */

int main(void)
{
    static uint64_t last_feed_ms = 0;
    uint64_t tim = 0;

    board_init();
    face_lib_init_module();

    face_cb_init();
    lcd_dis_list_init();

    printk("firmware version:\r\n%d.%d.%d\r\n", BUILD_VERSION_MAJOR, BUILD_VERSION_MINOR, BUILD_VERSION_MICRO);
    printk("face_lib_version:\r\n%s\r\n", face_lib_version());

    /*load cfg from flash*/
    if (flash_load_cfg(&g_board_cfg) == 0)
    {
        printk("load cfg failed,save default config\r\n");

        flash_cfg_set_default(&g_board_cfg);

        if (flash_save_cfg(&g_board_cfg) == 0)
        {
            printk("save g_board_cfg failed!\r\n");
        }
    }

    flash_cfg_print(&g_board_cfg);

    face_lib_regisiter_callback(&face_recognition_cb);

#if !CONFIG_ENABLE_UART_PROTOCOL
    init_lcd_cam(&g_board_cfg);
    init_relay_key_pin(&g_board_cfg);
#if CONFIG_NET_ENABLE
#if CONFIG_NET_ESP8285
    extern void demo_esp8285(void);
    demo_esp8285();
#elif CONFIG_NET_W5500
    extern void demo_w5500(void);
    demo_w5500();
#endif /* CONFIG_NET_ESP8285 */
#endif /* CONFIG_NET_ENABLE */
#else
#if CONFIG_NET_ENABLE
    protocol_init_device(&g_board_cfg, 1);
#if CONFIG_NET_ESP8285
    extern void demo_esp8285(void);
    demo_esp8285();
#elif CONFIG_NET_W5500
    extern void demo_w5500(void);
    demo_w5500();
#endif /* CONFIG_NET_ESP8285 */
#endif /* CONFIG_NET_ENABLE */
#endif /* CONFIG_ENABLE_OUTPUT_JPEG */

#if CONFIG_ENABLE_UART_PROTOCOL
#if CONFIG_ENABLE_OUTPUT_JPEG
    /* 这个需要可配置 */
    fpioa_set_function(CONFIG_JPEG_OUTPUT_PORT_TX, FUNC_UART1_TX + UART_DEV2 * 2);
    fpioa_set_function(CONFIG_JPEG_OUTPUT_PORT_RX, FUNC_UART1_RX + UART_DEV2 * 2);
    uart_config(UART_DEV2, CONFIG_OUTPUT_JPEG_UART_BAUD, 8, UART_STOP_1, UART_PARITY_NONE);

    printk("jpeg output uart cfg:\r\n");
    printk("                     tx:%d rx:%d, baud:%d\r\n",
           CONFIG_JPEG_OUTPUT_PORT_TX, CONFIG_JPEG_OUTPUT_PORT_RX, CONFIG_OUTPUT_JPEG_UART_BAUD);
    q_init(&q_core1, sizeof(void *), 10, FIFO, false); //core1 接受core0 上传图片到服务器的队列
    register_core1(core1_function, NULL);
#endif /* CONFIG_ENABLE_OUTPUT_JPEG */

    /* init device */
    protocol_regesiter_user_cb(&user_custom_cmd[0], sizeof(user_custom_cmd) / sizeof(user_custom_cmd[0]));
    protocol_init_device(&g_board_cfg, 0);
    protocol_send_init_done();
#endif /* CONFIG_ENABLE_UART_PROTOCOL */

    // /* all cfg, if modify by uart, must reboot to get effect */
    // face_recognition_cfg.auto_out_fea = (uint8_t)g_board_cfg.brd_soft_cfg.cfg.auto_out_fea;
    // face_recognition_cfg.compare_threshold = (float)g_board_cfg.brd_soft_cfg.out_threshold;

    set_lcd_bl(1);

    watchdog_init(0); /* init watch dog */

    while (1)
    {
#if CONFIG_ENABLE_UART_PROTOCOL
        if (!jpeg_recv_start_flag)
#endif /* CONFIG_ENABLE_UART_PROTOCOL */
        {
            face_recognition_cfg.auto_out_fea = (uint8_t)g_board_cfg.brd_soft_cfg.cfg.auto_out_fea;
            face_recognition_cfg.compare_threshold = (float)g_board_cfg.brd_soft_cfg.out_threshold;
            face_lib_run(&face_recognition_cfg);
        }

        /* get key state */
        update_key_state();

        if (g_key_long_press)
        {
            g_key_long_press = 0;
            /* key long press */
            printk("key long press\r\n");

#if CONFIG_LONG_PRESS_FUNCTION_KEY_RESTORE
            /* set cfg to default */
            printk("reset board config\r\n");
            board_cfg_t board_cfg;

            memset(&board_cfg, 0, sizeof(board_cfg_t));

            flash_cfg_set_default(&board_cfg);

            if (flash_save_cfg(&board_cfg) == 0)
            {
                printk("save board_cfg failed!\r\n");
            }
            /* set cfg to default end */
#if CONFIG_LONG_PRESS_FUNCTION_KEY_CLEAR_FEATURE
            printk("Del All feature!\n");
            flash_delete_face_all();
#endif
            char *str_del = (char *)malloc(sizeof(char) * 32);
            sprintf(str_del, "Factory Reset...");
            if (lcd_dis_list_add_str(1, 1, 16, 0, str_del, LCD_OFT, 0, RED, 1) == NULL)
            {
                printk("add dis str failed\r\n");
            }
            delay_flag = 1;
#endif
        }
        /******Process relay open********/
        tim = sysctl_get_time_us();
        tim = tim / 1000 / 1000;

        if (relay_open_flag && ((tim - last_open_relay_time_in_s) >= g_board_cfg.brd_soft_cfg.cfg.relay_open_s))
        {
            close_relay();
            relay_open_flag = 0;
        }

        if (lcd_bl_stat == 0)
        {
            static uint8_t led_cnt = 0, led_stat = 0;
            led_cnt++;
            if (led_cnt >= 10)
            {
                led_cnt = 0;
                led_stat ^= 0x1;
                set_RGB_LED(led_stat ? GLED : 0);
            }
        }

        //feed
        tim = sysctl_get_time_us();
        tim /= 1000; //ms
        if ((tim - last_feed_ms) > WATCH_DOG_FEED_TIME_MS)
        {
            // printk("heap:%ld KB\r\n", get_free_heap_size() / 1024);
            printk("feed wdt\r\n");
            last_feed_ms = tim;
            watchdog_feed(0);
        }

#if CONFIG_ENABLE_UART_PROTOCOL
        /******Process uart protocol********/
        if (recv_over_flag)
        {
            protocol_prase(cJSON_prase_buf);
            recv_over_flag = 0;
        }

        if (jpeg_recv_start_flag)
        {
            tim = sysctl_get_time_us();
            if (tim - jpeg_recv_start_time >= 10 * 1000 * 1000) //FIXME: 10s or 5s timeout
            {
                printk("abort to recv jpeg file\r\n");
                jpeg_recv_start_flag = 0;
                protocol_stop_recv_jpeg();
                protocol_send_cal_pic_result(7, "timeout to recv jpeg file", NULL, NULL, 0); //7  jpeg verify error
            }

            /* recv over */
            if (jpeg_recv_len != 0)
            {
                protocol_cal_pic_fea(&cal_pic_cfg, protocol_send_cal_pic_result);
                jpeg_recv_len = 0;
                jpeg_recv_start_flag = 0;
            }
        }
#endif /* CONFIG_ENABLE_UART_PROTOCOL */
    }
}
