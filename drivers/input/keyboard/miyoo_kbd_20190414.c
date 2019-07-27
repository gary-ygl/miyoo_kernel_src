/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option)any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/io.h>
#include <asm/arch-suniv/cpu.h>
#include <asm/arch-suniv/gpio.h>
#include <linux/uaccess.h>

//#define DEBUG
#define MIYOO_KBD_GET_HOTKEY  _IOWR(0x100, 0, unsigned long)
#define MIYOO_KBD_SET_VER     _IOWR(0x101, 0, unsigned long)
#define MIYOO_KBD_LOCK_KEY    _IOWR(0x102, 0, unsigned long)
    
#define MY_UP     0x0008
#define MY_DOWN   0x0800
#define MY_LEFT   0x0080
#define MY_RIGHT  0x0040
#define MY_A      0x0100
#define MY_B      0x0020
#define MY_TA     0x0010
#define MY_TB     0x0002
#define MY_SELECT 0x0400
#define MY_START  0x0200
#define MY_R      0x0004
#define MY_L1     0x1000
#define MY_R1     0x2000
#define MY_L2     0x4000
#define MY_R2     0x8000

#define IN_L1   ((32 * 2) + 1)
#define IN_R1   ((32 * 2) + 2)
#define IN_L2   ((32 * 4) + 0)
#define IN_R2   ((32 * 2) + 3)
#define OUT_1   ((32 * 4) + 7)
#define OUT_2   ((32 * 4) + 8)
#define OUT_3   ((32 * 4) + 9)
#define IN_1    ((32 * 4) + 2)
#define IN_2    ((32 * 4) + 3)
#define IN_3    ((32 * 4) + 4)
#define IN_4    ((32 * 4) + 5)
#define IN_A    ((32 * 3) + 0)
#define IN_TA   ((32 * 3) + 9)
#define IN_B    ((32 * 0) + 3)
#define IN_TB   ((32 * 2) + 0)
#define IN_MENU ((32 * 4) + 1)

#define USE_UART	1

static int major = -1;
static struct cdev mycdev;
static struct class *myclass = NULL;
static struct input_dev *mydev;
static struct timer_list mytimer;
static int myperiod=30;

static unsigned long miyoo_ver=1;
static unsigned long hotkey=0;
static unsigned long lockkey=0;
static uint8_t *gpio;
 
static int do_input_request(uint32_t pin, const char*name)
{
  if(gpio_request(pin, name) < 0){
    printk("failed to request gpio: %s\n", name);
    return -1;
  }
  gpio_direction_input(pin);
  return 0;
}

static int do_output_request(uint32_t pin, const char* name)
{
  if(gpio_request(pin, name) < 0){
    printk("failed to request gpio: %s\n", name);
    return -1;
  }
  gpio_direction_output(pin, 1);
  return 0;
}

#if defined(DEBUG)
static void print_key(uint32_t val, uint8_t is_pressed)
{
  uint32_t i;
  uint32_t map_val[] = {MY_UP, MY_DOWN, MY_LEFT, MY_RIGHT, MY_A, MY_B, MY_TA, MY_TB, MY_SELECT, MY_START, MY_R, MY_L1, MY_R1, MY_L2, MY_R2, -1};
  char* map_key[] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "SELECT", "START", "MENU", "L1", "R1", "L2", "R2"};

  for(i=0; map_val[i]!=-1; i++){
    if(map_val[i] == val){
      if(is_pressed){
        printk("%s\n", map_key[i]);
      } 
      break;
    }
  }
}
#endif

static void report_key(uint32_t btn, uint32_t mask, uint8_t key)
{
  static uint32_t btn_pressed=0;
  static uint32_t btn_released=0xffff;
 
  if(btn & mask){
    btn_released&= ~mask;
    if((btn_pressed & mask) == 0){
      btn_pressed|= mask;
      input_report_key(mydev, key, 1);
      #if defined(DEBUG)
        print_key(btn & mask, 1);
      #endif
    }
  }
  else{
    btn_pressed&= ~mask;
    if((btn_released & mask) == 0){
      btn_released|= mask;
      input_report_key(mydev, key, 0);
      #if defined(DEBUG)
        print_key(btn & mask, 0);
      #endif
    }
  }
}

static int bit_count(uint32_t val)
{
  int ret=0, x;

  for(x=0; x<32; x++){
    if(val & 1){
      ret+= 1;
    }
    val>>= 1;
  }
  return ret;
}

static void scan_handler(unsigned long unused)
{
  static uint32_t pre=0;
  uint32_t scan=0, val=0;

  if(miyoo_ver <= 2){
    for(scan=0; scan<3; scan++){
      gpio_set_value(OUT_1, 1);
      gpio_set_value(OUT_2, 1);
      gpio_set_value(OUT_3, 1);
      gpio_direction_input(OUT_1);
      gpio_direction_input(OUT_2);
      gpio_direction_input(OUT_3);
      switch(scan){
      case 0:
        gpio_direction_output(OUT_1, 0);
        break;
      case 1:
        gpio_direction_output(OUT_2, 0);
        break;
      case 2:
        gpio_direction_output(OUT_3, 0);
        break;
      }
      if (gpio_get_value(IN_1) == 0){
        val|= ((1 << 0) << (scan << 2));
      }
      if (gpio_get_value(IN_2) == 0){
        val|= ((1 << 1) << (scan << 2));
      }
      if (gpio_get_value(IN_3) == 0){
        val|= ((1 << 2) << (scan << 2));
      }
      if (gpio_get_value(IN_4) == 0){
        val|= ((1 << 3) << (scan << 2));
      }
    }
    if (gpio_get_value(IN_L1) == 0){
      val|= MY_L1;
    }
    if (gpio_get_value(IN_R1) == 0){
      val|= MY_R1;
    }
    if (gpio_get_value(IN_L2) == 0){
      //val|= MY_L2;
      val|= MY_B;
    }
    if (gpio_get_value(IN_R2) == 0){
      //val|= MY_R2;
      val|= MY_TB;
    }
    if (gpio_get_value(IN_A) == 0){
      val|= MY_A;
    }
    if (gpio_get_value(IN_TA) == 0){
      val|= MY_TA;
    }
  #if !defined(USE_UART)
    if (gpio_get_value(IN_B) == 0){
      val|= MY_B;
    }
    if (gpio_get_value(IN_TB) == 0){
      val|= MY_TB;
    }
  #endif
  }
  else{
    gpio_direction_input(IN_1);
    gpio_direction_input(IN_2);
    gpio_direction_input(IN_3);
    gpio_direction_input(IN_4);
    gpio_direction_input(OUT_1);
    gpio_direction_input(OUT_2);
    gpio_direction_input(OUT_3);
    gpio_direction_input(IN_A);
    gpio_direction_input(IN_TA);
    gpio_direction_input(IN_L2);
    gpio_direction_input(IN_L1);
    gpio_direction_input(IN_R1);
    gpio_direction_input(IN_MENU);

    if(gpio_get_value(IN_1) == 0){
      val|= MY_UP;
    }
    if(gpio_get_value(IN_2) == 0){
      val|= MY_DOWN;
    }
    if(gpio_get_value(IN_3) == 0){
      val|= MY_LEFT;
    }
    if(gpio_get_value(IN_4) == 0){
      val|= MY_RIGHT;
    }
    if(gpio_get_value(OUT_1) == 0){
      val|= MY_A;
    }
    if(gpio_get_value(OUT_2) == 0){
      val|= MY_B;
    }
    if(gpio_get_value(OUT_3) == 0){
      val|= MY_TA;
    }
    if(gpio_get_value(IN_TA) == 0){
      val|= MY_TB;
    }
    if(gpio_get_value(IN_A) == 0){
      val|= MY_SELECT;
    }
    if(gpio_get_value(IN_L2) == 0){
      val|= MY_START;
    }
    if(gpio_get_value(IN_L1) == 0){
      val|= MY_L1;
    }
    if(gpio_get_value(IN_R1) == 0){
      val|= MY_R1;
    }
    if(gpio_get_value(IN_MENU) == 0){
      val|= MY_R;
    }
  }

  if(lockkey){
    val = val & MY_R ? MY_R : 0;
  }

  // filter noise
  if(bit_count(val) > 2){
    if((val & MY_LEFT) && (val & MY_DOWN) && (val & MY_B)){
      val&= ~MY_START;
    }
	  if((val & MY_LEFT) && (val & MY_A) && (val & MY_TA)){
      val&= ~MY_DOWN;
    }
	  if((val & MY_LEFT) && (val & MY_DOWN) && (val & MY_A)){
      val&= ~MY_TA;
    }
	  if((val & MY_LEFT) && (val & MY_UP) && (val & MY_B)){
      val&= ~MY_TA;
    }
	  if((val & MY_B) && (val & MY_A) && (val & MY_TA)){
      val&= ~MY_START;
    }
	  if((val & MY_RIGHT) && (val & MY_UP) && (val & MY_B) && (val & MY_TB) && (val & MY_TA)){
      val&= ~MY_START;
    }
	  if((val & MY_LEFT) && (val & MY_DOWN) && (val & MY_B)){
      val&= ~MY_START;
    }
  }

  if((val & MY_SELECT) && (val & MY_B)){
    hotkey = hotkey == 0 ? 3 : hotkey;
    val&= ~MY_B;
  #if defined(DEBUG)
    printk("%s, volume++\n", __func__);
  #endif
  }
  else if((val & MY_SELECT) && (val & MY_A)){
    hotkey = hotkey == 0 ? 4 : hotkey;
    val&= ~MY_A;
  #if defined(DEBUG)
    printk("%s, volume--\n", __func__);
  #endif
  }
  else if((val & MY_SELECT) && (val & MY_TB)){
    hotkey = hotkey == 0 ? 1 : hotkey;
    val&= ~MY_TB;
  #if defined(DEBUG)
    printk("%s, backlight++\n", __func__);
  #endif
  }
  else if((val & MY_SELECT) && (val & MY_TA)){
    hotkey = hotkey == 0 ? 2 : hotkey;
    val&= ~MY_TA;
  #if defined(DEBUG)
    printk("%s, backlight--\n", __func__);
  #endif
  }
    
  if((val & MY_START) && (val & MY_B)){
    val&= ~MY_B;
    val&= ~MY_START;
    val|= MY_L1; // L1
  }
  else if((val & MY_START) && (val & MY_A)){
    val&= ~MY_A;
    val&= ~MY_START;
    val|= MY_R1; // R1
  }
  else if((val & MY_START) && (val & MY_TB)){
    val&= ~MY_TB;
    val&= ~MY_START;
    val|= MY_L2; // L2
  }
  else if((val & MY_START) && (val & MY_TA)){
    val&= ~MY_TA;
    val&= ~MY_START;
    val|= MY_R2; // R2
  } 

  if(pre != val){
    pre = val;
    report_key(pre, MY_UP, KEY_UP);
    report_key(pre, MY_DOWN, KEY_DOWN);
    if((miyoo_ver == 1) || (miyoo_ver == 3)){
      report_key(pre, MY_LEFT, KEY_LEFT);
      report_key(pre, MY_R, KEY_RIGHTCTRL);
    }
    else{
      report_key(pre, MY_R, KEY_LEFT);
      report_key(pre, MY_LEFT, KEY_RIGHTCTRL);
    }
    report_key(pre, MY_RIGHT, KEY_RIGHT);

    report_key(pre, MY_A, KEY_LEFTCTRL);
    report_key(pre, MY_B, KEY_SPACE);
    report_key(pre, MY_TA, KEY_LEFTALT);
    report_key(pre, MY_TB, KEY_LEFTSHIFT);
    
    report_key(pre, MY_SELECT, KEY_ESC);
    report_key(pre, MY_START, KEY_ENTER);

    report_key(pre, MY_L1, KEY_TAB);
    report_key(pre, MY_R1, KEY_BACKSPACE);
    report_key(pre, MY_L2, KEY_RIGHTALT);
    report_key(pre, MY_R2, KEY_RIGHTSHIFT);
    input_sync(mydev);
  }
  mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
}

static int myopen(struct inode *inode, struct file *file)
{
  return 0;
}

static int myclose(struct inode *inode, struct file *file)
{
  return 0;
}

static long myioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
  int ret;

  switch(cmd){
  case MIYOO_KBD_GET_HOTKEY:
    ret = copy_to_user((void*)arg, &hotkey, sizeof(unsigned long));
    hotkey = 0;
    break;
  case MIYOO_KBD_SET_VER:
    miyoo_ver = arg;
    if(miyoo_ver <= 0){
      miyoo_ver = 1;
    }
    printk("miyoo keypad config as v%d\n", (int)miyoo_ver);
    break;
  case MIYOO_KBD_LOCK_KEY:
    lockkey = arg;
    break;
  }
  return 0;
}

static const struct file_operations myfops = {
  .owner = THIS_MODULE,
  .open = myopen,
  .release = myclose,
  .unlocked_ioctl = myioctl,
};

static int __init kbd_init(void)
{
  uint32_t ret;

  gpio = ioremap(0x01c20800, 4096);
  ret = readl(gpio + (2 * 0x24 + 0x00));
  ret&= 0xffff0000;
  writel(ret, gpio + (2 * 0x24 + 0x00));

  ret = readl(gpio + (2 * 0x24 + 0x1c));
  //ret&= 0xffffff00;
  //ret|= 0x00000055;
  ret = 0x55555555;
  writel(ret, gpio + (2 * 0x24 + 0x1c));

  ret = readl(gpio + (4 * 0x24 + 0x00));
  ret&= 0xfffffff0;
  writel(ret, gpio + (4 * 0x24 + 0x00));

  ret = readl(gpio + (4 * 0x24 + 0x1c));
  //ret&= 0xffffffff0;
  //ret|= 0x000000001;
  ret = 0x55555555;
  writel(ret, gpio + (4 * 0x24 + 0x1c));

#if !defined(USE_UART)
  ret = readl(gpio + (0 * 0x24 + 0x00));
  ret&= 0xffff0fff;
  writel(ret, gpio + (0 * 0x24 + 0x00));  
#endif

  do_input_request(IN_L1, 	"gpio_l1");
  do_input_request(IN_R1, 	"gpio_r1");
  do_input_request(IN_L2, 	"gpio_l2");
  do_input_request(IN_R2, 	"gpio_r2");
  do_input_request(IN_1, 		"gpio_pe2");
  do_input_request(IN_2, 		"gpio_pe3");
  do_input_request(IN_3, 		"gpio_pe4");
  do_input_request(IN_4, 		"gpio_pe5");
  do_input_request(IN_A, 		"gpio_a");
  do_input_request(IN_TA, 	"gpio_ta");
#if !defined(USE_UART)
  do_input_request(IN_B, 		"gpio_b");
  do_input_request(IN_TB, 	"gpio_tb");
#endif
  do_output_request(OUT_1, 	"gpio_pe7");
  do_output_request(OUT_2, 	"gpio_pe8");
  do_output_request(OUT_3, 	"gpio_pe9");
  
  mydev = input_allocate_device();
  set_bit(EV_KEY,mydev-> evbit);
  set_bit(KEY_UP, mydev->keybit);
  set_bit(KEY_DOWN, mydev->keybit);
  set_bit(KEY_LEFT, mydev->keybit);
  set_bit(KEY_RIGHT, mydev->keybit);
  set_bit(KEY_ENTER, mydev->keybit);
  set_bit(KEY_ESC, mydev->keybit);
  set_bit(KEY_LEFTCTRL, mydev->keybit);
  set_bit(KEY_LEFTALT, mydev->keybit);
  set_bit(KEY_SPACE, mydev->keybit);
  set_bit(KEY_LEFTSHIFT, mydev->keybit);
  set_bit(KEY_TAB, mydev->keybit);
  set_bit(KEY_BACKSPACE, mydev->keybit);
  set_bit(KEY_RIGHTCTRL, mydev->keybit);
  set_bit(KEY_RIGHTALT, mydev->keybit);
  set_bit(KEY_RIGHTSHIFT, mydev->keybit);
  mydev->name = "miyoo_keypad";
  mydev->id.bustype = BUS_HOST;
  ret = input_register_device(mydev);
 
  alloc_chrdev_region(&major, 0, 1, "miyoo_kbd");
  myclass = class_create(THIS_MODULE, "miyoo_kbd");
  device_create(myclass, NULL, major, NULL, "miyoo_kbd");
  cdev_init(&mycdev, &myfops);
  cdev_add(&mycdev, major, 1);
  
	setup_timer(&mytimer, scan_handler, 0);
  mod_timer(&mytimer, jiffies + msecs_to_jiffies(myperiod));
  return 0;
}
  
static void __exit kbd_exit(void)
{
  input_unregister_device(mydev);
  del_timer(&mytimer);

  device_destroy(myclass, major);
  cdev_del(&mycdev);
  class_destroy(myclass);
  unregister_chrdev_region(major, 1);
  iounmap(gpio);
}
  
module_init(kbd_init);
module_exit(kbd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Steward Fu <steward.fu@gmail.com>");
MODULE_DESCRIPTION("Keyboard Driver for Miyoo handheld");
 
