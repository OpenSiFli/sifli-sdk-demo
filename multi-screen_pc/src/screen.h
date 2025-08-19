/**
 * @file lv_example_scroll.h
 *
 */

#ifndef SCREEN_H
#define SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif
#include "lvgl/lvgl.h" 
#include <rtthread.h>
/* System status data structure */
typedef struct {
    float cpu_usage;    
    float cpu_temp;
    
    float gpu_usage;
    float gpu_temp;
    
    float ram_usage;
    
    float disk_c;
    float disk_d;          
    
    float net_up;           
    float net_down;         
    
    char datetime[50];    
    bool has_new_data;  

    bool has_monitor_data;
    bool has_time_data;
} system_status_t;

/* Screen layout structure */
typedef struct {
    int16_t x;              
    int16_t y;             
    const void *src;       
    lv_obj_t *img_obj;     
    lv_obj_t *title_label;
    
    // Circular progress bar
    lv_obj_t *usage_arc;   
    lv_obj_t *usage_label;

    // Disk progress bar
    lv_obj_t *disk_c_bar;
    lv_obj_t *disk_c_label;
    lv_obj_t *disk_d_bar;   
    lv_obj_t *disk_d_label;

    // Network labels
    lv_obj_t *net_up_label; 
    lv_obj_t *net_down_label;
    
    // Date and time label
    lv_obj_t *date_label;
    lv_obj_t *time_label;

    // Chart related (usage + temperature)
    lv_obj_t *usage_chart;
    lv_chart_series_t *usage_series;
    lv_chart_series_t *temp_series;
} ScreenLayout;

extern system_status_t system_status;

void lv_init(void);
void sys_set_cmd(int argc, char **argv); 
void create_multi_screen_ui(void);



/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_EXAMPLE_SCROLL_H*/
