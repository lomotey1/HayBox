#include "comms/B0XXInputViewer.hpp"
#include "comms/DInputBackend.hpp"
#include "comms/GamecubeBackend.hpp"
#include "comms/N64Backend.hpp"
#include "comms/NintendoSwitchBackend.hpp"
#include "comms/XInputBackend.hpp"
#include "config/mode_selection.hpp"
#include "core/CommunicationBackend.hpp"
#include "core/InputMode.hpp"
#include "core/KeyboardMode.hpp"
#include "core/pinout.hpp"
#include "core/socd.hpp"
#include "core/state.hpp"
#include "input/GpioButtonInput.hpp"
#include "input/NunchukInput.hpp"
#include "joybus_utils.hpp"
#include "modes/Melee20Button.hpp"
#include "stdlib.hpp"

// ssd1306

#include ".pio/libdeps/pico/pico-ssd1306/example/BMSPA_font.h"
#include ".pio/libdeps/pico/pico-ssd1306/example/acme_5_outlines_font.h"
#include ".pio/libdeps/pico/pico-ssd1306/example/bubblesstandard_font.h"
#include ".pio/libdeps/pico/pico-ssd1306/example/crackers_font.h"
#include ".pio/libdeps/pico/pico-ssd1306/example/image.h"
#include ".pio/libdeps/pico/pico-ssd1306/ssd1306.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include <pico/bootrom.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

CommunicationBackend **backends = nullptr;
size_t backend_count;
KeyboardMode *current_kb_mode = nullptr;

GpioButtonMapping button_mappings[] = {
    {&InputState::l,            5 },
    { &InputState::left,        4 },
    { &InputState::down,        3 },
    { &InputState::right,       2 },

    { &InputState::mod_x,       6 },
    { &InputState::mod_y,       7 },

    { &InputState::select,      10},
    { &InputState::start,       0 },
    { &InputState::home,        11},

    { &InputState::c_left,      13},
    { &InputState::c_up,        12},
    { &InputState::c_down,      15},
    { &InputState::a,           14},
    { &InputState::c_right,     16},

    { &InputState::b,           26},
    { &InputState::x,           21},
    { &InputState::z,           19},
    { &InputState::up,          17},

    { &InputState::r,           27},
    { &InputState::y,           22},
    { &InputState::lightshield, 20},
    { &InputState::midshield,   18},
};
size_t button_count = sizeof(button_mappings) / sizeof(GpioButtonMapping);

const Pinout pinout = {
    .joybus_data = 28,
    .mux = -1,
    .nunchuk_detect = -1,
    .nunchuk_sda = -1,
    .nunchuk_scl = -1,
};

void setup() {
    // Create GPIO input source and use it to read button states for checking button holds.
    GpioButtonInput *gpio_input = new GpioButtonInput(button_mappings, button_count);

    InputState button_holds;
    gpio_input->UpdateInputs(button_holds);

    // Bootsel button hold as early as possible for safety.
    if (button_holds.start) {
        reset_usb_boot(0, 0);
    }

    // Turn on LED to indicate firmware booted.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    // Create array of input sources to be used.
    static InputSource *input_sources[] = { gpio_input };
    size_t input_source_count = sizeof(input_sources) / sizeof(InputSource *);

    ConnectedConsole console = detect_console(pinout.joybus_data);

    /* Select communication backend. */
    CommunicationBackend *primary_backend;
    if (console == ConnectedConsole::NONE) {
        if (button_holds.x) {
            // If no console detected and X is held on plugin then use Switch USB backend.
            NintendoSwitchBackend::RegisterDescriptor();
            backend_count = 1;
            primary_backend = new NintendoSwitchBackend(input_sources, input_source_count);
            backends = new CommunicationBackend *[backend_count] { primary_backend };

            // Default to Ultimate mode on Switch.
            primary_backend->SetGameMode(new Ultimate(socd::SOCD_2IP));
            return;
        } else if (button_holds.z) {
            // If no console detected and Z is held on plugin then use DInput backend.
            TUGamepad::registerDescriptor();
            TUKeyboard::registerDescriptor();
            backend_count = 2;
            primary_backend = new DInputBackend(input_sources, input_source_count);
            backends = new CommunicationBackend *[backend_count] {
                primary_backend, new B0XXInputViewer(input_sources, input_source_count)
            };
        } else {
            // Default to XInput mode if no console detected and no other mode forced.
            backend_count = 2;
            primary_backend = new XInputBackend(input_sources, input_source_count);
            backends = new CommunicationBackend *[backend_count] {
                primary_backend, new B0XXInputViewer(input_sources, input_source_count)
            };
        }
    } else {
        if (console == ConnectedConsole::GAMECUBE) {
            primary_backend =
                new GamecubeBackend(input_sources, input_source_count, pinout.joybus_data);
        } else if (console == ConnectedConsole::N64) {
            primary_backend = new N64Backend(input_sources, input_source_count, pinout.joybus_data);
        }

        // If console then only using 1 backend (no input viewer).
        backend_count = 1;
        backends = new CommunicationBackend *[backend_count] { primary_backend };
    }

    // Default to Melee mode.
    primary_backend->SetGameMode(
        new Melee20Button(socd::SOCD_2IP_NO_REAC, { .crouch_walk_os = false })
    );
}

void loop() {
    select_mode(backends[0]);

    for (size_t i = 0; i < backend_count; i++) {
        backends[i]->SendReport();
    }

    if (current_kb_mode != nullptr) {
        current_kb_mode->SendReport(backends[0]->GetInputs());
    }
}

/* Nunchuk code runs on the second core */
// NunchukInput *nunchuk = nullptr;

// ssd1306 on second core
const uint8_t num_chars_per_disp[] = { 7, 7, 7, 5 };
const uint8_t *fonts[4] = { acme_font, bubblesstandard_font, crackers_font, BMSPA_font };

#define SLEEPTIME 25

void setup1() {
    // while (backends == nullptr) {
    //     tight_loop_contents();
    // }

    // Create Nunchuk input source.
    // nunchuk = new NunchukInput(Wire, pinout.nunchuk_detect, pinout.nunchuk_sda,
    // pinout.nunchuk_scl);

    // Setup OLED
    i2c_init(i2c0, 400000);
    gpio_set_function(8, GPIO_FUNC_I2C);
    gpio_set_function(9, GPIO_FUNC_I2C);
    gpio_pull_up(8);
    gpio_pull_up(9);
}

void loop1() {
    // if (backends != nullptr) {
    //     nunchuk->UpdateInputs(backends[0]->GetInputs());
    //     busy_wait_us(50);
    // }

    // animation method
    const char *words[] = { "SSD1306", "DISPLAY", "DRIVER" };

    ssd1306_t disp;
    disp.external_vcc = false;
    ssd1306_init(&disp, 128, 64, 0x3C, i2c0);
    ssd1306_clear(&disp);

    printf("ANIMATION!\n");

    char buf[8];

    for (;;) {
        for (int y = 0; y < 31; ++y) {
            ssd1306_draw_line(&disp, 0, y, 127, y);
            ssd1306_show(&disp);
            sleep_ms(SLEEPTIME);
            ssd1306_clear(&disp);
        }

        for (int y = 0, i = 1; y >= 0; y += i) {
            ssd1306_draw_line(&disp, 0, 31 - y, 127, 31 + y);
            ssd1306_draw_line(&disp, 0, 31 + y, 127, 31 - y);
            ssd1306_show(&disp);
            sleep_ms(SLEEPTIME);
            ssd1306_clear(&disp);
            if (y == 32)
                i = -1;
        }

        for (int i = 0; i < sizeof(words) / sizeof(char *); ++i) {
            ssd1306_draw_string(&disp, 8, 24, 2, words[i]);
            ssd1306_show(&disp);
            sleep_ms(800);
            ssd1306_clear(&disp);
        }

        for (int y = 31; y < 63; ++y) {
            ssd1306_draw_line(&disp, 0, y, 127, y);
            ssd1306_show(&disp);
            sleep_ms(SLEEPTIME);
            ssd1306_clear(&disp);
        }

        for (size_t font_i = 0; font_i < sizeof(fonts) / sizeof(fonts[0]); ++font_i) {
            uint8_t c = 32;
            while (c <= 126) {
                uint8_t i = 0;
                for (; i < num_chars_per_disp[font_i]; ++i) {
                    if (c > 126)
                        break;
                    buf[i] = c++;
                }
                buf[i] = 0;

                ssd1306_draw_string_with_font(&disp, 8, 24, 2, fonts[font_i], buf);
                ssd1306_show(&disp);
                sleep_ms(800);
                ssd1306_clear(&disp);
            }
        }

        // ssd1306_bmp_show_image(&disp, image_data, image_size);
        // ssd1306_show(&disp);
        // sleep_ms(2000);
    }
}