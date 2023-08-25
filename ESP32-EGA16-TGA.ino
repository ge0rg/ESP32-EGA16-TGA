/*********
  ESP32-EGA16-TGA shitty camera project
  Georg Lukas, https://github.com/ge0rg/ESP32-EGA16-TGA

  Partially based on the following project:
  || Rui Santos
  || Complete project details at https://RandomNerdTutorials.com/esp32-cam-take-photo-save-microsd-card
  || 
  || Permission is hereby granted, free of charge, to any person obtaining a copy
  || of this software and associated documentation files.
  || The above copyright notice and this permission notice shall be included in all
  || copies or substantial portions of the Software.

  Includes Bayer pattern code from https://stackoverflow.com/a/68210469/539443
*********/

#include "esp_camera.h"
#include "Arduino.h"
#include "FS.h"                // SD Card ESP32
#include "SD_MMC.h"            // SD Card ESP32
#include "soc/soc.h"           // Disable brownour problems
#include "soc/rtc_cntl_reg.h"  // Disable brownour problems
#include "driver/rtc_io.h"
#include <EEPROM.h>            // read and write from flash memory

#define EEPROM_SIZE 1		/* we need one byte for the photo number */

#define WARMUP_PICS 10		/* number of warm-up shots before taking the actual image */
#define SQUARE_ERROR 1		/* find palette color with smallest mean square error instead of linear error */

#define STORE_RAW 0		/* store original image as RGB565 files */
#define STORE_RGB 0		/* store image output as RGB888 files */

#define STORE_TGA 1		/* store image output as TGA files */
#define FORCE_8BPP_TGA 1	/* ImageMagick requires at least 8bpp for TGA, not our optimal 4bpp */

#define STORE_CHINA_EXPORT 1	/* this will switch to monochome mode and override most settings */
#define STORE_DITHER_NONE 0
#define STORE_DITHER_AVERAGE 0
#define STORE_DITHER_ERROR 0
#define STORE_DITHER_ORDERED 0
#define STORE_DITHER_ORDERED_SHIFTED 1

#define CE_WIDTH 128
#define CE_HEIGHT 112
#define CE_COLORS 4

#define WIDTH 320
#define HEIGHT 200
#define SENSOR_HEIGHT 240

#define COLORS 16

#if FORCE_8BPP_TGA
#define STORE_COLORS 256
#else
#define STORE_COLORS COLORS
#endif

// Pin definition for CAMERA_MODEL_AI_THINKER
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#undef USE_BUTTON
#define BUTTON_GPIO 0

void printAndSleep(const char* error) {
  pinMode(4, OUTPUT);
  if (error != NULL) {
    Serial.println(error);
    int i;
    for (i = 1; i <= 5; i++) {
      digitalWrite(4, i % 2);
      delay(250);
    }
  }
  digitalWrite(4, LOW);
  rtc_gpio_hold_en(GPIO_NUM_4);
  Serial.println("Going to sleep now");
  delay(100);
  esp_deep_sleep_start();
  Serial.println("This will never be printed");
  while (1) {}
}


int pictureNumber = 0;

enum DitherMode {
	DITHER_NONE, /* store all colors */
	DITHER_AVERAGE, /* convert to closest color */
	DITHER_ERROR_PROP, /* reduced Floyd-Steinberg style error propagation */
	DITHER_ORDERED, /* Bayer pattern */
	DITHER_ORDERED_SHIFTED /* Bayer pattern shifted per color channel */
};

byte EGA_PALETTE[COLORS][3] = {
	/* 0	black	*/	{ 0x00, 0x00, 0x00 },
	/* 1	blue	*/	{ 0x00, 0x00, 0xAA },
	/* 2	green	*/	{ 0x00, 0xAA, 0x00 },
	/* 3	cyan	*/	{ 0x00, 0xAA, 0xAA },
	/* 4	red	*/	{ 0xAA, 0x00, 0x00 },
	/* 5	magenta	*/	{ 0xAA, 0x00, 0xAA },
	/* 6	yellow / brown	*/	{ 0xAA, 0x55, 0x00 },
	/* 7	white / light gray	*/	{ 0xAA, 0xAA, 0xAA },
	/* 8	dark gray / bright black	*/	{ 0x55, 0x55, 0x55 },
	/* 9	bright blue	*/	{ 0x55, 0x55, 0xFF },
	/* 10	bright green	*/	{ 0x55, 0xFF, 0x55 },
	/* 11	bright cyan	*/	{ 0x55, 0xFF, 0xFF },
	/* 12	bright red	*/	{ 0xFF, 0x55, 0x55 },
	/* 13	bright magenta	*/	{ 0xFF, 0x55, 0xFF },
	/* 14	bright yellow	*/	{ 0xFF, 0xFF, 0x55 },
	/* 15	bright white	*/	{ 0xFF, 0xFF, 0xFF }
};

#if STORE_CHINA_EXPORT
#include <ce_data.h>
#endif

const   int BAYER_PATTERN_16X16[16][16] =   {   //  16x16 Bayer Dithering Matrix.  Color levels: 256
		{     0, 191,  48, 239,  12, 203,  60, 251,   3, 194,  51, 242,  15, 206,  63, 254  }, 
		{   127,  64, 175, 112, 139,  76, 187, 124, 130,  67, 178, 115, 142,  79, 190, 127  },
		{    32, 223,  16, 207,  44, 235,  28, 219,  35, 226,  19, 210,  47, 238,  31, 222  },
		{   159,  96, 143,  80, 171, 108, 155,  92, 162,  99, 146,  83, 174, 111, 158,  95  },
		{     8, 199,  56, 247,   4, 195,  52, 243,  11, 202,  59, 250,   7, 198,  55, 246  },
		{   135,  72, 183, 120, 131,  68, 179, 116, 138,  75, 186, 123, 134,  71, 182, 119  },
		{    40, 231,  24, 215,  36, 227,  20, 211,  43, 234,  27, 218,  39, 230,  23, 214  },
		{   167, 104, 151,  88, 163, 100, 147,  84, 170, 107, 154,  91, 166, 103, 150,  87  },
		{     2, 193,  50, 241,  14, 205,  62, 253,   1, 192,  49, 240,  13, 204,  61, 252  },
		{   129,  66, 177, 114, 141,  78, 189, 126, 128,  65, 176, 113, 140,  77, 188, 125  },
		{    34, 225,  18, 209,  46, 237,  30, 221,  33, 224,  17, 208,  45, 236,  29, 220  },
		{   161,  98, 145,  82, 173, 110, 157,  94, 160,  97, 144,  81, 172, 109, 156,  93  },
		{    10, 201,  58, 249,   6, 197,  54, 245,   9, 200,  57, 248,   5, 196,  53, 244  },
		{   137,  74, 185, 122, 133,  70, 181, 118, 136,  73, 184, 121, 132,  69, 180, 117  },
		{    42, 233,  26, 217,  38, 229,  22, 213,  41, 232,  25, 216,  37, 228,  21, 212  },
		{   169, 106, 153,  90, 165, 102, 149,  86, 168, 105, 152,  89, 164, 101, 148,  85  }
	};

typedef struct {
   char  idlength;
   char  colourmaptype;
   char  datatypecode;
   short int colourmaporigin;
   short int colourmaplength;
   char  colourmapdepth;
   short int x_origin;
   short int y_origin;
   short width;
   short height;
   char  bitsperpixel;
   char  imagedescriptor;
} __attribute__((packed)) TgaHeader;

#define TGA_OFFSET (sizeof(TgaHeader) + 3*STORE_COLORS)
#if FORCE_8BPP_TGA
#define TGA_SIZE (TGA_OFFSET + WIDTH*HEIGHT)
#else
#define TGA_SIZE (TGA_OFFSET + WIDTH*HEIGHT/2)
#endif

#define RGB_SIZE (3*WIDTH*HEIGHT)


TgaHeader header = {
	0, 1, 1,	// empty ID field, color mapped (types  = 1)
	0, STORE_COLORS, 24, // palette definition: 16 (or 256) colors, 24 bits each
	0, 0, WIDTH, HEIGHT, // resolution
#if FORCE_8BPP_TGA
	8,
#else
	4,
#endif
	0
};

int findClosestColor(int r, int g, int b) {
	int best_match = INT_MAX;
	int best_color = 0;
	for (int i = 0; i < COLORS; i++) {
		int delta_r = abs(EGA_PALETTE[i][0]-r);
		int delta_g = abs(EGA_PALETTE[i][1]-g);
		int delta_b = abs(EGA_PALETTE[i][2]-b);
#if SQUARE_ERROR
		int match = delta_r*delta_r + delta_g*delta_g + delta_b*delta_b;
#else
		int match = delta_r + delta_g + delta_b;
#endif
		if (match < best_match) {
			best_match = match;
			best_color = i;
		}
	}
	return best_color;
}


int storeBufferToSD(int pictureNumber, const char *ext, byte* data, int len) {
  // Path where new picture will be saved in SD Card
  char path[64];
  sprintf(path, "/shitty_%03d.%s", pictureNumber, ext);

  fs::FS &fs = SD_MMC; 
  Serial.printf("Picture file name: %s (buf=%p)\r\n", path, data);
  
  File file = fs.open(path, FILE_WRITE);
  if(!file){
    printAndSleep("Failed to open file in writing mode");
    return -1;
  }
  else {
    //file.write(fb->buf, fb->len); // payload (image), payload length
    file.write(data, len); // payload (image), payload length
    Serial.printf("Saved file to path: %s (%d)\r\n", path, len);
  }
  file.close();
  return 0;
}
int storeBuffers(int pictureNumber, const char *ext, byte *rgb, byte *tga) {
  char ext_ext[16];
  int ret = 0;
#if STORE_TGA
  sprintf(ext_ext, "%s.tga", ext);
  ret |= storeBufferToSD(pictureNumber, ext_ext, tga, TGA_SIZE);
#endif
#if STORE_RGB
  sprintf(ext_ext, "%s.rgb", ext);
  ret |= storeBufferToSD(pictureNumber, ext_ext, rgb, RGB_SIZE);
#endif
  return ret;
}

#if STORE_CHINA_EXPORT
void ditherRGB565toChinaExport(uint8_t* src, uint8_t *dst) {
  for (int y = 0; y < CE_HEIGHT; y++) {
    for (int x = 0; x < CE_WIDTH; x++) {
      int scale_y = y*SENSOR_HEIGHT / CE_HEIGHT; /* CE image height is 47% of sensor image */
      int scale_x = 20 + x*280 / CE_WIDTH; /* CE image width corresponds to 47% of 280px */
      int i = scale_y*WIDTH + scale_x;
      int r = (int)(src[i*2] >> 3) * 255 / 31;
      int g = (int)(((src[i*2] & 0x7) << 3) | ((src[i*2 + 1] >> 5) & 0x07)) * 255 / 63;
      int b = (int)(src[i*2 + 1] & 0x1f) * 255 / 31;
      int mono = (r + b + 2*g)/4;
      int t = BAYER_PATTERN_16X16[x % 16][y % 16] / 2;
      mono = max(0, min(mono + t - 64, 255));
      int offset = CE_DATA_OFFSET + (16 + CE_HEIGHT - y)*CE_DATA_WIDTH + (16 + x);
      dst[offset] = (mono / 64) * 85;
    }
  }
}
#endif //STORE_CHINA_EXPORT

void ditherRGB565toRGB(uint8_t* src, uint8_t* rgb, uint8_t *packed, int width, int height, int dither_mode) {
  int err_r = 0, err_g = 0, err_b = 0;
  int i = 0, o = 0; /* buffer positions for input and output */
  uint8_t packed_pixel = 0;
  Serial.printf("Dithering %d: src=%p rgb=%p packed=%p w=%d h=%d\r\n",
    dither_mode, src, rgb, packed, width, height);
  for (int y = 0; y < height; y++) {
#if HEIGHT != SENSOR_HEIGHT
    /* skip one line every five lines to get from 240 to 200 */
    if (y  % 5 == 4) {
        //y += 1;
        i += width;
    }
#endif
    err_r = err_g = err_b = 0;
    for (int x = 0; x < width; x++) {
      /* convert RGB565 to RGB888 in individual int variables */
      int r = (int)(src[i*2] >> 3) * 255 / 31;
      int g = (int)(((src[i*2] & 0x7) << 3) | ((src[i*2 + 1] >> 5) & 0x07)) * 255 / 63;
      int b = (int)(src[i*2 + 1] & 0x1f) * 255 / 31;
      int match, t;
      /* preparation  step: get the target color into r, g, b */
      switch (dither_mode) {
      case DITHER_NONE:
	rgb[o*3 + 0] = r;
	rgb[o*3 + 1] = g;
	rgb[o*3 + 2] = b;
	/* abort this pixel's loop cycle, we are done */
	continue;
      case DITHER_AVERAGE:
	/* no special prep needed */
	break;
      case DITHER_ERROR_PROP:
	r = r + err_r;
	g = g + err_g;
	b = b + err_b;
	break;
      case DITHER_ORDERED:
	t = BAYER_PATTERN_16X16[x % 16][y % 16] / 2;
	r = max(0, min(r + t - 64, 255));
	t = BAYER_PATTERN_16X16[x % 16][y % 16] / 2;
	g = max(0, min(g + t - 64, 255));
	t = BAYER_PATTERN_16X16[x % 16][y % 16] / 2;
	b = max(0, min(b + t - 64, 255));
	break;
      case DITHER_ORDERED_SHIFTED:
	t = BAYER_PATTERN_16X16[x % 16][y % 16] / 2;
	r = max(0, min(r + t - 64, 255));
	t = BAYER_PATTERN_16X16[(x+1) % 16][y % 16] / 2;
	g = max(0, min(g + t - 64, 255));
	t = BAYER_PATTERN_16X16[x % 16][(y+1) % 16] / 2;
	b = max(0, min(b + t - 64, 255));
	break;
      }
      match = findClosestColor(r, g, b);
      err_r = r - EGA_PALETTE[match][0];
      err_g = g - EGA_PALETTE[match][1];
      err_b = b - EGA_PALETTE[match][2];

      rgb[o*3 + 0] = EGA_PALETTE[match][0];
      rgb[o*3 + 1] = EGA_PALETTE[match][1];
      rgb[o*3 + 2] = EGA_PALETTE[match][2];


#if STORE_TGA
#if FORCE_8BPP_TGA
      packed[(HEIGHT - y - 1)*WIDTH + x] = match;
#else
      packed_pixel = (packed_pixel << 4) | match;
      if (x % 2 == 1) {
	packed[((HEIGHT - y - 1)*WIDTH + x)/2] = packed_pixel;
	packed_pixel = 0;
      }
#endif // FORCE_8BPP_TGA
#endif // STORE_TGA
      i++;
      o++;
    }
  }
}

void takePictureToSD() {
    
  camera_fb_t * fb = NULL;
  
#ifndef USE_BUTTON
  // let the camera warm up
  for (int i= 0 ; i < WARMUP_PICS; i++) {
    Serial.printf("Warmup picture %d/%d...\r", i, WARMUP_PICS);
    fb = esp_camera_fb_get();
    if (fb)
      esp_camera_fb_return(fb);
    else
      Serial.println("Camera capture failed");
  }
#endif
  
  // Take Picture with Camera
  fb = esp_camera_fb_get();  
  if(!fb) {
    printAndSleep("Camera capture failed");
    return;
  }
  // initialize EEPROM with predefined size
  EEPROM.begin(EEPROM_SIZE);
  pictureNumber = EEPROM.read(0) + 1;


  byte *rgb = (byte*)ps_malloc(RGB_SIZE);
#if STORE_TGA
  byte *tga = (byte*)ps_malloc(TGA_SIZE);
  memcpy(tga, &header, sizeof(TgaHeader));
  for (int i = 0; i < STORE_COLORS; i++) {
    tga[sizeof(TgaHeader) + i*3 + 0] = EGA_PALETTE[i % COLORS][2];
    tga[sizeof(TgaHeader) + i*3 + 1] = EGA_PALETTE[i % COLORS][1];
    tga[sizeof(TgaHeader) + i*3 + 2] = EGA_PALETTE[i % COLORS][0];
  }

  byte *packed = tga + TGA_OFFSET;
#else
  byte *packed = NULL;
#endif // STORE_TGA
  //unsigned char *fbd = (unsigned char*)fb->buf;

#if STORE_RAW
  storeBufferToSD(pictureNumber, "rgb565", fb->buf, fb->len);
#endif

#if STORE_CHINA_EXPORT
  byte *ce = (byte*)ps_malloc(CE_DATA_SIZE);
  memcpy(ce, CE_DATA, CE_DATA_SIZE);
  ditherRGB565toChinaExport(fb->buf, ce);
  storeBufferToSD(pictureNumber, "ce.tga", ce, CE_DATA_SIZE);
#endif

  ditherRGB565toRGB(fb->buf, rgb, NULL, WIDTH, HEIGHT, DITHER_NONE);
#if STORE_RGB & STORE_DITHER_NONE
  storeBufferToSD(pictureNumber, "rgb", rgb, RGB_SIZE);
#endif

#if STORE_DITHER_AVERAGE
  ditherRGB565toRGB(fb->buf, rgb, packed, WIDTH, HEIGHT, DITHER_AVERAGE);
  storeBuffers(pictureNumber, "avg", rgb, tga);
#endif

#if STORE_DITHER_ERROR
  ditherRGB565toRGB(fb->buf, rgb, packed, WIDTH, HEIGHT, DITHER_ERROR_PROP);
  storeBuffers(pictureNumber, "err", rgb, tga);
#endif

#if STORE_DITHER_ORDERED
  ditherRGB565toRGB(fb->buf, rgb, packed, WIDTH, HEIGHT, DITHER_ORDERED);
  storeBuffers(pictureNumber, "org", rgb, tga);
#endif

#if STORE_DITHER_ORDERED_SHIFTED
  ditherRGB565toRGB(fb->buf, rgb, packed, WIDTH, HEIGHT, DITHER_ORDERED_SHIFTED);
  storeBuffers(pictureNumber, "xyz", rgb, tga);
#endif

  esp_camera_fb_return(fb);

  EEPROM.write(0, pictureNumber);
  EEPROM.commit();
}


void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
 
  Serial.begin(115200);
  //Serial.setDebugOutput(true);
  //Serial.println();
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 16000000;
  config.pixel_format = PIXFORMAT_RGB565; /* 0x61 0x65 = 01100|001011|00101 */
  
  if(psramFound()){
    // XGA hangs esp_camera_fb_get()
    config.frame_size = FRAMESIZE_QVGA; // FRAMESIZE_ + QVGA|CIF|VGA|SVGA|XGA|SXGA|UXGA
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    Serial.println("!!!!!!!!!!!!!!!!!!!! NO PSRAM !!!!!!!!!!!!!!!!!!!!");
  }
  
  // Init Camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  //Serial.println("Starting SD Card");
  if(!SD_MMC.begin()){
    printAndSleep("SD Card Mount Failed");
    return;
  }
  
  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE){
    printAndSleep("No SD Card attached");
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_contrast(s, 2);
  s->set_saturation(s, 2);

#ifndef USE_BUTTON
  takePictureToSD();
#endif

  // Turns off the ESP32-CAM white on-board LED (flash) connected to GPIO 4
  pinMode(4, OUTPUT);
  digitalWrite(4, LOW);
  rtc_gpio_hold_en(GPIO_NUM_4);
  
#ifdef USE_BUTTON
  pinMode(0, INPUT_PULLUP);
  Serial.println("Press shutter for fun...");
#else
  SD_MMC.end();
  printAndSleep(NULL);
#endif
}


void loop() {
#ifdef USE_BUTTON
  int btn_pressed = digitalRead(0) == 0;
  if (btn_pressed) {
    Serial.println("CHEEESE!!!");
    pinMode(0, OUTPUT);
    delay(200);
    takePictureToSD();
    pinMode(0, INPUT_PULLUP);
  }
#endif
}
