# GC9107 多个屏幕组合
## 支持的平台
<!-- 支持哪些板子和芯片平台 -->
+ sf32lb52-lcd_n16r8

## 概述
<!-- 例程简介 -->
这个是一个基于`GC9107`屏驱为基础的，多个`128*128`的`ZJY085-1212TBWIG42`屏幕组合成一个大的屏幕。可以实现每个屏幕显示内容不同，同时可以随意组合屏幕，比如六个屏幕，可以2x3,3x2,1x6等进行排列组合显示不同内容

## 硬件需求
运行该例程前，需要准备一块本例程支持的开发板
如下以六个屏幕为例，屏幕与开发板的连线和引脚说明<br>
所有屏幕相同引脚全部与开发板对应引脚相连(进行串行连接)：
|开发板引脚|屏幕引脚  |
|:---|:---|  
|GND      |GND|   
|VCC(3.3) |VCC|  
|PA04     |SCL|   
|PA05     |SDA|  
|PA00     |RST|  
|PA06     |DC |   
|VCC      |BL |  

各个屏幕的 CS 引脚分别与开发板对应GPIO引脚相连（可以自行添加或者减少屏幕）：  
|屏幕|LCD1 |LCD2 |LCD3 |LCD4 |LCD5 |LCD6 | ···  |    
|:---|:---|:---|:---|:---|:---|:---|:---| 
|CS与开发板引脚 |PA03 |PA25 |PA29 |PA37 |PA38 |PA24 |···  |  

## 软件介绍
### 初始化
* 对屏幕的CS引脚以及屏幕进行初始化，同时增加驱动能力
```c
static void LCD_Init(LCDC_HandleTypeDef *hlcdc)
{
    // 初始化CS引脚
    for (int i = 0; i < LCD_SCREEN_NUM; i++) {
        HAL_PIN_Set(lcd_cs_pad_gpio[i].pad, lcd_cs_pad_gpio[i].gpio, PIN_NOPULL, 1);
        rt_pin_mode(lcd_cs_pins[i], PIN_MODE_OUTPUT);
        rt_pin_write(lcd_cs_pins[i], PIN_HIGH);
    }

    // 设置引脚的驱动能力 PAD_PA04为SCL和PAD_PA06为DCX
    HAL_PIN_Set_DS0(PAD_PA04, 1, 1);
    HAL_PIN_Set_DS0(PAD_PA05, 1, 1);

    HAL_PIN_Set_DS1(PAD_PA04, 1, 1);
    HAL_PIN_Set_DS1(PAD_PA05, 1, 1);

    memcpy(&hlcdc->Init, &lcdc_int_cfg, sizeof(LCDC_InitTypeDef));
    HAL_LCDC_Init(hlcdc);

    //屏幕初始化
    LCD_Drv_Init(hlcdc);

}
```
### 读取屏幕ID
通过循环分别读取多个屏幕的ID，并打印在LOG中
```c
static uint32_t LCD_ReadID(LCDC_HandleTypeDef *hlcdc)
{
    uint32_t lcd_data[LCD_SCREEN_NUM];

    // 遍历六个屏幕读取ID
    for (int i = 0; i < LCD_SCREEN_NUM; i++) {
        current_lcd_cs_pin = lcd_cs_pins[i];
        lcd_data[i] = LCD_ReadData(hlcdc, REG_LCD_ID, 4);
        lcd_data[i] = ((lcd_data[i] << 1) >> 8) & 0xFFFFFF;
        rt_kprintf("\nLCD%d ReadID 0x%x \n", (i + 1), lcd_data[i]);        
    }
    return THE_LCD_ID;
}
```
### 设置屏幕显示区域
根据当前片选（CS）引脚，自动计算并设置当前屏幕的有效显示区域（ROI），并将全局坐标映射为该屏幕的局部坐标，最终配置寄存器，准备好后续的数据写入。
```c
static bool LCDC_SetROIArea(LCDC_HandleTypeDef *hlcdc)
{
    uint8_t parameter[4];
    uint8_t col = 0, row = 0;
    bool found = false;
    
    // 查找当前CS引脚对应的屏幕位置
    for (size_t i = 0; i < sizeof(screen_map)/sizeof(screen_map[0]); i++) {
        if (screen_map[i].cs_pin == current_lcd_cs_pin) {
            col = screen_map[i].col;
            row = screen_map[i].row;
            found = true;
            break;
        }
    }
    if (!found) {
        rt_kprintf("Error: Invalid CS pin %d\n", current_lcd_cs_pin);
        return RT_FALSE;
    }

    // 计算当前屏幕的物理边界
    const uint16_t screen_x0 = col * THE_LCD_PIXEL_WIDTH;
    const uint16_t screen_y0 = row * THE_LCD_PIXEL_HEIGHT;
    const uint16_t screen_x1 = screen_x0 + THE_LCD_PIXEL_WIDTH - 1;
    const uint16_t screen_y1 = screen_y0 + THE_LCD_PIXEL_HEIGHT - 1;

    // 检查ROI是否与当前屏幕有交集
    if (Region_Xpos0 > screen_x1 || Region_Xpos1 < screen_x0 ||
        Region_Ypos0 > screen_y1 || Region_Ypos1 < screen_y0) 
    {
        rt_kprintf("ROI [%d-%d, %d-%d] not on screen %d [%d-%d, %d-%d]\n",
                  Region_Xpos0, Region_Xpos1, Region_Ypos0, Region_Ypos1,
                  current_lcd_cs_pin, screen_x0, screen_x1, screen_y0, screen_y1);
        return RT_FALSE;
    }

    // 计算ROI在当前屏幕内的局部坐标（使用MAX/MIN处理边界）
    const uint16_t local_x0 = MAX(Region_Xpos0, screen_x0) - screen_x0;
    const uint16_t local_y0 = MAX(Region_Ypos0, screen_y0) - screen_y0;
    const uint16_t local_x1 = MIN(Region_Xpos1, screen_x1) - screen_x0;
    const uint16_t local_y1 = MIN(Region_Ypos1, screen_y1) - screen_y0;

    // 验证局部坐标有效性
    if (local_x0 >= THE_LCD_PIXEL_WIDTH || local_y0 >= THE_LCD_PIXEL_HEIGHT ||
        local_x1 >= THE_LCD_PIXEL_WIDTH || local_y1 >= THE_LCD_PIXEL_HEIGHT ||
        local_x0 > local_x1 || local_y0 > local_y1) 
    {
        rt_kprintf("Invalid local ROI on screen %d: X[%d-%d] Y[%d-%d]\n",
                  current_lcd_cs_pin, local_x0, local_x1, local_y0, local_y1);
        return RT_FALSE;
    }

    // 设置列地址 (CASET)
    parameter[0] = local_x0 >> 8;
    parameter[1] = local_x0 & 0xFF;
    parameter[2] = local_x1 >> 8;
    parameter[3] = local_x1 & 0xFF;
    LCD_WriteReg(hlcdc, REG_CASET, parameter, 4);

    // 设置行地址 (RASET)
    parameter[0] = local_y0 >> 8;
    parameter[1] = local_y0 & 0xFF;
    parameter[2] = local_y1 >> 8;
    parameter[3] = local_y1 & 0xFF;
    LCD_WriteReg(hlcdc, REG_RASET, parameter, 4);

    // 设置硬件ROI（使用全局坐标）
    HAL_LCDC_SetROIArea(hlcdc, 
        screen_x0 + local_x0, screen_y0 + local_y0,
        screen_x0 + local_x1, screen_y0 + local_y1
    );

    // rt_kprintf("Screen %d: Set ROI [%d,%d - %d,%d]\n",
    //           current_lcd_cs_pin,
    //           screen_x0 + local_x0, screen_y0 + local_y0,
    //           screen_x0 + local_x1, screen_y0 + local_y1);
              
    return RT_TRUE;
}

```
### 数据传输
这段代码是多屏幕拼接刷屏的核心流程，实现了多块LCD屏幕依次分区域刷新，并通过回调机制保证数据分屏传输的连续性和异步性。
```c
static void (* Ori_XferCpltCallback)(struct __LCDC_HandleTypeDef *lcdc);

static void HAL_GPIO_Set(uint16_t pin, int value)
{
    rt_pin_write(pin, (value != 0) ? PIN_HIGH : PIN_LOW);
}
static int current_screen_index = 0;
static void LCD_SendLayerDataCpltCbk(LCDC_HandleTypeDef *hlcdc)
{
    HAL_GPIO_Set(current_lcd_cs_pin, 1);
    current_screen_index++;
    while (current_screen_index < LCD_SCREEN_NUM)
    {
        current_lcd_cs_pin = lcd_cs_pins[current_screen_index];
        if (LCDC_SetROIArea(hlcdc))
        {

            HAL_GPIO_Set(current_lcd_cs_pin, 0);
            hlcdc->XferCpltCallback = LCD_SendLayerDataCpltCbk;
            HAL_LCDC_SendLayerData2Reg_IT(hlcdc, REG_WRITE_RAM, 1);
            return;
        }
        current_screen_index++;
    }
    // 全部完成，恢复原始回调
    current_screen_index = 0;
    hlcdc->XferCpltCallback = Ori_XferCpltCallback;
    if (Ori_XferCpltCallback)
        Ori_XferCpltCallback(hlcdc);
}

static void LCD_WriteMultiplePixels(LCDC_HandleTypeDef *hlcdc, const uint8_t *RGBCode, uint16_t Xpos0, uint16_t Ypos0, uint16_t Xpos1, uint16_t Ypos1)
{
    HAL_LCDC_LayerSetData(hlcdc, HAL_LCDC_LAYER_DEFAULT, (uint8_t *)RGBCode, Xpos0, Ypos0, Xpos1, Ypos1);
    Ori_XferCpltCallback = hlcdc->XferCpltCallback;
    current_screen_index = 0;
    while (current_screen_index < LCD_SCREEN_NUM)
    {
        current_lcd_cs_pin = lcd_cs_pins[current_screen_index];
        if (LCDC_SetROIArea(hlcdc))
        {
            HAL_GPIO_Set(current_lcd_cs_pin, 0);
            hlcdc->XferCpltCallback = LCD_SendLayerDataCpltCbk;
            HAL_LCDC_SendLayerData2Reg_IT(hlcdc, REG_WRITE_RAM, 1);
            return;
        }
        current_screen_index++;
    }
    // 全部失败
    rt_kprintf("ERROR: All LCDs failed to set ROI area\n");
    if (Ori_XferCpltCallback)
        Ori_XferCpltCallback(hlcdc);
}
```
### 写寄存器
这两个函数是多屏LCD驱动中用于写寄存器的底层操作，区别在于作用对象不同：
`LCD_WriteReg`只对当前选中的一块屏幕（由 current_lcd_cs_pin 指定）写寄存器。
`LCD_WriteReg_More`同时对所有屏幕写同一个寄存器（比如全体初始化、全体开关等）
```c
static void LCD_WriteReg(LCDC_HandleTypeDef *hlcdc, uint16_t LCD_Reg, uint8_t *Parameters, uint32_t NbParameters)
{
    HAL_GPIO_Set(current_lcd_cs_pin, 0);
    HAL_LCDC_WriteU8Reg(hlcdc, LCD_Reg, Parameters, NbParameters);
    HAL_GPIO_Set(current_lcd_cs_pin, 1);
}

static void LCD_WriteReg_More(LCDC_HandleTypeDef *hlcdc, uint16_t LCD_Reg, uint8_t *Parameters, uint32_t NbParameters)
{
    // 全部CS拉低
    for (int i = 0; i < LCD_SCREEN_NUM; i++) {
        HAL_GPIO_Set(lcd_cs_pins[i], 0);
    }
    HAL_LCDC_WriteU8Reg(hlcdc, LCD_Reg, Parameters, NbParameters);
    // 全部CS拉高
    for (int i = 0; i < LCD_SCREEN_NUM; i++) {
        HAL_GPIO_Set(lcd_cs_pins[i], 1);
    }
}
```
## 修改屏驱改 
* 如果想设置屏幕的排列方式，就需要在`\project\Kconfig.proj`进行配置屏幕模组液晶屏的分辨率和DPI。例如下代码屏幕为3x2排列的设置。<br>
```c
   config LCD_HOR_RES_MAX
        int
	    default 384 if LCD_USING_TFT_ZJY085_MULTI_SCREEN  <<<<<<前面的数字代表水平分辨率是384

    config LCD_VER_RES_MAX
        int
        default 256 if LCD_USING_TFT_ZJY085_MULTI_SCREEN   <<<<<<前面的数字代表垂直分辨率是256

config LCD_DPI
        int
        default 214 if LCD_USING_TFT_ZJY085_MULTI_SCREEN  <<<<<<前面的数字代表DPI值是214
```
* 如果想改变屏幕的排列方式，就只需要改变屏驱代码中的屏幕映射配置中的，屏幕摆列方式，修改第几行、第几列即可。以及设置屏幕的分辨率，如下代码演示：<br>
```c
// 屏幕映射配置
static const ScreenConfig screen_map[LCD_SCREEN_NUM] = {
    {LCD_CS_PIN_1, 0, 0},  // 第1块屏：第0行，第0列
    {LCD_CS_PIN_2, 1, 0},  // 第2块屏：第0行，第1列
    {LCD_CS_PIN_3, 2, 0},  // 第3块屏：第0行，第2列

    {LCD_CS_PIN_4, 0, 1},  // 第4块屏：第1行，第0列
    {LCD_CS_PIN_5, 1, 1},  // 第5块屏：第1行，第1列
    {LCD_CS_PIN_6, 2, 1},  // 第6块屏：第1行，第2列
};
```

* 如果想增加或减少屏幕数量，改变上面所介绍的分辨率和屏幕排列方式的同时，还需要增加或者减少CS引脚配置，以及修改屏幕数量，如下代码：<br>
```c
#define LCD_CS_PIN_1  03
#define LCD_CS_PIN_2  25
#define LCD_CS_PIN_3  29
#define LCD_CS_PIN_4  37 
#define LCD_CS_PIN_5  38
#define LCD_CS_PIN_6  24
// #define LCD_CS_PIN_7  27   //增加或者减少屏幕引脚配置

// 屏幕配置结构体
typedef struct {
    uint8_t cs_pin;
    uint8_t col;
    uint8_t row;
} ScreenConfig;

#define LCD_SCREEN_NUM 6  //修改屏幕数量
//增加或者减少CS引脚数组
static uint16_t lcd_cs_pins[LCD_SCREEN_NUM] = {LCD_CS_PIN_1, LCD_CS_PIN_2, LCD_CS_PIN_3, LCD_CS_PIN_4, LCD_CS_PIN_5, LCD_CS_PIN_6};

// 屏幕CS引脚对应的GPIO和PAD配置
static const struct {
    uint8_t pad;
    uint8_t gpio;
} lcd_cs_pad_gpio[LCD_SCREEN_NUM] = {
    {PAD_PA03, GPIO_A3},
    {PAD_PA25, GPIO_A25},
    {PAD_PA29, GPIO_A29},
    {PAD_PA37, GPIO_A37},
    {PAD_PA38, GPIO_A38},
    {PAD_PA24, GPIO_A24},
    // {PAD_PA27, GPIO_A27},//增加屏幕配置
};
```

**注意**<br>
1、屏幕与开发板连接线不易过长，连接线越长会影响频率，只能降低频率来正常显示   
2、屏幕的频率不宜过高如： `.freq = 48000000`,屏幕数量增加以及屏幕与开发板连接线过长都会导致屏幕花屏，如果出现这种状况就降低频率，比如:`.freq = 40000000`、`.freq = 24000000`等

## 预期结果
下图是正常演示的效果，可以看到六个屏幕同时显示，并且显示不同的内容<br>
![alt text](assets/result.jpg)<br>


## 异常诊断
```{warning}
1、如果出现花屏的状况，首先通过降低频率，之后观察演示效果
2、如果显示不正常，首先观察连接的线是否正确，检查排线特别是各个屏幕的CS引脚连线，观察LOG是否有初始化信息
```

## 参考文档
<!-- -->

## 更新记录
|版本 |日期   |发布说明 |
|:---|:---|:---|
|0.0.1 |6/2025 |初始版本 |
| | | |
| | | |


