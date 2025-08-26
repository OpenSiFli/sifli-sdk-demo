#include <stdbool.h>
#include "lvgl.h"
#include "screen.h"
#include "math.h"
#include "bf0_hal.h"
#include <stdio.h>
#include "lvsf_input.h"
#include <string.h>
#include <stdlib.h>
#include <rtdevice.h>
#include "board.h"
#include <time.h>

// Image resource declaration
LV_IMG_DECLARE(img1);

// Basic configuration of the screen
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 128
#define SCREEN_COUNT 6
#define ONE_DATA_MAXLEN 512


// Font definition (using LVGL built-in fonts)
#define TITLE_FONT &lv_font_montserrat_16
#define VALUE_FONT &lv_font_montserrat_20
#define LABEL_FONT &lv_font_montserrat_12
#define _LV_CHART_SERIES_MASK(series_idx) (1 << (16 + series_idx))
// Arrow symbols
#define UP_ARROW LV_SYMBOL_UP
#define DOWN_ARROW LV_SYMBOL_DOWN

system_status_t system_status = {0};
// Screen layout configuration
static ScreenLayout screen_layout[SCREEN_COUNT] = {
    {0, 0, &img1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {SCREEN_WIDTH, 0, &img1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {SCREEN_WIDTH*2, 0, &img1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {0, SCREEN_HEIGHT, &img1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {SCREEN_WIDTH, SCREEN_HEIGHT, &img1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {SCREEN_WIDTH*2, SCREEN_HEIGHT, &img1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}
};

// Screen titles
const char *screen_titles[SCREEN_COUNT] = {
    "CPU MOD", "GPU MOD", "RAM", "HDD", "NETWORK", "DATA\\TIME"
};


/* Create a circular progress bar with small squares. */
static lv_obj_t *create_grid_arc(lv_obj_t *parent, int16_t x, int16_t y, int16_t size)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, x, y);
    
    lv_arc_set_angles(arc, 0, 360);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_arc_color(arc, lv_color_hex(0x444444), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_MAIN);
    
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x00FF00), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 8, LV_PART_INDICATOR);
    
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(arc, lv_color_hex(0x000000), LV_PART_MAIN);
    
    return arc;
}

/* Create a black background with a white striped progress bar */
static lv_obj_t *create_black_white_bar(lv_obj_t *parent, int16_t x, int16_t y, int16_t w, int16_t h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_pos(bar, x, y);
    lv_bar_set_range(bar, 0, 100);

    lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, h/2, LV_PART_MAIN);

    lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    
    return bar;
}
/* Create a dual-line chart */
static lv_obj_t *create_dual_line_chart(
    lv_obj_t *parent, 
    int16_t x, 
    int16_t y, 
    int16_t w, 
    int16_t h, 
    lv_color_t line1_color, 
    lv_color_t line2_color,
    lv_chart_series_t **series1,
    lv_chart_series_t **series2
) {
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_size(chart, w, h);
    lv_obj_set_pos(chart, x, y);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);

    // Inner padding settings
    static lv_style_t style_chart;
    lv_style_init(&style_chart);
    lv_style_set_pad_left(&style_chart, 3);     // Leave space for X-axis labels on the left side.
    lv_style_set_pad_right(&style_chart, 25);    // Leave space for Y-axis labels on the right side.
    lv_style_set_pad_bottom(&style_chart, 15);   // Leave space for X-axis labels on the bottom side.
    lv_style_set_pad_top(&style_chart, 15);      // Leave space for the top side.
    lv_obj_add_style(chart, &style_chart, 0);
    
    // Basic Configuration of Charts
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 20);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_X, 0, 100);

    // Set the number of division lines
    lv_chart_set_div_line_count(chart, 0, 0);


    // Custom horizontal line at the bottom
    lv_obj_t *hline = lv_line_create(chart);
    static lv_point_t hpoints[] = {{0, 0}, {LV_PCT(100), 0}};
    lv_line_set_points(hline, hpoints, 2);
    lv_obj_set_style_line_width(hline, 2, 0);
    lv_obj_set_style_line_color(hline, lv_color_hex(0x00000000), 0);
    lv_obj_align(hline, LV_ALIGN_BOTTOM_MID, 0, 2); // Align to the bottom

    // Custom vertical line on the right side
    lv_obj_t *vline = lv_line_create(chart);
    static lv_point_t vpoints[] = {{0, 0}, {0, LV_PCT(100)}}; // From the top left to the bottom left
    lv_line_set_points(vline, vpoints, 2);
    lv_obj_set_style_line_width(vline, 2, 0);
    lv_obj_set_style_line_color(vline, lv_color_hex(0x00000000), 0);
    lv_obj_align(vline, LV_ALIGN_RIGHT_MID, 2, 0); // Align to the right

    // Calculate content area
    int16_t pad_left = lv_obj_get_style_pad_left(chart, 0);
    int16_t pad_right = lv_obj_get_style_pad_right(chart, 0);
    int16_t pad_top = lv_obj_get_style_pad_top(chart, 0);
    int16_t pad_bottom = lv_obj_get_style_pad_bottom(chart, 0);
    
    int16_t content_x = x + pad_left;
    int16_t content_y = y + pad_top;
    int16_t content_w = w - pad_left - pad_right; 
    int16_t content_h = h - pad_top - pad_bottom;  
    int16_t x_end = content_x + content_w; 
    int16_t y_end = content_y + content_h;
    
    // Y-axis scale label
    const char *y_labels[] = {"0", "20", "40", "60", "80", "100"};
    // Calculate Y-axis step
    int16_t y_step = content_h / 5;
    for(int i = 0; i < 6; i++) {
        lv_obj_t *label = lv_label_create(parent);
        lv_label_set_text(label, y_labels[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, LABEL_FONT, 0);
        
        int16_t label_y = y_end - (i * y_step) - (lv_obj_get_height(label) / 2);
        lv_obj_set_pos(label, x_end + 5, label_y - 10);
    }

    // X-axis unit label (bottom left)
    lv_obj_t *x_unit = lv_label_create(parent);
    lv_label_set_text(x_unit, "t/s");
    lv_obj_set_style_text_color(x_unit, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(x_unit, LABEL_FONT, 0);
    lv_obj_set_pos(x_unit, content_x - 6, y_end + 3);
    
    // Top right side of the Y-axis
    lv_obj_t *y_unit = lv_label_create(parent);
    lv_label_set_text(y_unit, "%/°C");
    lv_obj_set_style_text_color(y_unit, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(y_unit, LABEL_FONT, 0);
    lv_obj_set_pos(y_unit, x_end + 9, content_y - 20);  // Align with the top of the Y-axis
    
    *series1 = lv_chart_add_series(chart, line1_color, LV_CHART_AXIS_PRIMARY_Y);
    *series2 = lv_chart_add_series(chart, line2_color, LV_CHART_AXIS_PRIMARY_Y);
    // Adjust line width
    lv_obj_set_style_line_width(chart, 1, LV_PART_ITEMS | LV_STATE_DEFAULT | _LV_CHART_SERIES_MASK(0));
    lv_obj_set_style_line_width(chart, 1, LV_PART_ITEMS | LV_STATE_DEFAULT | _LV_CHART_SERIES_MASK(1));
    
    lv_obj_set_style_opa(chart, LV_OPA_TRANSP, LV_PART_INDICATOR | LV_STATE_DEFAULT | _LV_CHART_SERIES_MASK(0));
    lv_obj_set_style_opa(chart, LV_OPA_TRANSP, LV_PART_INDICATOR | LV_STATE_DEFAULT | _LV_CHART_SERIES_MASK(1));

    // Ensure line transparency is normal
    lv_obj_set_style_opa(chart, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT | _LV_CHART_SERIES_MASK(0));
    lv_obj_set_style_opa(chart, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT | _LV_CHART_SERIES_MASK(1));


    for (int i = 0; i < 20; i++) {
        lv_chart_set_next_value(chart, *series1, 0);
        lv_chart_set_next_value(chart, *series2, 0);
    }
    return chart;
}


/* Create screen UI elements */
static void create_screen_ui(int screen_idx)
{
    ScreenLayout *screen = &screen_layout[screen_idx];
    lv_obj_t *parent = screen->img_obj;
    
    screen->title_label = lv_label_create(parent);
    lv_label_set_text(screen->title_label, screen_titles[screen_idx]);
    lv_obj_align(screen->title_label, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_text_font(screen->title_label, TITLE_FONT, 0);

    switch (screen_idx) {
        case 0:  // CPU MOD
        case 1:  // GPU MOD
            screen->usage_chart = create_dual_line_chart(
                parent,     
                5, 20,        // x, y coordinate
                110, 80,      // width 110, height 80
                lv_color_hex(0x00FF00),  
                lv_color_hex(0xFFFF00),
                &screen->usage_series,   
                &screen->temp_series  
            );
            screen->usage_label = lv_label_create(parent);
            lv_label_set_recolor(screen->usage_label, true);
            // Rich text format: #color value text#
            lv_label_set_text(screen->usage_label, 
                "#00FF00 CPU: 0.0%#\n#FFFF00 TEMP: 0.0°C#"
            );
            lv_obj_set_style_text_font(screen->usage_label, TITLE_FONT, 0);
            lv_obj_set_style_text_color(screen->usage_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align(screen->usage_label, LV_ALIGN_TOP_MID, 8, 90);
            break;
            
        case 2:  // RAM
            screen->usage_arc = create_grid_arc(parent, 14, 30, 90);
            screen->usage_label = lv_label_create(screen->usage_arc);
            lv_label_set_text(screen->usage_label, "0%");
            lv_obj_set_style_text_font(screen->usage_label, VALUE_FONT, 0);
            lv_obj_center(screen->usage_label);
            break;
            
        case 3:  // HDD
            screen->disk_c_bar = create_black_white_bar(parent, 14, 35, 100, 12);
            screen->disk_c_label = lv_label_create(parent);
            lv_label_set_text(screen->disk_c_label, "C: --.-%");
            lv_obj_set_style_text_color(screen->disk_c_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align_to(screen->disk_c_label, screen->disk_c_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);
            
            screen->disk_d_bar = create_black_white_bar(parent, 14, 80, 100, 12);
            screen->disk_d_label = lv_label_create(parent);
            lv_label_set_text(screen->disk_d_label, "D: --.-%");
            lv_obj_set_style_text_color(screen->disk_d_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align_to(screen->disk_d_label, screen->disk_d_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);
            break;
            
        case 4:  // NETWORK
            screen->net_up_label = lv_label_create(parent);
            lv_label_set_text(screen->net_up_label, UP_ARROW " 0.00 MB/s");
            lv_obj_set_style_text_color(screen->net_up_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align(screen->net_up_label, LV_ALIGN_CENTER, 0, -20);

            screen->net_down_label = lv_label_create(parent);
            lv_label_set_text(screen->net_down_label, DOWN_ARROW " 0.00 MB/s");
            lv_obj_set_style_text_color(screen->net_down_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align(screen->net_down_label, LV_ALIGN_CENTER, 0, 20);
            break;
            
        case 5:  // DATA\TIME
            screen->date_label = lv_label_create(parent);
            lv_label_set_text(screen->date_label, "YYYY-MM-DD");
            lv_obj_set_style_text_color(screen->date_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align(screen->date_label, LV_ALIGN_CENTER, 0, -15);
            
            screen->time_label = lv_label_create(parent);
            lv_label_set_text(screen->time_label, "HH:MM:SS");
            lv_obj_set_style_text_color(screen->time_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_align(screen->time_label, LV_ALIGN_CENTER, 0, 15);
            break;
    }
}

/* Update screen data */
static void update_screen_display(lv_timer_t *timer)
{
    char buf[50];

    if (system_status.has_time_data) {
        char date[20], time_str[10];
        sscanf(system_status.datetime, "%s", date);
        lv_label_set_text(screen_layout[5].date_label, date);
        sscanf(system_status.datetime, "%*s %s", time_str);
        lv_label_set_text(screen_layout[5].time_label, time_str);
        system_status.has_time_data = false; 
    }

    if (system_status.has_monitor_data) {
        // Screen 1: CPU update
        float cpu_usage = system_status.cpu_usage;
        float cpu_temp = system_status.cpu_temp;
        lv_chart_set_next_value(screen_layout[0].usage_chart, screen_layout[0].usage_series, cpu_usage);
        lv_chart_set_next_value(screen_layout[0].usage_chart, screen_layout[0].temp_series, cpu_temp);
        sprintf(buf, "#00FF00 CPU: %.1f%%#\n#FFFF00 TEMP: %.1f°C#", cpu_usage, cpu_temp);
        lv_label_set_text(screen_layout[0].usage_label, buf);

        // Screen 2: GPU update
        float gpu_usage = system_status.gpu_usage;
        float gpu_temp = system_status.gpu_temp;
        lv_chart_set_next_value(screen_layout[1].usage_chart, screen_layout[1].usage_series, gpu_usage);
        lv_chart_set_next_value(screen_layout[1].usage_chart, screen_layout[1].temp_series, gpu_temp);

        sprintf(buf, "#00FF00 GPU: %.1f%%#\n#FFFF00 TEMP: %.1f°C#", gpu_usage, gpu_temp);
        lv_label_set_text(screen_layout[1].usage_label, buf);
        lv_label_set_recolor(screen_layout[1].usage_label, true);

        // Screen 3: RAM
        lv_arc_set_value(screen_layout[2].usage_arc, system_status.ram_usage);
        sprintf(buf, "%.1f%%", system_status.ram_usage);
        lv_label_set_text(screen_layout[2].usage_label, buf);

        // Screen 4: HDD
        lv_bar_set_value(screen_layout[3].disk_c_bar, system_status.disk_c, LV_ANIM_OFF);
        sprintf(buf, "C: %.1f%%", system_status.disk_c);
        lv_label_set_text(screen_layout[3].disk_c_label, buf);
        
        if (system_status.disk_d >= 0) {
            lv_bar_set_value(screen_layout[3].disk_d_bar, system_status.disk_d, LV_ANIM_OFF);
            sprintf(buf, "D: %.1f%%", system_status.disk_d);
            lv_label_set_text(screen_layout[3].disk_d_label, buf);
        }

        // Screen 5: NETWORK
        sprintf(buf, UP_ARROW " %.2f MB/s", system_status.net_up);
        lv_label_set_text(screen_layout[4].net_up_label, buf);
        
        sprintf(buf, DOWN_ARROW " %.2f MB/s", system_status.net_down);
        lv_label_set_text(screen_layout[4].net_down_label, buf);

        system_status.has_monitor_data = false;
    }
}

/* Create multi-screen UI */
void create_multi_screen_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    if (!scr) {
        rt_kprintf("Failed to get the LVGL home screen!\n");
        return;
    }

    for (int i = 0; i < SCREEN_COUNT; i++) {
        screen_layout[i].img_obj = lv_img_create(scr);
        lv_obj_set_size(screen_layout[i].img_obj, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_set_pos(screen_layout[i].img_obj, screen_layout[i].x, screen_layout[i].y);
        lv_img_set_src(screen_layout[i].img_obj, screen_layout[i].src);

        lv_obj_set_style_border_width(screen_layout[i].img_obj, 1, 0);
        lv_obj_set_style_border_color(screen_layout[i].img_obj, lv_color_hex(0x333333), 0);

        create_screen_ui(i);
    }
    
    lv_timer_t *update_timer = lv_timer_create(update_screen_display, 500, NULL);

}