#include "who_s3_cam.hpp"
#include "who_lcd.hpp"
#include "who_detect_app.hpp"

using namespace who::cam;
using namespace who::lcd;
using namespace who::app;

extern "C" void app_main(void)
{
    // Initialize status LED (optional)
    ESP_ERROR_CHECK(bsp_leds_init());
    ESP_ERROR_CHECK(bsp_led_set(BSP_LED_GREEN, false));

    // Initialize camera (RGB565 output expected by LCD)
    auto cam = new WhoS3Cam(PIXFORMAT_RGB565, FRAMESIZE_240X240, 2, true);

    // Initialize LCD
    auto lcd = new WhoLCD();

    // Initialize display app (no model, just raw camera stream)
    auto detect = new WhoDetectAppLCD({{128, 128, 128}}); // gray bounding box color (unused if model is null)
    detect->set_cam(cam);
    detect->set_model(nullptr);  // No face detection model
    detect->set_lcd(lcd);

    detect->run();
}
